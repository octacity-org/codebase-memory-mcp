//! graph_tools.zig — the compare_graphs, get_graph_schema, search_graph and
//! query_graph MCP tool handlers.

const std = @import("std");
const c = @import("c");
const mcp = @import("mcp.zig");

const CStr = mcp.CStr;
const HeapStr = mcp.HeapStr;
const Str = mcp.Str;
const Server = mcp.Server;
const MutDoc = mcp.MutDoc;
const MutVal = mcp.MutVal;
const Val = mcp.Val;
const textResult = mcp.textResult;
const strArg = mcp.strArg;
const intArg = mcp.intArg;
const boolArg = mcp.boolArg;
const orEmpty = mcp.orEmpty;
const fmtZ = mcp.fmtZ;
const CBM_NOT_FOUND = mcp.CBM_NOT_FOUND;

// ── compare_graphs ───────────────────────────────────────────────

const COMPARE_DEFAULT_LIMIT = 200;
const COMPARE_MAX_LIMIT = 1000;
const COMPARE_DEFAULT_SCAN_LIMIT = 2000000;
const COMPARE_MAX_SCAN_LIMIT = 10000000;
const COMPARE_SET_BYTE_BUDGET: usize = 512 * 1024;

const CompareSet = struct {
    items: ?*MutVal,
    returned: usize = 0,
    encoded_bytes: usize = 2, // the enclosing "[]"
    budget_exhausted: bool = false,
};

const CompareResponse = struct {
    doc: ?*MutDoc,
    limit: usize,
    nodes_added: CompareSet,
    nodes_removed: CompareSet,
    edges_added: CompareSet,
    edges_removed: CompareSet,
};

fn compareGraphsError(code: Str, message: CStr) HeapStr {
    const oom = "compare_graphs failed: out of memory";
    const doc = c.yyjson_mut_doc_new(null) orelse return textResult(oom, true);
    const root = c.yyjson_mut_obj(doc);
    if (root == null) {
        c.yyjson_mut_doc_free(doc);
        return textResult(oom, true);
    }
    c.yyjson_mut_doc_set_root(doc, root);
    if (!c.yyjson_mut_obj_add_strcpy(doc, root, "error", message) or
        !c.yyjson_mut_obj_add_strcpy(doc, root, "code", code))
    {
        c.yyjson_mut_doc_free(doc);
        return textResult(oom, true);
    }
    const json = mcp.docToStr(doc);
    c.yyjson_mut_doc_free(doc);
    if (json == null) return textResult(oom, true);
    return mcp.textResultOwned(json, true);
}

fn compareArgNameAllowed(name: []const u8) bool {
    return mcp.eql(name, "base_project") or mcp.eql(name, "target_project") or
        mcp.eql(name, "limit") or mcp.eql(name, "scan_limit");
}

fn compareParseBoundedInteger(root: ?*Val, key: Str, default_value: i64, maximum: i64, out: *u64, err: *Str) bool {
    const value = c.yyjson_obj_get(root, key);
    var parsed = default_value;
    if (value != null) {
        if (!c.yyjson_is_int(value)) {
            err.* = "limit values must be integers";
            return false;
        }
        parsed = c.yyjson_get_int(value);
    }
    if (parsed < 1 or parsed > maximum) {
        err.* = if (mcp.eql(std.mem.span(key), "limit")) "limit must be between 1 and 1000" else "scan_limit must be between 1 and 10000000";
        return false;
    }
    out.* = @intCast(parsed);
    return true;
}

const CompareArgs = struct {
    base_project: HeapStr = null,
    target_project: HeapStr = null,
    limit: u64 = 0,
    scan_limit: u64 = 0,

    fn deinit(self: *CompareArgs) void {
        c.free(self.base_project);
        c.free(self.target_project);
        self.base_project = null;
        self.target_project = null;
    }
};

fn compareParseArguments(args: CStr, out: *CompareArgs, err: *Str) bool {
    out.* = .{};
    const doc = mcp.readJson(if (args != null) args else "{}") orelse {
        err.* = "arguments must be valid JSON";
        return false;
    };
    defer c.yyjson_doc_free(doc);
    const root = c.yyjson_doc_get_root(doc);
    if (!c.yyjson_is_obj(root)) {
        err.* = "arguments must be an object";
        return false;
    }

    var iterator = c.yyjson_obj_iter_with(root);
    while (c.yyjson_obj_iter_next(&iterator)) |key| {
        const name = c.yyjson_get_str(key);
        if (name == null or !compareArgNameAllowed(std.mem.span(name))) {
            err.* = "unknown argument";
            return false;
        }
    }

    const base = c.yyjson_obj_get(root, "base_project");
    const target = c.yyjson_obj_get(root, "target_project");
    if (base == null or !c.yyjson_is_str(base) or c.yyjson_get_len(base) == 0 or target == null or
        !c.yyjson_is_str(target) or c.yyjson_get_len(target) == 0)
    {
        err.* = "base_project and target_project are required non-empty strings";
        return false;
    }
    if (mcp.cEql(c.yyjson_get_str(base), c.yyjson_get_str(target))) {
        err.* = "base_project and target_project must be distinct";
        return false;
    }
    if (!compareParseBoundedInteger(root, "limit", COMPARE_DEFAULT_LIMIT, COMPARE_MAX_LIMIT, &out.limit, err) or
        !compareParseBoundedInteger(root, "scan_limit", COMPARE_DEFAULT_SCAN_LIMIT, COMPARE_MAX_SCAN_LIMIT, &out.scan_limit, err))
    {
        return false;
    }

    out.base_project = mcp.dup(c.yyjson_get_str(base));
    out.target_project = mcp.dup(c.yyjson_get_str(target));
    if (out.base_project == null or out.target_project == null) {
        out.deinit();
        err.* = "out of memory while validating arguments";
        return false;
    }
    return true;
}

fn compareAddIdentityString(doc: ?*MutDoc, object: ?*MutVal, key: Str, value: CStr) bool {
    const sanitized = mcp.sanitizeUtf8Lossy(value);
    if (sanitized == null) return false;
    defer c.free(sanitized);
    return c.yyjson_mut_obj_add_strcpy(doc, object, key, sanitized);
}

fn compareNodeJson(doc: ?*MutDoc, node: *const c.cbm_graph_node_identity_t) ?*MutVal {
    const object = c.yyjson_mut_obj(doc);
    if (object == null or
        !compareAddIdentityString(doc, object, "qualified_name", node.qualified_name) or
        !compareAddIdentityString(doc, object, "label", node.label) or
        !compareAddIdentityString(doc, object, "file_path", node.file_path))
    {
        return null;
    }
    return object;
}

fn compareAppendItem(response: *CompareResponse, set: *CompareSet, item_doc: ?*MutDoc, item: ?*MutVal) bool {
    defer c.yyjson_mut_doc_free(item_doc);
    if (item_doc == null or item == null) return false;
    const encoded = mcp.docToStr(item_doc);
    if (encoded == null) return false;
    const encoded_len = c.strlen(encoded);
    const separator: usize = if (set.returned > 0) 1 else 0;
    c.free(encoded);

    if (set.encoded_bytes > COMPARE_SET_BYTE_BUDGET or
        separator > COMPARE_SET_BYTE_BUDGET - set.encoded_bytes or
        encoded_len > COMPARE_SET_BYTE_BUDGET - set.encoded_bytes - separator)
    {
        set.budget_exhausted = true;
        return true;
    }

    const copy = c.yyjson_mut_val_mut_copy(response.doc, item);
    if (copy == null or !c.yyjson_mut_arr_add_val(set.items, copy)) return false;
    set.encoded_bytes += separator + encoded_len;
    set.returned += 1;
    return true;
}

