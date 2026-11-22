//! cbm_zova_repository.zig — Read access to a cbm.zova file: one workspace
//! (nodes, edges, search, file coverage), the catalog of every workspace in
//! the file, and whole-workspace snapshots for incremental indexing.
//!
//! Implements src/zova/cbm_zova_repository.h. Without Zova every function
//! clears its outputs and fails.

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
const KeyedNodes = c.zova_graph_keyed_node_results;
const ScanResults = c.zova_graph_scan_results;
const ScanCursor = c.zova_graph_scan_cursor;

const Node = c.cbm_node_t;
const Edge = c.cbm_edge_t;
const SearchResult = c.cbm_search_result_t;
const SearchOutput = c.cbm_search_output_t;
const SearchParams = c.cbm_search_params_t;
const CatalogProject = c.cbm_zova_catalog_project_t;
const Scope = c.cbm_zova_catalog_scope_t;
const IdentityNode = c.cbm_zova_identity_node_t;
const IdentityEdge = c.cbm_zova_identity_edge_t;
const FileCoverage = c.cbm_zova_file_coverage_t;
const Snapshot = c.cbm_zova_workspace_snapshot_t;
const Components = c.cbm_zova_snapshot_components_t;

const OK: c_int = c.CBM_STORE_OK;
const ERR: c_int = c.CBM_STORE_ERR;
const NOT_FOUND: c_int = c.CBM_STORE_NOT_FOUND;
const SCAN_LIMIT: c_int = c.CBM_STORE_SCAN_LIMIT;

const ID_MAX: usize = c.CBM_ZOVA_WORKSPACE_ID_MAX;
const PROJECT_CAP: usize = c.CBM_SZ_4K;
const INT_MAX: c_int = std.math.maxInt(c_int);

const GRAPH_SCAN_PAGE: usize = 1024;
const EDGE_SCAN_PAGE: usize = 4096;
const GRAPH_NAME_CAP: usize = ID_MAX + 64;
const STRING_ARENA_CHUNK: usize = 64 * 1024;

const Error = error{Failed};

const NumericIdEntry = extern struct {
    numeric_id: i64,
    order: usize,
    node_key: i64,
    stable_id: [*:0]u8,
};

const Repository = extern struct {
    db: ?*Db = null,
    workspace_id: [ID_MAX]u8 = @splat(0),
    project: [PROJECT_CAP]u8 = @splat(0),
    workspace_key: i64 = 0,
    generation: i64 = 0,
    /// Graph nodes sorted by numeric id (the hash of the stable id), built on
    /// the first id lookup so per-node reads do not rescan the whole graph.
    numeric_ids: [*c]NumericIdEntry = null,
    numeric_id_count: usize = 0,
    numeric_id_strings: ?*StringArena = null,

    fn workspaceId(repo: *const Repository) Str {
        return @ptrCast(&repo.workspace_id);
    }
};

const Catalog = extern struct {
    db: ?*Db,
    projects: [*c]CatalogProject,
    count: c_int,
    transaction_open: bool,
    error_text: [512]u8,

    fn items(catalog: *const Catalog) []CatalogProject {
        return many(catalog.projects, catalog.count);
    }
};

fn Span(comptime P: type) type {
    const info = @typeInfo(P).pointer;
    return if (info.attrs.@"const") []const info.child else []info.child;
}

/// The items of a (C pointer, count) array; empty for NULL or count <= 0.
fn many(items: anytype, count: anytype) Span(@TypeOf(items)) {
    if (items == null or count <= 0) return &.{};
    return items[0..@intCast(count)];
}

/// free() for the `const` pointers of the store structs, which the caller of
/// this module owns.
fn freeConst(p: anytype) void {
    c.free(@ptrCast(@constCast(p)));
}

/// The string when it is neither NULL nor empty.
fn nonEmpty(text: ?Str) ?Str {
    const value = text orelse return null;
    return if (value[0] != 0) value else null;
}

fn cast(comptime T: type, p: ?*const anyopaque) *const T {
    return @ptrCast(@alignCast(p));
}

// ── String arena ────────────────────────────────────────────────

const StringArenaChunk = extern struct {
    next: ?*StringArenaChunk,
    used: usize,
    capacity: usize,

    /// The bytes that follow the header in the same allocation.
    fn data(chunk: *StringArenaChunk) [*]u8 {
        return @as([*]u8, @ptrCast(chunk)) + @sizeOf(StringArenaChunk);
    }
};

const StringArena = extern struct {
    head: ?*StringArenaChunk,
    chunk_count: u64,
    bytes_used: u64,
};

fn repoStringArenaFree(maybe_arena: ?*StringArena) void {
    const arena = maybe_arena orelse return;
    var chunk = arena.head;
    while (chunk) |current| {
        chunk = current.next;
        c.free(current);
    }
    c.free(arena);
}

fn repoStringArenaDup(maybe_arena: ?*StringArena, text: [*c]const u8, len: usize) ?[*:0]u8 {
    const arena = maybe_arena orelse return null;
    const needed = len + 1;
    var chunk = arena.head;
    if (chunk == null or chunk.?.capacity - chunk.?.used < needed) {
        const capacity = @max(needed, STRING_ARENA_CHUNK);
        const fresh: *StringArenaChunk = @ptrCast(@alignCast(c.malloc(@sizeOf(StringArenaChunk) + capacity) orelse return null));
        fresh.* = .{ .next = arena.head, .used = 0, .capacity = capacity };
        arena.head = fresh;
        arena.chunk_count += 1;
        chunk = fresh;
    }
    const current = chunk.?;
    const copy = current.data() + current.used;
    if (len != 0) @memcpy(copy[0..len], text[0..len]);
    copy[len] = 0;
    current.used += needed;
    arena.bytes_used += needed;
    return @ptrCast(copy);
}

/// A malloc'd NUL-terminated copy of text[0..len]; NULL on OOM.
fn repoDup(text: [*c]const u8, len: usize) [*c]u8 {
    const copy = interop.mallocN(u8, len + 1);
    if (copy == null) return null;
    if (len != 0) @memcpy(copy[0..len], text[0..len]);
    copy[len] = 0;
    return copy;
}

/// A malloc'd copy of a NUL-terminated string; NULL on OOM.
fn repoDupZ(text: CStr) [*c]u8 {
    return repoDup(text, c.strlen(text));
}

/// A count narrowed the way a C `(int)` cast narrows it.
fn narrowInt(value: u64) c_int {
    return @bitCast(@as(c_uint, @truncate(value)));
}

// ── Zova SDK calls ──────────────────────────────────────────────

/// Opens `path` read-only with the 5 s busy timeout every reader here uses.
fn repoOpenReadOnly(path: Str) ?*Db {
    var db: ?*Db = null;
    var message = std.mem.zeroes(c.zova_message);
    const status = c.zova_database_open_with_options(&c.zova_database_open_options_request{
        .path = path,
        .flags = c.ZOVA_OPEN_READ_ONLY,
        .busy_timeout_ms = 5000,
        .out_db = &db,
        .out_error_message = &message,
    });
    c.zova_message_free(&message);
    if (status == c.ZOVA_OK) return db;
    if (db) |opened| _ = c.zova_database_close(opened);
    return null;
}

fn repoExec(db: ?*Db, sql: Str) bool {
    return c.zova_database_exec(&c.zova_database_exec_request{ .db = db, .sql = sql }) == c.ZOVA_OK;
}

fn repoPrepare(db: ?*Db, sql: Str) ?*Stmt {
    var statement: ?*Stmt = null;
    const status = c.zova_database_prepare(&c.zova_database_prepare_request{
        .db = db,
        .sql = sql,
        .out_statement = &statement,
    });
    if (status == c.ZOVA_OK) return statement;
    repoFinalize(statement);
    return null;
}

fn repoFinalize(statement: ?*Stmt) void {
    if (statement) |open| _ = c.zova_statement_finalize(open);
}

fn repoBindText(statement: ?*Stmt, index: c_int, value: CStr) bool {
    return c.zova_statement_bind_text(&c.zova_statement_bind_text_request{
        .statement = statement,
        .index = index,
        .data = value,
        .len = c.strlen(value),
    }) == c.ZOVA_OK;
}

fn repoBindI64(statement: ?*Stmt, index: c_int, value: i64) bool {
    return c.zova_statement_bind_int64(&c.zova_statement_bind_int64_request{
        .statement = statement,
        .index = index,
        .value = value,
    }) == c.ZOVA_OK;
}

fn repoBindNull(statement: ?*Stmt, index: c_int) bool {
    return c.zova_statement_bind_null(&c.zova_statement_bind_null_request{
        .statement = statement,
        .index = index,
    }) == c.ZOVA_OK;
}

fn repoStep(statement: ?*Stmt, out: *c.zova_step_result) bool {
    return c.zova_statement_step(&c.zova_statement_step_request{
        .statement = statement,
        .out_result = out,
    }) == c.ZOVA_OK;
}

/// Steps once and says whether the statement produced a row; an error when
/// the step itself failed.
fn repoNextRow(statement: ?*Stmt) Error!bool {
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!repoStep(statement, &step)) return error.Failed;
    return step != c.ZOVA_STEP_DONE;
}

/// A malloc'd copy of the column, "" for SQL NULL; NULL when the read or the
/// copy failed.
fn repoColumnText(statement: ?*Stmt, index: c_int) [*c]u8 {
    var text = std.mem.zeroes(c.zova_text);
    if (c.zova_statement_column_text(&c.zova_statement_column_text_request{
        .statement = statement,
        .index = index,
        .out_text = &text,
    }) != c.ZOVA_OK) return null;
    defer c.zova_text_free(&text);
    return repoDup(text.data, if (text.data != null) text.len else 0);
}

fn repoColumnI64(statement: ?*Stmt, index: c_int) i64 {
    var value: i64 = 0;
    _ = c.zova_statement_column_int64(&c.zova_statement_column_int64_request{
        .statement = statement,
        .index = index,
        .out_value = &value,
    });
    return value;
}

/// The column narrowed the way a C `(int)` cast narrows it.
fn repoColumnInt(statement: ?*Stmt, index: c_int) c_int {
    return @truncate(repoColumnI64(statement, index));
}

fn repoColumnDouble(statement: ?*Stmt, index: c_int) f64 {
    var value: f64 = 0.0;
    _ = c.zova_statement_column_double(&c.zova_statement_column_double_request{
        .statement = statement,
        .index = index,
        .out_value = &value,
    });
    return value;
}

// ── Identities ──────────────────────────────────────────────────

/// FNV-1a 64 over the string; the offset basis for NULL.
fn fnv1a64(value: CStr) u64 {
    var hash: u64 = 1469598103934665603;
    if (value != null) {
        for (span(value)) |byte| hash = (hash ^ byte) *% 1099511628211;
    }
    return hash;
}

fn stableNumericId(value: CStr) i64 {
    return @intCast(fnv1a64(value) & std.math.maxInt(i64));
}

/// Writes "e:v1:<workspace>:<32 hex>" for the edge (source, type, target,
/// local name). False when a part is NULL or `out` is too small.
fn repoEdgeId(workspace_id: CStr, source: CStr, edge_type: CStr, target: CStr, local_name: CStr, out: []u8) bool {
    if (workspace_id == null or source == null or edge_type == null or target == null or local_name == null) return false;
    var hash: c.cbm_sha256_ctx = undefined;
    var digest: [c.CBM_SHA256_DIGEST_LEN]u8 = undefined;
    c.cbm_sha256_init(&hash);
    for ([_]CStr{ source, edge_type, target, local_name }) |part| {
        c.cbm_sha256_update(&hash, part, c.strlen(part));
        c.cbm_sha256_update(&hash, "\x00", 1);
    }
    c.cbm_sha256_final(&hash, &digest);
    const hex = std.fmt.bytesToHex(digest[0..16].*, .lower);
    return interop.bprint(out, "e:v1:{s}:{s}", .{ span(workspace_id), &hex }) < out.len;
}

fn repoGraphName(repo: *const Repository, out: []u8) bool {
    return c.cbm_zova_workspace_graph_name(repo.workspaceId(), out.ptr, out.len) == 0;
}

// ── Numeric id index ────────────────────────────────────────────

fn repoNumericIdCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    const a = cast(NumericIdEntry, left);
    const b = cast(NumericIdEntry, right);
    if (a.numeric_id != b.numeric_id) return if (a.numeric_id < b.numeric_id) -1 else 1;
    return @as(c_int, @intFromBool(a.order > b.order)) - @intFromBool(a.order < b.order);
}

fn repoNumericIdsClear(repo: *Repository) void {
    c.free(repo.numeric_ids);
    repoStringArenaFree(repo.numeric_id_strings);
    repo.numeric_ids = null;
    repo.numeric_id_count = 0;
    repo.numeric_id_strings = null;
}

/// One paged scan of the workspace graph; equal numeric ids keep scan order so
/// the lookup returns the same node the former linear search found first.
fn repoNumericIdsBuild(repo: *Repository) Error!void {
    repoNumericIdsClear(repo);
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (!repoGraphName(repo, &graph_name)) return error.Failed;
    repo.numeric_id_strings = @as(?*StringArena, interop.callocN(StringArena, 1)) orelse return error.Failed;
    errdefer repoNumericIdsClear(repo);
    var count: usize = 0;
    var cap: usize = 0;
    var cursor = std.mem.zeroes(ScanCursor);
    while (true) {
        var page = std.mem.zeroes(ScanResults);
        defer c.zova_graph_scan_results_free(&page);
        if (c.zova_graph_scan(&c.zova_graph_scan_request{
            .db = repo.db,
            .graph_name = &graph_name,
            .node_after = cursor,
            .node_limit = GRAPH_SCAN_PAGE,
            .edge_limit = 0,
            .out_results = &page,
        }) != c.ZOVA_OK) return error.Failed;
        const nodes = many(page.nodes, page.nodes_len);
        for (nodes) |*node| {
            if (node.node_id == null) continue;
            if (count == cap) {
                const next_cap = if (cap != 0) cap * 2 else GRAPH_SCAN_PAGE;
                const grown = interop.reallocN(NumericIdEntry, repo.numeric_ids, next_cap);
                if (grown == null) return error.Failed;
                repo.numeric_ids = grown;
                cap = next_cap;
            }
            const stable = repoStringArenaDup(repo.numeric_id_strings, node.node_id, c.strlen(node.node_id)) orelse return error.Failed;
            repo.numeric_ids[count] = .{
                .numeric_id = stableNumericId(stable),
                .order = count,
                .node_key = node.node_key,
                .stable_id = stable,
            };
            count += 1;
        }
        if (nodes.len != 0) {
            const last = &nodes[nodes.len - 1];
            cursor = .{ .created_order = last.created_order, .key = last.node_key };
        }
        if (page.has_more_nodes == 0) break;
    }
    if (count != 0) c.qsort(repo.numeric_ids, count, @sizeOf(NumericIdEntry), repoNumericIdCompare);
    repo.numeric_id_count = count;
}

/// The entries with `numeric_id` (equal ids are contiguous); null when the
/// index cannot be built, empty when no node has that id.
fn repoNumericIdsFind(repo: *Repository, numeric_id: i64) ?[]const NumericIdEntry {
    if (repo.numeric_id_strings == null) repoNumericIdsBuild(repo) catch return null;
    const entries = many(repo.numeric_ids, repo.numeric_id_count);
    var lo: usize = 0;
    var hi: usize = entries.len;
    while (lo < hi) {
        const mid = lo + (hi - lo) / 2;
        if (entries[mid].numeric_id < numeric_id) lo = mid + 1 else hi = mid;
    }
    var end = lo;
    while (end < entries.len and entries[end].numeric_id == numeric_id) end += 1;
    return entries[lo..end];
}

const KeyLookup = enum { found, missing, failed };

/// The Zova node key of `stable_id`. `failed` covers a NULL or empty id, an
/// index that cannot be built, and a node whose key is not positive.
fn repoNodeKeyForStableId(repo: *Repository, stable_id: ?Str, out_key: *i64) KeyLookup {
    const wanted = nonEmpty(stable_id) orelse return .failed;
    out_key.* = 0;
    const entries = repoNumericIdsFind(repo, stableNumericId(wanted)) orelse return .failed;
    for (entries) |*entry| {
        if (c.strcmp(entry.stable_id, wanted) == 0) {
            out_key.* = entry.node_key;
            return if (entry.node_key > 0) .found else .failed;
        }
    }
    return .missing;
}

/// Reads the graph nodes of `keys` into `out`, one result per key. The
/// caller releases `out` whatever the result; releasing a zeroed or already
/// released result is a no-op in the SDK.
fn repoNativeNodesByKey(repo: *const Repository, keys: [*c]const i64, count: usize, out: *KeyedNodes) bool {
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (!repoGraphName(repo, &graph_name)) return false;
    const status = c.zova_graph_nodes_get_many_keyed(&c.zova_graph_nodes_get_many_keyed_request{
        .db = repo.db,
        .graph_name = &graph_name,
        .node_keys = keys,
        .key_count = count,
        .out_results = out,
    });
    if (status != c.ZOVA_OK or out.len != count) {
        _ = c.fprintf(interop.stderr(), "zova keyed node read failed status=%d requested=%zu returned=%zu\n", @as(c_int, @bitCast(status)), count, out.len);
        return false;
    }
    return true;
}

fn repoDegreeMany(repo: *const Repository, keys: [*c]const i64, count: usize, direction: c_int, edge_type: CStr, out: [*c]u64) bool {
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (out == null or !repoGraphName(repo, &graph_name)) return false;
    return c.zova_graph_degree_many_keyed(&c.zova_graph_degree_many_keyed_request{
        .db = repo.db,
        .graph_name = &graph_name,
        .node_keys = keys,
        .node_count = count,
        .direction = direction,
        .edge_type = edge_type,
        .out_degrees = out,
        .out_degrees_capacity = count,
    }) == c.ZOVA_OK;
}

fn repoNow() c.struct_timespec {
    var now = std.mem.zeroes(c.struct_timespec);
    _ = c.cbm_clock_gettime(c.CLOCK_MONOTONIC, &now);
    return now;
}

fn repoElapsedMs(started: c.struct_timespec) f64 {
    const finished = repoNow();
    return @as(f64, @floatFromInt(finished.tv_sec - started.tv_sec)) * 1000.0 +
        @as(f64, @floatFromInt(finished.tv_nsec - started.tv_nsec)) / 1000000.0;
}

// ── Repository open and close ───────────────────────────────────

/// Finds the one ready, healthy workspace of `stored_project` and records its
/// id, key and generation in `repo`, which keeps the name the caller used.
fn repoLoadWorkspace(repo: *Repository, project: Str, stored_project: CStr) bool {
    if (c.cbm_zova_register_sql_functions(repo.db) != 0) return false;
    const sql =
        "SELECT r.workspace_id,p.workspace_key,g.generation FROM cbm_projects_v1 p " ++
        "JOIN cbm_workspace_registry r ON r.workspace_key=p.workspace_key " ++
        "JOIN cbm_database_generation_v1 g ON g.workspace_key=p.workspace_key " ++
        "WHERE p.project=?1 AND g.state='ready' AND g.generation=(SELECT MAX(g2.generation) " ++
        "FROM cbm_database_generation_v1 g2 WHERE g2.workspace_key=p.workspace_key " ++
        "AND g2.state='ready') AND NOT EXISTS (SELECT 1 FROM cbm_workspace_health_v1 h " ++
        "WHERE h.workspace_key=p.workspace_key AND h.state='rebuild_required')";
    var workspace: [*c]u8 = null;
    defer c.free(workspace);
    {
        const statement = repoPrepare(repo.db, sql) orelse return false;
        defer repoFinalize(statement);
        if (!repoBindText(statement, 1, stored_project)) return false;
        var step: c.zova_step_result = c.ZOVA_STEP_DONE;
        if (!repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return false;
        workspace = repoColumnText(statement, 0);
        repo.workspace_key = repoColumnI64(statement, 1);
        repo.generation = repoColumnI64(statement, 2);
        // A second row means the project name is not unique.
        var second: c.zova_step_result = c.ZOVA_STEP_DONE;
        if (!repoStep(statement, &second) or second == c.ZOVA_STEP_ROW) return false;
    }
    if (workspace == null or workspace[0] == 0 or c.strlen(workspace) >= ID_MAX or
        c.strlen(project) >= PROJECT_CAP) return false;
    _ = interop.bprint(&repo.workspace_id, "{s}", .{span(workspace)});
    _ = interop.bprint(&repo.project, "{s}", .{span(project)});
    return true;
}

pub export fn cbm_zova_repository_open(maybe_path: ?Str, maybe_project: ?Str) ?*Repository {
    if (!with_zova) return null;
    const path = nonEmpty(maybe_path) orelse return null;
    const project = nonEmpty(maybe_project) orelse return null;
    var stored_project = interop.heapStrdup(project);
    if (stored_project == null) return null;
    defer c.free(stored_project);
    // `project` may be a catalog selector; the tables hold the project name.
    if (cbm_zova_catalog_open(path)) |catalog| {
        defer cbm_zova_catalog_close(catalog);
        const selectors = [_]Str{project};
        var scope = std.mem.zeroes(Scope);
        defer cbm_zova_catalog_scope_free(&scope);
        if (cbm_zova_catalog_resolve(catalog, &selectors, 1, false, &scope) == OK and scope.count == 1) {
            const resolved = interop.heapStrdup(scope.projects[0].project);
            if (resolved == null) return null;
            c.free(stored_project);
            stored_project = resolved;
        }
    }
    const repo = @as(?*Repository, interop.callocN(Repository, 1)) orelse return null;
    repo.db = repoOpenReadOnly(path) orelse {
        c.free(repo);
        return null;
    };
    if (!repoLoadWorkspace(repo, project, stored_project)) {
        cbm_zova_repository_close(repo);
        return null;
    }
    return repo;
}

pub export fn cbm_zova_repository_close(maybe_repo: ?*Repository) void {
    if (!with_zova) return;
    const repo = maybe_repo orelse return;
    if (repo.db) |db| _ = c.zova_database_close(db);
    c.free(repo.numeric_ids);
    repoStringArenaFree(repo.numeric_id_strings);
    c.free(repo);
}

// ── Catalog ─────────────────────────────────────────────────────

fn catalogProjectFree(project: *CatalogProject) void {
    c.free(project.selector);
    c.free(project.project);
    c.free(project.root_path);
    c.free(project.indexed_at);
    c.free(project.model_fingerprint);
    c.free(project.health_reason);
    project.* = std.mem.zeroes(CatalogProject);
}

/// Deep copy; a NULL string copies as "". False (and `destination` cleared)
/// on OOM.
fn catalogProjectCopy(destination: *CatalogProject, source: *const CatalogProject) bool {
    destination.* = std.mem.zeroes(CatalogProject);
    destination.workspace_key = source.workspace_key;
    destination.vector_dimensions = source.vector_dimensions;
    destination.generation = source.generation;
    destination.ready = source.ready;
    destination.healthy = source.healthy;
    destination.workspace_id = source.workspace_id;
    inline for (.{ "selector", "project", "root_path", "indexed_at", "model_fingerprint", "health_reason" }) |field| {
        const text: CStr = @field(source, field);
        @field(destination, field) = interop.heapStrdup(if (text != null) text else "");
        if (@field(destination, field) == null) {
            catalogProjectFree(destination);
            return false;
        }
    }
    return true;
}

fn catalogSelectorUsed(earlier: []const CatalogProject, selector: CStr) bool {
    for (earlier) |*project| {
        if (project.selector != null and c.strcmp(project.selector, selector) == 0) return true;
    }
    return false;
}

fn catalogSelectorDisambiguate(selector: CStr, root_path: CStr, workspace_key: i64) [*c]u8 {
    const needed = c.strlen(selector) + 1 + 16 + 1 + 20 + 1;
    const result = interop.mallocN(u8, needed);
    if (result == null) return null;
    _ = interop.bprint(result[0..needed], "{s}-{x:0>16}-{d}", .{ span(selector), fnv1a64(root_path), workspace_key });
    return result;
}

/// Gives every project a unique selector: the shortest path suffix that no
/// earlier project uses, or a hashed name when the whole path is not enough.
fn catalogAssignSelectors(catalog: *Catalog) bool {
    const projects = catalog.items();
    for (projects, 0..) |*project, i| {
        var parent_levels: usize = 0;
        var selector = c.cbm_project_selector_from_path(project.root_path, parent_levels);
        if (selector == null) return false;
        while (catalogSelectorUsed(projects[0..i], selector)) {
            parent_levels += 1;
            var next = c.cbm_project_selector_from_path(project.root_path, parent_levels);
            if (next == null) {
                c.free(selector);
                return false;
            }
            if (c.strcmp(next, selector) == 0) {
                c.free(next);
                next = catalogSelectorDisambiguate(selector, project.root_path, project.workspace_key);
                if (next == null) {
                    c.free(selector);
                    return false;
                }
            }
            c.free(selector);
            selector = next;
        }
        project.selector = selector;
    }
    return true;
}

fn catalogProjectSelectorCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(CatalogProject, left).selector, cast(CatalogProject, right).selector);
}

