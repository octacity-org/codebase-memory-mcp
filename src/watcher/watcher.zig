//! watcher.zig — Git-based file change watcher.
//!
//! Implements src/watcher/watcher.h.
//!
//! Strategy: git status + HEAD tracking (the most reliable approach).
//! For non-git projects, the watcher skips polling (no fsnotify/dirmtime yet).
//!
//! Per-project state tracks:
//!   - Last git HEAD hash (detects commits, checkout, pull)
//!   - Last poll time + adaptive interval
//!   - Whether the project is a git repo
//!
//! Adaptive interval: 5s base + 1s per 500 files, capped at 60s.
//! Matches the Go watcher's `pollInterval()` logic.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const is_windows = builtin.os.tag == .windows;

const Str = [*:0]const u8;
const logInfo = interop.logInfo;
const logWarn = interop.logWarn;

// ── Constants ──────────────────────────────────────────────────────

const NS_PER_SEC: i64 = 1000000000;
const NS_PER_MS: i64 = 1000000;
const MSEC_PER_SEC = 1000;

/// Adaptive poll interval parameters (ms).
const POLL_BASE_MS = 5000;
const POLL_FILE_STEP = 500; // add 1s per this many files
const POLL_MAX_MS = 60000;

/// Stale-root pruning (#286): a watched project whose root directory stays
/// missing is pruned — its cached DB is deleted and the watch entry removed.
/// Deletion is destructive (the DB can hold user-authored data such as the
/// ADR), so it requires BOTH a streak of consecutive missing polls AND a
/// sustained-absence grace window measured from the streak's first miss.
const MISSING_ROOT_DELETE_AFTER = 3;
const PRUNE_GRACE_DEFAULT_S = 600; // 10 min; override: CBM_WATCHER_PRUNE_GRACE_S

/// Sleep chunk for responsive shutdown (ms).
const SLEEP_CHUNK_MS = 500;

const HEAD_MAX = 64;
const CMD_MAX = 1024;
const PATH_MAX = 4096;

const SIG_FNV_OFFSET: u64 = 1469598103934665603;
const SIG_FNV_PRIME: u64 = 1099511628211;

/// Portable command pieces: cbm_popen runs through cmd.exe on Windows, which
/// does NOT strip single quotes (git would receive a literal-quoted path →
/// "cannot find the path") and has no /dev/null. Use double quotes (stripped
/// by both cmd.exe and POSIX sh) and the platform null device.
const NULL_DEV = if (is_windows) "NUL" else "/dev/null";

// ── Per-project state ──────────────────────────────────────────────

const ProjectState = struct {
    project_name: ?[*:0]u8,
    root_path: ?[*:0]u8,
    /// git HEAD hash (last SUCCESSFULLY indexed)
    last_head: [HEAD_MAX]u8,
    /// false → skip polling
    is_git: bool,
    /// true after first poll
    baseline_done: bool,
    /// Detected-but-not-yet-indexed HEAD movement (#17, item 3): the observed
    /// head is committed to last_head only after a successful refresh, so a
    /// failed index keeps retry eligibility and a newer change arriving during
    /// indexing stays pending instead of being swallowed.
    change_pending: bool,
    pending_head: [HEAD_MAX]u8,
    /// Dirty-state signature (upstream #937): a persistently dirty worktree
    /// reindexes once per distinct dirty state, not on every poll. Committed
    /// to last_dirty_sig only after a successful refresh. 0 = clean tree.
    last_dirty_sig: u64,
    pending_dirty_sig: u64,
    /// consecutive polls where root was missing (ENOENT/ENOTDIR)
    missing_root_count: c_int,
    /// cbm_now_ms() of the streak's first miss (0 = no streak)
    first_missing_ms: u64,
    /// approximate, for interval calc
    file_count: c_int,
    /// adaptive poll interval
    interval_ms: c_int,
    /// next poll time (monotonic ns)
    next_poll_ns: i64,
    /// Published fields for cbm_watcher_project_info (guarded by projects_lock):
    published_strategy: c.cbm_watcher_strategy_t,
    published_interval_ms: c_int,
    last_scan_unix_s: i64,

    fn name(s: *const ProjectState) Str {
        return s.project_name.?;
    }

    fn root(s: *const ProjectState) Str {
        return s.root_path.?;
    }
};

// ── Watcher struct ─────────────────────────────────────────────────