fn compareNodeCallback(context: ?*anyopaque, added: bool, node: [*c]const c.cbm_graph_node_identity_t) callconv(.c) bool {
    const response: *CompareResponse = @ptrCast(@alignCast(context));
    const set = if (added) &response.nodes_added else &response.nodes_removed;
    if (set.returned >= response.limit or set.budget_exhausted) return true;
    const doc = c.yyjson_mut_doc_new(null);
    const item = if (doc != null) compareNodeJson(doc, node) else null;
    if (doc != null and item != null) c.yyjson_mut_doc_set_root(doc, item);
    return compareAppendItem(response, set, doc, item);
}

fn compareEdgeCallback(context: ?*anyopaque, added: bool, edge: [*c]const c.cbm_graph_edge_identity_t) callconv(.c) bool {
    const response: *CompareResponse = @ptrCast(@alignCast(context));
    const set = if (added) &response.edges_added else &response.edges_removed;
    if (set.returned >= response.limit or set.budget_exhausted) return true;
    const doc = c.yyjson_mut_doc_new(null);
    const item = if (doc != null) c.yyjson_mut_obj(doc) else null;
    const source = if (doc != null) compareNodeJson(doc, &edge.*.source) else null;
    const target = if (doc != null) compareNodeJson(doc, &edge.*.target) else null;
    const ok = item != null and source != null and target != null and
        c.yyjson_mut_obj_add_val(doc, item, "source", source) and
        c.yyjson_mut_obj_add_val(doc, item, "target", target) and
        compareAddIdentityString(doc, item, "type", edge.*.type) and
        (edge.*.local_name[0] == 0 or compareAddIdentityString(doc, item, "local_name", edge.*.local_name));
    if (ok) c.yyjson_mut_doc_set_root(doc, item);
    return compareAppendItem(response, set, doc, if (ok) item else null);
}

fn compareProjectJson(doc: ?*MutDoc, project: CStr, metadata: *const c.cbm_graph_compare_project_t) ?*MutVal {
    const object = c.yyjson_mut_obj(doc);
    if (object == null or !c.yyjson_mut_obj_add_strcpy(doc, object, "project", project) or
        !c.yyjson_mut_obj_add_sint(doc, object, "generation", metadata.generation) or
        !compareAddIdentityString(doc, object, "indexed_at", &metadata.indexed_at) or
        !c.yyjson_mut_obj_add_sint(doc, object, "node_count", metadata.node_count) or
        !c.yyjson_mut_obj_add_sint(doc, object, "edge_count", metadata.edge_count))
    {
        return null;
    }
    return object;
}

fn compareSetJson(doc: ?*MutDoc, set: *CompareSet, total: u64, limit: usize) ?*MutVal {
    const object = c.yyjson_mut_obj(doc);
    const reasons = c.yyjson_mut_arr(doc);
    const truncated = total > set.returned;
    if (object == null or reasons == null or !c.yyjson_mut_obj_add_val(doc, object, "items", set.items) or
        !c.yyjson_mut_obj_add_uint(doc, object, "returned", set.returned) or
        !c.yyjson_mut_obj_add_uint(doc, object, "total", total) or
        !c.yyjson_mut_obj_add_bool(doc, object, "truncated", truncated))
    {
        return null;
    }
    if (truncated and set.returned >= limit and !c.yyjson_mut_arr_add_strcpy(doc, reasons, "limit")) return null;
    if (truncated and set.budget_exhausted and !c.yyjson_mut_arr_add_strcpy(doc, reasons, "encoded_byte_budget")) return null;
    if (!c.yyjson_mut_obj_add_val(doc, object, "truncation_reasons", reasons)) return null;
    return object;
}

pub fn handleCompareGraphs(_: *Server, args: CStr) HeapStr {
    var parsed: CompareArgs = .{};
    var argument_error: Str = "";
    if (!compareParseArguments(args, &parsed, &argument_error)) {
        return compareGraphsError("invalid_arguments", argument_error);
    }
    defer parsed.deinit();

    const base_store = mcp.openProjectStore(parsed.base_project) orelse
        return compareGraphsError("project_not_indexed", "base project is not indexed");
    const target_store = mcp.openProjectStore(parsed.target_project) orelse {
        c.cbm_store_close(base_store);
        return compareGraphsError("project_not_indexed", "target project is not indexed");
    };

    const doc = c.yyjson_mut_doc_new(null);
    const root = if (doc != null) c.yyjson_mut_obj(doc) else null;
    var response = CompareResponse{
        .doc = doc,
        .limit = @intCast(parsed.limit),
        .nodes_added = .{ .items = if (doc != null) c.yyjson_mut_arr(doc) else null },
        .nodes_removed = .{ .items = if (doc != null) c.yyjson_mut_arr(doc) else null },
        .edges_added = .{ .items = if (doc != null) c.yyjson_mut_arr(doc) else null },
        .edges_removed = .{ .items = if (doc != null) c.yyjson_mut_arr(doc) else null },
    };
    if (doc == null or root == null or response.nodes_added.items == null or response.nodes_removed.items == null or
        response.edges_added.items == null or response.edges_removed.items == null)
    {
        c.cbm_store_close(target_store);
        c.cbm_store_close(base_store);
        c.yyjson_mut_doc_free(doc);
        return compareGraphsError("allocation_failed", "could not allocate comparison result");
    }
    c.yyjson_mut_doc_set_root(doc, root);

    var comparison = std.mem.zeroes(c.cbm_graph_compare_result_t);
    const compare_rc = c.cbm_store_compare_graphs(base_store, parsed.base_project, target_store, parsed.target_project, parsed.scan_limit, compareNodeCallback, compareEdgeCallback, &response, &comparison);
    c.cbm_store_close(target_store);
    c.cbm_store_close(base_store);

    if (compare_rc != c.CBM_STORE_OK) {
        c.yyjson_mut_doc_free(doc);
        if (compare_rc == c.CBM_STORE_SCAN_LIMIT) {
            return compareGraphsError("scan_limit_exceeded", "combined graph rows exceed scan_limit");
        }
        return compareGraphsError("query_failed", "graph comparison failed");
    }

    const base = compareProjectJson(doc, parsed.base_project, &comparison.base);
    const target = compareProjectJson(doc, parsed.target_project, &comparison.target);
    const nodes = c.yyjson_mut_obj(doc);
    const edges = c.yyjson_mut_obj(doc);
    const limits = c.yyjson_mut_obj(doc);
    const nodes_added = compareSetJson(doc, &response.nodes_added, comparison.nodes_added_total, response.limit);
    const nodes_removed = compareSetJson(doc, &response.nodes_removed, comparison.nodes_removed_total, response.limit);
    const edges_added = compareSetJson(doc, &response.edges_added, comparison.edges_added_total, response.limit);
    const edges_removed = compareSetJson(doc, &response.edges_removed, comparison.edges_removed_total, response.limit);
    const built = base != null and target != null and nodes != null and edges != null and limits != null and
        nodes_added != null and nodes_removed != null and edges_added != null and edges_removed != null and
        c.yyjson_mut_obj_add_int(doc, root, "schema_version", 1) and
        c.yyjson_mut_obj_add_val(doc, root, "base", base) and
        c.yyjson_mut_obj_add_val(doc, root, "target", target) and
        c.yyjson_mut_obj_add_val(doc, nodes, "added", nodes_added) and
        c.yyjson_mut_obj_add_val(doc, nodes, "removed", nodes_removed) and
        c.yyjson_mut_obj_add_val(doc, root, "nodes", nodes) and
        c.yyjson_mut_obj_add_val(doc, edges, "added", edges_added) and
        c.yyjson_mut_obj_add_val(doc, edges, "removed", edges_removed) and
        c.yyjson_mut_obj_add_val(doc, root, "edges", edges) and
        c.yyjson_mut_obj_add_uint(doc, limits, "limit", parsed.limit) and
        c.yyjson_mut_obj_add_uint(doc, limits, "scan_limit", parsed.scan_limit) and
        c.yyjson_mut_obj_add_uint(doc, limits, "encoded_byte_budget", COMPARE_SET_BYTE_BUDGET) and
        c.yyjson_mut_obj_add_val(doc, root, "limits", limits);
    if (!built) {
        c.yyjson_mut_doc_free(doc);
        return compareGraphsError("allocation_failed", "could not allocate comparison result");
    }

    const json = mcp.docToStr(doc);
    c.yyjson_mut_doc_free(doc);
    if (json == null) return compareGraphsError("allocation_failed", "could not serialize comparison result");
    return mcp.textResultOwned(json, false);
}

