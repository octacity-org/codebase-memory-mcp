//! index_supervisor.zig — run index_repository in a supervised worker
//! subprocess. Implements src/mcp/index_supervisor.h; see that header for the
//! design (fork+exec only, recursion prevented by the --index-worker argv flag).

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const CStr = interop.CStr;

// ── Worker-role state ────────────────────────────────────────────────

var worker_active = false;
var worker_response_out = std.mem.zeroes([1024]u8);

pub export fn cbm_index_set_worker_role(is_worker: bool, response_out: [*c]const u8) void {
    worker_active = is_worker;
    if (response_out != null and response_out[0] != 0) {
        _ = interop.bprint(&worker_response_out, "{s}", .{std.mem.span(response_out)});
    } else {
        worker_response_out[0] = 0;
    }
}

pub export fn cbm_index_worker_active() bool {
    return worker_active;
}

pub export fn cbm_index_worker_response_out() [*c]const u8 {
    return if (worker_response_out[0] != 0) &worker_response_out else null;
}

/// Test hook (#845): counts spawn ATTEMPTS (entry to cbm_index_spawn_worker),
/// including ones that fail to resolve the self binary — an embedder must
/// never even try to spawn.
var spawn_count: c_int = 0;

pub export fn cbm_index_supervisor_spawn_count() c_int {
    return spawn_count;
}

/// Test hook: counts SINGLE-THREADED spawns. Production recovery is parallel-
/// only (there are no sequential production runs); this must stay ZERO on
/// every supervised path — any nonzero count means a recovery/probe regressed
/// to the sequential crawl that ground an 81k-file TS corpus for hours.
var spawn_st_count: c_int = 0;

pub export fn cbm_index_supervisor_spawn_st_count() c_int {
    return spawn_st_count;
}

/// #845: opt-in host mark — see the header. Set once from the real binary's
/// main(); embedders never set it, so should_wrap() stays false for them.
var host_marked = false;

pub export fn cbm_index_supervisor_mark_host() void {
    host_marked = true;
}

pub export fn cbm_index_supervisor_should_wrap() bool {
    if (!host_marked) return false; // embedder (#845): never spawn `<self> cli --index-worker`
    if (worker_active) return false; // I am the worker — run in-process, never re-supervise
    const sv = c.getenv("CBM_INDEX_SUPERVISOR");
    if (sv != null and std.mem.eql(u8, std.mem.span(sv), "0")) return false; // kill switch → in-process
    return true;
}

/// Quiet-timeout (ms) for a supervised worker: killed + reported as a hang if
/// it emits no NEW log line within the window. This is a NO-PROGRESS timeout —
/// every completed log line the worker tails (per-batch
/// parallel.extract.progress every 10 files, plus each pass boundary) resets
/// it — NOT a total-time cap, so a large repo that keeps making progress is
/// never falsely killed. Default: 15 min (a genuinely stuck file emits nothing,
/// so this fires only on a real hang). The CBM_INDEX_WORKER_TIMEOUT_S override
/// (seconds → ms) tightens it for tests.
fn workerQuietTimeoutMs() c_int {
    const default_quiet_timeout_ms = 900000; // 15 min with no progress
    const ms_per_second = 1000;
    var s: c_long = 0;
    if (c.cbm_env_long("CBM_INDEX_WORKER_TIMEOUT_S", &s) and s > 0 and s <= std.math.maxInt(c_int) / ms_per_second) {
        return @intCast(s * ms_per_second);
    }
    // An unreadable value ("30s") used to read as 0 and fall through to the
    // 15-minute default silently; say so before dropping it.
    var raw = std.mem.zeroes([64]u8);
    if (c.cbm_safe_getenv("CBM_INDEX_WORKER_TIMEOUT_S", &raw, raw.len, null) != null and raw[0] != 0) {
        interop.logWarn("index.supervisor.worker_timeout_ignored", .{ "value", @as(CStr, &raw), "action", "using_default" });
    }
    return default_quiet_timeout_ms;
}

