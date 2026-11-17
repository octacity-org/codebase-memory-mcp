//! index_tools.zig — the index_repository MCP tool: the in-process pipeline
//! run and its response, the supervised worker with skip-and-continue crash
//! recovery, the per-server index job queue and the index hold.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const mcp = @import("mcp.zig");
const project_tools = @import("project_tools.zig");

const CStr = mcp.CStr;
const HeapStr = mcp.HeapStr;
const Str = mcp.Str;
const Server = mcp.Server;
const MutDoc = mcp.MutDoc;
const MutVal = mcp.MutVal;
const textResult = mcp.textResult;
const orEmpty = mcp.orEmpty;
const sp = mcp.sp;

/// Cap on excluded dir paths listed in the response — keep it compact on
/// large repos (node_modules / vendor / etc. can produce many skip points).
/// The full count is still reported via "count" + "truncated".
const INDEX_EXCLUDED_DIR_CAP = 25;

/// Cap on per-file skips embedded in the JSON response. The FULL, uncapped
/// list always goes to the per-run logfile; the JSON carries "count" +
/// "truncated" so nothing is silently hidden.
const INDEX_SKIPPED_FILE_CAP = 50;

fn count(n: c_int) usize {
    return @intCast(@max(n, 0));
}

/// Attach a compact summary of directory subtrees skipped during discovery
/// (#411). Shape: "excluded": {"dirs": [up to 25 rel-paths], "count": <total>,
/// "truncated": <bool>}. No-op when nothing was excluded.
fn addExcludedSummary(doc: ?*MutDoc, root: ?*MutVal, excluded_dirs: [*c][*c]u8, excluded_count: c_int) void {
    if (excluded_dirs == null or excluded_count <= 0) return;
    const excluded = c.yyjson_mut_obj(doc);
    const dirs = c.yyjson_mut_arr(doc);
    for (excluded_dirs[0..@min(count(excluded_count), INDEX_EXCLUDED_DIR_CAP)]) |dir| {
        if (dir != null) _ = c.yyjson_mut_arr_add_strcpy(doc, dirs, dir);
    }
    _ = c.yyjson_mut_obj_add_val(doc, excluded, "dirs", dirs);
    _ = c.yyjson_mut_obj_add_int(doc, excluded, "count", excluded_count);
    _ = c.yyjson_mut_obj_add_bool(doc, excluded, "truncated", excluded_count > INDEX_EXCLUDED_DIR_CAP);
    _ = c.yyjson_mut_obj_add_val(doc, root, "excluded", excluded);
}

/// Attach a summary of per-file skips (Stage 2 / Track B). Always emits a
/// top-level "skipped_count" (0 on clean runs). When there are skips, also
///   "skipped": {"files":[{path,reason,phase}..(<=50)], "count":N, "truncated":bool}
/// and, if a per-run logfile was written, "logfile": "<path>". Files the
/// parser only partly understood (phase "parse_partial", or "parse_unusable")
/// were still indexed, so they are reported apart as "parse_partial" with
/// their error line ranges and do not count as skipped. The run status stays
/// "indexed" — a skipped file is the expected handled outcome.
fn addSkippedSummary(doc: ?*MutDoc, root: ?*MutVal, errs_ptr: [*c]const c.cbm_file_error_t, n: c_int, logfile: CStr) void {
    const errs: []const c.cbm_file_error_t = if (errs_ptr != null) errs_ptr[0..count(n)] else &.{};
    var skipped_count: c_int = 0;
    var partial_count: c_int = 0;
    for (errs) |e| {
        if (c.cbm_pipeline_phase_is_partial(e.phase)) partial_count += 1 else skipped_count += 1;
    }
    _ = c.yyjson_mut_obj_add_int(doc, root, "skipped_count", skipped_count);
    if (errs.len == 0) return;
    for ([_]bool{ false, true }) |partial| {
        const total = if (partial) partial_count else skipped_count;
        if (total == 0) continue;
        const group = c.yyjson_mut_obj(doc);
        const files = c.yyjson_mut_arr(doc);
        var shown: usize = 0;
        for (errs) |e| {
            if (shown >= INDEX_SKIPPED_FILE_CAP) break;
            if (c.cbm_pipeline_phase_is_partial(e.phase) != partial) continue;
            const fe = c.yyjson_mut_obj(doc);
            _ = c.yyjson_mut_obj_add_strcpy(doc, fe, "path", orEmpty(e.path));
            if (partial) {
                _ = c.yyjson_mut_obj_add_strcpy(doc, fe, "error_ranges", orEmpty(e.reason));
                if (mcp.cEql(e.phase, "parse_unusable")) _ = c.yyjson_mut_obj_add_bool(doc, fe, "parse_unusable", true);
            } else {
                _ = c.yyjson_mut_obj_add_strcpy(doc, fe, "reason", orEmpty(e.reason));
                _ = c.yyjson_mut_obj_add_strcpy(doc, fe, "phase", orEmpty(e.phase));
            }
            _ = c.yyjson_mut_arr_add_val(files, fe);
            shown += 1;
        }
        _ = c.yyjson_mut_obj_add_val(doc, group, "files", files);
        _ = c.yyjson_mut_obj_add_int(doc, group, "count", total);
        _ = c.yyjson_mut_obj_add_bool(doc, group, "truncated", total > INDEX_SKIPPED_FILE_CAP);
        _ = c.yyjson_mut_obj_add_val(doc, root, if (partial) "parse_partial" else "skipped", group);
    }
    if (logfile != null and logfile[0] != 0) _ = c.yyjson_mut_obj_add_strcpy(doc, root, "logfile", logfile);
}