/// Reads one registry row into `project`. False when a column could not be
/// read; the caller frees the row.
fn catalogReadProject(statement: ?*Stmt, project: *CatalogProject) bool {
    const workspace_id = repoColumnText(statement, 0);
    defer c.free(workspace_id);
    project.workspace_key = repoColumnI64(statement, 1);
    project.root_path = repoColumnText(statement, 2);
    project.project = repoColumnText(statement, 3);
    project.indexed_at = repoColumnText(statement, 4);
    project.model_fingerprint = repoColumnText(statement, 5);
    project.vector_dimensions = repoColumnInt(statement, 6);
    project.generation = repoColumnI64(statement, 7);
    project.ready = repoColumnI64(statement, 8) != 0;
    const health = repoColumnText(statement, 9);
    defer c.free(health);
    project.health_reason = repoColumnText(statement, 10);
    project.healthy = health != null and c.strcmp(health, "rebuild_required") != 0;
    if (workspace_id == null or workspace_id[0] == 0 or c.strlen(workspace_id) >= ID_MAX or
        project.root_path == null or project.project == null or project.indexed_at == null or
        project.model_fingerprint == null or health == null or project.health_reason == null)
        return false;
    _ = interop.bprint(&project.workspace_id, "{s}", .{span(workspace_id)});
    return true;
}

/// Opens the database, starts the read transaction that pins the catalog's
/// view, and loads every registered workspace. The caller closes the catalog
/// on failure.
fn catalogLoad(catalog: *Catalog, path: Str) Error!void {
    catalog.db = repoOpenReadOnly(path) orelse return error.Failed;
    if (c.cbm_zova_register_sql_functions(catalog.db) != 0 or !repoExec(catalog.db, "BEGIN"))
        return error.Failed;
    catalog.transaction_open = true;

    const sql =
        "SELECT r.workspace_id,r.workspace_key,r.canonical_root,COALESCE(p.project,'')," ++
        "COALESCE(p.indexed_at,''),COALESCE(s.model_fingerprint,'')," ++
        "COALESCE(s.vector_dimensions,0),r.active_generation," ++
        "CASE WHEN g.state='ready' THEN 1 ELSE 0 END," ++
        "COALESCE(h.state,'healthy'),COALESCE(h.reason,'') " ++
        "FROM cbm_workspace_registry r " ++
        "LEFT JOIN cbm_projects_v1 p ON p.workspace_key=r.workspace_key " ++
        "LEFT JOIN cbm_workspace_index_state_v1 s ON s.workspace_key=r.workspace_key " ++
        "LEFT JOIN cbm_database_generation_v1 g ON g.workspace_key=r.workspace_key " ++
        "AND g.generation=r.active_generation " ++
        "LEFT JOIN cbm_workspace_health_v1 h ON h.workspace_key=r.workspace_key " ++
        "ORDER BY r.workspace_key";
    {
        const statement = repoPrepare(catalog.db, sql) orelse return error.Failed;
        defer repoFinalize(statement);
        var capacity: c_int = 8;
        catalog.projects = interop.callocN(CatalogProject, @intCast(capacity));
        if (catalog.projects == null) return error.Failed;
        while (try repoNextRow(statement)) {
            if (catalog.count == capacity) {
                if (capacity > @divTrunc(INT_MAX, 2)) return error.Failed;
                const old_capacity: usize = @intCast(capacity);
                capacity *= 2;
                const grown = interop.reallocN(CatalogProject, catalog.projects, @intCast(capacity));
                if (grown == null) return error.Failed;
                catalog.projects = grown;
                @memset(grown[old_capacity..@intCast(capacity)], std.mem.zeroes(CatalogProject));
            }
            const project = &many(catalog.projects, capacity)[@intCast(catalog.count)];
            if (!catalogReadProject(statement, project)) {
                catalogProjectFree(project);
                return error.Failed;
            }
            catalog.count += 1;
        }
    }
    if (!catalogAssignSelectors(catalog)) return error.Failed;
    if (catalog.count > 1) {
        c.qsort(catalog.projects, @intCast(catalog.count), @sizeOf(CatalogProject), catalogProjectSelectorCompare);
    }
}

pub export fn cbm_zova_catalog_open(maybe_path: ?Str) ?*Catalog {
    if (!with_zova) return null;
    const path = nonEmpty(maybe_path) orelse return null;
    const catalog = @as(?*Catalog, interop.callocN(Catalog, 1)) orelse return null;
    catalogLoad(catalog, path) catch {
        cbm_zova_catalog_close(catalog);
        return null;
    };
    return catalog;
}

pub export fn cbm_zova_catalog_close(maybe_catalog: ?*Catalog) void {
    if (!with_zova) return;
    const catalog = maybe_catalog orelse return;
    if (catalog.db != null and catalog.transaction_open) _ = repoExec(catalog.db, "ROLLBACK");
    for (catalog.items()) |*project| catalogProjectFree(project);
    c.free(catalog.projects);
    if (catalog.db) |db| _ = c.zova_database_close(db);
    c.free(catalog);
}

pub export fn cbm_zova_catalog_scope_free(maybe_scope: ?*Scope) void {
    const scope = maybe_scope orelse return;
    if (with_zova) {
        for (many(scope.projects, scope.count)) |*project| catalogProjectFree(project);
        for (many(scope.excluded_projects, scope.excluded_count)) |*project| catalogProjectFree(project);
        c.free(scope.projects);
        c.free(scope.excluded_projects);
    }
    scope.* = std.mem.zeroes(Scope);
}

pub export fn cbm_zova_catalog_list(maybe_catalog: ?*Catalog, maybe_out: ?*Scope) c_int {
    if (!with_zova) {
        if (maybe_out) |out| out.* = std.mem.zeroes(Scope);
        return ERR;
    }
    const catalog = maybe_catalog orelse return ERR;
    const out = maybe_out orelse return ERR;
    out.* = std.mem.zeroes(Scope);
    const projects = catalog.items();
    if (projects.len == 0) return OK;
    out.projects = interop.callocN(CatalogProject, projects.len);
    if (out.projects == null) return ERR;
    for (projects, many(out.projects, projects.len)) |*source, *copy| {
        if (!catalogProjectCopy(copy, source)) {
            cbm_zova_catalog_scope_free(out);
            return ERR;
        }
        out.count += 1;
    }
    return OK;
}

pub export fn cbm_zova_catalog_error(maybe_catalog: ?*const Catalog) Str {
    if (!with_zova) return "";
    const catalog = maybe_catalog orelse return "";
    return if (catalog.error_text[0] != 0) @ptrCast(&catalog.error_text) else "";
}

/// Fills `out` (already zeroed) for cbm_zova_catalog_resolve; the caller
/// frees `out` unless this returns OK.
fn catalogResolveInto(catalog: *Catalog, selectors: ?[*]const Str, selector_count: c_int, wildcard: bool, out: *Scope) c_int {
    const projects = catalog.items();
    const wanted: usize = @intCast(if (wildcard) catalog.count else selector_count);
    if (wanted != 0) {
        out.projects = interop.callocN(CatalogProject, wanted);
        if (out.projects == null) return ERR;
    }
    const included = many(out.projects, wanted);

    if (wildcard) {
        if (wanted != 0) {
            out.excluded_projects = interop.callocN(CatalogProject, wanted);
            if (out.excluded_projects == null) return ERR;
        }
        const excluded = many(out.excluded_projects, wanted);
        for (projects) |*project| {
            if (!project.ready or !project.healthy) {
                if (!catalogProjectCopy(&excluded[@intCast(out.excluded_count)], project)) return ERR;
                out.excluded_count += 1;
            } else {
                if (!catalogProjectCopy(&included[@intCast(out.count)], project)) return ERR;
                out.count += 1;
            }
        }
        return OK;
    }
    for (selectors.?[0..wanted]) |selector| {
        const match: *const CatalogProject = for (projects) |*project| {
            if (c.strcmp(project.selector, selector) == 0) break project;
        } else {
            _ = interop.bprint(&catalog.error_text, "project '{s}' was not found", .{span(selector)});
            return NOT_FOUND;
        };
        if (!match.ready or !match.healthy) {
            const reason: []const u8 = if (!match.ready)
                "no ready generation"
            else if (match.health_reason[0] != 0)
                span(match.health_reason)
            else
                "workspace unhealthy";
            _ = interop.bprint(&catalog.error_text, "project '{s}' is unavailable: {s}", .{ span(selector), reason });
            return ERR;
        }
        if (!catalogProjectCopy(&included[@intCast(out.count)], match)) return ERR;
        out.count += 1;
    }
    return OK;
}

pub export fn cbm_zova_catalog_resolve(maybe_catalog: ?*Catalog, selectors: ?[*]const Str, selector_count: c_int, wildcard: bool, maybe_out: ?*Scope) c_int {
    if (!with_zova) {
        if (maybe_out) |out| out.* = std.mem.zeroes(Scope);
        return ERR;
    }
    const catalog = maybe_catalog orelse return ERR;
    const out = maybe_out orelse return ERR;
    if (!wildcard and (selectors == null or selector_count <= 0)) return ERR;
    catalog.error_text[0] = 0;
    out.* = std.mem.zeroes(Scope);
    const rc = catalogResolveInto(catalog, selectors, selector_count, wildcard, out);
    if (rc != OK) cbm_zova_catalog_scope_free(out);
    return rc;
}

/// "WITH scope(workspace_key,selector) AS (VALUES (?5,?6),(?7,?8)…) " for the
/// projects of `scope`, malloc'd. `next_bind` receives the first parameter
/// index after the scope's.
fn catalogScopeCte(scope: *const Scope, next_bind: *c_int) ?[*:0]u8 {
    if (scope.count <= 0) return null;
    const capacity = 64 + @as(usize, @intCast(scope.count)) * 32;
    const sql = (@as(?[*]u8, interop.mallocN(u8, capacity)) orelse return null)[0..capacity];
    var used = interop.bprint(sql, "WITH scope(workspace_key,selector) AS (VALUES ", .{});
    var bind: c_int = 5;
    for (0..@intCast(scope.count)) |i| {
        const separator: []const u8 = if (i != 0) "," else "";
        const written = interop.bprint(sql[used..], "{s}(?{d},?{d})", .{ separator, bind, bind + 1 });
        if (written >= capacity - used) {
            c.free(sql.ptr);
            return null;
        }
        used += written;
        bind += 2;
    }
    if (used + 3 > capacity) {
        c.free(sql.ptr);
        return null;
    }
    @memcpy(sql[used..][0..3], ") \x00");
    next_bind.* = bind;
    return @ptrCast(sql.ptr);
}

/// Binds `value` at `index`, or NULL when it is absent or empty.
fn repoBindOptionalText(statement: ?*Stmt, index: c_int, value: ?Str) bool {
    return if (nonEmpty(value)) |text| repoBindText(statement, index, text) else repoBindNull(statement, index);
}

/// Binds ?1 (query), ?2 (file pattern or NULL), ?3 (label or NULL), ?4
/// (exact name or NULL) and the scope's pairs.
fn catalogBindFtsScope(statement: ?*Stmt, scope: *const Scope, query: Str, filter: FtsFilter) bool {
    if (!repoBindText(statement, 1, query)) return false;
    if (!repoBindOptionalText(statement, 2, filter.file_pattern) or
        !repoBindOptionalText(statement, 3, filter.label) or
        !repoBindOptionalText(statement, 4, filter.exact_name)) return false;
    var bind: c_int = 5;
    for (many(scope.projects, scope.count)) |*project| {
        if (!repoBindI64(statement, bind, project.workspace_key) or
            !repoBindText(statement, bind + 1, project.selector)) return false;
        bind += 2;
    }
    return true;
}

/// What narrows and orders a full-text search besides the match expression:
/// a file path glob, a node label, and the name whose exact (case-insensitive)
/// matches rank first. Each may be absent.
const FtsFilter = struct {
    file_pattern: ?Str,
    label: ?Str,
    exact_name: ?Str,
};

/// malloc'd "<cte><tail>" plus `extra` spare bytes; NULL on OOM.
fn catalogSql(cte: Str, tail: []const u8, extra: usize) ?[]u8 {
    const capacity = c.strlen(cte) + tail.len + extra + 1;
    const sql = (@as(?[*]u8, interop.mallocN(u8, capacity)) orelse return null)[0..capacity];
    _ = interop.bprint(sql, "{s}{s}", .{ span(cte), tail });
    return sql;
}

/// The number of FTS matches in the scope, before paging.
fn catalogFtsTotal(catalog: *Catalog, scope: *const Scope, cte: Str, query: Str, filter: FtsFilter) ?i64 {
    const count_tail =
        "SELECT count(*) FROM scope sc JOIN cbm_nodes_v1 n " ++
        "ON n.workspace_key=sc.workspace_key " ++
        "JOIN cbm_nodes_fts_v1 f ON f.rowid=n.zova_node_key " ++
        "JOIN cbm_files_v1 p ON p.file_key=n.file_key " ++
        "WHERE cbm_nodes_fts_v1 MATCH ?1 " ++
        "AND n.label NOT IN ('File','Folder','Module','Section','Variable','Project') " ++
        "AND (?2 IS NULL OR p.file_path GLOB ?2) " ++
        "AND (?3 IS NULL OR n.label=?3)";
    const count_sql = catalogSql(cte, count_tail, 0) orelse return null;
    defer c.free(count_sql.ptr);
    const statement = repoPrepare(catalog.db, @ptrCast(count_sql.ptr)) orelse return null;
    defer repoFinalize(statement);
    if (!catalogBindFtsScope(statement, scope, query, filter)) return null;
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return null;
    return repoColumnI64(statement, 0);
}

/// The rows of one FTS page with, per row, its Zova node key and workspace.
const FtsRows = struct {
    results: [*c]SearchResult = null,
    keys: [*c]i64 = null,
    workspace_keys: [*c]i64 = null,
    count: c_int = 0,
};

/// Runs the paged FTS query and collects its rows.
fn catalogFtsReadRows(catalog: *Catalog, scope: *const Scope, cte: Str, next_bind: c_int, query: Str, filter: FtsFilter, limit: c_int, offset: c_int, rows: *FtsRows) Error!void {
    const data_tail =
        "SELECT n.workspace_key,n.zova_node_key,sc.selector,n.label,n.name," ++
        "n.qualified_name,p.file_path,n.start_line,n.end_line,n.properties," ++
        "(bm25(cbm_nodes_fts_v1)-CASE WHEN n.label IN ('Function','Method') THEN 10.0 " ++
        "WHEN n.label='Route' THEN 8.0 WHEN n.label IN " ++
        "('Class','Interface','Type','Enum') THEN 5.0 ELSE 0.0 END" ++
        "-CASE WHEN ?4 IS NOT NULL AND n.name=?4 COLLATE NOCASE THEN 20.0 ELSE 0.0 END) AS rank " ++
        "FROM scope sc JOIN cbm_nodes_v1 n ON n.workspace_key=sc.workspace_key " ++
        "JOIN cbm_nodes_fts_v1 f ON f.rowid=n.zova_node_key " ++
        "JOIN cbm_files_v1 p ON p.file_key=n.file_key " ++
        "WHERE cbm_nodes_fts_v1 MATCH ?1 " ++
        "AND n.label NOT IN ('File','Folder','Module','Section','Variable','Project') " ++
        "AND (?2 IS NULL OR p.file_path GLOB ?2) " ++
        "AND (?3 IS NULL OR n.label=?3) " ++
        "ORDER BY rank,sc.selector,n.qualified_name,n.zova_node_key ";
    const statement = prepared: {
        const data_sql = catalogSql(cte, data_tail, 79) orelse return error.Failed;
        defer c.free(data_sql.ptr);
        const head = c.strlen(data_sql.ptr);
        _ = interop.bprint(data_sql[head..], "LIMIT ?{d} OFFSET ?{d}", .{ next_bind, next_bind + 1 });
        break :prepared repoPrepare(catalog.db, @ptrCast(data_sql.ptr)) orelse return error.Failed;
    };
    defer repoFinalize(statement);
    if (!catalogBindFtsScope(statement, scope, query, filter) or
        !repoBindI64(statement, next_bind, limit) or
        !repoBindI64(statement, next_bind + 1, offset)) return error.Failed;

    var capacity: c_int = @max(@min(limit, 64), 1);
    rows.results = interop.callocN(SearchResult, @intCast(capacity));
    rows.keys = interop.callocN(i64, @intCast(capacity));
    rows.workspace_keys = interop.callocN(i64, @intCast(capacity));
    if (rows.results == null or rows.keys == null or rows.workspace_keys == null) return error.Failed;
    while (try repoNextRow(statement)) {
        if (rows.count == capacity) {
            const old_capacity: usize = @intCast(capacity);
            if (capacity > @divTrunc(INT_MAX, 2)) return error.Failed;
            capacity *= 2;
            const grown_results = interop.reallocN(SearchResult, rows.results, @intCast(capacity));
            if (grown_results != null) rows.results = grown_results;
            const grown_keys = interop.reallocN(i64, rows.keys, @intCast(capacity));
            if (grown_keys != null) rows.keys = grown_keys;
            const grown_workspace_keys = interop.reallocN(i64, rows.workspace_keys, @intCast(capacity));
            if (grown_workspace_keys != null) rows.workspace_keys = grown_workspace_keys;
            if (grown_results == null or grown_keys == null or grown_workspace_keys == null) return error.Failed;
            @memset(grown_results[old_capacity..@intCast(capacity)], std.mem.zeroes(SearchResult));
        }
        const index: usize = @intCast(rows.count);
        rows.workspace_keys[index] = repoColumnI64(statement, 0);
        rows.keys[index] = repoColumnI64(statement, 1);
        const result = &many(rows.results, capacity)[index];
        const node = &result.node;
        node.project = repoColumnText(statement, 2);
        node.label = repoColumnText(statement, 3);
        node.name = repoColumnText(statement, 4);
        node.qualified_name = repoColumnText(statement, 5);
        node.file_path = repoColumnText(statement, 6);
        node.start_line = repoColumnInt(statement, 7);
        node.end_line = repoColumnInt(statement, 8);
        node.properties_json = repoColumnText(statement, 9);
        result.rank = repoColumnDouble(statement, 10);
        if (node.project == null or node.label == null or node.name == null or
            node.qualified_name == null or node.file_path == null or node.properties_json == null)
        {
            c.cbm_node_free_fields(node);
            return error.Failed;
        }
        rows.count += 1;
    }
}

