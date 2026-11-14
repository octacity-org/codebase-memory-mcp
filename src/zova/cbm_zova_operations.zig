//! cbm_zova_operations.zig — Maintenance operations on a cbm.zova file:
//! status, compaction, format migration, backup and restore, database and
//! workspace archives, health and recovery, and the schema-5 repack.
//!
//! Implements src/zova/cbm_zova_operations.h. Each operation returns a code
//! and describes the outcome in its report. Without Zova an operation checks
//! its arguments and returns INCOMPATIBLE.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const Str = interop.Str;
const span = std.mem.span;
const sliceTo = std.mem.sliceTo;

const Code = c.cbm_zova_operation_code_t;
const Report = c.cbm_zova_operation_report_t;
const HealthClass = c.cbm_zova_health_class_t;
const RepackRequest = c.cbm_zova_repack_request_t;
const RepackReport = c.cbm_zova_repack_report_t;
const CoordStatus = c.cbm_zova_coord_status_t;
const CoordHold = c.cbm_zova_coord_hold_t;
const WriterGuard = c.cbm_zova_writer_guard_t;
const Snapshot = c.cbm_zova_workspace_snapshot_t;
const GenerationInput = c.cbm_zova_workspace_generation_input_t;
const GenerationResult = c.cbm_zova_workspace_generation_result_t;

// Zova SDK types. They exist only with Zova; nothing reaches them without it.
const Db = c.zova_database;
const Stmt = c.zova_statement;

const with_zova = c.CBM_WITH_ZOVA != 0;
const is_windows = builtin.os.tag == .windows;

const OK: Code = c.CBM_ZOVA_OPERATION_OK;
const NOOP: Code = c.CBM_ZOVA_OPERATION_NOOP;
const INVALID: Code = c.CBM_ZOVA_OPERATION_INVALID;
const BUSY: Code = c.CBM_ZOVA_OPERATION_BUSY;
const INCOMPATIBLE: Code = c.CBM_ZOVA_OPERATION_INCOMPATIBLE;
const VERIFY_FAILED: Code = c.CBM_ZOVA_OPERATION_VERIFY_FAILED;
const DISK_REFUSED: Code = c.CBM_ZOVA_OPERATION_DISK_REFUSED;
const CONFIRMATION_REQUIRED: Code = c.CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED;
const WORKSPACE_REBUILD_REQUIRED: Code = c.CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED;
const WHOLE_FILE_RECOVERY_REQUIRED: Code = c.CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED;

const PATH_CAP = 4096;
const ID_MAX: usize = c.CBM_ZOVA_WORKSPACE_ID_MAX;
const HEX_LEN: usize = c.CBM_SHA256_HEX_LEN;
const SCHEMA_VERSION = c.CBM_ZOVA_DATABASE_SCHEMA_VERSION;
const ARCHIVE_VERSION = c.CBM_ZOVA_OPERATIONS_ARCHIVE_VERSION;

const schema_version_sql = "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1";
const workspace_count_sql = "SELECT count(*) FROM cbm_workspace_registry";

/// Counts the workspaces whose active generation is missing, not ready, or
/// has no integrity row.
const invalid_generations_sql =
    "SELECT count(*) FROM cbm_workspace_registry w LEFT JOIN " ++
    "cbm_database_generation_v1 g ON g.workspace_key=w.workspace_key AND " ++
    "g.generation=w.active_generation AND g.state='ready' LEFT JOIN " ++
    "cbm_generation_integrity_v2 i ON i.workspace_key=w.workspace_key AND " ++
    "i.generation=w.active_generation WHERE w.active_generation<=0 OR " ++
    "g.workspace_key IS NULL OR i.workspace_key IS NULL";

// ── Reports ─────────────────────────────────────────────────────

/// Resets `out` (when given) to `code`, `operation` and `reason`, and returns
/// `code` so a refusal is one statement.
fn operationsReport(out: ?*Report, code: Code, operation: []const u8, reason: []const u8) Code {
    if (out) |report| {
        report.* = std.mem.zeroes(Report);
        report.code = code;
        _ = interop.bprint(&report.operation, "{s}", .{operation});
        _ = interop.bprint(&report.reason, "{s}", .{reason});
    }
    return code;
}

fn operationsSetWorkspaceId(out: *Report, workspace_id: Str) void {
    _ = interop.bprint(&out.workspace_id, "{s}", .{span(workspace_id)});
}

fn operationsCopyStatusMetrics(out: *Report, status: *const Report) void {
    out.schema_version = status.schema_version;
    out.database_bytes = status.database_bytes;
    out.wal_bytes = status.wal_bytes;
    out.free_bytes = status.free_bytes;
    out.page_size = status.page_size;
    out.page_count = status.page_count;
    out.freelist_count = status.freelist_count;
}

fn operationsNow() c.struct_timespec {
    var now = std.mem.zeroes(c.struct_timespec);
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &now);
    return now;
}

fn operationsElapsedMs(started: c.struct_timespec, finished: c.struct_timespec) f64 {
    return @as(f64, @floatFromInt(finished.tv_sec - started.tv_sec)) * 1000.0 +
        @as(f64, @floatFromInt(finished.tv_nsec - started.tv_nsec)) / 1000000.0;
}

/// The string when it is neither NULL nor empty.
fn nonEmpty(text: ?Str) ?Str {
    const value = text orelse return null;
    return if (value[0] != 0) value else null;
}

/// strcmp() == 0 for two NUL-terminated buffers.
fn bufEql(left: []const u8, right: []const u8) bool {
    return std.mem.eql(u8, sliceTo(left, 0), sliceTo(right, 0));
}

// ── Zova SDK calls ──────────────────────────────────────────────

/// zova_database_open_with_options. Null unless the open returned ZOVA_OK and
/// a handle; `out_status` (optional) receives the status either way.
fn operationsOpen(path: Str, flags: u32, busy_timeout_ms: u32, out_status: ?*c.zova_status) ?*Db {
    var db: ?*Db = null;
    var message = std.mem.zeroes(c.zova_message);
    const status = c.zova_database_open_with_options(&c.zova_database_open_options_request{
        .path = path,
        .flags = flags,
        .busy_timeout_ms = busy_timeout_ms,
        .out_db = &db,
        .out_error_message = &message,
    });
    c.zova_message_free(&message);
    if (out_status) |out| out.* = status;
    if (status == c.ZOVA_OK) return db;
    if (db) |opened| _ = c.zova_database_close(opened);
    return null;
}

/// Read-only with the 5 s busy timeout every reader here uses.
fn operationsOpenReadOnly(path: Str) ?*Db {
    return operationsOpen(path, c.ZOVA_OPEN_READ_ONLY, 5000, null);
}

/// Null unless the prepare returned ZOVA_OK and a statement; `out_status`
/// (optional) receives the status either way.
fn operationsPrepare(db: *Db, sql: Str, out_status: ?*c.zova_status) ?*Stmt {
    var statement: ?*Stmt = null;
    const status = c.zova_database_prepare(&c.zova_database_prepare_request{
        .db = db,
        .sql = sql,
        .out_statement = &statement,
    });
    if (out_status) |out| out.* = status;
    if (status == c.ZOVA_OK) return statement;
    if (statement) |prepared| _ = c.zova_statement_finalize(prepared);
    return null;
}

fn operationsStep(statement: *Stmt, out: *c.zova_step_result) bool {
    return c.zova_statement_step(&c.zova_statement_step_request{
        .statement = statement,
        .out_result = out,
    }) == c.ZOVA_OK;
}

/// Steps once; true when the step succeeded with `expected`.
fn operationsStepIs(statement: *Stmt, expected: c.zova_step_result) bool {
    var step: c.zova_step_result = if (expected == c.ZOVA_STEP_ROW) c.ZOVA_STEP_DONE else c.ZOVA_STEP_ROW;
    return operationsStep(statement, &step) and step == expected;
}

fn operationsColumnI64(statement: *Stmt, index: c_int, out: *i64) bool {
    return c.zova_statement_column_int64(&c.zova_statement_column_int64_request{
        .statement = statement,
        .index = index,
        .out_value = out,
    }) == c.ZOVA_OK;
}

/// True when the column was read and is not NULL. The caller frees `out`
/// with zova_text_free either way.
fn operationsColumnText(statement: *Stmt, index: c_int, out: *c.zova_text) bool {
    return c.zova_statement_column_text(&c.zova_statement_column_text_request{
        .statement = statement,
        .index = index,
        .out_text = out,
    }) == c.ZOVA_OK and out.data != null;
}

fn operationsBindText(statement: *Stmt, index: c_int, text: Str) bool {
    return c.zova_statement_bind_text(&c.zova_statement_bind_text_request{
        .statement = statement,
        .index = index,
        .data = text,
        .len = std.mem.len(text),
    }) == c.ZOVA_OK;
}

fn operationsBindI64(statement: *Stmt, index: c_int, value: i64) bool {
    return c.zova_statement_bind_int64(&c.zova_statement_bind_int64_request{
        .statement = statement,
        .index = index,
        .value = value,
    }) == c.ZOVA_OK;
}

/// The text as printf's "%.*s" prints it: its length, or up to a NUL.
fn operationsTextSlice(text: c.zova_text) []const u8 {
    return sliceTo(text.data[0..text.len], 0);
}

/// Copies a non-empty text that fits into `out` as a C string.
fn operationsTextInto(text: c.zova_text, out: []u8) bool {
    if (text.len == 0 or text.len >= out.len) return false;
    @memcpy(out[0..text.len], text.data[0..text.len]);
    out[text.len] = 0;
    return true;
}

fn operationsQueryI64(db: *Db, sql: Str, out: *i64) bool {
    const statement = operationsPrepare(db, sql, null) orelse return false;
    defer _ = c.zova_statement_finalize(statement);
    return operationsStepIs(statement, c.ZOVA_STEP_ROW) and operationsColumnI64(statement, 0, out);
}

/// Statuses that say the database could not be read right now (locked by
/// another connection, out of memory or file descriptors), not that it is
/// damaged.
fn operationsStatusIsTransient(status: c.zova_status) bool {
    return status == c.ZOVA_BUSY or status == c.ZOVA_LOCKED or status == c.ZOVA_OUT_OF_MEMORY or
        status == c.ZOVA_CANT_OPEN;
}

/// PRAGMA quick_check. `out_status` (optional) receives the status of a
/// failed prepare or step; it stays ZOVA_OK when the check ran and found
/// damage.
fn operationsQuickCheckStatus(db: *Db, out_status: ?*c.zova_status) bool {
    if (out_status) |out| out.* = c.ZOVA_OK;
    var status: c.zova_status = c.ZOVA_OK;
    const statement = operationsPrepare(db, "PRAGMA quick_check", &status) orelse {
        if (out_status) |out| out.* = status;
        return false;
    };
    defer _ = c.zova_statement_finalize(statement);
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    status = c.zova_statement_step(&c.zova_statement_step_request{
        .statement = statement,
        .out_result = &step,
    });
    if (status != c.ZOVA_OK) {
        if (out_status) |out| out.* = status;
        return false;
    }
    var text = std.mem.zeroes(c.zova_text);
    defer c.zova_text_free(&text);
    return step == c.ZOVA_STEP_ROW and operationsColumnText(statement, 0, &text) and
        text.len == 2 and std.mem.eql(u8, text.data[0..2], "ok");
}

fn operationsQuickCheck(db: *Db) bool {
    return operationsQuickCheckStatus(db, null);
}

fn operationsForeignKeysOk(db: *Db) bool {
    const statement = operationsPrepare(db, "PRAGMA foreign_key_check", null) orelse return false;
    defer _ = c.zova_statement_finalize(statement);
    return operationsStepIs(statement, c.ZOVA_STEP_DONE);
}

/// Opens `path` read-only and checks that it is a sound database of the
/// current schema whose every workspace has a ready, verified generation.
fn operationsVerifyDatabase(path: Str, out_schema_version: ?*c_int) bool {
    const db = operationsOpenReadOnly(path) orelse return false;
    defer _ = c.zova_database_close(db);
    var schema_version: i64 = 0;
    var invalid_generations: i64 = 0;
    if (!(operationsQueryI64(db, schema_version_sql, &schema_version) and
        schema_version == SCHEMA_VERSION and operationsQuickCheck(db) and
        operationsForeignKeysOk(db) and
        operationsQueryI64(db, invalid_generations_sql, &invalid_generations) and
        invalid_generations == 0))
        return false;
    if (out_schema_version) |out| out.* = @intCast(schema_version);
    return true;
}

// ── Files and paths ─────────────────────────────────────────────

/// snprintf into a path buffer; null when the path does not fit.
fn operationsPath(buf: *[PATH_CAP]u8, comptime fmt: []const u8, args: anytype) ?Str {
    if (interop.bprint(buf, fmt, args) >= buf.len) return null;
    return @ptrCast(buf);
}

/// The directory holding `path`, written to `buf`: "." when `path` has no
/// separator.
fn operationsParentDirectory(path: []const u8, buf: *[PATH_CAP]u8) ?Str {
    if (path.len >= buf.len) return null;
    var slash = std.mem.lastIndexOfScalar(u8, path, '/');
    if (is_windows) {
        const backslash = std.mem.lastIndexOfScalar(u8, path, '\\');
        if (slash == null or (backslash != null and backslash.? > slash.?)) slash = backslash;
    }
    const parent: []const u8 = if (slash) |at| path[0..@max(at, 1)] else ".";
    @memcpy(buf[0..parent.len], parent);
    buf[parent.len] = 0;
    return @ptrCast(buf);
}

/// `path` without its ".zova" suffix. Null when it has no such suffix or is
/// too long for the sibling paths derived from it.
fn operationsZovaStem(path: []const u8) ?[]const u8 {
    if (path.len <= 5 or !std.mem.endsWith(u8, path, ".zova") or path.len - 5 > 4000) return null;
    return path[0 .. path.len - 5];
}

fn operationsSyncDirectory(path: Str) bool {
    if (is_windows) return true;
    const fd = c.open(path, c.O_RDONLY | c.O_DIRECTORY);
    if (fd < 0) return false;
    defer _ = c.close(fd);
    return c.fsync(fd) == 0;
}

/// fsync of a file's contents; nothing to do on Windows.
fn operationsSyncFile(path: Str) bool {
    if (is_windows) return true;
    const fd = c.open(path, c.O_RDONLY);
    if (fd < 0) return false;
    defer _ = c.close(fd);
    return c.fsync(fd) == 0;
}

fn operationsSyncStream(file: *c.FILE) bool {
    if (is_windows) return true;
    return c.fsync(c.fileno(file)) == 0;
}

/// mkdir with mode 0700. The Windows form of cbm_mkdir_mode drops the mode
/// and does not translate; cbm_mkdir is the same _mkdir call there.
fn operationsMakePrivateDirectory(path: Str) bool {
    return (if (is_windows) c.cbm_mkdir(path) else c.cbm_mkdir_mode(path, 0o700)) == 0;
}

