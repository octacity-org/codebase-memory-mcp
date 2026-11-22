//! pipeline.zig — Indexing pipeline orchestrator.
//!
//! Coordinates multi-pass indexing:
//!   1. Discover files
//!   2. Build structure (Project/Folder/Package/File nodes)
//!   3. Bulk load sources
//!   4. Extract definitions (fused: extract + write nodes + build registry)
//!   5. Resolve imports, calls, usages, semantic edges
//!   6. Post-passes: tests, communities, HTTP links, git history
//!   7. Dump graph buffer to SQLite

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const streq = util.streq;
const ptrDiff = util.ptrDiff;
const HashTable = c.CBMHashTable;
const Gbuf = c.cbm_gbuf_t;
const Node = c.cbm_gbuf_node_t;
const Ctx = c.cbm_pipeline_ctx_t;
const FileInfo = c.cbm_file_info_t;
const FileError = c.cbm_file_error_t;
const FileResult = c.CBMFileResult;
const Spill = c.cbm_result_spill_t;
const StemMarks = c.cbm_stem_marks_t;
const Timespec = c.struct_timespec;

const CBM_DIR_PERMS = 0o755;
const BYTES_PER_MB = 1024 * 1024;

// ── Global pkgmap (one active pipeline at a time) ───────────────────

var g_pkgmap: ?*HashTable = null;

pub export fn cbm_pipeline_get_pkgmap() ?*HashTable {
    return g_pkgmap;
}

pub export fn cbm_pipeline_set_pkgmap(map: ?*HashTable) void {
    g_pkgmap = map;
}

// ── Internal state ──────────────────────────────────────────────────

/// struct cbm_pipeline. The headers only name it as an opaque type.
const Pipeline = extern struct {
    repo_path: MutStr,
    db_path: MutStr,
    project_name: MutStr,
    git_ctx: c.cbm_git_context_t,
    branch_qn: MutStr,
    mode: c.cbm_index_mode_t,
    /// atomic_int: read and written through @atomicLoad / @atomicStore.
    cancelled: c_int,

    // Indexing state (set during run)
    gbuf: ?*Gbuf,
    registry: ?*c.cbm_registry_t,

    /// Directory subtrees skipped during discovery (rel paths). Captured from
    /// cbm_discover_ex so the MCP layer can report excluded subtrees (#411).
    /// Owned by the pipeline; freed in cbm_pipeline_free.
    excluded_dirs: [*c]MutStr,
    excluded_count: c_int,

    /// Set when discovery could not fully materialize the tree (bounded walk
    /// stack full / allocation failure). The MCP layer reports a degraded index
    /// instead of a complete-looking partial one (#17, upstream 03dc9c91).
    discover_degraded: bool,

    /// Per-file indexing failures (skipped files) surfaced via MCP/CLI/logfile
    /// (Stage 2 / Track B). A skip is the expected handled outcome of a bad or
    /// oversized file — the run still succeeds ("indexed"). Owned by the
    /// pipeline; freed in cbm_pipeline_free.
    file_errors: [*c]FileError,
    file_errors_count: c_int,
    file_errors_cap: c_int,

    /// User-defined extension overrides (loaded once per run)
    userconfig: ?*c.cbm_userconfig_t,

    /// Committed graph size at dump time (-1 = dump did not run). #334 gate axis.
    committed_nodes: c_int,
    committed_edges: c_int,
    zova_publish_ms: f64,
    last_route: c.cbm_pipeline_route_t,
    /// The last run rebuilt a generation stamped with an older index format.
    format_migration: bool,
    /// Why the last run failed, as a stable code; null when it did not fail
    /// or the cause has no code of its own. Points at a string literal.
    failure_reason: CStr,
    zova_publish_stats: c.cbm_pipeline_zova_publish_stats_t,

    saved_summary: c.cbm_project_summary_export_t,
    saved_summary_valid: bool,

    /// Opt-in discovery size limits (0 or negative = disabled).
    max_files: c_int,
    max_source_mb: c_int,
    discovered_files: c_int,
    discovered_source_bytes: i64,
};

/// (int)x for a size_t, as C converts it.
fn truncInt(v: usize) c_int {
    return @bitCast(@as(c_uint, @truncate(v)));
}

/// cbm_log_itoa of an int-valued expression.
fn itoa(v: c_int) CStr {
    return c.cbm_log_itoa(v);
}

/// cbm_log_itoa of a byte count in whole megabytes.
fn mbText(bytes: usize) CStr {
    return c.cbm_log_itoa(truncInt(bytes / BYTES_PER_MB));
}

/// cbm_log_itoa of the milliseconds elapsed since `start`.
fn elapsedText(start: Timespec) CStr {
    return c.cbm_log_itoa(@intFromFloat(c.cbm_elapsed_ms(start)));
}

fn logError(msg: interop.Str, kvs: anytype) void {
    interop.log(c.CBM_LOG_ERROR, msg, kvs);
}

fn now(t: *Timespec) void {
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, t);
}

/// Log current + peak RSS and the budget at a pipeline phase boundary (memory
/// profiling). The process peak is cumulative, so the boundary where peak_mb
/// first rises is the phase that set it (#49).
pub export fn cbm_pipeline_log_phase_mem(phase: CStr) void {
    interop.logInfo("mem.phase", .{
        "phase",     phase,
        "rss_mb",    mbText(c.cbm_mem_rss()),
        "peak_mb",   mbText(c.cbm_mem_peak_rss()),
        "budget_mb", mbText(c.cbm_mem_budget()),
    });
}

/// Post-extraction budget gate. The extract phase enforces the budget with
/// back-pressure and result spill, but registry/resolve/derived passes/dump
/// work on the whole graph at once. Reclaim freed pages, then fail the attempt
/// whole if the resident graph is still over budget, before anything is
/// published, so the previously served generation keeps answering.
///
/// Only an explicit CBM_MEM_BUDGET_MB hard-fails. The detected RAM-fraction
/// default stays a soft target: it is sized for extraction's spill heuristics,
/// and a run that overshoots it after extraction already finishes today (#49
/// measurements). Hard-failing it would turn successful indexes into repeatable
/// over_memory_budget errors with no way to configure out but the env var.
/// Returns 0 when under budget, when the budget is implicit, or when none is
/// configured. Every call site is single-threaded: the workers have joined, so
/// there is no transient to wait for.
pub export fn cbm_pipeline_budget_gate(phase: CStr) c_int {
    if (c.cbm_mem_budget() == 0 or !c.cbm_mem_budget_explicit()) return 0;
    if (!c.cbm_mem_over_budget()) return 0;
    c.cbm_mem_collect();
    if (!c.cbm_mem_over_budget()) return 0;
    logError("mem.budget.exceeded", .{
        "rss_mb",    mbText(c.cbm_mem_rss()),
        "budget_mb", mbText(c.cbm_mem_budget()),
        "phase",     @as(CStr, if (phase != null) phase else "post_extract"),
        "action",    "fail_whole",
    });
    return c.CBM_PIPELINE_ABORT_OVER_BUDGET;
}

// ── Lifecycle ───────────────────────────────────────────────────────

pub export fn cbm_pipeline_new(repo_path: CStr, db_path: CStr, mode: c.cbm_index_mode_t) ?*Pipeline {
    if (repo_path == null) return null;

    const p: *Pipeline = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Pipeline)) orelse return null));

    p.repo_path = c.strdup(repo_path);
    p.db_path = if (db_path != null) c.strdup(db_path) else null;
    p.project_name = c.cbm_project_name_from_path(repo_path);
    _ = c.cbm_git_context_resolve(repo_path, &p.git_ctx);
    p.branch_qn = c.cbm_git_context_branch_qn(p.project_name, &p.git_ctx);
    p.mode = mode;
    p.committed_nodes = -1;
    p.committed_edges = -1;
    p.cancelled = 0;

    return p;
}

pub export fn cbm_pipeline_set_project_name(p_c: ?*Pipeline, name: CStr) bool {
    const p = p_c orelse return false;
    if (name == null or name[0] == 0) return false;

    const normalized: MutStr = c.cbm_project_name_from_path(name);
    if (normalized == null) return false;
    if (!c.cbm_validate_project_name(normalized)) {
        c.free(normalized);
        return false;
    }

    c.free(p.project_name);
    p.project_name = normalized;
    c.free(p.branch_qn);
    p.branch_qn = c.cbm_git_context_branch_qn(p.project_name, &p.git_ctx);
    return true;
}

pub export fn cbm_pipeline_free(p_c: ?*Pipeline) void {
    const p = p_c orelse return;
    c.free(p.repo_path);
    c.free(p.db_path);
    c.free(p.project_name);
    c.cbm_discover_free_excluded(p.excluded_dirs, p.excluded_count);
    p.excluded_dirs = null;
    p.excluded_count = 0;
    for (0..ix(p.file_errors_count)) |i| {
        c.free(p.file_errors[i].path);
        c.free(p.file_errors[i].reason);
        c.free(p.file_errors[i].phase);
    }
    c.free(p.file_errors);
    p.file_errors = null;
    p.file_errors_count = 0;
    p.file_errors_cap = 0;
    c.free(p.branch_qn);
    c.cbm_store_project_summary_export_free(&p.saved_summary);
    p.saved_summary_valid = false;
    c.cbm_git_context_free(&p.git_ctx);
    // gbuf, store, registry freed during/after run
    // Defensively free userconfig in case run() was never called or panicked
    if (p.userconfig != null) {
        c.cbm_set_user_lang_config(null);
        c.cbm_userconfig_free(p.userconfig);
        p.userconfig = null;
    }
    c.free(p);
}

pub export fn cbm_pipeline_cancel(p: ?*Pipeline) void {
    if (p) |pl| @atomicStore(c_int, &pl.cancelled, 1, .seq_cst);
}

pub export fn cbm_pipeline_project_name(p: ?*const Pipeline) CStr {
    return if (p) |pl| pl.project_name else null;
}

pub export fn cbm_pipeline_repo_path(p: ?*const Pipeline) CStr {
    return if (p) |pl| pl.repo_path else null;
}

pub export fn cbm_pipeline_cancelled_ptr(p: ?*Pipeline) ?*c_int {
    return if (p) |pl| &pl.cancelled else null;
}

pub export fn cbm_pipeline_get_mode(p: ?*const Pipeline) c_int {
    return if (p) |pl| @intCast(pl.mode) else 0;
}

pub export fn cbm_pipeline_set_max_files(p: ?*Pipeline, max_files: c_int) void {
    if (p) |pl| pl.max_files = max_files;
}

pub export fn cbm_pipeline_set_max_source_mb(p: ?*Pipeline, max_source_mb: c_int) void {
    if (p) |pl| pl.max_source_mb = max_source_mb;
}