/// Sets each row's numeric id and label from the graph node behind it, one
/// keyed read per project.
fn catalogFtsApplyNative(catalog: *Catalog, scope: *const Scope, rows: *FtsRows) Error!void {
    const results = many(rows.results, rows.count);
    const keys = many(rows.keys, rows.count);
    const workspace_keys = many(rows.workspace_keys, rows.count);
    for (many(scope.projects, scope.count)) |*project| {
        var project_count: usize = 0;
        for (workspace_keys) |key| {
            if (key == project.workspace_key) project_count += 1;
        }
        if (project_count == 0) continue;
        const project_keys = interop.mallocN(i64, project_count);
        defer c.free(project_keys);
        const positions = interop.mallocN(usize, project_count);
        defer c.free(positions);
        if (project_keys == null or positions == null) return error.Failed;
        var written: usize = 0;
        for (workspace_keys, keys, 0..) |workspace_key, key, i| {
            if (workspace_key != project.workspace_key) continue;
            project_keys[written] = key;
            positions[written] = i;
            written += 1;
        }
        var workspace = Repository{ .db = catalog.db, .workspace_key = project.workspace_key };
        workspace.workspace_id = project.workspace_id;
        var native = std.mem.zeroes(KeyedNodes);
        defer c.zova_graph_keyed_node_results_free(&native);
        if (!repoNativeNodesByKey(&workspace, project_keys, project_count, &native)) return error.Failed;
        for (many(native.items, project_count), positions[0..project_count]) |*item, position| {
            if (item.found == 0 or item.node_id == null or item.kind == null) return error.Failed;
            const node = &results[position].node;
            node.id = stableNumericId(item.node_id);
            freeConst(node.label);
            node.label = repoDup(item.kind, item.kind_len);
            if (node.label == null) return error.Failed;
        }
    }
}

pub export fn cbm_zova_catalog_search_fts(maybe_catalog: ?*Catalog, maybe_scope: ?*const Scope, maybe_query: ?Str, file_pattern: ?Str, label: ?Str, exact_name: ?Str, limit_arg: c_int, offset: c_int, maybe_out: ?*SearchOutput) c_int {
    if (!with_zova) {
        if (maybe_out) |out| out.* = std.mem.zeroes(SearchOutput);
        return ERR;
    }
    const catalog = maybe_catalog orelse return ERR;
    const scope = maybe_scope orelse return ERR;
    const query = nonEmpty(maybe_query) orelse return ERR;
    const out = maybe_out orelse return ERR;
    if (scope.count <= 0 or offset < 0) return ERR;
    out.* = std.mem.zeroes(SearchOutput);
    const limit = if (limit_arg <= 0) 10 else limit_arg;

    var next_bind: c_int = 0;
    const cte = catalogScopeCte(scope, &next_bind) orelse return ERR;
    defer c.free(cte);
    const filter = FtsFilter{ .file_pattern = file_pattern, .label = label, .exact_name = exact_name };
    const total = catalogFtsTotal(catalog, scope, cte, query, filter) orelse return ERR;
    if (total < 0 or total > INT_MAX) return ERR;

    var rows = FtsRows{};
    defer {
        c.free(rows.keys);
        c.free(rows.workspace_keys);
    }
    read: {
        catalogFtsReadRows(catalog, scope, cte, next_bind, query, filter, limit, offset, &rows) catch break :read;
        catalogFtsApplyNative(catalog, scope, &rows) catch break :read;
        out.results = rows.results;
        out.count = rows.count;
        out.total = @intCast(total);
        return OK;
    }
    var failed = SearchOutput{ .results = rows.results, .count = rows.count, .total = 0 };
    c.cbm_store_search_free(&failed);
    return ERR;
}

fn catalogSearchResultCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left = &cast(SearchResult, left_ptr).node;
    const right = &cast(SearchResult, right_ptr).node;
    inline for (.{ "name", "qualified_name", "project" }) |field| {
        const a: CStr = @field(left, field);
        const b: CStr = @field(right, field);
        const compared = c.strcmp(if (a != null) a else "", if (b != null) b else "");
        if (compared != 0) return compared;
    }
    return @as(c_int, @intFromBool(left.id > right.id)) - @intFromBool(left.id < right.id);
}

/// The matches of every project in a scope, before sorting and paging.
const Combined = struct {
    results: [*c]SearchResult = null,
    count: c_int = 0,
    capacity: c_int = 0,

    fn free(combined: *Combined) void {
        var owned = SearchOutput{ .results = combined.results, .count = combined.count, .total = 0 };
        c.cbm_store_search_free(&owned);
    }
};

/// Runs the structured search in every project of the scope and moves the
/// matches into `combined`, each under its project's selector.
fn catalogSearchCollect(catalog: *Catalog, scope: *const Scope, params: *const SearchParams, combined: *Combined) Error!void {
    for (many(scope.projects, scope.count)) |*project| {
        var repository = Repository{
            .db = catalog.db,
            .workspace_key = project.workspace_key,
            .generation = project.generation,
        };
        repository.workspace_id = project.workspace_id;
        if (c.strlen(project.project) >= PROJECT_CAP) return error.Failed;
        _ = interop.bprint(&repository.project, "{s}", .{span(project.project)});

        var local_params = params.*;
        local_params.project = project.project;
        local_params.limit = INT_MAX;
        local_params.offset = 0;
        local_params.include_connected = false;
        var local = std.mem.zeroes(SearchOutput);
        defer c.cbm_store_search_free(&local);
        if (cbm_zova_repository_search(&repository, repository.workspaceId(), &local_params, &local) != OK)
            return error.Failed;
        if (local.count > INT_MAX - combined.count) return error.Failed;
        const needed = combined.count + local.count;
        if (needed > combined.capacity) {
            var grown_capacity: c_int = if (combined.capacity != 0) combined.capacity else 64;
            while (grown_capacity < needed) {
                if (grown_capacity > @divTrunc(INT_MAX, 2)) {
                    grown_capacity = needed;
                    break;
                }
                grown_capacity *= 2;
            }
            const grown = interop.reallocN(SearchResult, combined.results, @intCast(grown_capacity));
            if (grown == null) return error.Failed;
            combined.results = grown;
            @memset(grown[@intCast(combined.capacity)..@intCast(grown_capacity)], std.mem.zeroes(SearchResult));
            combined.capacity = grown_capacity;
        }
        for (many(local.results, local.count)) |*result| {
            const selector = interop.heapStrdup(project.selector);
            if (selector == null) return error.Failed;
            freeConst(result.node.project);
            result.node.project = selector;
            combined.results[@intCast(combined.count)] = result.*;
            combined.count += 1;
            result.* = std.mem.zeroes(SearchResult);
        }
    }
}

pub export fn cbm_zova_catalog_search(maybe_catalog: ?*Catalog, maybe_scope: ?*const Scope, maybe_params: ?*const SearchParams, maybe_out: ?*SearchOutput) c_int {
    if (!with_zova) {
        if (maybe_out) |out| out.* = std.mem.zeroes(SearchOutput);
        return ERR;
    }
    const catalog = maybe_catalog orelse return ERR;
    const scope = maybe_scope orelse return ERR;
    const params = maybe_params orelse return ERR;
    const out = maybe_out orelse return ERR;
    if (scope.count <= 0 or params.offset < 0) return ERR;
    out.* = std.mem.zeroes(SearchOutput);
    var combined = Combined{};
    defer combined.free();
    catalogSearchCollect(catalog, scope, params, &combined) catch return ERR;

    if (combined.count > 1) {
        c.qsort(combined.results, @intCast(combined.count), @sizeOf(SearchResult), catalogSearchResultCompare);
    }
    const limit = if (params.limit > 0) params.limit else 10;
    const available = if (params.offset < combined.count) combined.count - params.offset else 0;
    const page_count: usize = @intCast(@min(available, limit));
    var page: [*c]SearchResult = null;
    if (page_count != 0) {
        page = interop.callocN(SearchResult, page_count);
        if (page == null) return ERR;
    }
    const sources = many(combined.results, combined.count);
    for (many(page, page_count), 0..) |*slot, i| {
        const source = &sources[@as(usize, @intCast(params.offset)) + i];
        slot.* = source.*;
        source.* = std.mem.zeroes(SearchResult);
    }
    out.results = page;
    out.count = @intCast(page_count);
    out.total = combined.count;
    return OK;
}

pub export fn cbm_zova_catalog_search_semantic(maybe_catalog: ?*Catalog, maybe_path: ?Str, maybe_scope: ?*const Scope, keywords: [*c][*c]const u8, keyword_count: c_int, limit: c_int, offset: c_int, out: [*c][*c]c.cbm_vector_result_t, out_count: [*c]c_int, out_total: [*c]c_int) c_int {
    if (!with_zova) {
        if (out != null) out.* = null;
        if (out_count != null) out_count.* = 0;
        if (out_total != null) out_total.* = 0;
        return ERR;
    }
    const catalog = maybe_catalog orelse return ERR;
    const path = maybe_path orelse return ERR;
    const scope = maybe_scope orelse return ERR;
    if (scope.count <= 0 or keywords == null or keyword_count <= 0 or out == null or
        out_count == null or out_total == null) return ERR;
    out_total.* = 0;
    const projects = many(scope.projects, scope.count);
    const model: CStr = projects[0].model_fingerprint;
    const dimensions = projects[0].vector_dimensions;
    if (model == null or model[0] == 0 or dimensions <= 0) return c.CBM_ZOVA_CATALOG_INCOMPATIBLE_MODELS;
    for (projects) |*project| {
        if (project.model_fingerprint == null or c.strcmp(project.model_fingerprint, model) != 0 or
            project.vector_dimensions != dimensions)
            return c.CBM_ZOVA_CATALOG_INCOMPATIBLE_MODELS;
    }

    const vector_counts = interop.callocN(c_int, projects.len);
    defer c.free(vector_counts);
    var total: i64 = 0;
    {
        const count_statement = repoPrepare(catalog.db, "SELECT node_vector_rows FROM cbm_generation_integrity_v2 " ++
            "WHERE workspace_key=?1 AND generation=?2");
        defer repoFinalize(count_statement);
        if (count_statement == null or vector_counts == null) return ERR;
        for (projects, 0..) |*project, i| {
            if (c.zova_statement_reset(count_statement) != c.ZOVA_OK or
                c.zova_statement_clear_bindings(count_statement) != c.ZOVA_OK or
                !repoBindI64(count_statement, 1, project.workspace_key) or
                !repoBindI64(count_statement, 2, project.generation)) return ERR;
            var step: c.zova_step_result = c.ZOVA_STEP_DONE;
            if (!repoStep(count_statement, &step) or step != c.ZOVA_STEP_ROW) return ERR;
            const count = repoColumnI64(count_statement, 0);
            if (count < 0 or total > INT_MAX - count) return ERR;
            vector_counts[i] = @intCast(count);
            total += count;
        }
    }
    out_total.* = @intCast(total);

    var rc: c_int = OK;
    {
        const workspaces = interop.callocN(c.cbm_zova_vector_workspace_t, projects.len);
        if (workspaces == null) return ERR;
        defer c.free(workspaces);
        var workspace_count: usize = 0;
        for (projects, 0..) |*project, i| {
            if (vector_counts[i] == 0) continue;
            workspaces[workspace_count] = .{
                .workspace_key = project.workspace_key,
                .workspace_id = &project.workspace_id,
                .project = project.project,
                .selector = project.selector,
                .model_fingerprint = project.model_fingerprint,
                .vector_dimensions = project.vector_dimensions,
                .generation = project.generation,
            };
            workspace_count += 1;
        }
        if (workspace_count > 0) {
            rc = c.cbm_store_zova_multi_vector_search(path, workspaces, @intCast(workspace_count), keywords, keyword_count, limit, offset, out, out_count);
        } else {
            out.* = null;
            out_count.* = 0;
        }
    }

    // The search read through its own connection; the scope is stale when a
    // project it names has moved to another generation or model since.
    const current = if (rc == OK) cbm_zova_catalog_open(path) else null;
    defer cbm_zova_catalog_close(current);
    const selectors: ?[*]Str = interop.callocN(Str, projects.len);
    defer c.free(@ptrCast(selectors));
    var verified = std.mem.zeroes(Scope);
    defer cbm_zova_catalog_scope_free(&verified);
    if (current == null or selectors == null) rc = ERR;
    if (rc == OK) {
        for (projects, selectors.?[0..projects.len]) |*project, *selector| selector.* = project.selector;
        if (cbm_zova_catalog_resolve(current, selectors, scope.count, false, &verified) != OK)
            rc = c.CBM_ZOVA_CATALOG_STALE_SCOPE;
    }
    if (rc == OK) {
        for (projects, many(verified.projects, verified.count)) |*project, *now| {
            if (now.workspace_key != project.workspace_key or now.generation != project.generation or
                now.vector_dimensions != project.vector_dimensions or
                c.strcmp(now.model_fingerprint, project.model_fingerprint) != 0)
                rc = c.CBM_ZOVA_CATALOG_STALE_SCOPE;
        }
    }
    if (rc != OK) {
        c.cbm_store_free_vector_results(out.*, out_count.*);
        out.* = null;
        out_count.* = 0;
    }
    return rc;
}

// ── Workspace reads: nodes ──────────────────────────────────────

const KeyedNode = c.zova_graph_keyed_node_result;
const KeyedNeighbors = c.zova_graph_keyed_neighbor_results;
const EdgePayloads = c.zova_graph_edge_payload_results;
const ScanEdge = c.zova_graph_scan_edge;
const PayloadRecord = c.cbm_zova_edge_payload_record_t;

/// (size_t)INT64_MAX: every neighbor.
const NEIGHBOR_LIMIT: usize = @truncate(@as(u64, std.math.maxInt(i64)));

pub export fn cbm_zova_repository_workspace_id(maybe_repo: ?*const Repository) Str {
    if (!with_zova) return "";
    const repo = maybe_repo orelse return "";
    return repo.workspaceId();
}

/// The repository when `workspace_id` is well formed and names its workspace.
fn repoWorkspaceRequired(maybe_repo: ?*Repository, workspace_id: ?Str) ?*Repository {
    const repo = maybe_repo orelse return null;
    if (c.cbm_zova_workspace_id_validate(workspace_id) != 0) return null;
    return if (c.strcmp(repo.workspaceId(), workspace_id) == 0) repo else null;
}

const node_by_key_sql =
    "SELECT n.name,n.qualified_name,f.file_path,n.start_line,n.end_line,n.properties " ++
    "FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key " ++
    "WHERE n.workspace_key=?1 AND n.zova_node_key=?2";

fn repoProjectDup(repo: *const Repository) [*c]u8 {
    return repoDupZ(&repo.project);
}

/// Fills `node` from its graph node and the current row of node_by_key_sql.
/// A string that could not be copied is left NULL.
fn repoFillNode(repo: *const Repository, statement: ?*Stmt, item: *const KeyedNode, node: *Node) void {
    node.id = stableNumericId(item.node_id);
    node.project = repoProjectDup(repo);
    node.label = repoDup(item.kind, item.kind_len);
    node.name = repoColumnText(statement, 0);
    node.qualified_name = repoColumnText(statement, 1);
    node.file_path = repoColumnText(statement, 2);
    node.start_line = repoColumnInt(statement, 3);
    node.end_line = repoColumnInt(statement, 4);
    node.properties_json = repoColumnText(statement, 5);
}

fn repoNodeComplete(node: *const Node) bool {
    return node.project != null and node.label != null and node.name != null and
        node.qualified_name != null and node.file_path != null and node.properties_json != null;
}

/// Rebinds node_by_key_sql to `node_key` and steps to its row.
fn repoSeekNode(repo: *const Repository, statement: ?*Stmt, node_key: i64) bool {
    if (c.zova_statement_reset(statement) != c.ZOVA_OK or
        c.zova_statement_clear_bindings(statement) != c.ZOVA_OK or
        !repoBindI64(statement, 1, repo.workspace_key) or
        !repoBindI64(statement, 2, node_key)) return false;
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    return repoStep(statement, &step) and step == c.ZOVA_STEP_ROW;
}

fn hydrateNodeKey(repo: *Repository, node_key: i64, out: *Node) c_int {
    if (node_key <= 0) return ERR;
    const statement = repoPrepare(repo.db, node_by_key_sql) orelse return ERR;
    defer repoFinalize(statement);
    if (!repoBindI64(statement, 1, repo.workspace_key) or !repoBindI64(statement, 2, node_key)) return ERR;
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return NOT_FOUND;
    var native = std.mem.zeroes(KeyedNodes);
    defer c.zova_graph_keyed_node_results_free(&native);
    if (!repoNativeNodesByKey(repo, &node_key, 1, &native)) return ERR;
    const item = &many(native.items, 1)[0];
    if (item.found == 0 or item.node_id == null or item.kind == null) return ERR;
    out.* = std.mem.zeroes(Node);
    repoFillNode(repo, statement, item, out);
    return OK;
}

pub export fn cbm_zova_repository_find_node_by_qn(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_qualified_name: ?Str, maybe_out: ?*Node) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const qualified_name = maybe_qualified_name orelse return ERR;
    const out = maybe_out orelse return ERR;
    var key: i64 = 0;
    {
        const statement = repoPrepare(repo.db, "SELECT zova_node_key FROM cbm_nodes_v1 " ++
            "WHERE workspace_key=?1 AND qualified_name=?2");
        defer repoFinalize(statement);
        var step: c.zova_step_result = c.ZOVA_STEP_DONE;
        if (statement != null and repoBindI64(statement, 1, repo.workspace_key) and
            repoBindText(statement, 2, qualified_name) and
            repoStep(statement, &step) and step == c.ZOVA_STEP_ROW)
            key = repoColumnI64(statement, 0);
    }
    return if (key > 0) hydrateNodeKey(repo, key, out) else NOT_FOUND;
}

pub export fn cbm_zova_repository_find_node_by_stable_id(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_stable_id: ?Str, maybe_out: ?*Node) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const stable_id = maybe_stable_id orelse return ERR;
    const out = maybe_out orelse return ERR;
    var key: i64 = 0;
    return switch (repoNodeKeyForStableId(repo, stable_id, &key)) {
        .found => hydrateNodeKey(repo, key, out),
        .missing => NOT_FOUND,
        .failed => ERR,
    };
}

const StableNodeRequest = extern struct {
    stable_id: CStr,
    input_index: usize,
    node_key: i64,
};

fn repoStableNodeRequestCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(StableNodeRequest, left).stable_id, cast(StableNodeRequest, right).stable_id);
}

fn repoStableNodeRequestFind(requests: []StableNodeRequest, stable_id: CStr) ?*StableNodeRequest {
    const key = StableNodeRequest{ .stable_id = stable_id, .input_index = 0, .node_key = 0 };
    return @ptrCast(@alignCast(c.bsearch(&key, requests.ptr, requests.len, @sizeOf(StableNodeRequest), repoStableNodeRequestCompare)));
}

/// Fills `nodes[i]` for `stable_ids[i]`. `requests` and `keys` are scratch
/// arrays of the same length. NOT_FOUND when an id names no graph node.
fn repoNodesByStableIds(repo: *Repository, stable_ids: []const ?Str, requests: []StableNodeRequest, keys: []i64, nodes: []Node) c_int {
    for (stable_ids, requests, 0..) |stable_id, *request, i| {
        request.* = .{ .stable_id = nonEmpty(stable_id) orelse return ERR, .input_index = i, .node_key = 0 };
    }
    if (requests.len > 1) c.qsort(requests.ptr, requests.len, @sizeOf(StableNodeRequest), repoStableNodeRequestCompare);
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (!repoGraphName(repo, &graph_name)) return ERR;
    var cursor = std.mem.zeroes(ScanCursor);
    while (true) {
        var page = std.mem.zeroes(ScanResults);
        defer c.zova_graph_scan_results_free(&page);
        if (c.zova_graph_scan(&c.zova_graph_scan_request{
            .db = repo.db,
            .graph_name = &graph_name,
            .node_after = cursor,
            .node_limit = GRAPH_SCAN_PAGE,
            .edge_limit = 0,
            .out_results = &page,
        }) != c.ZOVA_OK) return ERR;
        const page_nodes = many(page.nodes, page.nodes_len);
        for (page_nodes) |*node| {
            if (repoStableNodeRequestFind(requests, node.node_id)) |request| request.node_key = node.node_key;
        }
        if (page_nodes.len != 0) {
            const last = &page_nodes[page_nodes.len - 1];
            cursor = .{ .created_order = last.created_order, .key = last.node_key };
        }
        if (page.has_more_nodes == 0) break;
    }
    for (requests) |*request| {
        if (request.node_key <= 0) return NOT_FOUND;
        keys[request.input_index] = request.node_key;
    }
    var native = std.mem.zeroes(KeyedNodes);
    defer c.zova_graph_keyed_node_results_free(&native);
    if (!repoNativeNodesByKey(repo, keys.ptr, keys.len, &native)) return ERR;
    const statement = repoPrepare(repo.db, node_by_key_sql) orelse return ERR;
    defer repoFinalize(statement);
    for (many(native.items, keys.len), keys, stable_ids, nodes) |*item, key, stable_id, *node| {
        if (item.found == 0 or item.node_key != key or item.node_id == null or item.kind == null or
            c.strcmp(item.node_id, stable_id) != 0) return ERR;
        if (!repoSeekNode(repo, statement, key)) return ERR;
        repoFillNode(repo, statement, item, node);
        if (!repoNodeComplete(node)) return ERR;
    }
    return OK;
}

pub export fn cbm_zova_repository_find_nodes_by_stable_ids(maybe_repo: ?*Repository, workspace_id: ?Str, stable_ids: ?[*]const ?Str, count: c_int, maybe_out: ?*[*c]Node) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const out = maybe_out orelse return ERR;
    if (count < 0 or (count > 0 and stable_ids == null)) return ERR;
    out.* = null;
    if (count == 0) return OK;
    const n: usize = @intCast(count);
    const requests = interop.callocN(StableNodeRequest, n);
    defer c.free(requests);
    const keys = interop.callocN(i64, n);
    defer c.free(keys);
    const nodes = interop.callocN(Node, n);
    const rc = if (requests == null or keys == null or nodes == null)
        ERR
    else
        repoNodesByStableIds(repo, stable_ids.?[0..n], requests[0..n], keys[0..n], nodes[0..n]);
    if (rc == OK) {
        out.* = nodes;
    } else if (rc == NOT_FOUND) {
        c.free(nodes);
    } else {
        c.cbm_store_free_nodes(nodes, count);
    }
    return rc;
}