// ── get_graph_schema ─────────────────────────────────────────────

pub fn handleGetGraphSchema(srv: *Server, args: CStr) HeapStr {
    const project = mcp.getProjectArg(args);
    const store = mcp.resolveStore(srv, project) orelse return mcp.projectNotFoundResult(project);
    defer c.free(project);

    const not_indexed = mcp.verifyProjectIndexed(store, project);
    if (not_indexed != null) return not_indexed;

    var schema = std.mem.zeroes(c.cbm_schema_info_t);
    if (c.cbm_store_get_schema(store, project, &schema) != c.CBM_STORE_OK) {
        return textResult("failed to read the graph schema", true);
    }
    defer c.cbm_store_schema_free(&schema);

    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);

    const labels = c.yyjson_mut_arr(doc);
    for (0..@intCast(@max(schema.node_label_count, 0))) |i| {
        const nl = &schema.node_labels[i];
        const lbl = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_str(doc, lbl, "label", nl.label);
        _ = c.yyjson_mut_obj_add_int(doc, lbl, "count", nl.count);
        const props = c.yyjson_mut_arr(doc);
        for (0..@intCast(@max(nl.property_count, 0))) |j| _ = c.yyjson_mut_arr_add_str(doc, props, nl.properties[j]);
        _ = c.yyjson_mut_obj_add_val(doc, lbl, "properties", props);
        _ = c.yyjson_mut_arr_add_val(labels, lbl);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "node_labels", labels);

    const types = c.yyjson_mut_arr(doc);
    for (0..@intCast(@max(schema.edge_type_count, 0))) |i| {
        const et = &schema.edge_types[i];
        const typ = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_str(doc, typ, "type", et.type);
        _ = c.yyjson_mut_obj_add_int(doc, typ, "count", et.count);
        const eprops = c.yyjson_mut_arr(doc);
        for (0..@intCast(@max(et.property_count, 0))) |j| _ = c.yyjson_mut_arr_add_str(doc, eprops, et.properties[j]);
        _ = c.yyjson_mut_obj_add_val(doc, typ, "properties", eprops);
        _ = c.yyjson_mut_arr_add_val(types, typ);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "edge_types", types);
    // The doc borrows schema strings: serialize before schema_free (deferred).
    return mcp.docResult(doc);
}

// ── search_graph ─────────────────────────────────────────────────

/// Validate edge type: uppercase letters + underscore only, max 64 chars.
fn validateEdgeType(s: CStr) bool {
    if (s == null or c.strlen(s) > 64) return false;
    for (std.mem.span(s)) |ch| {
        if (!(ch >= 'A' and ch <= 'Z') and ch != '_') return false;
    }
    return true;
}

const BM25_QUERY_BUF = 1024;
const BM25_DEFAULT_LIMIT = 100;

fn isWordChar(ch: u8) bool {
    return std.ascii.isAlphanumeric(ch) or ch == '_';
}

/// Build an FTS5 MATCH expression from a free-form query string by splitting
/// on non-word characters and joining the terms with OR. Each token is
/// quoted, so the caller can't inject FTS5 operators or phrases. Returns the
/// number of tokens emitted (0 if the query contained no usable terms).
fn bm25BuildMatch(query: []const u8, out: []u8) c_int {
    if (out.len < 2) return 0;
    var pos: usize = 0;
    var tokens: c_int = 0;
    var p: usize = 0;
    while (p < query.len) {
        while (p < query.len and !isWordChar(query[p])) p += 1;
        if (p >= query.len) break;
        const tok_start = p;
        while (p < query.len and isWordChar(query[p])) p += 1;
        const tok = query[tok_start..p];
        const sep: []const u8 = if (tokens > 0) " OR " else "";
        if (pos + sep.len + tok.len + 2 + 1 >= out.len) break; // out of room — keep what we have
        @memcpy(out[pos .. pos + sep.len], sep);
        pos += sep.len;
        // Quoted so a word such as OR or NOT is a term, not an FTS5 operator.
        out[pos] = '"';
        pos += 1;
        @memcpy(out[pos .. pos + tok.len], tok);
        pos += tok.len;
        out[pos] = '"';
        pos += 1;
        tokens += 1;
    }
    out[pos] = 0;
    return tokens;
}

/// True when the query uses FTS5 expression syntax (a phrase, a prefix,
/// grouping or an uppercase operator) and should reach MATCH unchanged.
fn bm25IsExpression(query: []const u8) bool {
    if (std.mem.indexOfAny(u8, query, "\"*()") != null) return true;
    var it = std.mem.tokenizeAny(u8, query, " \t");
    while (it.next()) |word| {
        for ([_][]const u8{ "AND", "OR", "NOT", "NEAR" }) |op| {
            if (mcp.eql(word, op)) return true;
        }
    }
    return false;
}

/// Match expression for the shared cbm.zova FTS index: plain words match any
/// word; FTS5 expressions pass through. Returns false when nothing is left.
fn zovaFtsMatch(query: CStr, out: []u8) bool {
    const q = std.mem.span(query);
    if (bm25IsExpression(q)) return mcp.bprint(out, "{s}", .{q}) < out.len;
    return bm25BuildMatch(q, out) > 0;
}

fn hasText(s: CStr) bool {
    return s != null and s[0] != 0;
}