fn operationsSameFile(left: Str, right: Str) bool {
    var left_stat: c.struct_stat = undefined;
    var right_stat: c.struct_stat = undefined;
    if (c.stat(left, &left_stat) != 0 or c.stat(right, &right_stat) != 0) return false;
    if (is_windows) return std.mem.eql(u8, span(left), span(right));
    return left_stat.st_dev == right_stat.st_dev and left_stat.st_ino == right_stat.st_ino;
}

/// cbm_lstat: the path itself, never the target of a symlink (plain stat on
/// Windows). Null when the path does not exist.
fn operationsLstat(path: Str) ?c.struct_stat {
    var value: c.struct_stat = undefined;
    return if (c.cbm_lstat(path, &value) == 0) value else null;
}

fn operationsIsRegular(value: *const c.struct_stat) bool {
    return (value.st_mode & c.S_IFMT) == c.S_IFREG;
}

fn operationsIsDirectory(value: *const c.struct_stat) bool {
    return (value.st_mode & c.S_IFMT) == c.S_IFDIR;
}

/// st_size of `path` through stat(); null when stat fails.
fn operationsFileSize(path: Str) ?i64 {
    var value: c.struct_stat = undefined;
    return if (c.stat(path, &value) == 0) value.st_size else null;
}

/// A file size as the unsigned byte count the reports carry.
fn operationsBytes(size: i64) u64 {
    return @bitCast(size);
}

/// A symlink reports S_IFLNK through lstat, so "regular" already excludes it.
fn operationsPathIsRegularNoSymlink(path: Str) bool {
    const value = operationsLstat(path) orelse return false;
    return operationsIsRegular(&value);
}

const PathState = enum { absent, regular, other };

fn operationsRestorePathState(path: Str) PathState {
    const value = operationsLstat(path) orelse return .absent;
    return if (operationsIsRegular(&value)) .regular else .other;
}

/// Unlinks `path` when it is a regular file. False when it is something
/// else or the unlink fails.
fn operationsRemoveIfRegular(path: Str) bool {
    return switch (operationsRestorePathState(path)) {
        .absent => true,
        .regular => c.cbm_unlink(path) == 0,
        .other => false,
    };
}

fn operationsRemoveClosedDatabaseSidecars(database_path: Str) bool {
    var buf: [PATH_CAP]u8 = undefined;
    for ([_][]const u8{ "-wal", "-shm", "-journal" }) |suffix| {
        const path = operationsPath(&buf, "{s}{s}", .{ span(database_path), suffix }) orelse return false;
        if (!operationsRemoveIfRegular(path)) return false;
    }
    return true;
}

fn operationsRemoveClosedWriterLock(database_path: Str) bool {
    var buf: [PATH_CAP]u8 = undefined;
    const path = operationsPath(&buf, "{s}.writer.lock", .{span(database_path)}) orelse return false;
    return operationsRemoveIfRegular(path);
}

/// Bytes available to this user on the filesystem holding `path`.
fn operationsFreeBytes(path: Str) error{ Unavailable, Overflow }!u64 {
    if (is_windows) {
        var parent_buf: [PATH_CAP]u8 = undefined;
        const parent = operationsParentDirectory(span(path), &parent_buf) orelse return error.Unavailable;
        var available = std.mem.zeroes(c.ULARGE_INTEGER);
        if (c.GetDiskFreeSpaceExA(parent, &available, null, null) == 0) return error.Unavailable;
        return available.QuadPart;
    }
    var filesystem = std.mem.zeroes(c.struct_statvfs);
    if (c.statvfs(path, &filesystem) != 0) return error.Unavailable;
    const fragment_size: u64 = if (filesystem.f_frsize != 0) filesystem.f_frsize else filesystem.f_bsize;
    if (fragment_size == 0) return error.Overflow;
    return std.math.mul(u64, filesystem.f_bavail, fragment_size) catch error.Overflow;
}

// ── Test faults ─────────────────────────────────────────────────

/// Test hook: CBM_ZOVA_TEST_CRASH_PHASE exits at the named phase and
/// CBM_ZOVA_TEST_FAIL_PHASE makes it fail.
fn operationsTestFault(phase: []const u8) bool {
    const crash: [*c]const u8 = c.getenv("CBM_ZOVA_TEST_CRASH_PHASE");
    if (crash != null and std.mem.eql(u8, span(crash), phase)) {
        if (is_windows) c.abort() else c._exit(86);
    }
    const fault: [*c]const u8 = c.getenv("CBM_ZOVA_TEST_FAIL_PHASE");
    return fault != null and std.mem.eql(u8, span(fault), phase);
}

fn operationsReplacementFault(operation: []const u8, phase: []const u8, out: ?*Report) bool {
    if (!operationsTestFault(phase)) return false;
    _ = operationsReport(out, VERIFY_FAILED, operation, phase);
    return true;
}

// ── Archives ────────────────────────────────────────────────────

const ArchiveManifest = struct {
    archive_version: c_int = 0,
    schema_version: c_int = 0,
    database_bytes: u64 = 0,
    database_sha256: [HEX_LEN + 1]u8 = @splat(0),
};

fn operationsFileSha256(path: Str, out: *[HEX_LEN + 1]u8, out_bytes: ?*u64) bool {
    const opened: ?*c.FILE = c.fopen(path, "rb");
    const file = opened orelse return false;
    var context: c.cbm_sha256_ctx = undefined;
    c.cbm_sha256_init(&context);
    var buffer: [65536]u8 = undefined;
    var bytes: u64 = 0;
    var failed = false;
    while (c.feof(file) == 0) {
        const count = c.fread(&buffer, 1, buffer.len, file);
        if (count != 0) {
            c.cbm_sha256_update(&context, &buffer, count);
            bytes += count;
        }
        if (c.ferror(file) != 0) {
            failed = true;
            break;
        }
    }
    _ = c.fclose(file);
    if (failed) return false;
    var digest: [c.CBM_SHA256_DIGEST_LEN]u8 = undefined;
    c.cbm_sha256_final(&context, &digest);
    const hex = "0123456789abcdef";
    for (digest, 0..) |byte, i| {
        out[i * 2] = hex[byte >> 4];
        out[i * 2 + 1] = hex[byte & 15];
    }
    out[HEX_LEN] = 0;
    if (out_bytes) |total| total.* = bytes;
    return true;
}

fn operationsWrite(file: *c.FILE, bytes: []const u8) bool {
    return c.fwrite(bytes.ptr, 1, bytes.len, file) == bytes.len;
}

/// One workspace's entry of a database archive manifest, from the current
/// row of operationsWorkspaceManifestQuery.
fn operationsWorkspaceManifestEntry(statement: *Stmt, buf: []u8) ?[]const u8 {
    var values: [6]c.zova_text = @splat(std.mem.zeroes(c.zova_text));
    defer for (&values) |*value| c.zova_text_free(value);
    var generation: i64 = 0;
    if (!operationsColumnI64(statement, 1, &generation)) return null;
    const columns = [_]c_int{ 0, 2, 3, 4, 5, 6 };
    for (columns, &values) |column, *value| {
        if (!operationsColumnText(statement, column, value)) return null;
    }
    const written = interop.bprint(
        buf,
        "{{\"workspace_id\":\"{s}\",\"generation\":{d}," ++
            "\"metadata_sha256\":\"{s}\",\"fts_sha256\":\"{s}\"," ++
            "\"topology_sha256\":\"{s}\",\"node_vector_sha256\":\"{s}\"," ++
            "\"token_vector_sha256\":\"{s}\"}}",
        .{
            operationsTextSlice(values[0]), generation,
            operationsTextSlice(values[1]), operationsTextSlice(values[2]),
            operationsTextSlice(values[3]), operationsTextSlice(values[4]),
            operationsTextSlice(values[5]),
        },
    );
    return if (written < buf.len) buf[0..written] else null;
}

fn operationsWorkspaceManifestQuery(db: *Db) ?*Stmt {
    return operationsPrepare(
        db,
        "SELECT r.workspace_id,r.active_generation,i.metadata_sha256," ++
            "i.fts_sha256,i.topology_sha256,i.node_vector_sha256," ++
            "i.token_vector_sha256 FROM cbm_workspace_registry r JOIN " ++
            "cbm_generation_integrity_v2 i ON i.workspace_key=r.workspace_key AND " ++
            "i.generation=r.active_generation ORDER BY r.workspace_id",
        null,
    );
}

/// A malloc'd copy of `value` escaped for a JSON string. Null for a NULL
/// `value` and on OOM.
fn operationsJsonEscapeAlloc(value: [*c]const u8) ?[*:0]u8 {
    if (value == null) return null;
    const source = span(value);
    if (source.len > (std.math.maxInt(usize) - 1) / 6) return null;
    const escaped = @as(?[*]u8, interop.mallocN(u8, source.len * 6 + 1)) orelse return null;
    var used: usize = 0;
    const hex = "0123456789abcdef";
    for (source) |byte| {
        const short_escape: ?*const [2]u8 = switch (byte) {
            '"' => "\\\"",
            '\\' => "\\\\",
            0x08 => "\\b",
            0x0c => "\\f",
            '\n' => "\\n",
            '\r' => "\\r",
            '\t' => "\\t",
            else => null,
        };
        if (short_escape) |escape| {
            escaped[used..][0..2].* = escape.*;
            used += 2;
        } else if (byte < 0x20) {
            escaped[used..][0..4].* = "\\u00".*;
            escaped[used + 4] = hex[byte >> 4];
            escaped[used + 5] = hex[byte & 15];
            used += 6;
        } else {
            escaped[used] = byte;
            used += 1;
        }
    }
    escaped[used] = 0;
    return @ptrCast(escaped);
}

/// The whole manifest of a workspace archive, malloc'd.
fn operationsWorkspaceArchiveManifestText(manifest: *const ArchiveManifest, snapshot: *const Snapshot) ?[*:0]u8 {
    const workspace_id = operationsJsonEscapeAlloc(&snapshot.workspace_id);
    defer c.free(workspace_id);
    const root = operationsJsonEscapeAlloc(snapshot.root_path);
    defer c.free(root);
    const project = operationsJsonEscapeAlloc(snapshot.project);
    defer c.free(project);
    const model = operationsJsonEscapeAlloc(snapshot.model_fingerprint);
    defer c.free(model);
    if (workspace_id == null or root == null or project == null or model == null) return null;
    const integrity = &snapshot.integrity;
    const format =
        "{{\"archive_version\":{d},\"schema_version\":{d},\"archive_kind\":\"workspace\"," ++
        "\"database\":{{\"path\":\"data.zova\",\"bytes\":{d},\"sha256\":\"{s}\"}}," ++
        "\"workspace\":{{\"workspace_id\":\"{s}\",\"canonical_root\":\"{s}\"," ++
        "\"project\":\"{s}\",\"model_fingerprint\":\"{s}\",\"vector_dimensions\":{d}," ++
        "\"generation\":{d},\"graph_nodes\":{d},\"graph_edges\":{d}," ++
        "\"metadata_nodes\":{d},\"metadata_edges\":{d}," ++
        "\"metadata_topology_edges\":{d},\"fts_rows\":{d}," ++
        "\"node_vector_rows\":{d},\"token_vector_rows\":{d}," ++
        "\"node_vectors\":{d},\"token_vectors\":{d}," ++
        "\"metadata_sha256\":\"{s}\",\"fts_sha256\":\"{s}\"," ++
        "\"topology_sha256\":\"{s}\",\"node_vector_sha256\":\"{s}\"," ++
        "\"token_vector_sha256\":\"{s}\"}}}}\n";
    const args = .{
        manifest.archive_version,                   manifest.schema_version,
        manifest.database_bytes,                    sliceTo(&manifest.database_sha256, 0),
        span(workspace_id.?),                       span(root.?),
        span(project.?),                            span(model.?),
        snapshot.vector_dimensions,                 snapshot.generation,
        integrity.graph_nodes,                      integrity.graph_edges,
        integrity.metadata_nodes,                   integrity.metadata_edges,
        integrity.metadata_topology_edges,          integrity.fts_rows,
        integrity.node_vector_rows,                 integrity.token_vector_rows,
        integrity.node_vectors,                     integrity.token_vectors,
        sliceTo(&integrity.metadata_sha256, 0),     sliceTo(&integrity.fts_sha256, 0),
        sliceTo(&integrity.topology_sha256, 0),     sliceTo(&integrity.node_vector_sha256, 0),
        sliceTo(&integrity.token_vector_sha256, 0),
    };
    const needed = std.fmt.count(format, args);
    const text = @as(?[*]u8, interop.mallocN(u8, needed + 1)) orelse return null;
    _ = interop.bprint(text[0 .. needed + 1], format, args);
    return @ptrCast(text);
}

fn operationsWriteWorkspaceArchiveManifest(manifest_path: Str, manifest: *const ArchiveManifest, snapshot: *const Snapshot) bool {
    const text = operationsWorkspaceArchiveManifestText(manifest, snapshot) orelse return false;
    defer c.free(text);
    const opened: ?*c.FILE = c.fopen(manifest_path, "wb");
    const file = opened orelse return false;
    defer _ = c.fclose(file);
    return operationsWrite(file, span(text)) and c.fflush(file) == 0 and operationsSyncStream(file);
}

fn operationsWriteArchiveManifest(data_path: Str, manifest_path: Str, manifest: *const ArchiveManifest) bool {
    const db = operationsOpenReadOnly(data_path) orelse return false;
    defer _ = c.zova_database_close(db);
    const opened: ?*c.FILE = c.fopen(manifest_path, "wb");
    const file = opened orelse return false;
    defer _ = c.fclose(file);
    const statement = operationsWorkspaceManifestQuery(db) orelse return false;
    defer _ = c.zova_statement_finalize(statement);
    var header_buf: [512]u8 = undefined;
    const header_len = interop.bprint(
        &header_buf,
        "{{\"archive_version\":{d},\"schema_version\":{d}," ++
            "\"database\":{{\"path\":\"data.zova\",\"bytes\":{d}," ++
            "\"sha256\":\"{s}\"}},\"workspaces\":[",
        .{
            manifest.archive_version, manifest.schema_version,
            manifest.database_bytes,  sliceTo(&manifest.database_sha256, 0),
        },
    );
    if (header_len >= header_buf.len or !operationsWrite(file, header_buf[0..header_len])) return false;
    var first = true;
    while (true) {
        var step: c.zova_step_result = c.ZOVA_STEP_DONE;
        if (!operationsStep(statement, &step)) return false;
        if (step == c.ZOVA_STEP_DONE) break;
        var entry_buf: [1024]u8 = undefined;
        const entry = operationsWorkspaceManifestEntry(statement, &entry_buf) orelse return false;
        if (!first and !operationsWrite(file, ",")) return false;
        if (!operationsWrite(file, entry)) return false;
        first = false;
    }
    return operationsWrite(file, "]}\n") and c.fflush(file) == 0 and operationsSyncStream(file);
}