const Watcher = struct {
    store: ?*c.cbm_store_t,
    index_fn: c.cbm_index_fn,
    user_data: ?*anyopaque,
    /// name → *ProjectState
    projects: ?*c.CBMHashTable,
    projects_lock: c.cbm_mutex_t,
    stopped: std.atomic.Value(c_int),
    /// Deferred-free list: freed after the next poll_once.
    pending_free: ?[*]*ProjectState,
    pending_free_count: c_int,
    pending_free_cap: c_int,

    fn lock(w: *Watcher) void {
        c.cbm_mutex_lock(&w.projects_lock);
    }

    fn unlock(w: *Watcher) void {
        c.cbm_mutex_unlock(&w.projects_lock);
    }

    fn getState(w: *Watcher, project_name: Str) ?*ProjectState {
        return @ptrCast(@alignCast(c.cbm_ht_get(w.projects, project_name)));
    }

    fn isStopped(w: *Watcher) bool {
        return w.stopped.load(.seq_cst) != 0;
    }
};

// ── Helpers ────────────────────────────────────────────────────────

fn dupZ(s: Str) ?[*:0]u8 {
    const len = std.mem.len(s);
    const raw: [*]u8 = @ptrCast(c.malloc(len + 1) orelse return null);
    @memcpy(raw[0 .. len + 1], s[0 .. len + 1]);
    return @ptrCast(raw);
}

/// snprintf(dst, sizeof(dst), "%s", src): truncating copy, always NUL-terminated.
fn copyZ(dst: []u8, src: []const u8) void {
    const n = @min(src.len, dst.len - 1);
    @memcpy(dst[0..n], src[0..n]);
    dst[n] = 0;
}

fn trimLineEnd(buf: []u8) []u8 {
    var len = std.mem.indexOfScalar(u8, buf, 0) orelse buf.len;
    while (len > 0 and (buf[len - 1] == '\n' or buf[len - 1] == '\r')) len -= 1;
    if (len < buf.len) buf[len] = 0;
    return buf[0..len];
}

fn popenGit(cmd: *[CMD_MAX]u8, comptime fmt: []const u8, args: anytype) ?*c.FILE {
    const cmd_z = std.mem.printSentinel(cmd, fmt, args, 0) catch return null;
    return c.cbm_popen_git(cmd_z.ptr);
}

// ── Adaptive interval ──────────────────────────────────────────────

pub export fn cbm_watcher_poll_interval_ms(file_count: c_int) c_int {
    const ms = POLL_BASE_MS +% @divTrunc(file_count, POLL_FILE_STEP) *% MSEC_PER_SEC;
    return @min(ms, POLL_MAX_MS);
}

// ── Git helpers ────────────────────────────────────────────────────

fn isGitRepo(root_path: Str) bool {
    var cmd: [CMD_MAX]u8 = undefined;
    const fp = popenGit(&cmd, "git -C \"{s}\" rev-parse --git-dir 2>{s}", .{
        std.mem.span(root_path), NULL_DEV,
    }) orelse return false;
    // Drain output so pclose gets a clean exit status.
    var drain: [128]u8 = undefined;
    while (c.fgets(&drain, drain.len, fp) != null) {}
    return c.cbm_pclose(fp) == 0;
}

fn gitHead(root_path: Str, out: []u8) c_int {
    var cmd: [CMD_MAX]u8 = undefined;
    const fp = popenGit(&cmd, "git -C \"{s}\" rev-parse HEAD 2>{s}", .{
        std.mem.span(root_path), NULL_DEV,
    }) orelse return c.CBM_NOT_FOUND;
    defer _ = c.cbm_pclose(fp);
    if (c.fgets(out.ptr, @intCast(out.len), fp) == null) return c.CBM_NOT_FOUND;
    _ = trimLineEnd(out);
    return 0;
}

// ── Dirty-state signature (upstream #937) ──────────────────────────

fn sigFold(h0: u64, data: []const u8) u64 {
    var h = h0;
    for (data) |b| {
        h ^= b;
        h *%= SIG_FNV_PRIME;
    }
    return h;
}

/// Platform-portable mtime_ns (mirrors pipeline_incremental.c).
fn statMtimeNs(st: *const c.struct_stat) i64 {
    if (builtin.os.tag.isDarwin()) {
        return @as(i64, st.st_mtimespec.tv_sec) * NS_PER_SEC + @as(i64, st.st_mtimespec.tv_nsec);
    } else if (is_windows) {
        return @as(i64, st.st_mtime) * NS_PER_SEC;
    } else {
        return @as(i64, st.st_mtim.tv_sec) * NS_PER_SEC + @as(i64, st.st_mtim.tv_nsec);
    }
}