/// Emit the search results as a JSON "results" array. Property docs from
/// enrichNodeProperties are appended to pdocs; the caller frees them AFTER
/// serializing doc (the strings are zero-copy pointers into them).
fn emitSearchResults(doc: ?*MutDoc, root: ?*MutVal, out: *const c.cbm_search_output_t, offset: c_int, pdocs: *PropDocs) void {
    _ = c.yyjson_mut_obj_add_int(doc, root, "total", out.total);
    const results = c.yyjson_mut_arr(doc);
    for (0..@intCast(@max(out.count, 0))) |i| {
        const sr = &out.results[i];
        const item = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_str(doc, item, "name", orEmpty(sr.node.name));
        _ = c.yyjson_mut_obj_add_str(doc, item, "qualified_name", orEmpty(sr.node.qualified_name));
        _ = c.yyjson_mut_obj_add_str(doc, item, "label", orEmpty(sr.node.label));
        _ = c.yyjson_mut_obj_add_str(doc, item, "file_path", orEmpty(sr.node.file_path));
        _ = c.yyjson_mut_obj_add_int(doc, item, "in_degree", sr.in_degree);
        _ = c.yyjson_mut_obj_add_int(doc, item, "out_degree", sr.out_degree);
        if (mcp.enrichNodeProperties(doc, item, sr.node.properties_json)) |pdoc| pdocs.add(pdoc);
        _ = c.yyjson_mut_arr_add_val(results, item);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "results", results);
    _ = c.yyjson_mut_obj_add_bool(doc, root, "has_more", out.total > offset + out.count);
}

/// Parsed property docs that must outlive serialization.
const PropDocs = struct {
    docs: std.ArrayList(*c.yyjson_doc) = .empty,

    fn add(self: *PropDocs, d: *c.yyjson_doc) void {
        self.docs.append(std.heap.c_allocator, d) catch c.yyjson_doc_free(d);
    }

    fn deinit(self: *PropDocs) void {
        for (self.docs.items) |d| c.yyjson_doc_free(d);
        self.docs.deinit(std.heap.c_allocator);
    }
};

fn searchArgsHasSemanticQuery(args: CStr) bool {
    const doc = mcp.readJson(args) orelse return false;
    defer c.yyjson_doc_free(doc);
    return c.yyjson_obj_get(c.yyjson_doc_get_root(doc), "semantic_query") != null;
}

fn emitFlaggedFtsResults(doc: ?*MutDoc, root: ?*MutVal, out: *const c.cbm_search_output_t, offset: c_int) void {
    _ = c.yyjson_mut_obj_add_int(doc, root, "total", out.total);
    _ = c.yyjson_mut_obj_add_str(doc, root, "search_mode", "bm25");
    const results = c.yyjson_mut_arr(doc);
    for (0..@intCast(@max(out.count, 0))) |i| {
        const sr = &out.results[i];
        const item = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_str(doc, item, "name", orEmpty(sr.node.name));
        _ = c.yyjson_mut_obj_add_str(doc, item, "qualified_name", orEmpty(sr.node.qualified_name));
        _ = c.yyjson_mut_obj_add_str(doc, item, "label", orEmpty(sr.node.label));
        _ = c.yyjson_mut_obj_add_str(doc, item, "file_path", orEmpty(sr.node.file_path));
        _ = c.yyjson_mut_obj_add_int(doc, item, "start_line", sr.node.start_line);
        _ = c.yyjson_mut_obj_add_int(doc, item, "end_line", sr.node.end_line);
        _ = c.yyjson_mut_obj_add_real(doc, item, "rank", sr.rank);
        _ = c.yyjson_mut_arr_add_val(results, item);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "results", results);
    _ = c.yyjson_mut_obj_add_bool(doc, root, "has_more", out.total > offset + out.count);
}

// search_graph continuation cursors. A cursor names the next offset of one
// result list (s = structural/BM25 rows, v = semantic rows) and binds it to
// the query arguments (everything except paging and format) and to the Zova
// generation it was issued against. A cursor replayed against other arguments
// or a re-indexed graph fails closed instead of skipping or repeating rows.
// Format: sg1.<kind>.<generation>.<query hash>.<offset>
const SG_CURSOR_SZ = 96;
const SG_SEMANTIC_MAX_LIMIT = 500;
const SG_SEMANTIC_MAX_OFFSET = 99998;

fn sgIsPagingKey(key: []const u8) bool {
    for ([_][]const u8{ "project", "offset", "limit", "cursor", "semantic_offset", "semantic_limit", "semantic_cursor", "format" }) |k| {
        if (mcp.eql(key, k)) return true;
    }
    return false;
}

fn sgFnv(h_in: u64, s: CStr) u64 {
    var h = h_in;
    if (s != null) {
        for (std.mem.span(s)) |ch| h = (h ^ ch) *% 1099511628211;
    }
    return (h ^ 0x1f) *% 1099511628211; // field separator
}

fn cStrLess(_: void, a: CStr, b: CStr) bool {
    return c.strcmp(a, b) < 0;
}

/// FNV-1a over the project and every non-paging argument in key order, so a
/// client may reorder arguments between pages. Returns false on OOM.
fn sgQueryHash(args_root: ?*Val, project: CStr, out: *u64) bool {
    var h = sgFnv(1469598103934665603, project);
    const n: usize = if (c.yyjson_is_obj(args_root)) c.yyjson_obj_size(args_root) else 0;
    var keys: []CStr = &.{};
    if (n > 0) keys = std.heap.c_allocator.alloc(CStr, n) catch return false;
    defer if (n > 0) std.heap.c_allocator.free(keys);
    var count: usize = 0;
    if (n > 0) {
        var it = c.yyjson_obj_iter_with(args_root);
        while (c.yyjson_obj_iter_next(&it)) |key| {
            if (!sgIsPagingKey(std.mem.span(c.yyjson_get_str(key)))) {
                keys[count] = c.yyjson_get_str(key);
                count += 1;
            }
        }
    }
    std.sort.pdq(CStr, keys[0..count], {}, cStrLess);
    var ok = true;
    for (keys[0..count]) |key| {
        if (!ok) break;
        const encoded = c.yyjson_val_write(c.yyjson_obj_get(args_root, key), 0, null);
        ok = encoded != null;
        h = sgFnv(sgFnv(h, key), encoded);
        c.free(encoded);
    }
    out.* = h;
    return ok;
}

fn sgCursorFormat(out: []u8, kind: u8, generation: i64, hash: u64, offset: c_int) [*:0]u8 {
    return fmtZ(out, "sg1.{c}.{d}.{x:0>16}.{d}", .{ kind, generation, hash, offset });
}

/// A cursor as sscanf("sg1.%c.%lld.%16llx.%d") reads it: the fields are
/// parsed leniently (whitespace, signs, a 0x prefix, trailing text) and the
/// canonical re-format then rejects anything that was not exact.
const ParsedCursor = struct { kind: u8, generation: i64, hash: u64, offset: c_int };

fn scanCursor(token: []const u8) ?ParsedCursor {
    if (!std.mem.startsWith(u8, token, "sg1.") or token.len < 6 or token[5] != '.') return null;
    var p: usize = 6;
    const generation = scanInt(i64, token, &p, 10, null) orelse return null;
    if (p >= token.len or token[p] != '.') return null;
    p += 1;
    const hash = scanInt(u64, token, &p, 16, 16) orelse return null;
    if (p >= token.len or token[p] != '.') return null;
    p += 1;
    const offset = scanInt(c_int, token, &p, 10, null) orelse return null;
    return .{ .kind = token[4], .generation = generation, .hash = hash, .offset = offset };
}

