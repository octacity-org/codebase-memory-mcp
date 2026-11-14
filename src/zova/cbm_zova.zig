//! cbm_zova.zig — The user-local cbm.zova database: workspace ids and names,
//! the SQL schema, publication of a workspace generation (full or delta), the
//! workspace registry, and the vector and graph query sessions.
//!
//! Implements src/zova/cbm_zova.h. Without Zova every function that needs the
//! database clears its outputs and fails; the id and name helpers still work.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const Str = interop.Str;
const CStr = interop.CStr;
const span = std.mem.span;

const with_zova = c.CBM_WITH_ZOVA != 0;

// Zova SDK types. They exist only with Zova; nothing reaches them without it.
const Db = if (with_zova) c.zova_database else anyopaque;
const Stmt = if (with_zova) c.zova_statement else anyopaque;

const VectorHit = c.cbm_zova_vector_hit_t;
const GraphVisit = c.cbm_zova_graph_visit_t;
const GraphAdjacency = c.cbm_zova_graph_adjacency_t;
const GraphMetrics = c.cbm_zova_graph_metrics_t;
const SqlCapabilities = c.cbm_zova_sql_capabilities_t;
const FormatStatus = c.cbm_zova_database_format_status_t;
const RepackPhase = c.cbm_zova_repack_phase_t;
const GenerationInput = c.cbm_zova_workspace_generation_input_t;
const GenerationResult = c.cbm_zova_workspace_generation_result_t;
const PublishModel = c.cbm_zova_publish_model_t;
const PreparedView = c.cbm_zova_prepared_view_t;
const WorkspaceDelta = c.cbm_zova_workspace_delta_t;
const PublishTestMetrics = c.cbm_zova_publish_test_metrics_t;
const StatementPhaseMetrics = c.cbm_zova_statement_phase_metrics_t;
const DeltaMetrics = c.cbm_zova_workspace_delta_metrics_t;

const node_fts_insert_trigger_sql =
    "CREATE TRIGGER IF NOT EXISTS cbm_nodes_v1_fts_ai AFTER INSERT ON cbm_nodes_v1 BEGIN " ++
    "INSERT INTO cbm_nodes_fts_v1(rowid,name,qualified_name,file_path,label) " ++
    "SELECT NEW.zova_node_key,cbm_camel_split(NEW.name),NEW.qualified_name,f.file_path,NEW.label " ++
    "FROM cbm_files_v1 f WHERE f.file_key=NEW.file_key; END;";

const ZV_PATH_MAX: usize = 1024;
const ZV_BATCH: usize = 512;
const ZV_ID_MAX: usize = 32;
const ZV_SQL_MAX: usize = 512;

const ID_MAX: usize = c.CBM_ZOVA_WORKSPACE_ID_MAX;
const DIGEST_HEX_SIZE: usize = c.CBM_ZOVA_DIGEST_HEX_SIZE;
const SCHEMA_VERSION: i64 = c.CBM_ZOVA_DATABASE_SCHEMA_VERSION;
const GRAPH_NAME_CAP: usize = ZV_ID_MAX + ID_MAX;
const COLLECTION_NAME_CAP: usize = ZV_ID_MAX + ID_MAX + 96;
const INT_MAX: c_int = std.math.maxInt(c_int);

var user_publish_test_metrics: PublishTestMetrics = std.mem.zeroes(PublishTestMetrics);
threadlocal var user_publish_delta_active: bool = false;

fn Span(comptime P: type) type {
    const info = @typeInfo(P).pointer;
    return if (info.attrs.@"const") []const info.child else []info.child;
}

/// The items of a (C pointer, count) array; empty for NULL or count <= 0.
fn many(items: anytype, count: anytype) Span(@TypeOf(items)) {
    if (items == null or count <= 0) return &.{};
    return items[0..@intCast(count)];
}

/// The string when it is neither NULL nor empty.
fn nonEmpty(text: ?Str) ?Str {
    const value = text orelse return null;
    return if (value[0] != 0) value else null;
}

fn cast(comptime T: type, p: ?*const anyopaque) *const T {
    return @ptrCast(@alignCast(p));
}

/// The caller's (pointer, size) output buffer; NULL when it has no room.
fn outBuf(out: ?[*]u8, size: usize) ?[]u8 {
    const buf = out orelse return null;
    return if (size != 0) buf[0..size] else null;
}

/// snprintf into `out`: 0 when the whole text fit, -1 when it was cut.
fn formatInto(out: []u8, comptime fmt: []const u8, args: anytype) c_int {
    return if (interop.bprint(out, fmt, args) < out.len) 0 else -1;
}

/// The NUL-terminated text at the start of `buf`.
fn bufZ(buf: anytype) Str {
    return @ptrCast(buf);
}

fn userPublishMetricRow(
    maybe_metrics: ?*StatementPhaseMetrics,
    bind_i64_calls: u64,
    bind_text_calls: u64,
    bind_double_calls: u64,
    cleared_bindings: bool,
) void {
    const metrics = maybe_metrics orelse return;
    metrics.rows += 1;
    metrics.bind_i64_calls += bind_i64_calls;
    metrics.bind_text_calls += bind_text_calls;
    metrics.bind_double_calls += bind_double_calls;
    metrics.step_calls += 1;
    metrics.reset_calls += 1;
    if (cleared_bindings) metrics.clear_bindings_calls += 1;
}

pub export fn cbm_zova_publish_test_metrics_reset() void {
    user_publish_test_metrics = std.mem.zeroes(PublishTestMetrics);
}

pub export fn cbm_zova_publish_test_metrics_get(out_metrics: ?*PublishTestMetrics) void {
    if (out_metrics) |out| out.* = user_publish_test_metrics;
}

pub export fn cbm_zova_build_enabled() bool {
    return with_zova;
}

// ── Workspace ids and names ─────────────────────────────────────

/// Copies `root_path` into `out` with '/' separators and no trailing '/'.
/// NULL for a NULL, empty or too long path.
fn workspaceNormalizeRoot(root_path: ?Str, out: *[ZV_PATH_MAX]u8) ?[:0]const u8 {
    const path = nonEmpty(root_path) orelse return null;
    if (interop.bprint(out, "{s}", .{span(path)}) >= ZV_PATH_MAX) return null;
    _ = c.cbm_normalize_path_sep(out);
    var len = c.strlen(out);
    while (len > 1 and out[len - 1] == '/') {
        len -= 1;
        out[len] = 0;
    }
    return out[0..len :0];
}

fn workspaceIdFromRoot(root: []const u8, out: []u8) c_int {
    var digest: [c.CBM_SHA256_HEX_LEN + 1]u8 = undefined;
    c.cbm_sha256_hex(root.ptr, root.len, &digest);
    return formatInto(out, "w1_{s}", .{digest[0..32]});
}

pub export fn cbm_zova_workspace_id_for_root(root_path: ?Str, out_workspace_id: ?[*]u8, out_workspace_id_size: usize) c_int {
    var root_buf: [ZV_PATH_MAX]u8 = undefined;
    const out = outBuf(out_workspace_id, out_workspace_id_size) orelse return -1;
    const root = workspaceNormalizeRoot(root_path, &root_buf) orelse return -1;
    return workspaceIdFromRoot(root, out);
}

pub export fn cbm_zova_workspace_registry_path(out: ?[*]u8, out_size: usize) c_int {
    const cache_dir: CStr = c.cbm_resolve_cache_dir();
    const buf = outBuf(out, out_size) orelse return -1;
    if (cache_dir == null) return -1;
    return formatInto(buf, "{s}/cbm.zova", .{span(cache_dir)});
}

/// One or more of [A-Za-z0-9_].
fn workspaceNameComponentValid(value: ?Str) bool {
    const text = nonEmpty(value) orelse return false;
    for (span(text)) |ch| {
        if (!std.ascii.isAlphanumeric(ch) and ch != '_') return false;
    }
    return true;
}

pub export fn cbm_zova_workspace_id_validate(workspace_id: ?Str) c_int {
    return if (workspaceNameComponentValid(workspace_id)) 0 else -1;
}

/// The first 16 bytes, in hex, of SHA-256 over each part followed by a NUL.
fn partsDigestHex(parts: []const Str) [32]u8 {
    var hash: c.cbm_sha256_ctx = undefined;
    var digest: [c.CBM_SHA256_DIGEST_LEN]u8 = undefined;
    c.cbm_sha256_init(&hash);
    for (parts) |part| {
        c.cbm_sha256_update(&hash, part, c.strlen(part));
        c.cbm_sha256_update(&hash, "\x00", 1);
    }
    c.cbm_sha256_final(&hash, &digest);
    return std.fmt.bytesToHex(digest[0..16].*, .lower);
}

pub export fn cbm_zova_workspace_token_id_v1(workspace_id: ?Str, token: ?Str, out: ?[*]u8, out_size: usize) c_int {
    if (!workspaceNameComponentValid(workspace_id)) return -1;
    const text = token orelse return -1;
    const buf = outBuf(out, out_size) orelse return -1;
    const hex = partsDigestHex(&.{text});
    return formatInto(buf, "t:v1:{s}:{s}", .{ span(workspace_id.?), &hex });
}

pub export fn cbm_zova_workspace_graph_name(workspace_id: ?Str, out: ?[*]u8, out_size: usize) c_int {
    if (!workspaceNameComponentValid(workspace_id)) return -1;
    const buf = outBuf(out, out_size) orelse return -1;
    return formatInto(buf, "cbm_graph_{s}", .{span(workspace_id.?)});
}

fn vectorCollectionName(comptime prefix: []const u8, workspace_id: ?Str, model_fingerprint: ?Str, dimensions: c_int, out: ?[*]u8, out_size: usize) c_int {
    if (!workspaceNameComponentValid(workspace_id) or !workspaceNameComponentValid(model_fingerprint) or dimensions <= 0) return -1;
    const buf = outBuf(out, out_size) orelse return -1;
    return formatInto(buf, prefix ++ "{s}_{s}_d{d}", .{ span(workspace_id.?), span(model_fingerprint.?), dimensions });
}

pub export fn cbm_zova_workspace_node_vector_collection_name(workspace_id: ?Str, model_fingerprint: ?Str, dimensions: c_int, out: ?[*]u8, out_size: usize) c_int {
    return vectorCollectionName("cbm_nodes_i8_", workspace_id, model_fingerprint, dimensions, out, out_size);
}

pub export fn cbm_zova_workspace_token_vector_collection_name(workspace_id: ?Str, model_fingerprint: ?Str, dimensions: c_int, out: ?[*]u8, out_size: usize) c_int {
    return vectorCollectionName("cbm_tokens_i8_", workspace_id, model_fingerprint, dimensions, out, out_size);
}

pub export fn cbm_zova_workspace_node_id_v1(
    workspace_id: ?Str,
    node_kind: ?Str,
    relative_path: ?Str,
    qualified_name: ?Str,
    semantic_discriminator: ?Str,
    out: ?[*]u8,
    out_size: usize,
) c_int {
    if (!workspaceNameComponentValid(workspace_id)) return -1;
    const kind = node_kind orelse return -1;
    const path = relative_path orelse return -1;
    const name = qualified_name orelse return -1;
    const discriminator = semantic_discriminator orelse return -1;
    const buf = outBuf(out, out_size) orelse return -1;
    const hex = partsDigestHex(&.{ kind, path, name, discriminator });
    return formatInto(buf, "n:v1:{s}:{s}", .{ span(workspace_id.?), &hex });
}

pub export fn cbm_zova_workspace_node_id_v2(
    workspace_id: ?Str,
    node_kind: ?Str,
    relative_path: ?Str,
    qualified_name: ?Str,
    semantic_discriminator: ?Str,
    out: ?[*]u8,
    out_size: usize,
) c_int {
    if (!workspaceNameComponentValid(workspace_id)) return -1;
    const kind = nonEmpty(node_kind) orelse return -1;
    const path = relative_path orelse return -1;
    const name = qualified_name orelse return -1;
    const discriminator = nonEmpty(semantic_discriminator) orelse return -1;
    const buf = outBuf(out, out_size) orelse return -1;
    const hex = partsDigestHex(&.{ kind, path, name, discriminator });
    return formatInto(buf, "n:v2:{s}:{s}", .{ span(workspace_id.?), &hex });
}

pub export fn cbm_zova_vector_hits_free(items: [*c]VectorHit, count: c_int) void {
    for (many(items, count)) |*item| c.free(item.vector_id);
    c.free(items);
}

pub export fn cbm_zova_graph_visits_free(visits: [*c]GraphVisit, count: c_int) void {
    for (many(visits, count)) |*visit| {
        c.free(visit.node_id);
        c.free(visit.name);
        c.free(visit.qualified_name);
        c.free(visit.file_path);
        c.free(visit.label);
        c.free(visit.properties);
    }
    c.free(visits);
}

pub export fn cbm_zova_graph_adjacency_free(items: [*c]GraphAdjacency, count: c_int) void {
    for (many(items, count)) |*item| {
        c.free(item.source_node_id);
        c.free(item.target_node_id);
        c.free(item.edge_type);
        c.free(item.properties);
    }
    c.free(items);
}

// ── Database handles ────────────────────────────────────────────

const ZovaDb = extern struct {
    db: ?*Db = null,
};

const VectorSession = extern struct {
    zdb: ZovaDb,
    generation: i64,
    generation_valid: bool,
};

const GraphSession = extern struct {
    zdb: ZovaDb,
    generation: i64,
    generation_valid: bool,
};

const ZV_CAMEL_SPLIT_BUF: usize = 2048;
const ZV_CAMEL_BUF_GUARD: usize = 2;
const ZV_GRAPH_HYDRATE_BATCH: usize = 128;

const ZV_DENOM_EPS_D: f64 = 1e-10;

/// A malloc'd NUL-terminated copy of data[0..len]; NULL on OOM.
fn zvStrndup(data: [*c]const u8, len: usize) [*c]u8 {
    const copy = interop.mallocN(u8, len + 1);
    if (copy == null) return null;
    if (len > 0) @memcpy(copy[0..len], data[0..len]);
    copy[len] = 0;
    return copy;
}

/// A malloc'd copy of a NUL-terminated string; NULL on OOM.
fn zvStrdup(text: CStr) [*c]u8 {
    return zvStrndup(text, c.strlen(text));
}

// ── SQL functions ───────────────────────────────────────────────

fn zvCamelShouldSplit(input: []const u8, i: usize) bool {
    if (i == 0) return false;
    const curr = input[i];
    const prev = input[i - 1];
    const next: u8 = if (i + 1 < input.len) input[i + 1] else 0;
    if (std.ascii.isUpper(curr) and std.ascii.isLower(prev)) return true;
    return std.ascii.isUpper(curr) and std.ascii.isUpper(prev) and std.ascii.isLower(next);
}

fn sqlResultInt(out: *c.zova_sql_result, value: i64) void {
    out.* = .{ .result_type = c.ZOVA_SQL_RESULT_INTEGER, .int64_value = value };
}

fn sqlResultDouble(out: *c.zova_sql_result, value: f64) void {
    out.* = .{ .result_type = c.ZOVA_SQL_RESULT_FLOAT, .double_value = value };
}

fn sqlResultText(out: *c.zova_sql_result, text: []const u8) void {
    out.* = .{ .result_type = c.ZOVA_SQL_RESULT_TEXT, .data = text.ptr, .data_len = text.len };
}

fn sqlResultError(out: *c.zova_sql_result, message: [:0]const u8) void {
    out.* = .{ .result_type = c.ZOVA_SQL_RESULT_ERROR, .error_message = message.ptr, .error_message_len = message.len };
}

fn sqlArg(call: ?*const c.zova_sql_function_call, index: usize) ?*const c.zova_sql_value {
    const invocation = call orelse return null;
    if (invocation.argv == null or index >= invocation.argc) return null;
    return &invocation.argv[index];
}

/// The bytes of a TEXT or BLOB argument; empty when it carries none.
fn sqlValueBytes(value: *const c.zova_sql_value) []const u8 {
    const data: [*]const u8 = @ptrCast(value.data orelse return "");
    return data[0..value.data_len];
}

/// A malloc'd NUL-terminated copy of a TEXT argument; NULL for anything else.
fn sqlTextCopy(maybe_value: ?*const c.zova_sql_value) [*c]u8 {
    const value = maybe_value orelse return null;
    if (value.value_type != c.ZOVA_SQL_VALUE_TEXT) return null;
    if (value.data == null and value.data_len != 0) return null;
    const text = sqlValueBytes(value);
    return zvStrndup(text.ptr, text.len);
}

fn zovaSqlCosineI8(_: ?*anyopaque, call: [*c]const c.zova_sql_function_call, out_result: [*c]c.zova_sql_result) callconv(.c) void {
    const out: *c.zova_sql_result = out_result;
    const a_value = sqlArg(call, 0);
    const b_value = sqlArg(call, 1);
    if (a_value == null or b_value == null or a_value.?.value_type != c.ZOVA_SQL_VALUE_BLOB or
        b_value.?.value_type != c.ZOVA_SQL_VALUE_BLOB or a_value.?.data_len != b_value.?.data_len or
        a_value.?.data_len == 0 or a_value.?.data == null or b_value.?.data == null)
    {
        return sqlResultDouble(out, 0.0);
    }

    const a: []const i8 = @ptrCast(sqlValueBytes(a_value.?));
    const b: []const i8 = @ptrCast(sqlValueBytes(b_value.?));
    var dot: i64 = 0;
    var mag_a: i64 = 0;
    var mag_b: i64 = 0;
    for (a, b) |x, y| {
        dot += @as(i64, x) * @as(i64, y);
        mag_a += @as(i64, x) * @as(i64, x);
        mag_b += @as(i64, y) * @as(i64, y);
    }

    const denom = @sqrt(@as(f64, @floatFromInt(mag_a))) * @sqrt(@as(f64, @floatFromInt(mag_b)));
    sqlResultDouble(out, if (denom > ZV_DENOM_EPS_D) @as(f64, @floatFromInt(dot)) / denom else 0.0);
}

// Zova copies the result bytes after the callback returns, so they must not
// live in its stack frame. Callbacks do not re-enter, so one buffer per
// thread is enough.
threadlocal var camel_split_buf: [ZV_CAMEL_SPLIT_BUF]u8 = undefined;

fn zovaSqlCamelSplit(_: ?*anyopaque, call: [*c]const c.zova_sql_function_call, out_result: [*c]c.zova_sql_result) callconv(.c) void {
    const out: *c.zova_sql_result = out_result;
    const value = sqlArg(call, 0) orelse return sqlResultText(out, "");
    if (value.value_type != c.ZOVA_SQL_VALUE_TEXT) return sqlResultText(out, "");
    const input = sqlValueBytes(value);
    if (input.len == 0) return sqlResultText(out, "");
    const buf = &camel_split_buf;
    if (input.len + 1 >= buf.len) return sqlResultText(out, input);
    @memcpy(buf[0..input.len], input);
    var len = input.len;
    buf[len] = ' ';
    len += 1;
    for (input, 0..) |ch, i| {
        if (len >= buf.len - ZV_CAMEL_BUF_GUARD) break;
        if (zvCamelShouldSplit(input, i)) {
            buf[len] = ' ';
            len += 1;
        }
        buf[len] = ch;
        len += 1;
    }
    buf[len] = 0;
    sqlResultText(out, buf[0..len]);
}

fn zovaSqlRegexpImpl(call: ?*const c.zova_sql_function_call, out: *c.zova_sql_result, flags: c_int) void {
    const pattern = sqlTextCopy(sqlArg(call, 0));
    defer c.free(pattern);
    const text = sqlTextCopy(sqlArg(call, 1));
    defer c.free(text);
    if (pattern == null or text == null) return sqlResultInt(out, 0);

    var re: c.cbm_regex_t = undefined;
    if (c.cbm_regcomp(&re, pattern, flags) != 0) return sqlResultError(out, "invalid regex");
    const matched = c.cbm_regexec(&re, text, 0, null, 0) == 0;
    c.cbm_regfree(&re);
    sqlResultInt(out, @intFromBool(matched));
}

fn zovaSqlRegexp(_: ?*anyopaque, call: [*c]const c.zova_sql_function_call, out_result: [*c]c.zova_sql_result) callconv(.c) void {
    zovaSqlRegexpImpl(call, out_result, c.CBM_REG_EXTENDED | c.CBM_REG_NOSUB);
}

fn zovaSqlIregexp(_: ?*anyopaque, call: [*c]const c.zova_sql_function_call, out_result: [*c]c.zova_sql_result) callconv(.c) void {
    zovaSqlRegexpImpl(call, out_result, c.CBM_REG_EXTENDED | c.CBM_REG_NOSUB | c.CBM_REG_ICASE);
}

/// True for ZOVA_OK. Anything else is logged with `phase` and the database's
/// last error message.
fn statusOk(status: c.zova_status, db: ?*Db, phase: ?Str) bool {
    if (status == c.ZOVA_OK) return true;
    const msg: CStr = if (db != null) c.zova_database_last_error_message(db) else null;
    interop.log(c.CBM_LOG_ERROR, "zova.err", .{
        "phase",  phase orelse "unknown",
        "status", c.zova_status_name(status),
        "msg",    if (msg != null) msg else @as(CStr, ""),
    });
    return false;
}

fn registerZovaSqlFunction(db: ?*Db, name: Str, arity: c_int, callback: c.zova_sql_scalar_callback) bool {
    const status = c.zova_database_register_function(&c.zova_sql_function_register_request{
        .db = db,
        .name = name,
        .arity = arity,
        .flags = c.ZOVA_SQL_FUNCTION_DETERMINISTIC | c.ZOVA_SQL_FUNCTION_INNOCUOUS,
        .user_data = null,
        .callback = callback,
        .destroy = null,
    });
    // Pinned Zova builds report duplicate registration as INVALID_ARGUMENT.
    // Treat that one case as success so callers can safely establish the
    // callback set on handles opened by another CBM helper.
    if (status == c.ZOVA_INVALID_ARGUMENT) return true;
    return statusOk(status, db, name);
}

fn registerSqlFunctions(db: ?*Db) bool {
    if (db == null) return false;
    return registerZovaSqlFunction(db, "cbm_cosine_i8", 2, zovaSqlCosineI8) and
        registerZovaSqlFunction(db, "cbm_camel_split", 1, zovaSqlCamelSplit) and
        registerZovaSqlFunction(db, "regexp", 2, zovaSqlRegexp) and
        registerZovaSqlFunction(db, "iregexp", 2, zovaSqlIregexp);
}

// ── Zova SDK calls ──────────────────────────────────────────────

fn execSql(db: ?*Db, sql: Str) c.zova_status {
    return c.zova_database_exec(&c.zova_database_exec_request{ .db = db, .sql = sql });
}

fn closeZova(zdb: *ZovaDb) void {
    if (zdb.db) |db| {
        _ = c.zova_database_close(db);
        zdb.db = null;
    }
}

/// The prepared statement; NULL after logging the failure under `phase`.
fn prepareZova(db: ?*Db, sql: Str, phase: Str) ?*Stmt {
    var stmt: ?*Stmt = null;
    const status = c.zova_database_prepare(&c.zova_database_prepare_request{ .db = db, .sql = sql, .out_statement = &stmt });
    if (statusOk(status, db, phase)) return stmt;
    finalize(stmt);
    return null;
}

fn finalize(stmt: ?*Stmt) void {
    if (stmt) |open| _ = c.zova_statement_finalize(open);
}

fn stepStatement(stmt: ?*Stmt, out: *c.zova_step_result) c.zova_status {
    return c.zova_statement_step(&c.zova_statement_step_request{ .statement = stmt, .out_result = out });
}

fn columnI64(stmt: ?*Stmt, index: c_int, out: *i64) c.zova_status {
    return c.zova_statement_column_int64(&c.zova_statement_column_int64_request{ .statement = stmt, .index = index, .out_value = out });
}

fn workspaceBindText(db: ?*Db, stmt: ?*Stmt, index: c_int, value: CStr, phase: Str) bool {
    return statusOk(c.zova_statement_bind_text(&c.zova_statement_bind_text_request{
        .statement = stmt,
        .index = index,
        .data = value,
        .len = c.strlen(value),
    }), db, phase);
}

fn workspaceBindI64(db: ?*Db, stmt: ?*Stmt, index: c_int, value: i64, phase: Str) bool {
    return statusOk(c.zova_statement_bind_int64(&c.zova_statement_bind_int64_request{
        .statement = stmt,
        .index = index,
        .value = value,
    }), db, phase);
}

/// Steps once and expects the statement to finish without a row.
fn workspaceStepDone(db: ?*Db, stmt: ?*Stmt, phase: Str) bool {
    var result: c.zova_step_result = c.ZOVA_STEP_DONE;
    return statusOk(stepStatement(stmt, &result), db, phase) and result == c.ZOVA_STEP_DONE;
}

/// Steps to the statement's row and reads int64 column 0, logging a failed
/// step or read under its phase. NULL when there is no row.
fn stepRowI64Logged(db: ?*Db, stmt: ?*Stmt, step_phase: Str, column_phase: Str) ?i64 {
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    var value: i64 = 0;
    if (!statusOk(stepStatement(stmt, &step), db, step_phase) or step != c.ZOVA_STEP_ROW) return null;
    return if (statusOk(columnI64(stmt, 0, &value), db, column_phase)) value else null;
}

/// stepRowI64Logged without the logging.
fn stepRowI64(stmt: ?*Stmt) ?i64 {
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    var value: i64 = 0;
    if (stepStatement(stmt, &step) != c.ZOVA_OK or step != c.ZOVA_STEP_ROW) return null;
    return if (columnI64(stmt, 0, &value) == c.ZOVA_OK) value else null;
}

/// Copies text column `index` into `out` with a NUL. False when the read
/// fails, the column is NULL or the text does not fit.
fn columnTextInto(db: ?*Db, stmt: ?*Stmt, index: c_int, out: []u8, phase: Str) bool {
    var text: c.zova_text = .{};
    defer c.zova_text_free(&text);
    if (!statusOk(c.zova_statement_column_text(&c.zova_statement_column_text_request{
        .statement = stmt,
        .index = index,
        .out_text = &text,
    }), db, phase)) return false;
    if (text.data == null or text.len + 1 > out.len) return false;
    @memcpy(out[0..text.len], text.data[0..text.len]);
    out[text.len] = 0;
    return true;
}

/// The CBM_ZOVA_TEST_FAIL_PHASE fault injection hook.
fn testFailPhase(phase: []const u8) bool {
    const fault: CStr = c.getenv("CBM_ZOVA_TEST_FAIL_PHASE");
    return fault != null and std.mem.eql(u8, span(fault), phase);
}

fn workspaceMetadataTableExists(db: ?*Db, table: Str) bool {
    const stmt = prepareZova(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND " ++
        "name=?1 LIMIT 1", "workspace_metadata.probe") orelse return false;
    defer finalize(stmt);
    if (!workspaceBindText(db, stmt, 1, table, "workspace_metadata.probe_bind")) return false;
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    return statusOk(stepStatement(stmt, &step), db, "workspace_metadata.probe_step") and step == c.ZOVA_STEP_ROW;
}

// ── Graph session: adjacency and degree ─────────────────────────

const AdjacencyList = struct {
    items: [*c]GraphAdjacency,
    count: c_int,
    capacity: c_int,
};

const GraphAdjacencyPayloadContext = struct {
    list: *AdjacencyList,
    max_results: c_int,
    source: CStr,
    target: CStr,
    edge_type: CStr,
    edge_type_len: usize,
};

fn graphAdjacencyAppendPayload(record: [*c]const c.cbm_zova_edge_payload_record_t, context_ptr: ?*anyopaque) callconv(.c) c_int {
    if (record == null or context_ptr == null) return -1;
    const context = cast(GraphAdjacencyPayloadContext, context_ptr);
    const list = context.list;
    if (list.count >= context.max_results) return 0;
    if (list.count == list.capacity) {
        if (list.capacity > @divTrunc(INT_MAX, 2)) return -1;
        const grown_capacity = list.capacity * 2;
        const grown = interop.reallocN(GraphAdjacency, list.items, @intCast(grown_capacity));
        if (grown == null) return -1;
        list.items = grown;
        @memset(grown[@intCast(list.capacity)..@intCast(grown_capacity)], std.mem.zeroes(GraphAdjacency));
        list.capacity = grown_capacity;
    }
    const item: *GraphAdjacency = list.items + @as(usize, @intCast(list.count));
    item.source_node_id = zvStrdup(context.source);
    item.target_node_id = zvStrdup(context.target);
    item.edge_type = zvStrndup(context.edge_type, context.edge_type_len);
    item.properties = zvStrndup(record.*.properties.data, record.*.properties.len);
    if (item.source_node_id == null or item.target_node_id == null or item.edge_type == null or item.properties == null) {
        c.free(item.source_node_id);
        c.free(item.target_node_id);
        c.free(item.edge_type);
        c.free(item.properties);
        item.* = std.mem.zeroes(GraphAdjacency);
        return -1;
    }
    list.count += 1;
    return 0;
}

/// The Zova neighbor directions "outbound", "inbound" or "both" name.
fn neighborDirections(direction: Str, dirs: *[2]c_int) []const c_int {
    const text = span(direction);
    const both = std.mem.eql(u8, text, "both");
    var count: usize = 0;
    if (both or std.mem.eql(u8, text, "outbound")) {
        dirs[count] = c.ZOVA_GRAPH_NEIGHBOR_OUTGOING;
        count += 1;
    }
    if (both or std.mem.eql(u8, text, "inbound")) {
        dirs[count] = c.ZOVA_GRAPH_NEIGHBOR_INCOMING;
        count += 1;
    }
    return dirs[0..count];
}

/// True when `graph_name` is the graph of `workspace_id`.
fn graphNameMatches(workspace_id: ?Str, graph_name: ?Str) bool {
    var expected: [ID_MAX + 32]u8 = undefined;
    const name = graph_name orelse return false;
    return cbm_zova_workspace_graph_name(workspace_id, &expected, expected.len) == 0 and
        c.strcmp(name, &expected) == 0;
}

/// Appends the edges of one (direction, edge type) neighbor read to `list`.
fn graphAdjacencyAppend(
    db: ?*Db,
    graph_name: Str,
    node_id: Str,
    direction: c_int,
    edge_type: CStr,
    max_results: c_int,
    list: *AdjacencyList,
) bool {
    var results: c.zova_graph_keyed_neighbor_results = .{};
    defer c.zova_graph_keyed_neighbor_results_free(&results);
    var payloads: c.zova_graph_edge_payload_results = .{};
    defer c.zova_graph_edge_payload_results_free(&payloads);
    if (!statusOk(c.zova_graph_neighbors_keyed(&c.zova_graph_neighbors_keyed_request{
        .db = db,
        .graph_name = graph_name,
        .node_id = node_id,
        .direction = direction,
        .edge_type = edge_type,
        .limit = @intCast(max_results - list.count),
        .out_results = &results,
    }), db, "graph.adjacency")) return false;
    if (results.len == 0) return true;

    const edge_keys = interop.callocN(i64, results.len);
    defer c.free(edge_keys);
    if (edge_keys == null) return false;
    const neighbors = results.items[0..results.len];
    for (neighbors, edge_keys[0..results.len]) |*neighbor, *edge_key| edge_key.* = neighbor.edge_key;
    if (!statusOk(c.zova_graph_edge_payload_get_many(&c.zova_graph_edge_payload_get_many_request{
        .db = db,
        .graph_name = graph_name,
        .edge_keys = edge_keys,
        .key_count = results.len,
        .out_results = &payloads,
    }), db, "graph.adjacency.payloads")) return false;
    if (payloads.len != results.len) return false;

    const outgoing = direction == c.ZOVA_GRAPH_NEIGHBOR_OUTGOING;
    for (neighbors, payloads.items[0..payloads.len]) |*neighbor, *payload| {
        if (list.count >= max_results) break;
        if (payload.found == 0 or payload.edge_key != neighbor.edge_key) return false;
        var context: GraphAdjacencyPayloadContext = .{
            .list = list,
            .max_results = max_results,
            .source = if (outgoing) node_id else neighbor.node_id,
            .target = if (outgoing) neighbor.node_id else node_id,
            .edge_type = neighbor.edge_type,
            .edge_type_len = neighbor.edge_type_len,
        };
        if (c.cbm_zova_edge_payload_visit(payload.payload, payload.payload_len, graphAdjacencyAppendPayload, &context, null) != 0) return false;
    }
    return true;
}