/// Fold a listed path's (size, mtime) into the signature so an in-place edit
/// of an already-dirty file still produces a new signature. A failed stat
/// (deleted file, quoting artifact) degrades to the entry text alone — the
/// deletion itself is represented by the porcelain status.
fn sigFoldPathStat(h: u64, root_path: Str, rel: []const u8) u64 {
    var abs_buf: [PATH_MAX]u8 = undefined;
    // snprintf truncation semantics: an over-long path is stat'ed truncated.
    var w: std.Io.Writer = .fixed(abs_buf[0 .. abs_buf.len - 1]);
    w.print("{s}/{s}", .{ std.mem.span(root_path), rel }) catch {};
    abs_buf[w.end] = 0;

    var st: c.struct_stat = undefined;
    if (c.stat(&abs_buf, &st) != 0) return h;
    const mt: i64 = statMtimeNs(&st);
    const sz: i64 = @intCast(st.st_size);
    return sigFold(sigFold(h, std.mem.asBytes(&mt)), std.mem.asBytes(&sz));
}

/// Signature of the current dirty state: FNV-1a over the entries of
/// `git status --porcelain -uall -z` plus each listed path's (size, mtime).
/// Returns 0 for a clean tree. Two polls over an untouched dirty tree yield
/// the same value; editing a dirty file, adding/removing one, or reverting
/// the tree to clean each yield a new one. -uall lists files inside
/// untracked directories individually (a nested addition under `?? dir/`
/// would otherwise be invisible); -z gives unquoted NUL-separated paths that
/// hash identically across polls and stat cleanly.
fn gitDirtySignature(root_path: Str) u64 {
    var cmd: [CMD_MAX]u8 = undefined;
    const fp = popenGit(&cmd, "git --no-optional-locks -C \"{s}\" status --porcelain -uall -z 2>{s}", .{
        std.mem.span(root_path), NULL_DEV,
    }) orelse return 0;

    var h: u64 = SIG_FNV_OFFSET;
    var any = false;
    var entry: [PATH_MAX]u8 = undefined;
    var elen: usize = 0;
    var overflow = false;
    // Rename/copy entries are followed by a second NUL token (the origin
    // path) that carries no XY prefix — fold it as text, don't stat it.
    var origin_token = false;
    while (true) {
        const ch = c.fgetc(fp);
        if (ch != c.EOF and ch != 0) {
            if (elen + 1 < entry.len) {
                entry[elen] = @intCast(ch);
            } else {
                overflow = true; // keep hashing prefix; skip stat for entry
            }
            elen += 1;
            continue;
        }
        const stored = @min(elen, entry.len - 1);
        if (stored > 0) {
            const text = entry[0..stored];
            any = true;
            h = sigFold(h, text);
            h = sigFold(h, &.{0}); // token separator
            if (origin_token) {
                origin_token = false;
            } else if (!overflow and stored > 3 and text[2] == ' ') {
                if (text[0] == 'R' or text[0] == 'C') origin_token = true;
                h = sigFoldPathStat(h, root_path, text[3..]);
            }
        }
        elen = 0;
        overflow = false;
        if (ch == c.EOF) break;
    }
    _ = c.cbm_pclose(fp);

    if (!is_windows) {
        // Submodules (POSIX-only, as the parent status cannot see them): fold
        // each submodule's porcelain output with paths re-rooted at the
        // superproject so they stat correctly. Line mode here (foreach output
        // is line-oriented); rename lines fail the stat and degrade to text-only.
        const submodule_status = "git --no-optional-locks -C '{s}' submodule foreach --quiet --recursive " ++
            "'git status --porcelain -uall 2>/dev/null | " ++
            "sed -e \"s@^\\(..\\) @\\1 $displaypath/@\"' 2>/dev/null";
        if (popenGit(&cmd, submodule_status, .{std.mem.span(root_path)})) |sub| {
            var line_buf: [PATH_MAX]u8 = undefined;
            while (c.fgets(&line_buf, line_buf.len, sub) != null) {
                const line = trimLineEnd(&line_buf);
                if (line.len == 0) continue;
                any = true;
                h = sigFold(h, line);
                h = sigFold(h, &.{0});
                if (line.len > 3 and line[2] == ' ') {
                    h = sigFoldPathStat(h, root_path, line[3..]);
                }
            }
            _ = c.cbm_pclose(sub);
        }
    }

    if (!any) return 0;
    return if (h != 0) h else 1; // reserve 0 for "clean"
}