/// One scanf integer conversion at token[p.*]: leading whitespace, an
/// optional sign, an optional 0x prefix for base 16, then digits (at most
/// width characters after the whitespace). Out-of-range values clamp like
/// strtoll; the caller's canonical check rejects them anyway.
fn scanInt(comptime T: type, token: []const u8, p: *usize, base: u8, width: ?usize) ?T {
    while (p.* < token.len and std.ascii.isWhitespace(token[p.*])) p.* += 1;
    const limit = if (width) |w| @min(token.len, p.* + w) else token.len;
    var i = p.*;
    var negative = false;
    if (i < limit and (token[i] == '+' or token[i] == '-')) {
        negative = token[i] == '-';
        i += 1;
    }
    if (base == 16 and i + 1 < limit and token[i] == '0' and (token[i + 1] == 'x' or token[i + 1] == 'X')) i += 2;
    const digits_start = i;
    while (i < limit and std.ascii.isHex(token[i]) and (base == 16 or std.ascii.isDigit(token[i]))) i += 1;
    if (i == digits_start) return null;
    p.* = i;
    const magnitude = std.fmt.parseInt(u128, token[digits_start..i], base) catch std.math.maxInt(u128);
    if (@typeInfo(T).int.signedness == .unsigned) {
        const v = std.math.cast(T, magnitude) orelse std.math.maxInt(T);
        return if (negative) 0 -% v else v;
    }
    const wide: i129 = if (negative) -@as(i129, magnitude) else magnitude;
    return std.math.cast(T, wide) orelse if (negative) std.math.minInt(T) else std.math.maxInt(T);
}

/// Resolve the start offset of one result list from its cursor or offset
/// argument. Returns null on success, otherwise an error written to err.
fn sgResumeOffset(args_root: ?*Val, cursor_key: Str, offset_key: Str, kind: u8, generation: i64, hash: u64, offset: *c_int, err: []u8) ?[*:0]const u8 {
    const cursor = c.yyjson_obj_get(args_root, cursor_key);
    if (cursor == null) return null;
    const ck = std.mem.span(cursor_key);
    if (c.yyjson_obj_get(args_root, offset_key) != null) {
        return fmtZ(err, "{s} and {s} are mutually exclusive", .{ ck, std.mem.span(offset_key) });
    }
    const token = c.yyjson_get_str(cursor);
    const parsed = if (token != null) scanCursor(std.mem.span(token)) else null;
    if (parsed == null or parsed.?.kind != kind or parsed.?.offset < 0) {
        return fmtZ(err, "invalid_cursor: {s} is not a {s} cursor from this tool; re-run the query " ++
            "without it", .{ ck, if (kind == 'v') "semantic" else "structural" });
    }
    const pc = parsed.?;
    var canonical: [SG_CURSOR_SZ]u8 = undefined;
    if (!mcp.cEql(sgCursorFormat(&canonical, pc.kind, pc.generation, pc.hash, pc.offset), token)) {
        return fmtZ(err, "invalid_cursor: {s} is malformed; re-run the query without it", .{ck});
    }
    if (pc.hash != hash) {
        return fmtZ(err, "invalid_cursor: {s} was issued for different query arguments; re-run the " ++
            "query without it", .{ck});
    }
    if (pc.generation != generation) {
        return fmtZ(err, "stale_cursor: the project was re-indexed since {s} was issued (generation " ++
            "{d}, now {d}); re-run the query without it", .{ ck, pc.generation, generation });
    }
    offset.* = pc.offset;
    return null;
}

const SEMANTIC_TYPE_ERROR = "semantic_query must be an array of keyword strings, e.g. " ++
    "[\"send\",\"pubsub\",\"publish\"] — not a single string. Split your query " ++
    "into individual keywords; each is scored independently via per-keyword " ++
    "min-cosine.";

/// The string arguments of a search_graph request (all heap, may be NULL).
const SearchStrings = struct {
    query: HeapStr,
    name_pattern: HeapStr,
    qn_pattern: HeapStr,
    label: HeapStr,
    file_pattern: HeapStr,
    relationship: HeapStr,

    fn read(args: CStr) SearchStrings {
        return .{
            .query = strArg(args, "query"),
            .name_pattern = strArg(args, "name_pattern"),
            .qn_pattern = strArg(args, "qn_pattern"),
            .label = strArg(args, "label"),
            .file_pattern = strArg(args, "file_pattern"),
            .relationship = strArg(args, "relationship"),
        };
    }

    fn deinit(self: *SearchStrings) void {
        inline for (@typeInfo(SearchStrings).@"struct".field_names) |name| c.free(@field(self, name));
    }
};