/// The whole file as a malloc'd C string; files over 16 MiB are refused.
fn operationsReadFile(path: Str) ?[*:0]u8 {
    const size = operationsFileSize(path) orelse return null;
    if (size < 0 or size > 16 * 1024 * 1024) return null;
    const length: usize = @intCast(size);
    const opened: ?*c.FILE = c.fopen(path, "rb");
    const file = opened orelse return null;
    defer _ = c.fclose(file);
    const text = @as(?[*]u8, interop.mallocN(u8, length + 1)) orelse return null;
    if (c.fread(text, 1, length, file) != length or c.ferror(file) != 0) {
        c.free(text);
        return null;
    }
    text[length] = 0;
    return @ptrCast(text);
}

/// An archive directory holds exactly data.zova and manifest.json, both
/// regular files.
fn operationsArchiveHasExactMembers(archive: Str) bool {
    const directory = c.cbm_opendir(archive) orelse return false;
    defer c.cbm_closedir(directory);
    var data_seen: usize = 0;
    var manifest_seen: usize = 0;
    while (@as(?*c.cbm_dirent_t, c.cbm_readdir(directory))) |entry| {
        const name = sliceTo(&entry.name, 0);
        const is_data = std.mem.eql(u8, name, "data.zova");
        const is_manifest = std.mem.eql(u8, name, "manifest.json");
        if (!is_data and !is_manifest) return false;
        var path_buf: [PATH_CAP]u8 = undefined;
        const path = operationsPath(&path_buf, "{s}/{s}", .{ span(archive), name }) orelse return false;
        if (!operationsPathIsRegularNoSymlink(path)) return false;
        if (is_data) data_seen += 1;
        if (is_manifest) manifest_seen += 1;
    }
    return data_seen == 1 and manifest_seen == 1;
}

fn operationsCleanupVerifiedArchiveReads(archive: Str) bool {
    var data_buf: [PATH_CAP]u8 = undefined;
    const data = operationsPath(&data_buf, "{s}/data.zova", .{span(archive)}) orelse return false;
    return operationsRemoveClosedDatabaseSidecars(data) and operationsArchiveHasExactMembers(archive);
}

/// The id of the only workspace in `data_path`, which must be ready.
fn operationsSingleReadyWorkspace(data_path: Str, out: *[ID_MAX]u8) bool {
    const db = operationsOpenReadOnly(data_path) orelse return false;
    defer _ = c.zova_database_close(db);
    const statement = operationsPrepare(
        db,
        "SELECT r.workspace_id FROM cbm_workspace_registry r " ++
            "JOIN cbm_database_generation_v1 g ON g.workspace_key=r.workspace_key " ++
            "AND g.generation=r.active_generation AND g.state='ready' " ++
            "JOIN cbm_workspace_index_state_v1 s ON s.workspace_key=r.workspace_key " ++
            "AND s.generation=r.active_generation ORDER BY r.workspace_id",
        null,
    ) orelse return false;
    defer _ = c.zova_statement_finalize(statement);
    var workspace = std.mem.zeroes(c.zova_text);
    defer c.zova_text_free(&workspace);
    if (!operationsStepIs(statement, c.ZOVA_STEP_ROW) or
        !operationsColumnText(statement, 0, &workspace) or workspace.len >= ID_MAX)
        return false;
    @memcpy(out[0..workspace.len], workspace.data[0..workspace.len]);
    out[workspace.len] = 0;
    return operationsStepIs(statement, c.ZOVA_STEP_DONE) and c.cbm_zova_workspace_id_validate(out) == 0;
}

fn operationsWorkspaceCount(path: Str, out: *i64) bool {
    const db = operationsOpenReadOnly(path) orelse return false;
    defer _ = c.zova_database_close(db);
    return operationsQueryI64(db, workspace_count_sql, out);
}

fn operationsGenerationMatches(actual: *const GenerationResult, expected: *const GenerationResult) bool {
    return actual.generation == expected.generation and
        bufEql(&actual.workspace_id, &expected.workspace_id) and
        actual.graph_nodes == expected.graph_nodes and
        actual.graph_edges == expected.graph_edges and
        actual.metadata_nodes == expected.metadata_nodes and
        actual.metadata_edges == expected.metadata_edges and
        actual.metadata_topology_edges == expected.metadata_topology_edges and
        actual.fts_rows == expected.fts_rows and
        actual.node_vector_rows == expected.node_vector_rows and
        actual.token_vector_rows == expected.token_vector_rows and
        actual.node_vectors == expected.node_vectors and
        actual.token_vectors == expected.token_vectors and
        bufEql(&actual.metadata_sha256, &expected.metadata_sha256) and
        bufEql(&actual.fts_sha256, &expected.fts_sha256) and
        bufEql(&actual.topology_sha256, &expected.topology_sha256) and
        bufEql(&actual.node_vector_sha256, &expected.node_vector_sha256) and
        bufEql(&actual.token_vector_sha256, &expected.token_vector_sha256);
}

/// The publish input that reproduces `snapshot`. It borrows the snapshot's
/// arrays.
fn operationsSnapshotInput(snapshot: *const Snapshot) GenerationInput {
    return .{
        .root_path = snapshot.root_path,
        .project = snapshot.project,
        .indexed_at = snapshot.indexed_at,
        .model_fingerprint = snapshot.model_fingerprint,
        .vector_dimensions = snapshot.vector_dimensions,
        .nodes = snapshot.nodes,
        .node_count = snapshot.node_count,
        .edges = snapshot.edges,
        .edge_count = snapshot.edge_count,
        .node_vectors = snapshot.node_vectors,
        .node_vector_count = snapshot.node_vector_count,
        .token_vectors = snapshot.token_vectors,
        .token_vector_count = snapshot.token_vector_count,
        .file_hashes = snapshot.file_hashes,
        .file_hash_count = snapshot.file_hash_count,
        .project_summary = snapshot.project_summary,
    };
}

const ArchiveKind = enum { database, workspace };
const ArchiveCheck = enum { valid, malformed, incompatible, path_alias };

/// What both archive kinds are checked for first.
const ArchiveContents = struct {
    /// manifest.json, malloc'd; the caller frees it.
    text: [*:0]u8,
    /// Bytes of `text` the manifest header takes.
    consumed: usize,
    manifest: ArchiveManifest,
};

/// The checks both archive kinds share: the directory layout, the manifest
/// header and its versions, and data.zova against the recorded digest. On
/// `.valid`, `data_buf` holds the path of data.zova and `out` the manifest.
fn operationsReadArchive(kind: ArchiveKind, archive: Str, live_path: ?Str, data_buf: *[PATH_CAP]u8, out: *ArchiveContents) ArchiveCheck {
    const value = operationsLstat(archive) orelse return .malformed;
    if (!operationsIsDirectory(&value)) return .malformed;
    if (!operationsArchiveHasExactMembers(archive)) return .malformed;
    var manifest_buf: [PATH_CAP]u8 = undefined;
    const data = operationsPath(data_buf, "{s}/data.zova", .{span(archive)}) orelse return .malformed;
    const manifest_path = operationsPath(&manifest_buf, "{s}/manifest.json", .{span(archive)}) orelse return .malformed;
    if (!operationsPathIsRegularNoSymlink(data) or !operationsPathIsRegularNoSymlink(manifest_path))
        return .malformed;
    if (live_path) |live| {
        if (operationsRestorePathState(live) == .regular and operationsSameFile(data, live)) return .path_alias;
    }
    const text = operationsReadFile(manifest_path) orelse return .malformed;
    var keep_text = false;
    defer if (!keep_text) c.free(text);
    var parsed = ArchiveManifest{};
    var bytes: c_ulonglong = 0;
    var consumed: c_int = 0;
    const fields = c.sscanf(
        text,
        switch (kind) {
            .database => "{\"archive_version\":%d,\"schema_version\":%d," ++
                "\"database\":{\"path\":\"data.zova\",\"bytes\":%llu," ++
                "\"sha256\":\"%64[0-9a-f]\"},\"workspaces\":[%n",
            .workspace => "{\"archive_version\":%d,\"schema_version\":%d," ++
                "\"archive_kind\":\"workspace\"," ++
                "\"database\":{\"path\":\"data.zova\",\"bytes\":%llu," ++
                "\"sha256\":\"%64[0-9a-f]\"},\"workspace\":%n",
        },
        &parsed.archive_version,
        &parsed.schema_version,
        &bytes,
        @as([*c]u8, &parsed.database_sha256),
        &consumed,
    );
    parsed.database_bytes = bytes;
    if (fields != 4 or (kind == .workspace and consumed <= 0) or
        sliceTo(&parsed.database_sha256, 0).len != HEX_LEN)
        return .malformed;
    if (parsed.archive_version != ARCHIVE_VERSION or parsed.schema_version != SCHEMA_VERSION)
        return .incompatible;
    var digest: [HEX_LEN + 1]u8 = undefined;
    var actual_bytes: u64 = 0;
    if (!operationsFileSha256(data, &digest, &actual_bytes) or
        actual_bytes != parsed.database_bytes or !bufEql(&digest, &parsed.database_sha256) or
        !operationsVerifyDatabase(data, null))
        return .malformed;
    keep_text = true;
    out.* = .{ .text = text, .consumed = @intCast(consumed), .manifest = parsed };
    return .valid;
}

/// A database archive: the manifest lists exactly the workspaces of
/// data.zova, with their digests.
fn operationsVerifyArchive(archive: Str, live_path: ?Str, out: ?*ArchiveManifest) ArchiveCheck {
    var data_buf: [PATH_CAP]u8 = undefined;
    var contents: ArchiveContents = undefined;
    const check = operationsReadArchive(.database, archive, live_path, &data_buf, &contents);
    if (check != .valid) return check;
    defer c.free(contents.text);
    const db = operationsOpenReadOnly(@ptrCast(&data_buf)) orelse return .malformed;
    defer _ = c.zova_database_close(db);
    const statement = operationsWorkspaceManifestQuery(db) orelse return .malformed;
    defer _ = c.zova_statement_finalize(statement);
    var cursor: []const u8 = span(contents.text)[contents.consumed..];
    var first = true;
    while (true) {
        var step: c.zova_step_result = c.ZOVA_STEP_DONE;
        if (!operationsStep(statement, &step)) return .malformed;
        if (step == c.ZOVA_STEP_DONE) break;
        var entry_buf: [1024]u8 = undefined;
        const entry = operationsWorkspaceManifestEntry(statement, &entry_buf) orelse return .malformed;
        if (!first) {
            if (!std.mem.startsWith(u8, cursor, ",")) return .malformed;
            cursor = cursor[1..];
        }
        if (!std.mem.startsWith(u8, cursor, entry)) return .malformed;
        cursor = cursor[entry.len..];
        first = false;
    }
    if (!std.mem.eql(u8, cursor, "]}\n")) return .malformed;
    if (out) |manifest| manifest.* = contents.manifest;
    return .valid;
}

/// A workspace archive: data.zova holds one ready workspace and the manifest
/// is exactly the one that workspace produces.
fn operationsVerifyWorkspaceArchive(archive: Str, live_path: ?Str, out: ?*ArchiveManifest) ArchiveCheck {
    var data_buf: [PATH_CAP]u8 = undefined;
    var contents: ArchiveContents = undefined;
    const check = operationsReadArchive(.workspace, archive, live_path, &data_buf, &contents);
    if (check != .valid) return check;
    defer c.free(contents.text);
    const data: Str = @ptrCast(&data_buf);
    var workspace_id: [ID_MAX]u8 = @splat(0);
    var snapshot = std.mem.zeroes(Snapshot);
    defer c.cbm_zova_workspace_snapshot_free(&snapshot);
    if (!operationsSingleReadyWorkspace(data, &workspace_id) or
        c.cbm_zova_repository_export_snapshot(data, &workspace_id, &snapshot) != 0)
        return .malformed;
    const expected = operationsWorkspaceArchiveManifestText(&contents.manifest, &snapshot) orelse return .malformed;
    defer c.free(expected);
    if (!std.mem.eql(u8, span(contents.text), span(expected))) return .malformed;
    if (out) |manifest| manifest.* = contents.manifest;
    return .valid;
}

fn operationsRemovePartialArchive(partial: Str) void {
    var buf: [PATH_CAP]u8 = undefined;
    if (operationsPath(&buf, "{s}/manifest.json", .{span(partial)})) |path| _ = c.cbm_unlink(path);
    if (operationsPath(&buf, "{s}/data.zova", .{span(partial)})) |path| {
        _ = operationsRemoveClosedDatabaseSidecars(path);
        _ = operationsRemoveClosedWriterLock(path);
        _ = c.cbm_unlink(path);
    }
    _ = c.rmdir(partial);
}

// ── Status ──────────────────────────────────────────────────────

pub export fn cbm_zova_operation_code_name(code: Code) Str {
    return switch (code) {
        OK => "ok",
        NOOP => "noop",
        INVALID => "invalid",
        BUSY => "busy",
        INCOMPATIBLE => "incompatible",
        VERIFY_FAILED => "verify_failed",
        DISK_REFUSED => "disk_refused",
        CONFIRMATION_REQUIRED => "confirmation_required",
        WORKSPACE_REBUILD_REQUIRED => "workspace_rebuild_required",
        WHOLE_FILE_RECOVERY_REQUIRED => "whole_file_recovery_required",
        else => "unknown",
    };
}

pub export fn cbm_zova_database_status(path: ?Str, out: ?*Report) Code {
    _ = operationsReport(out, INVALID, "status", "invalid_argument");
    const live = nonEmpty(path) orelse return INVALID;
    const report = out orelse return INVALID;
    if (!with_zova) return operationsReport(report, INCOMPATIBLE, "status", "zova_disabled");

    const database_size = if (operationsPathIsRegularNoSymlink(live)) operationsFileSize(live) else null;
    if (database_size == null or database_size.? < 0)
        return operationsReport(report, INVALID, "status", "database_not_regular");
    var wal_buf: [PATH_CAP]u8 = undefined;
    const wal_path = operationsPath(&wal_buf, "{s}-wal", .{span(live)}) orelse
        return operationsReport(report, INVALID, "status", "path_too_long");
    var wal_bytes: u64 = 0;
    if (operationsLstat(wal_path)) |wal_stat| {
        if (!operationsIsRegular(&wal_stat) or wal_stat.st_size < 0)
            return operationsReport(report, INVALID, "status", "wal_not_regular");
        wal_bytes = operationsBytes(wal_stat.st_size);
    }
    const free_bytes = operationsFreeBytes(live) catch |err|
        return operationsReport(report, VERIFY_FAILED, "status", switch (err) {
            error.Unavailable => "free_space_unavailable",
            error.Overflow => "free_space_overflow",
        });

    const started = operationsNow();
    const db = operationsOpenReadOnly(live) orelse
        return operationsReport(report, VERIFY_FAILED, "status", "database_open_failed");
    var schema_version: i64 = 0;
    var invalid_generations: i64 = 0;
    var page_size: i64 = -1;
    var page_count: i64 = -1;
    var freelist_count: i64 = -1;
    const schema_read = operationsQueryI64(db, schema_version_sql, &schema_version);
    if (!schema_read or schema_version != SCHEMA_VERSION) {
        _ = c.zova_database_close(db);
        if (schema_read and schema_version > SCHEMA_VERSION) {
            _ = operationsReport(report, INCOMPATIBLE, "status", "schema_version_incompatible");
            report.schema_version = std.math.cast(c_int, schema_version) orelse 0;
            return INCOMPATIBLE;
        }
        return operationsReport(report, VERIFY_FAILED, "status", "current_schema_required");
    }
    const verified = c.cbm_zova_user_database_schema_is_current(db) == 0 and
        operationsQuickCheck(db) and operationsForeignKeysOk(db) and
        operationsQueryI64(db, invalid_generations_sql, &invalid_generations) and
        invalid_generations == 0 and
        operationsQueryI64(db, "PRAGMA page_size", &page_size) and
        operationsQueryI64(db, "PRAGMA page_count", &page_count) and
        operationsQueryI64(db, "PRAGMA freelist_count", &freelist_count);
    _ = c.zova_database_close(db);
    if (!verified or page_size <= 0 or page_count < 0 or freelist_count < 0)
        return operationsReport(report, VERIFY_FAILED, "status", "database_verification_failed");
    const unsigned_page_size: u64 = @intCast(page_size);
    const unsigned_freelist_count: u64 = @intCast(freelist_count);
    const reclaimable_bytes = std.math.mul(u64, unsigned_page_size, unsigned_freelist_count) catch
        return operationsReport(report, VERIFY_FAILED, "status", "reclaimable_bytes_overflow");
    const finished = operationsNow();
    _ = operationsReport(report, OK, "status", "ok");
    report.schema_version = @intCast(schema_version);
    report.database_bytes = operationsBytes(database_size.?);
    report.wal_bytes = wal_bytes;
    report.free_bytes = free_bytes;
    report.page_size = unsigned_page_size;
    report.page_count = @intCast(page_count);
    report.freelist_count = unsigned_freelist_count;
    report.reclaimable_bytes = reclaimable_bytes;
    report.elapsed_ms = operationsElapsedMs(started, finished);
    return OK;
}