/// Count tracked files via git ls-files.
fn gitFileCount(root_path: Str) c_int {
    var cmd: [CMD_MAX]u8 = undefined;
    const fp = popenGit(&cmd, "git -C \"{s}\" ls-files 2>{s}", .{
        std.mem.span(root_path), NULL_DEV,
    }) orelse return 0;

    // Count newlines (one tracked file per line). `wc -l` is unavailable on
    // Windows, so count here, robust to paths longer than the read buffer.
    var count: c_int = 0;
    var buf: [1024]u8 = undefined;
    while (true) {
        const n = c.fread(&buf, 1, buf.len, fp);
        if (n == 0) break;
        count += @intCast(std.mem.count(u8, buf[0..n], "\n"));
    }
    _ = c.cbm_pclose(fp);
    return count;
}

// ── Project state lifecycle ────────────────────────────────────────

fn stateNew(project_name: Str, root_path: Str) ?*ProjectState {
    const s: *ProjectState = @ptrCast(@alignCast(c.calloc(1, @sizeOf(ProjectState)) orelse return null));
    s.project_name = dupZ(project_name);
    s.root_path = dupZ(root_path);
    s.interval_ms = POLL_BASE_MS;
    s.published_strategy = c.CBM_WATCHER_STRATEGY_PENDING;
    s.published_interval_ms = POLL_BASE_MS;
    s.last_scan_unix_s = 0;
    return s;
}

fn stateFree(s: ?*ProjectState) void {
    const state = s orelse return;
    c.free(state.project_name);
    c.free(state.root_path);
    c.free(state);
}

/// Move a state onto the deferred-free list (caller holds projects_lock).
/// The state may still be referenced by a poll_once snapshot; poll_once
/// drains the list at the start of its next cycle. Returns false when
/// growing the list fails (OOM): the state is left untouched and the
/// caller must keep it registered — freeing it immediately here could be
/// a use-after-free against an in-flight poll snapshot.
fn deferStateFree(w: *Watcher, s: *ProjectState) bool {
    if (w.pending_free_count >= w.pending_free_cap) {
        const new_cap: c_int = if (w.pending_free_cap != 0) w.pending_free_cap * 2 else 8;
        const tmp = c.realloc(@ptrCast(w.pending_free), @as(usize, @intCast(new_cap)) * @sizeOf(*ProjectState)) orelse {
            logWarn("watcher.unwatch.oom", .{ "project", s.name() });
            return false;
        };
        w.pending_free = @ptrCast(@alignCast(tmp));
        w.pending_free_cap = new_cap;
    }
    w.pending_free.?[@intCast(w.pending_free_count)] = s;
    w.pending_free_count += 1;
    return true;
}

fn drainPendingFree(w: *Watcher) void {
    for (0..@as(usize, @intCast(w.pending_free_count))) |i| stateFree(w.pending_free.?[i]);
    w.pending_free_count = 0;
}

// ── Stale-root pruning (#286) ──────────────────────────────────────

pub export fn cbm_watcher_root_missing_errno(err: c_int) bool {
    // Only ENOENT/ENOTDIR mean the root itself is gone. Anything else
    // (EACCES, EIO, ELOOP, a transient network mount, macOS TCC permission
    // revocation) is uncertainty: the directory may still exist even though
    // we cannot see it right now — never treat it as a deletion signal.
    // Windows (mingw/UCRT) maps ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND
    // to ENOENT, so the same check holds there (same convention as
    // find_deleted_files in pipeline_incremental.c).
    return err == c.ENOENT or err == c.ENOTDIR;
}

const RootStatus = enum {
    /// stat succeeded and the root is a directory
    present,
    /// genuinely gone: ENOENT/ENOTDIR (or replaced by a non-directory)
    missing,
    /// any other stat failure — must NOT count toward pruning
    uncertain,
};

fn rootStatus(root_path: ?Str, out_errno: *c_int) RootStatus {
    out_errno.* = 0;
    const path = root_path orelse return .uncertain;
    var st: c.struct_stat = undefined;
    if (c.stat(path, &st) == 0) {
        // Exists but is no longer a directory → the root directory is gone.
        return if ((st.st_mode & c.S_IFMT) == c.S_IFDIR) .present else .missing;
    }
    const err = std.c._errno().*;
    out_errno.* = err;
    return if (cbm_watcher_root_missing_errno(err)) .missing else .uncertain;
}