fn fputsAll(f: ?*c.FILE, parts: []const CStr) void {
    for (parts) |s| _ = c.fputs(s, f);
}

/// Write the FULL (uncapped) skip list to a per-run logfile — ONLY when >=1
/// file was skipped (no logfile on a clean run). Location:
///   $CBM_INDEX_LOG (override) else <cache_dir>/logs/<project>-<epoch>.log
/// Returns true and fills out_path on success.
fn writeSkipLogfile(project: CStr, errs_ptr: [*c]const c.cbm_file_error_t, n: c_int, out_path: []u8) bool {
    if (errs_ptr == null or n <= 0) return false;
    var path: [1024]u8 = undefined;
    const override = c.getenv("CBM_INDEX_LOG");
    if (override != null and override[0] != 0) {
        _ = mcp.bprint(&path, "{s}", .{sp(override)});
    } else {
        const cdir = c.cbm_resolve_cache_dir();
        if (cdir == null) return false;
        var logdir: [1024]u8 = undefined;
        _ = c.cbm_mkdir_p(mcp.fmtZ(&logdir, "{s}/logs", .{sp(cdir)}), 0o755);
        _ = mcp.bprint(&path, "{s}/{s}-{d}.log", .{ mcp.bufStr(&logdir), if (project != null) sp(project) else "index", @as(i64, c.time(null)) });
    }
    const f = c.cbm_fopen(&path, "wb");
    if (f == null) {
        interop.logWarn("index.logfile_open_fail", .{ "path", @as(CStr, &path) });
        return false;
    }
    var count_buf: [16]u8 = undefined;
    _ = c.fputs("# codebase-memory-mcp index skip report\n", f);
    fputsAll(f, &.{ "# project=", orEmpty(project), " skipped=", mcp.fmtZ(&count_buf, "{d}", .{n}), "\n" });
    _ = c.fputs("# columns: phase\treason\tpath\n", f);
    for (errs_ptr[0..count(n)]) |e| {
        fputsAll(f, &.{ orEmpty(e.phase), "\t", orEmpty(e.reason), "\t", orEmpty(e.path), "\n" });
    }
    _ = c.fclose(f);
    _ = mcp.bprint(out_path, "{s}", .{mcp.bufStr(&path)});
    return true;
}

/// Build the success portion of the index_repository response. Returns true
/// when status should be "degraded" (#334 plausibility gate).
fn buildIndexSuccessResponse(doc: ?*MutDoc, root: ?*MutVal, project_name: CStr, p: ?*c.cbm_pipeline_t, excluded_dirs: [*c][*c]u8, excluded_count: c_int, file_errors: [*c]const c.cbm_file_error_t, file_error_count: c_int, logfile: CStr) bool {
    addExcludedSummary(doc, root, excluded_dirs, excluded_count);
    addSkippedSummary(doc, root, file_errors, file_error_count, logfile);

    // Discovery could not fully materialize the tree (bounded walk stack full
    // / allocation failure): the index covers only the files actually found,
    // so the run must not report a complete-looking "indexed" status (#17,
    // upstream 03dc9c91).
    var degraded = c.cbm_pipeline_discover_degraded(p);

    var exp_nodes: c_int = -1;
    var exp_edges: c_int = -1;
    c.cbm_pipeline_get_committed_counts(p, &exp_nodes, &exp_edges);

    var nodes: c_int = 0;
    var edges: c_int = 0;

    var zova_path: [1024]u8 = undefined;
    const repository = if (c.cbm_zova_user_database_path(&zova_path, zova_path.len) != 0)
        null
    else
        c.cbm_zova_repository_open(&zova_path, project_name);
    if (repository) |repo| {
        const workspace_id = c.cbm_zova_repository_workspace_id(repo);
        if (c.cbm_zova_repository_counts(repo, workspace_id, &nodes, &edges) != c.CBM_STORE_OK or nodes < 0 or edges < 0) {
            nodes = 0;
            edges = 0;
            degraded = true;
        }
        // The published generation's coverage, not just this run's: an
        // incremental run re-parses only changed files, so its own skip list
        // would make older gaps look healed.
        project_tools.addCoverageSummary(doc, root, repo, workspace_id);
        c.cbm_zova_repository_close(repo);
    } else {
        degraded = true;
    }
    if (!degraded) {
        degraded = c.cbm_dump_verify_is_degraded(exp_nodes, nodes, c.cbm_dump_verify_min_ratio(), c.CBM_DUMP_VERIFY_MIN_FLOOR);
    }

    _ = c.yyjson_mut_obj_add_int(doc, root, "nodes", nodes);
    _ = c.yyjson_mut_obj_add_int(doc, root, "edges", edges);
    if (exp_nodes >= 0) {
        _ = c.yyjson_mut_obj_add_int(doc, root, "expected_nodes", exp_nodes);
        _ = c.yyjson_mut_obj_add_int(doc, root, "expected_edges", exp_edges);
    }

    if (degraded) {
        if (c.cbm_pipeline_discover_degraded(p)) {
            _ = c.yyjson_mut_obj_add_str(doc, root, "hint", "File discovery was incomplete (bounded walk budget or allocation " ++
                "failure) — the index covers only the files that were found. " ++
                "Re-run index_repository(repo_path=...) to rebuild.");
            interop.logWarn("dump.verify", .{ "reason", "discover_degraded" });
        } else {
            _ = c.yyjson_mut_obj_add_str(doc, root, "hint", "Committed shared Zova workspace failed verification. " ++
                "Re-run index_repository(repo_path=...) to rebuild.");
            interop.logWarn("dump.verify", .{ "reason", "zova_workspace_verification_failed", "expected_nodes", @as(Str, if (exp_nodes >= 0) "set" else "unknown") });
        }
    }
    return degraded;
}