pub export fn cbm_pipeline_get_discovered_files(p: ?*const Pipeline) c_int {
    return if (p) |pl| pl.discovered_files else 0;
}

pub export fn cbm_pipeline_get_discovered_source_bytes(p: ?*const Pipeline) i64 {
    return if (p) |pl| pl.discovered_source_bytes else 0;
}

pub export fn cbm_pipeline_get_excluded(p: ?*const Pipeline, out: ?*[*c]MutStr, count: ?*c_int) void {
    if (out) |o| o.* = if (p) |pl| pl.excluded_dirs else null;
    if (count) |n| n.* = if (p) |pl| pl.excluded_count else 0;
}

pub export fn cbm_pipeline_discover_degraded(p: ?*const Pipeline) bool {
    return if (p) |pl| pl.discover_degraded else false;
}

/// NULL-safe heap strdup (avoids a strdup dependency + guards NULL inputs).
fn feStrdup(s: CStr) MutStr {
    if (s == null) return null;
    const n = c.strlen(s) + 1;
    const d: MutStr = @ptrCast(c.malloc(n));
    if (d != null) @memcpy(d[0..n], s[0..n]);
    return d;
}

pub export fn cbm_pipeline_phase_is_partial(phase: CStr) bool {
    return phase != null and (streq(phase, "parse_partial") or streq(phase, "parse_unusable"));
}

pub export fn cbm_pipeline_add_file_error(p_c: ?*Pipeline, path: CStr, reason: CStr, phase: CStr) void {
    const p = p_c orelse return;
    if (p.file_errors_count >= p.file_errors_cap) {
        const ncap: c_int = if (p.file_errors_cap != 0) p.file_errors_cap * 2 else 16;
        const grown = interop.reallocN(FileError, p.file_errors, ix(ncap));
        // Never abort indexing just to record a skip — drop this record.
        if (grown == null) return;
        p.file_errors = grown;
        p.file_errors_cap = ncap;
    }
    const e = util.at(p.file_errors, ix(p.file_errors_count));
    e.path = feStrdup(path);
    e.reason = feStrdup(reason);
    e.phase = feStrdup(phase);
    p.file_errors_count += 1;
}

pub export fn cbm_pipeline_get_file_errors(p: ?*const Pipeline, out: ?*[*c]FileError, count: ?*c_int) void {
    if (out) |o| o.* = if (p) |pl| pl.file_errors else null;
    if (count) |n| n.* = if (p) |pl| pl.file_errors_count else 0;
}

fn orEmpty(s: CStr) CStr {
    return if (s != null) s else "";
}

/// Rebuild a File node's properties with its coverage stamp. The extension
/// key mirrors pass_structure / incr_add_changed_file_structure.
fn stampFileCoverage(p: *Pipeline, gbuf: ?*Gbuf, e: *const FileError, partial: bool) bool {
    const file_qn: MutStr = c.cbm_pipeline_fqn_compute(p.project_name, e.path, "__file__");
    if (file_qn == null) return false;
    const slash: CStr = c.strrchr(e.path, '/');
    const basename: CStr = if (slash != null) slash + 1 else e.path;
    const ext: CStr = c.strrchr(basename, '.');
    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_str(doc, root, "extension", orEmpty(ext));
    if (partial) {
        _ = c.yyjson_mut_obj_add_str(doc, root, "coverage", c.CBM_COVERAGE_PARTIAL);
        _ = c.yyjson_mut_obj_add_str(doc, root, "parse_error_ranges", orEmpty(e.reason));
        if (streq(e.phase, "parse_unusable")) {
            _ = c.yyjson_mut_obj_add_bool(doc, root, "parse_unusable", true);
        }
    } else {
        _ = c.yyjson_mut_obj_add_str(doc, root, "coverage", c.CBM_COVERAGE_MISSED);
        _ = c.yyjson_mut_obj_add_str(doc, root, "coverage_phase", e.phase);
        _ = c.yyjson_mut_obj_add_str(doc, root, "coverage_reason", orEmpty(e.reason));
    }
    const props: MutStr = c.yyjson_mut_write(doc, 0, null);
    c.yyjson_mut_doc_free(doc);
    const stamped = props != null and c.cbm_gbuf_set_node_props(gbuf, file_qn, props);
    c.free(props);
    c.free(file_qn);
    return stamped;
}

pub export fn cbm_pipeline_stamp_coverage(p_c: ?*Pipeline, gbuf: ?*Gbuf) void {
    const p = p_c orelse return;
    if (gbuf == null) return;
    var partial: c_int = 0;
    var missed: c_int = 0;
    // Partial first, so a file recorded under both keeps the stronger
    // "missed" stamp.
    for (0..2) |pass| {
        for (0..ix(p.file_errors_count)) |i| {
            const e = util.at(p.file_errors, i);
            if (e.path == null or e.phase == null) continue;
            const is_partial = cbm_pipeline_phase_is_partial(e.phase);
            if (is_partial != (pass == 0)) continue;
            if (stampFileCoverage(p, gbuf, e, is_partial)) {
                if (is_partial) partial += 1 else missed += 1;
            }
        }
    }
    if (partial > 0 or missed > 0) {
        interop.logInfo("index.coverage", .{ "partial", itoa(partial), "missed", itoa(missed) });
    }
}

pub export fn cbm_pipeline_get_committed_counts(p: ?*const Pipeline, nodes: ?*c_int, edges: ?*c_int) void {
    if (nodes) |n| n.* = if (p) |pl| pl.committed_nodes else -1;
    if (edges) |e| e.* = if (p) |pl| pl.committed_edges else -1;
}

pub export fn cbm_pipeline_get_zova_publish_ms(p: ?*const Pipeline) f64 {
    return if (p) |pl| pl.zova_publish_ms else 0.0;
}

pub export fn cbm_pipeline_get_last_route(p: ?*const Pipeline) c.cbm_pipeline_route_t {
    return if (p) |pl| pl.last_route else c.CBM_PIPELINE_ROUTE_UNKNOWN;
}

// ── Main-buffer id counter sync after parallel phases (#50) ─────────

const IdSyncHook = ?*const fn (?*Gbuf, ?*anyopaque) callconv(.c) void;
const BeforeDumpHook = ?*const fn (?*const Gbuf, ?*anyopaque) callconv(.c) void;
const ShareIdsHook = ?*const fn (?*Gbuf, [*c]i64, ?*anyopaque) callconv(.c) void;

threadlocal var g_id_sync_hook: IdSyncHook = null;
threadlocal var g_id_sync_hook_ctx: ?*anyopaque = null;
threadlocal var g_before_dump_hook: BeforeDumpHook = null;
threadlocal var g_before_dump_hook_ctx: ?*anyopaque = null;

pub export fn cbm_pipeline_set_id_sync_hook_for_tests(hook: IdSyncHook, ctx: ?*anyopaque) void {
    g_id_sync_hook = hook;
    g_id_sync_hook_ctx = ctx;
}

pub export fn cbm_pipeline_set_before_dump_hook_for_tests(hook: BeforeDumpHook, ctx: ?*anyopaque) void {
    g_before_dump_hook = hook;
    g_before_dump_hook_ctx = ctx;
}

pub export fn cbm_pipeline_run_before_dump_hook(gb: ?*const Gbuf) void {
    if (g_before_dump_hook) |hook| hook(gb, g_before_dump_hook_ctx);
}

pub export fn cbm_pipeline_sync_next_id(gb: ?*Gbuf, shared_ids: *i64) void {
    if (g_id_sync_hook) |hook| hook(gb, g_id_sync_hook_ctx);
    // Raise only. The merge leaves the main counter above the shared one when
    // merged edges take main-counter ids; lowering it to the shared value
    // handed those ids out a second time.
    c.cbm_gbuf_raise_next_id(gb, @atomicLoad(i64, shared_ids, .seq_cst));
}

// ── Shared id counter raise before parallel resolve (#88) ───────────

threadlocal var g_share_ids_hook: ShareIdsHook = null;
threadlocal var g_share_ids_hook_ctx: ?*anyopaque = null;

pub export fn cbm_pipeline_set_share_ids_hook_for_tests(hook: ShareIdsHook, ctx: ?*anyopaque) void {
    g_share_ids_hook = hook;
    g_share_ids_hook_ctx = ctx;
}

pub export fn cbm_pipeline_share_next_id(gb: ?*Gbuf, shared_ids: *i64) void {
    if (g_share_ids_hook) |hook| hook(gb, shared_ids, g_share_ids_hook_ctx);
    // No worker runs here, so a plain load and store is enough. Raise only:
    // the shared counter may already be above the main one.
    const main_next = c.cbm_gbuf_next_id(gb);
    if (main_next > @atomicLoad(i64, shared_ids, .seq_cst)) {
        @atomicStore(i64, shared_ids, main_next, .seq_cst);
    }
}

pub export fn cbm_pipeline_had_format_migration(p: ?*const Pipeline) bool {
    return if (p) |pl| pl.format_migration else false;
}

pub export fn cbm_pipeline_failure_reason(p: ?*const Pipeline) CStr {
    const pl = p orelse return null;
    // A cancelled run fails wherever it happened to stop; name the cancel.
    return if (cancelled(pl)) "cancelled" else pl.failure_reason;
}

pub export fn cbm_pipeline_get_zova_publish_stats(
    p_c: ?*const Pipeline,
    out_stats: ?*c.cbm_pipeline_zova_publish_stats_t,
) bool {
    const p = p_c orelse return false;
    const out = out_stats orelse return false;
    if (!p.zova_publish_stats.completed) return false;
    out.* = p.zova_publish_stats;
    return true;
}

pub export fn cbm_pipeline_set_committed_counts(p: ?*Pipeline, nodes: c_int, edges: c_int) void {
    if (p) |pl| {
        pl.committed_nodes = nodes;
        pl.committed_edges = edges;
    }
}

fn cancelled(p: *const Pipeline) bool {
    return @atomicLoad(c_int, &p.cancelled, .seq_cst) != 0;
}

// ── Hash table cleanup callback ─────────────────────────────────────

fn freeSeenDirKey(key: CStr, val: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = val;
    _ = ud;
    c.free(@constCast(key));
}

// ── Pass 1: Structure ───────────────────────────────────────────────

/// True for languages whose module QN is the CONTAINING DIRECTORY (Java/Go
/// package). MUST match cbm_lang_module_is_dir() (internal/cbm/helpers.c).
fn stemModuleIsDir(lang: c.CBMLanguage) bool {
    return lang == c.CBM_LANG_JAVA or lang == c.CBM_LANG_GO;
}