/// Read an entire file into a heap string (NUL-terminated). NULL on error.
fn slurpFile(path: CStr) [*c]u8 {
    const f = c.cbm_fopen(path, "rb");
    if (f == null) return null;
    defer _ = c.fclose(f);
    if (c.fseek(f, 0, c.SEEK_END) != 0) return null;
    const n = c.ftell(f);
    if (n < 0) return null;
    _ = c.fseek(f, 0, c.SEEK_SET);
    const len: usize = @intCast(n);
    const buf = interop.mallocN(u8, len + 1);
    if (buf == null) return null;
    const rd = c.fread(buf, 1, len, f);
    buf[rd] = 0;
    return buf;
}

/// Resolve a per-run temp path <cache_dir>/logs/.worker-<pid><suffix>.
fn workerTmpPath(out: []u8, pid: c_int, suffix: []const u8) void {
    const cdir = c.cbm_resolve_cache_dir();
    if (cdir != null and cdir[0] != 0) {
        var dir: [900]u8 = undefined;
        _ = interop.bprint(&dir, "{s}/logs", .{std.mem.span(cdir)});
        _ = c.cbm_mkdir_p(&dir, 0o755);
        _ = interop.bprint(out, "{s}/.worker-{d}{s}", .{ std.mem.sliceTo(&dir, 0), pid, suffix });
    } else {
        _ = interop.bprint(out, ".worker-{d}{s}", .{ pid, suffix });
    }
}

fn getpid() c_int {
    if (builtin.os.tag == .windows) return @intCast(std.os.windows.GetCurrentProcessId());
    return @intCast(c.getpid());
}