/// Response for a worker that crashed/hung/failed without producing a
/// result. The crash is already contained (this process survived); report it
/// rather than dying.
fn buildWorkerFailureResponse(args: CStr, outcome: c.cbm_proc_outcome_t) HeapStr {
    const repo_path = mcp.strArg(args, "repo_path");
    defer c.free(repo_path);
    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_str(doc, root, "status", "error");
    _ = c.yyjson_mut_obj_add_str(doc, root, "outcome", c.cbm_proc_outcome_str(outcome));
    _ = c.yyjson_mut_obj_add_str(doc, root, "hint", if (outcome == c.CBM_PROC_HANG)
        "Indexing worker timed out (a file made no progress). The worker was " ++
            "terminated and the server survived. Re-run to retry."
    else
        "Indexing worker crashed on a file. The crash was contained (the server " ++
            "survived). Re-run to retry; a future release isolates the culprit file.");
    if (repo_path != null) _ = c.yyjson_mut_obj_add_strcpy(doc, root, "repo_path", repo_path);
    const json = mcp.docToStr(doc);
    c.yyjson_mut_doc_free(doc);
    return mcp.textResultOwned(json, true);
}

/// Drop the cached store so the next query reopens whatever the worker
/// published to cbm.zova. NULL-safe: the background watcher path (main.zig) has
/// no MCP server, so there is nothing to invalidate.
fn supervisorInvalidateStore(srv: ?*Server) void {
    const s = srv orelse return;
    if (s.owns_store and s.store != null) {
        c.cbm_store_close(s.store);
        s.store = null;
    }
    c.free(s.current_project);
    s.current_project = null;
}

fn getpid() c_int {
    if (mcp.is_windows) return @intCast(std.os.windows.GetCurrentProcessId());
    return @intCast(c.getpid());
}

/// A per-supervisor-run temp path <cache_dir>/logs/.supervisor-<pid><suffix>
/// (falls back to the CWD if the cache dir is unresolvable). Used for the
/// crash-attribution marker and the quarantine list during recovery.
fn supervisorTmpPath(out: []u8, suffix: []const u8) void {
    const cdir = c.cbm_resolve_cache_dir();
    if (cdir != null and cdir[0] != 0) {
        var logdir: [1024]u8 = undefined;
        _ = c.cbm_mkdir_p(mcp.fmtZ(&logdir, "{s}/logs", .{sp(cdir)}), 0o755);
        _ = mcp.bprint(out, "{s}/.supervisor-{d}{s}", .{ mcp.bufStr(&logdir), getpid(), suffix });
    } else {
        _ = mcp.bprint(out, ".supervisor-{d}{s}", .{ getpid(), suffix });
    }
}

/// The crash/hang SUSPECT set: files whose last event in the worker's marker
/// journal ("S <rel>" / "D <rel>" lines, see cbm_index_mark_start/done) is an
/// S with no closing D, i.e. the in-flight set at kill time, OLDEST OPEN S
/// FIRST (for a hang the oldest still-open file IS the stuck one). A torn
/// final line (no trailing newline) is discarded by design.
const Suspects = struct {
    paths: std.ArrayList([]u8) = .empty,

    fn deinit(self: *Suspects) void {
        for (self.paths.items) |p| std.heap.c_allocator.free(p);
        self.paths.deinit(std.heap.c_allocator);
        self.* = .{};
    }

    fn indexOf(self: *const Suspects, rel: []const u8) ?usize {
        for (self.paths.items, 0..) |p, i| {
            if (std.mem.eql(u8, p, rel)) return i;
        }
        return null;
    }

    fn read(path: CStr) Suspects {
        var out: Suspects = .{};
        const f = c.cbm_fopen(path, "rb");
        if (f == null) return out;
        defer _ = c.fclose(f);
        const gpa = std.heap.c_allocator;
        var line_buf: [1024]u8 = undefined;
        while (c.fgets(&line_buf, line_buf.len, f) != null) {
            var line = mcp.bufStr(&line_buf);
            if (line.len == 0 or line[line.len - 1] != '\n') break; // torn final line — discard and stop
            line = line[0 .. line.len - 1];
            if (line.len > 0 and line[line.len - 1] == '\r') line = line[0 .. line.len - 1];
            if (line.len < 3 or (line[0] != 'S' and line[0] != 'D') or line[1] != ' ') continue;
            const rel = line[2..];
            if (line[0] == 'S') {
                if (out.indexOf(rel) != null) continue;
                const copy = gpa.dupe(u8, rel) catch break;
                out.paths.append(gpa, copy) catch {
                    gpa.free(copy);
                    break;
                };
            } else if (out.indexOf(rel)) |i| {
                gpa.free(out.paths.orderedRemove(i));
            }
        }
        return out;
    }
};

/// Append one quarantine entry "rel\tphase\n" (phase = "crash"|"hang"). The
/// worker's loader parses this back and reports the skip's phase in
/// skipped[]; a bare "rel" line is still tolerated there (defaults crash).
fn supervisorAppendQuarantine(path: CStr, rel: []const u8, phase: Str) bool {
    const f = c.cbm_fopen(path, "ab");
    if (f == null) return false;
    _ = c.fwrite(rel.ptr, 1, rel.len, f);
    fputsAll(f, &.{ "\t", phase, "\n" });
    _ = c.fclose(f);
    return true;
}