/// Each file's Module QN, computed as extraction computes it, and the Folder
/// QN of every directory above it.
pub export fn cbm_stem_marks_init(m: *StemMarks, project: CStr, files: [*c]const FileInfo, file_count: c_int) void {
    c.cbm_arena_init(&m.arena);
    m.module_qns = c.cbm_ht_create(c.CBM_SZ_256);
    m.dir_module_qns = c.cbm_ht_create(c.CBM_SZ_64);
    m.folder_qns = c.cbm_ht_create(c.CBM_SZ_256);
    for (0..ix(file_count)) |i| {
        const rel: CStr = files[i].rel_path;
        if (rel == null) continue;
        const qn: MutStr = c.cbm_fqn_module_source_lang(&m.arena, project, rel, files[i].language);
        if (qn != null) {
            const table = if (stemModuleIsDir(files[i].language)) m.dir_module_qns else m.module_qns;
            _ = c.cbm_ht_set(table, qn, qn);
        }
        var s: CStr = c.strchr(rel, '/');
        while (s != null) : (s = c.strchr(s + 1, '/')) {
            const dir: MutStr = c.cbm_arena_strndup(&m.arena, rel, ptrDiff(s, rel));
            const fqn: MutStr = if (dir != null) c.cbm_fqn_folder(&m.arena, project, dir) else null;
            if (fqn != null) _ = c.cbm_ht_set(m.folder_qns, fqn, fqn);
        }
    }
}

pub export fn cbm_stem_marks_free(m: *StemMarks) void {
    c.cbm_ht_free(m.module_qns);
    c.cbm_ht_free(m.dir_module_qns);
    c.cbm_ht_free(m.folder_qns);
    c.cbm_arena_destroy(&m.arena);
}

pub export fn cbm_stem_marks_side(m_c: ?*const StemMarks, qn: CStr) c_int {
    const m = m_c orelse return c.CBM_STEM_NONE;
    if (qn == null or c.cbm_ht_get(m.module_qns, qn) == null) return c.CBM_STEM_NONE;
    if (c.cbm_ht_get(m.dir_module_qns, qn) != null) return c.CBM_STEM_MODULE_YIELDS;
    return if (c.cbm_ht_get(m.folder_qns, qn) != null) c.CBM_STEM_FOLDER_YIELDS else c.CBM_STEM_NONE;
}

/// The extension names the yielding Module's file ("sh" for genproto.sh);
/// "file" when there is none.
fn stemMarkSuffix(rel_path: CStr) CStr {
    var base: CStr = c.strrchr(rel_path, '/');
    base = if (base != null) base + 1 else rel_path;
    const dot: CStr = c.strrchr(base, '.');
    return if (dot != null and dot != base and dot[1] != 0) dot + 1 else "file";
}

pub export fn cbm_stem_marks_module_qn(
    m_c: ?*const StemMarks,
    qn: CStr,
    rel_path: CStr,
    lang: c.CBMLanguage,
    buf: MutStr,
    buf_size: usize,
) CStr {
    const m = m_c orelse return qn;
    if (qn == null or rel_path == null or stemModuleIsDir(lang) or
        c.cbm_ht_get(m.dir_module_qns, qn) == null)
    {
        return qn;
    }
    const n = c.snprintf(buf, buf_size, "%s#%s", qn, stemMarkSuffix(rel_path));
    return if (n > 0 and ix(n) < buf_size) buf else qn;
}

pub export fn cbm_stem_marks_folder_qn(
    m: ?*const StemMarks,
    project: CStr,
    rel_dir: CStr,
    buf: MutStr,
    buf_size: usize,
) CStr {
    const qn: MutStr = c.cbm_pipeline_fqn_folder(project, rel_dir);
    if (qn == null) return null;
    const mark = if (m) |marks|
        c.cbm_ht_get(marks.module_qns, qn) != null and c.cbm_ht_get(marks.dir_module_qns, qn) == null
    else
        false;
    const n = c.snprintf(buf, buf_size, "%s%s", qn, @as(CStr, if (mark) "/" else ""));
    c.free(qn);
    return if (n >= 0 and ix(n) < buf_size) buf else null;
}

/// Create Project, Folder/Package, and File nodes in the graph buffer.
/// Walk directory chain upward, creating Folder nodes and CONTAINS_FOLDER edges.
fn createFolderChain(p: *Pipeline, marks: ?*const StemMarks, dir: CStr, seen_dirs: ?*HashTable) void {
    const walk: MutStr = c.strdup(dir);
    while (walk[0] != 0 and c.cbm_ht_get(seen_dirs, walk) == null) {
        _ = c.cbm_ht_set(seen_dirs, c.strdup(walk), @ptrFromInt(1));
        var folder_buf: [c.CBM_SZ_8K]u8 = undefined;
        const folder_qn = cbm_stem_marks_folder_qn(marks, p.project_name, walk, &folder_buf, folder_buf.len);
        var dir_base: CStr = c.strrchr(walk, '/');
        dir_base = if (dir_base != null) dir_base + 1 else walk;
        _ = c.cbm_gbuf_upsert_node(p.gbuf, "Folder", dir_base, folder_qn, walk, 0, 0, "{}");

        var pdir: MutStr = c.strdup(walk);
        const ps: MutStr = c.strrchr(pdir, '/');
        if (ps != null) {
            ps[0] = 0;
        } else {
            c.free(pdir);
            pdir = c.strdup("");
        }
        var pqn_buf: [c.CBM_SZ_8K]u8 = undefined;
        const pqn: CStr = if (pdir[0] == 0)
            (if (p.branch_qn != null) p.branch_qn else p.project_name)
        else
            cbm_stem_marks_folder_qn(marks, p.project_name, pdir, &pqn_buf, pqn_buf.len);
        const fnode: ?*const Node = c.cbm_gbuf_find_by_qn(p.gbuf, folder_qn);
        const pnode: ?*const Node = c.cbm_gbuf_find_by_qn(p.gbuf, pqn);
        if (fnode != null and pnode != null) {
            _ = c.cbm_gbuf_insert_edge(p.gbuf, pnode.?.id, fnode.?.id, "CONTAINS_FOLDER", "{}");
        }
        const up: MutStr = c.strrchr(walk, '/');
        if (up != null) up[0] = 0 else walk[0] = 0;
        c.free(pdir);
    }
    c.free(walk);
}

fn passStructure(p: *Pipeline, marks: ?*const StemMarks, files: [*c]const FileInfo, file_count: c_int) c_int {
    interop.logInfo("pass.start", .{ "pass", "structure", "files", itoa(file_count) });

    // Project node
    // coverage_version marks a generation whose File nodes carry coverage
    // stamps; incremental runs keep the node, so older generations stay
    // unmarked until the next full index. index_format is checked by
    // try_incremental before an existing generation is reused.
    _ = c.cbm_gbuf_upsert_node(
        p.gbuf,
        "Project",
        p.project_name,
        p.project_name,
        null,
        0,
        0,
        "{\"coverage_version\":" ++ c.CBM_COVERAGE_VERSION_STR ++
            ",\"index_format\":" ++ c.CBM_INDEX_FORMAT_VERSION_STR ++ "}",
    );
    const branch_qn: CStr = if (p.branch_qn != null) p.branch_qn else p.project_name;
    const branch_name: CStr = if (p.git_ctx.branch != null) p.git_ctx.branch else "working-tree";
    var branch_props: [c.CBM_SZ_2K]u8 = undefined;
    var branch_props_json: CStr = "{}";
    if (c.cbm_git_context_props_json(&p.git_ctx, &branch_props, branch_props.len) > 0) {
        branch_props_json = &branch_props;
    }
    if (p.branch_qn != null) {
        const branch_id = c.cbm_gbuf_upsert_node(p.gbuf, "Branch", branch_name, branch_qn, null, 0, 0, branch_props_json);
        const project_node: ?*const Node = c.cbm_gbuf_find_by_qn(p.gbuf, p.project_name);
        if (project_node != null and branch_id > 0) {
            _ = c.cbm_gbuf_insert_edge(p.gbuf, project_node.?.id, branch_id, "HAS_BRANCH", branch_props_json);
        }
    }

    // Collect unique directories and create Folder/Package nodes
    const seen_dirs = c.cbm_ht_create(c.CBM_SZ_256);

    for (0..ix(file_count)) |i| {
        const rel: CStr = files[i].rel_path;
        if (rel == null) continue;

        // Create File node
        const file_qn: MutStr = c.cbm_pipeline_fqn_compute(p.project_name, rel, "__file__");
        // Extract basename
        const slash: CStr = c.strrchr(rel, '/');
        const basename: CStr = if (slash != null) slash + 1 else rel;

        var props: [c.CBM_SZ_256]u8 = undefined;
        const ext: CStr = c.strrchr(basename, '.');
        _ = c.snprintf(&props, props.len, "{\"extension\":\"%s\"}", orEmpty(ext));

        _ = c.cbm_gbuf_upsert_node(p.gbuf, "File", basename, file_qn, rel, 0, 0, &props);

        // CONTAINS_FILE edge: parent dir -> file
        var dir: MutStr = c.strdup(rel);
        const last_slash: MutStr = c.strrchr(dir, '/');
        if (last_slash != null) {
            last_slash[0] = 0;
        } else {
            c.free(dir);
            dir = c.strdup("");
        }

        var parent_buf: [c.CBM_SZ_8K]u8 = undefined;
        const parent_qn: CStr = if (dir[0] == 0)
            branch_qn
        else
            cbm_stem_marks_folder_qn(marks, p.project_name, dir, &parent_buf, parent_buf.len);

        // Walk up directory chain, creating Folder nodes
        createFolderChain(p, marks, dir, seen_dirs);

        // Now create the CONTAINS_FILE edge
        const fnode: ?*const Node = c.cbm_gbuf_find_by_qn(p.gbuf, file_qn);
        const pnode: ?*const Node = c.cbm_gbuf_find_by_qn(p.gbuf, parent_qn);
        if (fnode != null and pnode != null) {
            _ = c.cbm_gbuf_insert_edge(p.gbuf, pnode.?.id, fnode.?.id, "CONTAINS_FILE", "{}");
        }

        c.free(file_qn);
        c.free(dir);
    }

    // Free seen_dirs keys
    c.cbm_ht_foreach(seen_dirs, freeSeenDirKey, null);
    c.cbm_ht_free(seen_dirs);

    interop.logInfo("pass.done", .{
        "pass",  "structure",
        "nodes", itoa(c.cbm_gbuf_node_count(p.gbuf)),
        "edges", itoa(c.cbm_gbuf_edge_count(p.gbuf)),
    });
    return 0;
}

// ── Githistory compute thread (for fused post-pass parallelism) ─────

const GhComputeArg = extern struct {
    repo_path: CStr,
    result: *c.cbm_githistory_result_t,
};