pub export fn cbm_zova_graph_session_adjacency(
    session: ?*GraphSession,
    workspace_id: ?Str,
    graph_name: ?Str,
    node_id: ?Str,
    direction: ?Str,
    edge_types: [*c]const CStr,
    edge_type_count: c_int,
    max_results: c_int,
    out: ?*[*c]GraphAdjacency,
    out_count: ?*c_int,
) c_int {
    if (out) |items| items.* = null;
    if (out_count) |count| count.* = 0;
    if (!with_zova) return -1;
    const graph_session = session orelse return -1;
    const node = nonEmpty(node_id) orelse return -1;
    const direction_name = direction orelse return -1;
    if (!workspaceNameComponentValid(workspace_id) or !graphNameMatches(workspace_id, graph_name) or
        edge_type_count < 0 or (edge_type_count != 0 and edge_types == null) or
        max_results <= 0 or out == null or out_count == null) return -1;
    var dirs_buf: [2]c_int = undefined;
    const dirs = neighborDirections(direction_name, &dirs_buf);
    if (dirs.len == 0) return -1;

    const loops: usize = if (edge_type_count != 0) @intCast(edge_type_count) else 1;
    var list: AdjacencyList = .{ .items = interop.callocN(GraphAdjacency, 8), .count = 0, .capacity = 8 };
    if (list.items == null) return -1;
    for (dirs) |dir| {
        for (0..loops) |t| {
            const edge_type: CStr = if (edge_type_count != 0) edge_types[t] else null;
            if (!graphAdjacencyAppend(graph_session.zdb.db, graph_name.?, node, dir, edge_type, max_results, &list)) {
                cbm_zova_graph_adjacency_free(list.items, list.count);
                return -1;
            }
        }
    }
    out.?.* = list.items;
    out_count.?.* = list.count;
    return 0;
}

pub export fn cbm_zova_graph_session_degree(
    session: ?*GraphSession,
    workspace_id: ?Str,
    graph_name: ?Str,
    node_id: ?Str,
    direction: ?Str,
    edge_types: [*c]const CStr,
    edge_type_count: c_int,
    out_degree: ?*c_int,
) c_int {
    if (!with_zova) {
        if (out_degree) |out| out.* = 0;
        return -1;
    }
    const out = out_degree orelse return -1;
    out.* = 0;
    const graph_session = session orelse return -1;
    const node = node_id orelse return -1;
    const direction_name = direction orelse return -1;
    if (!graphNameMatches(workspace_id, graph_name) or edge_type_count < 0 or
        (edge_type_count != 0 and edge_types == null)) return -1;
    var dirs_buf: [2]c_int = undefined;
    const dirs = neighborDirections(direction_name, &dirs_buf);
    if (dirs.len == 0) return -1;

    const db = graph_session.zdb.db;
    const loops: usize = if (edge_type_count != 0) @intCast(edge_type_count) else 1;
    var total: u64 = 0;
    for (dirs) |dir| {
        for (0..loops) |t| {
            var degree: u64 = 0;
            if (!statusOk(c.zova_graph_degree(&c.zova_graph_degree_request{
                .db = db,
                .graph_name = graph_name.?,
                .node_id = node,
                .direction = dir,
                .edge_type = if (edge_type_count != 0) edge_types[t] else null,
                .out_degree = &degree,
            }), db, "graph.degree") or degree > @as(u64, @intCast(INT_MAX)) - total) return -1;
            total += degree;
        }
    }
    out.* = @intCast(total);
    return 0;
}

pub export fn cbm_zova_graph_session_adjacency_prepare_count(_: ?*const GraphSession) c_int {
    return 0;
}

// ── Database format and open ────────────────────────────────────

/// Raw, read-only check of the Zova format version and the CBM schema
/// version, made before the pinned Zova SDK opens the file.
fn databaseFormatStatus(maybe_path: ?Str) FormatStatus {
    const path = nonEmpty(maybe_path) orelse return c.CBM_ZOVA_DATABASE_INCOMPATIBLE;
    var db: ?*c.sqlite3 = null;
    var stmt: ?*c.sqlite3_stmt = null;
    defer {
        _ = c.sqlite3_finalize(stmt);
        if (db != null) {
            _ = c.sqlite3_exec(db, "ROLLBACK", null, null, null);
            _ = c.sqlite3_close(db);
        }
    }
    if (c.sqlite3_open_v2(path, &db, c.SQLITE_OPEN_READONLY | c.SQLITE_OPEN_NOMUTEX, null) != c.SQLITE_OK or
        c.sqlite3_exec(db, "PRAGMA query_only=ON;BEGIN", null, null, null) != c.SQLITE_OK or
        c.sqlite3_prepare_v2(db, "SELECT (SELECT value FROM _zova_meta WHERE key='format_version')," ++
            "(SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1)", -1, &stmt, null) != c.SQLITE_OK or
        c.sqlite3_step(stmt) != c.SQLITE_ROW or c.sqlite3_column_type(stmt, 0) != c.SQLITE_TEXT or
        c.sqlite3_column_type(stmt, 1) != c.SQLITE_INTEGER)
    {
        return c.CBM_ZOVA_DATABASE_INCOMPATIBLE;
    }
    const format_text: CStr = c.sqlite3_column_text(stmt, 0);
    const schema = c.sqlite3_column_int64(stmt, 1);
    if (format_text == null) return c.CBM_ZOVA_DATABASE_INCOMPATIBLE;
    const format = span(format_text);
    const eql = std.mem.eql;
    if (eql(u8, format, "13") and schema == SCHEMA_VERSION) return c.CBM_ZOVA_DATABASE_COMPATIBLE;
    if ((eql(u8, format, "9") or eql(u8, format, "10") or eql(u8, format, "11") or eql(u8, format, "12")) and
        schema == SCHEMA_VERSION)
    {
        return c.CBM_ZOVA_DATABASE_MIGRATION_REQUIRED;
    }
    if (eql(u8, format, "7") and schema == 5) return c.CBM_ZOVA_DATABASE_REPACK_REQUIRED;
    return c.CBM_ZOVA_DATABASE_INCOMPATIBLE;
}

const repack_phase_names = [_]Str{
    "source_snapshot",         "temp_creation",       "workspace_publish", "verification", "fsync",
    "live_to_recovery_rename", "temp_to_live_rename", "final_reopen",
};

/// Registers the SQL functions and turns foreign keys on for a fresh handle.
fn prepareHandle(db: ?*Db) bool {
    return registerSqlFunctions(db) and execSql(db, "PRAGMA foreign_keys=ON") == c.ZOVA_OK;
}

fn openZova(maybe_path: ?Str, read_only: bool, out: *ZovaDb) bool {
    out.* = .{};
    const path = maybe_path orelse return false;
    const format_status = databaseFormatStatus(path);
    if (format_status == c.CBM_ZOVA_DATABASE_REPACK_REQUIRED) {
        interop.log(c.CBM_LOG_ERROR, "zova.open", .{ "path", path, "status", "repack_required" });
        return false;
    }
    if (format_status == c.CBM_ZOVA_DATABASE_MIGRATION_REQUIRED) {
        interop.log(c.CBM_LOG_ERROR, "zova.open", .{ "path", path, "status", "format_migration_required" });
        return false;
    }
    var err: c.zova_message = .{};
    defer c.zova_message_free(&err);
    const status = if (read_only)
        c.zova_database_open_with_options(&c.zova_database_open_options_request{
            .path = path,
            .flags = c.ZOVA_OPEN_READ_ONLY,
            .busy_timeout_ms = 5000,
            .out_db = &out.db,
            .out_error_message = &err,
        })
    else
        c.zova_database_open(&c.zova_database_open_request{
            .path = path,
            .out_db = &out.db,
            .out_error_message = &err,
        });
    if (status != c.ZOVA_OK) {
        interop.log(c.CBM_LOG_ERROR, "zova.open", .{
            "path",   path,
            "status", c.zova_status_name(status),
            "msg",    if (err.data != null) @as(CStr, err.data) else @as(CStr, ""),
        });
        return false;
    }
    user_publish_test_metrics.database_handle_open_count += 1;
    if (!prepareHandle(out.db)) {
        closeZova(out);
        return false;
    }
    return true;
}

/// Runs one capability probe without logging expected feature failures and
/// says whether it produced a row; NULL when the statement failed. A
/// successful statement with no rows is still a successful capability (for
/// example BM25 on an empty FTS table).
fn zovaProbeSql(db: ?*Db, sql: Str) ?bool {
    if (db == null) return null;
    var stmt: ?*Stmt = null;
    if (c.zova_database_prepare(&c.zova_database_prepare_request{ .db = db, .sql = sql, .out_statement = &stmt }) != c.ZOVA_OK or stmt == null) {
        return null;
    }
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    const stepped = stepStatement(stmt, &step) == c.ZOVA_OK;
    if (c.zova_statement_finalize(stmt) != c.ZOVA_OK or !stepped) return null;
    return step == c.ZOVA_STEP_ROW;
}

/// True when the probe ran and produced a row.
fn zovaProbeRow(db: ?*Db, sql: Str) bool {
    return zovaProbeSql(db, sql) orelse false;
}

pub export fn cbm_zova_probe_sql_capabilities(zova_path: ?Str, out_capabilities: ?*SqlCapabilities) c_int {
    if (!with_zova) {
        if (out_capabilities) |out| out.* = std.mem.zeroes(SqlCapabilities);
        return -1;
    }
    const path = nonEmpty(zova_path) orelse return -1;
    const out = out_capabilities orelse return -1;
    out.* = std.mem.zeroes(SqlCapabilities);
    var zdb: ZovaDb = .{};
    if (!openZova(path, false, &zdb)) return -1;
    defer closeZova(&zdb);
    const db = zdb.db;

    out.canonical_workspace_metadata = zovaProbeRow(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND " ++
        "name='cbm_nodes_v1' LIMIT 1");

    // Exercise the complete FTS surface on a disposable table instead of
    // relying on a converted sidecar containing a particular application
    // table.  This covers creation, insert, MATCH, update and delete.
    const fts_setup =
        "DROP TABLE IF EXISTS cbm_zova_capability_fts_v1;" ++
        "CREATE VIRTUAL TABLE cbm_zova_capability_fts_v1 USING fts5(" ++
        "workspace_id UNINDEXED,node_id UNINDEXED,name,qualified_name,file_path,label," ++
        "tokenize='unicode61 remove_diacritics 2');" ++
        "INSERT INTO cbm_zova_capability_fts_v1" ++
        "(workspace_id,node_id,name,qualified_name,file_path,label) " ++
        "VALUES('w1','n1','AlphaBeta','pkg::AlphaBeta','src/a.c','Function');";
    const fts_status = execSql(db, fts_setup);
    var fts_surface = fts_status == c.ZOVA_OK;
    if (!fts_surface) {
        interop.log(c.CBM_LOG_ERROR, "zova.capability", .{
            "feature", "fts5",
            "phase",   "create_insert",
            "status",  c.zova_status_name(fts_status),
            "msg",     c.zova_database_last_error_message(db),
        });
    }
    if (fts_surface) {
        fts_surface = zovaProbeRow(db, "SELECT rowid FROM cbm_zova_capability_fts_v1 " ++
            "WHERE cbm_zova_capability_fts_v1 MATCH 'AlphaBeta' LIMIT 1");
    }
    if (fts_surface) {
        const fts_update =
            "UPDATE cbm_zova_capability_fts_v1 SET name='BetaGamma' WHERE rowid=1;" ++
            "DELETE FROM cbm_zova_capability_fts_v1 WHERE rowid=1;";
        const update_status = execSql(db, fts_update);
        fts_surface = update_status == c.ZOVA_OK and
            zovaProbeRow(db, "SELECT count(*) FROM cbm_zova_capability_fts_v1");
        if (!fts_surface and update_status != c.ZOVA_OK) {
            interop.log(c.CBM_LOG_ERROR, "zova.capability", .{
                "feature", "fts5",
                "phase",   "update_delete",
                "status",  c.zova_status_name(update_status),
                "msg",     c.zova_database_last_error_message(db),
            });
        }
    }
    out.fts5 = fts_surface;
    if (fts_surface) {
        fts_surface = execSql(db, "INSERT INTO cbm_zova_capability_fts_v1" ++
            "(workspace_id,node_id,name,qualified_name,file_path,label) " ++
            "VALUES('w1','n2','Alpha','pkg::Alpha','src/b.c','Function');") == c.ZOVA_OK;
    }
    out.bm25 = fts_surface and zovaProbeRow(db, "SELECT bm25(cbm_zova_capability_fts_v1) " ++
        "FROM cbm_zova_capability_fts_v1 " ++
        "WHERE cbm_zova_capability_fts_v1 MATCH 'Alpha' LIMIT 1");
    _ = execSql(db, "DROP TABLE IF EXISTS cbm_zova_capability_fts_v1;");

    out.json = zovaProbeRow(db, "SELECT json_extract('{\"workspace_id\":\"w1\",\"value\":1}', " ++
        "'$.value') WHERE json_extract('{\"workspace_id\":\"w1\",\"value\":1}', " ++
        "'$.workspace_id') = 'w1'");
    out.recursive_cte = zovaProbeRow(db, "WITH RECURSIVE n(workspace_id,x) AS (VALUES('w1',1) UNION ALL " ++
        "SELECT workspace_id,x+1 FROM n WHERE workspace_id='w1' AND x<2) " ++
        "SELECT count(*) FROM n WHERE workspace_id='w1'");

    const function_probes = [_]Str{
        "SELECT cbm_camel_split('AlphaBeta')",
        "SELECT regexp('alpha', 'Alpha')",
        "SELECT iregexp('alpha', 'Alpha')",
        "SELECT cbm_cosine_i8(x'01', x'01')",
    };
    out.cbm_scalar_functions = true;
    for (function_probes) |probe| {
        if (!zovaProbeRow(db, probe)) {
            out.cbm_scalar_functions = false;
            break;
        }
    }

    out.transactions = execSql(db, "BEGIN; CREATE TEMP TABLE cbm_zova_capability_probe(x INTEGER); " ++
        "INSERT INTO cbm_zova_capability_probe VALUES(1); ROLLBACK;") == c.ZOVA_OK;
    return 0;
}

/// Enables WAL on a database Zova just created and verifies its 64 KiB pages.
fn configureNewZovaDatabase(db: ?*Db) bool {
    if (!statusOk(execSql(db, "PRAGMA journal_mode=WAL"), db, "zova.journal_mode_configure")) return false;
    const stmt = prepareZova(db, "PRAGMA page_size", "zova.page_size_prepare") orelse return false;
    const page_size = stepRowI64Logged(db, stmt, "zova.page_size_step", "zova.page_size_column");
    if (c.zova_statement_finalize(stmt) != c.ZOVA_OK) return false;
    return (page_size orelse return false) == 65536;
}

fn openOrCreateZova(path: Str, out: *ZovaDb) bool {
    if (c.cbm_file_exists(path)) return openZova(path, false, out);
    out.* = .{};
    var err: c.zova_message = .{};
    defer c.zova_message_free(&err);
    const status = c.zova_database_create_with_options(&c.zova_database_create_options_request{
        .path = path,
        .page_size = 65536,
        .out_db = &out.db,
        .out_error_message = &err,
    });
    if (status != c.ZOVA_OK) {
        interop.log(c.CBM_LOG_ERROR, "zova.registry_create", .{
            "path",   path,
            "status", c.zova_status_name(status),
            "msg",    if (err.data != null) @as(CStr, err.data) else @as(CStr, ""),
        });
        return false;
    }
    user_publish_test_metrics.database_handle_open_count += 1;
    // Zova selected 64 KiB pages before creating its private catalog. Keep the
    // new handle, enable WAL, and verify the effective page size in place.
    if (!configureNewZovaDatabase(out.db) or !prepareHandle(out.db)) {
        closeZova(out);
        _ = c.cbm_unlink(path);
        return false;
    }
    return true;
}

// ── Workspace registry ──────────────────────────────────────────

fn workspaceRegistryInit(db: ?*Db) bool {
    const sql =
        "CREATE TABLE IF NOT EXISTS cbm_workspace_registry(" ++
        "workspace_key INTEGER PRIMARY KEY, workspace_id TEXT NOT NULL UNIQUE, " ++
        "canonical_root TEXT NOT NULL UNIQUE, " ++
        "id_format_version INTEGER NOT NULL, active_generation INTEGER NOT NULL DEFAULT 0);";
    return statusOk(execSql(db, sql), db, "workspace.registry_init");
}

fn workspaceLegacyGenerationInit(db: ?*Db) bool {
    // The normal single-file schema uses cbm_database_generation_v1. Keep this
    // table out of fresh databases unless an old sidecar coordination API is
    // explicitly invoked.
    const sql =
        "CREATE TABLE IF NOT EXISTS cbm_workspace_generations(" ++
        "workspace_key INTEGER NOT NULL, generation INTEGER NOT NULL, state TEXT NOT NULL " ++
        "CHECK(state IN ('building','ready','failed','retired')), " ++
        "PRIMARY KEY(workspace_key,generation), " ++
        "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) " ++
        "ON DELETE CASCADE) WITHOUT ROWID;";
    return statusOk(execSql(db, sql), db, "workspace.legacy_generation_init");
}

fn workspaceRegistryBegin(db: ?*Db) bool {
    return statusOk(c.zova_database_begin_immediate(&c.zova_database_simple_request{ .db = db }), db, "workspace.begin");
}

fn workspaceRegistryCommit(db: ?*Db) bool {
    return statusOk(c.zova_database_commit(&c.zova_database_simple_request{ .db = db }), db, "workspace.commit");
}

fn workspaceRegistryRollback(db: ?*Db) void {
    _ = c.zova_database_rollback(&c.zova_database_simple_request{ .db = db });
}

// ── User database schema ────────────────────────────────────────

/// The first int64 of the first row of `sql`; NULL when it has none.
fn userSchemaScalar(db: ?*Db, sql: Str) ?i64 {
    if (db == null) return null;
    const stmt = prepareZova(db, sql, "user_database.schema_probe") orelse return null;
    defer finalize(stmt);
    return stepRowI64(stmt);
}

fn userSchemaScalarIs(db: ?*Db, sql: Str, expected: i64) bool {
    return (userSchemaScalar(db, sql) orelse return false) == expected;
}

fn userSchemaV3IsWellFormed(db: ?*Db) bool {
    const sql =
        "SELECT count(*) FROM sqlite_master WHERE type IN ('table','view') AND name IN (" ++
        "'cbm_workspace_registry','cbm_database_generation_v1'," ++
        "'cbm_projects_v1','cbm_files_v1','cbm_nodes_v1','cbm_file_hashes_v1'," ++
        "'cbm_token_vector_metadata_v1','cbm_nodes_fts_v1'," ++
        "'cbm_workspace_index_state_v1','cbm_project_summaries_v2'," ++
        "'cbm_generation_integrity_v2')";
    return userSchemaScalarIs(db, sql, 11);
}

fn userSchemaV4IsWellFormed(db: ?*Db) bool {
    return userSchemaV3IsWellFormed(db) and
        userSchemaScalarIs(db, "SELECT count(*) FROM pragma_table_info('cbm_workspace_migrations_v1')", 17);
}

fn userDatabaseSchemaIsCurrent(db: ?*Db) bool {
    return userSchemaV4IsWellFormed(db) and
        // columns
        userSchemaScalarIs(db, "SELECT count(*) FROM pragma_table_info('cbm_workspace_health_v1')", 5) and
        // exact_columns
        userSchemaScalarIs(db, "SELECT count(*) FROM pragma_table_info('cbm_workspace_health_v1') " ++
            "WHERE (cid=0 AND name='workspace_key' AND type='INTEGER' AND pk=1 AND " ++
            "dflt_value IS NULL) OR " ++
            "(cid=1 AND name='state' AND type='TEXT' AND \"notnull\"=1 AND pk=0 AND " ++
            "dflt_value IS NULL) OR " ++
            "(cid=2 AND name='reason' AND type='TEXT' AND \"notnull\"=1 AND pk=0 AND " ++
            "dflt_value IS NULL) OR " ++
            "(cid=3 AND name='checked_generation' AND type='INTEGER' AND \"notnull\"=1 " ++
            "AND pk=0 AND dflt_value IS NULL) OR " ++
            "(cid=4 AND name='checked_at' AND type='TEXT' AND \"notnull\"=1 AND pk=0 AND " ++
            "dflt_value IS NULL)", 5) and
        // foreign_keys
        userSchemaScalarIs(db, "SELECT count(*) FROM pragma_foreign_key_list(" ++
            "'cbm_workspace_health_v1') WHERE \"table\"='cbm_workspace_registry' " ++
            "AND \"from\"='workspace_key' AND \"to\"='workspace_key'", 1) and
        // state_constraint
        userSchemaScalarIs(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND " ++
            "name='cbm_workspace_health_v1' AND " ++
            "instr(sql,\"state IN ('healthy','rebuild_required')\") > 0", 1) and
        // token_columns
        userSchemaScalarIs(db, "SELECT count(*) FROM pragma_table_info(" ++
            "'cbm_token_vector_metadata_v1') WHERE " ++
            "(cid=0 AND name='token_key' AND type='INTEGER' AND pk=1) OR " ++
            "(cid=1 AND name='workspace_key' AND type='INTEGER' AND \"notnull\"=1) OR " ++
            "(cid=2 AND name='token' AND type='TEXT' AND \"notnull\"=1) OR " ++
            "(cid=3 AND name='idf' AND type='REAL' AND \"notnull\"=1)", 4) and
        // topology_columns
        userSchemaScalarIs(db, "SELECT count(*) FROM (" ++
            "SELECT 1 FROM pragma_table_info('cbm_nodes_v1') WHERE " ++
            "(cid=0 AND name='zova_node_key' AND type='INTEGER' AND pk=1) OR " ++
            "(cid=1 AND name='workspace_key' AND type='INTEGER' AND \"notnull\"=1) OR " ++
            "(cid=2 AND name='label' AND type='TEXT' AND \"notnull\"=1) OR " ++
            "(cid=3 AND name='name' AND type='TEXT' AND \"notnull\"=1) OR " ++
            "(cid=4 AND name='qualified_name' AND type='TEXT' AND \"notnull\"=1) OR " ++
            "(cid=5 AND name='file_key' AND type='INTEGER' AND \"notnull\"=1) OR " ++
            "(cid=6 AND name='start_line' AND type='INTEGER' AND \"notnull\"=1) OR " ++
            "(cid=7 AND name='end_line' AND type='INTEGER' AND \"notnull\"=1) OR " ++
            "(cid=8 AND name='properties' AND type='TEXT' AND \"notnull\"=1) OR " ++
            "(cid=9 AND name='source_ordinal' AND type='INTEGER' AND \"notnull\"=1))", 10) and
        // compact_foreign_keys
        userSchemaScalarIs(db, "SELECT count(*) FROM (" ++
            "SELECT 1 FROM pragma_foreign_key_list('cbm_nodes_v1') " ++
            "WHERE \"table\"='cbm_files_v1' AND ((seq=0 AND \"from\"='workspace_key' " ++
            "AND \"to\"='workspace_key') OR (seq=1 AND \"from\"='file_key' " ++
            "AND \"to\"='file_key')))", 2) and
        // compact_child_indexes
        userSchemaScalarIs(db, "SELECT count(*) FROM sqlite_master WHERE type='index' AND " ++
            "name='cbm_nodes_v1_workspace_file'", 1) and
        // guard_triggers
        userSchemaScalarIs(db, "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name IN (" ++
            "'cbm_nodes_v1_file_workspace_bi','cbm_nodes_v1_file_workspace_bu'," ++
            "'cbm_edges_v1_endpoint_workspace_bi'," ++
            "'cbm_edges_v1_endpoint_workspace_bu')", 0) and
        // forbidden_objects
        userSchemaScalarIs(db, "SELECT count(*) FROM sqlite_master WHERE " ++
            "name IN ('cbm_edges_v1','cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1'," ++
            "'cbm_fts_rowmap_v1','cbm_node_vectors_compat_v1'," ++
            "'cbm_token_vectors_compat_v1','_zova_vector_norms') " ++
            "OR name GLOB 'cbm_fts_w1_*'", 0);
}

const user_database_schema_sql =
    "CREATE TABLE IF NOT EXISTS cbm_database_schema_v1(" ++
    "id INTEGER PRIMARY KEY CHECK(id = 1)," ++
    "schema_version INTEGER NOT NULL," ++
    "metadata_projection_version INTEGER NOT NULL," ++
    "edge_metadata_projection_version INTEGER NOT NULL," ++
    "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);" ++
    "INSERT OR IGNORE INTO cbm_database_schema_v1" ++
    "(id,schema_version,metadata_projection_version,edge_metadata_projection_version)" ++
    "VALUES(1,7,0,0);" ++
    "CREATE TABLE IF NOT EXISTS cbm_database_generation_v1(" ++
    "workspace_key INTEGER NOT NULL, generation INTEGER NOT NULL," ++
    "state TEXT NOT NULL CHECK(state IN ('building','ready','failed','retired'))," ++
    "PRIMARY KEY(workspace_key,generation)," ++
    "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) " ++
    "ON DELETE CASCADE) WITHOUT ROWID;" ++
    "CREATE TABLE IF NOT EXISTS cbm_projects_v1(" ++
    "workspace_key INTEGER PRIMARY KEY, project TEXT NOT NULL, root_path TEXT NOT NULL," ++
    "indexed_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP," ++
    "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) " ++
    "ON DELETE CASCADE);" ++
    "CREATE TABLE IF NOT EXISTS cbm_files_v1(" ++
    "file_key INTEGER PRIMARY KEY, workspace_key INTEGER NOT NULL, file_path TEXT NOT NULL," ++
    "UNIQUE(workspace_key,file_path),UNIQUE(workspace_key,file_key)," ++
    "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) " ++
    "ON DELETE CASCADE);" ++
    "CREATE TABLE IF NOT EXISTS cbm_nodes_v1(" ++
    "zova_node_key INTEGER PRIMARY KEY, workspace_key INTEGER NOT NULL," ++
    "label TEXT NOT NULL, name TEXT NOT NULL, qualified_name TEXT NOT NULL," ++
    "file_key INTEGER NOT NULL, start_line INTEGER NOT NULL, end_line INTEGER NOT NULL," ++
    "properties TEXT NOT NULL DEFAULT '{}',source_ordinal INTEGER NOT NULL DEFAULT 0 " ++
    "CHECK(source_ordinal>=0)," ++
    "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) " ++
    "ON DELETE CASCADE," ++
    "FOREIGN KEY(workspace_key,file_key) " ++
    "REFERENCES cbm_files_v1(workspace_key,file_key));" ++
    "CREATE INDEX IF NOT EXISTS cbm_nodes_v1_workspace_file " ++
    "ON cbm_nodes_v1(workspace_key,file_key);" ++
    "CREATE TABLE IF NOT EXISTS cbm_file_hashes_v1(" ++
    "file_key INTEGER PRIMARY KEY, content_hash TEXT NOT NULL," ++
    "mtime_ns INTEGER NOT NULL DEFAULT 0, size_bytes INTEGER NOT NULL DEFAULT 0," ++
    "FOREIGN KEY(file_key) REFERENCES cbm_files_v1(file_key) ON DELETE CASCADE);" ++
    "CREATE TABLE IF NOT EXISTS cbm_token_vector_metadata_v1(" ++
    "token_key INTEGER PRIMARY KEY, workspace_key INTEGER NOT NULL," ++
    "token TEXT NOT NULL, idf REAL NOT NULL, UNIQUE(workspace_key,token)," ++
    "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) " ++
    "ON DELETE CASCADE);" ++
    "CREATE VIEW IF NOT EXISTS cbm_nodes_fts_content_v1 AS " ++
    "SELECT n.zova_node_key,cbm_camel_split(n.name) AS name,n.qualified_name,f.file_path,n.label " ++
    "FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key;" ++
    "CREATE VIRTUAL TABLE IF NOT EXISTS cbm_nodes_fts_v1 USING fts5(" ++
    "name,qualified_name,file_path,label,content='cbm_nodes_fts_content_v1'," ++
    "content_rowid='zova_node_key');" ++
    node_fts_insert_trigger_sql ++
    "CREATE TRIGGER IF NOT EXISTS cbm_nodes_v1_fts_bd BEFORE DELETE ON cbm_nodes_v1 BEGIN " ++
    "INSERT INTO cbm_nodes_fts_v1(cbm_nodes_fts_v1,rowid,name,qualified_name,file_path,label) " ++
    "SELECT 'delete',OLD.zova_node_key,cbm_camel_split(OLD.name),OLD.qualified_name,f.file_path,OLD.label " ++
    "FROM cbm_files_v1 f WHERE f.file_key=OLD.file_key; END;" ++
    "CREATE TRIGGER IF NOT EXISTS cbm_nodes_v1_fts_bu " ++
    "BEFORE UPDATE OF name,qualified_name,file_key,label ON cbm_nodes_v1 BEGIN " ++
    "INSERT INTO cbm_nodes_fts_v1(cbm_nodes_fts_v1,rowid,name,qualified_name,file_path,label) " ++
    "SELECT 'delete',OLD.zova_node_key,cbm_camel_split(OLD.name),OLD.qualified_name,f.file_path,OLD.label " ++
    "FROM cbm_files_v1 f WHERE f.file_key=OLD.file_key; END;" ++
    "CREATE TRIGGER IF NOT EXISTS cbm_nodes_v1_fts_au " ++
    "AFTER UPDATE OF name,qualified_name,file_key,label ON cbm_nodes_v1 BEGIN " ++
    "INSERT INTO cbm_nodes_fts_v1(rowid,name,qualified_name,file_path,label) " ++
    "SELECT NEW.zova_node_key,cbm_camel_split(NEW.name),NEW.qualified_name,f.file_path,NEW.label " ++
    "FROM cbm_files_v1 f WHERE f.file_key=NEW.file_key; END;" ++
    "CREATE TABLE IF NOT EXISTS cbm_workspace_index_state_v1(" ++
    "workspace_key INTEGER PRIMARY KEY, generation INTEGER NOT NULL," ++
    "model_fingerprint TEXT NOT NULL, vector_dimensions INTEGER NOT NULL," ++
    "indexed_at TEXT NOT NULL," ++
    "FOREIGN KEY(workspace_key,generation) REFERENCES cbm_database_generation_v1(workspace_key,generation) " ++
    "ON DELETE CASCADE);" ++
    "CREATE TABLE IF NOT EXISTS cbm_project_summaries_v2(" ++
    "workspace_key INTEGER PRIMARY KEY, summary TEXT NOT NULL, source_hash TEXT NOT NULL," ++
    "created_at TEXT NOT NULL, updated_at TEXT NOT NULL," ++
    "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) " ++
    "ON DELETE CASCADE);" ++
    "CREATE TABLE IF NOT EXISTS cbm_generation_integrity_v2(" ++
    "workspace_key INTEGER NOT NULL, generation INTEGER NOT NULL," ++
    "graph_nodes INTEGER NOT NULL, graph_edges INTEGER NOT NULL," ++
    "metadata_nodes INTEGER NOT NULL, metadata_edges INTEGER NOT NULL," ++
    "metadata_topology_edges INTEGER NOT NULL, fts_rows INTEGER NOT NULL," ++
    "node_vector_rows INTEGER NOT NULL, token_vector_rows INTEGER NOT NULL," ++
    "node_vectors INTEGER NOT NULL, token_vectors INTEGER NOT NULL," ++
    "metadata_sha256 TEXT NOT NULL, fts_sha256 TEXT NOT NULL," ++
    "topology_sha256 TEXT NOT NULL, node_vector_sha256 TEXT NOT NULL," ++
    "token_vector_sha256 TEXT NOT NULL," ++
    "PRIMARY KEY(workspace_key,generation)," ++
    "FOREIGN KEY(workspace_key,generation) REFERENCES cbm_database_generation_v1(workspace_key,generation) " ++
    "ON DELETE CASCADE) WITHOUT ROWID;" ++
    "CREATE TABLE IF NOT EXISTS cbm_workspace_migrations_v1(" ++
    "workspace_key INTEGER PRIMARY KEY," ++
    "migration_version INTEGER NOT NULL CHECK(migration_version=1)," ++
    "project TEXT NOT NULL,root_path TEXT NOT NULL," ++
    "source_db_path TEXT NOT NULL,source_zova_path TEXT NOT NULL," ++
    "source_generation INTEGER NOT NULL,target_generation INTEGER NOT NULL DEFAULT 0," ++
    "state TEXT NOT NULL CHECK(state IN ('prepared','copying','active','failed'," ++
    "'rolled_back','cleanup_pending','retired'))," ++
    "metadata_sha256 TEXT NOT NULL,fts_sha256 TEXT NOT NULL," ++
    "topology_sha256 TEXT NOT NULL,node_vector_sha256 TEXT NOT NULL," ++
    "token_vector_sha256 TEXT NOT NULL,prepared_at TEXT NOT NULL," ++
    "activated_at TEXT,retired_at TEXT," ++
    "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) " ++
    "ON DELETE CASCADE);" ++
    "CREATE TABLE IF NOT EXISTS cbm_workspace_health_v1(" ++
    "workspace_key INTEGER PRIMARY KEY," ++
    "state TEXT NOT NULL CHECK(state IN ('healthy','rebuild_required'))," ++
    "reason TEXT NOT NULL,checked_generation INTEGER NOT NULL,checked_at TEXT NOT NULL," ++
    "FOREIGN KEY(workspace_key) REFERENCES cbm_workspace_registry(workspace_key) " ++
    "ON DELETE CASCADE);";