/// How many times a failed index worker may be re-run, as
/// CBM_INDEX_MAX_RESTARTS sets it. Default 100; 0 means no restarts. An
/// unreadable value keeps the default and is logged (upstream 6b801eec).
fn indexRestartCap() c_int {
    const default_cap = 100;
    var v: c_long = 0;
    if (!c.cbm_env_long("CBM_INDEX_MAX_RESTARTS", &v)) {
        var raw = std.mem.zeroes([64]u8);
        if (c.cbm_safe_getenv("CBM_INDEX_MAX_RESTARTS", &raw, raw.len, null) != null and raw[0] != 0) {
            interop.logWarn("index.restart_cap.ignored", .{ "value", @as(CStr, &raw), "action", "using_default" });
        }
        return default_cap;
    }
    if (v < 0 or v > std.math.maxInt(c_int)) {
        interop.logWarn("index.restart_cap.out_of_range", .{ "action", "using_default" });
        return default_cap;
    }
    return @intCast(v);
}

pub export fn cbm_index_restart_cap_for_testing() c_int {
    return indexRestartCap();
}

/// Run index_repository in a supervised worker subprocess with
/// skip-and-continue (Stage 3c). Returns the response string (caller frees):
///   - the worker's own response on a clean first run (the common path);
///   - after a crash/hang, the response from a clean RECOVERY run that
///     quarantines the culprit file(s) — status="indexed" with them listed in
///     skipped[] as phase="crash"/"hang", and the good files indexed;
///   - a best-effort PARTIAL index (one final quarantine-only run) if the
///     recovery loop cannot converge but at least one file was quarantined;
///   - a contained-failure response only if even that cannot produce a clean run.
/// Returns NULL only when the worker could not be spawned at all, so the
/// caller degrades to the in-process path.
fn indexRunSupervisedImpl(srv: ?*Server, args: CStr) HeapStr {
    supervisorInvalidateStore(srv);
    defer supervisorInvalidateStore(srv);

    // First attempt: normal parallel run.
    var wr: c.cbm_index_worker_result_t = undefined;
    const rc = c.cbm_index_spawn_worker(args, false, null, null, &wr);
    if (rc != 0 or wr.outcome == c.CBM_PROC_SPAWN_FAILED) {
        c.cbm_index_worker_result_free(&wr);
        return null; // degrade to in-process
    }
    // A graceful nonzero exit with a response said why it failed (e.g. over
    // the memory budget): report it rather than re-running. A clean exit
    // transfers the worker's response (the common path); a clean exit with
    // no response degrades to the in-process path.
    if ((wr.outcome == c.CBM_PROC_EXIT_NONZERO and wr.response != null) or wr.outcome == c.CBM_PROC_CLEAN) {
        const resp = wr.response;
        wr.response = null;
        c.cbm_index_worker_result_free(&wr);
        return resp;
    }

    // Crash / hang / nonzero exit → skip-and-continue recovery. Re-run the
    // worker PARALLEL with the per-file marker JOURNAL armed; after each
    // failed run the journal's open-S set is the in-flight SUSPECT set. A file
    // is quarantined only when it appears in the suspect sets of TWO
    // CONSECUTIVE failed runs, and only ONE file per round: the OLDEST open S
    // in the intersection. A clean run then indexes the good files and
    // reports the quarantined ones as phase="crash"/"hang" skips.
    var last_outcome = wr.outcome;
    c.cbm_index_worker_result_free(&wr);

    var marker_path: [1024]u8 = undefined;
    var quarantine_path: [1024]u8 = undefined;
    supervisorTmpPath(&marker_path, ".marker");
    supervisorTmpPath(&quarantine_path, ".quarantine");
    _ = c.remove(&marker_path);
    // Start the quarantine list empty (truncate any stale file).
    if (c.cbm_fopen(&quarantine_path, "wb")) |qinit| _ = c.fclose(qinit);
    defer _ = c.remove(&quarantine_path);

    const cap = indexRestartCap();

    var resp: HeapStr = null;
    var quarantined: c_int = 0; // files pinned + added to the quarantine list so far
    var prev_suspects: ?Suspects = null; // previous failed round's in-flight set
    defer if (prev_suspects) |*p| p.deinit();
    var i: c_int = 0;
    recovery: while (i < cap) : (i += 1) {
        var wr2: c.cbm_index_worker_result_t = undefined;
        const rc2 = c.cbm_index_spawn_worker(args, false, &marker_path, &quarantine_path, &wr2);
        if (rc2 != 0) {
            last_outcome = wr2.outcome;
            c.cbm_index_worker_result_free(&wr2);
            break; // spawn failed mid-recovery — give up
        }
        if ((wr2.outcome == c.CBM_PROC_CLEAN or wr2.outcome == c.CBM_PROC_EXIT_NONZERO) and wr2.response != null) {
            resp = wr2.response;
            wr2.response = null;
            c.cbm_index_worker_result_free(&wr2);
            break; // good files indexed; quarantined files reported as crash/hang
        }
        last_outcome = wr2.outcome;
        c.cbm_index_worker_result_free(&wr2);
        if (last_outcome != c.CBM_PROC_CRASH and last_outcome != c.CBM_PROC_HANG) {
            // SPAWN_FAILED / nonzero exit / non-fault kill → not a crash we
            // can attribute; stop and report a contained failure.
            break;
        }
        // A fault signal → "crash"; a no-progress kill → "hang".
        const phase: Str = if (last_outcome == c.CBM_PROC_HANG) "hang" else "crash";
        var suspects = Suspects.read(&marker_path);
        _ = c.remove(&marker_path); // fresh journal for the next re-run
        if (suspects.paths.items.len == 0) {
            suspects.deinit();
            interop.logWarn("index.supervisor.unattributable", .{ "action", "give_up" });
            break;
        }
        if (prev_suspects) |*prev| {
            // Two-consecutive-strikes: quarantine the OLDEST open S that was
            // also in flight in the previous failed round.
            const pick = for (suspects.paths.items) |s| {
                if (prev.indexOf(s) != null) break s;
            } else {
                // Disjoint consecutive in-flight sets: the failure is not
                // attributable to a recurring file (systemic) — stop rather
                // than quarantine an innocent.
                suspects.deinit();
                interop.logWarn("index.supervisor.unattributable", .{ "action", "give_up" });
                break :recovery;
            };
            var pick_buf: [1024]u8 = undefined;
            const pick_z = mcp.fmtZ(&pick_buf, "{s}", .{pick});
            if (!supervisorAppendQuarantine(&quarantine_path, pick, phase)) {
                interop.logWarn("index.supervisor.quarantine_write_fail", .{ "path", @as(CStr, pick_z) });
                suspects.deinit();
                break;
            }
            quarantined += 1;
            var attempt_buf: [16]u8 = undefined;
            interop.logWarn("index.file_quarantined", .{ "path", @as(CStr, pick_z), "outcome", phase, "attempt", @as(CStr, mcp.fmtZ(&attempt_buf, "{d}", .{i + 1})) });
            prev.deinit();
        }
        prev_suspects = suspects;
    }

    _ = c.remove(&marker_path); // marker no longer needed

    // Terminal best-effort-partial: the loop exited WITHOUT a clean run (cap
    // exhausted, or an unattributable failure) but at least one file was
    // already quarantined. Try ONE final PARALLEL spawn with the accumulated
    // quarantine and NO marker — every known-bad file short-circuits, so a
    // clean run yields a PARTIAL index rather than a hard failure. Bounded by
    // the same quiet-timeout, so it cannot itself hang.
    if (resp == null and quarantined > 0) {
        var wrp: c.cbm_index_worker_result_t = undefined;
        const rcp = c.cbm_index_spawn_worker(args, false, null, &quarantine_path, &wrp);
        if (rcp == 0 and wrp.outcome == c.CBM_PROC_CLEAN and wrp.response != null) {
            resp = wrp.response;
            wrp.response = null;
            var qn: [16]u8 = undefined;
            interop.log(c.CBM_LOG_ERROR, "index.supervisor.partial", .{ "quarantined", @as(CStr, mcp.fmtZ(&qn, "{d}", .{quarantined})), "outcome", c.cbm_proc_outcome_str(last_outcome) });
        }
        c.cbm_index_worker_result_free(&wrp);
    }

    if (resp != null) return resp;
    return buildWorkerFailureResponse(args, last_outcome);
}