fn repoNodesByKeys(repo: *Repository, node_keys: []const i64, nodes: []Node) bool {
    for (node_keys) |key| {
        if (key <= 0) return false;
    }
    var native = std.mem.zeroes(KeyedNodes);
    defer c.zova_graph_keyed_node_results_free(&native);
    if (!repoNativeNodesByKey(repo, node_keys.ptr, node_keys.len, &native)) return false;
    const statement = repoPrepare(repo.db, node_by_key_sql) orelse return false;
    defer repoFinalize(statement);
    for (many(native.items, node_keys.len), node_keys, nodes) |*item, key, *node| {
        if (item.found == 0 or item.node_key != key or item.node_id == null or item.kind == null or
            !repoSeekNode(repo, statement, key)) return false;
        repoFillNode(repo, statement, item, node);
        if (!repoNodeComplete(node)) return false;
    }
    return true;
}

pub export fn cbm_zova_repository_find_nodes_by_keys(maybe_repo: ?*Repository, workspace_id: ?Str, node_keys: ?[*]const i64, count: c_int, maybe_out: ?*[*c]Node) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const out = maybe_out orelse return ERR;
    if (count < 0 or (count > 0 and node_keys == null)) return ERR;
    out.* = null;
    if (count == 0) return OK;
    const n: usize = @intCast(count);
    const nodes = interop.callocN(Node, n);
    if (nodes == null) return ERR;
    if (!repoNodesByKeys(repo, node_keys.?[0..n], nodes[0..n])) {
        c.cbm_store_free_nodes(nodes, count);
        return ERR;
    }
    out.* = nodes;
    return OK;
}

pub export fn cbm_zova_repository_stable_id_for_numeric_id(maybe_repo: ?*Repository, workspace_id: ?Str, numeric_id: i64, maybe_out: ?[*]u8, out_size: usize) c_int {
    if (!with_zova) {
        if (maybe_out) |out| {
            if (out_size != 0) out[0] = 0;
        }
        return ERR;
    }
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const out = (maybe_out orelse return ERR)[0..out_size];
    if (out.len == 0) return ERR;
    out[0] = 0;
    const entries = repoNumericIdsFind(repo, numeric_id) orelse return ERR;
    if (entries.len == 0) return NOT_FOUND;
    return if (interop.bprint(out, "{s}", .{span(entries[0].stable_id)}) < out.len) OK else ERR;
}

pub export fn cbm_zova_repository_find_node_by_numeric_id(repo: ?*Repository, workspace_id: ?Str, numeric_id: i64, out: ?*Node) c_int {
    if (!with_zova) return ERR;
    var stable: [ID_MAX + 96]u8 = undefined;
    const rc = cbm_zova_repository_stable_id_for_numeric_id(repo, workspace_id, numeric_id, &stable, stable.len);
    return if (rc == OK) cbm_zova_repository_find_node_by_stable_id(repo, workspace_id, @ptrCast(&stable), out) else rc;
}

/// Nodes read from SQL with the Zova key of each, before the graph read
/// that completes them.
const NodeRows = struct {
    nodes: [*c]Node = null,
    keys: [*c]i64 = null,
    count: c_int = 0,
};

fn repoFileOverlapRead(repo: *Repository, file_path: Str, start_line: c_int, end_line: c_int, rows: *NodeRows) Error!void {
    {
        const sql =
            "SELECT n.zova_node_key,n.name,n.qualified_name,f.file_path,n.start_line,n.end_line," ++
            "n.properties FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key " ++
            "WHERE n.workspace_key=?1 AND f.file_path=?2 " ++
            "AND n.label NOT IN ('Module','Package','File','Folder') " ++
            "AND n.start_line<=?3 AND n.end_line>=?4 " ++
            "ORDER BY n.start_line,n.qualified_name,n.zova_node_key";
        const statement = repoPrepare(repo.db, sql) orelse return error.Failed;
        defer repoFinalize(statement);
        if (!repoBindI64(statement, 1, repo.workspace_key) or !repoBindText(statement, 2, file_path) or
            !repoBindI64(statement, 3, end_line) or !repoBindI64(statement, 4, start_line)) return error.Failed;
        var cap: usize = 8;
        rows.nodes = interop.callocN(Node, cap);
        rows.keys = interop.callocN(i64, cap);
        if (rows.nodes == null or rows.keys == null) return error.Failed;
        while (try repoNextRow(statement)) {
            const index: usize = @intCast(rows.count);
            if (index == cap) {
                cap *= 2;
                const grown_nodes = interop.reallocN(Node, rows.nodes, cap);
                if (grown_nodes != null) rows.nodes = grown_nodes;
                const grown_keys = interop.reallocN(i64, rows.keys, cap);
                if (grown_keys != null) rows.keys = grown_keys;
                if (grown_nodes == null or grown_keys == null) return error.Failed;
                @memset(grown_nodes[index..cap], std.mem.zeroes(Node));
            }
            rows.keys[index] = repoColumnI64(statement, 0);
            const node = &many(rows.nodes, cap)[index];
            node.project = repoProjectDup(repo);
            node.name = repoColumnText(statement, 1);
            node.qualified_name = repoColumnText(statement, 2);
            node.file_path = repoColumnText(statement, 3);
            node.start_line = repoColumnInt(statement, 4);
            node.end_line = repoColumnInt(statement, 5);
            node.properties_json = repoColumnText(statement, 6);
            rows.count += 1;
        }
    }
    if (rows.count == 0) return;
    const count: usize = @intCast(rows.count);
    var native = std.mem.zeroes(KeyedNodes);
    defer c.zova_graph_keyed_node_results_free(&native);
    if (!repoNativeNodesByKey(repo, rows.keys, count, &native)) return error.Failed;
    for (many(rows.nodes, count), many(native.items, count)) |*node, *item| {
        if (item.found == 0 or item.node_id == null or item.kind == null) return error.Failed;
        node.id = stableNumericId(item.node_id);
        node.label = repoDup(item.kind, item.kind_len);
    }
}

pub export fn cbm_zova_repository_find_nodes_by_file_overlap(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_file_path: ?Str, start_line: c_int, end_line: c_int, maybe_out: ?*[*c]Node, maybe_out_count: ?*c_int) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const file_path = maybe_file_path orelse return ERR;
    const out = maybe_out orelse return ERR;
    const out_count = maybe_out_count orelse return ERR;
    out.* = null;
    out_count.* = 0;
    var rows = NodeRows{};
    defer c.free(rows.keys);
    repoFileOverlapRead(repo, file_path, start_line, end_line, &rows) catch {
        c.cbm_store_free_nodes(rows.nodes, rows.count);
        return ERR;
    };
    out.* = rows.nodes;
    out_count.* = rows.count;
    return OK;
}

// ── Workspace reads: project and files ──────────────────────────

pub export fn cbm_zova_repository_get_project(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_out: ?*c.cbm_project_t) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const out = maybe_out orelse return ERR;
    const statement = repoPrepare(repo.db, "SELECT project,indexed_at,root_path FROM cbm_projects_v1 " ++
        "WHERE workspace_key=?1") orelse return ERR;
    defer repoFinalize(statement);
    if (!repoBindI64(statement, 1, repo.workspace_key)) return ERR;
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return ERR;
    out.* = std.mem.zeroes(c.cbm_project_t);
    out.name = repoColumnText(statement, 0);
    out.indexed_at = repoColumnText(statement, 1);
    out.root_path = repoColumnText(statement, 2);
    return OK;
}

pub export fn cbm_zova_repository_index_status(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_generation: ?*i64, maybe_indexed_at: ?*[*c]u8) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const generation = maybe_generation orelse return ERR;
    const indexed_at = maybe_indexed_at orelse return ERR;
    var project = std.mem.zeroes(c.cbm_project_t);
    if (cbm_zova_repository_get_project(repo, workspace_id, &project) != OK) return ERR;
    generation.* = repo.generation;
    indexed_at.* = @constCast(project.indexed_at);
    freeConst(project.name);
    freeConst(project.root_path);
    return OK;
}

pub export fn cbm_zova_repository_counts(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_nodes: ?*c_int, maybe_edges: ?*c_int) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const nodes = maybe_nodes orelse return ERR;
    const edges = maybe_edges orelse return ERR;
    const statement = repoPrepare(repo.db, "SELECT (SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=?1)," ++
        "metadata_edges FROM cbm_generation_integrity_v2 " ++
        "WHERE workspace_key=?1 AND generation=?2") orelse return ERR;
    defer repoFinalize(statement);
    if (!repoBindI64(statement, 1, repo.workspace_key) or !repoBindI64(statement, 2, repo.generation)) return ERR;
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return ERR;
    nodes.* = repoColumnInt(statement, 0);
    edges.* = repoColumnInt(statement, 1);
    return OK;
}

/// Whether a Project node carries a coverage stamp. Generations indexed
/// before coverage stamps existed carry no marker.
fn repoCoverageRecorded(repo: *Repository) Error!bool {
    const statement = repoPrepare(repo.db, "SELECT properties FROM cbm_nodes_v1 WHERE workspace_key=?1 " ++
        "AND label='Project'") orelse return error.Failed;
    defer repoFinalize(statement);
    if (!repoBindI64(statement, 1, repo.workspace_key)) return error.Failed;
    var recorded = false;
    while (try repoNextRow(statement)) {
        const props = repoColumnText(statement, 0);
        defer c.free(props);
        if (props != null and c.strstr(props, "\"coverage_version\":") != null) recorded = true;
    }
    return recorded;
}

fn repoRootPath(repo: *Repository) Error![*c]u8 {
    const statement = repoPrepare(repo.db, "SELECT root_path FROM cbm_projects_v1 WHERE workspace_key=?1") orelse return error.Failed;
    defer repoFinalize(statement);
    if (!repoBindI64(statement, 1, repo.workspace_key)) return error.Failed;
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return error.Failed;
    const root_path = repoColumnText(statement, 0);
    return if (root_path != null) root_path else error.Failed;
}

/// Reads the coverage rows into `rows`/`n`, which the caller owns either way.
fn repoFileCoverageRows(repo: *Repository, exact_path: ?Str, rows: *[*c]FileCoverage, n: *c_int) Error!void {
    const statement = repoPrepare(repo.db, "SELECT f.file_path,n.properties,n.zova_node_key IS NOT NULL," ++
        "h.file_key IS NOT NULL,COALESCE(h.mtime_ns,0),COALESCE(h.size_bytes,0) " ++
        "FROM cbm_files_v1 f " ++
        "LEFT JOIN cbm_nodes_v1 n ON n.workspace_key=f.workspace_key " ++
        "AND n.file_key=f.file_key AND n.label='File' " ++
        "LEFT JOIN cbm_file_hashes_v1 h ON h.file_key=f.file_key " ++
        "WHERE f.workspace_key=?1 AND (?2 IS NULL OR f.file_path=?2) " ++
        "AND (n.zova_node_key IS NOT NULL OR h.file_key IS NOT NULL) " ++
        "ORDER BY f.file_path") orelse return error.Failed;
    defer repoFinalize(statement);
    if (!repoBindI64(statement, 1, repo.workspace_key)) return error.Failed;
    if (exact_path) |path| {
        if (!repoBindText(statement, 2, path)) return error.Failed;
    }
    var cap: c_int = 0;
    while (try repoNextRow(statement)) {
        if (n.* == cap) {
            if (cap > @divTrunc(INT_MAX, 2)) return error.Failed;
            const next: c_int = if (cap != 0) cap * 2 else 64;
            const grown = interop.reallocN(FileCoverage, rows.*, @intCast(next));
            if (grown == null) return error.Failed;
            rows.* = grown;
            cap = next;
        }
        const row = &many(rows.*, cap)[@intCast(n.*)];
        row.* = std.mem.zeroes(FileCoverage);
        row.file_path = repoColumnText(statement, 0);
        const has_node = repoColumnI64(statement, 2) != 0;
        row.properties = if (has_node) repoColumnText(statement, 1) else null;
        row.has_hash = repoColumnI64(statement, 3) != 0;
        row.mtime_ns = repoColumnI64(statement, 4);
        row.size_bytes = repoColumnI64(statement, 5);
        n.* += 1;
        if (row.file_path == null or (has_node and row.properties == null)) return error.Failed;
    }
}

fn repoFileCoverageRead(repo: *Repository, exact_path: ?Str, out_root_path: ?*[*c]u8, out_recorded: ?*bool, rows: *[*c]FileCoverage, n: *c_int) Error!void {
    if (out_recorded) |recorded| {
        recorded.* = false;
        recorded.* = try repoCoverageRecorded(repo);
    }
    if (out_root_path) |root_path| root_path.* = try repoRootPath(repo);
    try repoFileCoverageRows(repo, exact_path, rows, n);
}

pub export fn cbm_zova_repository_file_coverage(maybe_repo: ?*Repository, workspace_id: ?Str, exact_path: ?Str, maybe_out: ?*[*c]FileCoverage, maybe_count: ?*c_int, out_root_path: ?*[*c]u8, out_generation: ?*i64, out_recorded: ?*bool) c_int {
    if (!with_zova) {
        if (out_recorded) |recorded| recorded.* = false;
        if (maybe_out) |out| out.* = null;
        if (maybe_count) |count| count.* = 0;
        if (out_root_path) |root_path| root_path.* = null;
        return ERR;
    }
    const out = maybe_out orelse return ERR;
    const count = maybe_count orelse return ERR;
    out.* = null;
    count.* = 0;
    if (out_root_path) |root_path| root_path.* = null;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    if (out_generation) |generation| generation.* = repo.generation;
    var rows: [*c]FileCoverage = null;
    var n: c_int = 0;
    repoFileCoverageRead(repo, exact_path, out_root_path, out_recorded, &rows, &n) catch {
        cbm_zova_file_coverage_free(rows, n);
        if (out_root_path) |root_path| {
            c.free(root_path.*);
            root_path.* = null;
        }
        return ERR;
    };
    out.* = rows;
    count.* = n;
    return OK;
}

pub export fn cbm_zova_file_coverage_free(rows: [*c]FileCoverage, count: c_int) void {
    if (!with_zova) return;
    if (rows == null) return;
    for (many(rows, count)) |*row| {
        c.free(row.file_path);
        c.free(row.properties);
    }
    c.free(rows);
}

pub export fn cbm_zova_repository_project_summary(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_out: ?*c.cbm_project_summary_export_t) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const out = maybe_out orelse return ERR;
    out.* = std.mem.zeroes(c.cbm_project_summary_export_t);
    const statement = repoPrepare(repo.db, "SELECT summary,source_hash,created_at,updated_at " ++
        "FROM cbm_project_summaries_v2 WHERE workspace_key=?1") orelse return ERR;
    defer repoFinalize(statement);
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!repoBindI64(statement, 1, repo.workspace_key) or !repoStep(statement, &step)) return ERR;
    if (step == c.ZOVA_STEP_DONE) return NOT_FOUND;
    out.summary = repoColumnText(statement, 0);
    out.source_hash = repoColumnText(statement, 1);
    out.created_at = repoColumnText(statement, 2);
    out.updated_at = repoColumnText(statement, 3);
    return OK;
}

// ── Workspace reads: edges ──────────────────────────────────────

/// Where repoAppendPayloadEdge puts the records of one graph edge.
const PayloadEdgeContext = struct {
    repo: *const Repository,
    source: CStr,
    target: CStr,
    edge_type: CStr,
    edge_type_len: usize,
    edges: *[*c]Edge,
    count: *c_int,
    capacity: *c_int,
};

fn repoAppendPayloadEdge(maybe_record: [*c]const PayloadRecord, context_ptr: ?*anyopaque) callconv(.c) c_int {
    if (maybe_record == null) return -1;
    const record: *const PayloadRecord = @ptrCast(maybe_record);
    const context: *PayloadEdgeContext = @ptrCast(@alignCast(context_ptr orelse return -1));
    if (context.count.* == context.capacity.*) {
        if (context.capacity.* > @divTrunc(INT_MAX, 2)) return -1;
        const old_capacity: usize = @intCast(context.capacity.*);
        const grown_capacity = context.capacity.* * 2;
        const grown = interop.reallocN(Edge, context.edges.*, @intCast(grown_capacity));
        if (grown == null) return -1;
        @memset(grown[old_capacity..@intCast(grown_capacity)], std.mem.zeroes(Edge));
        context.edges.* = grown;
        context.capacity.* = grown_capacity;
    }
    var local_name: [ID_MAX + 80]u8 = undefined;
    const local = many(record.local_name.data, record.local_name.len);
    if (record.local_name.len >= local_name.len) return -1;
    @memcpy(local_name[0..local.len], local);
    local_name[record.local_name.len] = 0;
    var edge_id: [ID_MAX + 80]u8 = undefined;
    if (!repoEdgeId(context.repo.workspaceId(), context.source, context.edge_type, context.target, &local_name, &edge_id)) return -1;
    const project = repoProjectDup(context.repo);
    const edge_type = repoDup(context.edge_type, context.edge_type_len);
    const properties = repoDup(record.properties.data, record.properties.len);
    if (project == null or edge_type == null or properties == null) {
        c.free(project);
        c.free(edge_type);
        c.free(properties);
        return -1;
    }
    context.edges.*[@intCast(context.count.*)] = .{
        .id = stableNumericId(&edge_id),
        .project = project,
        .source_id = stableNumericId(context.source),
        .target_id = stableNumericId(context.target),
        .type = edge_type,
        .properties_json = properties,
    };
    context.count.* += 1;
    return 0;
}

pub export fn cbm_zova_repository_call_neighbor_keys(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_stable_id: ?Str, inbound: bool, maybe_out: ?*[*c]i64, maybe_out_count: ?*c_int) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const stable_id = maybe_stable_id orelse return ERR;
    const out = maybe_out orelse return ERR;
    const out_count = maybe_out_count orelse return ERR;
    out.* = null;
    out_count.* = 0;
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (!repoGraphName(repo, &graph_name)) return ERR;
    var neighbors = std.mem.zeroes(KeyedNeighbors);
    defer c.zova_graph_keyed_neighbor_results_free(&neighbors);
    if (c.zova_graph_neighbors_keyed(&c.zova_graph_neighbors_keyed_request{
        .db = repo.db,
        .graph_name = &graph_name,
        .node_id = stable_id,
        .direction = if (inbound) c.ZOVA_GRAPH_NEIGHBOR_INCOMING else c.ZOVA_GRAPH_NEIGHBOR_OUTGOING,
        .edge_type = null,
        .limit = NEIGHBOR_LIMIT,
        .out_results = &neighbors,
    }) != c.ZOVA_OK) return ERR;
    var keys: [*c]i64 = null;
    if (neighbors.len != 0) {
        keys = interop.callocN(i64, neighbors.len);
        if (keys == null) return ERR;
    }
    var count: usize = 0;
    for (many(neighbors.items, neighbors.len)) |*neighbor| {
        const edge_type = many(neighbor.edge_type, neighbor.edge_type_len);
        if (std.mem.eql(u8, edge_type, "CALLS") or std.mem.eql(u8, edge_type, "HTTP_CALLS") or
            std.mem.eql(u8, edge_type, "ASYNC_CALLS"))
        {
            keys[count] = neighbor.neighbor_node_key;
            count += 1;
        }
    }
    out.* = keys;
    out_count.* = @intCast(count);
    return OK;
}

/// Appends the edges between `stable_id` and its neighbors in one direction.
/// `skip_self_loops` drops the loops the outgoing pass already produced.
fn repoAppendNeighborEdges(repo: *Repository, graph_name: Str, stable_id: Str, direction: c_int, skip_self_loops: bool, edges: *[*c]Edge, count: *c_int, capacity: *c_int) bool {
    var neighbors = std.mem.zeroes(KeyedNeighbors);
    defer c.zova_graph_keyed_neighbor_results_free(&neighbors);
    if (c.zova_graph_neighbors_keyed(&c.zova_graph_neighbors_keyed_request{
        .db = repo.db,
        .graph_name = graph_name,
        .node_id = stable_id,
        .direction = direction,
        .edge_type = null,
        .limit = NEIGHBOR_LIMIT,
        .out_results = &neighbors,
    }) != c.ZOVA_OK) return false;
    const items = many(neighbors.items, neighbors.len);
    var payloads = std.mem.zeroes(EdgePayloads);
    defer c.zova_graph_edge_payload_results_free(&payloads);
    if (items.len != 0) {
        const edge_keys = interop.callocN(i64, items.len);
        if (edge_keys == null) return false;
        defer c.free(edge_keys);
        for (items, edge_keys[0..items.len]) |*item, *key| key.* = item.edge_key;
        if (c.zova_graph_edge_payload_get_many(&c.zova_graph_edge_payload_get_many_request{
            .db = repo.db,
            .graph_name = graph_name,
            .edge_keys = edge_keys,
            .key_count = items.len,
            .out_results = &payloads,
        }) != c.ZOVA_OK) return false;
    }
    if (payloads.len != items.len) return false;
    const outgoing = direction == c.ZOVA_GRAPH_NEIGHBOR_OUTGOING;
    for (items, many(payloads.items, items.len)) |*neighbor, *payload| {
        if (skip_self_loops and c.strcmp(neighbor.node_id, stable_id) == 0) continue;
        if (payload.found == 0 or payload.edge_key != neighbor.edge_key) return false;
        var context = PayloadEdgeContext{
            .repo = repo,
            .source = if (outgoing) stable_id else neighbor.node_id,
            .target = if (outgoing) neighbor.node_id else stable_id,
            .edge_type = neighbor.edge_type,
            .edge_type_len = neighbor.edge_type_len,
            .edges = edges,
            .count = count,
            .capacity = capacity,
        };
        if (c.cbm_zova_edge_payload_visit(payload.payload, payload.payload_len, repoAppendPayloadEdge, &context, null) != 0) return false;
    }
    return true;
}