pub export fn cbm_index_spawn_worker(args_json: [*c]const u8, single_thread: bool, marker_file: [*c]const u8, quarantine_file: [*c]const u8, result: [*c]c.cbm_index_worker_result_t) c_int {
    spawn_count += 1; // test hook (#845) — see cbm_index_supervisor_spawn_count
    if (single_thread) spawn_st_count += 1; // test hook — must stay 0: recovery is parallel-only
    result.*.outcome = c.CBM_PROC_SPAWN_FAILED;
    result.*.exit_code = -1;
    result.*.term_signal = 0;
    result.*.pid = 0;
    result.*.response = null;

    var self = std.mem.zeroes([1024]u8);
    if (!c.cbm_resolve_binary_path(null, &self, self.len) or self[0] == 0) {
        interop.logWarn("index.supervisor.no_self_path", .{ "action", "degrade_in_process" });
        return -1;
    }

    const pid = getpid();
    var resp_path: [1024]u8 = undefined;
    var log_path: [1024]u8 = undefined;
    workerTmpPath(&resp_path, pid, ".response");
    workerTmpPath(&log_path, pid, ".log");
    _ = c.remove(&resp_path); // clear any stale file

    // No --progress: the worker's DEFAULT structured logging already provides
    // the no-progress heartbeat (INFO parallel.extract.progress every 10 files
    // + each pass boundary — all newline-terminated → tailed → reset the
    // quiet-timeout). --progress would be strictly worse here: it installs a
    // REPLACE-mode sink that suppresses those default lines and emits per-file
    // extraction as a carriage-return in-place update (no trailing '\n'), which
    // cbm_tail_log does not count as progress. (It would not corrupt the
    // response either — that goes to the separate --response-out file, not
    // stdout.)
    const argv = [_]CStr{ &self, "cli", "--index-worker", "index_repository", args_json, "--response-out", &resp_path, null };

    // Recovery-run probe knobs → inherited env for the child. Spawns are
    // sequential, so mutating the parent's environment around a single spawn
    // is safe. Preserve a caller's worker override around a sequential recovery.
    const old_workers_value: CStr = if (single_thread) c.getenv("CBM_WORKERS") else null;
    const old_workers: [*c]u8 = if (old_workers_value != null) c.strdup(old_workers_value) else null;
    if (old_workers_value != null and old_workers == null) return -1;
    if (single_thread) _ = c.cbm_setenv("CBM_WORKERS", "0", 1);
    const has_marker = marker_file != null and marker_file[0] != 0;
    const has_quarantine = quarantine_file != null and quarantine_file[0] != 0;
    if (has_marker) _ = c.cbm_setenv("CBM_INDEX_MARKER_FILE", marker_file, 1);
    if (has_quarantine) _ = c.cbm_setenv("CBM_INDEX_QUARANTINE_FILE", quarantine_file, 1);

    var opts = std.mem.zeroes(c.cbm_proc_opts_t);
    opts.bin = &self;
    opts.argv = &argv;
    opts.log_file = &log_path;
    opts.quiet_timeout_ms = workerQuietTimeoutMs();
    // We manage log deletion ourselves after reaping (below): keep it on
    // failure for post-mortem, delete it only on a clean run. See the
    // observability note at the reap site.
    opts.delete_log_on_exit = false;

    var r: c.cbm_proc_result_t = undefined;
    const run_rc = c.cbm_subprocess_run(&opts, &r);

    if (single_thread) {
        if (old_workers != null) {
            _ = c.cbm_setenv("CBM_WORKERS", old_workers, 1);
        } else {
            _ = c.cbm_unsetenv("CBM_WORKERS");
        }
    }
    c.free(old_workers);
    if (has_marker) _ = c.cbm_unsetenv("CBM_INDEX_MARKER_FILE");
    if (has_quarantine) _ = c.cbm_unsetenv("CBM_INDEX_QUARANTINE_FILE");

    if (run_rc != 0) {
        _ = c.remove(&resp_path);
        _ = c.remove(&log_path); // empty/partial log from a failed spawn — nothing to keep
        interop.logWarn("index.supervisor.spawn_failed", .{ "action", "degrade_in_process" });
        return -1;
    }

    result.*.outcome = r.outcome;
    result.*.exit_code = r.exit_code;
    result.*.term_signal = r.term_signal;
    result.*.pid = r.pid;
    // A graceful nonzero exit (an isError result, e.g. over the memory budget)
    // also leaves its response behind; the supervisor passes it through.
    if (r.outcome == c.CBM_PROC_CLEAN or r.outcome == c.CBM_PROC_EXIT_NONZERO) {
        result.*.response = slurpFile(&resp_path);
    }
    _ = c.remove(&resp_path);

    var sig: [16]u8 = undefined;
    var exit_buf: [16]u8 = undefined;
    var pid_buf: [16]u8 = undefined;
    _ = interop.bprint(&sig, "{d}", .{r.term_signal});
    _ = interop.bprint(&exit_buf, "{d}", .{r.exit_code});
    _ = interop.bprint(&pid_buf, "{d}", .{r.pid});
    const outcome = c.cbm_proc_outcome_str(r.outcome);
    interop.logInfo("index.supervisor.reap", .{ "pid", @as(CStr, &pid_buf), "outcome", outcome, "exit_code", @as(CStr, &exit_buf), "signal", @as(CStr, &sig) });

    // Observability: on a CLEAN run the worker log is noise → delete it. On
    // ANY failure keep it and surface its path + raw exit code, so the
    // worker's own stdout/stderr (pipeline logs, any assert/abort text, the
    // exact exit code) is available post-mortem instead of vanishing.
    //
    // Exception: under CBM_PROFILE the log IS the deliverable — the worker's
    // msg=prof pass/sub-phase report is only written there, and deleting it
    // on success made profiling clean runs impossible. Keep it and say where.
    //
    // A worker that exited cleanly but left no response is a failure too: its
    // log is the only record of the run, so it is kept and named (#152).
    const clean = r.outcome == c.CBM_PROC_CLEAN and result.*.response != null;
    if (clean and !c.cbm_profile_active) {
        _ = c.remove(&log_path);
    } else if (clean) {
        interop.logInfo("index.supervisor.profile_log", .{ "log", @as(CStr, &log_path) });
    } else {
        interop.logWarn("index.supervisor.worker_failed", .{ "pid", @as(CStr, &pid_buf), "outcome", outcome, "exit_code", @as(CStr, &exit_buf), "log", @as(CStr, &log_path) });
    }
    return 0;
}

pub export fn cbm_index_worker_result_free(result: [*c]c.cbm_index_worker_result_t) void {
    if (result != null) {
        c.free(result.*.response);
        result.*.response = null;
    }
}