// ── Index job queue ──────────────────────────────────────────────

pub export fn cbm_index_job_queue_enter(srv_c: ?*c.cbm_mcp_server_t) void {
    const srv = mcp.srvOf(srv_c) orelse return;
    c.cbm_mutex_lock(&srv.index_queue_mutex);
    const ticket = srv.index_queue_next_ticket;
    srv.index_queue_next_ticket += 1;
    _ = srv.index_queue_waiters.fetchAdd(1, .monotonic);
    while (ticket != srv.index_queue_serving_ticket) {
        c.cbm_mutex_unlock(&srv.index_queue_mutex);
        _ = c.cbm_usleep(@as(c_uint, 10000)); // 10 ms
        c.cbm_mutex_lock(&srv.index_queue_mutex);
    }
    _ = srv.index_queue_waiters.fetchSub(1, .monotonic);
    c.cbm_mutex_unlock(&srv.index_queue_mutex);
}

pub export fn cbm_index_job_queue_leave(srv_c: ?*c.cbm_mcp_server_t) void {
    const srv = mcp.srvOf(srv_c) orelse return;
    c.cbm_mutex_lock(&srv.index_queue_mutex);
    srv.index_queue_serving_ticket += 1;
    c.cbm_mutex_unlock(&srv.index_queue_mutex);
}

fn indexRunSupervised(srv: ?*Server, args: CStr) HeapStr {
    const srv_c: ?*c.cbm_mcp_server_t = @ptrCast(srv);
    cbm_index_job_queue_enter(srv_c);
    defer cbm_index_job_queue_leave(srv_c);
    return indexRunSupervisedImpl(srv, args);
}

/// Run a minimal {"repo_path": "<root>"} through indexRunSupervised. Shared
/// by the session auto-index (srv present → its cached store is invalidated)
/// and the watcher re-index (srv NULL). Returns the worker's response string
/// (caller frees) or NULL to degrade.
pub fn indexRunSupervisedPath(srv: ?*Server, root_path: CStr) HeapStr {
    if (root_path == null or root_path[0] == 0) return null;
    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_strcpy(doc, root, "repo_path", root_path);
    const args = mcp.docToStr(doc);
    c.yyjson_mut_doc_free(doc);
    if (args == null) return null;
    defer c.free(args);
    return indexRunSupervised(srv, args);
}

/// The watcher re-index in main.zig has no MCP server, so it reaches the
/// supervised runner through this srv-less wrapper.
pub export fn cbm_mcp_index_run_supervised_path(root_path: [*c]const u8) [*c]u8 {
    return indexRunSupervisedPath(null, root_path);
}