fn handleSearchGraphFlagged(args: CStr, project: HeapStr) HeapStr {
    var path: [1024]u8 = undefined;
    if (project == null or project[0] == 0) return mcp.projectNotFoundResult(project);
    const repo = mcp.openRepository(&path, project) orelse return mcp.projectNotFoundResult(project);
    defer c.cbm_zova_repository_close(repo);
    defer c.free(project);
    const workspace_id = c.cbm_zova_repository_workspace_id(repo);
    // Cursors bind to the generation read before the search; the search is
    // accepted only if the same generation is still published after it.
    var generation: i64 = 0;
    var indexed_at: HeapStr = null;
    const status_rc = c.cbm_zova_repository_index_status(repo, workspace_id, &generation, &indexed_at);
    c.free(indexed_at);
    const args_doc = mcp.readJson(args);
    const args_root: ?*Val = if (args_doc) |d| c.yyjson_doc_get_root(d) else null;
    var query_hash: u64 = 0;
    if (status_rc != c.CBM_STORE_OK or !sgQueryHash(args_root, project, &query_hash)) {
        c.yyjson_doc_free(args_doc);
        return textResult("could not bind search_graph request to a generation", true);
    }
    var offset = intArg(args, "offset", 0);
    var semantic_offset = intArg(args, "semantic_offset", 0);
    var semantic_limit = intArg(args, "semantic_limit", 0);
    var paging_error: [512]u8 = undefined;
    var paging_failure = sgResumeOffset(args_root, "cursor", "offset", 's', generation, query_hash, &offset, &paging_error);
    if (paging_failure == null) {
        paging_failure = sgResumeOffset(args_root, "semantic_cursor", "semantic_offset", 'v', generation, query_hash, &semantic_offset, &paging_error);
    }
    if (paging_failure == null and (semantic_offset < 0 or semantic_offset > SG_SEMANTIC_MAX_OFFSET)) {
        paging_failure = "semantic_offset must be between 0 and 99998";
    }
    const semantic_limit_given = c.yyjson_obj_get(args_root, "semantic_limit") != null;
    if (paging_failure == null and semantic_limit_given and (semantic_limit < 1 or semantic_limit > SG_SEMANTIC_MAX_LIMIT)) {
        paging_failure = "semantic_limit must be between 1 and 500";
    }
    c.yyjson_doc_free(args_doc);
    if (paging_failure) |msg| return textResult(msg, true);

    var s = SearchStrings.read(args);
    defer s.deinit();
    const has_query = hasText(s.query);
    var params = std.mem.zeroes(c.cbm_search_params_t);
    params.project = project;
    params.label = s.label;
    params.name_pattern = if (has_query) s.query else s.name_pattern;
    params.qn_pattern = s.qn_pattern;
    params.file_pattern = s.file_pattern;
    params.relationship = s.relationship;
    params.exclude_entry_points = boolArg(args, "exclude_entry_points");
    params.include_connected = false;
    params.limit = intArg(args, "limit", if (has_query) BM25_DEFAULT_LIMIT else c.CBM_DEFAULT_SEARCH_LIMIT);
    params.offset = offset;
    params.min_degree = intArg(args, "min_degree", CBM_NOT_FOUND);
    params.max_degree = intArg(args, "max_degree", CBM_NOT_FOUND);
    var out = std.mem.zeroes(c.cbm_search_output_t);
    defer c.cbm_store_search_free(&out);
    // Semantic-only calls skip the structural search: with no structural
    // filter, running the UNFILTERED regex search would prepend unrelated
    // enriched nodes to a purely semantic request — same gate as the
    // compatibility route. (Upstream 108740d8.)
    const has_structural_filters = !searchArgsHasSemanticQuery(args) or s.name_pattern != null or
        s.qn_pattern != null or s.label != null or s.file_pattern != null or s.relationship != null or
        params.min_degree != CBM_NOT_FOUND or params.max_degree != CBM_NOT_FOUND;
    var rc: c_int = c.CBM_STORE_OK;
    var fts_query: [BM25_QUERY_BUF]u8 = undefined;
    if (has_query) {
        if (zovaFtsMatch(s.query, &fts_query)) {
            rc = c.cbm_zova_repository_search_fts(repo, workspace_id, &fts_query, s.file_pattern, s.label, s.query, params.limit, params.offset, &out);
        }
    } else if (has_structural_filters) {
        rc = c.cbm_zova_repository_search(repo, workspace_id, &params, &out);
    }
    const doc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(doc);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    var props_docs: PropDocs = .{};
    defer props_docs.deinit();
    if (rc == c.CBM_STORE_OK) {
        if (has_query) {
            emitFlaggedFtsResults(doc, root, &out, offset);
        } else if (has_structural_filters) {
            emitSearchResults(doc, root, &out, offset, &props_docs);
        }
    }
    const structural_ran = has_query or has_structural_filters;
    if (rc == c.CBM_STORE_OK and structural_ran and out.count > 0 and out.total > offset + out.count) {
        var cursor: [SG_CURSOR_SZ]u8 = undefined;
        _ = c.yyjson_mut_obj_add_strcpy(doc, root, "next_cursor", sgCursorFormat(&cursor, 's', generation, query_hash, offset + out.count));
    }
    if (!semantic_limit_given) {
        semantic_limit = if (params.limit > 0) @min(params.limit, SG_SEMANTIC_MAX_LIMIT) else 16;
    }
    var semantic_type_error = false;
    var semantic_next: c_int = -1;
    const vector_store = c.cbm_store_open_zova_workspace_query(&path, workspace_id);
    if (rc == c.CBM_STORE_OK and vector_store != null) {
        semantic_type_error = runSemanticQuery(doc, root, args, vector_store, project, semantic_offset, semantic_limit, &semantic_next);
    }
    if (vector_store != null) c.cbm_store_close(vector_store);
    if (semantic_next > 0) {
        var cursor: [SG_CURSOR_SZ]u8 = undefined;
        _ = c.yyjson_mut_obj_add_strcpy(doc, root, "semantic_next_cursor", sgCursorFormat(&cursor, 'v', generation, query_hash, semantic_next));
    }
    var generation_after: i64 = 0;
    var indexed_after: HeapStr = null;
    // A publish that landed mid-request may have mixed generations.
    const generation_changed = rc == c.CBM_STORE_OK and
        (c.cbm_zova_repository_index_status(repo, workspace_id, &generation_after, &indexed_after) != c.CBM_STORE_OK or
            generation_after != generation);
    c.free(indexed_after);
    if (semantic_type_error) return textResult(SEMANTIC_TYPE_ERROR, true);
    const json = if (rc == c.CBM_STORE_OK) mcp.docToStr(doc) else null;
    if (generation_changed) {
        c.free(json);
        return textResult("the project was re-indexed during search_graph; retry the request", true);
    }
    if (json == null) return textResult("shared cbm.zova search failed", true);
    return mcp.textResultOwned(json, false);
}

const MAX_KEYWORDS = 32;

/// Copy keyword strings from a yyjson array into keywords. Returns the
/// number of strings copied (capped at keywords.len).
fn extractSemanticKeywords(sq_val: ?*Val, keywords: *[MAX_KEYWORDS]CStr) c_int {
    const kw_count = @min(c.yyjson_arr_size(sq_val), keywords.len);
    var ki: usize = 0;
    var it: c.yyjson_arr_iter = undefined;
    _ = c.yyjson_arr_iter_init(sq_val, &it);
    while (c.yyjson_arr_iter_next(&it)) |kw_val| {
        if (ki < kw_count and c.yyjson_is_str(kw_val)) {
            keywords[ki] = c.yyjson_get_str(kw_val);
            ki += 1;
        }
    }
    return @intCast(ki);
}

/// Emit vector results as a "semantic_results" array on the doc.
fn emitSemanticResults(doc: ?*MutDoc, root: ?*MutVal, vresults: [*c]c.cbm_vector_result_t, vcount: c_int) void {
    const sem_results = c.yyjson_mut_arr(doc);
    for (0..@intCast(@max(vcount, 0))) |v| {
        const r = &vresults[v];
        const vitem = c.yyjson_mut_obj(doc);
        if (r.project != null) _ = c.yyjson_mut_obj_add_strcpy(doc, vitem, "project", r.project);
        _ = c.yyjson_mut_obj_add_strcpy(doc, vitem, "name", r.name);
        _ = c.yyjson_mut_obj_add_strcpy(doc, vitem, "qualified_name", r.qualified_name);
        _ = c.yyjson_mut_obj_add_strcpy(doc, vitem, "label", r.label);
        _ = c.yyjson_mut_obj_add_strcpy(doc, vitem, "file_path", r.file_path);
        const score = @round(r.score * 1e12) / 1e12;
        _ = c.yyjson_mut_obj_add_real(doc, vitem, "score", score);
        _ = c.yyjson_mut_arr_add_val(sem_results, vitem);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "semantic_results", sem_results);
}

/// Append one page of semantic_query vector-search results onto the doc:
/// ranks [offset, offset+limit). One extra ranked hit is materialized so
/// semantic_has_more is exact; semantic_total is exact ("eq") only when the
/// search ran out of hits, otherwise a lower bound ("gte"). next_offset is
/// the next rank when more remain, else -1. Returns true if semantic_query was
/// provided as a non-array (type error — the caller surfaces it).
fn runSemanticQuery(doc: ?*MutDoc, root: ?*MutVal, args: CStr, store: ?*c.cbm_store_t, project: CStr, offset: c_int, limit: c_int, next_offset: *c_int) bool {
    next_offset.* = -1;
    const args_doc = mcp.readJson(args) orelse return false;
    defer c.yyjson_doc_free(args_doc);
    const sq_val = c.yyjson_obj_get(c.yyjson_doc_get_root(args_doc), "semantic_query");
    if (sq_val != null and !c.yyjson_is_arr(sq_val)) return true;
    if (sq_val == null or c.yyjson_arr_size(sq_val) == 0) return false;
    var keywords: [MAX_KEYWORDS]CStr = undefined;
    const ki = extractSemanticKeywords(sq_val, &keywords);
    var vresults: [*c]c.cbm_vector_result_t = null;
    var vcount: c_int = 0;
    const materialize = offset + limit + 1;
    if (c.cbm_store_vector_search(store, project, @ptrCast(&keywords), ki, materialize, &vresults, &vcount) == c.CBM_STORE_OK) {
        const returned = @min(if (vcount > offset) vcount - offset else 0, limit);
        const has_more = vcount > offset + returned;
        _ = c.yyjson_mut_obj_add_int(doc, root, "semantic_total", vcount);
        _ = c.yyjson_mut_obj_add_str(doc, root, "semantic_total_relation", if (vcount < materialize) "eq" else "gte");
        _ = c.yyjson_mut_obj_add_int(doc, root, "semantic_returned", returned);
        _ = c.yyjson_mut_obj_add_bool(doc, root, "semantic_has_more", has_more);
        emitSemanticResults(doc, root, if (returned > 0) vresults + @as(usize, @intCast(offset)) else null, returned);
        if (has_more and returned > 0) next_offset.* = offset + returned;
        c.cbm_store_free_vector_results(vresults, vcount);
    }
    return false;
}