// ── Workspace delete ────────────────────────────────────────────

fn workspaceDeleteBody(path: ?Str, workspace_id: ?Str, confirmation: ?Str, out: ?*Report) Code {
    _ = operationsReport(out, INVALID, "delete_workspace", "invalid_argument");
    const live = nonEmpty(path) orelse return INVALID;
    const workspace = nonEmpty(workspace_id) orelse return INVALID;
    const report = out orelse return INVALID;
    const confirmed = nonEmpty(confirmation) orelse
        return operationsReport(report, CONFIRMATION_REQUIRED, "delete_workspace", "confirmation_required");
    if (!std.mem.eql(u8, span(workspace), span(confirmed)))
        return operationsReport(report, CONFIRMATION_REQUIRED, "delete_workspace", "confirmation_mismatch");
    if (c.cbm_zova_workspace_id_validate(workspace) != 0) return INVALID;
    if (!with_zova) return operationsReport(report, INCOMPATIBLE, "delete_workspace", "zova_disabled");

    const started = operationsNow();
    var before = std.mem.zeroes(Report);
    const status = cbm_zova_database_status(live, &before);
    if (status != OK) return operationsReport(report, status, "delete_workspace", "status_failed");
    var snapshot = std.mem.zeroes(Snapshot);
    if (c.cbm_zova_repository_export_snapshot(live, workspace, &snapshot) != 0) {
        const finished = operationsNow();
        _ = operationsReport(report, NOOP, "delete_workspace", "already_absent");
        operationsSetWorkspaceId(report, workspace);
        operationsCopyStatusMetrics(report, &before);
        report.reclaimable_bytes = 0;
        report.elapsed_ms = operationsElapsedMs(started, finished);
        return NOOP;
    }
    const generation = snapshot.generation;
    c.cbm_zova_workspace_snapshot_free(&snapshot);
    if (c.cbm_zova_user_database_delete_workspace(live, workspace) != 0)
        return operationsReport(report, VERIFY_FAILED, "delete_workspace", "delete_failed");
    var after = std.mem.zeroes(Report);
    if (cbm_zova_database_status(live, &after) != OK)
        return operationsReport(report, VERIFY_FAILED, "delete_workspace", "post_delete_status_failed");
    const finished = operationsNow();
    _ = operationsReport(report, OK, "delete_workspace", "ok");
    operationsSetWorkspaceId(report, workspace);
    report.generation = generation;
    operationsCopyStatusMetrics(report, &after);
    report.reclaimable_bytes = after.reclaimable_bytes -| before.reclaimable_bytes;
    report.elapsed_ms = operationsElapsedMs(started, finished);
    return OK;
}

// ── Database replacement ────────────────────────────────────────

fn operationsCheckpointTruncate(db: *Db) bool {
    const statement = operationsPrepare(db, "PRAGMA wal_checkpoint(TRUNCATE)", null) orelse return false;
    defer _ = c.zova_statement_finalize(statement);
    var busy: i64 = -1;
    var log_frames: i64 = -1;
    var checkpointed_frames: i64 = -1;
    return operationsStepIs(statement, c.ZOVA_STEP_ROW) and
        operationsColumnI64(statement, 0, &busy) and
        operationsColumnI64(statement, 1, &log_frames) and
        operationsColumnI64(statement, 2, &checkpointed_frames) and
        busy == 0 and log_frames == checkpointed_frames;
}

fn operationsRawSqliteCheckpointTruncate(path: Str) bool {
    var db: ?*c.sqlite3 = null;
    var log_frames: c_int = 0;
    var checkpointed_frames: c_int = 0;
    var rc = c.sqlite3_open_v2(path, &db, c.SQLITE_OPEN_READWRITE, null);
    if (rc == c.SQLITE_OK) _ = c.sqlite3_busy_timeout(db, 5000);
    if (rc == c.SQLITE_OK)
        rc = c.sqlite3_wal_checkpoint_v2(db, null, c.SQLITE_CHECKPOINT_TRUNCATE, &log_frames, &checkpointed_frames);
    if (db != null) _ = c.sqlite3_close(db);
    return rc == c.SQLITE_OK and checkpointed_frames == log_frames;
}

/// Compaction is skipped when it would reclaim less than 64 MiB and less
/// than a tenth of the file.
fn operationsCompactBelowThreshold(status: *const Report) bool {
    const absolute_threshold: u64 = 64 * 1024 * 1024;
    const below_absolute = status.reclaimable_bytes < absolute_threshold;
    const quotient = status.database_bytes / 10;
    const remainder = status.database_bytes % 10;
    const below_ratio = status.reclaimable_bytes < quotient or
        (status.reclaimable_bytes == quotient and remainder != 0);
    return below_absolute and below_ratio;
}

/// Which of the three files of a same-directory replacement exist.
const ReplacementFiles = struct {
    live: PathState,
    temporary: PathState,
    recovery: PathState,

    fn read(live: Str, temporary: Str, recovery: Str) ReplacementFiles {
        return .{
            .live = operationsRestorePathState(live),
            .temporary = operationsRestorePathState(temporary),
            .recovery = operationsRestorePathState(recovery),
        };
    }

    fn are(self: ReplacementFiles, live: PathState, temporary: PathState, recovery: PathState) bool {
        return self.live == live and self.temporary == temporary and self.recovery == recovery;
    }

    /// The states a replacement starts or resumes from: not started, the
    /// temporary written, the live file moved to recovery, the temporary
    /// moved to live.
    fn resumable(self: ReplacementFiles) bool {
        return self.are(.regular, .absent, .absent) or self.are(.regular, .regular, .absent) or
            self.are(.absent, .regular, .regular) or self.are(.regular, .absent, .regular);
    }

    fn anyOther(self: ReplacementFiles) bool {
        return self.live == .other or self.temporary == .other or self.recovery == .other;
    }
};

const ReplacementOptions = struct {
    operation: []const u8,
    after_temp_verification: []const u8,
    after_live_to_recovery: []const u8,
    after_temp_to_live: []const u8,
    before_recovery_cleanup: []const u8,
    remove_sidecars: bool,
    require_source_verified: bool,
    keep_recovery: bool = false,
};

/// `interrupted` is a test fault, which has already filled the report.
const Replacement = enum { done, failed, interrupted };

/// Complete or resume a verified same-directory database replacement.
fn operationsReplaceVerifiedDatabase(
    live: Str,
    temporary: Str,
    recovery: Str,
    parent: Str,
    options: *const ReplacementOptions,
    out_schema_version: ?*c_int,
    out: ?*Report,
) Replacement {
    var files = ReplacementFiles.read(live, temporary, recovery);
    if (!files.are(.regular, .regular, .absent) and !files.are(.absent, .regular, .regular) and
        !files.are(.regular, .absent, .regular))
        return .failed;

    var schema_version: c_int = 0;
    if (files.are(.regular, .regular, .absent)) {
        if (!operationsVerifyDatabase(temporary, &schema_version)) return .failed;
        if (operationsReplacementFault(options.operation, options.after_temp_verification, out))
            return .interrupted;
        if ((options.require_source_verified and !operationsVerifyDatabase(live, null)) or
            (options.remove_sidecars and
                (!operationsRemoveClosedDatabaseSidecars(live) or
                    !operationsRemoveClosedDatabaseSidecars(temporary))) or
            c.rename(live, recovery) != 0 or !operationsSyncDirectory(parent))
            return .failed;
        files.live = .absent;
        files.recovery = .regular;
        if (operationsReplacementFault(options.operation, options.after_live_to_recovery, out))
            return .interrupted;
    }
    if (files.are(.absent, .regular, .regular)) {
        if (!operationsVerifyDatabase(temporary, &schema_version) or
            (options.require_source_verified and !operationsVerifyDatabase(recovery, null)) or
            (options.remove_sidecars and
                (!operationsRemoveClosedDatabaseSidecars(temporary) or
                    !operationsRemoveClosedDatabaseSidecars(recovery) or
                    !operationsRemoveClosedDatabaseSidecars(live))) or
            c.rename(temporary, live) != 0 or !operationsSyncDirectory(parent))
            return .failed;
        files.live = .regular;
        files.temporary = .absent;
        if (operationsReplacementFault(options.operation, options.after_temp_to_live, out))
            return .interrupted;
    }
    if (!files.are(.regular, .absent, .regular) or !operationsVerifyDatabase(live, &schema_version))
        return .failed;
    if (operationsReplacementFault(options.operation, options.before_recovery_cleanup, out))
        return .interrupted;
    if (!options.keep_recovery and
        ((options.remove_sidecars and !operationsRemoveClosedDatabaseSidecars(recovery)) or
            c.cbm_unlink(recovery) != 0 or !operationsSyncDirectory(parent)))
        return .failed;
    if (out_schema_version) |version| version.* = schema_version;
    return .done;
}

// ── Compact ─────────────────────────────────────────────────────

pub export fn cbm_zova_database_compact(path: ?Str, out: ?*Report) Code {
    _ = operationsReport(out, INVALID, "compact", "invalid_argument");
    const live = nonEmpty(path) orelse return INVALID;
    const report = out orelse return INVALID;
    if (!with_zova) return INCOMPATIBLE;

    var temporary_buf: [PATH_CAP]u8 = undefined;
    var recovery_buf: [PATH_CAP]u8 = undefined;
    var parent_buf: [PATH_CAP]u8 = undefined;
    const stem = operationsZovaStem(span(live)) orelse return INVALID;
    const temporary = operationsPath(&temporary_buf, "{s}.compact.tmp.zova", .{stem}) orelse return INVALID;
    const recovery = operationsPath(&recovery_buf, "{s}.compact.recovery.zova", .{stem}) orelse return INVALID;
    const parent = operationsParentDirectory(span(live), &parent_buf) orelse return INVALID;

    var guard = std.mem.zeroes(WriterGuard);
    if (c.cbm_zova_writer_guard_try_acquire(live, &guard) != 0)
        return operationsReport(report, BUSY, "compact", "writer_busy");
    defer c.cbm_zova_writer_guard_release(&guard);
    const started = operationsNow();
    const files = ReplacementFiles.read(live, temporary, recovery);
    if (!files.resumable())
        return operationsReport(report, VERIFY_FAILED, "compact", "invalid_recovery_state");

    if (files.are(.regular, .absent, .absent)) {
        var before = std.mem.zeroes(Report);
        const status_code = cbm_zova_database_status(live, &before);
        if (status_code != OK) return operationsReport(report, status_code, "compact", "status_failed");
        if (operationsCompactBelowThreshold(&before)) {
            _ = operationsReport(report, NOOP, "compact", "below_threshold");
            operationsCopyStatusMetrics(report, &before);
            report.reclaimable_bytes = before.reclaimable_bytes;
            return NOOP;
        }
        const free_override: [*c]const u8 = c.getenv("CBM_ZOVA_TEST_COMPACT_FREE_BYTES");
        if (free_override != null and free_override[0] != 0) {
            var overridden: c_ulonglong = 0;
            var trailing: u8 = 0;
            if (c.sscanf(free_override, "%llu%c", &overridden, &trailing) != 1)
                return operationsReport(report, VERIFY_FAILED, "compact", "invalid_test_free_bytes");
            before.free_bytes = overridden;
        }
        const reserve: u64 = 1024 * 1024 * 1024;
        const guard_bytes: u64 = 8 * 1024 * 1024 * 1024;
        const with_reserve = std.math.add(u64, before.database_bytes, reserve) catch
            return operationsReport(report, DISK_REFUSED, "compact", "required_space_overflow");
        if (before.free_bytes < @max(with_reserve, guard_bytes)) {
            _ = operationsReport(report, DISK_REFUSED, "compact", "insufficient_disk");
            operationsCopyStatusMetrics(report, &before);
            report.reclaimable_bytes = before.reclaimable_bytes;
            return DISK_REFUSED;
        }

        const compacted = if (operationsOpen(live, 0, 0, null)) |db| compacted: {
            defer _ = c.zova_database_close(db);
            // Zova sets auto_vacuum on the source connection before copying,
            // which commits a page-1 write to the WAL. Checkpoint again so the
            // sidecar removal below never drops a committed frame.
            break :compacted operationsCheckpointTruncate(db) and
                c.zova_database_compact(&c.zova_database_compact_request{
                    .db = db,
                    .destination_path = temporary,
                    .flags = 0,
                }) == c.ZOVA_OK and
                operationsCheckpointTruncate(db);
        } else false;
        if (!compacted) {
            _ = operationsRemoveClosedDatabaseSidecars(temporary);
            _ = c.cbm_unlink(temporary);
            return operationsReport(report, BUSY, "compact", "checkpoint_or_compact_busy");
        }
        if (!operationsRemoveClosedDatabaseSidecars(live) or
            !operationsRemoveClosedDatabaseSidecars(temporary))
            return operationsReport(report, VERIFY_FAILED, "compact", "sidecar_cleanup_failed");
        if (operationsReplacementFault("compact", "compact_after_temp_creation", report))
            return VERIFY_FAILED;
    }

    const replacement = ReplacementOptions{
        .operation = "compact",
        .after_temp_verification = "compact_after_temp_verification",
        .after_live_to_recovery = "compact_after_live_to_recovery",
        .after_temp_to_live = "compact_after_temp_to_live",
        .before_recovery_cleanup = "compact_before_recovery_cleanup",
        .remove_sidecars = true,
        .require_source_verified = true,
    };
    var after = std.mem.zeroes(Report);
    switch (operationsReplaceVerifiedDatabase(live, temporary, recovery, parent, &replacement, null, report)) {
        .interrupted => return VERIFY_FAILED,
        .failed => return operationsReport(report, VERIFY_FAILED, "compact", "replacement_failed"),
        .done => {},
    }
    if (cbm_zova_database_status(live, &after) != OK)
        return operationsReport(report, VERIFY_FAILED, "compact", "replacement_failed");
    const finished = operationsNow();
    _ = operationsReport(report, OK, "compact", "ok");
    operationsCopyStatusMetrics(report, &after);
    report.reclaimable_bytes = after.reclaimable_bytes;
    report.elapsed_ms = operationsElapsedMs(started, finished);
    return OK;
}