pub export fn cbm_zova_repository_find_edges(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_stable_id: ?Str, direction: ?Str, maybe_out: ?*[*c]Edge, maybe_out_count: ?*c_int) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const stable_id = maybe_stable_id orelse return ERR;
    const out = maybe_out orelse return ERR;
    const out_count = maybe_out_count orelse return ERR;
    out.* = null;
    out_count.* = 0;
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (!repoGraphName(repo, &graph_name)) return ERR;
    const both = direction == null or c.strcmp(direction, "any") == 0;
    const directions: []const c_int = if (both)
        &.{ c.ZOVA_GRAPH_NEIGHBOR_OUTGOING, c.ZOVA_GRAPH_NEIGHBOR_INCOMING }
    else if (c.strcmp(direction, "inbound") == 0)
        &.{c.ZOVA_GRAPH_NEIGHBOR_INCOMING}
    else
        &.{c.ZOVA_GRAPH_NEIGHBOR_OUTGOING};
    var cap: c_int = 8;
    var count: c_int = 0;
    var edges: [*c]Edge = interop.callocN(Edge, @intCast(cap));
    if (edges == null) return ERR;
    for (directions, 0..) |one, d| {
        if (!repoAppendNeighborEdges(repo, @ptrCast(&graph_name), stable_id, one, both and d == 1, &edges, &count, &cap)) {
            c.cbm_store_free_edges(edges, count);
            return ERR;
        }
    }
    out.* = edges;
    out_count.* = count;
    return OK;
}

const EdgeRecordVisitor = *const fn (native: *const ScanEdge, record: *const PayloadRecord, context: ?*anyopaque) c_int;

const ScanRecordContext = struct {
    native: *const ScanEdge,
    visitor: EdgeRecordVisitor,
    context: ?*anyopaque,
};

fn repoScanPayloadRecord(record: [*c]const PayloadRecord, context_ptr: ?*anyopaque) callconv(.c) c_int {
    const context: *const ScanRecordContext = @ptrCast(@alignCast(context_ptr));
    return context.visitor(context.native, @ptrCast(record), context.context);
}

fn repoEdgeTypeMatches(edge: *const ScanEdge, edge_type: ?Str) bool {
    const wanted = edge_type orelse return true;
    return edge.edge_type != null and std.mem.eql(u8, many(edge.edge_type, edge.edge_type_len), span(wanted));
}

/// Visit every cbm edge record of the workspace graph in scan order, or only
/// those of edge_type. Payloads are read only for matching graph edges.
fn repoScanEdges(repo: *Repository, edge_type: ?Str, visitor: EdgeRecordVisitor, context: ?*anyopaque) c_int {
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (!repoGraphName(repo, &graph_name)) return ERR;
    var cursor = std.mem.zeroes(ScanCursor);
    var edge_keys: [EDGE_SCAN_PAGE]i64 = undefined;
    var matched: [EDGE_SCAN_PAGE]*const ScanEdge = undefined;
    while (true) {
        var page = std.mem.zeroes(ScanResults);
        defer c.zova_graph_scan_results_free(&page);
        var payloads = std.mem.zeroes(EdgePayloads);
        defer c.zova_graph_edge_payload_results_free(&payloads);
        if (c.zova_graph_scan(&c.zova_graph_scan_request{
            .db = repo.db,
            .graph_name = &graph_name,
            .edge_after = cursor,
            .node_limit = 0,
            .edge_limit = EDGE_SCAN_PAGE,
            .out_results = &page,
        }) != c.ZOVA_OK or page.edges_len > EDGE_SCAN_PAGE) return ERR;
        const page_edges = many(page.edges, page.edges_len);
        var match_count: usize = 0;
        for (page_edges) |*edge| {
            if (!repoEdgeTypeMatches(edge, edge_type)) continue;
            matched[match_count] = edge;
            edge_keys[match_count] = edge.edge_key;
            match_count += 1;
        }
        if (match_count != 0 and (c.zova_graph_edge_payload_get_many(&c.zova_graph_edge_payload_get_many_request{
            .db = repo.db,
            .graph_name = &graph_name,
            .edge_keys = &edge_keys,
            .key_count = match_count,
            .out_results = &payloads,
        }) != c.ZOVA_OK or payloads.len != match_count)) return ERR;
        for (matched[0..match_count], many(payloads.items, match_count)) |native, *payload| {
            var edge = ScanRecordContext{ .native = native, .visitor = visitor, .context = context };
            if (payload.found == 0 or payload.edge_key != native.edge_key or
                c.cbm_zova_edge_payload_visit(payload.payload, payload.payload_len, repoScanPayloadRecord, &edge, null) != 0)
                return ERR;
        }
        if (page_edges.len != 0) {
            const last = &page_edges[page_edges.len - 1];
            cursor = .{ .created_order = last.created_order, .key = last.edge_key };
        }
        if (page.has_more_edges == 0) return OK;
    }
}

const EdgeVisitor = @typeInfo(c.cbm_zova_repository_edge_visitor_t).optional.child;

const VisitEdgeContext = struct {
    visitor: EdgeVisitor,
    context: ?*anyopaque,
};

fn repoVisitEdgeRecord(native: *const ScanEdge, record: *const PayloadRecord, context_ptr: ?*anyopaque) c_int {
    const context: *const VisitEdgeContext = @ptrCast(@alignCast(context_ptr));
    return context.visitor(native.source_node_key, native.target_node_key, native.edge_type, native.edge_type_len, record.properties.data, record.properties.len, context.context);
}

pub export fn cbm_zova_repository_visit_edges(maybe_repo: ?*Repository, workspace_id: ?Str, edge_type: ?Str, visitor: c.cbm_zova_repository_edge_visitor_t, context: ?*anyopaque) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    var visit = VisitEdgeContext{ .visitor = visitor orelse return ERR, .context = context };
    return repoScanEdges(repo, edge_type, repoVisitEdgeRecord, &visit);
}

const TypedEdge = extern struct {
    source_key: i64,
    target_key: i64,
    local_name: [*c]u8,
    properties: [*c]u8,
};

const TypedEdges = struct {
    items: [*c]TypedEdge = null,
    count: c_int = 0,
    capacity: c_int = 0,
};

fn repoCollectTypedEdge(native: *const ScanEdge, record: *const PayloadRecord, context_ptr: ?*anyopaque) c_int {
    const edges: *TypedEdges = @ptrCast(@alignCast(context_ptr));
    if (edges.count == edges.capacity) {
        const capacity: c_int = if (edges.capacity != 0) edges.capacity * 2 else 64;
        const grown = interop.reallocN(TypedEdge, edges.items, @intCast(capacity));
        if (grown == null) return -1;
        edges.items = grown;
        edges.capacity = capacity;
    }
    const local_name = repoDup(record.local_name.data, record.local_name.len);
    const properties = repoDup(record.properties.data, record.properties.len);
    if (local_name == null or properties == null) {
        c.free(local_name);
        c.free(properties);
        return -1;
    }
    edges.items[@intCast(edges.count)] = .{
        .source_key = native.source_node_key,
        .target_key = native.target_node_key,
        .local_name = local_name,
        .properties = properties,
    };
    edges.count += 1;
    return 0;
}

fn repoInt64Compare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left = cast(i64, left_ptr).*;
    const right = cast(i64, right_ptr).*;
    return @as(c_int, @intFromBool(left > right)) - @intFromBool(left < right);
}

/// The graph node read for `key` (keys sorted, natives parallel to them);
/// null unless it was found and has an id.
fn repoKeyedNodeFor(keys: []const i64, natives: []const KeyedNode, key: i64) ?*const KeyedNode {
    const hit = c.bsearch(&key, keys.ptr, keys.len, @sizeOf(i64), repoInt64Compare) orelse return null;
    const item = &natives[(@intFromPtr(hit) - @intFromPtr(keys.ptr)) / @sizeOf(i64)];
    return if (item.node_id != null and item.found != 0) item else null;
}

/// Builds `edges[i]` from `items[i]`, taking over its properties. `key_buffer`
/// is scratch for two keys per item.
fn repoResolveTypedEdges(repo: *Repository, edge_type: Str, items: []TypedEdge, key_buffer: []i64, edges: []Edge) bool {
    for (items, 0..) |*item, i| {
        key_buffer[i * 2] = item.source_key;
        key_buffer[i * 2 + 1] = item.target_key;
    }
    c.qsort(key_buffer.ptr, key_buffer.len, @sizeOf(i64), repoInt64Compare);
    var unique: usize = 0;
    for (key_buffer) |key| {
        if (unique == 0 or key_buffer[unique - 1] != key) {
            key_buffer[unique] = key;
            unique += 1;
        }
    }
    const keys = key_buffer[0..unique];
    var native = std.mem.zeroes(KeyedNodes);
    defer c.zova_graph_keyed_node_results_free(&native);
    if (!repoNativeNodesByKey(repo, keys.ptr, keys.len, &native)) return false;
    const natives = many(native.items, keys.len);
    for (items, edges) |*item, *edge| {
        const source = repoKeyedNodeFor(keys, natives, item.source_key) orelse return false;
        const target = repoKeyedNodeFor(keys, natives, item.target_key) orelse return false;
        var edge_id: [ID_MAX + 80]u8 = undefined;
        if (!repoEdgeId(repo.workspaceId(), source.node_id, edge_type, target.node_id, item.local_name, &edge_id)) return false;
        edge.* = .{
            .id = stableNumericId(&edge_id),
            .project = repoProjectDup(repo),
            .source_id = stableNumericId(source.node_id),
            .target_id = stableNumericId(target.node_id),
            .type = repoDup(edge_type, c.strlen(edge_type)),
            .properties_json = item.properties,
        };
        item.properties = null;
        if (edge.project == null or edge.type == null) return false;
    }
    return true;
}

/// Every cbm edge of one type, in scan order, with the ids the other reads
/// use: endpoints and edge ids derive from stable ids, as in find_edges.
pub export fn cbm_zova_repository_find_edges_by_type(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_edge_type: ?Str, maybe_out: ?*[*c]Edge, maybe_out_count: ?*c_int) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const edge_type = maybe_edge_type orelse return ERR;
    const out = maybe_out orelse return ERR;
    const out_count = maybe_out_count orelse return ERR;
    out.* = null;
    out_count.* = 0;
    var found = TypedEdges{};
    defer {
        for (many(found.items, found.count)) |*item| {
            c.free(item.local_name);
            c.free(item.properties);
        }
        c.free(found.items);
    }
    if (repoScanEdges(repo, edge_type, repoCollectTypedEdge, &found) != OK) return ERR;
    const items = many(found.items, found.count);
    if (items.len == 0) return OK;
    const keys = interop.mallocN(i64, items.len * 2);
    defer c.free(keys);
    const edges = interop.callocN(Edge, items.len);
    if (keys == null or edges == null or
        !repoResolveTypedEdges(repo, edge_type, items, keys[0 .. items.len * 2], edges[0..items.len]))
    {
        c.cbm_store_free_edges(edges, found.count);
        return ERR;
    }
    out.* = edges;
    out_count.* = found.count;
    return OK;
}

// ── Workspace reads: graph identities ───────────────────────────

const IdentityEdges = struct {
    items: [*c]IdentityEdge = null,
    count: c_int = 0,
    capacity: c_int = 0,
    max_edges: u64,
    over_limit: bool = false,
};

fn repoCollectIdentityEdge(native: *const ScanEdge, record: *const PayloadRecord, context_ptr: ?*anyopaque) c_int {
    const edges: *IdentityEdges = @ptrCast(@alignCast(context_ptr));
    if (@as(u64, @intCast(edges.count)) >= edges.max_edges) {
        edges.over_limit = true;
        return -1;
    }
    if (edges.count == edges.capacity) {
        const capacity: c_int = if (edges.capacity != 0) edges.capacity * 2 else 64;
        const grown = interop.reallocN(IdentityEdge, edges.items, @intCast(capacity));
        if (grown == null) return -1;
        edges.items = grown;
        edges.capacity = capacity;
    }
    const edge_type = repoDup(native.edge_type, native.edge_type_len);
    const local_name = repoDup(record.local_name.data, record.local_name.len);
    if (edge_type == null or local_name == null) {
        c.free(edge_type);
        c.free(local_name);
        return -1;
    }
    edges.items[@intCast(edges.count)] = .{
        .source_key = native.source_node_key,
        .target_key = native.target_node_key,
        .type = edge_type,
        .local_name = local_name,
    };
    edges.count += 1;
    return 0;
}

/// Reads the identity of every node of the workspace, at most `max_nodes`.
/// The caller owns `nodes`/`count` whatever the result.
fn repoIdentityNodes(repo: *Repository, max_nodes: u64, nodes: *[*c]IdentityNode, count: *c_int) c_int {
    const statement = repoPrepare(repo.db, "SELECT n.zova_node_key,n.qualified_name,n.label,f.file_path " ++
        "FROM cbm_nodes_v1 n JOIN cbm_files_v1 f ON f.file_key=n.file_key " ++
        "WHERE n.workspace_key=?1 ORDER BY n.zova_node_key") orelse return ERR;
    defer repoFinalize(statement);
    if (!repoBindI64(statement, 1, repo.workspace_key)) return ERR;
    var capacity: c_int = 0;
    while (true) {
        var step: c.zova_step_result = c.ZOVA_STEP_DONE;
        if (!repoStep(statement, &step)) return ERR;
        if (step != c.ZOVA_STEP_ROW) return OK;
        if (@as(u64, @intCast(count.*)) >= max_nodes) return SCAN_LIMIT;
        if (count.* == capacity) {
            const grown_capacity: c_int = if (capacity != 0) capacity * 2 else 256;
            const grown = interop.reallocN(IdentityNode, nodes.*, @intCast(grown_capacity));
            if (grown == null) return ERR;
            nodes.* = grown;
            capacity = grown_capacity;
        }
        const node = &many(nodes.*, capacity)[@intCast(count.*)];
        node.node_key = repoColumnI64(statement, 0);
        node.qualified_name = repoColumnText(statement, 1);
        node.label = repoColumnText(statement, 2);
        node.file_path = repoColumnText(statement, 3);
        count.* += 1;
        if (node.qualified_name == null or node.label == null or node.file_path == null) return ERR;
    }
}

pub export fn cbm_zova_repository_graph_identities(maybe_repo: ?*Repository, workspace_id: ?Str, max_nodes: u64, max_edges: u64, maybe_out_nodes: ?*[*c]IdentityNode, maybe_node_count: ?*c_int, maybe_out_edges: ?*[*c]IdentityEdge, maybe_edge_count: ?*c_int) c_int {
    if (!with_zova) {
        if (maybe_out_nodes) |out_nodes| out_nodes.* = null;
        if (maybe_node_count) |node_count| node_count.* = 0;
        if (maybe_out_edges) |out_edges| out_edges.* = null;
        if (maybe_edge_count) |edge_count| edge_count.* = 0;
        return ERR;
    }
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const out_nodes = maybe_out_nodes orelse return ERR;
    const node_count = maybe_node_count orelse return ERR;
    const out_edges = maybe_out_edges orelse return ERR;
    const edge_count = maybe_edge_count orelse return ERR;
    out_nodes.* = null;
    node_count.* = 0;
    out_edges.* = null;
    edge_count.* = 0;
    var nodes: [*c]IdentityNode = null;
    var count: c_int = 0;
    var edges = IdentityEdges{ .max_edges = max_edges };
    var rc = repoIdentityNodes(repo, max_nodes, &nodes, &count);
    if (rc == OK) {
        rc = repoScanEdges(repo, null, repoCollectIdentityEdge, &edges);
        if (edges.over_limit) {
            rc = SCAN_LIMIT;
        } else if (rc != OK) {
            rc = ERR;
        }
    }
    if (rc != OK) {
        cbm_zova_identities_free(nodes, count, edges.items, edges.count);
        return rc;
    }
    out_nodes.* = nodes;
    node_count.* = count;
    out_edges.* = edges.items;
    edge_count.* = edges.count;
    return OK;
}

pub export fn cbm_zova_identities_free(nodes: [*c]IdentityNode, node_count: c_int, edges: [*c]IdentityEdge, edge_count: c_int) void {
    if (with_zova) {
        for (many(nodes, node_count)) |*node| {
            c.free(node.qualified_name);
            c.free(node.label);
            c.free(node.file_path);
        }
        for (many(edges, edge_count)) |*edge| {
            c.free(edge.type);
            c.free(edge.local_name);
        }
    }
    c.free(nodes);
    c.free(edges);
}

// ── Workspace reads: search ─────────────────────────────────────

/// Search rows read from SQL with the Zova key of each, before the graph
/// read that completes them.
const SearchRows = struct {
    results: [*c]SearchResult = null,
    keys: [*c]i64 = null,
    count: c_int = 0,

    fn freeResults(rows: *SearchRows) void {
        var owned = SearchOutput{ .results = rows.results, .count = rows.count, .total = 0 };
        c.cbm_store_search_free(&owned);
    }
};

/// Collects the rows of a statement that selects the node key, label, name,
/// qualified name, file path, start line, end line and properties, then the
/// rank when `rank_column` is given.
fn repoReadSearchRows(repo: *const Repository, statement: ?*Stmt, initial_capacity: usize, rank_column: ?c_int, rows: *SearchRows) Error!void {
    var cap = initial_capacity;
    rows.results = interop.callocN(SearchResult, cap);
    rows.keys = interop.callocN(i64, cap);
    if (rows.results == null or rows.keys == null) return error.Failed;
    while (try repoNextRow(statement)) {
        const index: usize = @intCast(rows.count);
        if (index == cap) {
            cap *= 2;
            const grown_results = interop.reallocN(SearchResult, rows.results, cap);
            if (grown_results != null) rows.results = grown_results;
            const grown_keys = interop.reallocN(i64, rows.keys, cap);
            if (grown_keys != null) rows.keys = grown_keys;
            if (grown_results == null or grown_keys == null) return error.Failed;
            @memset(grown_results[index..cap], std.mem.zeroes(SearchResult));
        }
        rows.keys[index] = repoColumnI64(statement, 0);
        const result = &many(rows.results, cap)[index];
        const node = &result.node;
        node.project = repoProjectDup(repo);
        node.label = repoColumnText(statement, 1);
        node.name = repoColumnText(statement, 2);
        node.qualified_name = repoColumnText(statement, 3);
        node.file_path = repoColumnText(statement, 4);
        node.start_line = repoColumnInt(statement, 5);
        node.end_line = repoColumnInt(statement, 6);
        node.properties_json = repoColumnText(statement, 7);
        if (rank_column) |column| result.rank = repoColumnDouble(statement, column);
        rows.count += 1;
    }
}

fn repoSearchFtsRead(repo: *Repository, query: Str, filter: FtsFilter, limit: c_int, offset: c_int, rows: *SearchRows) Error!void {
    {
        const sql = "SELECT n.zova_node_key,n.label,n.name,n.qualified_name,p.file_path," ++
            "n.start_line,n.end_line,n.properties,(bm25(cbm_nodes_fts_v1)-CASE " ++
            "WHEN n.label IN ('Function','Method') THEN 10.0 WHEN n.label='Route' THEN 8.0 " ++
            "WHEN n.label IN ('Class','Interface','Type','Enum') THEN 5.0 ELSE 0.0 END" ++
            "-CASE WHEN ?7 IS NOT NULL AND n.name=?7 COLLATE NOCASE THEN 20.0 ELSE 0.0 END) AS rank " ++
            "FROM cbm_nodes_fts_v1 f " ++
            "JOIN cbm_nodes_v1 n ON n.zova_node_key=f.rowid " ++
            "JOIN cbm_files_v1 p ON p.file_key=n.file_key " ++
            "WHERE n.workspace_key=?1 AND cbm_nodes_fts_v1 MATCH ?2 " ++
            "AND n.label NOT IN ('File','Folder','Module','Section','Variable','Project') " ++
            "AND (?3 IS NULL OR p.file_path GLOB ?3) " ++
            "AND (?6 IS NULL OR n.label=?6) " ++
            "ORDER BY rank,n.qualified_name,n.zova_node_key LIMIT ?4 OFFSET ?5";
        const statement = repoPrepare(repo.db, sql) orelse return error.Failed;
        defer repoFinalize(statement);
        if (!repoBindI64(statement, 1, repo.workspace_key) or !repoBindText(statement, 2, query)) return error.Failed;
        if (!repoBindOptionalText(statement, 3, filter.file_pattern) or
            !repoBindI64(statement, 4, limit) or !repoBindI64(statement, 5, offset) or
            !repoBindOptionalText(statement, 6, filter.label) or
            !repoBindOptionalText(statement, 7, filter.exact_name)) return error.Failed;
        try repoReadSearchRows(repo, statement, 8, 8, rows);
    }
    const count: usize = @intCast(rows.count);
    var native = std.mem.zeroes(KeyedNodes);
    defer c.zova_graph_keyed_node_results_free(&native);
    if (!repoNativeNodesByKey(repo, rows.keys, count, &native)) return error.Failed;
    for (many(rows.results, count), many(native.items, count)) |*result, *item| {
        if (item.found == 0) return error.Failed;
        result.node.id = stableNumericId(item.node_id);
        freeConst(result.node.label);
        result.node.label = repoDup(item.kind, item.kind_len);
    }
}

pub export fn cbm_zova_repository_search_fts(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_query: ?Str, file_pattern: ?Str, label: ?Str, exact_name: ?Str, limit: c_int, offset: c_int, maybe_out: ?*SearchOutput) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const query = nonEmpty(maybe_query) orelse return ERR;
    const out = maybe_out orelse return ERR;
    out.* = std.mem.zeroes(SearchOutput);
    var rows = SearchRows{};
    defer c.free(rows.keys);
    const filter = FtsFilter{ .file_pattern = file_pattern, .label = label, .exact_name = exact_name };
    repoSearchFtsRead(repo, query, filter, if (limit <= 0) 10 else limit, offset, &rows) catch {
        rows.freeResults();
        return ERR;
    };
    out.results = rows.results;
    out.count = rows.count;
    out.total = rows.count;
    return OK;
}