fn userDatabaseEnsureSchema(db: ?*Db) bool {
    if (db == null) return false;
    const schema_table_count = userSchemaScalar(db, "SELECT count(*) FROM sqlite_master WHERE type='table' " ++
        "AND name='cbm_database_schema_v1'") orelse return false;
    if (schema_table_count != 0 and schema_table_count != 1) return false;
    var schema_version: i64 = 0;
    if (schema_table_count == 1) {
        schema_version = userSchemaScalar(db, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1") orelse return false;
        if (schema_version < 1 or schema_version > SCHEMA_VERSION) return false;
    }
    if (schema_version == SCHEMA_VERSION) return userDatabaseSchemaIsCurrent(db);
    // Every pre-v7 file is immutable repack input. Never run partial in-place
    // DDL against a live v5/Zova-v7 database.
    if (schema_table_count == 1) return false;
    if (!workspaceRegistryBegin(db) or !workspaceRegistryInit(db)) {
        workspaceRegistryRollback(db);
        return false;
    }

    var ok = statusOk(execSql(db, user_database_schema_sql), db, "user_database.schema_init");
    if (ok and testFailPhase("schema_v6_before_commit")) ok = false;
    ok = ok and workspaceRegistryCommit(db);
    if (!ok) workspaceRegistryRollback(db);
    return ok;
}

/// Bootstrap the SQL portion of the user-local cbm.zova database. Publication
/// callers reuse userDatabaseEnsureSchema() on their existing handle.
pub export fn cbm_zova_user_database_init(zova_path: ?Str) c_int {
    if (!with_zova) return -1;
    const path = nonEmpty(zova_path) orelse return -1;
    var zdb: ZovaDb = .{};
    if (!openOrCreateZova(path, &zdb)) return -1;
    user_publish_test_metrics.database_open_count += 1;
    const ok = userDatabaseEnsureSchema(zdb.db);
    closeZova(&zdb);
    user_publish_test_metrics.database_close_count += 1;
    return if (ok) 0 else -1;
}

fn verifyWorkspaceSchema(db: ?*Db) bool {
    if (db == null) return false;
    const Check = struct { sql: Str, expected: i64 };
    const checks = [_]Check{
        .{ .sql = "PRAGMA foreign_keys", .expected = 1 },
        .{ .sql = "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1", .expected = SCHEMA_VERSION },
        .{ .sql = "SELECT count(*) FROM pragma_foreign_key_list('cbm_nodes_v1') " ++
            "WHERE \"table\"='cbm_files_v1'", .expected = 2 },
    };
    for (checks) |check| {
        const stmt = prepareZova(db, check.sql, "workspace_schema.verify_prepare") orelse return false;
        defer finalize(stmt);
        if ((stepRowI64(stmt) orelse return false) != check.expected) return false;
    }
    const violation = zovaProbeSql(db, "PRAGMA foreign_key_check") orelse return false;
    return !violation;
}

pub export fn cbm_zova_user_database_generation_begin(
    zova_path: ?Str,
    root_path: ?Str,
    generation: i64,
    out_workspace_id: ?[*]u8,
    out_workspace_id_size: usize,
) c_int {
    if (!with_zova) return -1;
    const path = nonEmpty(zova_path) orelse return -1;
    const out = out_workspace_id orelse return -1;
    if (nonEmpty(root_path) == null or generation <= 0 or out_workspace_id_size < ID_MAX) return -1;
    out[0] = 0;
    if (cbm_zova_user_database_init(path) != 0 or
        cbm_zova_workspace_get_or_create_at(path, root_path, out, out_workspace_id_size) != 0) return -1;

    var zdb: ZovaDb = .{};
    defer closeZova(&zdb);
    if (!openOrCreateZova(path, &zdb) or !workspaceRegistryBegin(zdb.db)) return -1;
    const db = zdb.db;
    const stmt = prepareZova(db, "INSERT INTO cbm_database_generation_v1(workspace_key,generation,state) " ++
        "SELECT workspace_key,?2,'building' FROM cbm_workspace_registry WHERE workspace_id=?1 " ++
        "ON CONFLICT(workspace_key,generation) DO UPDATE SET state='building'", "user_generation.begin_prepare");
    var ok = stmt != null and
        workspaceBindText(db, stmt, 1, out, "user_generation.begin_bind_workspace") and
        workspaceBindI64(db, stmt, 2, generation, "user_generation.begin_bind_generation") and
        workspaceStepDone(db, stmt, "user_generation.begin_step");
    finalize(stmt);
    ok = ok and workspaceRegistryCommit(db);
    if (!ok) {
        workspaceRegistryRollback(db);
        out[0] = 0;
    }
    return if (ok) 0 else -1;
}

/// Whether the workspace's row for `generation` is in state "building".
fn userGenerationIsBuilding(db: ?*Db, workspace_id: Str, generation: i64) bool {
    const stmt = prepareZova(db, "SELECT state FROM cbm_database_generation_v1 " ++
        "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry " ++
        "WHERE workspace_id=?1) AND generation=?2", "user_generation.finish_lookup_prepare") orelse return false;
    defer finalize(stmt);
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    var state: [16]u8 = @splat(0);
    return workspaceBindText(db, stmt, 1, workspace_id, "user_generation.finish_lookup_bind_workspace") and
        workspaceBindI64(db, stmt, 2, generation, "user_generation.finish_lookup_bind_generation") and
        statusOk(stepStatement(stmt, &step), db, "user_generation.finish_lookup_step") and
        step == c.ZOVA_STEP_ROW and
        columnTextInto(db, stmt, 0, &state, "user_generation.finish_lookup_column") and
        c.strcmp(&state, "building") == 0;
}

pub export fn cbm_zova_user_database_generation_finish(zova_path: ?Str, workspace_id: ?Str, generation: i64, ready: bool) c_int {
    if (!with_zova) return -1;
    const path = nonEmpty(zova_path) orelse return -1;
    if (!workspaceNameComponentValid(workspace_id) or generation <= 0) return -1;
    const id = workspace_id.?;
    var zdb: ZovaDb = .{};
    defer closeZova(&zdb);
    if (!openOrCreateZova(path, &zdb) or !workspaceRegistryBegin(zdb.db)) return -1;
    const db = zdb.db;

    var ok = userGenerationIsBuilding(db, id, generation);

    const next_state: Str = if (ready) "ready" else "failed";
    if (ok) {
        const stmt = prepareZova(db, "UPDATE cbm_database_generation_v1 SET state=?1 " ++
            "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry " ++
            "WHERE workspace_id=?2) AND generation=?3", "user_generation.finish_update_prepare");
        ok = stmt != null and
            workspaceBindText(db, stmt, 1, next_state, "user_generation.finish_update_bind_state") and
            workspaceBindText(db, stmt, 2, id, "user_generation.finish_update_bind_workspace") and
            workspaceBindI64(db, stmt, 3, generation, "user_generation.finish_update_bind_generation") and
            workspaceStepDone(db, stmt, "user_generation.finish_update_step");
        finalize(stmt);
    }
    if (ok and ready) {
        const stmt = prepareZova(db, "UPDATE cbm_workspace_registry SET active_generation=?1 " ++
            "WHERE workspace_id=?2", "user_generation.finish_active_prepare");
        ok = stmt != null and
            workspaceBindI64(db, stmt, 1, generation, "user_generation.finish_active_bind_generation") and
            workspaceBindText(db, stmt, 2, id, "user_generation.finish_active_bind_workspace") and
            workspaceStepDone(db, stmt, "user_generation.finish_active_step");
        finalize(stmt);
    }
    ok = ok and workspaceRegistryCommit(db);
    if (!ok) workspaceRegistryRollback(db);
    return if (ok) 0 else -1;
}

/// The registry key of `workspace_id`; NULL when it has none.
fn workspaceKeyForId(db: ?*Db, workspace_id: ?Str) ?i64 {
    const id = nonEmpty(workspace_id) orelse return null;
    if (db == null) return null;
    const stmt = prepareZova(db, "SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id=?1", "workspace.key_prepare") orelse return null;
    defer finalize(stmt);
    if (!workspaceBindText(db, stmt, 1, id, "workspace.key_bind")) return null;
    const key = stepRowI64Logged(db, stmt, "workspace.key_step", "workspace.key_column") orelse return null;
    return if (key > 0) key else null;
}

/// The positive key `sql` selects; NULL when it yields none.
fn nextPrivateKey(db: ?*Db, sql: Str, phase: Str) ?i64 {
    if (db == null) return null;
    const stmt = prepareZova(db, sql, phase) orelse return null;
    defer finalize(stmt);
    const key = stepRowI64Logged(db, stmt, phase, phase) orelse return null;
    return if (key > 0) key else null;
}

fn fileKeyGetOrCreate(db: ?*Db, workspace_key: i64, file_path: CStr) ?i64 {
    if (db == null or workspace_key <= 0 or file_path == null) return null;
    {
        const stmt = prepareZova(db, "INSERT INTO cbm_files_v1(workspace_key,file_path) VALUES(?1,?2) " ++
            "ON CONFLICT(workspace_key,file_path) DO NOTHING", "files.insert_prepare") orelse return null;
        defer finalize(stmt);
        if (!workspaceBindI64(db, stmt, 1, workspace_key, "files.insert_workspace") or
            !workspaceBindText(db, stmt, 2, file_path, "files.insert_path") or
            !workspaceStepDone(db, stmt, "files.insert_step")) return null;
    }
    const stmt = prepareZova(db, "SELECT file_key FROM cbm_files_v1 WHERE workspace_key=?1 AND file_path=?2", "files.lookup_prepare") orelse return null;
    defer finalize(stmt);
    if (!workspaceBindI64(db, stmt, 1, workspace_key, "files.lookup_workspace") or
        !workspaceBindText(db, stmt, 2, file_path, "files.lookup_path")) return null;
    const key = stepRowI64Logged(db, stmt, "files.lookup_step", "files.lookup_column") orelse return null;
    return if (key > 0) key else null;
}

/// Reads the legacy generation state of (workspace, generation) into
/// `out_state`.
fn workspaceQueryState(db: ?*Db, workspace_id: Str, generation: i64, out_state: []u8) bool {
    const stmt = prepareZova(db, "SELECT state FROM cbm_workspace_generations " ++
        "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry " ++
        "WHERE workspace_id=?1) AND generation=?2", "workspace.state_prepare") orelse return false;
    defer finalize(stmt);
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    return workspaceBindText(db, stmt, 1, workspace_id, "workspace.state_bind_id") and
        workspaceBindI64(db, stmt, 2, generation, "workspace.state_bind_generation") and
        statusOk(stepStatement(stmt, &step), db, "workspace.state_step") and
        step == c.ZOVA_STEP_ROW and
        columnTextInto(db, stmt, 0, out_state, "workspace.state_column");
}

// ── Node key index ──────────────────────────────────────────────

// Stable node id -> Zova node key for one delta publish. Stable ids are hashes,
// so the key can only be found from the graph; one paged scan here replaces a
// full scan per looked-up node. Built before the delta's graph writes: nodes the
// delta re-puts are resolved from the put results first, and nodes it leaves
// alone keep their keys.
const NodeKeyEntry = extern struct {
    node_id: [*c]u8,
    node_key: i64,
};

const NodeKeyIndex = struct {
    items: [*c]NodeKeyEntry = null,
    count: usize = 0,
};

fn nodeKeyEntryCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(NodeKeyEntry, left).node_id, cast(NodeKeyEntry, right).node_id);
}

fn nodeKeyIndexFree(index: *NodeKeyIndex) void {
    for (many(index.items, index.count)) |*entry| c.free(entry.node_id);
    c.free(index.items);
    index.* = .{};
}

fn nodeKeyIndexBuild(db: ?*Db, workspace_id: Str, out: *NodeKeyIndex) bool {
    out.* = .{};
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_graph_name(workspace_id, &graph_name, graph_name.len) != 0) return false;
    nodeKeyIndexScan(db, bufZ(&graph_name), out) catch {
        nodeKeyIndexFree(out);
        return false;
    };
    if (out.count != 0) c.qsort(out.items, out.count, @sizeOf(NodeKeyEntry), nodeKeyEntryCompare);
    return true;
}

fn nodeKeyIndexScan(db: ?*Db, graph_name: Str, out: *NodeKeyIndex) error{Failed}!void {
    var cap: usize = 0;
    var cursor: c.zova_graph_scan_cursor = .{};
    while (true) {
        var page: c.zova_graph_scan_results = .{};
        defer c.zova_graph_scan_results_free(&page);
        if (!statusOk(c.zova_graph_scan(&c.zova_graph_scan_request{
            .db = db,
            .graph_name = graph_name,
            .node_after = cursor,
            .node_limit = 1024,
            .edge_limit = 0,
            .out_results = &page,
        }), db, "nodes.scan")) return error.Failed;
        const nodes = many(page.nodes, page.nodes_len);
        for (nodes) |*node| {
            if (node.node_id == null) continue;
            if (out.count == cap) {
                const next_cap = if (cap != 0) cap * 2 else 1024;
                const grown = interop.reallocN(NodeKeyEntry, out.items, next_cap);
                if (grown == null) return error.Failed;
                out.items = grown;
                cap = next_cap;
            }
            const node_id = zvStrdup(node.node_id);
            if (node_id == null) return error.Failed;
            out.items[out.count] = .{ .node_id = node_id, .node_key = node.node_key };
            out.count += 1;
        }
        if (nodes.len != 0) {
            const last = &nodes[nodes.len - 1];
            cursor = .{ .created_order = last.created_order, .key = last.node_key };
        }
        if (page.has_more_nodes == 0) break;
    }
}

/// The Zova node key of `node_id`; NULL when the index does not hold it.
fn nodeKeyIndexFind(index: *const NodeKeyIndex, node_id: CStr) ?i64 {
    if (node_id == null or node_id[0] == 0 or index.count == 0) return null;
    const probe: NodeKeyEntry = .{ .node_id = @constCast(node_id), .node_key = 0 };
    const found = c.bsearch(&probe, index.items, index.count, @sizeOf(NodeKeyEntry), nodeKeyEntryCompare) orelse return null;
    const node_key = cast(NodeKeyEntry, found).node_key;
    return if (node_key > 0) node_key else null;
}

/// The Zova key of the (source, type, target) edge; NULL when it has none.
fn edgeKeyForIdentity(db: ?*Db, workspace_id: CStr, source_id: CStr, edge_type: CStr, target_id: CStr) ?i64 {
    if (db == null or workspace_id == null or source_id == null or edge_type == null or target_id == null) return null;
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_graph_name(workspace_id, &graph_name, graph_name.len) != 0) return null;
    var neighbors: c.zova_graph_keyed_neighbor_results = .{};
    defer c.zova_graph_keyed_neighbor_results_free(&neighbors);
    if (!statusOk(c.zova_graph_neighbors_keyed(&c.zova_graph_neighbors_keyed_request{
        .db = db,
        .graph_name = &graph_name,
        .node_id = source_id,
        .direction = c.ZOVA_GRAPH_NEIGHBOR_OUTGOING,
        .edge_type = edge_type,
        .limit = std.math.maxInt(i64),
        .out_results = &neighbors,
    }), db, "edges.keyed_neighbors")) return null;
    for (many(neighbors.items, neighbors.len)) |*neighbor| {
        if (neighbor.node_id != null and c.strcmp(neighbor.node_id, target_id) == 0) {
            return if (neighbor.edge_key > 0) neighbor.edge_key else null;
        }
    }
    return null;
}

// ── Workspace registry API ──────────────────────────────────────

const RootLookup = enum { failed, missing, found };

/// Reads the workspace id registered for `root` into `out`. The phases logged
/// on failure are `phase` plus "_prepare", "_bind_root", "_step", "_column".
fn workspaceLookupRoot(db: ?*Db, root: Str, out: []u8, comptime phase: []const u8) RootLookup {
    const stmt = prepareZova(db, "SELECT workspace_id FROM cbm_workspace_registry WHERE canonical_root = ?1", phase ++ "_prepare") orelse return .failed;
    defer finalize(stmt);
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!workspaceBindText(db, stmt, 1, root, phase ++ "_bind_root") or
        !statusOk(stepStatement(stmt, &step), db, phase ++ "_step")) return .failed;
    if (step == c.ZOVA_STEP_ROW) {
        return if (columnTextInto(db, stmt, 0, out, phase ++ "_column")) .found else .failed;
    }
    return if (step == c.ZOVA_STEP_DONE) .missing else .failed;
}

pub export fn cbm_zova_workspace_get_or_create_at(registry_path: ?Str, root_path: ?Str, out_workspace_id: ?[*]u8, out_workspace_id_size: usize) c_int {
    if (!with_zova) return -1;
    var root_buf: [ZV_PATH_MAX]u8 = undefined;
    const path = nonEmpty(registry_path) orelse return -1;
    const out_ptr = out_workspace_id orelse return -1;
    if (out_workspace_id_size < ID_MAX) return -1;
    const root = workspaceNormalizeRoot(root_path, &root_buf) orelse return -1;
    const out = out_ptr[0..out_workspace_id_size];
    out[0] = 0;
    var zdb: ZovaDb = .{};
    defer closeZova(&zdb);
    if (!openOrCreateZova(path, &zdb) or !workspaceRegistryInit(zdb.db) or !workspaceRegistryBegin(zdb.db)) return -1;
    const db = zdb.db;

    var ok = workspaceLookupRoot(db, root.ptr, out, "workspace.lookup") != .failed;
    if (ok and out[0] == 0) {
        var workspace_id: [ID_MAX]u8 = undefined;
        ok = workspaceIdFromRoot(root, &workspace_id) == 0;
        if (ok) {
            const stmt = prepareZova(db, "INSERT INTO cbm_workspace_registry" ++
                "(workspace_id,canonical_root,id_format_version,active_generation) " ++
                "VALUES(?1,?2,1,0)", "workspace.insert_prepare");
            ok = stmt != null and
                workspaceBindText(db, stmt, 1, &workspace_id, "workspace.insert_bind_id") and
                workspaceBindText(db, stmt, 2, root.ptr, "workspace.insert_bind_root") and
                workspaceStepDone(db, stmt, "workspace.insert_step");
            finalize(stmt);
        }
        if (ok) _ = interop.bprint(out, "{s}", .{span(bufZ(&workspace_id))});
    }
    ok = ok and workspaceRegistryCommit(db);
    if (!ok) {
        workspaceRegistryRollback(db);
        out[0] = 0;
    }
    return if (ok) 0 else -1;
}

/// The newest ready generation of the database; NULL when it has none.
fn sidecarGenerationReadDb(db: ?*Db) ?i64 {
    if (db == null) return null;
    const stmt = prepareZova(db, "SELECT MAX(generation) FROM cbm_database_generation_v1 " ++
        "WHERE state='ready'", "sidecar_generation.prepare") orelse return null;
    defer finalize(stmt);
    const generation = stepRowI64Logged(db, stmt, "sidecar_generation.step", "sidecar_generation.column") orelse return null;
    return if (generation > 0) generation else null;
}

/// The first int64 of the first row of `sql`, with `bind_value` bound to ?1
/// when it is not NULL.
fn zovaQueryI64(db: ?*Db, sql: Str, bind_value: CStr, phase: Str) ?i64 {
    if (db == null) return null;
    const stmt = prepareZova(db, sql, phase) orelse return null;
    defer finalize(stmt);
    if (bind_value != null and !workspaceBindText(db, stmt, 1, bind_value, phase)) return null;
    return stepRowI64Logged(db, stmt, phase, phase);
}

pub export fn cbm_zova_workspace_lookup_at(registry_path: ?Str, root_path: ?Str, out_workspace_id: ?[*]u8, out_workspace_id_size: usize) c_int {
    if (!with_zova) {
        if (outBuf(out_workspace_id, out_workspace_id_size)) |out| out[0] = 0;
        return -1;
    }
    var root_buf: [ZV_PATH_MAX]u8 = undefined;
    const path = nonEmpty(registry_path) orelse return -1;
    const out_ptr = out_workspace_id orelse return -1;
    if (out_workspace_id_size < ID_MAX) return -1;
    const root = workspaceNormalizeRoot(root_path, &root_buf) orelse return -1;
    if (!c.cbm_file_exists(path)) return -1;
    const out = out_ptr[0..out_workspace_id_size];
    out[0] = 0;
    var zdb: ZovaDb = .{};
    if (!openZova(path, true, &zdb)) return -1;
    defer closeZova(&zdb);
    return if (workspaceLookupRoot(zdb.db, root.ptr, out, "workspace.lookup_read") == .found) 0 else -1;
}

pub export fn cbm_zova_workspace_generation_begin_at(registry_path: ?Str, workspace_id: ?Str, generation: i64) c_int {
    if (!with_zova) return -1;
    const path = registry_path orelse return -1;
    const id = nonEmpty(workspace_id) orelse return -1;
    if (generation <= 0) return -1;
    var zdb: ZovaDb = .{};
    defer closeZova(&zdb);
    if (!openOrCreateZova(path, &zdb) or !workspaceRegistryInit(zdb.db) or
        !workspaceLegacyGenerationInit(zdb.db) or !workspaceRegistryBegin(zdb.db)) return -1;
    const db = zdb.db;
    const stmt = prepareZova(db, "INSERT INTO cbm_workspace_generations(workspace_key,generation,state) " ++
        "SELECT workspace_key,?2,'building' FROM cbm_workspace_registry " ++
        "WHERE workspace_id=?1", "workspace.begin_prepare");
    var ok = stmt != null and
        workspaceBindText(db, stmt, 1, id, "workspace.begin_bind_id") and
        workspaceBindI64(db, stmt, 2, generation, "workspace.begin_bind_generation") and
        workspaceStepDone(db, stmt, "workspace.begin_step");
    finalize(stmt);
    ok = ok and workspaceRegistryCommit(db);
    if (!ok) workspaceRegistryRollback(db);
    return if (ok) 0 else -1;
}

pub export fn cbm_zova_workspace_generation_finish_at(registry_path: ?Str, workspace_id: ?Str, generation: i64, ready: bool) c_int {
    if (!with_zova) return -1;
    const path = registry_path orelse return -1;
    const id = nonEmpty(workspace_id) orelse return -1;
    if (generation <= 0) return -1;
    var zdb: ZovaDb = .{};
    defer closeZova(&zdb);
    if (!openOrCreateZova(path, &zdb) or !workspaceRegistryInit(zdb.db) or
        !workspaceLegacyGenerationInit(zdb.db) or !workspaceRegistryBegin(zdb.db)) return -1;
    const db = zdb.db;
    var state: [16]u8 = @splat(0);
    var ok = workspaceQueryState(db, id, generation, &state) and c.strcmp(&state, "building") == 0;
    if (ok) {
        const stmt = prepareZova(db, "UPDATE cbm_workspace_generations SET state = ?1 " ++
            "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry " ++
            "WHERE workspace_id=?2) AND generation=?3", "workspace.finish_prepare");
        ok = stmt != null and
            workspaceBindText(db, stmt, 1, if (ready) "ready" else "failed", "workspace.finish_bind_state") and
            workspaceBindText(db, stmt, 2, id, "workspace.finish_bind_id") and
            workspaceBindI64(db, stmt, 3, generation, "workspace.finish_bind_generation") and
            workspaceStepDone(db, stmt, "workspace.finish_step");
        finalize(stmt);
    }
    if (ok and ready) {
        const stmt = prepareZova(db, "UPDATE cbm_workspace_registry SET active_generation = ?1 " ++
            "WHERE workspace_id = ?2", "workspace.active_prepare");
        ok = stmt != null and
            workspaceBindI64(db, stmt, 1, generation, "workspace.active_bind_generation") and
            workspaceBindText(db, stmt, 2, id, "workspace.active_bind_id") and
            workspaceStepDone(db, stmt, "workspace.active_step");
        finalize(stmt);
    }
    ok = ok and workspaceRegistryCommit(db);
    if (!ok) workspaceRegistryRollback(db);
    return if (ok) 0 else -1;
}

pub export fn cbm_zova_workspace_active_generation_at(registry_path: ?Str, workspace_id: ?Str, out_generation: ?*i64) c_int {
    if (!with_zova) return -1;
    const path = registry_path orelse return -1;
    const id = nonEmpty(workspace_id) orelse return -1;
    const out = out_generation orelse return -1;
    out.* = 0;
    var zdb: ZovaDb = .{};
    defer closeZova(&zdb);
    if (!openOrCreateZova(path, &zdb) or !workspaceRegistryInit(zdb.db)) return -1;
    const db = zdb.db;
    const stmt = prepareZova(db, "SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id = ?1", "workspace.active_get_prepare") orelse return -1;
    defer finalize(stmt);
    if (!workspaceBindText(db, stmt, 1, id, "workspace.active_get_bind_id")) return -1;
    out.* = stepRowI64Logged(db, stmt, "workspace.active_get_step", "workspace.active_get_column") orelse return -1;
    return 0;
}

// ── Vector collections and graphs ───────────────────────────────

fn collectionDeleteIfExists(db: ?*Db, name: Str) bool {
    var exists: u8 = 0;
    if (!statusOk(c.zova_vector_collection_exists(&c.zova_vector_collection_exists_request{
        .db = db,
        .name = name,
        .out_exists = &exists,
    }), db, "vector.exists")) return false;
    if (exists == 0) return true;
    return statusOk(c.zova_vector_collection_delete(&c.zova_vector_collection_delete_request{ .db = db, .name = name }), db, "vector.delete_collection");
}

fn collectionCreateI8(db: ?*Db, name: Str, vector_dim: c_int) bool {
    if (!collectionDeleteIfExists(db, name)) return false;
    return statusOk(c.zova_vector_collection_create(&c.zova_vector_collection_create_request{
        .db = db,
        .name = name,
        .options = .{
            .dimensions = @bitCast(vector_dim),
            .metric = c.ZOVA_VECTOR_METRIC_COSINE,
            .element_type = c.ZOVA_VECTOR_ELEMENT_TYPE_I8,
        },
    }), db, "vector.create_collection");
}

const WorkspaceGraphNodeMap = extern struct {
    sqlite_id: i64,
    node_key: i64,
    stable_id: [*c]u8,
};

/// The node map of one workspace generation, sorted by sqlite id.
const NodeMapList = struct {
    items: [*c]WorkspaceGraphNodeMap = null,
    count: usize = 0,

    fn slice(list: NodeMapList) []WorkspaceGraphNodeMap {
        return many(list.items, list.count);
    }
};

fn graphDeleteNamedIfExists(db: ?*Db, graph_name: Str) bool {
    var exists: u8 = 0;
    if (!statusOk(c.zova_graph_exists(&c.zova_graph_exists_request{
        .db = db,
        .name = graph_name,
        .out_exists = &exists,
    }), db, "workspace_graph.exists")) return false;
    if (exists == 0) return true;
    return statusOk(c.zova_graph_delete(&c.zova_graph_delete_request{ .db = db, .name = graph_name }), db, "workspace_graph.delete");
}

fn workspaceGraphNodeMapFree(items: [*c]WorkspaceGraphNodeMap, count: usize) void {
    for (many(items, count)) |*item| c.free(item.stable_id);
    c.free(items);
}