/// Sustained-absence window (seconds) before a missing root may be pruned.
/// Generous default: 10 minutes. Override with CBM_WATCHER_PRUNE_GRACE_S
/// (>= 0; 0 prunes as soon as the missing-poll streak is reached). Read on
/// each call so tests/operators can adjust via setenv without a restart —
/// same convention as cbm_max_file_bytes in limits.c.
fn pruneGraceS() c_long {
    const raw: ?Str = c.getenv("CBM_WATCHER_PRUNE_GRACE_S");
    if (raw) |r| {
        if (r[0] != 0) {
            std.c._errno().* = 0;
            var end: [*c]u8 = null;
            const v = c.strtol(r, &end, 10);
            if (std.c._errno().* == 0 and end != @as([*c]const u8, r) and end[0] == 0 and v >= 0) {
                return v;
            }
            // Unparseable / negative → fall through to the safe default.
        }
    }
    return PRUNE_GRACE_DEFAULT_S;
}

/// Returns false when the delete was refused because an index run holds the
/// workspace lease; the caller keeps the watch so the next poll retries.
fn deleteCachedProject(project_name: Str) bool {
    if (!c.cbm_validate_project_name(project_name)) return true;

    var path: [CMD_MAX]u8 = undefined;
    if (c.cbm_zova_user_database_path(&path, path.len) != 0) return true;
    const repo = c.cbm_zova_repository_open(&path, project_name) orelse return true;
    var workspace_id: [c.CBM_ZOVA_WORKSPACE_ID_MAX]u8 = undefined;
    copyZ(&workspace_id, std.mem.span(@as(Str, c.cbm_zova_repository_workspace_id(repo))));
    c.cbm_zova_repository_close(repo);

    var report: c.cbm_zova_operation_report_t = std.mem.zeroes(c.cbm_zova_operation_report_t);
    const code = c.cbm_zova_workspace_delete(&path, &workspace_id, &workspace_id, &report);
    const reason: Str = @ptrCast(&report.reason);
    if (code == c.CBM_ZOVA_OPERATION_BUSY and std.mem.eql(u8, std.mem.span(reason), "workspace_busy")) {
        logInfo("watcher.prune_deferred", .{ "project", project_name, "reason", reason });
        return false;
    }
    if (code != c.CBM_ZOVA_OPERATION_OK) {
        logWarn("watcher.zova_delete_failed", .{ "project", project_name, "reason", reason });
    }
    return true;
}

/// Hash table foreach callback to free state entries.
fn freeStateEntry(key: [*c]const u8, val: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
    _ = key;
    _ = ud;
    stateFree(@ptrCast(@alignCast(val)));
}

// ── Watcher lifecycle ──────────────────────────────────────────────

pub export fn cbm_watcher_new(store: ?*c.cbm_store_t, index_fn: c.cbm_index_fn, user_data: ?*anyopaque) ?*Watcher {
    const w: *Watcher = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Watcher)) orelse return null));
    w.store = store;
    w.index_fn = index_fn;
    w.user_data = user_data;
    w.projects = c.cbm_ht_create(32) orelse {
        c.free(w);
        return null;
    };
    c.cbm_mutex_init(&w.projects_lock);
    w.stopped = .init(0);
    return w;
}

pub export fn cbm_watcher_free(w_c: ?*Watcher) void {
    const w = w_c orelse return;
    // Safety net: ensure stopped is set before draining pending_free.
    // In production the caller should cbm_watcher_stop() + join first.
    w.stopped.store(1, .seq_cst);
    w.lock();
    c.cbm_ht_foreach(w.projects, freeStateEntry, null);
    c.cbm_ht_free(w.projects);
    drainPendingFree(w);
    c.free(@ptrCast(w.pending_free));
    w.unlock();
    c.cbm_mutex_destroy(&w.projects_lock);
    c.free(w);
}

// ── Watch list management ──────────────────────────────────────────

pub export fn cbm_watcher_watch(w_c: ?*Watcher, project_name_c: ?Str, root_path_c: ?Str) void {
    const w = w_c orelse return;
    const project_name = project_name_c orelse return;
    const root_path = root_path_c orelse return;

    // Reject paths with shell metacharacters — all git helpers use popen/system.
    if (!c.cbm_validate_shell_arg(root_path)) {
        logWarn("watcher.watch.reject", .{ "project", project_name, "reason", "path contains shell metacharacters" });
        return;
    }

    // Remove old entry first (key points to state's project_name).
    w.lock();
    if (w.getState(project_name)) |old| {
        _ = c.cbm_ht_delete(w.projects, project_name);
        stateFree(old);
    }

    const s = stateNew(project_name, root_path) orelse {
        w.unlock();
        logWarn("watcher.watch.oom", .{ "project", project_name, "path", root_path });
        return;
    };
    _ = c.cbm_ht_set(w.projects, s.project_name, s);
    w.unlock();
    logInfo("watcher.watch", .{ "project", project_name, "path", root_path });
}