fn ghComputeThreadFn(arg: ?*anyopaque) callconv(.c) ?*anyopaque {
    const a: *GhComputeArg = @ptrCast(@alignCast(arg));
    _ = c.cbm_pipeline_githistory_compute(a.repo_path, a.result);
    return null;
}

/// "async" when an infra binding names no broker.
fn brokerOrAsync(ib: *const c.CBMInfraBinding) CStr {
    return if (ib.broker != null) ib.broker else "async";
}

// Extract Route nodes from URL strings found in config files (YAML, HCL, TOML).
// These are infrastructure-defined endpoints (Cloud Scheduler, Terraform).
// Process infra bindings: topic→URL pairs from IaC configs.
// Creates Route nodes for endpoints and HANDLES edges linking
// topic Routes to endpoint Routes (bridging the gap).
/// Process one infra binding: create Route node + INFRA_MAPS edge.
fn processOneInfraBinding(gbuf: ?*Gbuf, ib: *const c.CBMInfraBinding, rel_path: CStr) c_int {
    var url_route_qn: [c.CBM_ROUTE_QN_SIZE]u8 = undefined;
    _ = c.snprintf(&url_route_qn, url_route_qn.len, "__route__infra__%s", ib.target_url);
    const url_route_id = c.cbm_gbuf_upsert_node(gbuf, "Route", ib.target_url, &url_route_qn, rel_path, 0, 0, "{\"source\":\"infra\"}");
    var topic_route_qn: [c.CBM_ROUTE_QN_SIZE]u8 = undefined;
    _ = c.snprintf(&topic_route_qn, topic_route_qn.len, "__route__%s__%s", brokerOrAsync(ib), ib.source_name);
    const topic_route: ?*const Node = c.cbm_gbuf_find_by_qn(gbuf, &topic_route_qn);
    var topic_route_id: i64 = undefined;
    if (topic_route) |route| {
        topic_route_id = route.id;
    } else {
        // The config file IS the declaration that the topic/queue/schedule exists;
        // upsert its Route node so the binding maps even when no code-side dispatch
        // call created the node first (e.g. a standalone scheduler/subscription
        // manifest).
        topic_route_id = c.cbm_gbuf_upsert_node(gbuf, "Route", ib.source_name, &topic_route_qn, rel_path, 0, 0, brokerOrAsync(ib));
        if (topic_route_id <= 0) return 0;
    }
    var props: [c.CBM_SZ_512]u8 = undefined;
    _ = c.snprintf(
        &props,
        props.len,
        "{\"broker\":\"%s\",\"topic\":\"%s\",\"endpoint\":\"%s\"}",
        brokerOrAsync(ib),
        ib.source_name,
        ib.target_url,
    );
    _ = c.cbm_gbuf_insert_edge(gbuf, topic_route_id, url_route_id, "INFRA_MAPS", &props);
    return 1;
}

pub export fn cbm_pipeline_process_infra_bindings(
    gbuf: ?*Gbuf,
    files: [*c]const FileInfo,
    result_cache: [*c][*c]FileResult,
    spill: ?*Spill,
    file_count: c_int,
) void {
    var bindings: c_int = 0;
    for (0..ix(file_count)) |i| {
        const fi: c_int = @intCast(i);
        if (result_cache[i] == null and c.cbm_result_spill_infra_binding_count(spill, fi) == 0) {
            continue;
        }
        const result: *FileResult = c.cbm_result_cache_acquire(result_cache, spill, fi) orelse continue;
        for (0..ix(result.infra_bindings.count)) |bi| {
            const ib = util.at(result.infra_bindings.items, bi);
            if (ib.source_name != null and ib.target_url != null) {
                bindings += processOneInfraBinding(gbuf, ib, files[i].rel_path);
            }
        }
        c.cbm_result_cache_release(result_cache, fi, result);
    }
    if (bindings > 0) {
        var buf: [c.CBM_SZ_16]u8 = undefined;
        _ = c.snprintf(&buf, buf.len, "%d", bindings);
        interop.logInfo("pass.infra_bindings", .{ "linked", @as(CStr, &buf) });
    }
}

pub export fn cbm_pipeline_is_infra_file(fp: CStr) bool {
    return fp != null and
        (c.strstr(fp, ".yaml") != null or c.strstr(fp, ".yml") != null or
            c.strstr(fp, ".tf") != null or c.strstr(fp, ".hcl") != null or
            c.strstr(fp, ".toml") != null);
}

/// True when a YAML key path denotes an UPSTREAM dependency, CONFIG value, or
/// HEALTHCHECK target rather than an endpoint this service exposes. Such URLs
/// (auth JWKS, downstream service base URLs, package-registry URLs, healthcheck
/// curl targets) are NOT routes the service serves and must not mint Route nodes
/// (#521). Exposed-endpoint keys (push_endpoint, post_url, callback, webhook)
/// are intentionally absent here so they still produce infra Route nodes.
fn isUpstreamConfigKey(key_path: CStr) bool {
    // No key context (e.g. flat string) — keep prior behaviour and mint.
    if (key_path == null) return false;
    const deny = [_][*:0]const u8{
        "jwks", "registry", "registries", "healthcheck", "upstream", "_service_url", "auth",
    };
    for (deny) |d| {
        if (c.strstr(key_path, d) != null) return true;
    }
    return false;
}

/// Try to create an infra Route node from one string_ref.
fn tryUpsertInfraRoute(gbuf: ?*Gbuf, sr: *const c.CBMStringRef, fp: CStr) void {
    if (sr.kind != c.CBM_STRREF_URL or sr.value == null or c.strstr(sr.value, "://") == null) return;
    // Skip upstream/config/healthcheck URLs — they are not exposed routes (#521).
    if (isUpstreamConfigKey(sr.key_path)) return;
    var route_qn: [c.CBM_ROUTE_QN_SIZE]u8 = undefined;
    _ = c.snprintf(&route_qn, route_qn.len, "__route__infra__%s", sr.value);
    var route_props: [c.CBM_SZ_512]u8 = undefined;
    if (sr.key_path != null) {
        _ = c.snprintf(&route_props, route_props.len, "{\"source\":\"infra\",\"key_path\":\"%s\"}", sr.key_path);
    } else {
        _ = c.snprintf(&route_props, route_props.len, "{\"source\":\"infra\"}");
    }
    _ = c.cbm_gbuf_upsert_node(gbuf, "Route", sr.value, &route_qn, fp, 0, 0, &route_props);
}

// A URL string_ref that does NOT denote a route the service serves: a value
// containing whitespace is a command/sentence with an embedded URL (e.g. a
// Docker healthcheck `curl --fail http://... || exit 1`); a NULL key_path is a
// context-less/duplicate ref; an upstream/config/healthcheck key is an external
// dependency, not an exposed route. (#521)
/// True when one dotted key segment names an endpoint the service is called at:
/// Pub/Sub push endpoints, Cloud Scheduler/Tasks `uri`, webhooks, callbacks,
/// post URLs. Case, '_' and '-' are ignored (pushEndpoint == push_endpoint).
fn isEndpointKeySegment(segment: CStr, len: usize) bool {
    var norm: [c.CBM_SZ_64]u8 = undefined;
    var n: usize = 0;
    var i: usize = 0;
    while (i < len and n < norm.len - 1) : (i += 1) {
        const ch = segment[i];
        if (ch == '_' or ch == '-') continue;
        norm[n] = if (ch >= 'A' and ch <= 'Z') ch - 'A' + 'a' else ch;
        n += 1;
    }
    norm[n] = 0;
    const contains = [_][*:0]const u8{ "endpoint", "webhook", "callback", "posturl" };
    for (contains) |needle| {
        if (c.strstr(&norm, needle) != null) return true;
    }
    return streq(&norm, "uri");
}

/// Allow-list, not deny-list: generic keys such as `url`, `link` or `avatarUrl`
/// hold documentation and data URLs (sponsor lists, profiles, images), so only
/// endpoint-named keys mint infra Routes. Upstream/config keys stay denied. (#521)
pub export fn cbm_pipeline_infra_route_key_allowed(key_path: CStr) bool {
    if (key_path == null or isUpstreamConfigKey(key_path)) return false;
    var segment = key_path;
    while (true) {
        const dot: CStr = c.strchr(segment, '.');
        const len = if (dot != null) ptrDiff(dot, segment) else c.strlen(segment);
        if (isEndpointKeySegment(segment, len)) return true;
        if (dot == null) return false;
        segment = dot + 1;
    }
}

fn routeSrDenied(sr: *const c.CBMStringRef) bool {
    if (sr.value == null or c.strchr(sr.value, ' ') != null) return true;
    return !cbm_pipeline_infra_route_key_allowed(sr.key_path);
}

pub export fn cbm_pipeline_extract_infra_routes(
    gbuf: ?*Gbuf,
    files: [*c]const FileInfo,
    result_cache: [*c][*c]FileResult,
    spill: ?*Spill,
    file_count: c_int,
) void {
    // DENY-WINS-BY-VALUE: the same URL is often extracted as several string_refs
    // at different key_path granularities (full path, leaf key, flat). The Route
    // node is keyed by VALUE, so it would be minted if ANY granularity passed the
    // per-ref guard — e.g. a denied full path `registries.terraform-registry.url`
    // is defeated by a sibling leaf `url`. So pass 1 collects every URL value
    // denied under ANY of its refs; pass 2 mints only values never denied. (#521)
    const denied = c.cbm_ht_create(16);
    // `denied` borrows its keys; values from a loaded (spilled) result are
    // copied here because the result is freed before pass 2 reads them.
    var denied_keys: c.CBMArena = undefined;
    c.cbm_arena_init(&denied_keys);
    for (0..2) |pass| {
        for (0..ix(file_count)) |i| {
            const fi: c_int = @intCast(i);
            if (!cbm_pipeline_is_infra_file(files[i].rel_path) or
                (result_cache[i] == null and c.cbm_result_spill_string_ref_count(spill, fi) == 0))
            {
                continue;
            }
            const result: *FileResult = c.cbm_result_cache_acquire(result_cache, spill, fi) orelse continue;
            const loaded = result != @as(?*FileResult, result_cache[i]);
            for (0..ix(result.string_refs.count)) |si| {
                const sr = util.at(result.string_refs.items, si);
                if (sr.kind != c.CBM_STRREF_URL or sr.value == null or
                    c.strstr(sr.value, "://") == null)
                {
                    continue;
                }
                if (pass == 0) {
                    if (denied != null and routeSrDenied(sr) and !c.cbm_ht_has(denied, sr.value)) {
                        const key: CStr = if (loaded)
                            c.cbm_arena_strdup(&denied_keys, sr.value)
                        else
                            sr.value;
                        if (key != null) _ = c.cbm_ht_set(denied, key, @ptrFromInt(1));
                    }
                } else if (denied == null or !c.cbm_ht_has(denied, sr.value)) {
                    tryUpsertInfraRoute(gbuf, sr, files[i].rel_path);
                }
            }
            c.cbm_result_cache_release(result_cache, fi, result);
        }
    }
    c.cbm_ht_free(denied);
    c.cbm_arena_destroy(&denied_keys);
}