// ── Format migration ────────────────────────────────────────────

/// Zova 1.2.0 stages a migration in "<destination>.migration-recovery/" and
/// removes it on success, but for a WAL database the staged copy's empty -wal
/// and -shm stay behind and keep the directory alive. Remove exactly those.
fn operationsRemoveMigrationStaging(destination: Str) void {
    const path = span(destination);
    const name = if (std.mem.lastIndexOfScalar(u8, path, '/')) |slash| path[slash + 1 ..] else path;
    var directory: [PATH_CAP + 32]u8 = undefined;
    var sidecar: [PATH_CAP + 32 + 512]u8 = undefined;
    _ = interop.bprint(&directory, "{s}.migration-recovery", .{path});
    const staging = sliceTo(&directory, 0);
    _ = interop.bprint(&sidecar, "{s}/{s}-wal", .{ staging, name });
    _ = c.cbm_unlink(&sidecar);
    _ = interop.bprint(&sidecar, "{s}/{s}-shm", .{ staging, name });
    _ = c.cbm_unlink(&sidecar);
    _ = c.cbm_rmdir(&directory);
}

pub export fn cbm_zova_database_migrate_format(path: ?Str, out: ?*Report) Code {
    const operation = "migrate-format";
    _ = operationsReport(out, INVALID, operation, "invalid_argument");
    const live = nonEmpty(path) orelse return INVALID;
    const report = out orelse return INVALID;
    if (!with_zova) return INCOMPATIBLE;

    var temporary_buf: [PATH_CAP]u8 = undefined;
    var recovery_buf: [PATH_CAP]u8 = undefined;
    var parent_buf: [PATH_CAP]u8 = undefined;
    const stem = operationsZovaStem(span(live)) orelse return INVALID;
    const temporary = operationsPath(&temporary_buf, "{s}.format.tmp.zova", .{stem}) orelse return INVALID;
    const recovery = operationsPath(&recovery_buf, "{s}.format.recovery.zova", .{stem}) orelse return INVALID;
    const parent = operationsParentDirectory(span(live), &parent_buf) orelse return INVALID;

    var guard = std.mem.zeroes(WriterGuard);
    if (c.cbm_zova_writer_guard_try_acquire(live, &guard) != 0)
        return operationsReport(report, BUSY, operation, "writer_busy");
    defer c.cbm_zova_writer_guard_release(&guard);
    const files = ReplacementFiles.read(live, temporary, recovery);
    if (!files.resumable())
        return operationsReport(report, VERIFY_FAILED, operation, "invalid_recovery_state");

    if (files.are(.regular, .absent, .absent)) {
        const format = c.cbm_zova_database_format_status(live);
        if (format == c.CBM_ZOVA_DATABASE_COMPATIBLE)
            return operationsReport(report, NOOP, operation, "current_format");
        if (format != c.CBM_ZOVA_DATABASE_MIGRATION_REQUIRED)
            return operationsReport(report, INCOMPATIBLE, operation, "unsupported_format");
        // Zova writes the migrated copy next to the source, so the directory
        // briefly holds both files.
        const reserve: u64 = 1024 * 1024 * 1024;
        const source_stat = operationsLstat(live);
        const free_bytes = if (source_stat != null and source_stat.?.st_size >= 0)
            operationsFreeBytes(live) catch null
        else
            null;
        if (free_bytes == null)
            return operationsReport(report, VERIFY_FAILED, operation, "free_space_unavailable");
        if (free_bytes.? < operationsBytes(source_stat.?.st_size) + reserve)
            return operationsReport(report, DISK_REFUSED, operation, "insufficient_disk");
        // The replacement drops the source sidecars, so the WAL must be empty.
        if (!operationsRawSqliteCheckpointTruncate(live))
            return operationsReport(report, BUSY, operation, "checkpoint_busy");
        var message = std.mem.zeroes(c.zova_message);
        const migrated = c.zova_database_migrate(&c.zova_database_migrate_request{
            .source_path = live,
            .destination_path = temporary,
            .flags = 0,
            .out_error_message = &message,
        });
        if (migrated != c.ZOVA_OK) {
            interop.log(c.CBM_LOG_ERROR, "zova.migrate_format", .{
                "path",  live,
                "error", if (message.data != null) @as(Str, @ptrCast(message.data)) else "unknown",
            });
            c.zova_message_free(&message);
            _ = operationsRemoveClosedDatabaseSidecars(temporary);
            _ = c.cbm_unlink(temporary);
            return operationsReport(report, VERIFY_FAILED, operation, "zova_migrate_failed");
        }
        c.zova_message_free(&message);
        operationsRemoveMigrationStaging(temporary);
        if (operationsReplacementFault(operation, "migrate_format_after_temp_creation", report))
            return report.code;
    }

    const replacement = ReplacementOptions{
        .operation = operation,
        .after_temp_verification = "migrate_format_after_temp_verification",
        .after_live_to_recovery = "migrate_format_after_live_to_recovery",
        .after_temp_to_live = "migrate_format_after_temp_to_live",
        .before_recovery_cleanup = "migrate_format_before_recovery_cleanup",
        .remove_sidecars = true,
        // The current SDK cannot open the old-format source to verify it.
        .require_source_verified = false,
    };
    var after = std.mem.zeroes(Report);
    switch (operationsReplaceVerifiedDatabase(live, temporary, recovery, parent, &replacement, null, report)) {
        .interrupted => return report.code,
        .failed => return operationsReport(report, VERIFY_FAILED, operation, "replacement_failed"),
        .done => {},
    }
    if (cbm_zova_database_status(live, &after) != OK)
        return operationsReport(report, VERIFY_FAILED, operation, "replacement_failed");
    _ = operationsReport(report, OK, operation, "ok");
    operationsCopyStatusMetrics(report, &after);
    return OK;
}

// ── Health and recovery ─────────────────────────────────────────

fn operationsWorkspaceHealthRow(db: *Db, workspace_id: Str, out_generation: *i64, out_rebuild: *bool) bool {
    const statement = operationsPrepare(
        db,
        "SELECT r.active_generation,COALESCE(h.state,'healthy') " ++
            "FROM cbm_workspace_registry r LEFT JOIN cbm_workspace_health_v1 h " ++
            "ON h.workspace_key=r.workspace_key WHERE r.workspace_id=?1",
        null,
    ) orelse return false;
    defer _ = c.zova_statement_finalize(statement);
    if (!operationsBindText(statement, 1, workspace_id)) return false;
    var state = std.mem.zeroes(c.zova_text);
    defer c.zova_text_free(&state);
    if (!operationsStepIs(statement, c.ZOVA_STEP_ROW) or
        !operationsColumnI64(statement, 0, out_generation) or
        !operationsColumnText(statement, 1, &state))
        return false;
    out_rebuild.* = std.mem.eql(u8, state.data[0..state.len], "rebuild_required");
    return true;
}

fn operationsMarkWorkspaceRebuildRequired(path: Str, workspace_id: Str, generation: i64, reason: Str) bool {
    var guard = std.mem.zeroes(WriterGuard);
    if (c.cbm_zova_writer_guard_acquire(path, &guard) != 0) return false;
    defer c.cbm_zova_writer_guard_release(&guard);
    var opened: ?*Db = null;
    var message = std.mem.zeroes(c.zova_message);
    const open_status = c.zova_database_open(&c.zova_database_open_request{
        .path = path,
        .out_db = &opened,
        .out_error_message = &message,
    });
    c.zova_message_free(&message);
    const db = opened orelse return false;
    defer _ = c.zova_database_close(db);
    const transaction = c.zova_database_simple_request{ .db = db };
    var ok = open_status == c.ZOVA_OK and c.zova_database_begin_immediate(&transaction) == c.ZOVA_OK;
    if (ok) ok = written: {
        const statement = operationsPrepare(
            db,
            "INSERT INTO cbm_workspace_health_v1" ++
                "(workspace_key,state,reason,checked_generation,checked_at) " ++
                "SELECT workspace_key,'rebuild_required',?2,?3," ++
                "strftime('%Y-%m-%dT%H:%M:%fZ','now') " ++
                "FROM cbm_workspace_registry WHERE workspace_id=?1 " ++
                "ON CONFLICT(workspace_key) DO UPDATE SET " ++
                "state=excluded.state,reason=excluded.reason," ++
                "checked_generation=excluded.checked_generation," ++
                "checked_at=excluded.checked_at",
            null,
        ) orelse break :written false;
        defer _ = c.zova_statement_finalize(statement);
        break :written operationsBindText(statement, 1, workspace_id) and
            operationsBindText(statement, 2, reason) and
            operationsBindI64(statement, 3, generation) and
            operationsStepIs(statement, c.ZOVA_STEP_DONE);
    };
    if (ok) {
        ok = c.zova_database_commit(&transaction) == c.ZOVA_OK;
    } else {
        _ = c.zova_database_rollback(&transaction);
    }
    return ok;
}

fn operationsWorkspaceProject(path: Str, workspace_id: Str, out: []u8) bool {
    const db = operationsOpenReadOnly(path) orelse return false;
    defer _ = c.zova_database_close(db);
    const statement = operationsPrepare(
        db,
        "SELECT p.project FROM cbm_projects_v1 p " ++
            "JOIN cbm_workspace_registry r USING(workspace_key) " ++
            "WHERE r.workspace_id=?1",
        null,
    ) orelse return false;
    defer _ = c.zova_statement_finalize(statement);
    if (!operationsBindText(statement, 1, workspace_id)) return false;
    var project = std.mem.zeroes(c.zova_text);
    defer c.zova_text_free(&project);
    return operationsStepIs(statement, c.ZOVA_STEP_ROW) and
        operationsColumnText(statement, 0, &project) and operationsTextInto(project, out);
}

/// Moves a damaged database and its -wal and -shm aside as
/// "<stem>.corrupt.zova", under the writer gate.
fn operationsQuarantineDatabase(path: Str) bool {
    var quarantine_buf: [PATH_CAP]u8 = undefined;
    var parent_buf: [PATH_CAP]u8 = undefined;
    const stem = operationsZovaStem(span(path)) orelse return false;
    const quarantine = operationsPath(&quarantine_buf, "{s}.corrupt.zova", .{stem}) orelse return false;
    const parent = operationsParentDirectory(span(path), &parent_buf) orelse return false;
    if (operationsRestorePathState(path) != .regular or operationsRestorePathState(quarantine) != .absent)
        return false;
    var source_bufs: [2][PATH_CAP]u8 = undefined;
    var quarantine_bufs: [2][PATH_CAP]u8 = undefined;
    var source_sidecars: [2]Str = undefined;
    var quarantine_sidecars: [2]Str = undefined;
    var present = [2]bool{ false, false };
    for ([_][]const u8{ "-wal", "-shm" }, 0..) |suffix, i| {
        source_sidecars[i] = operationsPath(&source_bufs[i], "{s}{s}", .{ span(path), suffix }) orelse return false;
        quarantine_sidecars[i] = operationsPath(&quarantine_bufs[i], "{s}{s}", .{ span(quarantine), suffix }) orelse return false;
        const source_state = operationsRestorePathState(source_sidecars[i]);
        if (source_state == .other or operationsRestorePathState(quarantine_sidecars[i]) != .absent) return false;
        present[i] = source_state == .regular;
    }
    var guard = std.mem.zeroes(WriterGuard);
    if (c.cbm_zova_writer_guard_acquire(path, &guard) != 0) return false;
    defer c.cbm_zova_writer_guard_release(&guard);
    var moved = [2]bool{ false, false };
    var ok = true;
    for (0..2) |i| {
        if (!present[i]) continue;
        if (c.rename(source_sidecars[i], quarantine_sidecars[i]) != 0) {
            ok = false;
            break;
        }
        moved[i] = true;
    }
    if (ok and c.rename(path, quarantine) != 0) ok = false;
    if (ok and !operationsSyncDirectory(parent)) ok = false;
    if (!ok) {
        var i: usize = 2;
        while (i > 0) {
            i -= 1;
            if (moved[i]) _ = c.rename(quarantine_sidecars[i], source_sidecars[i]);
        }
    }
    return ok;
}

/// Reads the registry's workspace ids, in order, into `ids`.
fn operationsReadWorkspaceIds(db: *Db, ids: [][ID_MAX]u8) bool {
    if (ids.len == 0) return true;
    const statement = operationsPrepare(
        db,
        "SELECT workspace_id FROM cbm_workspace_registry ORDER BY workspace_id",
        null,
    ) orelse return false;
    defer _ = c.zova_statement_finalize(statement);
    for (ids) |*id| {
        if (!operationsStepIs(statement, c.ZOVA_STEP_ROW)) return false;
        var value = std.mem.zeroes(c.zova_text);
        defer c.zova_text_free(&value);
        if (!operationsColumnText(statement, 0, &value) or !operationsTextInto(value, id)) return false;
    }
    return true;
}

/// Health of the whole database: every workspace is checked in turn and the
/// first one that is not healthy decides the result. Closes `db`.
fn operationsHealthEveryWorkspace(path: Str, db: *Db, schema_version: i64, out_class: *HealthClass, out: *Report) Code {
    var workspace_count: i64 = 0;
    if (!operationsQueryI64(db, workspace_count_sql, &workspace_count) or workspace_count < 0 or
        @as(u64, @intCast(workspace_count)) > std.math.maxInt(usize) / ID_MAX)
    {
        _ = c.zova_database_close(db);
        return operationsReport(out, WHOLE_FILE_RECOVERY_REQUIRED, "health", "workspace_inventory_invalid");
    }
    const count: usize = @intCast(workspace_count);
    const allocated: ?[*][ID_MAX]u8 = if (count > 0) interop.callocN([ID_MAX]u8, count) else null;
    defer c.free(@ptrCast(allocated));
    const ids: ?[][ID_MAX]u8 = if (count == 0) &.{} else if (allocated) |rows| rows[0..count] else null;
    const inventory_ok = if (ids) |rows| operationsReadWorkspaceIds(db, rows) else false;
    _ = c.zova_database_close(db);
    if (!inventory_ok)
        return operationsReport(out, WHOLE_FILE_RECOVERY_REQUIRED, "health", "workspace_inventory_invalid");
    for (ids.?) |*id| {
        var workspace_health: HealthClass = c.CBM_ZOVA_HEALTH_OK;
        var workspace_report = std.mem.zeroes(Report);
        const code = cbm_zova_database_health(path, @ptrCast(id), &workspace_health, &workspace_report);
        if (code != OK) {
            out.* = workspace_report;
            out_class.* = workspace_health;
            return code;
        }
    }
    out_class.* = c.CBM_ZOVA_HEALTH_OK;
    _ = operationsReport(out, OK, "health", "ok");
    out.schema_version = @intCast(schema_version);
    return OK;
}