/// The stable id mapped to `sqlite_id`; NULL when the map does not hold it.
fn workspaceGraphNodeMapFind(items: []const WorkspaceGraphNodeMap, sqlite_id: i64) CStr {
    var low: usize = 0;
    var high: usize = items.len;
    while (low < high) {
        const mid = low + (high - low) / 2;
        if (items[mid].sqlite_id == sqlite_id) return items[mid].stable_id;
        if (items[mid].sqlite_id < sqlite_id) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return null;
}

fn workspaceDumpNodeIdV2(workspace_id: Str, node: *const c.CBMDumpNode, out: []u8) bool {
    if (node.label == null or node.name == null or node.qualified_name == null or node.file_path == null or
        !workspaceNameComponentValid(workspace_id) or out.len == 0) return false;
    var relative_path: [ZV_PATH_MAX]u8 = undefined;
    if (interop.bprint(&relative_path, "{s}", .{span(node.file_path)}) >= relative_path.len) return false;
    _ = c.cbm_normalize_path_sep(&relative_path);
    var discriminator: [512]u8 = undefined;
    if (node.qualified_name[0] != 0) {
        if (interop.bprint(&discriminator, "named:{s}", .{span(node.qualified_name)}) >= discriminator.len) return false;
    } else if (node.name[0] == 0 or node.start_line < 0 or node.end_line < node.start_line or
        interop.bprint(&discriminator, "local:{s}:span:{d}:{d}", .{ span(node.name), node.start_line, node.end_line }) >= discriminator.len)
    {
        return false;
    }
    return cbm_zova_workspace_node_id_v2(
        @ptrCast(workspace_id),
        @ptrCast(node.label),
        bufZ(&relative_path),
        @ptrCast(node.qualified_name),
        bufZ(&discriminator),
        out.ptr,
        out.len,
    ) == 0;
}

fn workspaceGraphNodeMapCompare(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const left = cast(WorkspaceGraphNodeMap, a);
    const right = cast(WorkspaceGraphNodeMap, b);
    if (left.sqlite_id < right.sqlite_id) return -1;
    if (left.sqlite_id > right.sqlite_id) return 1;
    return c.strcmp(left.stable_id, right.stable_id);
}

/// Builds the (sqlite id, stable id) map of `nodes`, sorted by sqlite id.
/// Fails when an id is not positive or two nodes share an id.
fn workspaceGenerationMapBuild(workspace_id: Str, nodes: [*c]const c.CBMDumpNode, node_count: c_int, out: *NodeMapList) bool {
    out.* = .{};
    if (node_count < 0 or (node_count > 0 and nodes == null)) return false;
    const count: usize = @intCast(node_count);
    if (count == 0) return true;
    const map = interop.callocN(WorkspaceGraphNodeMap, count);
    if (map == null) return false;
    for (nodes[0..count], map[0..count], 0..) |*node, *entry, i| {
        var stable_id: [ID_MAX + 64]u8 = undefined;
        if (node.id <= 0 or !workspaceDumpNodeIdV2(workspace_id, node, &stable_id)) {
            workspaceGraphNodeMapFree(map, i);
            return false;
        }
        entry.sqlite_id = node.id;
        entry.stable_id = zvStrdup(&stable_id);
        if (entry.stable_id == null) {
            workspaceGraphNodeMapFree(map, i);
            return false;
        }
    }
    if (count > 1) c.qsort(map, count, @sizeOf(WorkspaceGraphNodeMap), workspaceGraphNodeMapCompare);
    for (1..count) |i| {
        if (map[i - 1].sqlite_id == map[i].sqlite_id or c.strcmp(map[i - 1].stable_id, map[i].stable_id) == 0) {
            workspaceGraphNodeMapFree(map, count);
            return false;
        }
    }
    out.* = .{ .items = map, .count = count };
    return true;
}

// ── Import of one workspace ─────────────────────────────────────

fn userDatabaseResetStatement(stmt: ?*Stmt, phase: Str) bool {
    if (stmt == null or c.zova_statement_reset(stmt) != c.ZOVA_OK or c.zova_statement_clear_bindings(stmt) != c.ZOVA_OK) {
        interop.log(c.CBM_LOG_ERROR, "zova.user_import", .{ "phase", phase });
        return false;
    }
    return true;
}

fn userDatabaseResetReboundStatement(stmt: ?*Stmt, phase: Str) bool {
    if (stmt == null or c.zova_statement_reset(stmt) != c.ZOVA_OK) {
        interop.log(c.CBM_LOG_ERROR, "zova.user_import", .{ "phase", phase });
        return false;
    }
    return true;
}

fn userDatabaseClearWorkspace(db: ?*Db, workspace_id: Str) bool {
    if (user_publish_delta_active) {
        user_publish_test_metrics.delta_clear_violation_count += 1;
        return false;
    }
    const workspace_key = workspaceKeyForId(db, workspace_id) orelse return false;
    const statements = [_]Str{
        "DELETE FROM cbm_nodes_v1 WHERE workspace_key=?1",
        "DELETE FROM cbm_file_hashes_v1 WHERE file_key IN " ++
            "(SELECT file_key FROM cbm_files_v1 WHERE workspace_key=?1)",
        "DELETE FROM cbm_files_v1 WHERE workspace_key=?1",
        "DELETE FROM cbm_token_vector_metadata_v1 WHERE workspace_key=?1",
        "DELETE FROM cbm_project_summaries_v2 WHERE workspace_key=?1",
        "DELETE FROM cbm_projects_v1 WHERE workspace_key=?1",
    };
    for (statements) |sql| {
        const stmt = prepareZova(db, sql, "user_import.clear_prepare") orelse return false;
        defer finalize(stmt);
        if (!workspaceBindI64(db, stmt, 1, workspace_key, "user_import.clear_bind") or
            !workspaceStepDone(db, stmt, "user_import.clear_step")) return false;
    }
    return true;
}

/// Puts every node into the workspace graph and writes its cbm_nodes_v1 row,
/// recording the Zova node key of each in `map`.
fn userDatabaseWriteNodesWithMap(db: ?*Db, workspace_id: Str, node_items: [*c]const c.CBMDumpNode, node_count: c_int, map_list: NodeMapList) bool {
    if (node_count < 0) return false;
    const count: usize = @intCast(node_count);
    if ((count > 0 and (node_items == null or map_list.items == null)) or map_list.count != count) return false;
    const nodes = many(node_items, count);
    const map = map_list.slice();
    const workspace_key = workspaceKeyForId(db, workspace_id) orelse return false;
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    const native_keys = if (count != 0) interop.callocN(i64, count) else null;
    defer c.free(native_keys);
    {
        const native = if (count != 0) interop.callocN(c.zova_graph_node_input, count) else null;
        defer c.free(native);
        if (cbm_zova_workspace_graph_name(@ptrCast(workspace_id), &graph_name, graph_name.len) != 0 or
            (count != 0 and (native == null or native_keys == null))) return false;
        for (nodes, map, 0..) |*node, *entry, i| {
            native[i] = .{
                .graph_name = &graph_name,
                .node_id = entry.stable_id,
                .kind = node.label,
                .target_type = c.ZOVA_GRAPH_TARGET_NONE,
                .target_namespace = null,
                .target_ref = null,
            };
        }
        if (count != 0 and !statusOk(c.zova_graph_node_put_many_keyed(&c.zova_graph_node_put_many_keyed_request{
            .db = db,
            .nodes = native,
            .nodes_len = count,
            .out_node_keys = native_keys,
            .out_node_keys_capacity = count,
        }), db, "user_import.native_nodes")) return false;
    }
    const node_stmt = prepareZova(db, "INSERT INTO cbm_nodes_v1(zova_node_key,workspace_key,label,name,qualified_name," ++
        "file_key,start_line,end_line,properties,source_ordinal) " ++
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) " ++
        "ON CONFLICT(zova_node_key) DO UPDATE SET " ++
        "label=excluded.label,name=excluded.name,qualified_name=excluded.qualified_name," ++
        "file_key=excluded.file_key,start_line=excluded.start_line,end_line=excluded.end_line," ++
        "properties=excluded.properties,source_ordinal=excluded.source_ordinal", "user_import.nodes_prepare") orelse return false;
    defer finalize(node_stmt);
    for (nodes, map, 0..) |*node, *entry, i| {
        if (node.id <= 0 or node.project == null or node.label == null or node.name == null or
            node.qualified_name == null or node.file_path == null or
            (i > 0 and nodes[i - 1].id >= node.id)) return false;
        if (entry.sqlite_id != node.id or entry.stable_id == null or
            !workspaceNameComponentValid(workspace_id)) return false;

        entry.node_key = native_keys[i];
        const file_key = fileKeyGetOrCreate(db, workspace_key, node.file_path) orelse return false;
        if (!workspaceBindI64(db, node_stmt, 1, entry.node_key, "user_import.node_key") or
            !workspaceBindI64(db, node_stmt, 2, workspace_key, "user_import.node_workspace") or
            !workspaceBindText(db, node_stmt, 3, node.label, "user_import.node_label") or
            !workspaceBindText(db, node_stmt, 4, node.name, "user_import.node_name") or
            !workspaceBindText(db, node_stmt, 5, node.qualified_name, "user_import.node_qn") or
            !workspaceBindI64(db, node_stmt, 6, file_key, "user_import.node_file") or
            !workspaceBindI64(db, node_stmt, 7, node.start_line, "user_import.node_start") or
            !workspaceBindI64(db, node_stmt, 8, node.end_line, "user_import.node_end") or
            !workspaceBindText(db, node_stmt, 9, if (node.properties != null) node.properties else "{}", "user_import.node_properties") or
            !workspaceBindI64(db, node_stmt, 10, @intCast(i), "user_import.node_source_ordinal") or
            !workspaceStepDone(db, node_stmt, "user_import.node_step") or
            !userDatabaseResetStatement(node_stmt, "user_import.node_reset")) return false;
    }
    return true;
}

fn userDatabaseWriteNodes(db: ?*Db, workspace_id: Str, nodes: [*c]const c.CBMDumpNode, node_count: c_int, out_map: *NodeMapList) bool {
    out_map.* = .{};
    var map: NodeMapList = .{};
    if (!workspaceGenerationMapBuild(workspace_id, nodes, node_count, &map)) return false;
    if (!userDatabaseWriteNodesWithMap(db, workspace_id, nodes, node_count, map)) {
        workspaceGraphNodeMapFree(map.items, map.count);
        return false;
    }
    out_map.* = map;
    return true;
}

/// Puts every edge into the workspace graph, then replaces the payload of each
/// distinct native edge with the encoded group of dump edges that share it.
fn userDatabaseWriteEdges(db: ?*Db, workspace_id: Str, edge_items: [*c]const c.CBMDumpEdge, edge_count: c_int, map: NodeMapList) bool {
    if (edge_count < 0 or (edge_count > 0 and edge_items == null)) return false;
    const count: usize = @intCast(edge_count);
    const edges = many(edge_items, count);
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_graph_name(workspace_id, &graph_name, graph_name.len) != 0) return false;
    if (count == 0) return true;

    const native_keys = interop.callocN(i64, count);
    defer c.free(native_keys);
    {
        const native = interop.callocN(c.zova_graph_edge_input, count);
        defer c.free(native);
        if (native == null or native_keys == null) return false;
        for (edges, 0..) |*edge, i| {
            native[i] = .{
                .graph_name = &graph_name,
                .from_node_id = workspaceGraphNodeMapFind(map.slice(), edge.source_id),
                .edge_type = edge.type,
                .to_node_id = workspaceGraphNodeMapFind(map.slice(), edge.target_id),
            };
        }
        if (!statusOk(c.zova_graph_edge_put_many_keyed(&c.zova_graph_edge_put_many_keyed_request{
            .db = db,
            .edges = native,
            .edges_len = count,
            .out_edge_keys = native_keys,
            .out_edge_keys_capacity = count,
        }), db, "user_import.native_edges")) return false;
    }

    const handled = interop.callocN(bool, count);
    defer c.free(handled);
    const group = interop.callocN([*c]const c.CBMDumpEdge, count);
    defer c.free(@ptrCast(group));
    const replacements = interop.callocN(c.zova_graph_edge_payload_replacement, count);
    defer c.free(replacements);
    const owned_payloads = interop.callocN([*c]u8, count);
    var replacement_count: usize = 0;
    defer {
        if (owned_payloads != null) {
            for (owned_payloads[0..replacement_count]) |payload| c.free(payload);
        }
        c.free(@ptrCast(owned_payloads));
    }
    if (handled == null or group == null or replacements == null or owned_payloads == null) return false;

    for (0..count) |i| {
        if (handled[i]) continue;
        if (native_keys[i] <= 0) return false;
        var group_count: usize = 0;
        for (i..count) |j| {
            if (!handled[j] and native_keys[j] == native_keys[i]) {
                handled[j] = true;
                group[group_count] = &edges[j];
                group_count += 1;
            }
        }
        var payload_len: usize = 0;
        if (c.cbm_zova_edge_payload_encoded_size(group, group_count, &payload_len) != 0) return false;
        const payload: [*c]u8 = if (payload_len > 0) interop.mallocN(u8, payload_len) else null;
        if (payload_len > 0 and payload == null) return false;
        var encoded: usize = 0;
        if (c.cbm_zova_edge_payload_encode(group, group_count, payload, payload_len, &encoded) != 0 or
            encoded != payload_len)
        {
            c.free(payload);
            return false;
        }
        owned_payloads[replacement_count] = payload;
        replacements[replacement_count] = .{
            .edge_key = native_keys[i],
            .payload = payload,
            .payload_len = payload_len,
        };
        replacement_count += 1;
    }
    if (replacement_count == 0) return true;
    return statusOk(c.zova_graph_edge_payload_replace_many(&c.zova_graph_edge_payload_replace_many_request{
        .db = db,
        .graph_name = &graph_name,
        .replacements = replacements,
        .replacement_count = replacement_count,
    }), db, "user_import.edge_payloads");
}

fn userDatabaseWriteSummary(db: ?*Db, workspace_id: Str, maybe_summary: ?*const c.cbm_zova_project_summary_input_t) bool {
    const summary = maybe_summary orelse return true;
    if (!summary.present) {
        const stmt = prepareZova(db, "DELETE FROM cbm_project_summaries_v2 WHERE workspace_key=(SELECT " ++
            "workspace_key FROM cbm_workspace_registry WHERE workspace_id=?1)", "user_import.summary_delete_prepare") orelse return false;
        defer finalize(stmt);
        return workspaceBindText(db, stmt, 1, workspace_id, "user_import.summary_delete_bind") and
            workspaceStepDone(db, stmt, "user_import.summary_delete_step");
    }
    if (summary.summary == null or summary.source_hash == null or summary.created_at == null or summary.updated_at == null) return false;
    const stmt = prepareZova(db, "INSERT INTO cbm_project_summaries_v2(workspace_key,summary,source_hash,created_at,updated_at) " ++
        "SELECT workspace_key,?2,?3,?4,?5 FROM cbm_workspace_registry WHERE workspace_id=?1 " ++
        "ON CONFLICT(workspace_key) DO UPDATE SET summary=excluded.summary," ++
        "source_hash=excluded.source_hash,created_at=excluded.created_at,updated_at=excluded.updated_at", "user_import.summary_prepare") orelse return false;
    defer finalize(stmt);
    return workspaceBindText(db, stmt, 1, workspace_id, "user_import.summary_workspace") and
        workspaceBindText(db, stmt, 2, summary.summary, "user_import.summary_value") and
        workspaceBindText(db, stmt, 3, summary.source_hash, "user_import.summary_hash") and
        workspaceBindText(db, stmt, 4, summary.created_at, "user_import.summary_created") and
        workspaceBindText(db, stmt, 5, summary.updated_at, "user_import.summary_updated") and
        workspaceStepDone(db, stmt, "user_import.summary_step");
}

fn userImportProject(db: ?*Db, workspace_id: Str, project: Str, root_path: Str) bool {
    const stmt = prepareZova(db, "INSERT INTO cbm_projects_v1(workspace_key,project,root_path) " ++
        "SELECT workspace_key,?2,?3 FROM cbm_workspace_registry WHERE workspace_id=?1 " ++
        "ON CONFLICT(workspace_key) DO UPDATE SET " ++
        "project=excluded.project,root_path=excluded.root_path,indexed_at=CURRENT_TIMESTAMP", "user_import.project_prepare") orelse return false;
    defer finalize(stmt);
    return workspaceBindText(db, stmt, 1, workspace_id, "user_import.project_workspace") and
        workspaceBindText(db, stmt, 2, project, "user_import.project_name") and
        workspaceBindText(db, stmt, 3, root_path, "user_import.project_root") and
        workspaceStepDone(db, stmt, "user_import.project_step");
}

/// Replaces the stored graph of the workspace with `nodes` and `edges` in one
/// transaction. The caller owns the generation row.
fn userImportWorkspaceWrite(
    zova_path: Str,
    workspace_id: Str,
    root_path: Str,
    project: Str,
    nodes: [*c]const c.CBMDumpNode,
    node_count: c_int,
    edges: [*c]const c.CBMDumpEdge,
    edge_count: c_int,
) bool {
    var zdb: ZovaDb = .{};
    defer closeZova(&zdb);
    if (!openOrCreateZova(zova_path, &zdb) or !workspaceRegistryBegin(zdb.db)) return false;
    const db = zdb.db;
    var map: NodeMapList = .{};
    defer workspaceGraphNodeMapFree(map.items, map.count);
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    const ok = userDatabaseClearWorkspace(db, workspace_id) and
        cbm_zova_workspace_graph_name(workspace_id, &graph_name, graph_name.len) == 0 and
        graphDeleteNamedIfExists(db, bufZ(&graph_name)) and
        statusOk(c.zova_graph_create(&c.zova_graph_create_request{ .db = db, .name = &graph_name }), db, "user_import.graph_create") and
        userImportProject(db, workspace_id, project, root_path) and
        userDatabaseWriteNodes(db, workspace_id, nodes, node_count, &map) and
        !testFailPhase("user_after_nodes") and
        userDatabaseWriteEdges(db, workspace_id, edges, edge_count, map) and
        !testFailPhase("user_after_edges") and
        !testFailPhase("user_before_commit") and
        workspaceRegistryCommit(db);
    if (!ok) workspaceRegistryRollback(db);
    return ok;
}

pub export fn cbm_zova_user_database_import_workspace(
    zova_path: ?Str,
    root_path: ?Str,
    project: ?Str,
    generation: i64,
    nodes: [*c]const c.CBMDumpNode,
    node_count: c_int,
    edges: [*c]const c.CBMDumpEdge,
    edge_count: c_int,
) c_int {
    if (!with_zova) return -1;
    const path = nonEmpty(zova_path) orelse return -1;
    const root = nonEmpty(root_path) orelse return -1;
    const name = nonEmpty(project) orelse return -1;
    if (generation <= 0 or node_count < 0 or edge_count < 0 or
        (node_count > 0 and nodes == null) or (edge_count > 0 and edges == null)) return -1;
    var workspace_id: [ID_MAX]u8 = @splat(0);
    if (cbm_zova_user_database_generation_begin(path, root, generation, &workspace_id, workspace_id.len) != 0) return -1;
    const id = bufZ(&workspace_id);

    if (!userImportWorkspaceWrite(path, id, root, name, nodes, node_count, edges, edge_count) or
        cbm_zova_user_database_generation_finish(path, id, generation, true) != 0)
    {
        _ = cbm_zova_user_database_generation_finish(path, id, generation, false);
        return -1;
    }
    return 0;
}

pub export fn cbm_zova_workspace_generation_digest_input(workspace_id: ?Str, input: ?*const GenerationInput, out_result: ?*GenerationResult) c_int {
    if (!with_zova) {
        if (out_result) |out| out.* = std.mem.zeroes(GenerationResult);
        return -1;
    }
    const id = workspace_id orelse return -1;
    const in = input orelse return -1;
    const out = out_result orelse return -1;
    var model: ?*PublishModel = null;
    if (c.cbm_zova_publish_model_build(id, in, &model) != 0) return -1;
    defer c.cbm_zova_publish_model_free(model);
    const digests = c.cbm_zova_publish_model_digests(model);
    if (digests == null) return -1;
    out.* = digests.*;
    return 0;
}

// ── Publication: registry and bookkeeping rows ──────────────────

/// Derives the workspace id of `root_path` into `out_id` and registers the
/// workspace when the registry does not hold it yet.
fn userWorkspaceIdTx(db: ?*Db, root_path: ?Str, out_id: []u8) bool {
    var root_buf: [ZV_PATH_MAX]u8 = undefined;
    const root = workspaceNormalizeRoot(root_path, &root_buf) orelse return false;
    if (workspaceIdFromRoot(root, out_id) != 0) return false;
    if (!workspaceRegistryInit(db)) return false;
    const stmt = prepareZova(db, "INSERT INTO cbm_workspace_registry(workspace_id,canonical_root,id_format_version,active_generation) " ++
        "VALUES(?1,?2,1,0) ON CONFLICT(canonical_root) DO NOTHING", "user_publish.workspace_prepare") orelse return false;
    defer finalize(stmt);
    return workspaceBindText(db, stmt, 1, out_id.ptr, "user_publish.workspace_id") and
        workspaceBindText(db, stmt, 2, root.ptr, "user_publish.workspace_root") and
        workspaceStepDone(db, stmt, "user_publish.workspace_step");
}

fn userPublishGenerationStateTx(db: ?*Db, workspace_id: Str, generation: i64, ready: bool) bool {
    {
        const stmt = prepareZova(db, "UPDATE cbm_database_generation_v1 SET state=?1 WHERE workspace_key=" ++
            "(SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id=?2) " ++
            "AND generation=?3", "user_publish.generation_update_prepare") orelse return false;
        defer finalize(stmt);
        if (!workspaceBindText(db, stmt, 1, if (ready) "ready" else "failed", "user_publish.generation_state") or
            !workspaceBindText(db, stmt, 2, workspace_id, "user_publish.generation_workspace") or
            !workspaceBindI64(db, stmt, 3, generation, "user_publish.generation_id") or
            !workspaceStepDone(db, stmt, "user_publish.generation_step")) return false;
    }
    if (!ready) return true;
    const stmt = prepareZova(db, "UPDATE cbm_workspace_registry SET active_generation=?1 WHERE workspace_id=?2", "user_publish.active_prepare") orelse return false;
    defer finalize(stmt);
    return workspaceBindI64(db, stmt, 1, generation, "user_publish.active_generation") and
        workspaceBindText(db, stmt, 2, workspace_id, "user_publish.active_workspace") and
        workspaceStepDone(db, stmt, "user_publish.active_step");
}

fn userPublishClearHealthTx(db: ?*Db, workspace_id: Str) bool {
    const statement = prepareZova(db, "DELETE FROM cbm_workspace_health_v1 WHERE workspace_key=(SELECT " ++
        "workspace_key FROM cbm_workspace_registry WHERE workspace_id=?1)", "user_publish.health_prepare") orelse return false;
    defer finalize(statement);
    return workspaceBindText(db, statement, 1, workspace_id, "user_publish.health_workspace") and
        workspaceStepDone(db, statement, "user_publish.health_step");
}

fn userPublishInsertGenerationTx(db: ?*Db, workspace_id: Str, generation: i64) bool {
    const stmt = prepareZova(db, "INSERT INTO cbm_database_generation_v1(workspace_key,generation,state) " ++
        "SELECT workspace_key,?2,'building' FROM cbm_workspace_registry WHERE workspace_id=?1", "user_publish.generation_insert_prepare") orelse return false;
    defer finalize(stmt);
    return workspaceBindText(db, stmt, 1, workspace_id, "user_publish.generation_insert_workspace") and
        workspaceBindI64(db, stmt, 2, generation, "user_publish.generation_insert_id") and
        workspaceStepDone(db, stmt, "user_publish.generation_insert_step");
}

fn userPublishProjectTx(db: ?*Db, workspace_id: Str, root_path: CStr, project: CStr, indexed_at: CStr) bool {
    const stmt = prepareZova(db, "INSERT INTO cbm_projects_v1(workspace_key,project,root_path,indexed_at) " ++
        "SELECT workspace_key,?2,?3,?4 FROM cbm_workspace_registry WHERE workspace_id=?1 " ++
        "ON CONFLICT(workspace_key) DO UPDATE SET " ++
        "project=excluded.project,root_path=excluded.root_path,indexed_at=excluded.indexed_at", "user_publish.project_prepare") orelse return false;
    defer finalize(stmt);
    return workspaceBindText(db, stmt, 1, workspace_id, "user_publish.project_workspace") and
        workspaceBindText(db, stmt, 2, project, "user_publish.project_name") and
        workspaceBindText(db, stmt, 3, root_path, "user_publish.project_root") and
        workspaceBindText(db, stmt, 4, indexed_at, "user_publish.project_indexed_at") and
        workspaceStepDone(db, stmt, "user_publish.project_step");
}

fn userPublishWriteIndexStateTx(db: ?*Db, workspace_id: Str, generation: i64, model: CStr, dimensions: c_int, indexed_at: CStr) bool {
    const stmt = prepareZova(db, "INSERT INTO cbm_workspace_index_state_v1" ++
        "(workspace_key,generation,model_fingerprint,vector_dimensions,indexed_at) " ++
        "SELECT workspace_key,?2,?3,?4,?5 FROM cbm_workspace_registry WHERE workspace_id=?1 " ++
        "ON CONFLICT(workspace_key) DO UPDATE SET " ++
        "generation=excluded.generation,model_fingerprint=excluded.model_fingerprint," ++
        "vector_dimensions=excluded.vector_dimensions,indexed_at=excluded.indexed_at", "user_publish.index_state_prepare") orelse return false;
    defer finalize(stmt);
    return workspaceBindText(db, stmt, 1, workspace_id, "user_publish.index_state_workspace") and
        workspaceBindI64(db, stmt, 2, generation, "user_publish.index_state_generation") and
        workspaceBindText(db, stmt, 3, model, "user_publish.index_state_model") and
        workspaceBindI64(db, stmt, 4, dimensions, "user_publish.index_state_dimensions") and
        workspaceBindText(db, stmt, 5, indexed_at, "user_publish.index_state_indexed_at") and
        workspaceStepDone(db, stmt, "user_publish.index_state_step");
}

/// The number of vectors the collection holds; NULL when it cannot be read.
fn vectorCollectionCount(db: ?*Db, collection: Str, phase: Str) ?u64 {
    var info: c.zova_vector_collection_info = .{};
    defer c.zova_vector_collection_info_free(&info);
    if (!statusOk(c.zova_vector_collection_info_get(&c.zova_vector_collection_info_get_request{
        .db = db,
        .name = collection,
        .out_info = &info,
    }), db, phase)) return null;
    return info.vector_count;
}

fn userPublishNativeReadbackCountsTx(db: ?*Db, result: *const GenerationResult, node_collection: Str, token_collection: Str) bool {
    if (db == null) return false;
    const node_vectors = vectorCollectionCount(db, node_collection, "user_publish.readback.node_collection") orelse return false;
    if (node_vectors != result.node_vectors) return false;
    const token_vectors = vectorCollectionCount(db, token_collection, "user_publish.readback.token_collection") orelse return false;
    return token_vectors == result.token_vectors;
}

fn userPublishIntegrityTx(db: ?*Db, workspace_id: Str, generation: i64, result: *const GenerationResult) bool {
    const stmt = prepareZova(db, "INSERT INTO cbm_generation_integrity_v2" ++
        "(workspace_key,generation,graph_nodes,graph_edges,metadata_nodes,metadata_edges," ++
        "metadata_topology_edges,fts_rows,node_vector_rows,token_vector_rows,node_vectors," ++
        "token_vectors,metadata_sha256,fts_sha256,topology_sha256,node_vector_sha256," ++
        "token_vector_sha256) SELECT workspace_key,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12," ++
        "?13,?14,?15,?16,?17 FROM cbm_workspace_registry WHERE workspace_id=?1", "user_publish.integrity_prepare") orelse return false;
    defer finalize(stmt);
    return workspaceBindText(db, stmt, 1, workspace_id, "user_publish.integrity_workspace") and
        workspaceBindI64(db, stmt, 2, generation, "user_publish.integrity_generation") and
        workspaceBindI64(db, stmt, 3, @bitCast(result.graph_nodes), "user_publish.integrity_graph_nodes") and
        workspaceBindI64(db, stmt, 4, @bitCast(result.graph_edges), "user_publish.integrity_graph_edges") and
        workspaceBindI64(db, stmt, 5, @bitCast(result.metadata_nodes), "user_publish.integrity_metadata_nodes") and
        workspaceBindI64(db, stmt, 6, @bitCast(result.metadata_edges), "user_publish.integrity_metadata_edges") and
        workspaceBindI64(db, stmt, 7, @bitCast(result.metadata_topology_edges), "user_publish.integrity_topology_edges") and
        workspaceBindI64(db, stmt, 8, @bitCast(result.fts_rows), "user_publish.integrity_fts_rows") and
        workspaceBindI64(db, stmt, 9, @bitCast(result.node_vector_rows), "user_publish.integrity_node_vector_rows") and
        workspaceBindI64(db, stmt, 10, @bitCast(result.token_vector_rows), "user_publish.integrity_token_vector_rows") and
        workspaceBindI64(db, stmt, 11, @bitCast(result.node_vectors), "user_publish.integrity_node_vectors") and
        workspaceBindI64(db, stmt, 12, @bitCast(result.token_vectors), "user_publish.integrity_token_vectors") and
        workspaceBindText(db, stmt, 13, &result.metadata_sha256, "user_publish.integrity_metadata_digest") and
        workspaceBindText(db, stmt, 14, &result.fts_sha256, "user_publish.integrity_fts_digest") and
        workspaceBindText(db, stmt, 15, &result.topology_sha256, "user_publish.integrity_topology_digest") and
        workspaceBindText(db, stmt, 16, &result.node_vector_sha256, "user_publish.integrity_node_vector_digest") and
        workspaceBindText(db, stmt, 17, &result.token_vector_sha256, "user_publish.integrity_token_vector_digest") and
        workspaceStepDone(db, stmt, "user_publish.integrity_step");
}

fn userPublishElapsedMs(start: *const c.struct_timespec, end: *const c.struct_timespec) f64 {
    return @as(f64, @floatFromInt(end.tv_sec - start.tv_sec)) * 1000.0 +
        @as(f64, @floatFromInt(end.tv_nsec - start.tv_nsec)) / 1000000.0;
}

fn monotonicNow() c.struct_timespec {
    var now = std.mem.zeroes(c.struct_timespec);
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &now);
    return now;
}

/// The milliseconds since `mark`, which moves to now. Logs the phase when
/// CBM_ZOVA_PUBLISH_PROFILE is "1".
fn userPublishProfilePhase(phase: Str, maybe_mark: ?*c.struct_timespec) f64 {
    const mark = maybe_mark orelse return 0.0;
    const now = monotonicNow();
    const elapsed_ms = userPublishElapsedMs(mark, &now);
    const enabled: CStr = c.getenv("CBM_ZOVA_PUBLISH_PROFILE");
    if (enabled != null and c.strcmp(enabled, "1") == 0) {
        var elapsed: [64]u8 = undefined;
        _ = interop.bprint(&elapsed, "{d:.3}", .{elapsed_ms});
        interop.log(c.CBM_LOG_INFO, "zova.single_file_publish.phase", .{ "phase", phase, "elapsed_ms", bufZ(&elapsed) });
    }
    mark.* = now;
    return elapsed_ms;
}

// ── Publication: private keys ───────────────────────────────────

const UserFileKey = extern struct {
    path: CStr = null,
    file_key: i64 = 0,
};

const UserNodeKey = extern struct {
    stable_id: CStr = null,
    node_key: i64 = 0,
};

const UserTokenKey = extern struct {
    token: CStr = null,
    token_key: i64 = 0,
};

const UserTopologyKey = extern struct {
    source_stable_id: CStr = null,
    edge_type: CStr = null,
    target_stable_id: CStr = null,
    edge_key: i64 = 0,
};

/// A calloc'ed array and its length. The owner frees `items`.
fn List(comptime T: type) type {
    return struct {
        items: [*c]T = null,
        count: usize = 0,

        fn slice(list: @This()) []T {
            return many(list.items, list.count);
        }
    };
}

/// calloc of `count` items; NULL for a count of 0.
fn callocList(comptime T: type, count: usize) [*c]T {
    return if (count != 0) interop.callocN(T, count) else null;
}

/// A count the C API returns as an int; a negative one counts as 0.
fn countOf(count: c_int) usize {
    return if (count > 0) @intCast(count) else 0;
}

/// (size_t)value, as C converts it.
fn sizeOfInt(value: c_int) usize {
    return @bitCast(@as(isize, value));
}

/// One int8 vector of `dimensions` values stored under `id`.
fn i8VectorInput(id: CStr, vector: [*c]const u8, dimensions: c_int) c.zova_vector_input {
    return .{
        .id = id,
        .values = .{
            .element_type = c.ZOVA_VECTOR_ELEMENT_TYPE_I8,
            .i8_values = @ptrCast(vector),
            .values_len = sizeOfInt(dimensions),
        },
    };
}

fn userNodeKeyCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(UserNodeKey, left).stable_id, cast(UserNodeKey, right).stable_id);
}

fn userTokenKeyCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(UserTokenKey, left).token, cast(UserTokenKey, right).token);
}

fn userStringPointerCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(CStr, left).*, cast(CStr, right).*);
}