// Run decorator_tags, configlink, and route matching passes.
fn predumpDeco(ctx: *Ctx) void {
    _ = c.cbm_pipeline_pass_decorator_tags(ctx.gbuf, ctx.project_name);
}
fn predumpRoute(ctx: *Ctx) void {
    _ = c.cbm_pipeline_create_route_nodes(ctx.gbuf);
}
fn predumpSim(ctx: *Ctx) void {
    _ = c.cbm_pipeline_pass_similarity(ctx);
}
fn predumpSem(ctx: *Ctx) void {
    _ = c.cbm_pipeline_pass_semantic_edges(ctx);
}
fn predumpCfg(ctx: *Ctx) void {
    _ = c.cbm_pipeline_pass_configlink(ctx);
}
fn predumpComplexity(ctx: *Ctx) void {
    _ = c.cbm_pipeline_pass_complexity(ctx);
}
fn predumpImportance(ctx: *Ctx) void {
    _ = c.cbm_pipeline_pass_importance(ctx);
}

const PredumpPass = struct {
    run: *const fn (*Ctx) void,
    name: [*:0]const u8,
    /// true = skip in fast mode
    moderate_only: bool,
};

const predump_passes = [_]PredumpPass{
    .{ .run = predumpDeco, .name = "decorator_tags", .moderate_only = false },
    .{ .run = predumpCfg, .name = "configlink", .moderate_only = false },
    .{ .run = predumpRoute, .name = "route_match", .moderate_only = false },
    .{ .run = predumpSim, .name = "similarity", .moderate_only = true },
    .{ .run = predumpSem, .name = "semantic_edges", .moderate_only = true },
    .{ .run = predumpComplexity, .name = "complexity", .moderate_only = false },
    // Importance runs LAST: it reads CALLS/USAGE (extraction) and TESTS
    // (pass_tests, run before this loop).
    .{ .run = predumpImportance, .name = "importance", .moderate_only = false },
};

fn runPredumpPasses(p: *Pipeline, ctx: *Ctx) c_int {
    var t: Timespec = undefined;
    for (predump_passes) |pass| {
        if (cancelled(p)) break;
        // "moderate_only" passes (similarity/semantic edges) run in FULL,
        // MODERATE and ADVANCED — they are skipped only in FAST. Compare
        // explicitly against FAST rather than `> MODERATE` so ADVANCED
        // (numerically 3) is not mistaken for a lighter mode than FULL.
        if (pass.moderate_only and p.mode == c.CBM_MODE_FAST) continue;
        // #49: each derived pass allocates whole-graph scratch. Check before
        // it starts so an over-budget run stops instead of stacking another
        // pass's working set on top.
        var phase: [c.CBM_SZ_64]u8 = undefined;
        _ = c.snprintf(&phase, phase.len, "before_%s", pass.name);
        const gate_rc = cbm_pipeline_budget_gate(&phase);
        if (gate_rc != 0) return gate_rc;
        now(&t);
        pass.run(ctx);
        interop.logInfo("pass.timing", .{ "pass", pass.name, "elapsed_ms", elapsedText(t) });
        cbm_pipeline_log_phase_mem(pass.name);
    }
    return 0;
}

fn freeResultCache(cache: [*c][*c]FileResult, file_count: c_int) void {
    for (0..ix(file_count)) |i| {
        if (cache[i] != null) c.cbm_free_result(cache[i]);
    }
    c.free(@ptrCast(cache));
}

/// Run the parallel pipeline path: extract, registry, resolve, infra, k8s.
fn runParallelPasses(
    p: *Pipeline,
    ctx: *Ctx,
    files: [*c]const FileInfo,
    file_count: c_int,
    worker_count: c_int,
    t: *Timespec,
) c_int {
    interop.logInfo("pipeline.mode", .{
        "mode",    "parallel",
        "workers", itoa(worker_count),
        "files",   itoa(file_count),
    });
    var shared_ids: i64 = c.cbm_gbuf_next_id(p.gbuf);
    const cache: [*c][*c]FileResult =
        @ptrCast(@alignCast(c.calloc(@bitCast(@as(isize, file_count)), @sizeOf([*c]FileResult))));
    if (cache == null) {
        logError("pipeline.err", .{ "phase", "cache_alloc" });
        return c.CBM_NOT_FOUND;
    }
    now(t);
    var rc = c.cbm_parallel_extract(ctx, files, file_count, cache, &shared_ids, worker_count);
    interop.logInfo("pass.timing", .{ "pass", "parallel_extract", "elapsed_ms", elapsedText(t.*) });
    if (rc != 0 or cancelled(p)) {
        c.free(@ptrCast(cache));
        return if (rc != 0) rc else c.CBM_NOT_FOUND;
    }
    cbm_pipeline_sync_next_id(p.gbuf, &shared_ids);
    // extract -> registry handoff: return the extract phase's freed-but-retained
    // allocator pages to the OS before registry_build allocates. On a 2x Linux
    // index the extract peak holds ~13 GB of reclaimable pages (peak_mb 20.7 vs
    // live rss_mb 7); not returning them pushed the process over the system
    // memory-pressure threshold and got it SIGKILLed at registry entry.
    c.cbm_mem_collect();
    interop.logInfo("mem.collect", .{ "phase", "post_extract", "rss_mb", mbText(c.cbm_mem_rss()) });
    now(t);
    rc = c.cbm_build_registry_from_cache(ctx, files, file_count, cache);
    interop.logInfo("pass.timing", .{ "pass", "registry_build", "elapsed_ms", elapsedText(t.*) });
    cbm_pipeline_log_phase_mem("registry_build");
    // #49: registry_build is the first post-extract whole-graph allocation.
    if (rc == 0 and !cancelled(p)) rc = cbm_pipeline_budget_gate("before_lsp_cross_prepare");
    if (rc != 0 or cancelled(p)) {
        freeResultCache(cache, file_count);
        return if (rc != 0) rc else c.CBM_NOT_FOUND;
    }
    // Resolve workers must not reuse ids of nodes the registry build made.
    cbm_pipeline_share_next_id(p.gbuf, &shared_ids);
    // Cross-file LSP precondition: build a project-wide CBMLSPDef[]
    // once. The fused resolve_worker invokes cbm_pxc_run_one(_ts) per
    // file using these defs + the file's IMPORTS map, so cross-file
    // type-resolved CALLS land in result->resolved_calls before the
    // CALLS-edge emission. This replaces the old sequential
    // cbm_pipeline_pass_lsp_cross pass which re-read every source from
    // disk and re-parsed every tree on a single thread (~520s on
    // kubernetes). Soft-failure: NULL all_defs / NULL def_modules just
    // mean cross-file LSP no-ops; per-file LSP already ran during
    // extract.
    now(t);
    // Cross-file LSP (type-aware call/usage resolution across files) — the
    // most expensive phase. CBM_DISABLE_LSP_CROSS=1 opts out (it can SIGSEGV
    // on large TS projects — see #340/#344); with cross-LSP off, all_defs
    // stays NULL and the fused resolver simply no-ops cross-file resolution
    // (per-file LSP already ran during extract).
    var cbm_lsp_cross_env: [c.CBM_SZ_16]u8 = undefined;
    const run_cross_lsp = c.cbm_safe_getenv(
        "CBM_DISABLE_LSP_CROSS",
        &cbm_lsp_cross_env,
        cbm_lsp_cross_env.len,
        null,
    ) == null;
    if (!run_cross_lsp) {
        interop.logInfo("lsp_cross.skipped", .{ "reason", "CBM_DISABLE_LSP_CROSS env set" });
    }
    // Owns the per-language cross registries below and the def strings of
    // spilled results, so it outlives all_defs.
    var cross_lsp_arena: c.CBMArena = undefined;
    c.cbm_arena_init(&cross_lsp_arena);
    var def_modules: [*c]MutStr = null;
    var def_count: c_int = 0;
    var all_defs: [*c]c.CBMLSPDef = null;
    if (run_cross_lsp) {
        def_modules = @ptrCast(@alignCast(c.calloc(@intCast(file_count), @sizeOf(MutStr))));
        all_defs = if (def_modules != null)
            c.cbm_pxc_collect_all_defs(ctx, cache, files, file_count, ctx.project_name, def_modules, &cross_lsp_arena, &def_count)
        else
            null;
    }
    // Build inverted index: module_qn → defs. The fused resolve_worker
    // uses this to filter the global all_defs[] down to just the defs
    // each file actually needs (own_module + imported modules) — the
    // gopls "package summary" pattern. Drops per-file registry build
    // cost from O(all_defs) to O(relevant_defs), typically 50-100×
    // smaller per file.
    const module_def_index =
        if (all_defs != null) c.cbm_pxc_build_module_def_index(all_defs, def_count) else null;
    // Tier 2 full: pre-build per-language cross-LSP registries.
    // Built ONCE here; shared READ-ONLY across all files of that language
    // during resolve. Per-file work is then: parse + AST walk + O(1) lookups
    // — no registry build, no Phase 1b mutations. Languages added so far:
    // Go, Python. Others (C/C++, TS/JS, PHP, C#) fall back to per-file.
    var cross_registries = std.mem.zeroes(c.CBMCrossLspRegistries);
    if (all_defs != null) {
        cross_registries.go = c.cbm_go_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.python = c.cbm_py_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.c = c.cbm_c_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.cs = c.cbm_cs_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.ts = c.cbm_ts_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.zig_args = c.cbm_pxc_collect_zig_args(
            ctx,
            cache,
            files,
            file_count,
            def_modules,
            module_def_index,
            all_defs,
            def_count,
            &cross_lsp_arena,
            &cross_registries.zig_arg_count,
        );
        // Rust: NOT built here. The shared all_defs registry is built LAZILY on the
        // first NULL-filter rust file (the amplifier files) inside cbm_parallel_resolve
        // — repos whose rust files all filter to subsets never pay the build/RSS.
    }
    interop.logInfo("pass.timing", .{ "pass", "lsp_cross_prepare", "elapsed_ms", elapsedText(t.*) });
    cbm_pipeline_log_phase_mem("lsp_cross_prepare");
    // #49: the cross-file registries are a whole-graph working set allocated
    // after extract. Stop before resolve if they pushed RSS over the budget.
    if (cbm_pipeline_budget_gate("before_parallel_resolve") != 0) {
        rc = c.CBM_PIPELINE_ABORT_OVER_BUDGET;
    }
    if (rc == 0) {
        now(t);
        rc = c.cbm_parallel_resolve(
            ctx,
            files,
            file_count,
            cache,
            &shared_ids,
            worker_count,
            all_defs,
            def_count,
            def_modules,
            module_def_index,
            &cross_registries,
        );
        interop.logInfo("pass.timing", .{ "pass", "parallel_resolve", "elapsed_ms", elapsedText(t.*) });
        cbm_pipeline_log_phase_mem("parallel_resolve");
    }
    c.cbm_pxc_free_module_def_index(module_def_index);
    c.free(@ptrCast(@constCast(cross_registries.zig_args)));
    c.cbm_arena_destroy(&cross_lsp_arena); // releases all per-lang registries
    c.free(all_defs);
    if (def_modules != null) {
        for (0..ix(file_count)) |i| c.free(def_modules[i]);
        c.free(@ptrCast(def_modules));
    }
    cbm_pipeline_sync_next_id(p.gbuf, &shared_ids);
    // A budget abort means stop allocating: the infra passes would run over
    // the same cache only to return the abort below. Other resolve errors keep
    // main's behaviour and still run them.
    if (rc != c.CBM_PIPELINE_ABORT_OVER_BUDGET) {
        cbm_pipeline_extract_infra_routes(p.gbuf, files, cache, ctx.spill, file_count);
        cbm_pipeline_process_infra_bindings(p.gbuf, files, cache, ctx.spill, file_count);
    }
    freeResultCache(cache, file_count);
    if (rc != 0) return rc;
    now(t);
    _ = c.cbm_pipeline_pass_k8s(ctx, files, file_count);
    interop.logInfo("pass.timing", .{ "pass", "k8s", "elapsed_ms", elapsedText(t.*) });
    return if (cancelled(p)) c.CBM_NOT_FOUND else 0;
}