pub export fn cbm_watcher_unwatch(w_c: ?*Watcher, project_name_c: ?Str) void {
    const w = w_c orelse return;
    const project_name = project_name_c orelse return;
    var removed = false;
    w.lock();
    if (w.getState(project_name)) |s| {
        // The entry leaves the table only once its state is safely on the
        // deferred-free list; on OOM the watch stays registered.
        if (deferStateFree(w, s)) {
            _ = c.cbm_ht_delete(w.projects, project_name);
            removed = true;
        }
    }
    w.unlock();
    if (removed) logInfo("watcher.unwatch", .{ "project", project_name });
}

pub export fn cbm_watcher_touch(w_c: ?*Watcher, project_name_c: ?Str) void {
    const w = w_c orelse return;
    const project_name = project_name_c orelse return;
    w.lock();
    // Reset backoff — poll immediately on next cycle.
    if (w.getState(project_name)) |s| s.next_poll_ns = 0;
    w.unlock();
}

pub export fn cbm_watcher_watch_count(w_c: ?*Watcher) c_int {
    const w = w_c orelse return 0;
    w.lock();
    const count: c_int = @intCast(c.cbm_ht_count(w.projects));
    w.unlock();
    return count;
}

pub export fn cbm_watcher_project_info(
    w_c: ?*Watcher,
    project_name_c: ?Str,
    out: ?*c.cbm_watcher_project_info_t,
) bool {
    if (out) |o| {
        o.* = std.mem.zeroes(c.cbm_watcher_project_info_t);
    }
    const w = w_c orelse return false;
    const project_name = project_name_c orelse return false;
    const o = out orelse return false;

    w.lock();
    defer w.unlock();

    const s = w.getState(project_name) orelse return false;
    o.strategy = s.published_strategy;
    o.poll_interval_ms = s.published_interval_ms;
    o.last_scan_unix_s = s.last_scan_unix_s;
    return true;
}

// ── Single poll cycle ──────────────────────────────────────────────

fn nowNs() i64 {
    var ts: c.struct_timespec = undefined;
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &ts);
    return @as(i64, @intCast(ts.tv_sec)) * NS_PER_SEC + @as(i64, @intCast(ts.tv_nsec));
}

/// Init baseline for a project: check if git, get HEAD, count files.
fn initBaseline(s: *ProjectState) void {
    var st: c.struct_stat = undefined;
    if (c.stat(s.root(), &st) != 0) {
        logWarn("watcher.root_gone", .{ "project", s.name(), "path", s.root() });
        s.baseline_done = true;
        s.is_git = false;
        return;
    }

    s.is_git = isGitRepo(s.root());
    s.baseline_done = true;

    if (s.is_git) {
        _ = gitHead(s.root(), &s.last_head);
        s.file_count = gitFileCount(s.root());
        s.interval_ms = cbm_watcher_poll_interval_ms(s.file_count);
        const files: Str = if (s.file_count > 0) "yes" else "0";
        logInfo("watcher.baseline", .{ "project", s.name(), "strategy", "git", "files", files });
    } else {
        logInfo("watcher.baseline", .{ "project", s.name(), "strategy", "none" });
    }

    s.next_poll_ns = nowNs() + @as(i64, s.interval_ms) * NS_PER_MS;
}

/// Check if a project has changes. Returns true if reindex needed.
///
/// HEAD movements are recorded as PENDING, not handled (#17 item 3): the
/// observed head lands in pending_head and is committed to last_head only
/// after the index refresh succeeds (pollProject). Consequences:
///   - A failed index keeps the movement pending → retry next poll.
///   - Further movements while pending coalesce into the newest observed head
///     (one retry covers them all — identical events are not re-queued).
/// The dirty-state signature is staged the same way, in pending_dirty_sig.
fn checkChanges(s: *ProjectState) bool {
    if (!s.is_git) return false;

    // Check HEAD movement.
    var changed = false;
    var head: [HEAD_MAX]u8 = @splat(0);
    if (gitHead(s.root(), &head) == 0) {
        const head_str = std.mem.sliceTo(&head, 0);
        if (s.change_pending) {
            // Coalesce: while a change awaits successful refresh, track the
            // newest observed head so one retry indexes the latest state.
            copyZ(&s.pending_head, head_str);
            changed = true;
        } else if (s.last_head[0] != 0 and !std.mem.eql(u8, head_str, std.mem.sliceTo(&s.last_head, 0))) {
            // HEAD moved — commit, checkout, pull. Record as pending; do NOT
            // touch last_head until the reindex succeeds.
            copyZ(&s.pending_head, head_str);
            s.change_pending = true;
            changed = true;
        } else {
            copyZ(&s.last_head, head_str);
        }
    }

    // Working tree: reindex only when the dirty state itself changed, so an
    // idle dirty tree does not reindex on every poll (upstream #937).
    s.pending_dirty_sig = gitDirtySignature(s.root());
    return changed or s.pending_dirty_sig != s.last_dirty_sig;
}