/// A fixed SQL buffer filled the way strncat() fills one: a clause that
/// does not fit is cut, and the text stays NUL-terminated.
const SqlBuffer = struct {
    text: [4096]u8 = undefined,
    len: usize = 0,

    fn append(sql: *SqlBuffer, clause: []const u8) void {
        const n = @min(clause.len, sql.text.len - sql.len - 1);
        @memcpy(sql.text[sql.len..][0..n], clause[0..n]);
        sql.len += n;
        sql.text[sql.len] = 0;
    }
};

/// Reads the degrees of `keys` for one edge type and direction into `tmp`,
/// adds them to `sum`, and copies them to `copy` when given.
fn repoAddDegrees(repo: *const Repository, keys: [*c]const i64, direction: c_int, edge_type: CStr, tmp: []u64, sum: []u64, copy: ?[]u64) bool {
    @memset(tmp, 0);
    if (!repoDegreeMany(repo, keys, tmp.len, direction, edge_type, tmp.ptr)) return false;
    for (sum, tmp) |*total, degree| total.* +%= degree;
    if (copy) |first| @memcpy(first, tmp);
    return true;
}

/// Runs the structured search: reads every matching row into `rows`, then
/// moves the rows that pass the degree filters and fall in the requested
/// page into `page`. The caller owns both whatever the result.
fn repoSearchRead(repo: *Repository, params: *const SearchParams, rows: *SearchRows, page: *SearchOutput) Error!void {
    var sql = SqlBuffer{};
    sql.append("SELECT n.zova_node_key,n.label,n.name,n.qualified_name,f.file_path," ++
        "n.start_line,n.end_line,n.properties FROM cbm_nodes_v1 n " ++
        "JOIN cbm_files_v1 f ON f.file_key=n.file_key WHERE n.workspace_key=?1");
    var binds: [64]CStr = undefined;
    var bind_count: usize = 0;
    const filters = [_]struct { clause: []const u8, value: CStr }{
        .{ .clause = " AND n.label=?", .value = params.label },
        .{ .clause = if (params.case_sensitive) " AND regexp(?,n.name)" else " AND iregexp(?,n.name)", .value = params.name_pattern },
        .{ .clause = if (params.case_sensitive) " AND regexp(?,n.qualified_name)" else " AND iregexp(?,n.qualified_name)", .value = params.qn_pattern },
        .{ .clause = " AND f.file_path GLOB ?", .value = params.file_pattern },
    };
    for (filters) |filter| {
        if (filter.value == null or filter.value[0] == 0) continue;
        sql.append(filter.clause);
        binds[bind_count] = filter.value;
        bind_count += 1;
    }
    if (params.exclude_labels != null) {
        var i: usize = 0;
        while (params.exclude_labels[i] != null and bind_count < 63) : (i += 1) {
            sql.append(" AND n.label<>?");
            binds[bind_count] = params.exclude_labels[i];
            bind_count += 1;
        }
    }
    sql.append(" ORDER BY n.name,n.qualified_name,n.zova_node_key");
    {
        const statement = repoPrepare(repo.db, @ptrCast(&sql.text)) orelse return error.Failed;
        defer repoFinalize(statement);
        if (!repoBindI64(statement, 1, repo.workspace_key)) return error.Failed;
        for (binds[0..bind_count], 2..) |value, index| {
            if (!repoBindText(statement, @intCast(index), value)) return error.Failed;
        }
        try repoReadSearchRows(repo, statement, 64, null, rows);
    }

    const count: usize = @intCast(rows.count);
    var native = std.mem.zeroes(KeyedNodes);
    defer c.zova_graph_keyed_node_results_free(&native);
    // Seven arrays of `count` degrees in one block.
    const block = interop.callocN(u64, count * 7);
    defer c.free(block);
    if (block == null) return error.Failed;
    const degrees = block[0 .. count * 7];
    const in = degrees[0..count];
    const outd = degrees[count..][0..count];
    const tmp = degrees[count * 2 ..][0..count];
    const calls_in = degrees[count * 3 ..][0..count];
    const calls_out = degrees[count * 4 ..][0..count];
    const rel_in = degrees[count * 5 ..][0..count];
    const rel_out = degrees[count * 6 ..][0..count];
    if (!repoNativeNodesByKey(repo, rows.keys, count, &native)) return error.Failed;
    for ([_]Str{ "CALLS", "USAGE", "CALL_REFERENCE", "INHERITS", "IMPLEMENTS" }, 0..) |degree_type, t| {
        if (!repoAddDegrees(repo, rows.keys, c.ZOVA_GRAPH_NEIGHBOR_INCOMING, degree_type, tmp, in, if (t == 0) calls_in else null) or
            !repoAddDegrees(repo, rows.keys, c.ZOVA_GRAPH_NEIGHBOR_OUTGOING, degree_type, tmp, outd, if (t == 0) calls_out else null))
            return error.Failed;
    }
    if (params.relationship != null) {
        if (!repoDegreeMany(repo, rows.keys, count, c.ZOVA_GRAPH_NEIGHBOR_INCOMING, params.relationship, rel_in.ptr) or
            !repoDegreeMany(repo, rows.keys, count, c.ZOVA_GRAPH_NEIGHBOR_OUTGOING, params.relationship, rel_out.ptr))
            return error.Failed;
    }
    const limit: c_int = if (params.limit > 0) params.limit else 10;
    const page_capacity: usize = @intCast(@min(limit, rows.count));
    page.results = interop.callocN(SearchResult, page_capacity);
    if (page_capacity != 0 and page.results == null) return error.Failed;
    var accepted: c_int = 0;
    for (many(rows.results, count), many(native.items, count), 0..) |*result, *item, i| {
        if (item.found == 0) return error.Failed;
        const degree = narrowInt(in[i] +% outd[i]);
        var keep = true;
        if (params.relationship != null) {
            keep = if (params.direction == null or c.strcmp(params.direction, "any") == 0)
                rel_in[i] +% rel_out[i] > 0
            else if (c.strcmp(params.direction, "inbound") == 0)
                rel_in[i] > 0
            else
                rel_out[i] > 0;
        }
        if (keep and params.exclude_entry_points and calls_in[i] == 0 and calls_out[i] > 0) keep = false;
        if (keep and params.min_degree >= 0 and degree < params.min_degree) keep = false;
        if (keep and params.max_degree >= 0 and degree > params.max_degree) keep = false;
        if (!keep) continue;
        if (accepted >= params.offset and page.count < limit) {
            result.node.id = stableNumericId(item.node_id);
            freeConst(result.node.label);
            result.node.label = repoDup(item.kind, item.kind_len);
            result.in_degree = narrowInt(in[i]);
            result.out_degree = narrowInt(outd[i]);
            page.results[@intCast(page.count)] = result.*;
            page.count += 1;
            result.* = std.mem.zeroes(SearchResult);
        }
        accepted += 1;
    }
    page.total = accepted;
}

pub export fn cbm_zova_repository_search(maybe_repo: ?*Repository, workspace_id: ?Str, maybe_params: ?*const SearchParams, maybe_out: ?*SearchOutput) c_int {
    if (!with_zova) return ERR;
    const repo = repoWorkspaceRequired(maybe_repo, workspace_id) orelse return ERR;
    const params = maybe_params orelse return ERR;
    const out = maybe_out orelse return ERR;
    out.* = std.mem.zeroes(SearchOutput);
    var rows = SearchRows{};
    defer {
        c.free(rows.keys);
        rows.freeResults();
    }
    var page = std.mem.zeroes(SearchOutput);
    repoSearchRead(repo, params, &rows, &page) catch {
        c.cbm_store_search_free(&page);
        return ERR;
    };
    out.* = page;
    return OK;
}

// ── Workspace snapshots ─────────────────────────────────────────

const DumpNode = c.CBMDumpNode;
const DumpEdge = c.CBMDumpEdge;
const VectorInfo = c.zova_vector_collection_info;
const Digest = [c.CBM_ZOVA_DIGEST_HEX_SIZE]u8;

const COMPONENT_NONE: Components = c.CBM_ZOVA_SNAPSHOT_COMPONENT_NONE;
const COMPONENT_TOPOLOGY: Components = c.CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY;
const COMPONENT_NODE_VECTORS: Components = c.CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS;
const COMPONENT_TOKEN_VECTORS: Components = c.CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS;
const COMPONENT_ALL: Components = c.CBM_ZOVA_SNAPSHOT_COMPONENT_ALL;

const PHASE_OPEN: u32 = c.CBM_ZOVA_SNAPSHOT_BASE_PHASE_OPEN;
const PHASE_HEADER: u32 = c.CBM_ZOVA_SNAPSHOT_BASE_PHASE_HEADER;
const PHASE_INTEGRITY: u32 = c.CBM_ZOVA_SNAPSHOT_BASE_PHASE_INTEGRITY;
const PHASE_NODES: u32 = c.CBM_ZOVA_SNAPSHOT_BASE_PHASE_NODES;
const PHASE_EDGES: u32 = c.CBM_ZOVA_SNAPSHOT_BASE_PHASE_EDGES;
const PHASE_HASHES_SUMMARY: u32 = c.CBM_ZOVA_SNAPSHOT_BASE_PHASE_HASHES_SUMMARY;
const PHASE_CLOSE: u32 = c.CBM_ZOVA_SNAPSHOT_BASE_PHASE_CLOSE;

/// A zeroed array of `count` items; NULL for an empty one and on OOM.
fn snapshotAlloc(comptime T: type, count: usize) [*c]T {
    return if (count != 0) interop.callocN(T, count) else null;
}

/// Frees an array of `count` malloc'd strings and the array itself.
fn freeStrings(strings: [*c][*c]u8, count: c_int) void {
    for (many(strings, count)) |string| c.free(string);
    c.free(@ptrCast(strings));
}

fn snapshotRelease(snapshot: *Snapshot) void {
    c.free(snapshot.root_path);
    c.free(snapshot.project);
    c.free(snapshot.indexed_at);
    c.free(snapshot.model_fingerprint);
    freeStrings(snapshot.node_stable_ids, snapshot.node_count);
    for (many(snapshot.nodes, snapshot.node_count)) |*node| {
        freeConst(node.project);
        freeConst(node.label);
        freeConst(node.name);
        freeConst(node.qualified_name);
        freeConst(node.file_path);
        freeConst(node.properties);
    }
    c.free(snapshot.zova_node_keys);
    c.free(snapshot.node_source_ordinals);
    c.free(snapshot.nodes);
    freeStrings(snapshot.edge_ids, snapshot.edge_count);
    for (many(snapshot.edges, snapshot.edge_count)) |*edge| {
        // Edges read from the database share the snapshot's project string
        // and keep their other strings in the arena.
        if (edge.project != snapshot.project) freeConst(edge.project);
        if (snapshot.edge_string_arena == null) {
            freeConst(edge.type);
            freeConst(edge.properties);
            freeConst(edge.url_path);
            freeConst(edge.local_name);
        }
    }
    c.free(snapshot.edges);
    repoStringArenaFree(@ptrCast(@alignCast(snapshot.edge_string_arena)));
    for (many(snapshot.topology_edges, snapshot.topology_edge_count)) |*edge| {
        c.free(edge.source_stable_id);
        c.free(edge.edge_type);
        c.free(edge.target_stable_id);
    }
    c.free(snapshot.topology_edges);
    freeStrings(snapshot.node_vector_ids, snapshot.node_vector_count);
    for (many(snapshot.node_vectors, snapshot.node_vector_count)) |*vector| {
        freeConst(vector.project);
        freeConst(vector.vector);
    }
    c.free(snapshot.node_vectors);
    freeStrings(snapshot.token_vector_ids, snapshot.token_vector_count);
    for (many(snapshot.token_vectors, snapshot.token_vector_count)) |*vector| {
        freeConst(vector.project);
        freeConst(vector.token);
        freeConst(vector.vector);
    }
    c.free(snapshot.token_vectors);
    for (many(snapshot.file_hashes, snapshot.file_hash_count)) |*hash| {
        freeConst(hash.file_path);
        freeConst(hash.content_hash);
    }
    c.free(snapshot.file_hashes);
    freeConst(snapshot.project_summary.summary);
    freeConst(snapshot.project_summary.source_hash);
    freeConst(snapshot.project_summary.created_at);
    freeConst(snapshot.project_summary.updated_at);
}

pub export fn cbm_zova_workspace_snapshot_free(maybe_snapshot: ?*Snapshot) void {
    const snapshot = maybe_snapshot orelse return;
    if (with_zova) snapshotRelease(snapshot);
    snapshot.* = std.mem.zeroes(Snapshot);
}

/// The single count a `SELECT count(*) ... WHERE workspace_key=?1` returns;
/// -1 on failure.
fn snapshotCount(repo: *const Repository, sql: Str) c_int {
    const statement = repoPrepare(repo.db, sql) orelse return -1;
    defer repoFinalize(statement);
    if (!repoBindI64(statement, 1, repo.workspace_key)) return -1;
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    return if (repoStep(statement, &step) and step == c.ZOVA_STEP_ROW) repoColumnInt(statement, 0) else -1;
}

/// Maps a Zova node key to the 1-based id the node has in the snapshot.
const SnapshotNodeKeyRef = extern struct {
    node_key: i64,
    snapshot_id: i64,
};

fn snapshotNodeKeyRefCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left = cast(SnapshotNodeKeyRef, left_ptr).node_key;
    const right = cast(SnapshotNodeKeyRef, right_ptr).node_key;
    return @as(c_int, @intFromBool(left > right)) - @intFromBool(left < right);
}

/// The snapshot id of `node_key` in refs sorted by key; -1 when absent.
fn snapshotMapNodeKey(refs: []const SnapshotNodeKeyRef, node_key: i64) i64 {
    var low: usize = 0;
    var high = refs.len;
    while (low < high) {
        const mid = low + (high - low) / 2;
        if (refs[mid].node_key == node_key) return refs[mid].snapshot_id;
        if (refs[mid].node_key < node_key) low = mid + 1 else high = mid;
    }
    return -1;
}

const SnapshotNodeRow = extern struct {
    node_key: i64,
    stable_id: [*c]u8,
    node: DumpNode,
    source_ordinal: u64,
};

fn snapshotNodeRowCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(SnapshotNodeRow, left).stable_id, cast(SnapshotNodeRow, right).stable_id);
}

/// Reads every node of the workspace into the snapshot, ordered by stable id
/// and numbered from 1. `out_node_keys` receives the key-to-id map, sorted by
/// key, which the caller frees.
fn snapshotReadNodes(repo: *Repository, snapshot: *Snapshot, out_node_keys: *[*c]SnapshotNodeKeyRef) Error!void {
    var phase_started = repoNow();
    const signed_count = snapshotCount(repo, "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=?1");
    if (signed_count < 0) return error.Failed;
    const count: usize = @intCast(signed_count);
    snapshot.node_count = signed_count;
    snapshot.nodes = snapshotAlloc(DumpNode, count);
    const stable_ids = snapshotAlloc([*c]u8, count);
    errdefer c.free(@ptrCast(stable_ids));
    const node_keys = snapshotAlloc(SnapshotNodeKeyRef, count);
    errdefer c.free(node_keys);
    const rows_ptr = snapshotAlloc(SnapshotNodeRow, count);
    defer c.free(rows_ptr);
    const keys = snapshotAlloc(i64, count);
    defer c.free(keys);
    snapshot.node_source_ordinals = snapshotAlloc(u64, count);
    snapshot.zova_node_keys = snapshotAlloc(i64, count);
    if (count != 0 and (snapshot.nodes == null or stable_ids == null or node_keys == null or
        rows_ptr == null or keys == null or snapshot.node_source_ordinals == null or
        snapshot.zova_node_keys == null)) return error.Failed;
    const rows = many(rows_ptr, count);
    // Until the rows move into the snapshot they own their strings.
    errdefer for (rows) |*row| {
        c.free(row.stable_id);
        freeConst(row.node.project);
        freeConst(row.node.label);
        freeConst(row.node.name);
        freeConst(row.node.qualified_name);
        freeConst(row.node.file_path);
        freeConst(row.node.properties);
    };
    {
        const statement = repoPrepare(repo.db, "SELECT n.zova_node_key,n.name,n.qualified_name,f.file_path,n.start_line," ++
            "n.end_line,n.properties,n.source_ordinal FROM cbm_nodes_v1 n " ++
            "JOIN cbm_files_v1 f ON f.file_key=n.file_key " ++
            "WHERE n.workspace_key=?1 ORDER BY n.zova_node_key") orelse return error.Failed;
        defer repoFinalize(statement);
        if (!repoBindI64(statement, 1, repo.workspace_key)) return error.Failed;
        for (rows, many(keys, count)) |*row, *key| {
            var step: c.zova_step_result = c.ZOVA_STEP_DONE;
            if (!repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return error.Failed;
            const node = &row.node;
            row.node_key = repoColumnI64(statement, 0);
            key.* = row.node_key;
            node.project = repoDupZ(snapshot.project);
            node.name = repoColumnText(statement, 1);
            node.qualified_name = repoColumnText(statement, 2);
            node.file_path = repoColumnText(statement, 3);
            node.start_line = repoColumnInt(statement, 4);
            node.end_line = repoColumnInt(statement, 5);
            node.properties = repoColumnText(statement, 6);
            row.source_ordinal = @bitCast(repoColumnI64(statement, 7));
            if (node.project == null or node.name == null or node.qualified_name == null or
                node.file_path == null or node.properties == null or row.node_key <= 0) return error.Failed;
        }
    }
    snapshot.metrics.nodes_sql_ms = repoElapsedMs(phase_started);
    phase_started = repoNow();
    var native = std.mem.zeroes(KeyedNodes);
    defer c.zova_graph_keyed_node_results_free(&native);
    if (!repoNativeNodesByKey(repo, keys, count, &native)) return error.Failed;
    snapshot.metrics.nodes_native_ms = repoElapsedMs(phase_started);
    phase_started = repoNow();
    for (rows, many(native.items, count), 0..) |*row, *item, i| {
        if (item.found == 0 or item.node_key != row.node_key or item.node_id == null or item.kind == null) {
            _ = c.fprintf(interop.stderr(), "zova keyed node mismatch index=%d requested=%lld found=%u returned=%lld\n", @as(c_int, @intCast(i)), @as(c_longlong, row.node_key), @as(c_uint, item.found), @as(c_longlong, item.node_key));
            return error.Failed;
        }
        row.stable_id = repoDup(item.node_id, item.node_id_len);
        row.node.label = repoDup(item.kind, item.kind_len);
        if (row.stable_id == null or row.node.label == null) return error.Failed;
    }
    if (count > 1) c.qsort(rows.ptr, count, @sizeOf(SnapshotNodeRow), snapshotNodeRowCompare);
    for (rows, 0..) |*row, i| {
        const id: i64 = @intCast(i + 1);
        row.node.id = id;
        snapshot.nodes[i] = row.node;
        stable_ids[i] = row.stable_id;
        snapshot.zova_node_keys[i] = row.node_key;
        snapshot.node_source_ordinals[i] = row.source_ordinal;
        node_keys[i] = .{ .node_key = row.node_key, .snapshot_id = id };
    }
    if (count > 1) c.qsort(node_keys, count, @sizeOf(SnapshotNodeKeyRef), snapshotNodeKeyRefCompare);
    snapshot.node_stable_ids = stable_ids;
    out_node_keys.* = node_keys;
    snapshot.metrics.nodes_finalize_ms = repoElapsedMs(phase_started);
    snapshot.metrics.node_rows = count;
    snapshot.metrics.base_phase_mask |= PHASE_NODES;
}

const SnapshotEdgeSortRow = extern struct {
    edge: DumpEdge,
    edge_id: [*c]u8,
};

fn snapshotEdgeSortRowCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(SnapshotEdgeSortRow, left).edge_id, cast(SnapshotEdgeSortRow, right).edge_id);
}

/// Where snapshotAppendPayloadEdge puts the records of one graph edge.
const SnapshotPayloadContext = struct {
    snapshot: *Snapshot,
    arena: ?*StringArena,
    node_keys: []const SnapshotNodeKeyRef,
    native: *const ScanEdge,
    expected_count: c_int,
    written: *c_int,
};

fn snapshotAppendPayloadEdge(maybe_record: [*c]const PayloadRecord, context_ptr: ?*anyopaque) callconv(.c) c_int {
    if (maybe_record == null) return -1;
    const record: *const PayloadRecord = @ptrCast(maybe_record);
    const context: *SnapshotPayloadContext = @ptrCast(@alignCast(context_ptr orelse return -1));
    if (context.written.* >= context.expected_count) return -1;
    const source_id = snapshotMapNodeKey(context.node_keys, context.native.source_node_key);
    const target_id = snapshotMapNodeKey(context.node_keys, context.native.target_node_key);
    if (source_id <= 0 or target_id <= 0) return -1;
    const edge = &many(context.snapshot.edges, context.expected_count)[@intCast(context.written.*)];
    edge.* = .{
        .id = context.written.* + 1,
        .project = context.snapshot.project,
        .source_id = source_id,
        .target_id = target_id,
        .type = repoStringArenaDup(context.arena, context.native.edge_type, context.native.edge_type_len),
        .properties = repoStringArenaDup(context.arena, record.properties.data, record.properties.len),
        .url_path = repoStringArenaDup(context.arena, record.url_path.data, record.url_path.len),
        .local_name = repoStringArenaDup(context.arena, record.local_name.data, record.local_name.len),
    };
    if (edge.type == null or edge.properties == null or edge.url_path == null or edge.local_name == null) return -1;
    context.written.* += 1;
    return 0;
}