// ── Index hold ───────────────────────────────────────────────────

/// Index hold for root_path (ADR-0001): the preparation barrier shared, then
/// the workspace lease. The root is canonicalized the way index_repository
/// canonicalizes repo_path, so the lease names the workspace the run publishes.
pub export fn cbm_mcp_index_hold_acquire(root_path: [*c]const u8, wait_ms: i64, out: [*c]c.cbm_zova_index_hold_t, out_workspace_id: [*c]u8, out_workspace_id_size: usize) c.cbm_zova_coord_status_t {
    var root: HeapStr = if (root_path != null) mcp.dup(root_path) else null;
    if (root == null) {
        if (out != null) out.* = std.mem.zeroes(c.cbm_zova_index_hold_t);
        return c.CBM_ZOVA_COORD_ERROR;
    }
    _ = c.cbm_normalize_path_sep(root);
    root = mcp.canonicalizeRepoPathIfExists(root);
    defer c.free(root);
    return c.cbm_zova_index_hold_acquire(root, wait_ms, out, out_workspace_id, out_workspace_id_size);
}

pub export fn cbm_mcp_index_hold_refusal_reason(status: c.cbm_zova_coord_status_t, hold: [*c]const c.cbm_zova_index_hold_t) [*c]const u8 {
    return switch (status) {
        c.CBM_ZOVA_COORD_OK => "ok",
        c.CBM_ZOVA_COORD_CANCELLED => "cancelled",
        c.CBM_ZOVA_COORD_BUSY => if (hold != null and hold.*.barrier_busy) "database_replacement_in_progress" else "workspace_busy",
        else => "coordination_unavailable",
    };
}

/// Machine-readable refusal, in the BUSY + reason shape of the Zova operations.
fn indexHoldRefusal(status: c.cbm_zova_coord_status_t, hold: *const c.cbm_zova_index_hold_t, workspace_id: CStr) HeapStr {
    const reason = cbm_mcp_index_hold_refusal_reason(status, hold);
    const hint: Str = if (status == c.CBM_ZOVA_COORD_ERROR)
        "Cannot use the coordination lock file next to cbm.zova; check the cache " ++
            "directory permissions."
    else if (status == c.CBM_ZOVA_COORD_CANCELLED)
        "The server is shutting down."
    else if (mcp.cEql(reason, "workspace_busy"))
        "Another process is indexing this workspace. Retry when it finishes; " ++
            "CBM_INDEX_LEASE_TIMEOUT_S sets how long a request waits."
    else
        "A restore, import or repack is replacing cbm.zova. Retry when it finishes.";
    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_str(doc, root, "status", if (status == c.CBM_ZOVA_COORD_ERROR) "error" else "busy");
    _ = c.yyjson_mut_obj_add_str(doc, root, "reason", reason);
    if (workspace_id != null and workspace_id[0] != 0) _ = c.yyjson_mut_obj_add_strcpy(doc, root, "workspace_id", workspace_id);
    _ = c.yyjson_mut_obj_add_str(doc, root, "hint", hint);
    const json = mcp.docToStr(doc);
    c.yyjson_mut_doc_free(doc);
    defer c.free(json);
    return textResult(if (json != null) json else "{\"status\":\"busy\"}", true);
}

// ── index_repository ─────────────────────────────────────────────

/// The process that received the request holds the index hold for the whole
/// run, including a supervised worker's life. The worker itself never takes
/// it: POSIX record locks are not inherited, so it would conflict with its
/// parent.
pub fn handleIndexRepository(srv: *Server, args: CStr) HeapStr {
    if (c.cbm_index_worker_active()) return indexRepositoryRun(srv, args);
    var repo_path = mcp.strArg(args, "repo_path");
    if (repo_path == null) repo_path = mcp.strArg(args, "path");
    if (repo_path == null) return indexRepositoryRun(srv, args); // reports the missing field
    var hold: c.cbm_zova_index_hold_t = undefined;
    var workspace_id = std.mem.zeroes([128]u8);
    const status = cbm_mcp_index_hold_acquire(repo_path, c.cbm_zova_coord_default_wait_ms(), &hold, &workspace_id, workspace_id.len);
    c.free(repo_path);
    if (status != c.CBM_ZOVA_COORD_OK) {
        interop.logWarn("index.hold.refused", .{ "reason", cbm_mcp_index_hold_refusal_reason(status, &hold), "workspace", @as(CStr, &workspace_id) });
        return indexHoldRefusal(status, &hold, &workspace_id);
    }
    defer c.cbm_zova_index_hold_release(&hold);
    return indexRepositoryRun(srv, args);
}

fn closeCachedStore(srv: *Server) void {
    if (srv.owns_store and srv.store != null) {
        c.cbm_store_close(srv.store);
        srv.store = null;
    }
    c.free(srv.current_project);
    srv.current_project = null;
}