/// The key of the entry named `name` in `items`, which are sorted by name;
/// 0 when no entry has that name.
fn namedKeyFind(comptime T: type, comptime name_field: []const u8, comptime key_field: []const u8, items: []const T, name: CStr) i64 {
    var low: usize = 0;
    var high: usize = items.len;
    while (low < high) {
        const mid = low + (high - low) / 2;
        const order = c.strcmp(@field(items[mid], name_field), name);
        if (order == 0) return @field(items[mid], key_field);
        if (order < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return 0;
}

fn userFileKeyFind(items: []const UserFileKey, path: CStr) i64 {
    return namedKeyFind(UserFileKey, "path", "file_key", items, path);
}

fn userNodeKeyFind(items: []const UserNodeKey, stable_id: CStr) i64 {
    return namedKeyFind(UserNodeKey, "stable_id", "node_key", items, stable_id);
}

fn userTokenKeyFind(items: []const UserTokenKey, token: CStr) i64 {
    return namedKeyFind(UserTokenKey, "token", "token_key", items, token);
}

fn userTopologyIdentityCompare(row: *const c.cbm_zova_publish_edge_t, source: CStr, edge_type: CStr, target: CStr) c_int {
    var order = c.strcmp(row.source_stable_id, source);
    if (order == 0) order = c.strcmp(row.source.*.type, edge_type);
    if (order == 0) order = c.strcmp(row.target_stable_id, target);
    return order;
}

fn userPublishModelTopologyFind(model: ?*const PublishModel, source: CStr, edge_type: CStr, target: CStr) ?*const c.cbm_zova_publish_edge_t {
    var low: c_int = 0;
    var high: c_int = c.cbm_zova_publish_model_topology_count(model);
    while (low < high) {
        const middle = low + @divTrunc(high - low, 2);
        const row: *const c.cbm_zova_publish_edge_t = c.cbm_zova_publish_model_topology_at(model, middle) orelse return null;
        const order = userTopologyIdentityCompare(row, source, edge_type, target);
        if (order < 0) {
            low = middle + 1;
        } else if (order > 0) {
            high = middle;
        } else {
            return row;
        }
    }
    return null;
}

/// Writes a private key as the decimal id Zova vectors are stored under.
fn userPrivateKeyId(key: i64, out: *[32]u8) bool {
    return key > 0 and interop.bprint(out, "{d}", .{key}) < out.len;
}

/// The `index`-th buffer of an array of private key ids.
fn idAt(ids: [*c][32]u8, index: usize) *[32]u8 {
    return ids + index;
}

/// Sorts `paths` and moves the distinct ones to the front; returns them.
fn sortUniquePaths(paths: []CStr) []CStr {
    if (paths.len > 1) c.qsort(@ptrCast(paths.ptr), paths.len, @sizeOf(CStr), userStringPointerCompare);
    var unique_count: usize = 0;
    for (paths, 0..) |path, i| {
        if (i == 0 or c.strcmp(paths[i - 1], path) != 0) {
            paths[unique_count] = path;
            unique_count += 1;
        }
    }
    return paths[0..unique_count];
}

/// The file key of every path the delta inserts, updates or re-hashes,
/// sorted by path. Creates the cbm_files_v1 row of a path that has none.
fn userDeltaFileKeysTx(db: ?*Db, workspace_key: i64, delta: ?*const WorkspaceDelta, metrics: *const DeltaMetrics, out: *List(UserFileKey)) bool {
    out.* = .{};
    const capacity64 = metrics.node_inserts + metrics.node_updates + metrics.file_hash_upserts;
    if (capacity64 > INT_MAX) return false;
    const capacity: usize = @intCast(capacity64);
    const paths = callocList(CStr, capacity);
    defer c.free(@ptrCast(paths));
    if (capacity != 0 and paths == null) return false;
    var path_count: usize = 0;
    for (0..@intCast(metrics.node_inserts)) |i| {
        const row = c.cbm_zova_workspace_delta_node_insert_at(delta, @intCast(i));
        if (row == null or row.*.source == null) return false;
        paths[path_count] = row.*.source.*.file_path;
        path_count += 1;
    }
    for (0..@intCast(metrics.node_updates)) |i| {
        const row = c.cbm_zova_workspace_delta_node_update_at(delta, @intCast(i));
        if (row == null or row.*.source == null) return false;
        paths[path_count] = row.*.source.*.file_path;
        path_count += 1;
    }
    for (0..@intCast(metrics.file_hash_upserts)) |i| {
        const row = c.cbm_zova_workspace_delta_file_hash_upsert_at(delta, @intCast(i));
        if (row == null) return false;
        paths[path_count] = row.*.file_path;
        path_count += 1;
    }
    const unique = sortUniquePaths(many(paths, path_count));
    const items = callocList(UserFileKey, unique.len);
    if (unique.len != 0 and items == null) return false;
    for (unique, 0..) |path, i| {
        items[i].path = path;
        items[i].file_key = fileKeyGetOrCreate(db, workspace_key, path) orelse {
            c.free(items);
            return false;
        };
        user_publish_test_metrics.delta_file_key_resolutions += 1;
    }
    out.* = .{ .items = items, .count = unique.len };
    return true;
}

/// Inserts a cbm_files_v1 row for every path of the model and returns their
/// file keys, sorted by path.
fn userPublishFileKeysTx(db: ?*Db, model: ?*const PublishModel, workspace_key: i64, out: *List(UserFileKey)) bool {
    out.* = .{};
    const input = c.cbm_zova_publish_model_input(model);
    const hash_count = c.cbm_zova_publish_model_file_hash_count(model);
    const capacity = countOf(input.*.node_count + hash_count);
    const paths = callocList(CStr, capacity);
    defer c.free(@ptrCast(paths));
    if (capacity != 0 and paths == null) return false;
    var path_count: usize = 0;
    for (many(input.*.nodes, input.*.node_count)) |*node| {
        paths[path_count] = node.file_path;
        path_count += 1;
    }
    for (0..countOf(hash_count)) |i| {
        paths[path_count] = c.cbm_zova_publish_model_file_hash_at(model, @intCast(i)).*.file_path;
        path_count += 1;
    }
    const unique = sortUniquePaths(many(paths, path_count));
    if (unique.len == 0) return true;
    const items = callocList(UserFileKey, unique.len);
    if (items == null) return false;
    var done = false;
    defer if (!done) c.free(items);
    const next_key = nextPrivateKey(db, "SELECT COALESCE(MAX(file_key),0)+1 FROM cbm_files_v1", "user_publish.next_file_key") orelse return false;
    const statement = prepareZova(db, "INSERT INTO cbm_files_v1(file_key,workspace_key,file_path) " ++
        "VALUES(?1,?2,?3)", "user_publish.files_prepare") orelse return false;
    defer finalize(statement);
    for (unique, 0..) |path, i| {
        items[i] = .{ .path = path, .file_key = next_key + @as(i64, @intCast(i)) };
        if (!workspaceBindI64(db, statement, 1, items[i].file_key, "user_publish.file_key") or
            !workspaceBindI64(db, statement, 2, workspace_key, "user_publish.file_workspace") or
            !workspaceBindText(db, statement, 3, items[i].path, "user_publish.file_path") or
            !workspaceStepDone(db, statement, "user_publish.file_step") or
            !userDatabaseResetReboundStatement(statement, "user_publish.file_reset")) return false;
        userPublishMetricRow(&user_publish_test_metrics.canonical_files_sql, 2, 1, 0, false);
    }
    done = true;
    out.* = .{ .items = items, .count = unique.len };
    return true;
}

// ── Publication: the rows of a whole model ──────────────────────

fn userPublishModelNodesTx(db: ?*Db, model: ?*const PublishModel, workspace_key: i64, file_keys: []const UserFileKey, node_keys: []const UserNodeKey) bool {
    user_publish_test_metrics.canonical_node_fts_passes += 1;
    const count = countOf(c.cbm_zova_publish_model_node_count(model));
    const node = prepareZova(db, "INSERT INTO cbm_nodes_v1(zova_node_key,workspace_key,label,name,qualified_name," ++
        "file_key,start_line,end_line,properties,source_ordinal) " ++
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)", "user_publish.model_nodes_prepare") orelse return false;
    defer finalize(node);
    for (0..count) |i| {
        const row = c.cbm_zova_publish_model_node_at(model, @intCast(i));
        if (row == null or row.*.source == null) return false;
        const source = row.*.source;
        const file_key = userFileKeyFind(file_keys, source.*.file_path);
        if (file_key <= 0 or row.*.ordinal != i) return false;
        if (i >= node_keys.len or c.strcmp(node_keys[i].stable_id, row.*.stable_id) != 0 or
            node_keys[i].node_key <= 0) return false;
        if (!workspaceBindI64(db, node, 1, node_keys[i].node_key, "user_publish.model_node_key") or
            !workspaceBindI64(db, node, 2, workspace_key, "user_publish.model_node_ws") or
            !workspaceBindText(db, node, 3, source.*.label, "user_publish.model_node_label") or
            !workspaceBindText(db, node, 4, source.*.name, "user_publish.model_node_name") or
            !workspaceBindText(db, node, 5, source.*.qualified_name, "user_publish.model_node_qn") or
            !workspaceBindI64(db, node, 6, file_key, "user_publish.model_node_file") or
            !workspaceBindI64(db, node, 7, source.*.start_line, "user_publish.model_node_start") or
            !workspaceBindI64(db, node, 8, source.*.end_line, "user_publish.model_node_end") or
            !workspaceBindText(db, node, 9, if (source.*.properties != null) source.*.properties else "{}", "user_publish.model_node_properties") or
            !workspaceBindI64(db, node, 10, @bitCast(row.*.source_ordinal), "user_publish.model_node_source_ordinal") or
            !workspaceStepDone(db, node, "user_publish.model_node_step") or
            !userDatabaseResetReboundStatement(node, "user_publish.model_node_reset")) return false;
        userPublishMetricRow(&user_publish_test_metrics.canonical_nodes_sql, 6, 4, 0, false);
    }
    return true;
}

fn userPublishModelHashesTx(db: ?*Db, model: ?*const PublishModel, file_keys: []const UserFileKey) bool {
    const statement = prepareZova(db, "INSERT INTO cbm_file_hashes_v1(file_key,content_hash,mtime_ns,size_bytes) " ++
        "VALUES(?1,?2,?3,?4)", "user_publish.model_hash_prepare") orelse return false;
    defer finalize(statement);
    const count = countOf(c.cbm_zova_publish_model_file_hash_count(model));
    for (0..count) |i| {
        const row = c.cbm_zova_publish_model_file_hash_at(model, @intCast(i));
        if (row == null) return false;
        const file_key = userFileKeyFind(file_keys, row.*.file_path);
        if (file_key <= 0) return false;
        if (!workspaceBindI64(db, statement, 1, file_key, "user_publish.model_hash_file") or
            !workspaceBindText(db, statement, 2, row.*.content_hash, "user_publish.model_hash_value") or
            !workspaceBindI64(db, statement, 3, row.*.mtime_ns, "user_publish.model_hash_mtime") or
            !workspaceBindI64(db, statement, 4, row.*.size_bytes, "user_publish.model_hash_size") or
            !workspaceStepDone(db, statement, "user_publish.model_hash_step") or
            !userDatabaseResetReboundStatement(statement, "user_publish.model_hash_reset")) return false;
        userPublishMetricRow(&user_publish_test_metrics.canonical_hashes_sql, 3, 1, 0, false);
    }
    return true;
}

/// Inserts the metadata row of every token vector and returns the token
/// keys, in model order.
fn userPublishModelTokenMetadataTx(db: ?*Db, model: ?*const PublishModel, workspace_key: i64, out: *List(UserTokenKey)) bool {
    out.* = .{};
    const count = countOf(c.cbm_zova_publish_model_token_vector_count(model));
    if (count == 0) return true;
    const token_keys = callocList(UserTokenKey, count);
    if (token_keys == null) return false;
    var done = false;
    defer if (!done) c.free(token_keys);
    const next_key = nextPrivateKey(db, "SELECT COALESCE(MAX(token_key),0)+1 FROM cbm_token_vector_metadata_v1", "user_publish.next_token_key") orelse return false;
    const statement = prepareZova(db, "INSERT INTO cbm_token_vector_metadata_v1(token_key,workspace_key,token,idf) " ++
        "VALUES(?1,?2,?3,?4)", "user_publish.model_token_prepare") orelse return false;
    defer finalize(statement);
    for (0..count) |i| {
        const row = c.cbm_zova_publish_model_token_vector_at(model, @intCast(i));
        if (row == null or row.*.source == null or row.*.ordinal != i) return false;
        token_keys[i] = .{ .token = row.*.source.*.token, .token_key = next_key + @as(i64, @intCast(i)) };
        if (!workspaceBindI64(db, statement, 1, token_keys[i].token_key, "user_publish.model_token_key") or
            !workspaceBindI64(db, statement, 2, workspace_key, "user_publish.model_token_ws") or
            !workspaceBindText(db, statement, 3, row.*.source.*.token, "user_publish.model_token_value") or
            !statusOk(c.zova_statement_bind_double(&c.zova_statement_bind_double_request{
                .statement = statement,
                .index = 4,
                .value = row.*.source.*.idf,
            }), db, "user_publish.model_token_idf") or
            !workspaceStepDone(db, statement, "user_publish.model_token_step") or
            !userDatabaseResetReboundStatement(statement, "user_publish.model_token_reset")) return false;
        userPublishMetricRow(&user_publish_test_metrics.canonical_token_metadata_sql, 2, 1, 1, false);
    }
    done = true;
    out.* = .{ .items = token_keys, .count = count };
    return true;
}

const UserNativeGraphProfile = struct {
    materialize_ms: f64 = 0,
    reset_ms: f64 = 0,
    nodes_ms: f64 = 0,
    edges_ms: f64 = 0,
    validate_ms: f64 = 0,
    key_generation_ms: f64 = 0,
    cleanup_ms: f64 = 0,
};

/// Rebuilds the native graph of the model's workspace and returns the Zova
/// key of every node and topology edge, in model order. An empty database
/// takes the one-call fresh build; otherwise nodes, edges and payloads go in
/// separately.
fn userPublishModelGraphTx(
    db: ?*Db,
    model: ?*const PublishModel,
    out_node_keys: *[*c]UserNodeKey,
    out_topology_keys: *[*c]UserTopologyKey,
    out_nodes: ?*u64,
    out_edges: ?*u64,
    profile: *UserNativeGraphProfile,
) bool {
    out_node_keys.* = null;
    out_topology_keys.* = null;
    profile.* = .{};
    var profile_mark = monotonicNow();
    const workspace_id = c.cbm_zova_publish_model_workspace_id(model);
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_graph_name(@ptrCast(workspace_id), &graph_name, graph_name.len) != 0) {
        profile.materialize_ms = userPublishProfilePhase("native_graph.materialize", &profile_mark);
        return false;
    }
    const node_count = countOf(c.cbm_zova_publish_model_node_count(model));
    const edge_count = countOf(c.cbm_zova_publish_model_topology_count(model));
    // Declared first so it runs after the arrays below are released.
    defer profile.cleanup_ms = userPublishProfilePhase("native_graph.cleanup", &profile_mark);
    const nodes = callocList(c.zova_graph_fresh_node_input, node_count);
    defer c.free(nodes);
    const edges = callocList(c.zova_graph_fresh_edge_payload_input, edge_count);
    defer c.free(edges);
    const node_key_values = callocList(i64, node_count);
    defer c.free(node_key_values);
    const edge_key_values = callocList(i64, edge_count);
    defer c.free(edge_key_values);
    const node_keys = callocList(UserNodeKey, node_count);
    const topology_keys = callocList(UserTopologyKey, edge_count);
    var ok = false;
    defer if (!ok) {
        c.free(node_keys);
        c.free(topology_keys);
    };
    if ((node_count != 0 and (nodes == null or node_key_values == null or node_keys == null)) or
        (edge_count != 0 and (edges == null or edge_key_values == null or topology_keys == null)))
    {
        profile.materialize_ms = userPublishProfilePhase("native_graph.materialize", &profile_mark);
        return false;
    }
    for (0..node_count) |i| {
        const row = c.cbm_zova_publish_model_node_at(model, @intCast(i));
        nodes[i] = .{
            .node_id = row.*.stable_id,
            .kind = row.*.source.*.label,
            .target_type = c.ZOVA_GRAPH_TARGET_NONE,
            .target_namespace = null,
            .target_ref = null,
        };
        node_keys[i].stable_id = row.*.stable_id;
    }
    for (0..edge_count) |i| {
        const row = c.cbm_zova_publish_model_topology_at(model, @intCast(i));
        edges[i] = .{
            .from_node_ordinal = @intCast(row.*.source_ordinal),
            .edge_type = row.*.source.*.type,
            .to_node_ordinal = @intCast(row.*.target_ordinal),
            .payload = row.*.payload,
            .payload_len = row.*.payload_len,
        };
        topology_keys[i] = .{
            .source_stable_id = row.*.source_stable_id,
            .edge_type = row.*.source.*.type,
            .target_stable_id = row.*.target_stable_id,
        };
    }
    profile.materialize_ms = userPublishProfilePhase("native_graph.materialize", &profile_mark);
    ok = graphDeleteNamedIfExists(db, bufZ(&graph_name));
    var use_fresh_build = false;
    if (ok) {
        var graph_list: c.zova_graph_list = .{};
        defer c.zova_graph_list_free(&graph_list);
        ok = statusOk(c.zova_graphs_list(&c.zova_graph_list_request{ .db = db, .out_list = &graph_list }), db, "user_publish.model_graph_list");
        if (ok) use_fresh_build = graph_list.len == 0;
    }
    if (ok and !use_fresh_build) {
        ok = statusOk(c.zova_graph_create(&c.zova_graph_create_request{ .db = db, .name = &graph_name }), db, "user_publish.model_graph_create");
    }
    profile.reset_ms = userPublishProfilePhase("native_graph.reset", &profile_mark);
    if (ok and use_fresh_build) {
        user_publish_test_metrics.native_graph_fresh_calls += 1;
        user_publish_test_metrics.native_graph_prepared_calls += 1;
        ok = statusOk(c.zova_graph_build_fresh_prepared_keyed_with_payloads(&c.zova_graph_build_fresh_prepared_keyed_with_payloads_request{
            .db = db,
            .graph_name = &graph_name,
            .nodes = nodes,
            .nodes_len = node_count,
            .edges = edges,
            .edges_len = edge_count,
            .out_node_keys = node_key_values,
            .out_node_keys_capacity = node_count,
            .out_edge_keys = edge_key_values,
            .out_edge_keys_capacity = edge_count,
        }), db, "user_publish.model_graph_fresh_prepared");
        if (ok and testFailPhase("user_after_graph_nodes")) ok = false;
        if (ok and testFailPhase("user_after_graph_edges")) ok = false;
        // Fresh construction is one atomic Zova call, so it has no truthful
        // node/edge timing boundary. Keep the node phase empty and report the
        // complete bulk-build call in the edge phase used by existing reports.
        profile.nodes_ms = 0.0;
        profile.edges_ms = userPublishProfilePhase("native_graph.edges", &profile_mark);
    } else if (ok) {
        const legacy_nodes = callocList(c.zova_graph_node_input, node_count);
        defer c.free(legacy_nodes);
        const legacy_edges = callocList(c.zova_graph_edge_input, edge_count);
        defer c.free(legacy_edges);
        if ((node_count != 0 and legacy_nodes == null) or (edge_count != 0 and legacy_edges == null)) ok = false;
        if (ok) {
            for (0..node_count) |i| {
                const row = c.cbm_zova_publish_model_node_at(model, @intCast(i));
                legacy_nodes[i] = .{
                    .graph_name = &graph_name,
                    .node_id = row.*.stable_id,
                    .kind = row.*.source.*.label,
                    .target_type = c.ZOVA_GRAPH_TARGET_NONE,
                    .target_namespace = null,
                    .target_ref = null,
                };
            }
            for (0..edge_count) |i| {
                const row = c.cbm_zova_publish_model_topology_at(model, @intCast(i));
                legacy_edges[i] = .{
                    .graph_name = &graph_name,
                    .from_node_id = row.*.source_stable_id,
                    .edge_type = row.*.source.*.type,
                    .to_node_id = row.*.target_stable_id,
                };
            }
        }
        if (ok and node_count != 0) {
            user_publish_test_metrics.native_graph_node_calls += 1;
            ok = statusOk(c.zova_graph_node_put_many_keyed(&c.zova_graph_node_put_many_keyed_request{
                .db = db,
                .nodes = legacy_nodes,
                .nodes_len = node_count,
                .out_node_keys = node_key_values,
                .out_node_keys_capacity = node_count,
            }), db, "user_publish.model_graph_nodes");
        }
        if (ok and testFailPhase("user_after_graph_nodes")) ok = false;
        profile.nodes_ms = userPublishProfilePhase("native_graph.nodes", &profile_mark);
        if (ok and edge_count != 0) {
            user_publish_test_metrics.native_graph_edge_calls += 1;
            ok = statusOk(c.zova_graph_edge_put_many_keyed(&c.zova_graph_edge_put_many_keyed_request{
                .db = db,
                .edges = legacy_edges,
                .edges_len = edge_count,
                .out_edge_keys = edge_key_values,
                .out_edge_keys_capacity = edge_count,
            }), db, "user_publish.model_graph_edges");
        }
        if (ok and edge_count != 0) {
            const replacements = interop.callocN(c.zova_graph_edge_payload_replacement, edge_count);
            defer c.free(replacements);
            ok = replacements != null;
            if (ok) {
                for (0..edge_count) |i| {
                    const row = c.cbm_zova_publish_model_topology_at(model, @intCast(i));
                    replacements[i] = .{
                        .edge_key = edge_key_values[i],
                        .payload = row.*.payload,
                        .payload_len = row.*.payload_len,
                    };
                }
                ok = statusOk(c.zova_graph_edge_payload_replace_many(&c.zova_graph_edge_payload_replace_many_request{
                    .db = db,
                    .graph_name = &graph_name,
                    .replacements = replacements,
                    .replacement_count = edge_count,
                }), db, "user_publish.model_graph_payloads");
            }
        }
        if (ok and testFailPhase("user_after_graph_edges")) ok = false;
        profile.edges_ms = userPublishProfilePhase("native_graph.edges", &profile_mark);
    }
    if (ok) {
        for (0..node_count) |i| {
            if (node_key_values[i] <= 0) {
                ok = false;
                break;
            }
            node_keys[i].node_key = node_key_values[i];
        }
    }
    if (ok) {
        for (0..edge_count) |i| {
            if (edge_key_values[i] <= 0) {
                ok = false;
                break;
            }
            topology_keys[i].edge_key = edge_key_values[i];
        }
    }
    if (ok) {
        var info: c.zova_graph_info = .{};
        defer c.zova_graph_info_free(&info);
        ok = statusOk(c.zova_graph_info_get(&c.zova_graph_info_get_request{
            .db = db,
            .name = &graph_name,
            .out_info = &info,
        }), db, "user_publish.model_graph_info");
        if (ok) {
            if (out_nodes) |out| out.* = info.node_count;
            if (out_edges) |out| out.* = info.edge_count;
        }
        profile.validate_ms = userPublishProfilePhase("native_graph.validate", &profile_mark);
    }
    if (ok) {
        out_node_keys.* = node_keys;
        out_topology_keys.* = topology_keys;
    }
    return ok;
}

fn userPublishModelVectorsTx(
    db: ?*Db,
    model: ?*const PublishModel,
    node_collection: Str,
    token_collection: Str,
    node_keys: []const UserNodeKey,
    token_keys: []const UserTokenKey,
) bool {
    const input = c.cbm_zova_publish_model_input(model);
    const node_count = countOf(c.cbm_zova_publish_model_node_vector_count(model));
    const token_count = countOf(c.cbm_zova_publish_model_token_vector_count(model));
    const nodes = callocList(c.zova_vector_input, node_count);
    defer c.free(nodes);
    const tokens = callocList(c.zova_vector_input, token_count);
    defer c.free(tokens);
    const node_ids = callocList([32]u8, node_count);
    defer c.free(node_ids);
    const token_ids = callocList([32]u8, token_count);
    defer c.free(token_ids);
    if ((node_count != 0 and (nodes == null or node_ids == null)) or
        (token_count != 0 and (tokens == null or token_ids == null))) return false;
    for (0..node_count) |i| {
        const row = c.cbm_zova_publish_model_node_vector_at(model, @intCast(i));
        if (row == null or row.*.ordinal != i or row.*.node_ordinal >= node_keys.len) return false;
        const node_key = &node_keys[@intCast(row.*.node_ordinal)];
        if (node_key.node_key <= 0 or c.strcmp(node_key.stable_id, row.*.stable_id) != 0 or
            !userPrivateKeyId(node_key.node_key, idAt(node_ids, i))) return false;
        nodes[i] = i8VectorInput(idAt(node_ids, i), row.*.source.*.vector, input.*.vector_dimensions);
    }
    for (0..token_count) |i| {
        const row = c.cbm_zova_publish_model_token_vector_at(model, @intCast(i));
        if (row == null or row.*.source == null or row.*.ordinal != i or i >= token_keys.len) return false;
        const key = token_keys[i].token_key;
        if (key <= 0 or c.strcmp(token_keys[i].token, row.*.source.*.token) != 0 or
            !userPrivateKeyId(key, idAt(token_ids, i))) return false;
        tokens[i] = i8VectorInput(idAt(token_ids, i), row.*.source.*.vector, input.*.vector_dimensions);
    }
    if (!collectionCreateI8(db, node_collection, input.*.vector_dimensions) or
        !collectionCreateI8(db, token_collection, input.*.vector_dimensions)) return false;
    if (node_count != 0) {
        user_publish_test_metrics.native_node_vector_calls += 1;
        if (!statusOk(c.zova_vector_put_many(&c.zova_vector_put_many_request{
            .db = db,
            .collection_name = node_collection,
            .vectors = nodes,
            .vectors_len = node_count,
        }), db, "user_publish.model_node_vectors")) return false;
    }
    if (testFailPhase("user_after_node_vectors")) return false;
    if (token_count != 0) {
        user_publish_test_metrics.native_token_vector_calls += 1;
        if (!statusOk(c.zova_vector_put_many(&c.zova_vector_put_many_request{
            .db = db,
            .collection_name = token_collection,
            .vectors = tokens,
            .vectors_len = token_count,
        }), db, "user_publish.model_token_vectors")) return false;
    }
    return !testFailPhase("user_after_token_vectors");
}

// ── Publication: delta ──────────────────────────────────────────

const DeltaIdAt = *const fn (?*const WorkspaceDelta, c_int) callconv(.c) [*c]const u8;

/// Runs `sql` (?1 workspace key, ?2 id) once per id `item_at` returns.
fn userDeltaDeleteIdsTx(db: ?*Db, workspace_key: i64, sql: Str, phase: Str, delta: ?*const WorkspaceDelta, item_at: DeltaIdAt, count: u64) bool {
    if (count == 0) return true;
    const statement = prepareZova(db, sql, phase) orelse return false;
    defer finalize(statement);
    for (0..@intCast(count)) |i| {
        const id = item_at(delta, @intCast(i));
        if (id == null) return false;
        if (!workspaceBindI64(db, statement, 1, workspace_key, phase) or
            !workspaceBindText(db, statement, 2, id, phase) or
            !workspaceStepDone(db, statement, phase) or
            !userDatabaseResetStatement(statement, phase)) return false;
    }
    return true;
}

/// Deletes the vectors, token metadata, graph edges and graph nodes the
/// delta removes.
fn userDeltaDeleteNativeTx(
    db: ?*Db,
    workspace_id: Str,
    node_index: *const NodeKeyIndex,
    node_collection: Str,
    token_collection: Str,
    delta: ?*const WorkspaceDelta,
    metrics: *const DeltaMetrics,
) bool {
    {
        const node_vector_deletes: usize = @intCast(metrics.node_vector_deletes);
        const token_vector_deletes: usize = @intCast(metrics.token_vector_deletes);
        const node_vectors = callocList(CStr, node_vector_deletes);
        defer c.free(@ptrCast(node_vectors));
        const token_vectors = callocList(CStr, token_vector_deletes);
        defer c.free(@ptrCast(token_vectors));
        const node_vector_ids = callocList([32]u8, node_vector_deletes);
        defer c.free(node_vector_ids);
        const token_vector_ids = callocList([32]u8, token_vector_deletes);
        defer c.free(token_vector_ids);
        if ((node_vector_deletes != 0 and (node_vectors == null or node_vector_ids == null)) or
            (token_vector_deletes != 0 and (token_vectors == null or token_vector_ids == null))) return false;
        const workspace_key = workspaceKeyForId(db, workspace_id) orelse return false;
        for (0..node_vector_deletes) |i| {
            const stable_id = c.cbm_zova_workspace_delta_node_vector_delete_at(delta, @intCast(i));
            if (stable_id == null) return false;
            const node_key = nodeKeyIndexFind(node_index, stable_id) orelse return false;
            if (!userPrivateKeyId(node_key, idAt(node_vector_ids, i))) return false;
            node_vectors[i] = idAt(node_vector_ids, i);
        }
        if (token_vector_deletes != 0) {
            const token_key_statement = prepareZova(db, "SELECT token_key FROM cbm_token_vector_metadata_v1 " ++
                "WHERE workspace_key=?1 AND token=?2", "user_delta.token_key_prepare") orelse return false;
            defer finalize(token_key_statement);
            for (0..token_vector_deletes) |i| {
                const token = c.cbm_zova_workspace_delta_token_vector_delete_at(delta, @intCast(i));
                if (c.zova_statement_reset(token_key_statement) != c.ZOVA_OK or
                    c.zova_statement_clear_bindings(token_key_statement) != c.ZOVA_OK or
                    !workspaceBindI64(db, token_key_statement, 1, workspace_key, "user_delta.token_key_ws") or
                    !workspaceBindText(db, token_key_statement, 2, token, "user_delta.token_key_token")) return false;
                var step: c.zova_step_result = c.ZOVA_STEP_DONE;
                var token_key: i64 = 0;
                if (!statusOk(stepStatement(token_key_statement, &step), db, "user_delta.token_key_step") or
                    step != c.ZOVA_STEP_ROW or
                    !statusOk(columnI64(token_key_statement, 0, &token_key), db, "user_delta.token_key_value") or
                    !userPrivateKeyId(token_key, idAt(token_vector_ids, i))) return false;
                token_vectors[i] = idAt(token_vector_ids, i);
            }
        }
        if (node_vector_deletes != 0 and !statusOk(c.zova_vector_delete_many(&c.zova_vector_delete_many_request{
            .db = db,
            .collection_name = node_collection,
            .vector_ids = node_vectors,
            .vector_count = node_vector_deletes,
        }), db, "user_delta.node_vector_delete")) return false;
        if (token_vector_deletes != 0 and !statusOk(c.zova_vector_delete_many(&c.zova_vector_delete_many_request{
            .db = db,
            .collection_name = token_collection,
            .vector_ids = token_vectors,
            .vector_count = token_vector_deletes,
        }), db, "user_delta.token_vector_delete")) return false;
        if (token_vector_deletes != 0 and !userDeltaDeleteIdsTx(
            db,
            workspace_key,
            "DELETE FROM cbm_token_vector_metadata_v1 WHERE workspace_key=?1 AND token=?2",
            "user_delta.token_delete",
            delta,
            c.cbm_zova_workspace_delta_token_vector_delete_at,
            metrics.token_vector_deletes,
        )) return false;
    }
    if (testFailPhase("user_delta_after_vector_deletes")) return false;

    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_graph_name(workspace_id, &graph_name, graph_name.len) != 0) return false;
    const topology_deletes: usize = @intCast(metrics.topology_deletes);
    const node_deletes: usize = @intCast(metrics.node_deletes);
    const edges = callocList(c.zova_graph_edge_input, topology_deletes);
    defer c.free(edges);
    const nodes = callocList(CStr, node_deletes);
    defer c.free(@ptrCast(nodes));
    if ((topology_deletes != 0 and edges == null) or (node_deletes != 0 and nodes == null)) return false;
    for (0..topology_deletes) |i| {
        const edge = c.cbm_zova_workspace_delta_topology_delete_at(delta, @intCast(i));
        if (edge == null) return false;
        edges[i] = .{
            .graph_name = &graph_name,
            .from_node_id = edge.*.source_stable_id,
            .edge_type = edge.*.edge_type,
            .to_node_id = edge.*.target_stable_id,
        };
    }
    for (0..node_deletes) |i| nodes[i] = c.cbm_zova_workspace_delta_node_delete_at(delta, @intCast(i));
    if (topology_deletes != 0 and !statusOk(c.zova_graph_edge_delete_many(&c.zova_graph_edge_delete_many_request{
        .db = db,
        .edges = edges,
        .edges_len = topology_deletes,
    }), db, "user_delta.graph_edge_delete")) return false;
    if (node_deletes != 0 and !statusOk(c.zova_graph_node_delete_many(&c.zova_graph_node_delete_many_request{
        .db = db,
        .graph_name = &graph_name,
        .node_ids = nodes,
        .node_count = node_deletes,
    }), db, "user_delta.graph_node_delete")) return false;
    return !testFailPhase("user_delta_after_graph_deletes");
}

/// Deletes the cbm_nodes_v1 and cbm_file_hashes_v1 rows the delta removes.
fn userDeltaDeleteCanonicalTx(db: ?*Db, workspace_key: i64, node_index: *const NodeKeyIndex, delta: ?*const WorkspaceDelta, metrics: *const DeltaMetrics) bool {
    if (metrics.node_deletes != 0) {
        const node_delete = prepareZova(db, "DELETE FROM cbm_nodes_v1 WHERE workspace_key=?1 AND zova_node_key=?2", "user_delta.node_delete_prepare") orelse return false;
        defer finalize(node_delete);
        for (0..@intCast(metrics.node_deletes)) |i| {
            const stable_id = c.cbm_zova_workspace_delta_node_delete_at(delta, @intCast(i));
            if (stable_id == null) return false;
            const node_key = nodeKeyIndexFind(node_index, stable_id) orelse return false;
            if (!workspaceBindI64(db, node_delete, 1, workspace_key, "user_delta.node_delete_ws") or
                !workspaceBindI64(db, node_delete, 2, node_key, "user_delta.node_delete_key") or
                !workspaceStepDone(db, node_delete, "user_delta.node_delete_step") or
                !userDatabaseResetStatement(node_delete, "user_delta.node_delete_reset")) return false;
        }
    }
    if (!userDeltaDeleteIdsTx(
        db,
        workspace_key,
        "DELETE FROM cbm_file_hashes_v1 WHERE file_key=(SELECT file_key FROM cbm_files_v1 " ++
            "WHERE workspace_key=?1 AND file_path=?2)",
        "user_delta.hash_delete",
        delta,
        c.cbm_zova_workspace_delta_file_hash_delete_at,
        metrics.file_hash_deletes,
    )) return false;
    return !testFailPhase("user_delta_after_canonical_deletes");
}