/// Reads every edge record of the workspace graph into snapshot.edges in
/// scan order. On failure `failure` names the stage, when the stage has a
/// name.
fn snapshotScanEdges(repo: *Repository, snapshot: *Snapshot, node_keys: []const SnapshotNodeKeyRef, written: *c_int, failure: *?Str) bool {
    var graph_name: [GRAPH_NAME_CAP]u8 = undefined;
    if (!repoGraphName(repo, &graph_name)) {
        failure.* = "graph_name";
        return false;
    }
    var cursor = std.mem.zeroes(ScanCursor);
    while (true) {
        var page = std.mem.zeroes(ScanResults);
        defer c.zova_graph_scan_results_free(&page);
        var payloads = std.mem.zeroes(EdgePayloads);
        defer c.zova_graph_edge_payload_results_free(&payloads);
        const status = c.zova_graph_scan(&c.zova_graph_scan_request{
            .db = repo.db,
            .graph_name = &graph_name,
            .edge_after = cursor,
            .node_limit = 0,
            .edge_limit = EDGE_SCAN_PAGE,
            .out_results = &page,
        });
        if (status != c.ZOVA_OK) {
            _ = c.fprintf(interop.stderr(), "zova snapshot graph scan failed status=%s\n", c.zova_status_name(status));
            failure.* = "graph_scan";
            return false;
        }
        const page_edges = many(page.edges, page.edges_len);
        if (page_edges.len != 0) {
            snapshot.metrics.edge_scan_pages += 1;
            snapshot.metrics.edge_native_rows += page_edges.len;
            const edge_keys = interop.callocN(i64, page_edges.len);
            if (edge_keys == null) return false;
            defer c.free(edge_keys);
            for (page_edges, edge_keys[0..page_edges.len]) |*edge, *key| key.* = edge.edge_key;
            if (c.zova_graph_edge_payload_get_many(&c.zova_graph_edge_payload_get_many_request{
                .db = repo.db,
                .graph_name = &graph_name,
                .edge_keys = edge_keys,
                .key_count = page_edges.len,
                .out_results = &payloads,
            }) != c.ZOVA_OK) return false;
        }
        if (payloads.len != page_edges.len) return false;
        snapshot.metrics.edge_keyed_read_count += payloads.len;
        for (page_edges, many(payloads.items, page_edges.len)) |*native, *payload| {
            if (payload.found == 0 or payload.edge_key != native.edge_key) {
                failure.* = "payload_identity";
                return false;
            }
            var context = SnapshotPayloadContext{
                .snapshot = snapshot,
                .arena = @ptrCast(@alignCast(snapshot.edge_string_arena)),
                .node_keys = node_keys,
                .native = native,
                .expected_count = snapshot.edge_count,
                .written = written,
            };
            if (c.cbm_zova_edge_payload_visit(payload.payload, payload.payload_len, snapshotAppendPayloadEdge, &context, null) != 0) {
                failure.* = "payload_decode";
                return false;
            }
        }
        if (page_edges.len != 0) {
            const last = &page_edges[page_edges.len - 1];
            cursor = .{ .created_order = last.created_order, .key = last.edge_key };
        }
        if (page.has_more_edges == 0) return true;
    }
}

/// Reads every edge of the workspace into the snapshot, ordered by edge id
/// and numbered from 1. The edge strings live in snapshot.edge_string_arena.
fn snapshotReadEdges(repo: *Repository, snapshot: *Snapshot, node_keys: []const SnapshotNodeKeyRef) Error!void {
    var phase_started = repoNow();
    if (snapshot.integrity.metadata_edges > INT_MAX) return error.Failed;
    const count: usize = @intCast(snapshot.integrity.metadata_edges);
    snapshot.edge_count = @intCast(count);
    snapshot.edges = snapshotAlloc(DumpEdge, count);
    snapshot.edge_ids = snapshotAlloc([*c]u8, count);
    const arena: ?*StringArena = snapshotAlloc(StringArena, @intFromBool(count != 0));
    snapshot.edge_string_arena = arena;
    if (count != 0 and (snapshot.edges == null or snapshot.edge_ids == null or arena == null)) return error.Failed;
    snapshot.metrics.edges_sql_ms = repoElapsedMs(phase_started);
    phase_started = repoNow();
    var written: c_int = 0;
    var failure: ?Str = null;
    var scanned = snapshotScanEdges(repo, snapshot, node_keys, &written, &failure);
    snapshot.metrics.edge_logical_rows = @intCast(written);
    if (scanned and written != snapshot.edge_count) scanned = false;
    if (!scanned) {
        if (failure) |stage| _ = c.fprintf(interop.stderr(), "zova snapshot edge read failed stage=%s\n", stage);
        return error.Failed;
    }
    snapshot.metrics.edges_native_ms = repoElapsedMs(phase_started);
    phase_started = repoNow();
    const rows_ptr = snapshotAlloc(SnapshotEdgeSortRow, count);
    if (count != 0 and rows_ptr == null) return error.Failed;
    const rows = many(rows_ptr, count);
    defer {
        for (rows) |*row| c.free(row.edge_id);
        c.free(rows_ptr);
    }
    const edges = many(snapshot.edges, count);
    for (rows, edges) |*row, *edge| {
        var edge_id: [ID_MAX + 80]u8 = undefined;
        if (!repoEdgeId(&snapshot.workspace_id, snapshot.node_stable_ids[@intCast(edge.source_id - 1)], edge.type, snapshot.node_stable_ids[@intCast(edge.target_id - 1)], edge.local_name, &edge_id)) return error.Failed;
        row.edge_id = repoDupZ(&edge_id);
        if (row.edge_id == null) return error.Failed;
        row.edge = edge.*;
    }
    if (count > 1) c.qsort(rows.ptr, count, @sizeOf(SnapshotEdgeSortRow), snapshotEdgeSortRowCompare);
    for (rows, edges, many(snapshot.edge_ids, count), 1..) |*row, *edge, *edge_id, id| {
        edge.* = row.edge;
        edge.id = @intCast(id);
        edge_id.* = row.edge_id;
        row.edge_id = null;
    }
    snapshot.metrics.edges_finalize_ms = repoElapsedMs(phase_started);
    snapshot.metrics.edge_rows = count;
    if (arena) |strings| {
        snapshot.metrics.edge_string_arena_chunks = strings.chunk_count;
        snapshot.metrics.edge_string_arena_bytes = strings.bytes_used;
    }
    snapshot.metrics.base_phase_mask |= PHASE_EDGES;
}

pub export fn cbm_zova_workspace_snapshot_format_edge_id(maybe_snapshot: ?*const Snapshot, edge_index: c_int, maybe_out: ?[*]u8, out_size: usize) c_int {
    if (!with_zova) {
        if (maybe_out) |out| {
            if (out_size != 0) out[0] = 0;
        }
        return -1;
    }
    const snapshot = maybe_snapshot orelse return -1;
    if (edge_index < 0 or edge_index >= snapshot.edge_count) return -1;
    const out = (maybe_out orelse return -1)[0..out_size];
    if (out.len == 0) return -1;
    const index: usize = @intCast(edge_index);
    if (snapshot.edge_ids != null and snapshot.edge_ids[index] != null) {
        return if (interop.bprint(out, "{s}", .{span(snapshot.edge_ids[index])}) < out.len) 0 else -1;
    }
    const edge = &many(snapshot.edges, snapshot.edge_count)[index];
    if (edge.source_id <= 0 or edge.source_id > snapshot.node_count or
        edge.target_id <= 0 or edge.target_id > snapshot.node_count or edge.type == null) return -1;
    const local_name: CStr = if (edge.local_name != null) edge.local_name else "";
    return if (repoEdgeId(&snapshot.workspace_id, snapshot.node_stable_ids[@intCast(edge.source_id - 1)], edge.type, snapshot.node_stable_ids[@intCast(edge.target_id - 1)], local_name, out)) 0 else -1;
}

const SnapshotTopologyRef = extern struct {
    source_stable_id: CStr,
    edge_type: CStr,
    target_stable_id: CStr,
};

fn snapshotTopologyRefCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left = cast(SnapshotTopologyRef, left_ptr);
    const right = cast(SnapshotTopologyRef, right_ptr);
    var order = c.strcmp(left.source_stable_id, right.source_stable_id);
    if (order == 0) order = c.strcmp(left.edge_type, right.edge_type);
    if (order == 0) order = c.strcmp(left.target_stable_id, right.target_stable_id);
    return order;
}

/// Builds the distinct (source, type, target) edges of `snapshot` into
/// `out`, which may be the same snapshot.
fn snapshotBuildTopology(snapshot: *const Snapshot, out: *Snapshot) bool {
    if (snapshot.edge_count < 0) return false;
    const count: usize = @intCast(snapshot.edge_count);
    const refs_ptr = snapshotAlloc(SnapshotTopologyRef, count);
    defer c.free(refs_ptr);
    if (count != 0 and refs_ptr == null) return false;
    const refs = many(refs_ptr, count);
    for (refs, many(snapshot.edges, count)) |*ref, *edge| {
        if (edge.source_id <= 0 or edge.source_id > snapshot.node_count or
            edge.target_id <= 0 or edge.target_id > snapshot.node_count or edge.type == null) return false;
        ref.* = .{
            .source_stable_id = snapshot.node_stable_ids[@intCast(edge.source_id - 1)],
            .edge_type = edge.type,
            .target_stable_id = snapshot.node_stable_ids[@intCast(edge.target_id - 1)],
        };
        if (ref.source_stable_id == null or ref.target_stable_id == null) return false;
    }
    if (count > 1) c.qsort(refs.ptr, count, @sizeOf(SnapshotTopologyRef), snapshotTopologyRefCompare);
    var unique_count: usize = 0;
    for (refs, 0..) |*ref, i| {
        if (i == 0 or snapshotTopologyRefCompare(&refs[i - 1], ref) != 0) unique_count += 1;
    }
    if (unique_count != snapshot.integrity.metadata_topology_edges) return false;
    out.topology_edges = snapshotAlloc(c.cbm_zova_snapshot_topology_edge_t, unique_count);
    if (unique_count != 0 and out.topology_edges == null) return false;
    out.topology_edge_count = @intCast(unique_count);
    const rows = many(out.topology_edges, unique_count);
    var written: usize = 0;
    for (refs, 0..) |*ref, i| {
        if (i > 0 and snapshotTopologyRefCompare(&refs[i - 1], ref) == 0) continue;
        const row = &rows[written];
        written += 1;
        row.source_stable_id = repoDupZ(ref.source_stable_id);
        row.edge_type = repoDupZ(ref.edge_type);
        row.target_stable_id = repoDupZ(ref.target_stable_id);
        if (row.source_stable_id == null or row.edge_type == null or row.target_stable_id == null) return false;
    }
    return true;
}

/// The size of the int8 vector collection `name`, when it has the snapshot's
/// dimensions and exactly `expected` vectors; null otherwise.
fn snapshotVectorCount(repo: *const Repository, name: Str, dimensions: c_int, expected: u64) ?c_int {
    var info = std.mem.zeroes(VectorInfo);
    if (c.zova_vector_collection_info_get(&c.zova_vector_collection_info_get_request{
        .db = repo.db,
        .name = name,
        .out_info = &info,
    }) != c.ZOVA_OK) return null;
    defer c.zova_vector_collection_info_free(&info);
    if (info.element_type != c.ZOVA_VECTOR_ELEMENT_TYPE_I8 or
        info.dimensions != @as(u32, @bitCast(dimensions)) or
        info.vector_count > std.math.maxInt(i32) or info.vector_count != expected) return null;
    return @intCast(info.vector_count);
}

/// A malloc'd copy of the int8 values of vector `key` in `collection`; NULL
/// when the vector cannot be read or does not have `dimensions` values.
fn snapshotVectorBytes(repo: *const Repository, collection: Str, key: i64, dimensions: c_int) [*c]u8 {
    var vector_id: [32]u8 = undefined;
    if (interop.bprint(&vector_id, "{d}", .{key}) >= vector_id.len) return null;
    var native = std.mem.zeroes(c.zova_vector);
    defer c.zova_vector_free(&native);
    if (c.zova_vector_get(&c.zova_vector_get_request{
        .db = repo.db,
        .collection_name = collection,
        .vector_id = &vector_id,
        .out_vector = &native,
    }) != c.ZOVA_OK or native.element_type != c.ZOVA_VECTOR_ELEMENT_TYPE_I8 or
        native.values_len != @as(usize, @bitCast(@as(isize, dimensions)))) return null;
    const bytes = interop.mallocN(u8, native.values_len);
    if (bytes != null) _ = c.memcpy(bytes, native.i8_values, native.values_len);
    return bytes;
}

/// Reads the vector of every node of `snapshot` that has one into `out`,
/// which may be the same snapshot.
fn snapshotReadNodeVectors(repo: *Repository, snapshot: *const Snapshot, out: *Snapshot) bool {
    var collection: [ID_MAX + 128]u8 = undefined;
    if (c.cbm_zova_workspace_node_vector_collection_name(repo.workspaceId(), snapshot.model_fingerprint, snapshot.vector_dimensions, &collection, collection.len) != 0) return false;
    const node_count = snapshotVectorCount(repo, @ptrCast(&collection), snapshot.vector_dimensions, snapshot.integrity.node_vectors) orelse return false;
    const count: usize = @intCast(node_count);
    out.node_vectors = snapshotAlloc(c.CBMDumpVector, count);
    out.node_vector_ids = snapshotAlloc([*c]u8, count);
    out.node_vector_count = node_count;
    if (count != 0 and (out.node_vectors == null or out.node_vector_ids == null)) return false;
    const vectors = many(out.node_vectors, count);
    const vector_ids = many(out.node_vector_ids, count);
    var written: usize = 0;
    for (many(snapshot.nodes, snapshot.node_count), 0..) |*node, i| {
        if (snapshot.zova_node_keys == null) return false;
        const key = snapshot.zova_node_keys[i];
        var vector_id: [32]u8 = undefined;
        if (interop.bprint(&vector_id, "{d}", .{key}) >= vector_id.len) return false;
        var exists: u8 = 0;
        if (c.zova_vector_exists(&c.zova_vector_exists_request{
            .db = repo.db,
            .collection_name = &collection,
            .vector_id = &vector_id,
            .out_exists = &exists,
        }) != c.ZOVA_OK) return false;
        if (exists == 0) continue;
        if (written >= count) return false;
        const bytes = snapshotVectorBytes(repo, @ptrCast(&collection), key, snapshot.vector_dimensions);
        if (bytes == null) return false;
        const vector = &vectors[written];
        vector_ids[written] = repoDupZ(snapshot.node_stable_ids[i]);
        vector.* = .{
            .node_id = node.id,
            .project = repoDupZ(snapshot.project),
            .vector = bytes,
            .vector_len = snapshot.vector_dimensions,
        };
        written += 1;
        if (vector_ids[written - 1] == null or vector.project == null) return false;
    }
    return written == count;
}

const SnapshotTokenRow = extern struct {
    token_key: i64,
    public_id: [*c]u8,
    token: [*c]u8,
    idf: f64,
};

fn snapshotTokenRowCompare(left: ?*const anyopaque, right: ?*const anyopaque) callconv(.c) c_int {
    return c.strcmp(cast(SnapshotTokenRow, left).public_id, cast(SnapshotTokenRow, right).public_id);
}