fn pruneMissingProject(w: *Watcher, s: *ProjectState) void {
    var name_buf: [CMD_MAX]u8 = undefined;
    copyZ(&name_buf, std.mem.span(s.project_name orelse return));
    const project_name: Str = @ptrCast(&name_buf);

    // Delete first: if an index run of this workspace holds its lease, keep
    // the watch so the root is still missing (and pruned) on the next poll.
    if (!deleteCachedProject(project_name)) return;

    var removed = false;
    w.lock();
    // Deferred free (same discipline as cbm_watcher_unwatch): this state is
    // referenced by the poll_once snapshot iterating us. On OOM the watch
    // stays registered and pruning retries on the next cycle.
    if (w.getState(project_name) == s and deferStateFree(w, s)) {
        _ = c.cbm_ht_delete(w.projects, project_name);
        removed = true;
    }
    w.unlock();

    if (removed) logInfo("watcher.root_pruned", .{ "project", project_name });
}

fn pollProject(w: *Watcher, s: *ProjectState, now: i64, reindexed: *c_int) void {
    var num_buf: [32]u8 = undefined;

    // Stale-root pruning (#286): classify the root BEFORE the baseline /
    // is_git / interval gates so vanished roots are noticed even for
    // non-git projects and regardless of adaptive backoff.
    var stat_errno: c_int = 0;
    switch (rootStatus(s.root_path, &stat_errno)) {
        .uncertain => {
            // EACCES / EIO / network blip / TCC revocation — the root may
            // still exist. Never count toward pruning; restart the streak so
            // only an uninterrupted run of genuine ENOENT/ENOTDIR
            // observations can delete user data.
            if (s.missing_root_count > 0) {
                s.missing_root_count = 0;
                s.first_missing_ms = 0;
            }
            const errno_str = std.mem.printSentinel(&num_buf, "{d}", .{stat_errno}, 0) catch unreachable;
            logWarn("watcher.root_stat_error", .{ "project", s.name(), "path", s.root(), "errno", errno_str.ptr });
            return;
        },
        .missing => {
            const now_ms = c.cbm_now_ms();
            if (s.missing_root_count == 0) s.first_missing_ms = now_ms;
            s.missing_root_count += 1;
            const polls_str = std.mem.printSentinel(&num_buf, "{d}", .{s.missing_root_count}, 0) catch unreachable;
            logWarn("watcher.root_missing", .{ "project", s.name(), "path", s.root(), "polls", polls_str.ptr });
            const grace_ms = @as(u64, @bitCast(@as(i64, pruneGraceS()))) *% MSEC_PER_SEC;
            if (s.missing_root_count >= MISSING_ROOT_DELETE_AFTER and now_ms -% s.first_missing_ms >= grace_ms) {
                pruneMissingProject(w, s);
            }
            return;
        },
        .present => {},
    }
    if (s.missing_root_count > 0) {
        logInfo("watcher.root_restored", .{ "project", s.name(), "path", s.root() });
        s.missing_root_count = 0;
        s.first_missing_ms = 0;
    }

    // Initialize baseline on first poll.
    if (!s.baseline_done) {
        initBaseline(s);
        w.lock();
        s.published_strategy = if (s.is_git) c.CBM_WATCHER_STRATEGY_GIT else c.CBM_WATCHER_STRATEGY_NONE;
        s.published_interval_ms = s.interval_ms;
        s.last_scan_unix_s = @as(i64, c.time(null));
        w.unlock();
        return;
    }

    // Skip non-git projects.
    if (!s.is_git) return;

    // Respect adaptive interval.
    if (now < s.next_poll_ns) return;

    // Check for changes.
    if (!checkChanges(s)) {
        s.next_poll_ns = now + @as(i64, s.interval_ms) * NS_PER_MS;
        w.lock();
        s.published_interval_ms = s.interval_ms;
        s.last_scan_unix_s = @as(i64, c.time(null));
        w.unlock();
        return;
    }

    // Trigger reindex.
    logInfo("watcher.changed", .{ "project", s.name(), "strategy", "git" });
    if (w.index_fn) |index_fn| {
        const rc = index_fn(s.name(), s.root(), w.user_data);
        if (rc == 0) {
            reindexed.* += 1;
            // Mark handled only after successful refresh (#17 item 3): commit
            // the OBSERVED pending head, never the current HEAD — a newer
            // change that arrived while indexing must stay detectable on the
            // next poll instead of being silently swallowed.
            if (s.change_pending) {
                copyZ(&s.last_head, std.mem.sliceTo(&s.pending_head, 0));
                s.change_pending = false;
                s.pending_head[0] = 0;
            }
            s.last_dirty_sig = s.pending_dirty_sig;
            // Refresh file count for interval.
            s.file_count = gitFileCount(s.root());
            s.interval_ms = cbm_watcher_poll_interval_ms(s.file_count);
        } else if (rc > 0) {
            // Skipped (busy or shutting down): baselines stay uncommitted, so
            // the next poll retries.
            logInfo("watcher.index.retry", .{ "project", s.name() });
        } else {
            // Keep the change pending: last_head still holds the previously
            // indexed head, so the next poll re-detects and retries.
            logWarn("watcher.index.err", .{ "project", s.name(), "retry", "pending" });
        }
    }

    s.next_poll_ns = now + @as(i64, s.interval_ms) * NS_PER_MS;
    w.lock();
    s.published_interval_ms = s.interval_ms;
    s.last_scan_unix_s = @as(i64, c.time(null));
    w.unlock();
}