/// run_parallel_passes with a spill store (F1) the extract can park results
/// in when it runs over its memory budget. The store's file lives under the
/// cache dir and is deleted when the run ends.
fn runParallelPipeline(
    p: *Pipeline,
    ctx: *Ctx,
    files: [*c]const FileInfo,
    file_count: c_int,
    worker_count: c_int,
    t: *Timespec,
) c_int {
    const cache_dir: CStr = c.cbm_resolve_cache_dir();
    var spill_dir: [c.CBM_SZ_1K]u8 = undefined;
    if (cache_dir != null and
        c.snprintf(&spill_dir, spill_dir.len, "%s/spill", cache_dir) < @as(c_int, spill_dir.len))
    {
        ctx.spill = c.cbm_result_spill_open(&spill_dir, file_count);
    }
    var rc = runParallelPasses(p, ctx, files, file_count, worker_count, t);
    if (rc == 0 and c.cbm_result_spill_load_failures(ctx.spill) > 0) {
        // A pass ran without some parked results: never publish that graph.
        logError("mem.spill.failed", .{
            "load_failures", itoa(@truncate(c.cbm_result_spill_load_failures(ctx.spill))),
        });
        rc = c.CBM_NOT_FOUND;
    }
    if (ctx.spill != null) {
        var parked: i64 = 0;
        var bytes: i64 = 0;
        var loads: i64 = 0;
        c.cbm_result_spill_stats(ctx.spill, &parked, &bytes, &loads);
        if (parked > 0) {
            interop.logInfo("mem.spill.done", .{
                "parked", itoa(@truncate(parked)),
                "mb",     itoa(@truncate(@divTrunc(bytes, BYTES_PER_MB))),
                "loads",  itoa(@truncate(loads)),
            });
        }
        c.cbm_result_spill_close(ctx.spill);
        ctx.spill = null;
    }
    return rc;
}

const PIPELINE_INCREMENTAL_FATAL = -2;

/// True when the stored generation's Project node carries the current
/// index_format. Unstamped generations predate the stamp.
fn storedIndexFormatCurrent(stored: ?*const Gbuf, project: CStr) bool {
    const key = "\"index_format\":" ++ c.CBM_INDEX_FORMAT_VERSION_STR;
    const node: ?*const Node = c.cbm_gbuf_find_by_qn(stored, project);
    const n = node orelse return false;
    if (n.properties_json == null) return false;
    const hit: CStr = c.strstr(n.properties_json, key);
    return hit != null and !std.ascii.isDigit(hit[key.len]);
}

/// Try the incremental pipeline against the project's committed cbm.zova
/// workspace. Returns >= 0 if incremental was used (the return code), or -1 to
/// proceed with full. CBM_PIPELINE_ABORT_OVER_BUDGET passes through unchanged.
fn tryIncremental(p: *Pipeline, files: [*c]FileInfo, file_count: c_int) c_int {
    var zova_path: [c.CBM_SZ_1K]u8 = undefined;
    if (c.cbm_zova_user_database_path(&zova_path, zova_path.len) != 0) return c.CBM_NOT_FOUND;
    const repo = c.cbm_zova_repository_open(&zova_path, p.project_name);
    if (repo == null) return c.CBM_NOT_FOUND;

    var summary = std.mem.zeroes(c.cbm_project_summary_export_t);
    const workspace_id = c.cbm_zova_repository_workspace_id(repo);
    if (c.cbm_zova_repository_project_summary(repo, workspace_id, &summary) == c.CBM_STORE_OK) {
        c.cbm_store_project_summary_export_free(&p.saved_summary);
        p.saved_summary = summary;
        p.saved_summary_valid = true;
    }
    var snapshot = std.mem.zeroes(c.cbm_zova_workspace_snapshot_t);
    var preloaded = c.cbm_gbuf_new(p.project_name, p.repo_path);
    if (preloaded != null and c.cbm_zova_repository_export_incremental_snapshot_to_graph(
        &zova_path,
        workspace_id,
        &snapshot,
        preloaded,
    ) == 0) {
        const hash_count: c_int = snapshot.file_hash_count;
        if (hash_count > 0 and !storedIndexFormatCurrent(preloaded, p.project_name)) {
            // Reusing it would mix old-format QNs with new ones.
            interop.logInfo("pipeline.route", .{
                "path",            "format_change_reindex",
                "required_format", c.CBM_INDEX_FORMAT_VERSION_STR,
                "authority",       "zova",
            });
            p.format_migration = true;
        } else if (hash_count > 0 and file_count <= hash_count + @divTrunc(hash_count, 2)) {
            interop.logInfo("pipeline.route", .{
                "path",          "incremental",
                "stored_hashes", itoa(hash_count),
                "authority",     "zova",
            });
            // The exported snapshot owns all of its data. Release the
            // read transaction before publishing the next generation,
            // otherwise it pins the old WAL and prevents checkpointing.
            c.cbm_zova_repository_close(repo);
            p.last_route = c.CBM_PIPELINE_ROUTE_INCREMENTAL;
            const rc = c.cbm_pipeline_run_incremental_zova(@ptrCast(p), &zova_path, files, file_count, &snapshot, preloaded);
            preloaded = null;
            c.cbm_zova_workspace_snapshot_free(&snapshot);
            if (rc == c.CBM_PIPELINE_INCREMENTAL_DECLINED) {
                interop.logInfo("pipeline.route", .{
                    "path",      "incremental_declined_reindex",
                    "authority", "zova",
                });
                return c.CBM_NOT_FOUND;
            }
            if (rc == c.CBM_PIPELINE_ABORT_OVER_BUDGET) return rc;
            return if (rc == 0) 0 else PIPELINE_INCREMENTAL_FATAL;
        }
        if (hash_count > 0 and !p.format_migration) {
            interop.logInfo("pipeline.route", .{
                "path",          "mode_change_reindex",
                "stored_hashes", itoa(hash_count),
                "discovered",    itoa(file_count),
                "authority",     "zova",
            });
        }
        c.cbm_zova_workspace_snapshot_free(&snapshot);
    }
    c.cbm_gbuf_free(preloaded);
    c.cbm_zova_repository_close(repo);
    return c.CBM_NOT_FOUND;
}

/// Publication result fields copied to the pipeline's publish stats under the
/// same name.
const publish_result_fields = .{
    "full_clear_count",               "unchanged_rewrite_count",     "nodes_inserted",
    "nodes_updated",                  "nodes_deleted",               "edges_inserted",
    "edges_deleted",                  "node_vectors_upserted",       "node_vectors_deleted",
    "token_vectors_upserted",         "token_vectors_deleted",       "normalization_ms",
    "model_nodes_ms",                 "model_edges_ms",              "model_edge_endpoint_ms",
    "model_edge_sort_ms",             "model_edge_group_ms",         "model_edge_payload_ms",
    "model_edge_digest_ms",           "model_edge_default_payloads", "model_edge_payload_scratch_edges",
    "model_hashes_ms",                "model_vectors_ms",            "model_digests_ms",
    "writer_guard_ms",                "database_init_ms",            "database_open_ms",
    "transaction_begin_ms",           "transaction_body_ms",         "transaction_commit_ms",
    "database_close_ms",              "clear_ms",                    "finalize_ms",
    "canonical_files_ms",             "canonical_nodes_ms",          "canonical_edges_ms",
    "canonical_hashes_ms",            "fts_ms",                      "token_metadata_ms",
    "native_graph_ms",                "native_graph_materialize_ms", "native_graph_reset_ms",
    "native_graph_nodes_ms",          "native_graph_edges_ms",       "native_graph_validate_ms",
    "native_graph_key_generation_ms", "native_graph_cleanup_ms",     "native_vectors_ms",
    "fresh_validation_ms",            "fresh_index_ms",              "fresh_commit_ms",
    "fresh_build_ms",                 "readback_ms",
};

/// Snapshot metrics copied to the publish stats as `snapshot_<name>`.
const snapshot_metric_fields = .{
    "base_ms",           "optional_ms",     "open_ms",           "header_ms",
    "integrity_ms",      "nodes_sql_ms",    "nodes_native_ms",   "nodes_finalize_ms",
    "edges_sql_ms",      "edges_native_ms", "edges_finalize_ms", "hashes_summary_ms",
    "close_ms",          "graph_buffer_ms", "base_phase_mask",   "node_rows",
    "edge_rows",         "file_hash_rows",  "topology_rows",     "node_vector_rows",
    "token_vector_rows",
};