/// Reports that `workspace_id` needs a rebuild for `reason`.
fn operationsRebuildRequired(out: *Report, out_class: *HealthClass, workspace_id: Str, reason: []const u8) Code {
    _ = operationsReport(out, WORKSPACE_REBUILD_REQUIRED, "health", reason);
    operationsSetWorkspaceId(out, workspace_id);
    out_class.* = c.CBM_ZOVA_HEALTH_WORKSPACE_REBUILD;
    return WORKSPACE_REBUILD_REQUIRED;
}

pub export fn cbm_zova_database_health(path: ?Str, workspace_id: ?Str, out_class: ?*HealthClass, out: ?*Report) Code {
    _ = operationsReport(out, INVALID, "health", "invalid_argument");
    const live = nonEmpty(path) orelse return INVALID;
    const class = out_class orelse return INVALID;
    const report = out orelse return INVALID;
    if (workspace_id != null and c.cbm_zova_workspace_id_validate(workspace_id) != 0) return INVALID;
    if (!with_zova) return INCOMPATIBLE;

    class.* = c.CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY;
    var open_status: c.zova_status = c.ZOVA_OK;
    const opened = operationsOpen(live, c.ZOVA_OPEN_READ_ONLY, 5000, &open_status);
    var check_status: c.zova_status = c.ZOVA_OK;
    var schema_version: i64 = 0;
    const shared_ok = if (opened) |db|
        operationsQuickCheckStatus(db, &check_status) and
            operationsQueryI64(db, schema_version_sql, &schema_version) and
            schema_version == SCHEMA_VERSION and
            c.cbm_zova_user_database_schema_is_current(db) == 0
    else
        false;
    if (!shared_ok) {
        if (opened) |db| _ = c.zova_database_close(db);
        // Unreadable right now is not damage; never send it to recovery.
        if (operationsStatusIsTransient(if (opened != null) check_status else open_status))
            return operationsReport(report, BUSY, "health", "database_unavailable");
        _ = operationsReport(report, WHOLE_FILE_RECOVERY_REQUIRED, "health", "shared_database_invalid");
        report.schema_version = @truncate(schema_version);
        return WHOLE_FILE_RECOVERY_REQUIRED;
    }
    const db = opened.?;
    if (!operationsForeignKeysOk(db)) {
        _ = c.zova_database_close(db);
        _ = operationsReport(report, WHOLE_FILE_RECOVERY_REQUIRED, "health", "foreign_key_violation");
        report.schema_version = @intCast(schema_version);
        return WHOLE_FILE_RECOVERY_REQUIRED;
    }
    const workspace = workspace_id orelse
        return operationsHealthEveryWorkspace(live, db, schema_version, class, report);

    var generation: i64 = 0;
    var already_rebuild = false;
    const row_read = operationsWorkspaceHealthRow(db, workspace, &generation, &already_rebuild);
    _ = c.zova_database_close(db);
    if (!row_read) return operationsRebuildRequired(report, class, workspace, "workspace_generation_invalid");
    if (already_rebuild) {
        _ = operationsRebuildRequired(report, class, workspace, "workspace_rebuild_required");
        report.generation = generation;
        report.schema_version = @intCast(schema_version);
        return WORKSPACE_REBUILD_REQUIRED;
    }
    const mismatch: ?Str = mismatch: {
        var snapshot = std.mem.zeroes(Snapshot);
        if (c.cbm_zova_repository_export_snapshot(live, workspace, &snapshot) != 0)
            break :mismatch "workspace_public_state_invalid";
        defer c.cbm_zova_workspace_snapshot_free(&snapshot);
        const input = operationsSnapshotInput(&snapshot);
        var readback = std.mem.zeroes(GenerationResult);
        if (c.cbm_zova_workspace_generation_digest_input(workspace, &input, &readback) != 0)
            break :mismatch "workspace_digest_mismatch";
        readback.generation = snapshot.generation;
        _ = interop.bprint(&readback.workspace_id, "{s}", .{span(workspace)});
        break :mismatch if (operationsGenerationMatches(&readback, &snapshot.integrity)) null else "workspace_digest_mismatch";
    };
    if (mismatch) |reason| {
        if (!operationsMarkWorkspaceRebuildRequired(live, workspace, generation, reason))
            return operationsReport(report, VERIFY_FAILED, "health", "health_state_write_failed");
        _ = operationsRebuildRequired(report, class, workspace, span(reason));
        report.generation = generation;
        report.schema_version = @intCast(schema_version);
        return WORKSPACE_REBUILD_REQUIRED;
    }
    _ = operationsReport(report, OK, "health", "ok");
    operationsSetWorkspaceId(report, workspace);
    report.generation = generation;
    report.schema_version = @intCast(schema_version);
    class.* = c.CBM_ZOVA_HEALTH_OK;
    return OK;
}

fn workspaceRecoverBody(path: ?Str, workspace_id: ?Str, repo_path: ?Str, out: ?*Report) Code {
    const operation = "recover_workspace";
    _ = operationsReport(out, INVALID, operation, "invalid_argument");
    const live = nonEmpty(path) orelse return INVALID;
    if (c.cbm_zova_workspace_id_validate(workspace_id) != 0) return INVALID;
    const workspace = workspace_id orelse return INVALID;
    const repo = nonEmpty(repo_path) orelse return INVALID;
    const report = out orelse return INVALID;
    if (!with_zova) return INCOMPATIBLE;

    var expected_workspace: [ID_MAX]u8 = undefined;
    var user_path: [PATH_CAP]u8 = undefined;
    if (c.cbm_zova_workspace_id_for_root(repo, &expected_workspace, expected_workspace.len) != 0 or
        !bufEql(&expected_workspace, span(workspace)) or
        c.cbm_zova_user_database_path(&user_path, user_path.len) != 0 or
        !bufEql(&user_path, span(live)))
        return operationsReport(report, INVALID, operation, "identity_or_route_mismatch");
    var health: HealthClass = c.CBM_ZOVA_HEALTH_OK;
    var health_report = std.mem.zeroes(Report);
    var health_code = cbm_zova_database_health(live, workspace, &health, &health_report);
    if (health_code == WHOLE_FILE_RECOVERY_REQUIRED and health == c.CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY) {
        // Moving the file aside needs the exclusive barrier, which the caller
        // takes after releasing this run's shared hold.
        _ = operationsReport(report, WHOLE_FILE_RECOVERY_REQUIRED, operation, "whole_file_recovery_required");
        operationsSetWorkspaceId(report, workspace);
        return WHOLE_FILE_RECOVERY_REQUIRED;
    }
    if (health_code != WORKSPACE_REBUILD_REQUIRED or health != c.CBM_ZOVA_HEALTH_WORKSPACE_REBUILD) {
        if (health_code == OK) return operationsReport(report, NOOP, operation, "workspace_healthy");
        return operationsReport(report, health_code, operation, sliceTo(&health_report.reason, 0));
    }
    var project: [256]u8 = undefined;
    const pipeline = if (operationsWorkspaceProject(live, workspace, &project))
        c.cbm_pipeline_new(repo, null, c.CBM_MODE_FULL)
    else
        null;
    if (pipeline == null or !c.cbm_pipeline_set_project_name(pipeline, &project)) {
        if (pipeline != null) c.cbm_pipeline_free(pipeline);
        return operationsReport(report, VERIFY_FAILED, operation, "pipeline_create_failed");
    }
    const run_rc = c.cbm_pipeline_run(pipeline);
    c.cbm_pipeline_free(pipeline);
    if (run_rc != 0) {
        _ = operationsReport(report, VERIFY_FAILED, operation, "source_rebuild_failed");
        operationsSetWorkspaceId(report, workspace);
        report.generation = health_report.generation;
        return VERIFY_FAILED;
    }
    health_code = cbm_zova_database_health(live, workspace, &health, &health_report);
    if (health_code != OK or health != c.CBM_ZOVA_HEALTH_OK)
        return operationsReport(report, VERIFY_FAILED, operation, "post_rebuild_health_failed");
    _ = operationsReport(report, OK, operation, "ok");
    operationsSetWorkspaceId(report, workspace);
    report.generation = health_report.generation;
    report.schema_version = health_report.schema_version;
    return OK;
}

// ── Backup and restore ──────────────────────────────────────────

pub export fn cbm_zova_database_backup(source_path: ?Str, destination_path: ?Str, out: ?*Report) Code {
    _ = operationsReport(out, INVALID, "backup", "invalid_argument");
    const source = nonEmpty(source_path) orelse return INVALID;
    const destination = nonEmpty(destination_path) orelse return INVALID;
    const report = out orelse return INVALID;
    if (std.mem.eql(u8, span(source), span(destination))) return INVALID;
    if (!with_zova) return INCOMPATIBLE;
    if (!operationsPathIsRegularNoSymlink(source) or operationsLstat(destination) != null) return INVALID;

    const started = operationsNow();
    const source_db = operationsOpenReadOnly(source) orelse return VERIFY_FAILED;
    const backup_ok = c.zova_database_backup(&c.zova_database_backup_request{
        .db = source_db,
        .destination_path = destination,
        .flags = 0,
    }) == c.ZOVA_OK;
    _ = c.zova_database_close(source_db);
    var schema_version: c_int = 0;
    const destination_size = if (backup_ok and operationsVerifyDatabase(destination, &schema_version))
        operationsFileSize(destination)
    else
        null;
    const database_size = destination_size orelse {
        _ = c.cbm_unlink(destination);
        return operationsReport(report, VERIFY_FAILED, "backup", "verification_failed");
    };
    const finished = operationsNow();
    _ = operationsReport(report, OK, "backup", "ok");
    report.schema_version = schema_version;
    report.database_bytes = operationsBytes(database_size);
    report.elapsed_ms = operationsElapsedMs(started, finished);
    var wal_buf: [PATH_CAP]u8 = undefined;
    if (operationsPath(&wal_buf, "{s}-wal", .{span(source)})) |wal_path| {
        if (operationsFileSize(wal_path)) |wal_size| report.wal_bytes = operationsBytes(wal_size);
    }
    return OK;
}

fn databaseRestoreBody(live_path: ?Str, backup_path: ?Str, confirm_replace: bool, out: ?*Report) Code {
    _ = operationsReport(out, INVALID, "restore", "invalid_argument");
    const live = nonEmpty(live_path) orelse return INVALID;
    const backup = nonEmpty(backup_path) orelse return INVALID;
    const report = out orelse return INVALID;
    if (!confirm_replace)
        return operationsReport(report, CONFIRMATION_REQUIRED, "restore", "confirmation_required");
    if (!with_zova) return INCOMPATIBLE;

    var temporary_buf: [PATH_CAP]u8 = undefined;
    var recovery_buf: [PATH_CAP]u8 = undefined;
    var parent_buf: [PATH_CAP]u8 = undefined;
    const stem = operationsZovaStem(span(live)) orelse return INVALID;
    const temporary = operationsPath(&temporary_buf, "{s}.restore.tmp.zova", .{stem}) orelse return INVALID;
    const recovery = operationsPath(&recovery_buf, "{s}.restore.recovery.zova", .{stem}) orelse return INVALID;
    const parent = operationsParentDirectory(span(live), &parent_buf) orelse return INVALID;
    if (std.mem.eql(u8, span(temporary), span(live)) or std.mem.eql(u8, span(temporary), span(backup)))
        return INVALID;
    if (!operationsPathIsRegularNoSymlink(backup)) return INVALID;
    const preflight_live_state = operationsRestorePathState(live);
    if (preflight_live_state == .other or
        (preflight_live_state == .regular and operationsSameFile(live, backup)))
        return INVALID;
    var guard = std.mem.zeroes(WriterGuard);
    if (c.cbm_zova_writer_guard_acquire(live, &guard) != 0)
        return operationsReport(report, BUSY, "restore", "writer_busy");
    defer c.cbm_zova_writer_guard_release(&guard);
    const started = operationsNow();
    const files = ReplacementFiles.read(live, temporary, recovery);
    if (!files.resumable() or
        (files.temporary == .regular and operationsSameFile(temporary, backup)) or
        (files.recovery == .regular and operationsSameFile(recovery, backup)) or
        !operationsVerifyDatabase(backup, null))
        return operationsReport(report, VERIFY_FAILED, "restore", "invalid_recovery_state");

    if (files.temporary == .absent and files.recovery == .absent) {
        var message = std.mem.zeroes(c.zova_message);
        const restored = c.zova_database_restore(&c.zova_database_restore_request{
            .source_path = backup,
            .destination_path = temporary,
            .flags = 0,
            .out_error_message = &message,
        }) == c.ZOVA_OK;
        c.zova_message_free(&message);
        if (!restored) return operationsReport(report, VERIFY_FAILED, "restore", "replacement_failed");
        if (operationsReplacementFault("restore", "restore_after_temp_creation", report))
            return VERIFY_FAILED;
    }
    const replacement = ReplacementOptions{
        .operation = "restore",
        .after_temp_verification = "restore_after_temp_verification",
        .after_live_to_recovery = "restore_after_live_to_recovery",
        .after_temp_to_live = "restore_after_temp_to_live",
        .before_recovery_cleanup = "restore_before_recovery_cleanup",
        // A hot -wal left by a crashed writer would replay onto the restored file.
        .remove_sidecars = true,
        .require_source_verified = false,
    };
    var schema_version: c_int = 0;
    switch (operationsReplaceVerifiedDatabase(live, temporary, recovery, parent, &replacement, &schema_version, report)) {
        .interrupted => return VERIFY_FAILED,
        .failed => return operationsReport(report, VERIFY_FAILED, "restore", "replacement_failed"),
        .done => {},
    }

    const finished = operationsNow();
    _ = operationsReport(report, OK, "restore", "ok");
    report.schema_version = schema_version;
    if (operationsFileSize(live)) |live_size| report.database_bytes = operationsBytes(live_size);
    report.elapsed_ms = operationsElapsedMs(started, finished);
    return OK;
}

// ── Database archives ───────────────────────────────────────────