/// The node the delta inserts or updates at `index`: inserts come first.
fn deltaNodeUpsertAt(delta: ?*const WorkspaceDelta, metrics: *const DeltaMetrics, index: usize) [*c]const c.cbm_zova_publish_node_t {
    const inserts: usize = @intCast(metrics.node_inserts);
    return if (index < inserts)
        c.cbm_zova_workspace_delta_node_insert_at(delta, @intCast(index))
    else
        c.cbm_zova_workspace_delta_node_update_at(delta, @intCast(index - inserts));
}

fn userDeltaUpsertNodesTx(
    db: ?*Db,
    workspace_key: i64,
    delta: ?*const WorkspaceDelta,
    metrics: *const DeltaMetrics,
    file_keys: []const UserFileKey,
    node_keys: []const UserNodeKey,
) bool {
    const total: usize = @intCast(metrics.node_inserts + metrics.node_updates);
    if (total == 0) return true;
    const node = prepareZova(db, "INSERT INTO cbm_nodes_v1(zova_node_key,workspace_key,label,name,qualified_name,file_key," ++
        "start_line,end_line,properties,source_ordinal) " ++
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) " ++
        "ON CONFLICT(zova_node_key) DO UPDATE SET " ++
        "label=excluded.label,name=excluded.name,qualified_name=excluded.qualified_name," ++
        "file_key=excluded.file_key,start_line=excluded.start_line,end_line=excluded.end_line," ++
        "properties=excluded.properties,source_ordinal=excluded.source_ordinal", "user_delta.node_upsert_prepare") orelse return false;
    defer finalize(node);
    for (0..total) |i| {
        const row = deltaNodeUpsertAt(delta, metrics, i);
        const source = row.*.source;
        const node_key = userNodeKeyFind(node_keys, row.*.stable_id);
        const file_key = userFileKeyFind(file_keys, source.*.file_path);
        if (node_key <= 0 or file_key <= 0) return false;
        if (!workspaceBindI64(db, node, 1, node_key, "user_delta.node_key") or
            !workspaceBindI64(db, node, 2, workspace_key, "user_delta.node_ws") or
            !workspaceBindText(db, node, 3, source.*.label, "user_delta.node_label") or
            !workspaceBindText(db, node, 4, source.*.name, "user_delta.node_name") or
            !workspaceBindText(db, node, 5, source.*.qualified_name, "user_delta.node_qn") or
            !workspaceBindI64(db, node, 6, file_key, "user_delta.node_file") or
            !workspaceBindI64(db, node, 7, source.*.start_line, "user_delta.node_start") or
            !workspaceBindI64(db, node, 8, source.*.end_line, "user_delta.node_end") or
            !workspaceBindText(db, node, 9, if (source.*.properties != null) source.*.properties else "{}", "user_delta.node_props") or
            !workspaceBindI64(db, node, 10, @bitCast(row.*.source_ordinal), "user_delta.node_source_ordinal") or
            !workspaceStepDone(db, node, "user_delta.node_step") or
            !userDatabaseResetStatement(node, "user_delta.node_reset")) return false;
    }
    return true;
}

/// The token key of every token vector the delta upserts, sorted by token:
/// the stored key of a known token, a new key otherwise.
fn userDeltaTokenKeysTx(db: ?*Db, workspace_key: i64, delta: ?*const WorkspaceDelta, metrics: *const DeltaMetrics, out: *List(UserTokenKey)) bool {
    out.* = .{};
    const count: usize = @intCast(metrics.token_vector_upserts);
    if (count == 0) return true;
    const keys = interop.callocN(UserTokenKey, count);
    if (keys == null) return false;
    var done = false;
    defer if (!done) c.free(keys);
    const next_key = nextPrivateKey(db, "SELECT COALESCE(MAX(token_key),0)+1 FROM cbm_token_vector_metadata_v1", "user_delta.next_token_key") orelse return false;
    {
        const statement = prepareZova(db, "SELECT token_key FROM cbm_token_vector_metadata_v1 " ++
            "WHERE workspace_key=?1 AND token=?2", "user_delta.token_lookup_prepare") orelse return false;
        defer finalize(statement);
        var allocated: i64 = 0;
        for (0..count) |i| {
            const row = c.cbm_zova_workspace_delta_token_vector_upsert_at(delta, @intCast(i));
            if (row == null or row.*.source == null or row.*.source.*.token == null) return false;
            if (c.zova_statement_reset(statement) != c.ZOVA_OK or
                c.zova_statement_clear_bindings(statement) != c.ZOVA_OK or
                !workspaceBindI64(db, statement, 1, workspace_key, "user_delta.token_lookup_ws") or
                !workspaceBindText(db, statement, 2, row.*.source.*.token, "user_delta.token_lookup_token")) return false;
            var step: c.zova_step_result = c.ZOVA_STEP_DONE;
            if (!statusOk(stepStatement(statement, &step), db, "user_delta.token_lookup_step")) return false;
            var key: i64 = 0;
            if (step == c.ZOVA_STEP_ROW) {
                if (!statusOk(columnI64(statement, 0, &key), db, "user_delta.token_lookup_value")) return false;
            } else if (step == c.ZOVA_STEP_DONE) {
                key = next_key + allocated;
                allocated += 1;
            } else {
                return false;
            }
            if (key <= 0) return false;
            keys[i] = .{ .token = row.*.source.*.token, .token_key = key };
        }
    }
    if (count > 1) c.qsort(keys, count, @sizeOf(UserTokenKey), userTokenKeyCompare);
    done = true;
    out.* = .{ .items = keys, .count = count };
    return true;
}

/// Puts the nodes, edges, edge payloads and vectors the delta inserts or
/// updates. Returns the Zova key of every put node, sorted by stable id.
fn userDeltaPutNativeTx(
    db: ?*Db,
    workspace_id: Str,
    node_index: *const NodeKeyIndex,
    node_collection: Str,
    token_collection: Str,
    delta: ?*const WorkspaceDelta,
    metrics: *const DeltaMetrics,
    model: ?*const PublishModel,
    out_node_keys: *List(UserNodeKey),
    token_keys: []const UserTokenKey,
) bool {
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_graph_name(workspace_id, &graph_name, graph_name.len) != 0) return false;
    const node_count: usize = @intCast(metrics.node_inserts + metrics.node_updates);
    const edge_inserts: usize = @intCast(metrics.edge_inserts);
    const node_keys = callocList(UserNodeKey, node_count);
    var done = false;
    defer if (!done) c.free(node_keys);
    {
        const nodes = callocList(c.zova_graph_node_input, node_count);
        defer c.free(nodes);
        const edges = callocList(c.zova_graph_edge_input, edge_inserts);
        defer c.free(edges);
        const node_key_values = callocList(i64, node_count);
        defer c.free(node_key_values);
        const edge_keys = callocList(i64, edge_inserts);
        defer c.free(edge_keys);
        if ((node_count != 0 and (nodes == null or node_key_values == null or node_keys == null)) or
            (edge_inserts != 0 and (edges == null or edge_keys == null))) return false;
        for (0..node_count) |i| {
            const row = deltaNodeUpsertAt(delta, metrics, i);
            nodes[i] = .{
                .graph_name = &graph_name,
                .node_id = row.*.stable_id,
                .kind = row.*.source.*.label,
                .target_type = c.ZOVA_GRAPH_TARGET_NONE,
                .target_namespace = null,
                .target_ref = null,
            };
            node_keys[i].stable_id = row.*.stable_id;
        }
        for (0..edge_inserts) |i| {
            const row = c.cbm_zova_workspace_delta_edge_insert_at(delta, @intCast(i));
            edges[i] = .{
                .graph_name = &graph_name,
                .from_node_id = row.*.source_stable_id,
                .edge_type = row.*.source.*.type,
                .to_node_id = row.*.target_stable_id,
            };
        }
        if (node_count != 0 and !statusOk(c.zova_graph_node_put_many_keyed(&c.zova_graph_node_put_many_keyed_request{
            .db = db,
            .nodes = nodes,
            .nodes_len = node_count,
            .out_node_keys = node_key_values,
            .out_node_keys_capacity = node_count,
        }), db, "user_delta.graph_nodes")) return false;
        for (0..node_count) |i| {
            node_keys[i].node_key = node_key_values[i];
            if (node_keys[i].node_key <= 0) return false;
        }
        if (node_count > 1) c.qsort(node_keys, node_count, @sizeOf(UserNodeKey), userNodeKeyCompare);
        if (edge_inserts != 0 and !statusOk(c.zova_graph_edge_put_many_keyed(&c.zova_graph_edge_put_many_keyed_request{
            .db = db,
            .edges = edges,
            .edges_len = edge_inserts,
            .out_edge_keys = edge_keys,
            .out_edge_keys_capacity = edge_inserts,
        }), db, "user_delta.graph_edges")) return false;
        for (0..edge_inserts) |i| {
            if (edge_keys[i] <= 0) return false;
        }

        // Every topology edge that gained or lost a logical edge gets its
        // payload replaced.
        const topology_count = countOf(c.cbm_zova_publish_model_topology_count(model));
        const affected = callocList(bool, topology_count);
        defer c.free(affected);
        const replacements = callocList(c.zova_graph_edge_payload_replacement, topology_count);
        defer c.free(replacements);
        if (topology_count != 0 and (affected == null or replacements == null)) return false;
        for (0..edge_inserts) |i| {
            const row = c.cbm_zova_workspace_delta_edge_insert_at(delta, @intCast(i));
            if (row == null or row.*.topology_ordinal >= topology_count) return false;
            affected[@intCast(row.*.topology_ordinal)] = true;
        }
        for (0..@intCast(metrics.edge_deletes)) |i| {
            const deleted = c.cbm_zova_workspace_delta_edge_delete_at(delta, @intCast(i));
            if (deleted == null) return false;
            if (userPublishModelTopologyFind(model, deleted.*.source_stable_id, deleted.*.edge_type, deleted.*.target_stable_id)) |row| {
                if (row.ordinal >= topology_count) return false;
                affected[@intCast(row.ordinal)] = true;
            }
        }
        var replacement_count: usize = 0;
        for (0..topology_count) |i| {
            if (!affected[i]) continue;
            const row = c.cbm_zova_publish_model_topology_at(model, @intCast(i));
            if (row == null) return false;
            const edge_key = edgeKeyForIdentity(db, workspace_id, row.*.source_stable_id, row.*.source.*.type, row.*.target_stable_id) orelse return false;
            if (edge_key <= 0) return false;
            replacements[replacement_count] = .{
                .edge_key = edge_key,
                .payload = row.*.payload,
                .payload_len = row.*.payload_len,
            };
            replacement_count += 1;
        }
        if (replacement_count > 0 and !statusOk(c.zova_graph_edge_payload_replace_many(&c.zova_graph_edge_payload_replace_many_request{
            .db = db,
            .graph_name = &graph_name,
            .replacements = replacements,
            .replacement_count = replacement_count,
        }), db, "user_delta.graph_payloads")) return false;
    }
    if (testFailPhase("user_delta_after_graph_puts")) return false;

    const input = c.cbm_zova_publish_model_input(model);
    const node_vector_upserts: usize = @intCast(metrics.node_vector_upserts);
    const token_vector_upserts: usize = @intCast(metrics.token_vector_upserts);
    const node_vectors = callocList(c.zova_vector_input, node_vector_upserts);
    defer c.free(node_vectors);
    const token_vectors = callocList(c.zova_vector_input, token_vector_upserts);
    defer c.free(token_vectors);
    const node_vector_ids = callocList([32]u8, node_vector_upserts);
    defer c.free(node_vector_ids);
    const token_vector_ids = callocList([32]u8, token_vector_upserts);
    defer c.free(token_vector_ids);
    if ((node_vector_upserts != 0 and (node_vectors == null or node_vector_ids == null)) or
        (token_vector_upserts != 0 and (token_vectors == null or token_vector_ids == null))) return false;
    for (0..node_vector_upserts) |i| {
        const row = c.cbm_zova_workspace_delta_node_vector_upsert_at(delta, @intCast(i));
        // A node this delta put has its key in `node_keys`; any other node
        // kept the key the index read before the graph writes.
        const put_key = userNodeKeyFind(many(node_keys, node_count), row.*.stable_id);
        const key = if (put_key > 0) put_key else (nodeKeyIndexFind(node_index, row.*.stable_id) orelse return false);
        if (!userPrivateKeyId(key, idAt(node_vector_ids, i))) return false;
        node_vectors[i] = i8VectorInput(idAt(node_vector_ids, i), row.*.source.*.vector, input.*.vector_dimensions);
    }
    for (0..token_vector_upserts) |i| {
        const row = c.cbm_zova_workspace_delta_token_vector_upsert_at(delta, @intCast(i));
        const key = userTokenKeyFind(token_keys, row.*.source.*.token);
        if (!userPrivateKeyId(key, idAt(token_vector_ids, i))) return false;
        token_vectors[i] = i8VectorInput(idAt(token_vector_ids, i), row.*.source.*.vector, input.*.vector_dimensions);
    }
    if (node_vector_upserts != 0 and !statusOk(c.zova_vector_put_many(&c.zova_vector_put_many_request{
        .db = db,
        .collection_name = node_collection,
        .vectors = node_vectors,
        .vectors_len = node_vector_upserts,
    }), db, "user_delta.node_vectors")) return false;
    if (token_vector_upserts != 0 and !statusOk(c.zova_vector_put_many(&c.zova_vector_put_many_request{
        .db = db,
        .collection_name = token_collection,
        .vectors = token_vectors,
        .vectors_len = token_vector_upserts,
    }), db, "user_delta.token_vectors")) return false;
    if (testFailPhase("user_delta_after_vector_puts")) return false;
    done = true;
    out_node_keys.* = .{ .items = node_keys, .count = node_count };
    return true;
}

/// Upserts the token metadata and file hashes of the delta, replaces the
/// project summary when the delta carries one, and drops file rows nothing
/// refers to any more.
fn userDeltaUpsertMetadataTx(
    db: ?*Db,
    workspace_id: Str,
    workspace_key: i64,
    delta: ?*const WorkspaceDelta,
    metrics: *const DeltaMetrics,
    file_keys: []const UserFileKey,
    token_keys: []const UserTokenKey,
) bool {
    if (metrics.token_vector_upserts != 0) {
        const statement = prepareZova(db, "INSERT INTO cbm_token_vector_metadata_v1(token_key,workspace_key,token,idf) " ++
            "VALUES(?1,?2,?3,?4) ON CONFLICT(workspace_key,token) DO UPDATE SET " ++
            "idf=excluded.idf", "user_delta.token_upsert_prepare") orelse return false;
        defer finalize(statement);
        for (0..@intCast(metrics.token_vector_upserts)) |i| {
            const row = c.cbm_zova_workspace_delta_token_vector_upsert_at(delta, @intCast(i));
            const token_key = userTokenKeyFind(token_keys, row.*.source.*.token);
            if (token_key <= 0) return false;
            if (!workspaceBindI64(db, statement, 1, token_key, "user_delta.token_key") or
                !workspaceBindI64(db, statement, 2, workspace_key, "user_delta.token_ws") or
                !workspaceBindText(db, statement, 3, row.*.source.*.token, "user_delta.token_value") or
                !statusOk(c.zova_statement_bind_double(&c.zova_statement_bind_double_request{
                    .statement = statement,
                    .index = 4,
                    .value = row.*.source.*.idf,
                }), db, "user_delta.token_idf") or
                !workspaceStepDone(db, statement, "user_delta.token_step") or
                !userDatabaseResetStatement(statement, "user_delta.token_reset")) return false;
        }
    }
    if (metrics.file_hash_upserts != 0) {
        const statement = prepareZova(db, "INSERT INTO cbm_file_hashes_v1(file_key,content_hash,mtime_ns,size_bytes) " ++
            "VALUES(?1,?2,?3,?4) ON CONFLICT(file_key) DO UPDATE SET " ++
            "content_hash=excluded.content_hash,mtime_ns=excluded.mtime_ns,size_bytes=excluded.size_bytes", "user_delta.hash_upsert_prepare") orelse return false;
        defer finalize(statement);
        for (0..@intCast(metrics.file_hash_upserts)) |i| {
            const row = c.cbm_zova_workspace_delta_file_hash_upsert_at(delta, @intCast(i));
            const file_key = userFileKeyFind(file_keys, row.*.file_path);
            if (file_key <= 0) return false;
            if (!workspaceBindI64(db, statement, 1, file_key, "user_delta.hash_file_key") or
                !workspaceBindText(db, statement, 2, row.*.content_hash, "user_delta.hash_value") or
                !workspaceBindI64(db, statement, 3, row.*.mtime_ns, "user_delta.hash_mtime") or
                !workspaceBindI64(db, statement, 4, row.*.size_bytes, "user_delta.hash_size") or
                !workspaceStepDone(db, statement, "user_delta.hash_step") or
                !userDatabaseResetStatement(statement, "user_delta.hash_reset")) return false;
        }
    }
    if (c.cbm_zova_workspace_delta_replaces_summary(delta) and
        !userDatabaseWriteSummary(db, workspace_id, c.cbm_zova_workspace_delta_summary(delta))) return false;
    const may_have_orphan_files = metrics.node_deletes != 0 or metrics.node_updates != 0 or metrics.file_hash_deletes != 0;
    if (may_have_orphan_files) {
        const statement = prepareZova(db, "DELETE FROM cbm_files_v1 WHERE workspace_key=?1 " ++
            "AND NOT EXISTS(SELECT 1 FROM cbm_nodes_v1 n " ++
            "WHERE n.workspace_key=cbm_files_v1.workspace_key " ++
            "AND n.file_key=cbm_files_v1.file_key) " ++
            "AND NOT EXISTS(SELECT 1 FROM cbm_file_hashes_v1 h WHERE h.file_key=cbm_files_v1.file_key)", "user_delta.files_cleanup_prepare") orelse return false;
        defer finalize(statement);
        if (!workspaceBindI64(db, statement, 1, workspace_key, "user_delta.files_cleanup_ws") or
            !workspaceStepDone(db, statement, "user_delta.files_cleanup_step")) return false;
    }
    return !testFailPhase("user_delta_after_metadata_upserts");
}

/// Copies the timings of the publish model build into the result.
fn resultSetModelMetrics(out_result: *GenerationResult, model_metrics: *const c.cbm_zova_publish_model_metrics_t) void {
    out_result.normalization_ms = model_metrics.normalization_ms;
    out_result.model_nodes_ms = model_metrics.nodes_ms;
    out_result.model_edges_ms = model_metrics.edges_ms;
    out_result.model_edge_endpoint_ms = model_metrics.prepared_endpoint_ms;
    out_result.model_edge_sort_ms = model_metrics.prepared_topology_sort_ms;
    out_result.model_edge_group_ms = model_metrics.prepared_topology_group_ms;
    out_result.model_edge_payload_ms = model_metrics.prepared_payload_ms;
    out_result.model_edge_digest_ms = model_metrics.prepared_topology_digest_ms;
    out_result.model_edge_default_payloads = model_metrics.prepared_single_default_payload_count;
    out_result.model_edge_payload_scratch_edges = model_metrics.prepared_payload_scratch_edge_count;
    out_result.model_hashes_ms = model_metrics.hashes_ms;
    out_result.model_vectors_ms = model_metrics.vectors_ms;
    out_result.model_digests_ms = model_metrics.digests_ms;
}

/// Applies `delta` on top of the active generation and makes the result the
/// next ready generation. Runs inside the caller's transaction.
fn userDatabasePublishDeltaTx(db: ?*Db, model: ?*const PublishModel, delta: ?*const WorkspaceDelta, out_result: *GenerationResult) bool {
    if (db == null or model == null or delta == null) return false;
    const input = c.cbm_zova_publish_model_input(model);
    const model_workspace_id = c.cbm_zova_publish_model_workspace_id(model);
    const digests = c.cbm_zova_publish_model_digests(model);
    if (input == null or model_workspace_id == null or digests == null) return false;
    out_result.* = digests.*;
    var metrics: DeltaMetrics = .{};
    c.cbm_zova_workspace_delta_metrics(delta, &metrics);
    var model_metrics: c.cbm_zova_publish_model_metrics_t = .{};
    c.cbm_zova_publish_model_metrics(model, &model_metrics);
    out_result.publication_mode = c.CBM_ZOVA_PUBLICATION_MODE_DELTA;
    out_result.inserted_count = metrics.node_inserts + metrics.edge_inserts;
    out_result.updated_count = metrics.node_updates + metrics.node_vector_upserts +
        metrics.token_vector_upserts + metrics.file_hash_upserts + metrics.summary_replacements;
    out_result.deleted_count = metrics.node_deletes + metrics.edge_deletes +
        metrics.node_vector_deletes + metrics.token_vector_deletes + metrics.file_hash_deletes;
    resultSetModelMetrics(out_result, &model_metrics);
    out_result.diff_ms = metrics.diff_ms;
    out_result.full_clear_count = 0;
    out_result.unchanged_rewrite_count = 0;
    out_result.nodes_inserted = metrics.node_inserts;
    out_result.nodes_updated = metrics.node_updates;
    out_result.nodes_deleted = metrics.node_deletes;
    out_result.edges_inserted = metrics.edge_inserts;
    out_result.edges_deleted = metrics.edge_deletes;
    out_result.node_vectors_upserted = metrics.node_vector_upserts;
    out_result.node_vectors_deleted = metrics.node_vector_deletes;
    out_result.token_vectors_upserted = metrics.token_vector_upserts;
    out_result.token_vectors_deleted = metrics.token_vector_deletes;
    if (!registerSqlFunctions(db)) return false;
    var workspace_id_buf: [ID_MAX]u8 = @splat(0);
    if (!userWorkspaceIdTx(db, @ptrCast(input.*.root_path), &workspace_id_buf) or
        c.strcmp(&workspace_id_buf, model_workspace_id) != 0) return false;
    const workspace_id = bufZ(&workspace_id_buf);
    const workspace_key = workspaceKeyForId(db, workspace_id) orelse return false;
    const expected_generation = c.cbm_zova_workspace_delta_expected_generation(delta);
    if (expected_generation <= 0) return false;
    const active_generation = zovaQueryI64(db, "SELECT r.active_generation FROM cbm_workspace_registry r " ++
        "JOIN cbm_database_generation_v1 g ON g.workspace_key=r.workspace_key " ++
        "AND g.generation=r.active_generation AND g.state='ready' " ++
        "WHERE r.workspace_id=?1", workspace_id, "user_delta.active_generation") orelse return false;
    if (active_generation != expected_generation) return false;
    const current_max = zovaQueryI64(db, "SELECT COALESCE(MAX(generation),0) FROM cbm_database_generation_v1 " ++
        "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry " ++
        "WHERE workspace_id=?1)", workspace_id, "user_delta.current_generation") orelse return false;
    if (current_max <= 0 or current_max == std.math.maxInt(i64)) return false;
    const generation = current_max + 1;
    if (!userPublishInsertGenerationTx(db, workspace_id, generation)) return false;
    if (testFailPhase("user_delta_after_generation")) return false;

    var node_collection_buf: [COLLECTION_NAME_CAP]u8 = undefined;
    var token_collection_buf: [COLLECTION_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_node_vector_collection_name(workspace_id, @ptrCast(input.*.model_fingerprint), input.*.vector_dimensions, &node_collection_buf, node_collection_buf.len) != 0 or
        cbm_zova_workspace_token_vector_collection_name(workspace_id, @ptrCast(input.*.model_fingerprint), input.*.vector_dimensions, &token_collection_buf, token_collection_buf.len) != 0) return false;
    const node_collection = bufZ(&node_collection_buf);
    const token_collection = bufZ(&token_collection_buf);
    var file_keys: List(UserFileKey) = .{};
    defer c.free(file_keys.items);
    var delta_node_keys: List(UserNodeKey) = .{};
    defer c.free(delta_node_keys.items);
    var delta_token_keys: List(UserTokenKey) = .{};
    defer c.free(delta_token_keys.items);
    var ok = true;
    {
        var node_index: NodeKeyIndex = .{};
        defer nodeKeyIndexFree(&node_index);
        if (metrics.node_deletes != 0 or metrics.node_vector_deletes != 0 or metrics.node_vector_upserts != 0) {
            ok = nodeKeyIndexBuild(db, workspace_id, &node_index);
        }
        ok = ok and userDeltaDeleteCanonicalTx(db, workspace_key, &node_index, delta, &metrics);
        ok = ok and userDeltaDeleteNativeTx(db, workspace_id, &node_index, node_collection, token_collection, delta, &metrics);
        ok = ok and userPublishProjectTx(db, workspace_id, input.*.root_path, input.*.project, input.*.indexed_at);
        ok = ok and userDeltaFileKeysTx(db, workspace_key, delta, &metrics, &file_keys);
        ok = ok and userDeltaTokenKeysTx(db, workspace_key, delta, &metrics, &delta_token_keys);
        ok = ok and userDeltaPutNativeTx(db, workspace_id, &node_index, node_collection, token_collection, delta, &metrics, model, &delta_node_keys, delta_token_keys.slice());
    }
    ok = ok and userDeltaUpsertNodesTx(db, workspace_key, delta, &metrics, file_keys.slice(), delta_node_keys.slice());
    ok = ok and !testFailPhase("user_delta_after_node_upserts");
    ok = ok and !testFailPhase("user_delta_after_edge_inserts");
    ok = ok and userDeltaUpsertMetadataTx(db, workspace_id, workspace_key, delta, &metrics, file_keys.slice(), delta_token_keys.slice());
    out_result.generation = generation;

    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    ok = ok and cbm_zova_workspace_graph_name(workspace_id, &graph_name, graph_name.len) == 0;
    if (ok) {
        var graph_info: c.zova_graph_info = .{};
        defer c.zova_graph_info_free(&graph_info);
        ok = statusOk(c.zova_graph_info_get(&c.zova_graph_info_get_request{
            .db = db,
            .name = &graph_name,
            .out_info = &graph_info,
        }), db, "user_delta.graph_readback") and
            graph_info.node_count == out_result.graph_nodes and
            graph_info.edge_count == out_result.graph_edges;
    }
    ok = ok and userPublishNativeReadbackCountsTx(db, out_result, node_collection, token_collection);
    if (ok) {
        user_publish_test_metrics.integrity_writes += 1;
        ok = userPublishIntegrityTx(db, workspace_id, generation, out_result);
    }
    ok = ok and !testFailPhase("user_delta_after_integrity");
    ok = ok and userPublishWriteIndexStateTx(db, workspace_id, generation, input.*.model_fingerprint, input.*.vector_dimensions, input.*.indexed_at);
    ok = ok and userPublishClearHealthTx(db, workspace_id);
    ok = ok and userPublishGenerationStateTx(db, workspace_id, generation, true);
    ok = ok and !testFailPhase("user_delta_before_commit");
    _ = interop.bprint(&out_result.workspace_id, "{s}", .{span(workspace_id)});
    if (ok) {
        user_publish_test_metrics.delta_authoritative_rows_touched +=
            metrics.node_vector_deletes + metrics.token_vector_deletes +
            metrics.topology_deletes + metrics.node_deletes +
            metrics.node_deletes + metrics.node_updates + metrics.edge_deletes +
            metrics.token_vector_deletes + metrics.file_hash_deletes +
            metrics.node_inserts + metrics.node_updates +
            metrics.node_inserts + metrics.node_updates + metrics.edge_inserts +
            metrics.topology_inserts + metrics.node_inserts + metrics.node_updates +
            metrics.node_vector_upserts + metrics.token_vector_upserts +
            metrics.token_vector_upserts + metrics.file_hash_upserts +
            metrics.summary_replacements;
    }
    return ok;
}

// ── Publication: full, through a fresh build session ────────────

fn userExecI64Bound(db: ?*Db, sql: Str, bind_value: i64, phase: Str) bool {
    if (db == null) return false;
    const stmt = prepareZova(db, sql, phase) orelse return false;
    defer finalize(stmt);
    return workspaceBindI64(db, stmt, 1, bind_value, phase) and workspaceStepDone(db, stmt, phase);
}

/// Drops the FTS insert trigger so a full publish can load cbm_nodes_v1
/// without one trigger run per row.
fn userPublishFullNodesBeginTx(db: ?*Db) bool {
    return statusOk(execSql(db, "DROP TRIGGER cbm_nodes_v1_fts_ai;"), db, "user_publish.nodes_bulk_begin");
}

/// Restores the FTS insert trigger; with `populate` it first fills the FTS
/// table from the node rows of the workspace in one statement.
fn userPublishFullNodesEndTx(db: ?*Db, workspace_key: i64, populate: bool) bool {
    var ok = true;
    if (populate) {
        ok = userExecI64Bound(db, "INSERT INTO cbm_nodes_fts_v1(rowid,name,qualified_name,file_path,label) " ++
            "SELECT n.zova_node_key,cbm_camel_split(n.name),n.qualified_name,f.file_path,n.label " ++
            "FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key " ++
            "WHERE n.workspace_key=?1 ORDER BY n.zova_node_key", workspace_key, "user_publish.fts_bulk_insert");
        if (ok) user_publish_test_metrics.full_fts_bulk_statements += 1;
    }
    const restored = statusOk(execSql(db, node_fts_insert_trigger_sql), db, "user_publish.nodes_bulk_restore");
    return ok and restored;
}

const USER_FRESH_BATCH_ROWS: usize = 512;

fn userFreshI64(value: i64) c.zova_fresh_value {
    return .{ .value_type = c.ZOVA_FRESH_VALUE_INT64, .int64_value = value };
}

fn userFreshText(value: CStr) c.zova_fresh_value {
    const text: CStr = if (value != null) value else "";
    return .{ .value_type = c.ZOVA_FRESH_VALUE_TEXT, .bytes = text, .bytes_len = c.strlen(text) };
}

fn userFreshLoadNodes(
    db: ?*Db,
    build: ?*c.zova_fresh_build,
    model: ?*const PublishModel,
    workspace_key: i64,
    file_keys: []const UserFileKey,
    node_keys: []const i64,
) bool {
    const columns = [_]CStr{
        "zova_node_key", "workspace_key", "label",    "name",       "qualified_name",
        "file_key",      "start_line",    "end_line", "properties", "source_ordinal",
    };
    const count = countOf(c.cbm_zova_publish_model_node_count(model));
    const values = if (count != 0) interop.callocN(c.zova_fresh_value, USER_FRESH_BATCH_ROWS * columns.len) else null;
    defer c.free(values);
    if (count != 0 and values == null) return false;
    user_publish_test_metrics.canonical_node_fts_passes += 1;
    var offset: usize = 0;
    while (offset < count) : (offset += USER_FRESH_BATCH_ROWS) {
        const rows = @min(count - offset, USER_FRESH_BATCH_ROWS);
        for (0..rows) |row_index| {
            const index = offset + row_index;
            const row = c.cbm_zova_publish_model_node_at(model, @intCast(index));
            if (row == null or row.*.source == null) return false;
            const source = row.*.source;
            const file_key = userFileKeyFind(file_keys, source.*.file_path);
            if (index >= node_keys.len or node_keys[index] <= 0 or file_key <= 0 or row.*.ordinal != index) return false;
            const out = values + row_index * columns.len;
            out[0] = userFreshI64(node_keys[index]);
            out[1] = userFreshI64(workspace_key);
            out[2] = userFreshText(source.*.label);
            out[3] = userFreshText(source.*.name);
            out[4] = userFreshText(source.*.qualified_name);
            out[5] = userFreshI64(file_key);
            out[6] = userFreshI64(source.*.start_line);
            out[7] = userFreshI64(source.*.end_line);
            out[8] = userFreshText(if (source.*.properties != null) source.*.properties else "{}");
            out[9] = userFreshI64(@bitCast(row.*.source_ordinal));
        }
        if (!statusOk(c.zova_fresh_build_table_rows(&c.zova_fresh_build_rows_request{
            .build = build,
            .table_name = "cbm_nodes_v1",
            .column_names = &columns,
            .column_count = columns.len,
            .values = values,
            .row_count = rows,
        }), db, "user_publish.fresh_nodes")) return false;
    }
    return true;
}