fn pipelinePublishZovaUserDatabase(
    p_c: ?*Pipeline,
    gbuf: ?*Gbuf,
    files: [*c]const FileInfo,
    file_count: c_int,
    mode_skipped: [*c]const c.cbm_file_hash_t,
    mode_skipped_count: c_int,
    before: ?*c.cbm_zova_workspace_snapshot_t,
) c_int {
    const p = p_c orelse return c.CBM_NOT_FOUND;
    if (gbuf == null or file_count < 0 or mode_skipped_count < 0 or
        (file_count > 0 and files == null) or (mode_skipped_count > 0 and mode_skipped == null))
    {
        return c.CBM_NOT_FOUND;
    }
    p.zova_publish_ms = 0.0;
    p.zova_publish_stats = std.mem.zeroes(c.cbm_pipeline_zova_publish_stats_t);
    var route_started = std.mem.zeroes(Timespec);
    now(&route_started);
    const capacity = file_count + mode_skipped_count;
    const hashes: [*c]c.cbm_zova_file_hash_input_t =
        if (capacity > 0) interop.callocN(c.cbm_zova_file_hash_input_t, ix(capacity)) else null;
    if (capacity > 0 and hashes == null) return c.CBM_NOT_FOUND;
    var count: usize = 0;
    for (0..ix(file_count)) |i| {
        var fst: c.struct_stat = undefined;
        if (c.stat(files[i].path, &fst) != 0) continue;
        hashes[count] = std.mem.zeroes(c.cbm_zova_file_hash_input_t);
        hashes[count].file_path = files[i].rel_path;
        hashes[count].content_hash = "";
        hashes[count].mtime_ns = c.cbm_stat_mtime_ns(&fst);
        hashes[count].size_bytes = fst.st_size;
        count += 1;
    }
    for (0..ix(mode_skipped_count)) |i| {
        hashes[count] = std.mem.zeroes(c.cbm_zova_file_hash_input_t);
        hashes[count].file_path = mode_skipped[i].rel_path;
        hashes[count].content_hash = orEmpty(mode_skipped[i].sha256);
        hashes[count].mtime_ns = mode_skipped[i].mtime_ns;
        hashes[count].size_bytes = mode_skipped[i].size;
        count += 1;
    }
    var summary = std.mem.zeroes(c.cbm_zova_project_summary_input_t);
    if (p.saved_summary_valid) {
        summary.present = true;
        summary.summary = p.saved_summary.summary;
        summary.source_hash = p.saved_summary.source_hash;
        summary.created_at = p.saved_summary.created_at;
        summary.updated_at = p.saved_summary.updated_at;
    }
    var result = std.mem.zeroes(c.cbm_zova_workspace_generation_result_t);
    const rc = if (before != null)
        c.cbm_gbuf_publish_zova_user_database_delta(gbuf, before, hashes, @intCast(count), &summary, &result)
    else
        c.cbm_gbuf_publish_zova_user_database(gbuf, hashes, @intCast(count), &summary, &result);
    c.free(hashes);
    if (rc != 0) {
        logError("zova.single_file_publish", .{
            "project", @as(CStr, p.project_name),
            "reason",  "publication_failed",
        });
        p.failure_reason = "persist_failed";
        return rc;
    }
    p.zova_publish_ms = result.publish_ms;
    const is_delta = result.publication_mode == c.CBM_ZOVA_PUBLICATION_MODE_DELTA;
    var stats = std.mem.zeroes(c.cbm_pipeline_zova_publish_stats_t);
    stats.completed = true;
    stats.delta = is_delta;
    inline for (publish_result_fields) |name| @field(stats, name) = @field(result, name);
    stats.route_ms = c.cbm_elapsed_ms(route_started);
    stats.snapshot_completed = before != null;
    if (before) |b| {
        inline for (snapshot_metric_fields) |name| {
            @field(stats, "snapshot_" ++ name) = @field(b.metrics, name);
        }
        stats.snapshot_hydrated_components = b.hydrated_components;
        stats.snapshot_generation = b.generation;
    }
    p.zova_publish_stats = stats;
    var generation_text: [32]u8 = undefined;
    var publish_ms_text: [32]u8 = undefined;
    var inserted_text: [32]u8 = undefined;
    var updated_text: [32]u8 = undefined;
    var deleted_text: [32]u8 = undefined;
    _ = c.snprintf(&generation_text, generation_text.len, "%lld", @as(c_longlong, result.generation));
    _ = c.snprintf(&publish_ms_text, publish_ms_text.len, "%.3f", result.publish_ms);
    _ = c.snprintf(&inserted_text, inserted_text.len, "%llu", @as(c_ulonglong, result.inserted_count));
    _ = c.snprintf(&updated_text, updated_text.len, "%llu", @as(c_ulonglong, result.updated_count));
    _ = c.snprintf(&deleted_text, deleted_text.len, "%llu", @as(c_ulonglong, result.deleted_count));
    interop.logInfo("zova.single_file_publish", .{
        "project",          @as(CStr, p.project_name),
        "generation",       @as(CStr, &generation_text),
        "publish_ms",       @as(CStr, &publish_ms_text),
        "publication_mode", @as(CStr, if (is_delta) "delta" else "full"),
        "inserted",         @as(CStr, &inserted_text),
        "updated",          @as(CStr, &updated_text),
        "deleted",          @as(CStr, &deleted_text),
    });
    return 0;
}

pub export fn cbm_pipeline_publish_zova_user_database(
    p: ?*Pipeline,
    gbuf: ?*Gbuf,
    files: [*c]const FileInfo,
    file_count: c_int,
    mode_skipped: [*c]const c.cbm_file_hash_t,
    mode_skipped_count: c_int,
) c_int {
    return pipelinePublishZovaUserDatabase(p, gbuf, files, file_count, mode_skipped, mode_skipped_count, null);
}

pub export fn cbm_pipeline_publish_zova_user_database_delta(
    p: ?*Pipeline,
    gbuf: ?*Gbuf,
    files: [*c]const FileInfo,
    file_count: c_int,
    mode_skipped: [*c]const c.cbm_file_hash_t,
    mode_skipped_count: c_int,
    before: ?*c.cbm_zova_workspace_snapshot_t,
) c_int {
    if (before == null) return c.CBM_NOT_FOUND;
    return pipelinePublishZovaUserDatabase(p, gbuf, files, file_count, mode_skipped, mode_skipped_count, before);
}

/// Publish the graph and file hashes to the shared cbm.zova.
fn dumpAndPersistHashes(p: *Pipeline, files: [*c]const FileInfo, file_count: c_int, t: *Timespec) c_int {
    now(t);
    // #49: dump + publish materialize the whole graph again (normalization,
    // native topology, FTS, vectors). Check before starting; nothing has been
    // published yet, so an abort leaves the previous generation serving.
    cbm_pipeline_log_phase_mem("pre_dump");
    var gate_rc = cbm_pipeline_budget_gate("before_dump");
    if (gate_rc != 0) return gate_rc;
    var zova_dir: [c.CBM_SZ_1K]u8 = undefined;
    if (c.cbm_zova_user_database_path(&zova_dir, zova_dir.len) == 0) {
        const last_slash: MutStr = c.strrchr(&zova_dir, '/');
        if (last_slash != null) {
            last_slash[0] = 0;
            _ = c.cbm_mkdir_p(&zova_dir, CBM_DIR_PERMS);
        }
    }
    // Capture committed counts before preparing the dump, which releases the
    // graph buffer indexes (graph_buffer.c), after which cbm_gbuf_node_count()
    // returns 0 and the #334 plausibility gate would never fire.
    cbm_pipeline_run_before_dump_hook(p.gbuf);
    p.committed_nodes = c.cbm_gbuf_node_count(p.gbuf);
    p.committed_edges = c.cbm_gbuf_edge_count(p.gbuf);
    const prepare_rc = c.cbm_gbuf_prepare_zova_dump(p.gbuf);
    if (prepare_rc != 0) {
        logError("pipeline.err", .{ "phase", "zova_prepare" });
        return prepare_rc;
    }
    cbm_pipeline_log_phase_mem("zova_prepare");
    // Preparing the dump releases the gbuf indexes; if RSS is still over
    // budget now, publishing would only raise it further. Abort before the
    // atomic publication starts.
    gate_rc = cbm_pipeline_budget_gate("before_publish");
    if (gate_rc != 0) return gate_rc;
    const publish_rc = cbm_pipeline_publish_zova_user_database(p, p.gbuf, files, file_count, null, 0);
    if (publish_rc != 0) {
        logError("pipeline.err", .{ "phase", "zova_single_file" });
        return publish_rc;
    }
    cbm_pipeline_log_phase_mem("zova_publish");
    return 0;
}

/// Run githistory pass.
fn runGithistory(p: *Pipeline, ctx: *Ctx) c_int {
    var t_gh: Timespec = undefined;
    now(&t_gh);

    var gh_result = std.mem.zeroes(c.cbm_githistory_result_t);
    var gh_thread: c.cbm_thread_t = undefined;
    var gh_threaded = false;
    var gh_arg: GhComputeArg = .{ .repo_path = ctx.repo_path, .result = &gh_result };

    if (p.mode != c.CBM_MODE_FAST) {
        if (c.cbm_default_worker_count(true) > 1) {
            if (c.cbm_thread_create(&gh_thread, 0, ghComputeThreadFn, &gh_arg) == 0) {
                gh_threaded = true;
            }
        }
        if (!gh_threaded) {
            _ = c.cbm_pipeline_githistory_compute(ctx.repo_path, &gh_result);
            interop.logInfo("pass.timing", .{
                "pass",       "githistory_compute",
                "elapsed_ms", elapsedText(t_gh),
            });
        }
    } else {
        interop.logInfo("pass.skip", .{ "pass", "githistory", "reason", "fast_mode" });
    }

    if (gh_threaded) {
        _ = c.cbm_thread_join(&gh_thread);
        interop.logInfo("pass.timing", .{
            "pass",       "githistory_compute",
            "elapsed_ms", elapsedText(t_gh),
        });
    }

    var gh_edges: c_int = 0;
    if (gh_result.count > 0 or gh_result.file_temporal_count > 0) {
        gh_edges = c.cbm_pipeline_githistory_apply(ctx, &gh_result);
    }
    interop.logInfo("pass.done", .{
        "pass",    "githistory",
        "commits", itoa(gh_result.commit_count),
        "edges",   itoa(gh_edges),
    });
    c.free(gh_result.couplings);
    c.free(gh_result.file_temporal);
    return 0;
}

// ── Pipeline run ────────────────────────────────────────────────────

/// Run tests + git history. Returns 0 on success.
fn runTestsAndHistory(p: *Pipeline, ctx: *Ctx, files: [*c]const FileInfo, file_count: c_int) c_int {
    var t: Timespec = undefined;
    now(&t);
    const t_tests = util.profStart();
    var rc = c.cbm_pipeline_pass_tests(ctx, files, file_count);
    util.profEnd("pipeline", "pass_tests", &t_tests, file_count);
    interop.logInfo("pass.timing", .{ "pass", "tests", "elapsed_ms", elapsedText(t) });
    if (rc == 0 and !cancelled(p)) {
        const t_gh = util.profStart();
        rc = runGithistory(p, ctx);
        util.profEnd("pipeline", "pass_githistory", &t_gh, 0);
    }
    if (cancelled(p)) return c.CBM_NOT_FOUND;
    return rc;
}