pub export fn cbm_zova_database_export(source_path: ?Str, archive_directory: ?Str, out: ?*Report) Code {
    _ = operationsReport(out, INVALID, "export_database", "invalid_argument");
    const source = nonEmpty(source_path) orelse return INVALID;
    const archive = nonEmpty(archive_directory) orelse return INVALID;
    const report = out orelse return INVALID;
    if (!with_zova) return INCOMPATIBLE;
    if (!operationsPathIsRegularNoSymlink(source)) return INVALID;

    var partial_buf: [PATH_CAP]u8 = undefined;
    var data_buf: [PATH_CAP]u8 = undefined;
    var manifest_buf: [PATH_CAP]u8 = undefined;
    var parent_buf: [PATH_CAP]u8 = undefined;
    const partial = operationsPath(&partial_buf, "{s}.partial", .{span(archive)}) orelse return INVALID;
    const data = operationsPath(&data_buf, "{s}/data.zova", .{span(partial)}) orelse return INVALID;
    const manifest_path = operationsPath(&manifest_buf, "{s}/manifest.json", .{span(partial)}) orelse return INVALID;
    const parent = operationsParentDirectory(span(archive), &parent_buf) orelse return INVALID;
    if (operationsLstat(archive) != null) return INVALID;
    if (operationsLstat(partial) != null)
        return operationsReport(report, BUSY, "export_database", "partial_exists");
    if (!operationsMakePrivateDirectory(partial)) return INVALID;

    const started = operationsNow();
    var backup = std.mem.zeroes(Report);
    var manifest = ArchiveManifest{ .archive_version = ARCHIVE_VERSION, .schema_version = SCHEMA_VERSION };
    const exported = cbm_zova_database_backup(source, data, &backup) == OK and
        operationsFileSha256(data, &manifest.database_sha256, &manifest.database_bytes) and
        operationsWriteArchiveManifest(data, manifest_path, &manifest) and
        operationsRemoveClosedDatabaseSidecars(data) and
        operationsVerifyArchive(partial, null, null) == .valid and
        operationsRemoveClosedDatabaseSidecars(data) and
        operationsArchiveHasExactMembers(partial) and
        operationsSyncDirectory(partial) and
        c.rename(partial, archive) == 0 and
        operationsSyncDirectory(parent);
    if (!exported) {
        operationsRemovePartialArchive(partial);
        return operationsReport(report, VERIFY_FAILED, "export_database", "archive_verification_failed");
    }
    const finished = operationsNow();
    _ = operationsReport(report, OK, "export_database", "ok");
    report.archive_version = manifest.archive_version;
    report.schema_version = manifest.schema_version;
    report.database_bytes = manifest.database_bytes;
    report.elapsed_ms = operationsElapsedMs(started, finished);
    return OK;
}

pub export fn cbm_zova_database_import(live_path: ?Str, archive_directory: ?Str, confirm_replace: bool, out: ?*Report) Code {
    _ = operationsReport(out, INVALID, "import_database", "invalid_argument");
    const live = nonEmpty(live_path) orelse return INVALID;
    const archive = nonEmpty(archive_directory) orelse return INVALID;
    const report = out orelse return INVALID;
    if (!confirm_replace)
        return operationsReport(report, CONFIRMATION_REQUIRED, "import_database", "confirmation_required");
    if (!with_zova) return INCOMPATIBLE;

    const value = operationsLstat(archive) orelse return INVALID;
    if (!operationsIsDirectory(&value)) return INVALID;
    var manifest = ArchiveManifest{};
    switch (operationsVerifyArchive(archive, live, &manifest)) {
        .valid => {},
        .incompatible => return operationsReport(report, INCOMPATIBLE, "import_database", "archive_version_incompatible"),
        .path_alias => return INVALID,
        .malformed => return operationsReport(report, VERIFY_FAILED, "import_database", "archive_verification_failed"),
    }
    var data_buf: [PATH_CAP]u8 = undefined;
    const data = operationsPath(&data_buf, "{s}/data.zova", .{span(archive)}) orelse return INVALID;
    const code = cbm_zova_database_restore(live, data, true, report);
    if (code == OK) {
        _ = interop.bprint(&report.operation, "{s}", .{"import_database"});
        report.archive_version = manifest.archive_version;
    }
    return code;
}

// ── Workspace archives ──────────────────────────────────────────

pub export fn cbm_zova_workspace_export(source_path: ?Str, workspace_id: ?Str, archive_directory: ?Str, out: ?*Report) Code {
    const operation = "export_workspace";
    _ = operationsReport(out, INVALID, operation, "invalid_argument");
    const source = nonEmpty(source_path) orelse return INVALID;
    const archive = nonEmpty(archive_directory) orelse return INVALID;
    const report = out orelse return INVALID;
    if (c.cbm_zova_workspace_id_validate(workspace_id) != 0) return INVALID;
    const workspace = workspace_id orelse return INVALID;
    if (!with_zova) return INCOMPATIBLE;
    if (!operationsPathIsRegularNoSymlink(source)) return INVALID;

    var snapshot = std.mem.zeroes(Snapshot);
    if (c.cbm_zova_repository_export_snapshot(source, workspace, &snapshot) != 0)
        return operationsReport(report, INVALID, operation, "workspace_not_ready");
    defer c.cbm_zova_workspace_snapshot_free(&snapshot);
    var partial_buf: [PATH_CAP]u8 = undefined;
    var data_buf: [PATH_CAP]u8 = undefined;
    var manifest_buf: [PATH_CAP]u8 = undefined;
    var parent_buf: [PATH_CAP]u8 = undefined;
    const partial = operationsPath(&partial_buf, "{s}.partial", .{span(archive)}) orelse return INVALID;
    const data = operationsPath(&data_buf, "{s}/data.zova", .{span(partial)}) orelse return INVALID;
    const manifest_path = operationsPath(&manifest_buf, "{s}/manifest.json", .{span(partial)}) orelse return INVALID;
    const parent = operationsParentDirectory(span(archive), &parent_buf) orelse return INVALID;
    if (operationsLstat(archive) != null) return INVALID;
    if (operationsLstat(partial) != null) return operationsReport(report, BUSY, operation, "partial_exists");
    if (!operationsMakePrivateDirectory(partial)) return INVALID;

    const started = operationsNow();
    const input = operationsSnapshotInput(&snapshot);
    var published = std.mem.zeroes(GenerationResult);
    var manifest = ArchiveManifest{ .archive_version = ARCHIVE_VERSION, .schema_version = SCHEMA_VERSION };
    var failure_phase: []const u8 = "publish_failed";
    var ok = c.cbm_zova_user_database_publish_workspace_at_generation(data, &input, snapshot.generation, &published) == 0;
    if (ok) {
        failure_phase = "published_inventory_mismatch";
        ok = operationsGenerationMatches(&published, &snapshot.integrity);
    }
    if (ok) {
        failure_phase = "sidecar_cleanup_failed";
        ok = operationsRemoveClosedDatabaseSidecars(data);
    }
    if (ok) {
        failure_phase = "writer_lock_cleanup_failed";
        ok = operationsRemoveClosedWriterLock(data);
    }
    if (ok) {
        failure_phase = "published_snapshot_verification_failed";
        var verified = std.mem.zeroes(Snapshot);
        ok = c.cbm_zova_repository_export_snapshot(data, workspace, &verified) == 0;
        c.cbm_zova_workspace_snapshot_free(&verified);
    }
    if (ok) {
        failure_phase = "database_digest_failed";
        ok = operationsFileSha256(data, &manifest.database_sha256, &manifest.database_bytes);
    }
    if (ok) {
        failure_phase = "manifest_write_failed";
        ok = operationsWriteWorkspaceArchiveManifest(manifest_path, &manifest, &snapshot);
    }
    if (ok) {
        failure_phase = "manifest_sidecar_cleanup_failed";
        ok = operationsRemoveClosedDatabaseSidecars(data);
    }
    if (ok) {
        failure_phase = "archive_verification_failed";
        ok = operationsVerifyWorkspaceArchive(partial, null, null) == .valid;
    }
    if (ok) {
        failure_phase = "verification_sidecar_cleanup_failed";
        ok = operationsRemoveClosedDatabaseSidecars(data);
    }
    if (ok) {
        failure_phase = "archive_members_failed";
        ok = operationsArchiveHasExactMembers(partial);
    }
    if (ok) {
        failure_phase = "partial_sync_failed";
        ok = operationsSyncDirectory(partial);
    }
    if (ok) {
        failure_phase = "archive_rename_failed";
        ok = c.rename(partial, archive) == 0;
    }
    if (ok) {
        failure_phase = "parent_sync_failed";
        ok = operationsSyncDirectory(parent);
    }
    if (!ok) {
        operationsRemovePartialArchive(partial);
        return operationsReport(report, VERIFY_FAILED, operation, failure_phase);
    }
    const finished = operationsNow();
    _ = operationsReport(report, OK, operation, "ok");
    operationsSetWorkspaceId(report, workspace);
    report.archive_version = manifest.archive_version;
    report.schema_version = manifest.schema_version;
    report.generation = snapshot.generation;
    report.database_bytes = manifest.database_bytes;
    report.elapsed_ms = operationsElapsedMs(started, finished);
    return OK;
}

pub export fn cbm_zova_workspace_import(target_path: ?Str, archive_directory: ?Str, replace: bool, out: ?*Report) Code {
    const operation = "import_workspace";
    _ = operationsReport(out, INVALID, operation, "invalid_argument");
    const target = nonEmpty(target_path) orelse return INVALID;
    const archive = nonEmpty(archive_directory) orelse return INVALID;
    const report = out orelse return INVALID;
    if (!with_zova) return INCOMPATIBLE;

    const target_state = operationsRestorePathState(target);
    if (target_state == .other) return INVALID;
    var manifest = ArchiveManifest{};
    switch (operationsVerifyWorkspaceArchive(archive, target, &manifest)) {
        .valid => {},
        .incompatible => return operationsReport(report, INCOMPATIBLE, operation, "archive_version_incompatible"),
        .path_alias => return INVALID,
        .malformed => return operationsReport(report, VERIFY_FAILED, operation, "archive_verification_failed"),
    }
    var data_buf: [PATH_CAP]u8 = undefined;
    const data = operationsPath(&data_buf, "{s}/data.zova", .{span(archive)}) orelse return INVALID;
    var workspace_buf: [ID_MAX]u8 = @splat(0);
    if (!operationsSingleReadyWorkspace(data, &workspace_buf)) {
        _ = operationsCleanupVerifiedArchiveReads(archive);
        return operationsReport(report, VERIFY_FAILED, operation, "workspace_inventory_invalid");
    }
    const workspace: Str = @ptrCast(&workspace_buf);
    var snapshot = std.mem.zeroes(Snapshot);
    if (c.cbm_zova_repository_export_snapshot(data, workspace, &snapshot) != 0) {
        _ = operationsCleanupVerifiedArchiveReads(archive);
        return operationsReport(report, VERIFY_FAILED, operation, "workspace_snapshot_invalid");
    }
    defer c.cbm_zova_workspace_snapshot_free(&snapshot);
    if (!operationsCleanupVerifiedArchiveReads(archive))
        return operationsReport(report, VERIFY_FAILED, operation, "archive_read_cleanup_failed");
    // The import publishes this workspace, so it holds what an index run of it
    // holds: the barrier shared, then the workspace lease. It does not wait.
    var barrier = std.mem.zeroes(CoordHold);
    var lease = std.mem.zeroes(CoordHold);
    defer c.cbm_zova_coord_release(&barrier);
    defer c.cbm_zova_coord_release(&lease);
    var coord = c.cbm_zova_prepare_barrier_acquire(target, false, 0, &barrier);
    var busy_reason: []const u8 = "database_replacement_in_progress";
    if (coord == c.CBM_ZOVA_COORD_OK) {
        coord = c.cbm_zova_workspace_lease_acquire(target, workspace, 0, &lease);
        busy_reason = "workspace_busy";
    }
    if (coord != c.CBM_ZOVA_COORD_OK) return operationsCoordRefusal(report, operation, coord, busy_reason);

    const started = operationsNow();
    var schema_version: c_int = 0;
    if ((target_state == .absent and c.cbm_zova_user_database_init(target) != 0) or
        !operationsVerifyDatabase(target, &schema_version) or schema_version != SCHEMA_VERSION)
        return operationsReport(report, VERIFY_FAILED, operation, "target_verification_failed");
    const existing_present = existing: {
        var collision = std.mem.zeroes(Snapshot);
        defer c.cbm_zova_workspace_snapshot_free(&collision);
        if (c.cbm_zova_repository_export_snapshot(target, workspace, &collision) != 0) break :existing false;
        if (!bufEql(&collision.workspace_id, &snapshot.workspace_id) or
            !std.mem.eql(u8, span(collision.root_path), span(snapshot.root_path)))
            return operationsReport(report, INVALID, operation, "identity_mismatch");
        if (!replace) return operationsReport(report, INVALID, operation, "workspace_exists");
        if (snapshot.generation < collision.generation)
            return operationsReport(report, INCOMPATIBLE, operation, "archive_generation_older");
        if (snapshot.generation != collision.generation) break :existing true;
        if (!operationsGenerationMatches(&collision.integrity, &snapshot.integrity))
            return operationsReport(report, VERIFY_FAILED, operation, "same_generation_digest_mismatch");
        const finished = operationsNow();
        _ = operationsReport(report, NOOP, operation, "already_current");
        operationsSetWorkspaceId(report, workspace);
        report.schema_version = schema_version;
        report.archive_version = manifest.archive_version;
        report.generation = snapshot.generation;
        if (operationsFileSize(target)) |target_size| report.database_bytes = operationsBytes(target_size);
        report.elapsed_ms = operationsElapsedMs(started, finished);
        return NOOP;
    };
    var workspaces_before: i64 = 0;
    var workspaces_after: i64 = 0;
    if (!operationsWorkspaceCount(target, &workspaces_before))
        return operationsReport(report, VERIFY_FAILED, operation, "target_inventory_failed");
    const input = operationsSnapshotInput(&snapshot);
    var published = std.mem.zeroes(GenerationResult);
    var imported = std.mem.zeroes(Snapshot);
    const verified = c.cbm_zova_user_database_publish_workspace_at_generation(target, &input, snapshot.generation, &published) == 0 and
        operationsGenerationMatches(&published, &snapshot.integrity) and
        c.cbm_zova_repository_export_snapshot(target, workspace, &imported) == 0 and
        operationsGenerationMatches(&imported.integrity, &snapshot.integrity) and
        operationsWorkspaceCount(target, &workspaces_after) and
        workspaces_after == workspaces_before + @intFromBool(!existing_present);
    c.cbm_zova_workspace_snapshot_free(&imported);
    if (!verified) return operationsReport(report, VERIFY_FAILED, operation, "published_verification_failed");
    const finished = operationsNow();
    _ = operationsReport(report, OK, operation, "ok");
    operationsSetWorkspaceId(report, workspace);
    report.schema_version = schema_version;
    report.archive_version = manifest.archive_version;
    report.generation = snapshot.generation;
    if (operationsFileSize(target)) |target_size| report.database_bytes = operationsBytes(target_size);
    report.elapsed_ms = operationsElapsedMs(started, finished);
    return OK;
}

// ── Repack ──────────────────────────────────────────────────────

fn repackCode(out: *RepackReport, code: Code) Code {
    out.code = code;
    return code;
}