fn userFreshLoadFts(db: ?*Db, build: ?*c.zova_fresh_build, model: ?*const PublishModel, node_keys: []const i64) bool {
    const columns = [_]CStr{ "rowid", "name", "qualified_name", "file_path", "label" };
    const count = countOf(c.cbm_zova_publish_model_node_count(model));
    const values = if (count != 0) interop.callocN(c.zova_fresh_value, USER_FRESH_BATCH_ROWS * columns.len) else null;
    defer c.free(values);
    if (count != 0 and values == null) return false;
    var offset: usize = 0;
    while (offset < count) : (offset += USER_FRESH_BATCH_ROWS) {
        const rows = @min(count - offset, USER_FRESH_BATCH_ROWS);
        for (0..rows) |row_index| {
            const index = offset + row_index;
            const row = c.cbm_zova_publish_model_node_at(model, @intCast(index));
            if (row == null or row.*.source == null) return false;
            const source = row.*.source;
            const fts_name = c.cbm_zova_publish_model_fts_name_at(model, @intCast(index));
            if (fts_name == null or index >= node_keys.len or node_keys[index] <= 0) return false;
            const out = values + row_index * columns.len;
            out[0] = userFreshI64(node_keys[index]);
            out[1] = userFreshText(fts_name);
            out[2] = userFreshText(source.*.qualified_name);
            out[3] = userFreshText(source.*.file_path);
            out[4] = userFreshText(source.*.label);
        }
        if (!statusOk(c.zova_fresh_build_fts_rows(&c.zova_fresh_build_rows_request{
            .build = build,
            .table_name = "cbm_nodes_fts_v1",
            .column_names = &columns,
            .column_count = columns.len,
            .values = values,
            .row_count = rows,
        }), db, "user_publish.fresh_fts")) return false;
    }
    return true;
}

fn userFreshLoadNodeVectors(db: ?*Db, build: ?*c.zova_fresh_build, model: ?*const PublishModel, collection: Str, node_keys: []const i64) bool {
    const input = c.cbm_zova_publish_model_input(model);
    const count = countOf(c.cbm_zova_publish_model_node_vector_count(model));
    const vectors = if (count != 0) interop.callocN(c.zova_vector_input, USER_FRESH_BATCH_ROWS) else null;
    defer c.free(vectors);
    const ids = if (count != 0) interop.callocN([32]u8, USER_FRESH_BATCH_ROWS) else null;
    defer c.free(ids);
    if (count != 0 and (vectors == null or ids == null)) return false;
    var offset: usize = 0;
    while (offset < count) : (offset += USER_FRESH_BATCH_ROWS) {
        const rows = @min(count - offset, USER_FRESH_BATCH_ROWS);
        for (0..rows) |row_index| {
            const index = offset + row_index;
            const row = c.cbm_zova_publish_model_node_vector_at(model, @intCast(index));
            if (row == null or row.*.node_ordinal >= node_keys.len) return false;
            const key = node_keys[@intCast(row.*.node_ordinal)];
            if (row.*.ordinal != index or key <= 0 or !userPrivateKeyId(key, idAt(ids, row_index))) return false;
            vectors[row_index] = i8VectorInput(idAt(ids, row_index), row.*.source.*.vector, input.*.vector_dimensions);
        }
        if (!statusOk(c.zova_fresh_build_vectors(&c.zova_fresh_build_vectors_request{
            .build = build,
            .collection_name = collection,
            .vectors = vectors,
            .vectors_len = rows,
        }), db, "user_publish.fresh_node_vectors")) return false;
    }
    return true;
}

fn userFreshLoadTokenVectors(db: ?*Db, build: ?*c.zova_fresh_build, model: ?*const PublishModel, collection: Str, token_keys: []const UserTokenKey) bool {
    const input = c.cbm_zova_publish_model_input(model);
    const count = countOf(c.cbm_zova_publish_model_token_vector_count(model));
    const vectors = if (count != 0) interop.callocN(c.zova_vector_input, USER_FRESH_BATCH_ROWS) else null;
    defer c.free(vectors);
    const ids = if (count != 0) interop.callocN([32]u8, USER_FRESH_BATCH_ROWS) else null;
    defer c.free(ids);
    if (count != 0 and (vectors == null or ids == null)) return false;
    if (count != token_keys.len) return false;
    var offset: usize = 0;
    while (offset < count) : (offset += USER_FRESH_BATCH_ROWS) {
        const rows = @min(count - offset, USER_FRESH_BATCH_ROWS);
        for (0..rows) |row_index| {
            const index = offset + row_index;
            const row = c.cbm_zova_publish_model_token_vector_at(model, @intCast(index));
            const key = token_keys[index].token_key;
            if (row == null or row.*.source == null or row.*.ordinal != index or key <= 0 or
                c.strcmp(token_keys[index].token, row.*.source.*.token) != 0 or
                !userPrivateKeyId(key, idAt(ids, row_index))) return false;
            vectors[row_index] = i8VectorInput(idAt(ids, row_index), row.*.source.*.vector, input.*.vector_dimensions);
        }
        if (!statusOk(c.zova_fresh_build_vectors(&c.zova_fresh_build_vectors_request{
            .build = build,
            .collection_name = collection,
            .vectors = vectors,
            .vectors_len = rows,
        }), db, "user_publish.fresh_token_vectors")) return false;
    }
    return true;
}

/// Whether the database holds no graph yet, which a fresh build session
/// requires; NULL when the graphs cannot be listed.
fn userPublishCanUseFreshSession(db: ?*Db) ?bool {
    if (db == null) return null;
    var graphs: c.zova_graph_list = .{};
    defer c.zova_graph_list_free(&graphs);
    if (!statusOk(c.zova_graphs_list(&c.zova_graph_list_request{ .db = db, .out_list = &graphs }), db, "user_publish.fresh_graph_list")) return null;
    return graphs.len == 0;
}

const UserFreshPublishProfile = struct {
    graph: UserNativeGraphProfile = .{},
    validation_ms: f64 = 0,
    table_ms: f64 = 0,
    fts_ms: f64 = 0,
    vector_ms: f64 = 0,
    index_ms: f64 = 0,
    commit_ms: f64 = 0,
    build_ms: f64 = 0,
};

/// `unavailable`: this Zova cannot open a fresh build session here, nothing
/// was written, and the caller publishes the ordinary way.
const FreshOutcome = enum { published, failed, unavailable };

/// Loads the graph, the FTS rows, the node rows and both vector collections
/// of the model through one Zova fresh build session.
fn userPublishModelFreshSessionTx(
    db: ?*Db,
    model: ?*const PublishModel,
    workspace_key: i64,
    file_keys: []const UserFileKey,
    token_keys: []const UserTokenKey,
    node_collection: Str,
    token_collection: Str,
    out_nodes: ?*u64,
    out_edges: ?*u64,
    out_profile: *UserFreshPublishProfile,
) FreshOutcome {
    if (db == null or model == null) return .failed;
    out_profile.* = .{};
    const input = c.cbm_zova_publish_model_input(model);
    const workspace_id = c.cbm_zova_publish_model_workspace_id(model);
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (input == null or workspace_id == null or
        cbm_zova_workspace_graph_name(@ptrCast(workspace_id), &graph_name, graph_name.len) != 0) return .failed;
    const node_count = countOf(c.cbm_zova_publish_model_node_count(model));
    const edge_count = countOf(c.cbm_zova_publish_model_topology_count(model));
    var mark: c.struct_timespec = undefined;
    const published = blk: {
        const nodes = callocList(c.zova_graph_fresh_node_input, node_count);
        defer c.free(nodes);
        const edges = callocList(c.zova_graph_fresh_edge_payload_input, edge_count);
        defer c.free(edges);
        const node_keys = callocList(i64, node_count);
        defer c.free(node_keys);
        const edge_keys = callocList(i64, edge_count);
        defer c.free(edge_keys);
        if ((node_count != 0 and (nodes == null or node_keys == null)) or
            (edge_count != 0 and (edges == null or edge_keys == null))) return .failed;
        mark = monotonicNow();
        var ok = true;
        for (0..node_count) |i| {
            const row = c.cbm_zova_publish_model_node_at(model, @intCast(i));
            if (row == null or row.*.source == null or row.*.ordinal != i) {
                ok = false;
                break;
            }
            nodes[i] = .{
                .node_id = row.*.stable_id,
                .kind = row.*.source.*.label,
                .target_type = c.ZOVA_GRAPH_TARGET_NONE,
            };
        }
        if (ok) {
            for (0..edge_count) |i| {
                const row = c.cbm_zova_publish_model_topology_at(model, @intCast(i));
                if (row == null or row.*.source == null or row.*.ordinal != i) {
                    ok = false;
                    break;
                }
                edges[i] = .{
                    .from_node_ordinal = @intCast(row.*.source_ordinal),
                    .edge_type = row.*.source.*.type,
                    .to_node_ordinal = @intCast(row.*.target_ordinal),
                    .payload = row.*.payload,
                    .payload_len = row.*.payload_len,
                };
            }
        }
        out_profile.graph.materialize_ms = userPublishProfilePhase("fresh.materialize", &mark);
        ok = ok and collectionCreateI8(db, node_collection, input.*.vector_dimensions);
        ok = ok and collectionCreateI8(db, token_collection, input.*.vector_dimensions);
        ok = ok and userPublishFullNodesBeginTx(db);

        var build: ?*c.zova_fresh_build = null;
        if (ok) {
            const begin_status = c.zova_fresh_build_begin(&c.zova_fresh_build_begin_request{ .db = db, .out_build = &build });
            if (begin_status == c.ZOVA_INVALID_ARGUMENT) {
                return if (userPublishFullNodesEndTx(db, workspace_key, false)) .unavailable else .failed;
            }
            ok = statusOk(begin_status, db, "user_publish.fresh_begin");
        }
        out_profile.graph.reset_ms = userPublishProfilePhase("fresh.begin", &mark);
        const fresh_started = mark;
        if (ok) {
            user_publish_test_metrics.native_graph_fresh_calls += 1;
            user_publish_test_metrics.native_graph_prepared_calls += 1;
            ok = statusOk(c.zova_fresh_build_graph(&c.zova_fresh_build_graph_request{
                .build = build,
                .graph_name = &graph_name,
                .nodes = nodes,
                .nodes_len = node_count,
                .edges = edges,
                .edges_len = edge_count,
                .out_node_keys = node_keys,
                .out_node_keys_capacity = node_count,
                .out_edge_keys = edge_keys,
                .out_edge_keys_capacity = edge_count,
            }), db, "user_publish.fresh_graph");
        }
        const keys = many(node_keys, node_count);
        ok = ok and !testFailPhase("user_after_graph_nodes");
        ok = ok and !testFailPhase("user_after_graph_edges");
        ok = ok and userFreshLoadFts(db, build, model, keys);
        ok = ok and !testFailPhase("user_after_bulk_fts");
        ok = ok and userFreshLoadNodes(db, build, model, workspace_key, file_keys, keys);
        ok = ok and !testFailPhase("user_after_nodes_before_bulk_finalize");
        ok = ok and !testFailPhase("user_after_edges_before_bulk_finalize");
        ok = ok and userFreshLoadNodeVectors(db, build, model, node_collection, keys);
        ok = ok and !testFailPhase("user_after_node_vectors");
        ok = ok and userFreshLoadTokenVectors(db, build, model, token_collection, token_keys);
        ok = ok and !testFailPhase("user_after_token_vectors");

        var profile: c.zova_fresh_build_profile = .{};
        ok = ok and statusOk(c.zova_fresh_build_finish(&c.zova_fresh_build_finish_request{
            .build = build,
            .out_node_keys = node_keys,
            .out_node_keys_capacity = node_count,
            .out_edge_keys = edge_keys,
            .out_edge_keys_capacity = edge_count,
            .out_profile = &profile,
        }), db, "user_publish.fresh_finish");
        if (!ok and build != null) _ = c.zova_fresh_build_abort(build);
        if (build != null) c.zova_fresh_build_destroy(build);
        ok = ok and userPublishFullNodesEndTx(db, workspace_key, false);
        if (ok) {
            for (keys) |key| {
                if (key <= 0) ok = false;
            }
            for (many(edge_keys, edge_count)) |key| {
                if (key <= 0) ok = false;
            }
        }
        const fresh_completed = monotonicNow();
        out_profile.build_ms = userPublishElapsedMs(&fresh_started, &fresh_completed);
        out_profile.validation_ms = profile.validation_ms;
        out_profile.graph.nodes_ms = profile.graph_node_load_ms;
        out_profile.graph.edges_ms = profile.graph_edge_load_ms;
        out_profile.graph.validate_ms = profile.graph_validation_ms;
        out_profile.graph.key_generation_ms = profile.graph_key_generation_ms;
        out_profile.table_ms = profile.table_load_ms;
        out_profile.fts_ms = profile.fts_load_ms;
        out_profile.vector_ms = profile.vector_load_ms;
        out_profile.index_ms = profile.index_build_ms;
        out_profile.commit_ms = profile.commit_ms;
        if (ok) {
            if (out_nodes) |out| out.* = node_count;
            if (out_edges) |out| out.* = edge_count;
            user_publish_test_metrics.full_fts_trigger_rows_avoided += node_count;
        }
        mark = monotonicNow();
        break :blk ok;
    };
    out_profile.graph.cleanup_ms = userPublishProfilePhase("fresh.cleanup", &mark);
    return if (published) .published else .failed;
}

// ── Publication: full ───────────────────────────────────────────

fn isEmpty(text: CStr) bool {
    return text == null or text[0] == 0;
}

/// Replaces everything the workspace of `model` holds and makes the result
/// the next ready generation (`requested_generation` when it is positive).
/// Runs inside the caller's transaction.
fn userDatabasePublishModelTx(db: ?*Db, model: ?*const PublishModel, requested_generation: i64, maybe_out_result: ?*GenerationResult) bool {
    const input = c.cbm_zova_publish_model_input(model);
    const out_result = maybe_out_result orelse return false;
    if (db == null or input == null or isEmpty(input.*.root_path) or isEmpty(input.*.project) or
        isEmpty(input.*.indexed_at) or isEmpty(input.*.model_fingerprint) or input.*.vector_dimensions <= 0 or
        input.*.node_count < 0 or input.*.edge_count < 0 or input.*.node_vector_count < 0 or
        input.*.token_vector_count < 0 or input.*.file_hash_count < 0 or
        (input.*.node_count > 0 and input.*.nodes == null) or (input.*.edge_count > 0 and input.*.edges == null) or
        (input.*.node_vector_count > 0 and input.*.node_vectors == null) or
        (input.*.token_vector_count > 0 and input.*.token_vectors == null) or
        (input.*.file_hash_count > 0 and input.*.file_hashes == null))
    {
        return false;
    }
    var profile_mark = monotonicNow();
    const model_result = c.cbm_zova_publish_model_digests(model);
    const model_workspace_id = c.cbm_zova_publish_model_workspace_id(model);
    if (model_result == null or model_workspace_id == null) return false;
    out_result.* = model_result.*;
    var model_metrics: c.cbm_zova_publish_model_metrics_t = .{};
    c.cbm_zova_publish_model_metrics(model, &model_metrics);
    out_result.publication_mode = c.CBM_ZOVA_PUBLICATION_MODE_FULL;
    out_result.inserted_count = out_result.metadata_nodes + out_result.metadata_edges +
        out_result.node_vectors + out_result.token_vectors + countOf(input.*.file_hash_count);
    resultSetModelMetrics(out_result, &model_metrics);
    out_result.full_clear_count = 1;
    out_result.nodes_inserted = out_result.metadata_nodes;
    out_result.edges_inserted = out_result.metadata_edges;
    out_result.node_vectors_upserted = out_result.node_vectors;
    out_result.token_vectors_upserted = out_result.token_vectors;
    if (!registerSqlFunctions(db)) return false;
    if (!statusOk(execSql(db, "PRAGMA cache_size=-65536"), db, "user_publish.cache_size")) return false;
    if (!workspaceNameComponentValid(@ptrCast(input.*.model_fingerprint))) return false;
    _ = userPublishProfilePhase("normalize", &profile_mark);

    var workspace_id_buf: [ID_MAX]u8 = @splat(0);
    if (!userWorkspaceIdTx(db, @ptrCast(input.*.root_path), &workspace_id_buf) or
        c.strcmp(&workspace_id_buf, model_workspace_id) != 0) return false;
    const workspace_id = bufZ(&workspace_id_buf);
    const workspace_key = workspaceKeyForId(db, workspace_id) orelse return false;
    const current_max = zovaQueryI64(db, "SELECT COALESCE(MAX(generation),0) FROM cbm_database_generation_v1 " ++
        "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry " ++
        "WHERE workspace_id=?1)", workspace_id, "user_publish.current_generation") orelse return false;
    if (current_max < 0 or
        (requested_generation > 0 and requested_generation <= current_max) or
        (requested_generation == 0 and current_max == std.math.maxInt(i64))) return false;
    const generation = if (requested_generation > 0) requested_generation else current_max + 1;
    if (generation <= 0 or !userPublishInsertGenerationTx(db, workspace_id, generation)) return false;

    user_publish_test_metrics.full_clear_count += 1;
    var ok = userDatabaseClearWorkspace(db, workspace_id);
    var file_keys: List(UserFileKey) = .{};
    defer c.free(file_keys.items);
    var node_keys: [*c]UserNodeKey = null;
    defer c.free(node_keys);
    var token_keys: List(UserTokenKey) = .{};
    defer c.free(token_keys.items);
    var topology_keys: [*c]UserTopologyKey = null;
    defer c.free(topology_keys);
    out_result.clear_ms = userPublishProfilePhase("clear", &profile_mark);
    ok = ok and userPublishProjectTx(db, workspace_id, input.*.root_path, input.*.project, input.*.indexed_at);
    ok = ok and userPublishFileKeysTx(db, model, workspace_key, &file_keys);
    out_result.canonical_files_ms = userPublishProfilePhase("canonical_files", &profile_mark);

    var node_collection_buf: [COLLECTION_NAME_CAP]u8 = @splat(0);
    var token_collection_buf: [COLLECTION_NAME_CAP]u8 = @splat(0);
    ok = ok and cbm_zova_workspace_node_vector_collection_name(workspace_id, @ptrCast(input.*.model_fingerprint), input.*.vector_dimensions, &node_collection_buf, node_collection_buf.len) == 0 and
        cbm_zova_workspace_token_vector_collection_name(workspace_id, @ptrCast(input.*.model_fingerprint), input.*.vector_dimensions, &token_collection_buf, token_collection_buf.len) == 0;
    const node_collection = bufZ(&node_collection_buf);
    const token_collection = bufZ(&token_collection_buf);

    var graph_nodes: u64 = 0;
    var graph_edges: u64 = 0;
    var use_fresh_session = false;
    if (ok) {
        if (userPublishCanUseFreshSession(db)) |can_use| use_fresh_session = can_use else ok = false;
    }
    var hashes_published = false;
    var token_metadata_published = false;
    if (ok and use_fresh_session) {
        ok = userPublishModelHashesTx(db, model, file_keys.slice());
        ok = ok and userDatabaseWriteSummary(db, workspace_id, &input.*.project_summary);
        hashes_published = ok;
        out_result.canonical_hashes_ms = userPublishProfilePhase("canonical_hashes", &profile_mark);
        ok = ok and userPublishModelTokenMetadataTx(db, model, workspace_key, &token_keys);
        token_metadata_published = ok;
        out_result.token_metadata_ms = userPublishProfilePhase("token_metadata", &profile_mark);
        ok = ok and !testFailPhase("user_after_metadata");

        var fresh_profile: UserFreshPublishProfile = .{};
        if (ok) {
            switch (userPublishModelFreshSessionTx(db, model, workspace_key, file_keys.slice(), token_keys.slice(), node_collection, token_collection, &graph_nodes, &graph_edges, &fresh_profile)) {
                .published => {},
                .failed => ok = false,
                .unavailable => {
                    use_fresh_session = false;
                    profile_mark = monotonicNow();
                },
            }
        }
        if (use_fresh_session) {
            const graph = fresh_profile.graph;
            out_result.native_graph_materialize_ms = graph.materialize_ms;
            out_result.native_graph_reset_ms = graph.reset_ms;
            out_result.native_graph_nodes_ms = graph.nodes_ms;
            out_result.native_graph_edges_ms = graph.edges_ms;
            out_result.native_graph_validate_ms = graph.validate_ms;
            out_result.native_graph_key_generation_ms = graph.key_generation_ms;
            out_result.native_graph_cleanup_ms = graph.cleanup_ms;
            out_result.native_graph_ms = graph.materialize_ms + graph.reset_ms + graph.nodes_ms +
                graph.edges_ms + graph.validate_ms + graph.key_generation_ms + graph.cleanup_ms;
            out_result.canonical_nodes_ms = fresh_profile.table_ms;
            out_result.fts_ms = fresh_profile.fts_ms;
            out_result.canonical_edges_ms = 0.0;
            out_result.native_vectors_ms = fresh_profile.vector_ms;
            out_result.fresh_validation_ms = fresh_profile.validation_ms;
            out_result.fresh_index_ms = fresh_profile.index_ms;
            out_result.fresh_commit_ms = fresh_profile.commit_ms;
            out_result.fresh_build_ms = fresh_profile.build_ms;
            profile_mark = monotonicNow();
            ok = ok and !testFailPhase("user_after_fts");
        }
    }
    if (ok and !use_fresh_session) {
        var graph_profile: UserNativeGraphProfile = .{};
        ok = userPublishModelGraphTx(db, model, &node_keys, &topology_keys, &graph_nodes, &graph_edges, &graph_profile);
        out_result.native_graph_materialize_ms = graph_profile.materialize_ms;
        out_result.native_graph_reset_ms = graph_profile.reset_ms;
        out_result.native_graph_nodes_ms = graph_profile.nodes_ms;
        out_result.native_graph_edges_ms = graph_profile.edges_ms;
        out_result.native_graph_validate_ms = graph_profile.validate_ms;
        out_result.native_graph_key_generation_ms = graph_profile.key_generation_ms;
        out_result.native_graph_cleanup_ms = graph_profile.cleanup_ms;
        out_result.native_graph_ms = userPublishProfilePhase("native_graph", &profile_mark);

        const model_node_keys = many(node_keys, c.cbm_zova_publish_model_node_count(model));
        var nodes_bulk_active = false;
        if (ok) {
            ok = userPublishFullNodesBeginTx(db);
            nodes_bulk_active = ok;
        }
        ok = ok and userPublishModelNodesTx(db, model, workspace_key, file_keys.slice(), model_node_keys);
        out_result.canonical_nodes_ms = userPublishProfilePhase("canonical_nodes", &profile_mark);
        const populate_fts = ok and !testFailPhase("user_after_nodes_before_bulk_finalize");
        ok = ok and populate_fts;
        if (nodes_bulk_active) {
            const ended = userPublishFullNodesEndTx(db, workspace_key, populate_fts);
            ok = ok and ended;
            if (populate_fts and ended) {
                user_publish_test_metrics.full_fts_trigger_rows_avoided += countOf(input.*.node_count);
            }
        }
        out_result.fts_ms = userPublishProfilePhase("fts", &profile_mark);
        ok = ok and !testFailPhase("user_after_bulk_fts");
        ok = ok and !testFailPhase("user_after_edges_before_bulk_finalize");
        out_result.canonical_edges_ms = userPublishProfilePhase("canonical_edges", &profile_mark);
        if (!hashes_published) {
            ok = ok and userPublishModelHashesTx(db, model, file_keys.slice());
            ok = ok and userDatabaseWriteSummary(db, workspace_id, &input.*.project_summary);
            out_result.canonical_hashes_ms = userPublishProfilePhase("canonical_hashes", &profile_mark);
        }
        if (!token_metadata_published) {
            ok = ok and userPublishModelTokenMetadataTx(db, model, workspace_key, &token_keys);
            out_result.token_metadata_ms = userPublishProfilePhase("token_metadata", &profile_mark);
            ok = ok and !testFailPhase("user_after_metadata");
        }
        ok = ok and !testFailPhase("user_after_fts");
        ok = ok and userPublishModelVectorsTx(db, model, node_collection, token_collection, model_node_keys, token_keys.slice());
        out_result.native_vectors_ms = userPublishProfilePhase("native_vectors", &profile_mark);
    }

    out_result.generation = generation;
    ok = ok and graph_nodes == out_result.graph_nodes and graph_edges == out_result.graph_edges;
    _ = userPublishProfilePhase("digests", &profile_mark);
    ok = ok and userPublishNativeReadbackCountsTx(db, out_result, node_collection, token_collection);
    out_result.readback_ms = userPublishProfilePhase("readback", &profile_mark);
    if (ok) {
        user_publish_test_metrics.integrity_writes += 1;
        ok = userPublishIntegrityTx(db, workspace_id, generation, out_result);
    }
    ok = ok and !testFailPhase("user_after_integrity");
    ok = ok and userPublishWriteIndexStateTx(db, workspace_id, generation, input.*.model_fingerprint, input.*.vector_dimensions, input.*.indexed_at);
    ok = ok and userPublishClearHealthTx(db, workspace_id);
    ok = ok and userPublishGenerationStateTx(db, workspace_id, generation, true);
    ok = ok and !testFailPhase("user_before_commit");
    out_result.finalize_ms = userPublishProfilePhase("finalize", &profile_mark);
    _ = interop.bprint(&out_result.workspace_id, "{s}", .{span(workspace_id)});
    return ok;
}

/// The publish model of `input`, which the caller frees; NULL when the
/// workspace id or the model cannot be built.
fn publishModelBuild(maybe_input: ?*const GenerationInput) ?*PublishModel {
    const input = maybe_input orelse return null;
    var workspace_id: [ID_MAX]u8 = undefined;
    var model: ?*PublishModel = null;
    if (cbm_zova_workspace_id_for_root(@ptrCast(input.root_path), &workspace_id, workspace_id.len) != 0 or
        c.cbm_zova_publish_model_build(&workspace_id, input, &model) != 0) return null;
    return model;
}

fn userDatabasePublishWorkspaceTx(db: ?*Db, input: ?*const GenerationInput, out_result: ?*GenerationResult) bool {
    const model = publishModelBuild(input) orelse return false;
    defer c.cbm_zova_publish_model_free(model);
    return userDatabasePublishModelTx(db, model, 0, out_result);
}

/// st_size of `path` through stat(); NULL when stat fails.
fn fileBytes(path: Str) ?u64 {
    var value: c.struct_stat = undefined;
    return if (c.stat(path, &value) == 0) @bitCast(@as(i64, value.st_size)) else null;
}

/// What one publication transaction writes: the whole workspace, or a delta
/// on top of its active generation.
const Publication = union(enum) {
    full: struct { model: *const PublishModel, requested_generation: i64 },
    delta: struct { after: *const PublishModel, delta: *const WorkspaceDelta },
};

/// Takes the writer guard, opens (or creates) the database at `zova_path`,
/// runs `publication` in one transaction and records the timing and the file
/// sizes of every stage in `out_result`.
fn userDatabasePublish(zova_path: Str, publication: Publication, comptime wal_phase: Str, out_result: *GenerationResult) bool {
    out_result.* = std.mem.zeroes(GenerationResult);
    var phase_started = monotonicNow();
    var writer_guard: c.cbm_zova_writer_guard_t = .{};
    if (c.cbm_zova_writer_guard_acquire(zova_path, &writer_guard) != 0) return false;
    defer c.cbm_zova_writer_guard_release(&writer_guard);
    var phase_finished = monotonicNow();
    const writer_guard_ms = userPublishElapsedMs(&phase_started, &phase_finished);
    phase_started = phase_finished;
    var zdb: ZovaDb = .{};
    if (!openOrCreateZova(zova_path, &zdb)) return false;
    user_publish_test_metrics.database_open_count += 1;
    phase_finished = monotonicNow();
    const database_open_ms = userPublishElapsedMs(&phase_started, &phase_finished);
    phase_started = phase_finished;
    if (!userDatabaseEnsureSchema(zdb.db)) {
        closeZova(&zdb);
        user_publish_test_metrics.database_close_count += 1;
        return false;
    }
    phase_finished = monotonicNow();
    const database_init_ms = userPublishElapsedMs(&phase_started, &phase_finished);
    const started = monotonicNow();
    var wal_path_buf: [ZV_PATH_MAX]u8 = undefined;
    const wal_path: ?Str = if (interop.bprint(&wal_path_buf, "{s}-wal", .{span(zova_path)}) < wal_path_buf.len) bufZ(&wal_path_buf) else null;
    const database_bytes_before = fileBytes(zova_path) orelse 0;
    const wal_bytes_before = if (wal_path) |path| fileBytes(path) orelse 0 else 0;
    phase_started = monotonicNow();
    var ok = statusOk(execSql(zdb.db, "PRAGMA journal_mode=WAL;" ++
        "PRAGMA synchronous=NORMAL;" ++
        "PRAGMA cache_size=-65536;"), zdb.db, wal_phase);
    if (ok) {
        user_publish_test_metrics.transaction_count += 1;
        ok = workspaceRegistryBegin(zdb.db);
    }
    phase_finished = monotonicNow();
    const transaction_begin_ms = userPublishElapsedMs(&phase_started, &phase_finished);
    phase_started = phase_finished;
    if (ok) {
        switch (publication) {
            .full => |full| ok = userDatabasePublishModelTx(zdb.db, full.model, full.requested_generation, out_result),
            .delta => |incremental| {
                user_publish_delta_active = true;
                ok = userDatabasePublishDeltaTx(zdb.db, incremental.after, incremental.delta, out_result);
                user_publish_delta_active = false;
            },
        }
    }
    phase_finished = monotonicNow();
    const transaction_body_ms = userPublishElapsedMs(&phase_started, &phase_finished);
    phase_started = phase_finished;
    if (ok) {
        ok = workspaceRegistryCommit(zdb.db);
    } else {
        workspaceRegistryRollback(zdb.db);
    }
    phase_finished = monotonicNow();
    const transaction_commit_ms = userPublishElapsedMs(&phase_started, &phase_finished);
    phase_started = phase_finished;
    closeZova(&zdb);
    user_publish_test_metrics.database_close_count += 1;
    phase_finished = monotonicNow();
    const database_close_ms = userPublishElapsedMs(&phase_started, &phase_finished);
    const finished = monotonicNow();
    out_result.publish_ms = userPublishElapsedMs(&started, &finished);
    out_result.writer_guard_ms = writer_guard_ms;
    out_result.database_init_ms = database_init_ms;
    out_result.database_open_ms = database_open_ms;
    out_result.transaction_begin_ms = transaction_begin_ms;
    out_result.transaction_body_ms = transaction_body_ms;
    out_result.transaction_commit_ms = transaction_commit_ms;
    out_result.database_close_ms = database_close_ms;
    out_result.database_bytes = fileBytes(zova_path) orelse database_bytes_before;
    out_result.wal_bytes_before = wal_bytes_before;
    if (wal_path) |path| {
        if (fileBytes(path)) |bytes| out_result.wal_bytes_after = bytes;
    }
    return ok;
}

fn userDatabasePublishModelAtGeneration(zova_path: ?Str, maybe_model: ?*const PublishModel, requested_generation: i64, maybe_out_result: ?*GenerationResult) c_int {
    const path = nonEmpty(zova_path) orelse return -1;
    const model = maybe_model orelse return -1;
    const out_result = maybe_out_result orelse return -1;
    if (requested_generation < 0) return -1;
    const publication: Publication = .{ .full = .{ .model = model, .requested_generation = requested_generation } };
    return if (userDatabasePublish(path, publication, "user_publish.wal", out_result)) 0 else -1;
}

fn userDatabasePublishWorkspaceAtGeneration(zova_path: ?Str, input: ?*const GenerationInput, requested_generation: i64, out_result: ?*GenerationResult) c_int {
    const model = publishModelBuild(input) orelse return -1;
    defer c.cbm_zova_publish_model_free(model);
    return userDatabasePublishModelAtGeneration(zova_path, model, requested_generation, out_result);
}