fn indexRepositoryRun(srv: *Server, args: CStr) HeapStr {
    // Supervisor gate: run the index in a crash/hang-isolating worker
    // subprocess unless this process IS the worker or the kill switch
    // (CBM_INDEX_SUPERVISOR=0) is set. On spawn failure, fall through to the
    // in-process path (degrade).
    if (c.cbm_index_supervisor_should_wrap()) {
        const supervised = indexRunSupervised(srv, args);
        if (supervised != null) return supervised;
    }

    var repo_path = mcp.strArg(args, "repo_path");
    if (repo_path == null) repo_path = mcp.strArg(args, "path");
    const mode_str = mcp.strArg(args, "mode");
    const name_override = mcp.strArg(args, "name");
    defer c.free(name_override);

    if (repo_path == null) {
        c.free(mode_str);
        return textResult("required field: repo_path; alias accepted: path; example: " ++
            "{\"repo_path\":\"/absolute/path/to/repo\"}", true);
    }
    _ = c.cbm_normalize_path_sep(repo_path);
    repo_path = mcp.canonicalizeRepoPathIfExists(repo_path);
    defer c.free(repo_path);

    // Workspace boundary (upstream 7fa5b077, #17): cbm_mcp_check_index_root
    // enforces CBM_ALLOWED_ROOT. Unset by default, so the standard "index the
    // path I gave you" behaviour is unchanged.
    var boundary_reason: CStr = null;
    var canonbuf: [4096]u8 = undefined;
    if (mcp.cbm_mcp_check_index_root(repo_path, &canonbuf, canonbuf.len, &boundary_reason) != 0) {
        c.free(mode_str);
        return textResult(if (boundary_reason != null) boundary_reason else "repo_path is outside the allowed root", true);
    }

    var mode: c.cbm_index_mode_t = c.CBM_MODE_FULL;
    if (mode_str != null and mcp.cEql(mode_str, "fast")) {
        mode = c.CBM_MODE_FAST;
    } else if (mode_str != null and mcp.cEql(mode_str, "moderate")) {
        mode = c.CBM_MODE_MODERATE;
    }
    c.free(mode_str);

    const p = c.cbm_pipeline_new(repo_path, null, mode) orelse return textResult("failed to create pipeline", true);
    if (name_override != null and name_override[0] != 0 and !c.cbm_pipeline_set_project_name(p, name_override)) {
        c.cbm_pipeline_free(p);
        return textResult("invalid project name", true);
    }

    const max_files: c_int = if (srv.config != null) c.cbm_config_get_int(srv.config, c.CBM_CONFIG_INDEX_MAX_FILES, 0) else 0;
    const max_source_mb: c_int = if (srv.config != null) c.cbm_config_get_int(srv.config, c.CBM_CONFIG_INDEX_MAX_SOURCE_MB, 0) else 0;
    if (max_files > 0) c.cbm_pipeline_set_max_files(p, max_files);
    if (max_source_mb > 0) c.cbm_pipeline_set_max_source_mb(p, max_source_mb);

    const project_name = mcp.dup(c.cbm_pipeline_project_name(p));
    defer c.free(project_name);

    // Close cached store — the pipeline republishes the project to cbm.zova.
    closeCachedStore(srv);

    // Same-workspace exclusion is the workspace lease held by
    // handleIndexRepository. Track active pipeline so signal handler and
    // notifications/cancelled can cancel it mid-run.
    srv.active_pipeline = p;
    const rc = c.cbm_pipeline_run(p);
    srv.active_pipeline = null;

    // Capture the excluded-subtree list (#411) and the per-file skip list
    // while the pipeline (which owns the strings) is still alive — the
    // response builder copies them into the JSON doc.
    var excluded_dirs: [*c][*c]u8 = null;
    var excluded_count: c_int = 0;
    c.cbm_pipeline_get_excluded(p, &excluded_dirs, &excluded_count);
    var file_errors: [*c]c.cbm_file_error_t = null;
    var file_error_count: c_int = 0;
    c.cbm_pipeline_get_file_errors(p, &file_errors, &file_error_count);

    c.cbm_mem_collect(); // return mimalloc pages to OS after large indexing

    // Invalidate cached store so next query reopens the fresh database.
    closeCachedStore(srv);

    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_str(doc, root, "project", project_name);

    if (rc == 0) {
        // Write the per-run logfile ONLY when there were skips (no logfile on
        // a clean run). The FULL list goes to the file; the JSON caps at 50.
        var logfile_path = std.mem.zeroes([1024]u8);
        const has_logfile = writeSkipLogfile(project_name, file_errors, file_error_count, &logfile_path);
        const degraded = buildIndexSuccessResponse(doc, root, project_name, p, excluded_dirs, excluded_count, file_errors, file_error_count, if (has_logfile) &logfile_path else null);
        _ = c.yyjson_mut_obj_add_str(doc, root, "status", if (degraded) "degraded" else "indexed");
        if (c.cbm_pipeline_had_format_migration(p)) _ = c.yyjson_mut_obj_add_bool(doc, root, "format_migration", true);
    } else if (rc == c.CBM_PIPELINE_ABORT_OVER_BUDGET) {
        // Resident memory stayed above the budget after back-pressure and one
        // confirmation cycle, so the run stopped before publication. Name the
        // cause, the numbers and the fact that the previous index still
        // serves; the generic "check repo_path" hint below would mislead.
        const mib = 1024 * 1024;
        const budget_mb: c_int = @intCast(c.cbm_mem_budget() / mib);
        const peak_rss_mb: c_int = @intCast(c.cbm_mem_peak_rss() / mib);
        var budget_text: [32]u8 = undefined;
        var peak_text: [32]u8 = undefined;
        interop.log(c.CBM_LOG_ERROR, "index.abort", .{
            "reason",      "over_memory_budget",
            "project",     @as(CStr, project_name),
            "budget_mb",   @as(CStr, mcp.fmtZ(&budget_text, "{d}", .{budget_mb})),
            "peak_rss_mb", @as(CStr, mcp.fmtZ(&peak_text, "{d}", .{peak_rss_mb})),
        });
        _ = c.yyjson_mut_obj_add_str(doc, root, "status", "error");
        _ = c.yyjson_mut_obj_add_str(doc, root, "reason", "over_memory_budget");
        _ = c.yyjson_mut_obj_add_str(doc, root, "previous_index", "preserved");
        _ = c.yyjson_mut_obj_add_int(doc, root, "budget_mb", budget_mb);
        _ = c.yyjson_mut_obj_add_int(doc, root, "peak_rss_mb", peak_rss_mb);
        _ = c.yyjson_mut_obj_add_str(doc, root, "hint", if (c.cbm_mem_budget_explicit())
            "Indexing stopped: resident memory stayed above " ++
                "CBM_MEM_BUDGET_MB after backpressure; no partial graph was " ++
                "published and the previous index still serves. Raise " ++
                "CBM_MEM_BUDGET_MB or exclude large subtrees."
        else
            "Indexing stopped: resident memory stayed above the detected " ++
                "RAM budget after backpressure; no partial graph was " ++
                "published and the previous index still serves. Raise " ++
                "CBM_MEM_BUDGET_MB, lower CBM_WORKERS, or exclude large " ++
                "subtrees.");
    } else if (rc == c.CBM_PIPELINE_ABORT_MAX_FILES) {
        const discovered_files = c.cbm_pipeline_get_discovered_files(p);
        var files_text: [32]u8 = undefined;
        var limit_text: [32]u8 = undefined;
        var hint_buf: [512]u8 = undefined;
        interop.log(c.CBM_LOG_ERROR, "index.abort", .{
            "reason",           "max_files_exceeded",
            "project",          @as(CStr, project_name),
            "discovered_files", @as(CStr, mcp.fmtZ(&files_text, "{d}", .{discovered_files})),
            "index_max_files",  @as(CStr, mcp.fmtZ(&limit_text, "{d}", .{max_files})),
        });
        _ = c.yyjson_mut_obj_add_str(doc, root, "status", "error");
        _ = c.yyjson_mut_obj_add_str(doc, root, "reason", "max_files_exceeded");
        _ = c.yyjson_mut_obj_add_str(doc, root, "previous_index", "preserved");
        _ = c.yyjson_mut_obj_add_int(doc, root, "index_max_files", max_files);
        _ = c.yyjson_mut_obj_add_int(doc, root, "discovered_files", discovered_files);
        const hint = mcp.fmtZ(&hint_buf, "Indexing stopped: discovered {d} files, which exceeds index_max_files ({d}); no partial graph was published and the previous index still serves. Raise index_max_files or exclude large subtrees.", .{ discovered_files, max_files });
        _ = c.yyjson_mut_obj_add_strcpy(doc, root, "hint", hint);
    } else if (rc == c.CBM_PIPELINE_ABORT_MAX_SOURCE_MB) {
        const mib: i64 = 1024 * 1024;
        const discovered_bytes = c.cbm_pipeline_get_discovered_source_bytes(p);
        const discovered_mb: i64 = @divTrunc(discovered_bytes + mib - 1, mib);
        var mb_text: [32]u8 = undefined;
        var limit_text: [32]u8 = undefined;
        var hint_buf: [512]u8 = undefined;
        interop.log(c.CBM_LOG_ERROR, "index.abort", .{
            "reason",               "max_source_mb_exceeded",
            "project",              @as(CStr, project_name),
            "discovered_source_mb", @as(CStr, mcp.fmtZ(&mb_text, "{d}", .{discovered_mb})),
            "index_max_source_mb",  @as(CStr, mcp.fmtZ(&limit_text, "{d}", .{max_source_mb})),
        });
        _ = c.yyjson_mut_obj_add_str(doc, root, "status", "error");
        _ = c.yyjson_mut_obj_add_str(doc, root, "reason", "max_source_mb_exceeded");
        _ = c.yyjson_mut_obj_add_str(doc, root, "previous_index", "preserved");
        _ = c.yyjson_mut_obj_add_int(doc, root, "index_max_source_mb", max_source_mb);
        _ = c.yyjson_mut_obj_add_int(doc, root, "discovered_source_mb", discovered_mb);
        _ = c.yyjson_mut_obj_add_int(doc, root, "discovered_source_bytes", discovered_bytes);
        const hint = mcp.fmtZ(&hint_buf, "Indexing stopped: discovered {d} MiB ({d} bytes) of source, which exceeds index_max_source_mb ({d} MiB); no partial graph was published and the previous index still serves. Raise index_max_source_mb or exclude large subtrees.", .{ discovered_mb, discovered_bytes, max_source_mb });
        _ = c.yyjson_mut_obj_add_strcpy(doc, root, "hint", hint);
    } else {
        _ = c.yyjson_mut_obj_add_str(doc, root, "status", "error");
        _ = c.yyjson_mut_obj_add_str(doc, root, "hint", "Pipeline failed. Check repo_path exists and contains source files. " ++
            "Try mode='fast' for a quicker diagnostic run.");
    }

    const json = mcp.docToStr(doc);
    c.yyjson_mut_doc_free(doc);
    // Free the pipeline only after the response doc copied the excluded list.
    // Supervised worker: skip the deep free — the process exits right after
    // handing over the response (main.zig fast-exits), and piecemeal-freeing a
    // multi-GB graph before process death costs minutes on kernel-scale
    // repos; the OS reclaims it wholesale at exit.
    if (c.cbm_index_worker_active()) {
        interop.logInfo("index.worker.fast_exit", .{ "skip", "pipeline_free" });
    } else {
        c.cbm_pipeline_free(p);
    }
    return mcp.textResultOwned(json, rc != 0);
}