/// Publishes every workspace of a schema-5 snapshot into `temporary` and
/// checks each against the digest of its input.
fn operationsRepackPublish(snapshot: *const c.cbm_zova_v5_snapshot_t, count: c_int, temporary: Str) bool {
    var i: c_int = 0;
    while (i < count) : (i += 1) {
        var generation: i64 = 0;
        const input = c.cbm_zova_v5_snapshot_input_at(snapshot, i, &generation);
        var expected = std.mem.zeroes(GenerationResult);
        var published = std.mem.zeroes(GenerationResult);
        var workspace_id: [ID_MAX]u8 = undefined;
        if (input == null or generation <= 0 or
            c.cbm_zova_workspace_id_for_root(input.*.root_path, &workspace_id, workspace_id.len) != 0 or
            c.cbm_zova_workspace_generation_digest_input(&workspace_id, input, &expected) != 0)
            return false;
        expected.generation = generation;
        _ = interop.bprint(&expected.workspace_id, "{s}", .{sliceTo(&workspace_id, 0)});
        if (c.cbm_zova_user_database_publish_workspace_at_generation(temporary, input, generation, &published) != 0 or
            !operationsGenerationMatches(&published, &expected))
            return false;
        interop.logInfo("zova.repack", .{ "phase", "workspace_published" });
    }
    return true;
}

/// The repacked file is a sound current-schema database holding `count`
/// workspaces and none of the schema-5 tables.
fn operationsRepackTargetVerified(temporary: Str, count: c_int) bool {
    var schema: c_int = 0;
    if (!operationsVerifyDatabase(temporary, &schema)) return false;
    interop.logInfo("zova.repack", .{ "phase", "target_verified" });
    const target_db = operationsOpen(temporary, 0, 5000, null) orelse return false;
    var target_workspaces: i64 = -1;
    var forbidden: i64 = -1;
    const read = operationsCheckpointTruncate(target_db) and
        operationsQueryI64(target_db, workspace_count_sql, &target_workspaces) and
        operationsQueryI64(
            target_db,
            "SELECT count(*) FROM sqlite_master WHERE name IN ('cbm_zova_trace_nodes_v1'," ++
                "'cbm_zova_edge_metadata_v1','cbm_fts_rowmap_v1','cbm_node_vectors_compat_v1'," ++
                "'cbm_token_vectors_compat_v1') OR name GLOB 'cbm_fts_w1_*'",
            &forbidden,
        );
    _ = c.zova_database_close(target_db);
    return read and schema == SCHEMA_VERSION and target_workspaces == count and forbidden == 0;
}

fn databaseRepackBody(request: ?*const RepackRequest, out: ?*RepackReport) Code {
    const report = out orelse return INVALID;
    report.* = std.mem.zeroes(RepackReport);
    const live = nonEmpty(if (request) |r| @as(?Str, @ptrCast(r.live_path)) else null) orelse
        return repackCode(report, INVALID);
    if (!with_zova) return repackCode(report, INCOMPATIBLE);

    const keep_recovery = request.?.keep_recovery;
    var temporary_buf: [PATH_CAP]u8 = undefined;
    var recovery_buf: [PATH_CAP]u8 = undefined;
    var parent_buf: [PATH_CAP]u8 = undefined;
    const stem = operationsZovaStem(span(live)) orelse return repackCode(report, INVALID);
    const temporary = operationsPath(&temporary_buf, "{s}.repack.tmp.zova", .{stem}) orelse
        return repackCode(report, INVALID);
    const recovery = operationsPath(&recovery_buf, "{s}.repack.recovery.zova", .{stem}) orelse
        return repackCode(report, INVALID);
    const parent = operationsParentDirectory(span(live), &parent_buf) orelse
        return repackCode(report, INVALID);
    _ = interop.bprint(&report.recovery_path, "{s}", .{span(recovery)});
    var guard = std.mem.zeroes(WriterGuard);
    if (c.cbm_zova_writer_guard_try_acquire(live, &guard) != 0) return repackCode(report, BUSY);
    defer c.cbm_zova_writer_guard_release(&guard);
    var files = ReplacementFiles.read(live, temporary, recovery);
    if (files.anyOther()) return repackCode(report, VERIFY_FAILED);

    var ignored_bytes: u64 = 0;
    if (files.are(.regular, .absent, .absent)) {
        if (operationsVerifyDatabase(live, null)) {
            report.source_cbm_schema = SCHEMA_VERSION;
            report.target_cbm_schema = SCHEMA_VERSION;
            return repackCode(report, NOOP);
        }
        if (!operationsRawSqliteCheckpointTruncate(live)) return repackCode(report, BUSY);
        interop.logInfo("zova.repack", .{ "phase", "source_checkpointed" });
        var opened: ?*c.cbm_zova_v5_snapshot_t = null;
        if (c.cbm_zova_v5_snapshot_open(live, &opened) != 0 or opened == null)
            return repackCode(report, INCOMPATIBLE);
        const snapshot = opened.?;
        const count = c.cbm_zova_v5_snapshot_workspace_count(snapshot);
        interop.logInfo("zova.repack", .{ "phase", "source_snapshot", "workspaces", @as(Str, if (count == 2) "2" else "other") });
        report.source_cbm_schema = 5;
        report.target_cbm_schema = SCHEMA_VERSION;
        report.workspace_count = count;
        if (count < 1 or !operationsFileSha256(live, &report.source_sha256, &ignored_bytes)) {
            c.cbm_zova_v5_snapshot_close(snapshot);
            return repackCode(report, VERIFY_FAILED);
        }
        _ = operationsRemoveClosedDatabaseSidecars(temporary);
        _ = c.cbm_unlink(temporary);
        // The gate has one process-wide mutex, so publishing the isolated
        // sibling temp while holding the live guard would self-deadlock.
        // Revalidate the source digest after reacquiring before replacement.
        c.cbm_zova_writer_guard_release(&guard);
        var ok = operationsRepackPublish(snapshot, count, temporary);
        c.cbm_zova_v5_snapshot_close(snapshot);
        if (ok and c.cbm_zova_writer_guard_try_acquire(live, &guard) != 0) return repackCode(report, BUSY);
        if (ok) {
            var current_source: [HEX_LEN + 1]u8 = undefined;
            ok = operationsFileSha256(live, &current_source, &ignored_bytes) and
                bufEql(&current_source, &report.source_sha256);
        }
        ok = ok and operationsRepackTargetVerified(temporary, count) and
            operationsFileSha256(temporary, &report.target_sha256, &ignored_bytes) and
            operationsSyncFile(temporary) and operationsSyncFile(live) and
            operationsSyncDirectory(parent);
        if (!ok) return repackCode(report, VERIFY_FAILED);
        files.temporary = .regular;
        if (operationsReplacementFault("repack", "repack_after_temp_creation", null))
            return repackCode(report, VERIFY_FAILED);
    }

    if (files.are(.regular, .absent, .regular) and operationsVerifyDatabase(live, null)) {
        report.source_cbm_schema = 5;
        report.target_cbm_schema = SCHEMA_VERSION;
        _ = operationsFileSha256(recovery, &report.source_sha256, &ignored_bytes);
        _ = operationsFileSha256(live, &report.target_sha256, &ignored_bytes);
        if (!keep_recovery and
            (!operationsRemoveClosedDatabaseSidecars(recovery) or
                c.cbm_unlink(recovery) != 0 or !operationsSyncDirectory(parent)))
            return repackCode(report, VERIFY_FAILED);
        return repackCode(report, NOOP);
    }
    const replacement = ReplacementOptions{
        .operation = "repack",
        .after_temp_verification = "repack_after_temp_verification",
        .after_live_to_recovery = "repack_after_live_to_recovery",
        .after_temp_to_live = "repack_after_temp_to_live",
        .before_recovery_cleanup = "repack_before_recovery_cleanup",
        .remove_sidecars = true,
        .require_source_verified = false,
        .keep_recovery = keep_recovery,
    };
    if (report.source_sha256[0] == 0) {
        const source_path = if (files.recovery == .regular) recovery else live;
        if (!operationsFileSha256(source_path, &report.source_sha256, &ignored_bytes))
            return repackCode(report, VERIFY_FAILED);
    }
    var target_schema: c_int = 0;
    const replaced = operationsReplaceVerifiedDatabase(live, temporary, recovery, parent, &replacement, &target_schema, null);
    interop.logInfo("zova.repack", .{ "phase", "replacement_returned" });
    if (replaced != .done) return repackCode(report, VERIFY_FAILED);
    report.source_cbm_schema = 5;
    report.target_cbm_schema = target_schema;
    var count: i64 = 0;
    if (!operationsWorkspaceCount(live, &count) or count > std.math.maxInt(c_int))
        return repackCode(report, VERIFY_FAILED);
    report.workspace_count = @truncate(count);
    if (keep_recovery) _ = operationsFileSha256(recovery, &report.source_sha256, &ignored_bytes);
    if (!operationsFileSha256(live, &report.target_sha256, &ignored_bytes))
        return repackCode(report, VERIFY_FAILED);
    return repackCode(report, OK);
}

// ── Coordination (ADR-0001) ─────────────────────────────────────

/// Map a refused lease or barrier acquisition onto the operation report.
fn operationsCoordRefusal(out: ?*Report, operation: []const u8, status: CoordStatus, busy_reason: []const u8) Code {
    if (status == c.CBM_ZOVA_COORD_ERROR)
        return operationsReport(out, VERIFY_FAILED, operation, "coordination_unavailable");
    return operationsReport(out, BUSY, operation, if (status == c.CBM_ZOVA_COORD_CANCELLED) "cancelled" else busy_reason);
}

/// Deleting a workspace mutates it, so it takes the workspace lease. It does
/// not wait: callers that should wait (MCP delete_project) retry on BUSY.
pub export fn cbm_zova_workspace_delete(path: ?Str, workspace_id: ?Str, confirmation: ?Str, out: ?*Report) Code {
    if (!with_zova or nonEmpty(path) == null or c.cbm_zova_workspace_id_validate(workspace_id) != 0)
        return workspaceDeleteBody(path, workspace_id, confirmation, out);
    var lease = std.mem.zeroes(CoordHold);
    const status = c.cbm_zova_workspace_lease_acquire(path, workspace_id, 0, &lease);
    if (status != c.CBM_ZOVA_COORD_OK)
        return operationsCoordRefusal(out, "delete_workspace", status, "workspace_busy");
    defer c.cbm_zova_coord_release(&lease);
    return workspaceDeleteBody(path, workspace_id, confirmation, out);
}

/// Quarantine replaces the database under every workspace, so like restore it
/// holds the prepare barrier exclusive (index runs elsewhere hold it shared),
/// and it re-checks health under that hold before moving the file aside.
fn operationsRecoverQuarantine(path: Str, workspace_id: Str, out: *Report) Code {
    const operation = "recover_workspace";
    var barrier = std.mem.zeroes(CoordHold);
    const status = c.cbm_zova_prepare_barrier_acquire(path, true, 0, &barrier);
    if (status != c.CBM_ZOVA_COORD_OK)
        return operationsCoordRefusal(out, operation, status, "indexing_in_progress");
    defer c.cbm_zova_coord_release(&barrier);
    var health: HealthClass = c.CBM_ZOVA_HEALTH_OK;
    var health_report = std.mem.zeroes(Report);
    const health_code = cbm_zova_database_health(path, workspace_id, &health, &health_report);
    if (health_code != WHOLE_FILE_RECOVERY_REQUIRED or health != c.CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY) {
        // The file changed state while no hold was taken; report what it is now.
        if (health_code == OK) return operationsReport(out, NOOP, operation, "workspace_healthy");
        return operationsReport(out, health_code, operation, sliceTo(&health_report.reason, 0));
    }
    if (!operationsQuarantineDatabase(path))
        return operationsReport(out, VERIFY_FAILED, operation, "quarantine_failed");
    _ = operationsReport(out, WHOLE_FILE_RECOVERY_REQUIRED, operation, "quarantined_no_verified_backup");
    operationsSetWorkspaceId(out, workspace_id);
    return WHOLE_FILE_RECOVERY_REQUIRED;
}

/// Recovery rebuilds the workspace through a pipeline, so it holds what every
/// index run holds: the barrier shared, then the workspace lease.
pub export fn cbm_zova_workspace_recover(path: ?Str, workspace_id: ?Str, repo_path: ?Str, out: ?*Report) Code {
    if (!with_zova or nonEmpty(path) == null or c.cbm_zova_workspace_id_validate(workspace_id) != 0)
        return workspaceRecoverBody(path, workspace_id, repo_path, out);
    const code = held: {
        var barrier = std.mem.zeroes(CoordHold);
        var lease = std.mem.zeroes(CoordHold);
        var status = c.cbm_zova_prepare_barrier_acquire(path, false, 0, &barrier);
        if (status != c.CBM_ZOVA_COORD_OK)
            return operationsCoordRefusal(out, "recover_workspace", status, "database_replacement_in_progress");
        defer c.cbm_zova_coord_release(&barrier);
        status = c.cbm_zova_workspace_lease_acquire(path, workspace_id, 0, &lease);
        if (status != c.CBM_ZOVA_COORD_OK)
            return operationsCoordRefusal(out, "recover_workspace", status, "workspace_busy");
        defer c.cbm_zova_coord_release(&lease);
        break :held workspaceRecoverBody(path, workspace_id, repo_path, out);
    };
    const report = out orelse return code;
    if (code != WHOLE_FILE_RECOVERY_REQUIRED or !bufEql(&report.reason, "whole_file_recovery_required"))
        return code;
    return operationsRecoverQuarantine(path.?, workspace_id.?, report);
}

/// Restore replaces the database file, so it waits (bounded) for index runs
/// that prepared against the current file to finish, and keeps new ones from
/// starting until the replacement is done.
pub export fn cbm_zova_database_restore(live_path: ?Str, backup_path: ?Str, confirm_replace: bool, out: ?*Report) Code {
    if (!with_zova or nonEmpty(live_path) == null or nonEmpty(backup_path) == null or !confirm_replace)
        return databaseRestoreBody(live_path, backup_path, confirm_replace, out);
    var barrier = std.mem.zeroes(CoordHold);
    const status = c.cbm_zova_prepare_barrier_acquire(live_path, true, c.cbm_zova_coord_default_wait_ms(), &barrier);
    if (status != c.CBM_ZOVA_COORD_OK)
        return operationsCoordRefusal(out, "restore", status, "indexing_in_progress");
    defer c.cbm_zova_coord_release(&barrier);
    return databaseRestoreBody(live_path, backup_path, confirm_replace, out);
}

/// Repack replaces the database file; like its writer-gate acquisition it
/// does not wait.
pub export fn cbm_zova_database_repack(request: ?*const RepackRequest, out: ?*RepackReport) Code {
    const report = out orelse return databaseRepackBody(request, out);
    const live = nonEmpty(if (request) |r| @as(?Str, @ptrCast(r.live_path)) else null);
    if (!with_zova or live == null) return databaseRepackBody(request, out);
    var barrier = std.mem.zeroes(CoordHold);
    const status = c.cbm_zova_prepare_barrier_acquire(live, true, 0, &barrier);
    if (status != c.CBM_ZOVA_COORD_OK) {
        report.* = std.mem.zeroes(RepackReport);
        return repackCode(report, if (status == c.CBM_ZOVA_COORD_ERROR) VERIFY_FAILED else BUSY);
    }
    defer c.cbm_zova_coord_release(&barrier);
    return databaseRepackBody(request, out);
}