/// The no-Zova result of every publish entry point: a zeroed result and -1.
fn publishUnavailable(out_result: ?*GenerationResult) c_int {
    if (out_result) |result| result.* = std.mem.zeroes(GenerationResult);
    return -1;
}

pub export fn cbm_zova_user_database_publish_model(zova_path: ?Str, model: ?*const PublishModel, out_result: ?*GenerationResult) c_int {
    if (!with_zova) return publishUnavailable(out_result);
    return userDatabasePublishModelAtGeneration(zova_path, model, 0, out_result);
}

pub export fn cbm_zova_user_database_publish_prepared_view(zova_path: ?Str, view: ?*const PreparedView, out_result: ?*GenerationResult) c_int {
    if (!with_zova) return publishUnavailable(out_result);
    return userDatabasePublishModelAtGeneration(zova_path, view, 0, out_result);
}

pub export fn cbm_zova_user_database_publish_delta(
    zova_path: ?Str,
    maybe_after: ?*const PublishModel,
    maybe_delta: ?*const WorkspaceDelta,
    maybe_out_result: ?*GenerationResult,
) c_int {
    if (!with_zova) return publishUnavailable(maybe_out_result);
    const path = nonEmpty(zova_path) orelse return -1;
    const after = maybe_after orelse return -1;
    const delta = maybe_delta orelse return -1;
    const out_result = maybe_out_result orelse return -1;
    const publication: Publication = .{ .delta = .{ .after = after, .delta = delta } };
    return if (userDatabasePublish(path, publication, "user_delta.wal", out_result)) 0 else -1;
}

pub export fn cbm_zova_user_database_publish_workspace(zova_path: ?Str, input: ?*const GenerationInput, out_result: ?*GenerationResult) c_int {
    if (!with_zova) return publishUnavailable(out_result);
    return userDatabasePublishWorkspaceAtGeneration(zova_path, input, 0, out_result);
}

pub export fn cbm_zova_user_database_publish_workspace_at_generation(
    zova_path: ?Str,
    input: ?*const GenerationInput,
    requested_generation: i64,
    out_result: ?*GenerationResult,
) c_int {
    if (!with_zova) return publishUnavailable(out_result);
    if (requested_generation <= 0) return -1;
    return userDatabasePublishWorkspaceAtGeneration(zova_path, input, requested_generation, out_result);
}

// ── Workspace deletion ──────────────────────────────────────────

/// Reads the embedding model and the vector size the workspace was indexed
/// with; `model` stays empty when it has no index state row. A statement
/// that cannot be prepared or bound counts as no row.
fn userDeleteReadIndexState(db: ?*Db, workspace_key: i64, model: *[256]u8, dimensions: *c_int) bool {
    const state = prepareZova(db, "SELECT model_fingerprint,vector_dimensions FROM cbm_workspace_index_state_v1 " ++
        "WHERE workspace_key=?1", "user_delete.state_prepare") orelse return true;
    defer finalize(state);
    if (!workspaceBindI64(db, state, 1, workspace_key, "user_delete.state_bind")) return true;
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (stepStatement(state, &step) != c.ZOVA_OK) return false;
    if (step != c.ZOVA_STEP_ROW) return true;
    var text: c.zova_text = .{};
    defer c.zova_text_free(&text);
    if (c.zova_statement_column_text(&c.zova_statement_column_text_request{
        .statement = state,
        .index = 0,
        .out_text = &text,
    }) != c.ZOVA_OK or text.data == null or text.len >= model.len) return false;
    @memcpy(model[0..text.len], text.data[0..text.len]);
    model[text.len] = 0;
    var value: i64 = 0;
    if (columnI64(state, 1, &value) != c.ZOVA_OK) return false;
    dimensions.* = @truncate(value);
    return true;
}

/// Removes the graph, the vector collections and every row of the
/// workspace. Does nothing for a workspace the registry does not hold.
fn userDeleteWorkspaceTx(db: ?*Db, workspace_id: Str) bool {
    const present = zovaQueryI64(db, "SELECT count(*) FROM cbm_workspace_registry WHERE workspace_id=?1", workspace_id, "user_delete.exists") orelse return false;
    if (present == 0) return true;
    const workspace_key = workspaceKeyForId(db, workspace_id) orelse return false;

    var model: [256]u8 = @splat(0);
    var dimensions: c_int = 0;
    if (!userDeleteReadIndexState(db, workspace_key, &model, &dimensions)) return false;

    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_graph_name(workspace_id, &graph_name, graph_name.len) != 0) return false;
    if (!graphDeleteNamedIfExists(db, bufZ(&graph_name))) return false;
    if (model[0] != 0 and dimensions > 0) {
        var nodes: [COLLECTION_NAME_CAP]u8 = undefined;
        var tokens: [COLLECTION_NAME_CAP]u8 = undefined;
        if (cbm_zova_workspace_node_vector_collection_name(workspace_id, bufZ(&model), dimensions, &nodes, nodes.len) != 0 or
            cbm_zova_workspace_token_vector_collection_name(workspace_id, bufZ(&model), dimensions, &tokens, tokens.len) != 0 or
            !collectionDeleteIfExists(db, bufZ(&nodes)) or
            !collectionDeleteIfExists(db, bufZ(&tokens))) return false;
    }
    if (!userDatabaseClearWorkspace(db, workspace_id)) return false;
    inline for (.{
        "cbm_workspace_health_v1",     "cbm_workspace_index_state_v1",
        "cbm_generation_integrity_v2", "cbm_database_generation_v1",
    }) |table| {
        const stmt = prepareZova(db, "DELETE FROM " ++ table ++ " WHERE workspace_key=?1", "user_delete.prepare") orelse return false;
        defer finalize(stmt);
        if (!workspaceBindI64(db, stmt, 1, workspace_key, "user_delete.bind") or
            !workspaceStepDone(db, stmt, "user_delete.step")) return false;
    }
    const stmt = prepareZova(db, "DELETE FROM cbm_workspace_registry WHERE workspace_id=?1", "user_delete.registry_prepare") orelse return false;
    defer finalize(stmt);
    return workspaceBindText(db, stmt, 1, workspace_id, "user_delete.registry_bind") and
        workspaceStepDone(db, stmt, "user_delete.registry_step");
}

pub export fn cbm_zova_user_database_delete_workspace(zova_path: ?Str, workspace_id: ?Str) c_int {
    if (!with_zova) return -1;
    const path = nonEmpty(zova_path) orelse return -1;
    if (cbm_zova_workspace_id_validate(workspace_id) != 0) return -1;
    var guard: c.cbm_zova_writer_guard_t = .{};
    if (c.cbm_zova_writer_guard_acquire(path, &guard) != 0) return -1;
    defer c.cbm_zova_writer_guard_release(&guard);
    var zdb: ZovaDb = .{};
    defer closeZova(&zdb);
    if (!openZova(path, false, &zdb) or !workspaceRegistryBegin(zdb.db)) return -1;
    if (!userDeleteWorkspaceTx(zdb.db, workspace_id.?)) {
        workspaceRegistryRollback(zdb.db);
        return -1;
    }
    return if (workspaceRegistryCommit(zdb.db)) 0 else -1;
}

pub export fn cbm_zova_delete_workspace_graph(zova_path: ?Str, workspace_id: ?Str) c_int {
    if (!with_zova) return -1;
    const path = zova_path orelse return -1;
    if (!workspaceNameComponentValid(workspace_id)) return -1;
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_graph_name(workspace_id, &graph_name, graph_name.len) != 0) return -1;
    var zdb: ZovaDb = .{};
    defer closeZova(&zdb);
    if (!openZova(path, false, &zdb) or !workspaceRegistryBegin(zdb.db)) return -1;
    if (!graphDeleteNamedIfExists(zdb.db, bufZ(&graph_name))) {
        workspaceRegistryRollback(zdb.db);
        return -1;
    }
    return if (workspaceRegistryCommit(zdb.db)) 0 else -1;
}

// ── Vector session ──────────────────────────────────────────────

/// A malloc'd copy of text column `index`; NULL when it cannot be read.
fn columnTextOwned(db: ?*Db, stmt: ?*Stmt, index: c_int) [*c]u8 {
    var text: c.zova_text = .{};
    defer c.zova_text_free(&text);
    if (!statusOk(c.zova_statement_column_text(&c.zova_statement_column_text_request{
        .statement = stmt,
        .index = index,
        .out_text = &text,
    }), db, "column.text")) return null;
    return if (text.data != null) zvStrndup(text.data, text.len) else zvStrndup("", 0);
}

/// Opens `zova_path` read-only as a vector or a graph session and reads the
/// sidecar generation, when the database has one.
fn sessionOpen(comptime Session: type, zova_path: ?Str) ?*Session {
    const path = zova_path orelse return null;
    const session = interop.callocN(Session, 1);
    if (session == null or !openZova(path, true, &session.*.zdb)) {
        c.free(session);
        return null;
    }
    if (sidecarGenerationReadDb(session.*.zdb.db)) |generation| {
        session.*.generation = generation;
        session.*.generation_valid = true;
    }
    return session;
}

pub export fn cbm_zova_vector_session_open(zova_path: ?Str) ?*VectorSession {
    if (!with_zova) return null;
    return sessionOpen(VectorSession, zova_path);
}

pub export fn cbm_zova_vector_session_search_collection_i8(
    session: ?*VectorSession,
    collection: ?Str,
    query_values: [*c]const i8,
    query_count: c_int,
    vector_dim: c_int,
    prefilter_limit: c_int,
    limit: c_int,
    out: ?*[*c]VectorHit,
    out_count: ?*c_int,
) c_int {
    if (!with_zova) {
        if (out) |items| items.* = null;
        if (out_count) |count| count.* = 0;
        return -1;
    }
    const vector_session = session orelse return -1;
    const collection_name = nonEmpty(collection) orelse return -1;
    const out_items = out orelse return -1;
    const out_items_count = out_count orelse return -1;
    if (query_values == null or query_count <= 0 or vector_dim <= 0 or prefilter_limit <= 0 or limit <= 0) return -1;
    out_items.* = null;
    out_items_count.* = 0;
    const db = vector_session.zdb.db;
    var results: c.zova_vector_search_results = .{};
    defer c.zova_vector_search_results_free(&results);
    const searched = if (query_count == 1)
        statusOk(c.zova_vector_search(&c.zova_vector_search_request{
            .db = db,
            .collection_name = collection_name,
            .query = .{
                .element_type = c.ZOVA_VECTOR_ELEMENT_TYPE_I8,
                .i8_values = query_values,
                .values_len = sizeOfInt(vector_dim),
            },
            .limit = sizeOfInt(limit),
            .out_results = &results,
        }), db, "workspace_vector.search_i8")
    else
        statusOk(c.zova_vector_search_multi_i8(&c.zova_vector_search_multi_i8_request{
            .db = db,
            .collection_name = collection_name,
            .query_values = query_values,
            .query_values_len = sizeOfInt(query_count) * sizeOfInt(vector_dim),
            .query_count = sizeOfInt(query_count),
            .dimensions = sizeOfInt(vector_dim),
            .candidate_ids = null,
            .candidate_count = 0,
            .mode = c.ZOVA_VECTOR_MULTI_I8_SEARCH_CBM_PREFILTER_MIN_COSINE,
            .aggregation = c.ZOVA_VECTOR_MULTI_I8_AGGREGATION_MIN_COSINE,
            .prefilter_query_index = 0,
            .prefilter_limit = sizeOfInt(prefilter_limit),
            .limit = sizeOfInt(limit),
            .out_results = &results,
        }), db, "workspace_vector.search_multi_i8");
    if (!searched) return -1;
    const items = callocList(VectorHit, results.len);
    if (results.len != 0 and items == null) return -1;
    for (many(results.items, results.len), 0..) |*result, i| {
        items[i].vector_id = zvStrndup(result.id, result.id_len);
        if (items[i].vector_id == null) {
            cbm_zova_vector_hits_free(items, @intCast(i));
            return -1;
        }
        items[i].score = 1.0 - result.distance;
    }
    out_items.* = items;
    out_items_count.* = @intCast(results.len);
    return 0;
}

pub export fn cbm_zova_vector_session_get_workspace_token_i8(
    session: ?*VectorSession,
    workspace_id: ?Str,
    model_fingerprint: ?Str,
    vector_dim: c_int,
    token: ?Str,
    out_values: [*c]i8,
    out_len: usize,
    out_found: ?*bool,
) c_int {
    if (!with_zova) {
        if (out_found) |found| found.* = false;
        return -1;
    }
    const vector_session = session orelse return -1;
    const token_text = nonEmpty(token) orelse return -1;
    const found = out_found orelse return -1;
    if (!workspaceNameComponentValid(workspace_id) or !workspaceNameComponentValid(model_fingerprint) or
        vector_dim <= 0 or out_values == null or out_len != sizeOfInt(vector_dim)) return -1;
    found.* = false;
    const db = vector_session.zdb.db;
    var collection: [COLLECTION_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_token_vector_collection_name(workspace_id, model_fingerprint, vector_dim, &collection, collection.len) != 0) return -1;
    var vector_id: [32]u8 = undefined;
    {
        const statement = prepareZova(db, "SELECT m.token_key FROM cbm_token_vector_metadata_v1 m " ++
            "JOIN cbm_workspace_registry w ON w.workspace_key=m.workspace_key " ++
            "WHERE w.workspace_id=?1 AND m.token=?2", "token_vector.key_prepare") orelse return -1;
        defer finalize(statement);
        if (!workspaceBindText(db, statement, 1, workspace_id.?, "token_vector.key_workspace") or
            !workspaceBindText(db, statement, 2, token_text, "token_vector.key_token")) return -1;
        var step: c.zova_step_result = c.ZOVA_STEP_DONE;
        if (!statusOk(stepStatement(statement, &step), db, "token_vector.key_step")) return -1;
        if (step == c.ZOVA_STEP_DONE) return 0;
        var token_key: i64 = 0;
        if (step != c.ZOVA_STEP_ROW or
            !statusOk(columnI64(statement, 0, &token_key), db, "token_vector.key_value") or
            !userPrivateKeyId(token_key, &vector_id)) return -1;
    }
    var vector: c.zova_vector = .{};
    const status = c.zova_vector_get(&c.zova_vector_get_request{
        .db = db,
        .collection_name = &collection,
        .vector_id = &vector_id,
        .out_vector = &vector,
    });
    if (status == c.ZOVA_VECTOR_NOT_FOUND) return 0;
    if (status != c.ZOVA_OK) return -1;
    defer c.zova_vector_free(&vector);
    if (vector.element_type != c.ZOVA_VECTOR_ELEMENT_TYPE_I8 or vector.values_len != out_len or
        vector.i8_values == null) return -1;
    @memcpy(out_values[0..out_len], vector.i8_values[0..out_len]);
    found.* = true;
    return 0;
}

pub export fn cbm_zova_vector_session_close(session: ?*VectorSession) void {
    if (!with_zova) return;
    const vector_session = session orelse return;
    closeZova(&vector_session.zdb);
    c.free(vector_session);
}

pub export fn cbm_zova_vector_session_generation(session: ?*const VectorSession, out_generation: ?*i64) c_int {
    if (!with_zova) {
        if (out_generation) |out| out.* = 0;
        return -1;
    }
    const vector_session = session orelse return -1;
    const out = out_generation orelse return -1;
    if (!vector_session.generation_valid) return -1;
    out.* = vector_session.generation;
    return 0;
}

pub export fn cbm_zova_vector_session_has_workspace(session: ?*const VectorSession, workspace_id: ?Str, out_present: ?*bool) c_int {
    if (!with_zova) {
        if (out_present) |present| present.* = false;
        return -1;
    }
    const vector_session = session orelse return -1;
    const present = out_present orelse return -1;
    if (!workspaceNameComponentValid(workspace_id)) return -1;
    present.* = false;
    const db = vector_session.zdb.db;
    const sql: Str = if (workspaceMetadataTableExists(db, "cbm_nodes_v1"))
        "SELECT 1 FROM cbm_nodes_v1 n JOIN cbm_workspace_registry r " ++
            "ON r.workspace_key=n.workspace_key WHERE r.workspace_id=?1 LIMIT 1"
    else if (workspaceMetadataTableExists(db, "cbm_zova_generation_integrity_v1"))
        "SELECT 1 FROM cbm_zova_generation_integrity_v1 " ++
            "WHERE workspace_id=?1 LIMIT 1"
    else
        return 0;
    const stmt = prepareZova(db, sql, "vector_session.workspace_prepare") orelse return -1;
    defer finalize(stmt);
    if (!workspaceBindText(db, stmt, 1, workspace_id.?, "vector_session.workspace_bind")) return -1;
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!statusOk(stepStatement(stmt, &step), db, "vector_session.workspace_step") or
        (step != c.ZOVA_STEP_ROW and step != c.ZOVA_STEP_DONE)) return -1;
    present.* = step == c.ZOVA_STEP_ROW;
    return 0;
}

// ── Graph session ───────────────────────────────────────────────

const GraphVisitIndex = extern struct {
    node_id: CStr = null,
    index: usize = 0,
};

fn graphVisitIndexCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(GraphVisitIndex, left).node_id, cast(GraphVisitIndex, right).node_id);
}

/// The position in the visit list of `node_id`; NULL when `items`, sorted
/// by node id, does not hold it exactly once.
fn graphVisitFind(items: []const GraphVisitIndex, node_id: CStr) ?usize {
    const key: GraphVisitIndex = .{ .node_id = node_id };
    const hit = c.bsearch(&key, items.ptr, items.len, @sizeOf(GraphVisitIndex), graphVisitIndexCompare) orelse return null;
    const at = (@intFromPtr(hit) - @intFromPtr(items.ptr)) / @sizeOf(GraphVisitIndex);
    if ((at > 0 and c.strcmp(items[at - 1].node_id, node_id) == 0) or
        (at + 1 < items.len and c.strcmp(items[at + 1].node_id, node_id) == 0)) return null;
    return items[at].index;
}

fn graphRecordElapsed(start: *const c.struct_timespec, out_ms: *f64) void {
    const end = monotonicNow();
    out_ms.* += userPublishElapsedMs(start, &end);
}

/// Fills name, file, label, lines and properties of every visit from
/// cbm_nodes_v1. `index` is the visits sorted by node id.
fn graphHydrateVisits(db: ?*Db, workspace_id: Str, visits: []GraphVisit, index: []const GraphVisitIndex, metrics: ?*GraphMetrics) bool {
    const count = visits.len;
    const filled = callocList(bool, count);
    defer c.free(filled);
    const node_keys = callocList(i64, count);
    defer c.free(node_keys);
    if (filled == null or node_keys == null) return false;
    if (!workspaceMetadataTableExists(db, "cbm_nodes_v1")) return false;
    const workspace_key = workspaceKeyForId(db, workspace_id) orelse return false;
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (cbm_zova_workspace_graph_name(workspace_id, &graph_name, graph_name.len) != 0) return false;

    var cursor: c.zova_graph_scan_cursor = .{};
    var more = true;
    while (more) {
        var scan: c.zova_graph_scan_results = .{};
        defer c.zova_graph_scan_results_free(&scan);
        if (c.zova_graph_scan(&c.zova_graph_scan_request{
            .db = db,
            .graph_name = &graph_name,
            .node_after = cursor,
            .node_limit = 4096,
            .edge_limit = 0,
            .out_results = &scan,
        }) != c.ZOVA_OK) return false;
        const nodes = many(scan.nodes, scan.nodes_len);
        for (nodes) |*node| {
            if (graphVisitFind(index, node.node_id)) |visit_index| node_keys[visit_index] = node.node_key;
        }
        more = scan.has_more_nodes != 0;
        if (nodes.len != 0) {
            const last = &nodes[nodes.len - 1];
            cursor.created_order = last.created_order;
            cursor.key = last.node_key;
        } else if (more) {
            return false;
        }
    }

    var offset: usize = 0;
    while (offset < count) : (offset += ZV_GRAPH_HYDRATE_BATCH) {
        const batch_keys: []const i64 = node_keys[offset..@min(count, offset + ZV_GRAPH_HYDRATE_BATCH)];
        for (batch_keys) |key| {
            if (key <= 0) return false;
        }
        {
            var keyed: c.zova_graph_keyed_node_results = .{};
            defer c.zova_graph_keyed_node_results_free(&keyed);
            if (c.zova_graph_nodes_get_many_keyed(&c.zova_graph_nodes_get_many_keyed_request{
                .db = db,
                .graph_name = &graph_name,
                .node_keys = batch_keys.ptr,
                .key_count = batch_keys.len,
                .out_results = &keyed,
            }) != c.ZOVA_OK or keyed.len != batch_keys.len) return false;
            for (many(keyed.items, keyed.len), batch_keys, visits[offset..][0..batch_keys.len]) |*item, key, *visit| {
                if (item.found == 0 or item.node_key != key or c.strcmp(item.node_id, visit.node_id) != 0) return false;
            }
        }

        var sql: [ZV_SQL_MAX * 8]u8 = undefined;
        var length = interop.bprint(&sql, "SELECT n.zova_node_key,n.name,n.qualified_name,f.file_path,n.label,n.start_line," ++
            "n.end_line,n.properties FROM cbm_nodes_v1 n " ++
            "JOIN cbm_files_v1 f ON f.file_key=n.file_key " ++
            "WHERE n.workspace_key=?1 AND n.zova_node_key IN (", .{});
        if (length >= sql.len) return false;
        for (0..batch_keys.len) |i| {
            const written = interop.bprint(sql[length..], "{s}?{d}", .{ if (i == 0) "" else ",", i + 2 });
            if (written >= sql.len - length) return false;
            length += written;
        }
        if (interop.bprint(sql[length..], ")", .{}) >= sql.len - length) return false;

        var stage_start = monotonicNow();
        const stmt = prepareZova(db, bufZ(&sql), "graph.hydrate_prepare");
        defer finalize(stmt);
        if (metrics) |graph_metrics| graphRecordElapsed(&stage_start, &graph_metrics.hydrate_prepare_ms);
        if (stmt == null or !workspaceBindI64(db, stmt, 1, workspace_key, "graph.hydrate_bind_workspace")) return false;
        for (batch_keys, 2..) |key, parameter| {
            if (!workspaceBindI64(db, stmt, @intCast(parameter), key, "graph.hydrate_bind_node")) return false;
        }
        stage_start = monotonicNow();
        while (true) {
            var step: c.zova_step_result = c.ZOVA_STEP_DONE;
            if (!statusOk(stepStatement(stmt, &step), db, "graph.hydrate_step")) return false;
            if (step == c.ZOVA_STEP_DONE) break;
            var node_key: i64 = 0;
            if (!statusOk(columnI64(stmt, 0, &node_key), db, "graph.hydrate_node_key")) return false;
            const in_batch = for (batch_keys, 0..) |key, i| {
                if (key == node_key) break i;
            } else return false;
            const visit_index = offset + in_batch;
            if (filled[visit_index]) return false;
            const visit = &visits[visit_index];
            visit.name = columnTextOwned(db, stmt, 1);
            visit.qualified_name = columnTextOwned(db, stmt, 2);
            visit.file_path = columnTextOwned(db, stmt, 3);
            visit.label = columnTextOwned(db, stmt, 4);
            visit.properties = columnTextOwned(db, stmt, 7);
            var start_line: i64 = 0;
            var end_line: i64 = 0;
            if (visit.name == null or visit.qualified_name == null or visit.file_path == null or visit.label == null or
                visit.properties == null or
                !statusOk(columnI64(stmt, 5, &start_line), db, "graph.hydrate_start") or
                !statusOk(columnI64(stmt, 6, &end_line), db, "graph.hydrate_end")) return false;
            visit.start_line = std.math.cast(c_int, start_line) orelse return false;
            visit.end_line = std.math.cast(c_int, end_line) orelse return false;
            filled[visit_index] = true;
        }
        if (metrics) |graph_metrics| graphRecordElapsed(&stage_start, &graph_metrics.hydrate_step_ms);
    }
    for (filled[0..count]) |done| {
        if (!done) return false;
    }
    return true;
}

fn graphNativeProfileEnabled() bool {
    const value: CStr = c.getenv("CBM_ZOVA_GRAPH_PROFILE");
    return value != null and c.strcmp(value, "1") == 0;
}

fn graphMetricsCopyNativeProfile(metrics: *GraphMetrics, profile: *const c.zova_graph_walk_profile) void {
    metrics.native_profiled = true;
    metrics.native_mutex_wait_ms = profile.mutex_wait_ms;
    metrics.native_root_lookup_ms = profile.root_lookup_ms;
    metrics.native_adjacency_prepare_ms = profile.adjacency_prepare_ms;
    metrics.native_adjacency_execute_ms = profile.adjacency_execute_ms;
    metrics.native_bfs_bookkeeping_allocation_ms = profile.bfs_bookkeeping_allocation_ms;
    metrics.native_c_abi_result_export_ms = profile.c_abi_result_export_ms;
    metrics.native_total_profiled_ms = profile.total_profiled_ms;
    metrics.frontier_expansions = profile.frontier_expansions;
    metrics.adjacency_query_binds = profile.adjacency_query_binds;
    metrics.adjacency_rows_stepped = profile.adjacency_rows_stepped;
    metrics.native_result_count = profile.result_count;
}

pub export fn cbm_zova_graph_session_open(zova_path: ?Str) ?*GraphSession {
    if (!with_zova) return null;
    return sessionOpen(GraphSession, zova_path);
}

pub export fn cbm_zova_graph_session_close(session: ?*GraphSession) void {
    if (!with_zova) return;
    const graph_session = session orelse return;
    closeZova(&graph_session.zdb);
    c.free(graph_session);
}

pub export fn cbm_zova_graph_session_generation(session: ?*const GraphSession, out_generation: ?*i64) c_int {
    if (!with_zova) {
        if (out_generation) |out| out.* = 0;
        return -1;
    }
    const graph_session = session orelse return -1;
    const out = out_generation orelse return -1;
    if (!graph_session.generation_valid) return -1;
    out.* = graph_session.generation;
    return 0;
}

pub export fn cbm_zova_graph_session_walk_calls(
    session: ?*GraphSession,
    workspace_id: ?Str,
    graph_name: ?Str,
    start_node_id: ?Str,
    direction: ?Str,
    max_depth: c_int,
    max_results: c_int,
    out_visits: ?*[*c]GraphVisit,
    out_count: ?*c_int,
    out_metrics: ?*GraphMetrics,
) c_int {
    if (out_visits) |visits| visits.* = null;
    if (out_count) |count| count.* = 0;
    if (out_metrics) |metrics| {
        metrics.* = std.mem.zeroes(GraphMetrics);
        metrics.fallback = true;
    }
    if (!with_zova) return -1;
    const graph_session = session orelse return -1;
    const graph = nonEmpty(graph_name) orelse return -1;
    const start_node = nonEmpty(start_node_id) orelse return -1;
    const direction_name = direction orelse return -1;
    if (!workspaceNameComponentValid(workspace_id) or max_depth < 0 or max_results <= 0 or
        out_visits == null or out_count == null or max_results == INT_MAX) return -1;
    const zova_direction: c_int = if (c.strcmp(direction_name, "inbound") == 0)
        c.ZOVA_GRAPH_NEIGHBOR_INCOMING
    else if (c.strcmp(direction_name, "outbound") == 0)
        c.ZOVA_GRAPH_NEIGHBOR_OUTGOING
    else
        return -1;
    const db = graph_session.zdb.db;
    var results: c.zova_graph_walk_results = .{};
    defer c.zova_graph_walk_results_free(&results);
    const limit = sizeOfInt(max_results) + 1;
    var stage_start = monotonicNow();
    var ok: bool = undefined;
    if (graphNativeProfileEnabled()) {
        var profile: c.zova_graph_walk_profile = .{};
        ok = statusOk(c.zova_graph_walk_direction_profiled(&c.zova_graph_walk_direction_profiled_request{
            .db = db,
            .graph_name = graph,
            .start_node_id = start_node,
            .direction = zova_direction,
            .edge_type = "CALLS",
            .max_depth = @intCast(max_depth),
            .limit = limit,
            .out_results = &results,
            .out_profile = &profile,
        }), db, "graph.walk_calls_profiled");
        if (out_metrics) |metrics| graphMetricsCopyNativeProfile(metrics, &profile);
    } else {
        ok = statusOk(c.zova_graph_walk_direction(&c.zova_graph_walk_direction_request{
            .db = db,
            .graph_name = graph,
            .start_node_id = start_node,
            .direction = zova_direction,
            .edge_type = "CALLS",
            .max_depth = @intCast(max_depth),
            .limit = limit,
            .out_results = &results,
        }), db, "graph.walk_calls");
    }
    if (out_metrics) |metrics| graphRecordElapsed(&stage_start, &metrics.walk_ms);
    const walked = many(results.items, results.len);
    if (!ok or walked.len == 0 or c.strcmp(walked[0].node_id, start_node) != 0 or walked[0].depth != 0 or
        walked.len > limit or walked.len - 1 > INT_MAX) return -1;

    const count = walked.len - 1;
    const visits = interop.callocN(GraphVisit, count);
    const index = interop.callocN(GraphVisitIndex, count);
    defer c.free(index);
    stage_start = monotonicNow();
    ok = count == 0 or (visits != null and index != null);
    if (ok) {
        for (walked[1..], 0..) |*item, i| {
            if (item.node_id == null or item.node_id[0] == 0 or item.depth == 0 or item.depth > INT_MAX) {
                ok = false;
                break;
            }
            visits[i].node_id = zvStrdup(item.node_id);
            visits[i].hop = @intCast(item.depth);
            index[i] = .{ .node_id = visits[i].node_id, .index = i };
            if (visits[i].node_id == null) {
                ok = false;
                break;
            }
        }
    }
    if (ok and count > 1) {
        c.qsort(index, count, @sizeOf(GraphVisitIndex), graphVisitIndexCompare);
        for (1..count) |i| {
            if (c.strcmp(index[i - 1].node_id, index[i].node_id) == 0) {
                ok = false;
                break;
            }
        }
    }
    if (out_metrics) |metrics| graphRecordElapsed(&stage_start, &metrics.result_build_ms);
    if (ok and count > 0) {
        ok = graphHydrateVisits(db, workspace_id.?, visits[0..count], index[0..count], out_metrics);
    }
    if (!ok) {
        cbm_zova_graph_visits_free(visits, @intCast(count));
        return -1;
    }
    out_visits.?.* = visits;
    out_count.?.* = @intCast(count);
    if (out_metrics) |metrics| {
        metrics.walk_count = 1;
        metrics.fallback = false;
    }
    return 0;
}

// ── Exports that only a Zova build defines ──────────────────────

pub const zova_only = if (with_zova) struct {
    pub export fn cbm_zova_database_format_status(path: ?Str) FormatStatus {
        return databaseFormatStatus(path);
    }

    pub export fn cbm_zova_repack_phase_name(phase: RepackPhase) ?Str {
        if (phase < 0 or phase >= c.CBM_ZOVA_REPACK_PHASE_COUNT) return null;
        return repack_phase_names[@intCast(phase)];
    }

    pub export fn cbm_zova_register_sql_functions(db: ?*Db) c_int {
        return if (registerSqlFunctions(db)) 0 else -1;
    }

    pub export fn cbm_zova_user_database_verify_workspace_schema(db: ?*Db) c_int {
        return if (verifyWorkspaceSchema(db)) 0 else -1;
    }

    pub export fn cbm_zova_user_database_schema_is_current(db: ?*Db) c_int {
        return if (userDatabaseSchemaIsCurrent(db)) 0 else -1;
    }

    /// Caller-owned transaction primitive. It never begins, commits, or
    /// rolls back; the caller remains responsible for transaction lifetime.
    pub export fn cbm_zova_user_database_publish_workspace_tx(db: ?*Db, input: ?*const GenerationInput, out_result: ?*GenerationResult) c_int {
        return if (userDatabasePublishWorkspaceTx(db, input, out_result)) 0 else -1;
    }

    pub export fn cbm_zova_user_database_publish_model_tx(db: ?*Db, model: ?*const PublishModel, requested_generation: i64, out_result: ?*GenerationResult) c_int {
        return if (userDatabasePublishModelTx(db, model, requested_generation, out_result)) 0 else -1;
    }
} else struct {};