/// Reads every token vector of the workspace into `out`, which may be the
/// same snapshot, ordered by public token id and numbered from 1.
fn snapshotReadTokenVectors(repo: *Repository, snapshot: *const Snapshot, out: *Snapshot) bool {
    var collection: [ID_MAX + 128]u8 = undefined;
    if (c.cbm_zova_workspace_token_vector_collection_name(repo.workspaceId(), snapshot.model_fingerprint, snapshot.vector_dimensions, &collection, collection.len) != 0) return false;
    const token_count = snapshotVectorCount(repo, @ptrCast(&collection), snapshot.vector_dimensions, snapshot.integrity.token_vectors) orelse return false;
    const count: usize = @intCast(token_count);
    out.token_vectors = snapshotAlloc(c.CBMDumpTokenVec, count);
    out.token_vector_ids = snapshotAlloc([*c]u8, count);
    out.token_vector_count = token_count;
    if (count != 0 and (out.token_vectors == null or out.token_vector_ids == null)) return false;
    const rows_ptr = snapshotAlloc(SnapshotTokenRow, count);
    if (count != 0 and rows_ptr == null) return false;
    const rows = many(rows_ptr, count);
    defer {
        for (rows) |*row| {
            c.free(row.public_id);
            c.free(row.token);
        }
        c.free(rows_ptr);
    }
    {
        const statement = repoPrepare(repo.db, "SELECT token_key,token,idf FROM cbm_token_vector_metadata_v1 " ++
            "WHERE workspace_key=?1 ORDER BY token_key") orelse return false;
        defer repoFinalize(statement);
        if (!repoBindI64(statement, 1, repo.workspace_key)) return false;
        var step: c.zova_step_result = c.ZOVA_STEP_DONE;
        for (rows) |*row| {
            if (!repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return false;
            row.token_key = repoColumnI64(statement, 0);
            row.token = repoColumnText(statement, 1);
            row.idf = repoColumnDouble(statement, 2);
            var public_id: [ID_MAX + 64]u8 = undefined;
            if (row.token_key <= 0 or row.token == null or
                c.cbm_zova_workspace_token_id_v1(repo.workspaceId(), row.token, &public_id, public_id.len) != 0) return false;
            row.public_id = repoDupZ(&public_id);
            if (row.public_id == null) return false;
        }
        // The metadata table must hold exactly as many tokens as the collection.
        if (!repoStep(statement, &step) or step != c.ZOVA_STEP_DONE) return false;
    }
    if (count > 1) c.qsort(rows.ptr, count, @sizeOf(SnapshotTokenRow), snapshotTokenRowCompare);
    for (rows, many(out.token_vectors, count), many(out.token_vector_ids, count), 1..) |*row, *vector, *vector_id, id| {
        const bytes = snapshotVectorBytes(repo, @ptrCast(&collection), row.token_key, snapshot.vector_dimensions);
        if (bytes == null) return false;
        vector_id.* = row.public_id;
        row.public_id = null;
        vector.* = .{
            .id = @intCast(id),
            .project = repoDupZ(snapshot.project),
            .token = row.token,
            .vector = bytes,
            .vector_len = snapshot.vector_dimensions,
            .idf = @floatCast(row.idf),
        };
        row.token = null;
        if (vector.project == null) return false;
    }
    return true;
}

fn snapshotReadHashesSummary(repo: *Repository, snapshot: *Snapshot) bool {
    const phase_started = repoNow();
    const count = snapshotCount(repo, "SELECT count(*) FROM cbm_file_hashes_v1 h JOIN cbm_files_v1 f " ++
        "ON f.file_key=h.file_key WHERE f.workspace_key=?1");
    if (count < 0) return false;
    snapshot.file_hash_count = count;
    snapshot.file_hashes = snapshotAlloc(c.cbm_zova_file_hash_input_t, @intCast(count));
    if (count != 0 and snapshot.file_hashes == null) return false;
    {
        const statement = repoPrepare(repo.db, "SELECT f.file_path,h.content_hash,h.mtime_ns,h.size_bytes FROM cbm_file_hashes_v1 h " ++
            "JOIN cbm_files_v1 f ON f.file_key=h.file_key " ++
            "WHERE f.workspace_key=?1 ORDER BY f.file_path") orelse return false;
        defer repoFinalize(statement);
        if (!repoBindI64(statement, 1, repo.workspace_key)) return false;
        for (many(snapshot.file_hashes, count)) |*hash| {
            var step: c.zova_step_result = c.ZOVA_STEP_DONE;
            if (!repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return false;
            hash.file_path = repoColumnText(statement, 0);
            hash.content_hash = repoColumnText(statement, 1);
            hash.mtime_ns = repoColumnI64(statement, 2);
            hash.size_bytes = repoColumnI64(statement, 3);
            if (hash.file_path == null or hash.content_hash == null) return false;
        }
    }
    {
        const statement = repoPrepare(repo.db, "SELECT summary,source_hash,created_at,updated_at FROM cbm_project_summaries_v2 " ++
            "WHERE workspace_key=?1") orelse return false;
        defer repoFinalize(statement);
        var step: c.zova_step_result = c.ZOVA_STEP_DONE;
        if (!repoBindI64(statement, 1, repo.workspace_key) or !repoStep(statement, &step)) return false;
        if (step == c.ZOVA_STEP_ROW) {
            const summary = &snapshot.project_summary;
            summary.present = true;
            summary.summary = repoColumnText(statement, 0);
            summary.source_hash = repoColumnText(statement, 1);
            summary.created_at = repoColumnText(statement, 2);
            summary.updated_at = repoColumnText(statement, 3);
            if (summary.summary == null or summary.source_hash == null or
                summary.created_at == null or summary.updated_at == null) return false;
        }
    }
    snapshot.metrics.hashes_summary_ms = repoElapsedMs(phase_started);
    snapshot.metrics.file_hash_rows = @intCast(count);
    snapshot.metrics.base_phase_mask |= PHASE_HASHES_SUMMARY;
    return true;
}

/// Reads the ready active generation of the repository's workspace into the
/// snapshot and the workspace key into `repo`.
fn snapshotReadHeader(repo: *Repository, snapshot: *Snapshot) bool {
    const statement = repoPrepare(repo.db, "SELECT r.workspace_key,r.canonical_root,p.root_path,p.project,p.indexed_at," ++
        "s.model_fingerprint,s.vector_dimensions,r.active_generation " ++
        "FROM cbm_workspace_registry r " ++
        "JOIN cbm_projects_v1 p ON p.workspace_key=r.workspace_key " ++
        "JOIN cbm_workspace_index_state_v1 s ON s.workspace_key=r.workspace_key " ++
        "JOIN cbm_database_generation_v1 g ON g.workspace_key=r.workspace_key " ++
        "AND g.generation=r.active_generation AND g.state='ready' " ++
        "WHERE r.workspace_id=?1 AND s.generation=r.active_generation") orelse return false;
    defer repoFinalize(statement);
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!repoBindText(statement, 1, repo.workspaceId()) or
        !repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return false;
    repo.workspace_key = repoColumnI64(statement, 0);
    snapshot.root_path = repoColumnText(statement, 1);
    const project_root = repoColumnText(statement, 2);
    defer c.free(project_root);
    snapshot.project = repoColumnText(statement, 3);
    snapshot.indexed_at = repoColumnText(statement, 4);
    snapshot.model_fingerprint = repoColumnText(statement, 5);
    snapshot.vector_dimensions = repoColumnInt(statement, 6);
    snapshot.generation = repoColumnI64(statement, 7);
    if (snapshot.root_path == null or snapshot.project == null or snapshot.indexed_at == null or
        snapshot.model_fingerprint == null or snapshot.vector_dimensions <= 0 or
        snapshot.generation <= 0) return false;
    var expected: [ID_MAX]u8 = undefined;
    if (project_root == null or c.strcmp(project_root, snapshot.root_path) != 0 or
        c.cbm_zova_workspace_id_for_root(snapshot.root_path, &expected, expected.len) != 0 or
        c.strcmp(&expected, repo.workspaceId()) != 0) return false;
    return repoStep(statement, &step) and step == c.ZOVA_STEP_DONE;
}

fn snapshotCopyDigest(statement: ?*Stmt, column: c_int, out: *Digest) bool {
    const digest = repoColumnText(statement, column);
    defer c.free(digest);
    if (digest == null or c.strlen(digest) != out.len - 1) return false;
    @memcpy(out, digest[0..out.len]);
    return true;
}

fn snapshotReadIntegrity(repo: *Repository, snapshot: *Snapshot) bool {
    const statement = repoPrepare(repo.db, "SELECT graph_nodes,graph_edges,metadata_nodes,metadata_edges," ++
        "metadata_topology_edges,fts_rows,node_vector_rows,token_vector_rows," ++
        "node_vectors,token_vectors,metadata_sha256,fts_sha256,topology_sha256," ++
        "node_vector_sha256,token_vector_sha256 FROM cbm_generation_integrity_v2 " ++
        "WHERE workspace_key=?1 AND generation=?2") orelse return false;
    defer repoFinalize(statement);
    var step: c.zova_step_result = c.ZOVA_STEP_DONE;
    if (!repoBindI64(statement, 1, repo.workspace_key) or !repoBindI64(statement, 2, snapshot.generation) or
        !repoStep(statement, &step) or step != c.ZOVA_STEP_ROW) return false;
    const integrity = &snapshot.integrity;
    _ = interop.bprint(&integrity.workspace_id, "{s}", .{span(repo.workspaceId())});
    integrity.generation = snapshot.generation;
    // In the order the statement selects them.
    const counters = [_]*u64{
        &integrity.graph_nodes,             &integrity.graph_edges,
        &integrity.metadata_nodes,          &integrity.metadata_edges,
        &integrity.metadata_topology_edges, &integrity.fts_rows,
        &integrity.node_vector_rows,        &integrity.token_vector_rows,
        &integrity.node_vectors,            &integrity.token_vectors,
    };
    for (counters, 0..) |counter, column| counter.* = @bitCast(repoColumnI64(statement, @intCast(column)));
    const digests = [_]*Digest{
        &integrity.metadata_sha256,    &integrity.fts_sha256,          &integrity.topology_sha256,
        &integrity.node_vector_sha256, &integrity.token_vector_sha256,
    };
    for (digests, counters.len..) |digest, column| {
        if (!snapshotCopyDigest(statement, @intCast(column), digest)) return false;
    }
    return repoStep(statement, &step) and step == c.ZOVA_STEP_DONE;
}

/// Whether the graph and both vector collections hold the counts the
/// integrity row records.
fn snapshotVerifyNative(repo: *Repository, snapshot: *const Snapshot) bool {
    var graph_name: [ID_MAX + 32]u8 = undefined;
    var node_collection: [ID_MAX + 128]u8 = undefined;
    var token_collection: [ID_MAX + 128]u8 = undefined;
    if (c.cbm_zova_workspace_graph_name(repo.workspaceId(), &graph_name, graph_name.len) != 0 or
        c.cbm_zova_workspace_node_vector_collection_name(repo.workspaceId(), snapshot.model_fingerprint, snapshot.vector_dimensions, &node_collection, node_collection.len) != 0 or
        c.cbm_zova_workspace_token_vector_collection_name(repo.workspaceId(), snapshot.model_fingerprint, snapshot.vector_dimensions, &token_collection, token_collection.len) != 0) return false;
    var graph = std.mem.zeroes(c.zova_graph_info);
    defer c.zova_graph_info_free(&graph);
    var nodes = std.mem.zeroes(VectorInfo);
    defer c.zova_vector_collection_info_free(&nodes);
    var tokens = std.mem.zeroes(VectorInfo);
    defer c.zova_vector_collection_info_free(&tokens);
    return c.zova_graph_info_get(&c.zova_graph_info_get_request{ .db = repo.db, .name = &graph_name, .out_info = &graph }) == c.ZOVA_OK and
        c.zova_vector_collection_info_get(&c.zova_vector_collection_info_get_request{ .db = repo.db, .name = &node_collection, .out_info = &nodes }) == c.ZOVA_OK and
        c.zova_vector_collection_info_get(&c.zova_vector_collection_info_get_request{ .db = repo.db, .name = &token_collection, .out_info = &tokens }) == c.ZOVA_OK and
        graph.node_count == snapshot.integrity.graph_nodes and
        graph.edge_count == snapshot.integrity.graph_edges and
        nodes.vector_count == snapshot.integrity.node_vectors and
        tokens.vector_count == snapshot.integrity.token_vectors;
}

/// Reads the snapshot inside the open read transaction. `phase` names the
/// base phase reached, for the failure message.
fn snapshotExportRead(repo: *Repository, snapshot: *Snapshot, components: Components, phase: *Str) bool {
    phase.* = "header";
    var phase_started = repoNow();
    if (!snapshotReadHeader(repo, snapshot)) return false;
    snapshot.metrics.header_ms = repoElapsedMs(phase_started);
    snapshot.metrics.base_phase_mask |= PHASE_HEADER;

    phase.* = "integrity";
    phase_started = repoNow();
    if (!snapshotReadIntegrity(repo, snapshot)) return false;
    snapshot.metrics.integrity_ms = repoElapsedMs(phase_started);
    snapshot.metrics.base_phase_mask |= PHASE_INTEGRITY;

    phase.* = "nodes";
    var node_keys: [*c]SnapshotNodeKeyRef = null;
    defer c.free(node_keys);
    snapshotReadNodes(repo, snapshot, &node_keys) catch return false;
    phase.* = "edges";
    snapshotReadEdges(repo, snapshot, many(node_keys, snapshot.node_count)) catch return false;
    phase.* = "hashes_summary";
    if (!snapshotReadHashesSummary(repo, snapshot)) return false;

    const optional_started = repoNow();
    if (components & COMPONENT_TOPOLOGY != 0 and !snapshotBuildTopology(snapshot, snapshot)) return false;
    if (components & COMPONENT_NODE_VECTORS != 0 and !snapshotReadNodeVectors(repo, snapshot, snapshot)) return false;
    if (components & COMPONENT_TOKEN_VECTORS != 0 and !snapshotReadTokenVectors(repo, snapshot, snapshot)) return false;
    snapshot.metrics.optional_ms = if (components == COMPONENT_NONE) 0.0 else repoElapsedMs(optional_started);
    const integrity = &snapshot.integrity;
    if (snapshot.node_count != narrowInt(integrity.metadata_nodes) or
        snapshot.edge_count != narrowInt(integrity.metadata_edges)) return false;
    if (components & COMPONENT_NODE_VECTORS != 0 and
        snapshot.node_vector_count != narrowInt(integrity.node_vectors)) return false;
    if (components & COMPONENT_TOKEN_VECTORS != 0 and
        snapshot.token_vector_count != narrowInt(integrity.token_vectors)) return false;
    if (components == COMPONENT_ALL and !snapshotVerifyNative(repo, snapshot)) return false;
    snapshot.hydrated_components = components;
    snapshot.metrics.hydrated_components = components;
    snapshot.metrics.topology_rows = @intCast(snapshot.topology_edge_count);
    snapshot.metrics.node_vector_rows = @intCast(snapshot.node_vector_count);
    snapshot.metrics.token_vector_rows = @intCast(snapshot.token_vector_count);
    return true;
}

fn repositoryExportSnapshot(maybe_path: ?Str, maybe_workspace_id: ?Str, maybe_out: ?*Snapshot, components: Components) c_int {
    const out = maybe_out orelse return -1;
    out.* = std.mem.zeroes(Snapshot);
    const path = nonEmpty(maybe_path) orelse return -1;
    if (c.cbm_zova_workspace_id_validate(maybe_workspace_id) != 0) return -1;
    const workspace_id = span(maybe_workspace_id.?);
    var snapshot = std.mem.zeroes(Snapshot);
    var repo = Repository{};
    _ = interop.bprint(&repo.workspace_id, "{s}", .{workspace_id});
    _ = interop.bprint(&snapshot.workspace_id, "{s}", .{workspace_id});
    const base_started = repoNow();
    var phase: Str = "open";
    repo.db = repoOpenReadOnly(path);
    snapshot.metrics.open_ms = repoElapsedMs(base_started);
    var ok = repo.db != null;
    if (ok) snapshot.metrics.base_phase_mask |= PHASE_OPEN;
    var transaction_started = false;
    if (ok) {
        ok = c.cbm_zova_register_sql_functions(repo.db) == 0 and repoExec(repo.db, "BEGIN");
        transaction_started = ok;
    }
    if (ok) ok = snapshotExportRead(&repo, &snapshot, components, &phase);
    const close_started = repoNow();
    if (transaction_started) {
        const finished = repoExec(repo.db, if (ok) "COMMIT" else "ROLLBACK");
        if (ok and !finished) ok = false;
    }
    if (repo.db) |db| _ = c.zova_database_close(db);
    snapshot.metrics.close_ms = repoElapsedMs(close_started);
    if (ok) snapshot.metrics.base_phase_mask |= PHASE_CLOSE;
    snapshot.metrics.base_ms = repoElapsedMs(base_started);
    if (!ok) {
        _ = c.fprintf(interop.stderr(), "zova snapshot export failed phase=%s\n", phase);
        cbm_zova_workspace_snapshot_free(&snapshot);
        return -1;
    }
    out.* = snapshot;
    return 0;
}

/// What the snapshot exports do in a build without Zova.
fn snapshotUnavailable(maybe_out: ?*Snapshot) c_int {
    if (maybe_out) |out| out.* = std.mem.zeroes(Snapshot);
    return -1;
}

pub export fn cbm_zova_repository_export_snapshot(path: ?Str, workspace_id: ?Str, out: ?*Snapshot) c_int {
    if (!with_zova) return snapshotUnavailable(out);
    return repositoryExportSnapshot(path, workspace_id, out, COMPONENT_ALL);
}

pub export fn cbm_zova_repository_export_incremental_snapshot(path: ?Str, workspace_id: ?Str, out: ?*Snapshot) c_int {
    if (!with_zova) return snapshotUnavailable(out);
    return repositoryExportSnapshot(path, workspace_id, out, COMPONENT_NONE);
}

const SnapshotGraphNodeOrder = extern struct {
    source_ordinal: u64,
    index: c_int,
};

fn snapshotGraphNodeOrderCompare(left_ptr: ?*const anyopaque, right_ptr: ?*const anyopaque) callconv(.c) c_int {
    const left = cast(SnapshotGraphNodeOrder, left_ptr);
    const right = cast(SnapshotGraphNodeOrder, right_ptr);
    if (left.source_ordinal != right.source_ordinal) return if (left.source_ordinal < right.source_ordinal) -1 else 1;
    return @as(c_int, @intFromBool(left.index > right.index)) - @intFromBool(left.index < right.index);
}

/// Loads the snapshot's nodes, in source order, and its edges into `graph`.
fn snapshotLoadGraph(snapshot: *const Snapshot, graph: ?*c.cbm_gbuf_t) bool {
    if (c.cbm_gbuf_reserve(graph, snapshot.node_count, snapshot.edge_count) != 0) return false;
    const node_count: usize = @intCast(snapshot.node_count);
    // Graph buffer ids by snapshot node id, which starts at 1.
    const ids_ptr = snapshotAlloc(i64, if (node_count != 0) node_count + 1 else 0);
    defer c.free(ids_ptr);
    const order_ptr = snapshotAlloc(SnapshotGraphNodeOrder, node_count);
    defer c.free(order_ptr);
    if (node_count != 0 and (ids_ptr == null or order_ptr == null or snapshot.node_source_ordinals == null)) return false;
    const ids = many(ids_ptr, node_count + 1);
    const order = many(order_ptr, node_count);
    for (order, many(snapshot.node_source_ordinals, node_count), 0..) |*entry, source_ordinal, i| {
        entry.* = .{ .source_ordinal = source_ordinal, .index = @intCast(i) };
    }
    if (node_count > 1) c.qsort(order.ptr, node_count, @sizeOf(SnapshotGraphNodeOrder), snapshotGraphNodeOrderCompare);
    const nodes = many(snapshot.nodes, node_count);
    for (order) |entry| {
        const node = &nodes[@intCast(entry.index)];
        if (node.id <= 0 or node.id > snapshot.node_count or ids[@intCast(node.id)] != 0) return false;
        const id = c.cbm_gbuf_upsert_node(graph, node.label, node.name, node.qualified_name, node.file_path, node.start_line, node.end_line, node.properties);
        ids[@intCast(node.id)] = id;
        if (id <= 0) return false;
    }
    for (many(snapshot.edges, snapshot.edge_count)) |*edge| {
        const source = if (edge.source_id > 0 and edge.source_id <= snapshot.node_count) ids[@intCast(edge.source_id)] else 0;
        const target = if (edge.target_id > 0 and edge.target_id <= snapshot.node_count) ids[@intCast(edge.target_id)] else 0;
        if (source <= 0 or target <= 0 or
            c.cbm_gbuf_insert_edge(graph, source, target, edge.type, edge.properties) <= 0) return false;
    }
    return true;
}

pub export fn cbm_zova_repository_export_incremental_snapshot_to_graph(path: ?Str, workspace_id: ?Str, maybe_out: ?*Snapshot, graph: ?*c.cbm_gbuf_t) c_int {
    if (!with_zova) return snapshotUnavailable(maybe_out);
    if (graph == null or cbm_zova_repository_export_incremental_snapshot(path, workspace_id, maybe_out) != 0) return -1;
    const out = maybe_out.?;
    const started = repoNow();
    if (!snapshotLoadGraph(out, graph)) {
        cbm_zova_workspace_snapshot_free(out);
        return -1;
    }
    out.metrics.graph_buffer_ms = repoElapsedMs(started);
    return 0;
}

fn snapshotIntegrityMatches(left: *const Snapshot, right: *const Snapshot) bool {
    const a = &left.integrity;
    const b = &right.integrity;
    return left.generation == right.generation and
        c.strcmp(&left.workspace_id, &right.workspace_id) == 0 and
        a.metadata_nodes == b.metadata_nodes and a.metadata_edges == b.metadata_edges and
        a.metadata_topology_edges == b.metadata_topology_edges and
        a.node_vectors == b.node_vectors and a.token_vectors == b.token_vectors and
        c.strcmp(&a.metadata_sha256, &b.metadata_sha256) == 0 and
        c.strcmp(&a.fts_sha256, &b.fts_sha256) == 0 and
        c.strcmp(&a.topology_sha256, &b.topology_sha256) == 0 and
        c.strcmp(&a.node_vector_sha256, &b.node_vector_sha256) == 0 and
        c.strcmp(&a.token_vector_sha256, &b.token_vector_sha256) == 0;
}

/// Reads the pending vector components into `hydrated` inside the open read
/// transaction, after checking that the database still holds the generation
/// `snapshot` was read from.
fn snapshotHydrateVectors(repo: *Repository, pending: Components, snapshot: *const Snapshot, current: *Snapshot, hydrated: *Snapshot) c_int {
    if (!snapshotReadHeader(repo, current) or !snapshotReadIntegrity(repo, current)) return c.CBM_ZOVA_SNAPSHOT_ERROR;
    if (!snapshotIntegrityMatches(snapshot, current)) return c.CBM_ZOVA_SNAPSHOT_STALE;
    if (pending & COMPONENT_NODE_VECTORS != 0 and !snapshotReadNodeVectors(repo, snapshot, hydrated)) return c.CBM_ZOVA_SNAPSHOT_ERROR;
    if (pending & COMPONENT_TOKEN_VECTORS != 0 and !snapshotReadTokenVectors(repo, snapshot, hydrated)) return c.CBM_ZOVA_SNAPSHOT_ERROR;
    return c.CBM_ZOVA_SNAPSHOT_OK;
}

/// Reads the pending components of `snapshot` into `hydrated`. The topology
/// derives from the snapshot itself; only the vectors need the database.
fn snapshotHydrate(path: Str, workspace_id: Str, pending: Components, snapshot: *const Snapshot, hydrated: *Snapshot) c_int {
    if (pending & COMPONENT_TOPOLOGY != 0 and !snapshotBuildTopology(snapshot, hydrated)) return c.CBM_ZOVA_SNAPSHOT_ERROR;
    if (pending & (COMPONENT_NODE_VECTORS | COMPONENT_TOKEN_VECTORS) == 0) return c.CBM_ZOVA_SNAPSHOT_OK;
    var repo = Repository{};
    var current = std.mem.zeroes(Snapshot);
    defer cbm_zova_workspace_snapshot_free(&current);
    _ = interop.bprint(&repo.workspace_id, "{s}", .{span(workspace_id)});
    _ = interop.bprint(&current.workspace_id, "{s}", .{span(workspace_id)});
    const db = repoOpenReadOnly(path) orelse return c.CBM_ZOVA_SNAPSHOT_ERROR;
    defer _ = c.zova_database_close(db);
    repo.db = db;
    if (c.cbm_zova_register_sql_functions(db) != 0 or !repoExec(db, "PRAGMA query_only=ON;BEGIN")) return c.CBM_ZOVA_SNAPSHOT_ERROR;
    const rc = snapshotHydrateVectors(&repo, pending, snapshot, &current, hydrated);
    const finished = repoExec(db, if (rc == c.CBM_ZOVA_SNAPSHOT_OK) "COMMIT" else "ROLLBACK");
    return if (rc == c.CBM_ZOVA_SNAPSHOT_OK and !finished) c.CBM_ZOVA_SNAPSHOT_ERROR else rc;
}

pub export fn cbm_zova_repository_hydrate_incremental_components(maybe_path: ?Str, maybe_workspace_id: ?Str, expected_generation: i64, requested: Components, maybe_snapshot: ?*Snapshot) c_int {
    if (!with_zova) return c.CBM_ZOVA_SNAPSHOT_ERROR;
    const path = nonEmpty(maybe_path) orelse return c.CBM_ZOVA_SNAPSHOT_ERROR;
    const snapshot = maybe_snapshot orelse return c.CBM_ZOVA_SNAPSHOT_ERROR;
    if (c.cbm_zova_workspace_id_validate(maybe_workspace_id) != 0) return c.CBM_ZOVA_SNAPSHOT_ERROR;
    const workspace_id = maybe_workspace_id.?;
    if (c.strcmp(workspace_id, &snapshot.workspace_id) != 0 or
        requested == COMPONENT_NONE or requested & ~COMPONENT_ALL != 0) return c.CBM_ZOVA_SNAPSHOT_ERROR;
    if (expected_generation <= 0 or expected_generation != snapshot.generation) return c.CBM_ZOVA_SNAPSHOT_STALE;
    const pending = requested & ~snapshot.hydrated_components;
    if (pending == COMPONENT_NONE) return c.CBM_ZOVA_SNAPSHOT_OK;

    const started = repoNow();
    var hydrated = std.mem.zeroes(Snapshot);
    defer cbm_zova_workspace_snapshot_free(&hydrated);
    const rc = snapshotHydrate(path, workspace_id, pending, snapshot, &hydrated);
    if (rc != c.CBM_ZOVA_SNAPSHOT_OK) return rc;

    if (pending & COMPONENT_TOPOLOGY != 0) {
        snapshot.topology_edges = hydrated.topology_edges;
        snapshot.topology_edge_count = hydrated.topology_edge_count;
        hydrated.topology_edges = null;
        hydrated.topology_edge_count = 0;
    }
    if (pending & COMPONENT_NODE_VECTORS != 0) {
        snapshot.node_vectors = hydrated.node_vectors;
        snapshot.node_vector_ids = hydrated.node_vector_ids;
        snapshot.node_vector_count = hydrated.node_vector_count;
        hydrated.node_vectors = null;
        hydrated.node_vector_ids = null;
        hydrated.node_vector_count = 0;
    }
    if (pending & COMPONENT_TOKEN_VECTORS != 0) {
        snapshot.token_vectors = hydrated.token_vectors;
        snapshot.token_vector_ids = hydrated.token_vector_ids;
        snapshot.token_vector_count = hydrated.token_vector_count;
        hydrated.token_vectors = null;
        hydrated.token_vector_ids = null;
        hydrated.token_vector_count = 0;
    }
    snapshot.hydrated_components |= pending;
    snapshot.metrics.hydrated_components = snapshot.hydrated_components;
    snapshot.metrics.topology_rows = @intCast(snapshot.topology_edge_count);
    snapshot.metrics.node_vector_rows = @intCast(snapshot.node_vector_count);
    snapshot.metrics.token_vector_rows = @intCast(snapshot.token_vector_count);
    snapshot.metrics.optional_ms += repoElapsedMs(started);
    return c.CBM_ZOVA_SNAPSHOT_OK;
}