/// Snapshot of project state pointers, filled under the lock.
const Snapshot = struct {
    items: [*]*ProjectState,
    count: usize,
    cap: usize,

    fn add(key: [*c]const u8, val: ?*anyopaque, ud: ?*anyopaque) callconv(.c) void {
        _ = key;
        const sc: *Snapshot = @ptrCast(@alignCast(ud));
        if (val != null and sc.count < sc.cap) {
            sc.items[sc.count] = @ptrCast(@alignCast(val));
            sc.count += 1;
        }
    }
};

pub export fn cbm_watcher_poll_once(w_c: ?*Watcher) c_int {
    const w = w_c orelse return 0;

    // Snapshot project pointers under lock, then poll without holding it.
    // This keeps the critical section small — pollProject does git I/O and
    // may invoke index_fn which runs the full pipeline.
    w.lock();

    // Free deferred entries from the previous cycle.
    drainPendingFree(w);

    const n: usize = @intCast(c.cbm_ht_count(w.projects));
    if (n == 0) {
        w.unlock();
        return 0;
    }
    const snap: [*]*ProjectState = @ptrCast(@alignCast(c.malloc(n * @sizeOf(*ProjectState)) orelse {
        w.unlock();
        return 0;
    }));
    defer c.free(@ptrCast(snap));
    var sc: Snapshot = .{ .items = snap, .count = 0, .cap = n };
    c.cbm_ht_foreach(w.projects, Snapshot.add, &sc);
    w.unlock();

    const now = nowNs();
    var reindexed: c_int = 0;
    for (snap[0..sc.count]) |s| pollProject(w, s, now, &reindexed);
    return reindexed;
}

// ── Blocking run loop ──────────────────────────────────────────────

pub export fn cbm_watcher_stop(w_c: ?*Watcher) void {
    if (w_c) |w| w.stopped.store(1, .seq_cst);
}

pub export fn cbm_watcher_run(w_c: ?*Watcher, base_interval_ms_arg: c_int) c_int {
    const w = w_c orelse return c.CBM_NOT_FOUND;
    const base_interval_ms: c_int = if (base_interval_ms_arg <= 0) POLL_BASE_MS else base_interval_ms_arg;

    const pace: Str = if (base_interval_ms > 999) "multi-sec" else "fast";
    logInfo("watcher.start", .{ "interval_ms", pace });

    while (!w.isStopped()) {
        _ = cbm_watcher_poll_once(w);

        // Sleep in small increments to allow responsive shutdown.
        var slept: c_int = 0;
        while (slept < base_interval_ms and !w.isStopped()) {
            const chunk = @min(base_interval_ms - slept, SLEEP_CHUNK_MS);
            _ = c.cbm_usleep(@as(c_uint, @intCast(chunk)) * MSEC_PER_SEC);
            slept += chunk;
        }
    }

    logInfo("watcher.stop", .{});
    return 0;
}