/// Run tests, git history, predump passes, and dump+persist.
fn runPostExtraction(p: *Pipeline, ctx: *Ctx, files: [*c]const FileInfo, file_count: c_int) c_int {
    // #49: extract already enforces the budget. Everything from here on works
    // on the whole graph, so gate the entry too.
    cbm_pipeline_log_phase_mem("extract_done");
    var rc = cbm_pipeline_budget_gate("before_tests_and_history");
    if (rc != 0) return rc;
    rc = runTestsAndHistory(p, ctx, files, file_count);
    if (rc != 0) return rc;
    cbm_pipeline_log_phase_mem("tests_and_history");

    const t_predump = util.profStart();
    rc = runPredumpPasses(p, ctx);
    util.profEnd("pipeline", "3_predump_passes_total", &t_predump, 0);
    if (rc != 0) return rc;

    if (!cancelled(p)) {
        var t: Timespec = undefined;
        const t_dump = util.profStart();
        rc = dumpAndPersistHashes(p, files, file_count, &t);
        util.profEnd("pipeline", "4_dump_and_persist", &t_dump, 0);
    }
    return rc;
}

/// Run structure + extraction passes.
fn runStructureAndExtraction(p: *Pipeline, ctx: *Ctx, files: [*c]const FileInfo, file_count: c_int) c_int {
    var t: Timespec = undefined;
    now(&t);
    const t_struct = util.profStart();
    _ = passStructure(p, ctx.stem_marks, files, file_count);
    util.profEnd("pipeline", "pass_structure", &t_struct, file_count);
    interop.logInfo("pass.timing", .{ "pass", "structure", "elapsed_ms", elapsedText(t) });
    if (cancelled(p)) return c.CBM_NOT_FOUND;

    const worker_count = c.cbm_default_worker_count(true);
    const t_extract_total = util.profStart();
    const rc = runParallelPipeline(p, ctx, files, file_count, worker_count, &t);
    util.profEnd("pipeline", "2_extraction_total", &t_extract_total, file_count);
    if (cancelled(p)) return c.CBM_NOT_FOUND;
    if (rc == 0) cbm_pipeline_stamp_coverage(p, p.gbuf);
    return rc;
}

/// The stem collisions decide Folder QNs (structure) and Module QNs
/// (extraction); both phases read the one set.
fn runExtractionPhase(p: *Pipeline, ctx: *Ctx, files: [*c]const FileInfo, file_count: c_int) c_int {
    var marks: StemMarks = undefined;
    cbm_stem_marks_init(&marks, p.project_name, files, file_count);
    ctx.stem_marks = &marks;
    const rc = runStructureAndExtraction(p, ctx, files, file_count);
    ctx.stem_marks = null;
    cbm_stem_marks_free(&marks);
    return rc;
}

/// The full-index part of cbm_pipeline_run, from graph buffer creation to the
/// publication. The caller releases everything it sets up on `p` and `ctx`.
fn runFull(
    p: *Pipeline,
    ctx: *Ctx,
    path_aliases: *?*c.cbm_path_alias_collection_t,
    files: [*c]const FileInfo,
    file_count: c_int,
    t0: Timespec,
    t_pipeline_total: *const Timespec,
) c_int {
    // Phase 2: Create graph buffer and registry
    p.gbuf = c.cbm_gbuf_new(p.project_name, p.repo_path);
    p.registry = c.cbm_registry_new();

    // Phase 2b: Load build-tool path aliases (tsconfig/jsconfig today). NULL
    // when no usable configs are found — non-TS projects pay nothing.
    path_aliases.* = c.cbm_load_path_aliases_excluded(p.repo_path, p.excluded_dirs, p.excluded_count);

    // Build shared context for pass functions
    ctx.* = std.mem.zeroes(Ctx);
    ctx.project_name = p.project_name;
    ctx.repo_path = p.repo_path;
    ctx.gbuf = p.gbuf;
    ctx.registry = p.registry;
    ctx.cancelled = &p.cancelled;
    ctx.pipeline = @ptrCast(p); // so passes can record per-file skips (Track B)
    ctx.mode = @intCast(p.mode);
    ctx.path_aliases = path_aliases.*;
    ctx.excluded_dirs = p.excluded_dirs;
    ctx.excluded_count = p.excluded_count;

    var rc = runExtractionPhase(p, ctx, files, file_count);
    if (rc != 0) return rc;

    rc = runPostExtraction(p, ctx, files, file_count);
    if (rc != 0) return rc;

    cbm_pipeline_log_phase_mem("pipeline_done");
    interop.logInfo("pipeline.done", .{
        "nodes",      itoa(c.cbm_gbuf_node_count(p.gbuf)),
        "edges",      itoa(c.cbm_gbuf_edge_count(p.gbuf)),
        "elapsed_ms", elapsedText(t0),
    });
    util.profEnd("pipeline", "TOTAL", t_pipeline_total, 0);
    return 0;
}

pub export fn cbm_pipeline_run(p_c: ?*Pipeline) c_int {
    const p = p_c orelse return c.CBM_NOT_FOUND;
    p.last_route = c.CBM_PIPELINE_ROUTE_UNKNOWN;
    p.format_migration = false;
    p.failure_reason = null;
    p.discovered_files = 0;
    p.discovered_source_bytes = 0;

    const t_pipeline_total = util.profStart();
    var t0: Timespec = undefined;
    now(&t0);
    var path_aliases: ?*c.cbm_path_alias_collection_t = null;
    var ctx = std.mem.zeroes(Ctx); // zeroed first: cleanup reads ctx.pkgmap

    // C/C++ #define Macro nodes (#375) dominate extraction on macro-dense repos
    // (≈49% of nodes on the Linux kernel), so gate them to full mode — moderate
    // and fast skip them entirely. Set before any extraction dispatch.
    c.cbm_set_macro_extraction(@intFromBool(p.mode == c.CBM_MODE_FULL));

    // Load user-defined extension overrides (fail-open: NULL on error)
    const t_userconfig = util.profStart();
    p.userconfig = c.cbm_userconfig_load(p.repo_path);
    c.cbm_set_user_lang_config(p.userconfig);
    util.profEnd("pipeline", "0_userconfig_load", &t_userconfig, 0);

    // Phase 1: Discover files
    const t_discover = util.profStart();
    var opts = std.mem.zeroes(c.cbm_discover_opts_t);
    opts.mode = p.mode;
    opts.ignore_file = null;
    opts.max_file_size = 0;
    var files: [*c]FileInfo = null;
    var file_count: c_int = 0;
    // Capture skipped subtrees on the pipeline so the MCP layer can report
    // which directories were excluded (#411). Replace any prior list (e.g. a
    // re-run on the same pipeline) to avoid leaking the previous one.
    c.cbm_discover_free_excluded(p.excluded_dirs, p.excluded_count);
    p.excluded_dirs = null;
    p.excluded_count = 0;
    var discover_degraded = false;
    var rc = c.cbm_discover_ex(p.repo_path, &opts, &files, &file_count, &p.excluded_dirs, &p.excluded_count, &discover_degraded);
    p.discover_degraded = discover_degraded;
    if (discover_degraded) interop.logWarn("pipeline.discover", .{ "degraded", "true" });
    if (rc != 0) logError("pipeline.err", .{ "phase", "discover", "rc", itoa(rc) });
    util.profEnd("pipeline", "1_discover", &t_discover, file_count);
    interop.logInfo("pipeline.discover", .{
        "files",      itoa(file_count),
        "elapsed_ms", elapsedText(t0),
    });
    if (rc != 0 or cancelled(p)) {
        if (rc != 0) p.failure_reason = "discover_failed";
        rc = c.CBM_NOT_FOUND;
    } else {
        p.discovered_files = file_count;
        var total_bytes: i64 = 0;
        if (files != null and file_count > 0) {
            for (files[0..@intCast(file_count)]) |f| {
                if (f.size > 0) total_bytes += f.size;
            }
        }
        p.discovered_source_bytes = total_bytes;

        if (p.max_files > 0 and file_count > p.max_files) {
            interop.logWarn("pipeline.abort", .{
                "reason", "max_files_exceeded",
                "files",  itoa(file_count),
                "limit",  itoa(p.max_files),
            });
            c.cbm_discover_free(files, file_count);
            c.cbm_set_user_lang_config(null);
            c.cbm_userconfig_free(p.userconfig);
            p.userconfig = null;
            return c.CBM_PIPELINE_ABORT_MAX_FILES;
        }
        if (p.max_source_mb > 0) {
            const limit_bytes: i64 = @as(i64, p.max_source_mb) * BYTES_PER_MB;
            if (total_bytes > limit_bytes) {
                const mb_int: c_int = @intCast(@min(@divTrunc(total_bytes, BYTES_PER_MB), std.math.maxInt(c_int)));
                interop.logWarn("pipeline.abort", .{
                    "reason",    "max_source_mb_exceeded",
                    "source_mb", itoa(mb_int),
                    "limit_mb",  itoa(p.max_source_mb),
                });
                c.cbm_discover_free(files, file_count);
                c.cbm_set_user_lang_config(null);
                c.cbm_userconfig_free(p.userconfig);
                p.userconfig = null;
                return c.CBM_PIPELINE_ABORT_MAX_SOURCE_MB;
            }
        }

        // Check for existing DB → try incremental or delete for reindex
        rc = tryIncremental(p, files, file_count);
        if (rc == PIPELINE_INCREMENTAL_FATAL) {
            c.cbm_discover_free(files, file_count);
            return c.CBM_NOT_FOUND;
        }
        if (rc == c.CBM_PIPELINE_ABORT_OVER_BUDGET) {
            // A full run would hit the same budget; the served generation stays.
            c.cbm_discover_free(files, file_count);
            return rc;
        }
        if (rc >= 0) {
            c.cbm_discover_free(files, file_count);
            return rc;
        }
        interop.logInfo("pipeline.route", .{ "path", "full" });
        p.last_route = c.CBM_PIPELINE_ROUTE_FULL;

        rc = runFull(p, &ctx, &path_aliases, files, file_count, t0, &t_pipeline_total);
    }

    // cleanup:
    c.cbm_pkgmap_free(ctx.pkgmap);
    ctx.pkgmap = null;
    cbm_pipeline_set_pkgmap(null);
    c.cbm_discover_free(files, file_count);
    c.cbm_gbuf_free(p.gbuf);
    p.gbuf = null;
    c.cbm_registry_free(p.registry);
    p.registry = null;
    c.cbm_path_alias_collection_free(path_aliases);
    // Clear and free user extension config
    c.cbm_set_user_lang_config(null);
    c.cbm_userconfig_free(p.userconfig);
    p.userconfig = null;
    return rc;
}