fn emitCatalogSearchResults(doc: ?*MutDoc, root: ?*MutVal, out: *const c.cbm_search_output_t, offset: c_int, bm25: bool) void {
    _ = c.yyjson_mut_obj_add_int(doc, root, "total", out.total);
    if (bm25) _ = c.yyjson_mut_obj_add_str(doc, root, "search_mode", "bm25");
    const results = c.yyjson_mut_arr(doc);
    for (0..@intCast(@max(out.count, 0))) |i| {
        const result = &out.results[i];
        const item = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_str(doc, item, "project", orEmpty(result.node.project));
        _ = c.yyjson_mut_obj_add_str(doc, item, "name", orEmpty(result.node.name));
        _ = c.yyjson_mut_obj_add_str(doc, item, "qualified_name", orEmpty(result.node.qualified_name));
        _ = c.yyjson_mut_obj_add_str(doc, item, "label", orEmpty(result.node.label));
        _ = c.yyjson_mut_obj_add_str(doc, item, "file_path", orEmpty(result.node.file_path));
        _ = c.yyjson_mut_obj_add_int(doc, item, "start_line", result.node.start_line);
        _ = c.yyjson_mut_obj_add_int(doc, item, "end_line", result.node.end_line);
        if (bm25) {
            _ = c.yyjson_mut_obj_add_real(doc, item, "rank", result.rank);
        } else {
            _ = c.yyjson_mut_obj_add_int(doc, item, "in_degree", result.in_degree);
            _ = c.yyjson_mut_obj_add_int(doc, item, "out_degree", result.out_degree);
        }
        _ = c.yyjson_mut_arr_add_val(results, item);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "results", results);
    _ = c.yyjson_mut_obj_add_bool(doc, root, "has_more", out.total > offset + out.count);
}

fn emitCatalogScope(doc: ?*MutDoc, root: ?*MutVal, scope: *const c.cbm_zova_catalog_scope_t) void {
    const scope_json = c.yyjson_mut_obj(doc);
    const projects = c.yyjson_mut_arr(doc);
    const generations = c.yyjson_mut_obj(doc);
    for (0..@intCast(@max(scope.count, 0))) |i| {
        _ = c.yyjson_mut_arr_add_strcpy(doc, projects, scope.projects[i].selector);
        _ = c.yyjson_mut_obj_add_int(doc, generations, scope.projects[i].selector, scope.projects[i].generation);
    }
    _ = c.yyjson_mut_obj_add_val(doc, scope_json, "projects", projects);
    _ = c.yyjson_mut_obj_add_val(doc, scope_json, "generations", generations);
    const excluded = c.yyjson_mut_arr(doc);
    for (0..@intCast(@max(scope.excluded_count, 0))) |i| {
        const project = &scope.excluded_projects[i];
        const entry = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_strcpy(doc, entry, "project", project.selector);
        const reason: CStr = if (!project.ready)
            "no ready generation"
        else if (hasText(project.health_reason))
            project.health_reason
        else
            "workspace unhealthy";
        _ = c.yyjson_mut_obj_add_str(doc, entry, "reason", reason);
        _ = c.yyjson_mut_arr_add_val(excluded, entry);
    }
    _ = c.yyjson_mut_obj_add_val(doc, scope_json, "excluded_projects", excluded);
    _ = c.yyjson_mut_obj_add_val(doc, root, "scope", scope_json);
}

fn handleSearchGraphCatalog(args: CStr, requested: *const mcp.ProjectScope) HeapStr {
    if (boolArg(args, "include_connected")) {
        return textResult("include_connected is not supported for multi-project search", true);
    }
    const args_doc = mcp.readJson(args);
    defer c.yyjson_doc_free(args_doc);
    const args_root: ?*Val = if (args_doc) |d| c.yyjson_doc_get_root(d) else null;
    const semantic = if (args_root != null) c.yyjson_obj_get(args_root, "semantic_query") else null;
    if (semantic != null and (!c.yyjson_is_arr(semantic) or c.yyjson_arr_size(semantic) == 0)) {
        return textResult("semantic_query must be a non-empty array of keyword strings", true);
    }
    if (semantic != null) {
        var it: c.yyjson_arr_iter = undefined;
        _ = c.yyjson_arr_iter_init(semantic, &it);
        while (c.yyjson_arr_iter_next(&it)) |keyword| {
            if (!c.yyjson_is_str(keyword) or c.yyjson_get_str(keyword)[0] == 0) {
                return textResult("semantic_query entries must be non-empty strings", true);
            }
        }
    }

    var path: [1024]u8 = undefined;
    if (c.cbm_zova_user_database_path(&path, path.len) != 0) {
        return textResult("cannot resolve shared Zova database", true);
    }
    const catalog = c.cbm_zova_catalog_open(&path) orelse return textResult("cannot open shared Zova catalog", true);
    defer c.cbm_zova_catalog_close(catalog);
    var scope = std.mem.zeroes(c.cbm_zova_catalog_scope_t);
    defer c.cbm_zova_catalog_scope_free(&scope);
    const resolve_status = c.cbm_zova_catalog_resolve(catalog, @ptrCast(requested.projects), @intCast(requested.project_count), requested.wildcard, &scope);
    if (resolve_status != c.CBM_STORE_OK or scope.count == 0) {
        const catalog_error = c.cbm_zova_catalog_error(catalog);
        if (catalog_error[0] != 0) return textResult(catalog_error, true);
        return textResult(if (resolve_status == c.CBM_STORE_NOT_FOUND)
            "one or more requested projects were not found"
        else
            "no ready healthy projects matched the scope", true);
    }

    var s = SearchStrings.read(args);
    defer s.deinit();
    const has_query = hasText(s.query);
    const offset = intArg(args, "offset", 0);
    const limit = intArg(args, "limit", if (has_query) BM25_DEFAULT_LIMIT else c.CBM_DEFAULT_SEARCH_LIMIT);
    var output = std.mem.zeroes(c.cbm_search_output_t);
    defer c.cbm_store_search_free(&output);
    var semantic_results: [*c]c.cbm_vector_result_t = null;
    var semantic_count: c_int = 0;
    defer c.cbm_store_free_vector_results(semantic_results, semantic_count);
    var semantic_total: c_int = 0;
    var search_status: c_int = c.CBM_STORE_ERR;
    if (semantic != null) {
        var keywords: [MAX_KEYWORDS]CStr = undefined;
        const keyword_count = extractSemanticKeywords(semantic, &keywords);
        if (keyword_count > 0) {
            search_status = c.cbm_zova_catalog_search_semantic(catalog, &path, &scope, @ptrCast(&keywords), keyword_count, limit, offset, &semantic_results, &semantic_count, &semantic_total);
        }
    } else if (s.relationship == null or validateEdgeType(s.relationship)) {
        var fts_query: [BM25_QUERY_BUF]u8 = undefined;
        if (has_query) {
            search_status = if (zovaFtsMatch(s.query, &fts_query))
                c.cbm_zova_catalog_search_fts(catalog, &scope, &fts_query, s.file_pattern, s.label, s.query, limit, offset, &output)
            else
                c.CBM_STORE_OK;
        } else {
            var params = std.mem.zeroes(c.cbm_search_params_t);
            params.label = s.label;
            params.name_pattern = s.name_pattern;
            params.qn_pattern = s.qn_pattern;
            params.file_pattern = s.file_pattern;
            params.relationship = s.relationship;
            params.exclude_entry_points = boolArg(args, "exclude_entry_points");
            params.limit = limit;
            params.offset = offset;
            params.min_degree = intArg(args, "min_degree", CBM_NOT_FOUND);
            params.max_degree = intArg(args, "max_degree", CBM_NOT_FOUND);
            search_status = c.cbm_zova_catalog_search(catalog, &scope, &params, &output);
        }
    }

    if (search_status != c.CBM_STORE_OK) {
        if (search_status == c.CBM_ZOVA_CATALOG_INCOMPATIBLE_MODELS)
            return textResult("selected projects use incompatible semantic models or dimensions", true);
        if (search_status == c.CBM_ZOVA_CATALOG_STALE_SCOPE)
            return textResult("project generations changed during semantic search; retry the request", true);
        return textResult("multi-project search failed", true);
    }
    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    if (semantic != null) {
        emitSemanticResults(doc, root, semantic_results, semantic_count);
        _ = c.yyjson_mut_obj_add_int(doc, root, "total", semantic_total);
        _ = c.yyjson_mut_obj_add_bool(doc, root, "has_more", semantic_total > offset + semantic_count);
    } else {
        emitCatalogSearchResults(doc, root, &output, offset, has_query);
    }
    emitCatalogScope(doc, root, &scope);
    const json = mcp.docToStr(doc);
    c.yyjson_mut_doc_free(doc);
    if (json == null) return textResult("multi-project search failed", true);
    return mcp.textResultOwned(json, false);
}

pub fn handleSearchGraph(_: *Server, args: CStr) HeapStr {
    var scope: mcp.ProjectScope = .{};
    var scope_error: ?Str = null;
    if (mcp.projectScopeParse(args, &scope, &scope_error) != 0) {
        scope.deinit();
        return textResult(scope_error orelse "invalid project scope", true);
    }
    if (scope.project_count > 0) {
        defer scope.deinit();
        const args_doc = mcp.readJson(args);
        defer c.yyjson_doc_free(args_doc);
        const args_root: ?*Val = if (args_doc) |d| c.yyjson_doc_get_root(d) else null;
        for ([_]Str{ "cursor", "semantic_cursor", "semantic_offset", "semantic_limit" }) |key| {
            if (c.yyjson_obj_get(args_root, key) != null) {
                var message: [128]u8 = undefined;
                return textResult(fmtZ(&message, "{s} is supported for single-project search only", .{std.mem.span(key)}), true);
            }
        }
        return handleSearchGraphCatalog(args, &scope);
    }
    const project = scope.project;
    scope.project = null;
    scope.deinit();
    return handleSearchGraphFlagged(args, project);
}

// ── query_graph ──────────────────────────────────────────────────

pub fn handleQueryGraph(_: *Server, args: CStr) HeapStr {
    const query = strArg(args, "query");
    const project = mcp.getProjectArg(args);
    if (query == null) {
        c.free(project);
        return textResult("query is required", true);
    }
    defer c.free(query);
    var path: [1024]u8 = undefined;
    const repo = mcp.openRepository(&path, project) orelse return mcp.projectNotFoundResult(project);
    defer c.free(project);
    if (c.cbm_zova_workspace_id_validate(c.cbm_zova_repository_workspace_id(repo)) != 0) {
        c.cbm_zova_repository_close(repo);
        return textResult("workspace-scoped Zova Cypher request is invalid", true);
    }
    const workspace_id = c.cbm_zova_repository_workspace_id(repo);
    var workspace_copy: [c.CBM_ZOVA_WORKSPACE_ID_MAX]u8 = undefined;
    _ = mcp.bprint(&workspace_copy, "{s}", .{std.mem.span(workspace_id)});
    const ws = mcp.bufStr(&workspace_copy);
    var generation: i64 = 0;
    var indexed_at: HeapStr = null;
    const status_rc = c.cbm_zova_repository_index_status(repo, workspace_id, &generation, &indexed_at);
    c.free(indexed_at);
    if (status_rc != c.CBM_STORE_OK) {
        c.cbm_zova_repository_close(repo);
        return textResult("could not bind Cypher request generation", true);
    }
    var plan: ?*c.cbm_query_t = null;
    var parse_error: HeapStr = null;
    defer c.free(parse_error);
    if (c.cbm_cypher_parse(query, @ptrCast(&plan), &parse_error) != 0) {
        c.cbm_zova_repository_close(repo);
        var diagnostic: [1024]u8 = undefined;
        return textResult(fmtZ(&diagnostic, "route=unsupported workspace={s} operator=parser reason={s}", .{ ws, if (parse_error != null) std.mem.span(parse_error) else "Cypher parse failed" }), true);
    }
    var route_reason: CStr = null;
    const route = c.cbm_cypher_classify_plan(plan, &route_reason);
    c.cbm_query_free(plan);
    if (route == c.CBM_CYPHER_ROUTE_UNSUPPORTED) {
        c.cbm_zova_repository_close(repo);
        var diagnostic: [1024]u8 = undefined;
        return textResult(fmtZ(&diagnostic, "route=unsupported workspace={s} operator=logical_plan reason={s}", .{ ws, if (route_reason != null) std.mem.span(route_reason) else "unsupported query shape" }), true);
    }
    const zova_store = c.cbm_store_open_zova_workspace_query(&path, workspace_id);
    c.cbm_zova_repository_close(repo);
    if (zova_store == null) return textResult("could not open workspace-scoped Cypher session", true);
    const max_rows = intArg(args, "max_rows", 1000);
    var result = std.mem.zeroes(c.cbm_cypher_result_t);
    defer c.cbm_cypher_result_free(&result);
    const rc = c.cbm_cypher_execute(zova_store, query, project, max_rows, &result);
    c.cbm_store_close(zova_store);
    if (rc != 0) return textResult(if (result.@"error" != null) result.@"error" else "Cypher execution failed", true);

    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    const cols = c.yyjson_mut_arr(doc);
    const col_count: usize = @intCast(@max(result.col_count, 0));
    for (0..col_count) |i| _ = c.yyjson_mut_arr_add_str(doc, cols, result.columns[i]);
    _ = c.yyjson_mut_obj_add_val(doc, root, "columns", cols);
    const rows = c.yyjson_mut_arr(doc);
    for (0..@intCast(@max(result.row_count, 0))) |r| {
        const row = c.yyjson_mut_arr(doc);
        for (0..col_count) |ci| _ = c.yyjson_mut_arr_add_str(doc, row, orEmpty(result.rows[r][ci]));
        _ = c.yyjson_mut_arr_add_val(rows, row);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "rows", rows);
    _ = c.yyjson_mut_obj_add_int(doc, root, "total", result.row_count);
    if (result.warning != null) _ = c.yyjson_mut_obj_add_str(doc, root, "warning", result.warning);
    // The doc borrows result strings: serialize before result_free (deferred).
    return mcp.docResult(doc);
}
