//! mcp.zig — MCP server core: JSON-RPC 2.0 parsing and formatting, the tool
//! registry, argument extraction, the server object, and project/store
//! resolution shared by the tool handlers. Implements that part of
//! src/mcp/mcp.h; the handlers live in the sibling files this one imports.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

pub const Str = [*:0]const u8;
pub const CStr = [*c]const u8;
pub const HeapStr = [*c]u8;
pub const sp = interop.sp;
pub const bprint = interop.bprint;

pub const is_windows = builtin.os.tag == .windows;

pub const Doc = c.yyjson_doc;
pub const Val = c.yyjson_val;
pub const MutDoc = c.yyjson_mut_doc;
pub const MutVal = c.yyjson_mut_val;

pub const CBM_NOT_FOUND: c_int = -1;

/// JSON-RPC 2.0 standard error codes.
pub const JSONRPC_PARSE_ERROR = -32700;
pub const JSONRPC_METHOD_NOT_FOUND = -32601;

// ── Helpers ────────────────────────────────────────────────────────

/// Write a yyjson_mut_doc to a heap-allocated JSON string.
/// ALLOW_INVALID_UNICODE: some database strings may contain non-UTF-8 bytes
/// from older indexing runs — don't fail serialization over it.
pub fn docToStr(doc: ?*MutDoc) HeapStr {
    return c.yyjson_mut_write(doc, c.YYJSON_WRITE_ALLOW_INVALID_UNICODE, null);
}

/// Parse a NUL-terminated JSON string; null for NULL or invalid input.
pub fn readJson(json: CStr) ?*Doc {
    if (json == null) return null;
    return c.yyjson_read(json, c.strlen(json), 0);
}

/// snprintf into buf and return it as a C string.
pub fn fmtZ(buf: []u8, comptime fmt: []const u8, args: anytype) [*:0]u8 {
    _ = bprint(buf, fmt, args);
    return @ptrCast(buf.ptr);
}

/// The NUL-terminated contents of a fixed buffer.
pub fn bufStr(buf: []const u8) []const u8 {
    return std.mem.sliceTo(buf, 0);
}

pub fn eql(a: []const u8, b: []const u8) bool {
    return std.mem.eql(u8, a, b);
}

/// strcmp(a, b) == 0 for two non-NULL C strings.
pub fn cEql(a: CStr, b: CStr) bool {
    return c.strcmp(a, b) == 0;
}

/// A heap copy of a static string, freed by the caller with free().
pub fn dup(s: CStr) HeapStr {
    return c.cbm_heap_strdup(s);
}

/// Text result of a static or caller-owned message.
pub fn textResult(text: CStr, is_error: bool) HeapStr {
    return cbm_mcp_text_result(text, is_error);
}

/// Text result built from a heap JSON string, which is freed.
pub fn textResultOwned(json: HeapStr, is_error: bool) HeapStr {
    const res = cbm_mcp_text_result(json, is_error);
    c.free(json);
    return res;
}

/// Serialize doc (which is freed) and wrap it as a successful text result.
pub fn docResult(doc: ?*MutDoc) HeapStr {
    const json = docToStr(doc);
    c.yyjson_mut_doc_free(doc);
    return textResultOwned(json, false);
}

// ══════════════════════════════════════════════════════════════════
//  JSON-RPC PARSING
// ══════════════════════════════════════════════════════════════════

pub export fn cbm_jsonrpc_parse(line: [*c]const u8, out: [*c]c.cbm_jsonrpc_request_t) c_int {
    out.* = std.mem.zeroes(c.cbm_jsonrpc_request_t);
    out.*.id = CBM_NOT_FOUND;

    const doc = readJson(line) orelse return CBM_NOT_FOUND;
    defer c.yyjson_doc_free(doc);

    const root = c.yyjson_doc_get_root(doc);
    if (!c.yyjson_is_obj(root)) return CBM_NOT_FOUND;

    const v_jsonrpc = c.yyjson_obj_get(root, "jsonrpc");
    const v_method = c.yyjson_obj_get(root, "method");
    const v_id = c.yyjson_obj_get(root, "id");
    const v_params = c.yyjson_obj_get(root, "params");

    if (v_method == null or !c.yyjson_is_str(v_method)) return CBM_NOT_FOUND;

    out.*.jsonrpc = dup(if (v_jsonrpc != null and c.yyjson_is_str(v_jsonrpc)) c.yyjson_get_str(v_jsonrpc) else "2.0");
    out.*.method = dup(c.yyjson_get_str(v_method));

    if (v_id != null) {
        out.*.has_id = true;
        if (c.yyjson_is_int(v_id)) {
            out.*.id = c.yyjson_get_int(v_id);
        } else if (c.yyjson_is_str(v_id)) {
            // JSON-RPC 2.0 §4 permits string ids (Claude Desktop uses them).
            // Preserve verbatim instead of coercing via strtol (issue #253).
            out.*.id_str = dup(c.yyjson_get_str(v_id));
        }
    }

    if (v_params != null) out.*.params_raw = c.yyjson_val_write(v_params, 0, null);
    return 0;
}

pub export fn cbm_jsonrpc_request_free(r: [*c]c.cbm_jsonrpc_request_t) void {
    if (r == null) return;
    interop.strFree(&r.*.jsonrpc);
    interop.strFree(&r.*.method);
    interop.strFree(&r.*.id_str);
    interop.strFree(&r.*.params_raw);
    r.* = std.mem.zeroes(c.cbm_jsonrpc_request_t);
}

// ══════════════════════════════════════════════════════════════════
//  JSON-RPC FORMATTING
// ══════════════════════════════════════════════════════════════════

/// Parse json and add it to obj under key; nothing on a parse failure.
fn addParsed(doc: ?*MutDoc, obj: ?*MutVal, key: Str, json: CStr) void {
    const parsed = readJson(json) orelse return;
    defer c.yyjson_doc_free(parsed);
    _ = c.yyjson_mut_obj_add_val(doc, obj, key, c.yyjson_val_mut_copy(doc, c.yyjson_doc_get_root(parsed)));
}

pub export fn cbm_jsonrpc_format_response(resp: [*c]const c.cbm_jsonrpc_response_t) [*c]u8 {
    const doc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(doc);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);

    _ = c.yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    if (resp.*.id_str != null) {
        _ = c.yyjson_mut_obj_add_str(doc, root, "id", resp.*.id_str);
    } else {
        _ = c.yyjson_mut_obj_add_int(doc, root, "id", resp.*.id);
    }

    if (resp.*.error_json != null) {
        addParsed(doc, root, "error", resp.*.error_json);
    } else if (resp.*.result_json != null) {
        addParsed(doc, root, "result", resp.*.result_json);
    } else {
        // JSON-RPC 2.0 spec: response MUST contain "result" or "error".
        _ = c.yyjson_mut_obj_add_null(doc, root, "result");
    }
    return docToStr(doc);
}

pub export fn cbm_jsonrpc_format_error(id: i64, code: c_int, message: [*c]const u8) [*c]u8 {
    const doc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(doc);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);

    _ = c.yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    _ = c.yyjson_mut_obj_add_int(doc, root, "id", id);

    const err = c.yyjson_mut_obj(doc);
    _ = c.yyjson_mut_obj_add_int(doc, err, "code", code);
    _ = c.yyjson_mut_obj_add_str(doc, err, "message", message);
    _ = c.yyjson_mut_obj_add_val(doc, root, "error", err);
    return docToStr(doc);
}

// ══════════════════════════════════════════════════════════════════
//  MCP PROTOCOL HELPERS
// ══════════════════════════════════════════════════════════════════

pub export fn cbm_mcp_text_result(text: [*c]const u8, is_error: bool) [*c]u8 {
    const doc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(doc);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);

    const body: CStr = if (text != null) text else "";
    const content = c.yyjson_mut_arr(doc);
    const item = c.yyjson_mut_obj(doc);
    _ = c.yyjson_mut_obj_add_str(doc, item, "type", "text");
    _ = c.yyjson_mut_obj_add_str(doc, item, "text", body);
    _ = c.yyjson_mut_arr_add_val(content, item);
    _ = c.yyjson_mut_obj_add_val(doc, root, "content", content);

    if (!is_error and text != null) {
        if (readJson(text)) |structured_doc| {
            defer c.yyjson_doc_free(structured_doc);
            const structured_root = c.yyjson_doc_get_root(structured_doc);
            if (c.yyjson_is_obj(structured_root)) {
                _ = c.yyjson_mut_obj_add_val(doc, root, "structuredContent", c.yyjson_val_mut_copy(doc, structured_root));
            }
        }
    }
    if (is_error) {
        // Upstream f59d24fd (#1522): errors keep a bounded machine-readable
        // structuredContent.error. Text payloads carry no structuredContent
        // key at all (no duplication per f27f0a2a/#1375, no empty {} which
        // schema-honoring clients render as the whole result). JSON-object
        // payloads keep the parsed object above.
        const structured = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_str(doc, structured, "error", body);
        _ = c.yyjson_mut_obj_add_val(doc, root, "structuredContent", structured);
    }
    _ = c.yyjson_mut_obj_add_bool(doc, root, "isError", is_error);
    return docToStr(doc);
}

pub export fn cbm_mcp_cancel_request_matches(params_json: [*c]const u8, active_id: i64, active_id_str: [*c]const u8) bool {
    const doc = readJson(params_json) orelse return false;
    defer c.yyjson_doc_free(doc);
    const request_id = c.yyjson_obj_get(c.yyjson_doc_get_root(doc), "requestId");
    if (request_id == null) return false;
    if (active_id_str != null) {
        return c.yyjson_is_str(request_id) and cEql(c.yyjson_get_str(request_id), active_id_str);
    }
    return c.yyjson_is_int(request_id) and c.yyjson_get_int(request_id) == active_id;
}

// ── Tool definitions ─────────────────────────────────────────────

const ToolDef = struct {
    name: Str,
    title: Str,
    description: Str,
    input_schema: Str, // JSON string
};

const tools = [_]ToolDef{
    .{
        .name = "index_repository",
        .title = "Index repository",
        .description = "Index a repository into the knowledge graph.",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"repo_path\":{\"type\":\"string\",\"description\":" ++
            "\"Path to the repository\"}," ++
            "\"path\":{\"type\":\"string\",\"description\":" ++
            "\"Alias for repo_path\"}," ++
            "\"mode\":{\"type\":\"string\"," ++
            "\"enum\":[\"full\",\"moderate\",\"fast\"]," ++
            "\"default\":\"full\",\"description\":\"All modes run type-aware LSP call/usage " ++
            "resolution (per-file + cross-file). full: all files + similarity/semantic edges. " ++
            "moderate: filtered files + similarity/semantic. fast: filtered files, no " ++
            "similarity/semantic.\"}," ++
            "\"name\":{\"type\":\"string\",\"description\":" ++
            "\"Override the derived project name. Non-ASCII bytes are encoded and unsafe path characters " ++
            "are normalized.\"}" ++
            "},\"required\":[\"repo_path\"]}",
    },
    .{
        .name = "search_graph",
        .title = "Search graph",
        .description = "Search the code knowledge graph for functions, classes, routes, and variables. Use INSTEAD " ++
            "OF grep/glob when finding code definitions, implementations, or relationships. Three search " ++
            "modes: (1) query='update settings' for BM25 ranked full-text search with camelCase " ++
            "splitting and structural label boosting — recommended for natural-language discovery; " ++
            "(2) name_pattern='.*regex.*' for exact pattern matching; (3) semantic_query=[...] for " ++
            "vector cosine search that bridges vocabulary (finds 'publish' when you search 'send'). " ++
            "The three modes are independent and can be combined in a single call. " ++
            "PAGINATION: results are capped at limit (default 200) — broader queries are silently " ++
            "truncated. The response always includes 'total' (full match count before limit) and " ++
            "'has_more' (true when total > offset+returned). Detect truncation with has_more, then " ++
            "page by re-calling with offset=offset+limit until has_more is false, or pass next_cursor " ++
            "back as cursor; semantic rows page independently with semantic_offset/semantic_limit or " ++
            "semantic_next_cursor. Cursors are bound to the arguments and the index generation and " ++
            "fail with stale_cursor after a re-index instead of skipping or repeating rows. Narrow " ++
            "first via label/file_pattern/min_degree before paginating large result sets.",
        .input_schema = "{\"type\":\"object\",\"properties\":{" ++
            "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\"," ++
            "\"description\":\"compact: indented key/value text with flat object arrays as " ++
            "tables; fewer bytes, same data.\"}," ++
            "\"project\":{\"type\":\"string\"}," ++
            "\"projects\":{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"string\"}," ++
            "\"description\":\"Project selectors to search together. Use [\\\"*\\\"] for all " ++
            "ready projects. Requires Zova full authority; ranking and pagination are global. " ++
            "Every result includes its owning project. Mutually exclusive with project.\"}," ++
            "\"query\":{\"type\":\"string\",\"description\":\"Natural-language or keyword full-text " ++
            "search using BM25 ranking. Tokens are split on whitespace; camelCase identifiers are " ++
            "indexed as individual words (updateCloudClient → update, cloud, client). Results are " ++
            "ranked with structural boosting: Functions/Methods +10, Routes +8, Classes/Interfaces +5. " ++
            "Noise labels (File/Folder/Module/Variable) are filtered out. When provided, name_pattern " ++
            "is ignored.\"}," ++
            "\"label\":{\"type\":\"string\"},\"name_pattern\":{\"type\":\"string\"},\"qn_pattern\":{" ++
            "\"type\":\"string\"},\"file_pattern\":{\"type\":\"string\"}," ++
            "\"relationship\":{\"type\":\"string\"},\"min_degree\":{\"type\":\"integer\"}," ++
            "\"max_degree\":{\"type\":\"integer\"},\"exclude_entry_points\":{\"type\":\"boolean\"}," ++
            "\"include_connected\":{\"type\":\"boolean\"},\"semantic_query\":{" ++
            "\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"MUST be an ARRAY of " ++
            "keyword strings (e.g. [\\\"send\\\",\\\"pubsub\\\",\\\"publish\\\"]) — NOT a single string. " ++
            "Each keyword is scored independently via per-keyword min-cosine; results reflect functions " ++
            "that score well on ALL keywords. Requires moderate/full index mode. Results appear in the " ++
            "'semantic_results' field (separate from 'results').\"}," ++
            "\"semantic_limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500," ++
            "\"description\":\"Semantic rows per call. Default: limit, capped at 500.\"}," ++
            "\"semantic_offset\":{\"type\":\"integer\",\"default\":0,\"minimum\":0," ++
            "\"maximum\":99998}," ++
            "\"semantic_cursor\":{\"type\":\"string\",\"description\":\"semantic_next_cursor " ++
            "from the previous page. Single-project only; mutually exclusive with " ++
            "semantic_offset.\"}," ++
            "\"cursor\":{\"type\":\"string\",\"description\":\"next_cursor from the previous " ++
            "page. Single-project only; mutually exclusive with offset.\"}," ++
            "\"limit\":{\"type\":" ++
            "\"integer\",\"description\":\"Max results per call. Default 200. Response carries " ++
            "'total' (full match count) and 'has_more' (true if truncated) so callers can " ++
            "detect the limit and paginate.\"},\"offset\":{\"type\":\"integer\",\"default\":0," ++
            "\"description\":\"Skip the first N matching nodes. Combine with 'limit' to page: " ++
            "increment offset by limit and re-call while has_more is true.\"}}," ++
            "\"oneOf\":[{\"required\":[\"project\"]},{\"required\":[\"projects\"]}]}",
    },
    .{
        .name = "query_graph",
        .title = "Query graph",
        .description = "Execute a Cypher query against the knowledge graph for complex multi-hop patterns, " ++
            "aggregations, and cross-service analysis. The response includes 'total' (returned " ++
            "row count). There is a hard 100k row ceiling — for broad queries add LIMIT in the " ++
            "Cypher itself or use search_graph + offset/limit pagination instead. " ++
            "COMPLEXITY / BOTTLENECKS: every Function and Method node carries queryable complexity " ++
            "properties — cyclomatic (complexity), cognitive, loop_count, loop_depth (max nested-loop " ++
            "depth, a polynomial-degree proxy), plus interprocedural transitive_loop_depth (worst-case " ++
            "nested-loop degree propagated along CALLS edges) and a recursive flag. Additional " ++
            "hot-path signals: linear_scan_in_loop (count of find/contains/indexOf-style scans inside a " ++
            "loop — the hidden O(n^2) that loop_depth misses), alloc_in_loop (allocations/appends inside " ++
            "a loop), recursion_in_loop (a self-call inside a loop), unguarded_recursion (recursion with " ++
            "no conditionally-guarded base case), param_count and max_access_depth (structure smells). " ++
            "Find all hot-path candidates in one query, e.g. MATCH (f:Function) WHERE " ++
            "f.transitive_loop_depth >= 3 OR f.linear_scan_in_loop >= 1 RETURN f.qualified_name, " ++
            "f.transitive_loop_depth, f.linear_scan_in_loop ORDER BY f.transitive_loop_depth DESC.",
        .input_schema = "{\"type\":\"object\",\"properties\":{" ++
            "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\"," ++
            "\"description\":\"compact: indented key/value text with flat object arrays as " ++
            "tables; fewer bytes, same data.\"}," ++
            "\"query\":{\"type\":\"string\",\"description\":\"Cypher " ++
            "query\"},\"project\":{\"type\":\"string\"},\"max_rows\":{\"type\":\"integer\"," ++
            "\"description\":" ++
            "\"Optional row limit. Default: unlimited up to a 100k row " ++
            "ceiling. No offset support — use search_graph for paginated browsing.\"}}," ++
            "\"required\":[\"query\",\"project\"]}",
    },
    .{
        .name = "trace_path",
        .title = "Trace path",
        .description = "Trace paths through the code graph. Modes: calls (callers/callees), data_flow (value " ++
            "propagation with args at each hop), cross_service (through HTTP/async Route nodes). " ++
            "Use INSTEAD OF grep for callers, dependencies, impact analysis, or data flow tracing.",
        .input_schema = "{\"type\":\"object\",\"properties\":{" ++
            "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\"," ++
            "\"description\":\"compact: indented key/value text with flat object arrays as " ++
            "tables; fewer bytes, same data.\"}," ++
            "\"function_name\":{\"type\":\"string\"},\"project\":{" ++
            "\"type\":\"string\"},\"file_path\":{\"type\":\"string\",\"description\":" ++
            "\"Keep only functions defined in this repository-relative file (or path suffix) when " ++
            "the name is ambiguous.\"},\"direction\":{\"type\":\"string\",\"enum\":[\"inbound\",\"outbound\"," ++
            "\"both\"],\"default\":\"both\"},\"depth\":{\"type\":\"integer\",\"default\":3},\"mode\":{" ++
            "\"type\":\"string\",\"enum\":[\"calls\",\"data_flow\",\"cross_service\"],\"default\":" ++
            "\"calls\",\"description\":\"calls: follow CALLS edges. data_flow: follow CALLS+DATA_FLOWS " ++
            "with arg expressions. cross_service: follow HTTP_CALLS+ASYNC_CALLS+DATA_FLOWS through " ++
            "Routes.\"},\"parameter_name\":{\"type\":" ++
            "\"string\",\"description\":\"For data_flow mode: " ++
            "scope trace to a specific parameter name\"},\"edge_types\":{\"type\":\"array\",\"items\":{" ++
            "\"type\":\"string\"},\"description\":\"Replaces the mode's edge types. CALL_REFERENCE " ++
            "(a function passed or stored as a value) is not followed by mode calls; pass both " ++
            "CALLS and CALL_REFERENCE to follow it.\"},\"risk_labels\":{\"type\":\"boolean\",\"default\":false," ++
            "\"description\":\"Add risk classification (CRITICAL/HIGH/MEDIUM/LOW) based on hop distance" ++
            "\"},\"include_tests\":{\"type\":\"boolean\",\"default\":false," ++
            "\"description\":\"Include test files in results. When false (default), test files are " ++
            "filtered out. When true, test nodes are included with is_test=true marker." ++
            "\"},\"include_evidence\":{\"type\":\"boolean\",\"default\":false," ++
            "\"description\":\"Add how each hop's edge was resolved: strategy (lsp | language_rule | " ++
            "heuristic | unresolved) and the resolver's confidence. Off by default. Use it to judge " ++
            "whether an edge is trustworthy, not to find edges.\"}}," ++
            "\"required\":[\"function_name\",\"project\"]}",
    },
    .{
        .name = "get_code_snippet",
        .title = "Get code snippet",
        .description = "Read source code for a function/class/symbol. IMPORTANT: First call search_graph to find the " ++
            "exact qualified_name, then pass it here. This is a read tool, not a search tool. Accepts " ++
            "full qualified_name (exact match) or short function name (returns suggestions if ambiguous).",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"qualified_name\":{\"type\":\"string\",\"description\":" ++
            "\"Full qualified_name from search_graph, or short function name\"},\"project\":{" ++
            "\"type\":\"string\"},\"file_path\":{\"type\":\"string\",\"description\":" ++
            "\"Keep only matches defined in this repository-relative file (or path suffix) when a " ++
            "short name is ambiguous.\"},\"include_neighbors\":{" ++
            "\"type\":\"boolean\",\"default\":false}},\"required\":[\"qualified_name\",\"project\"]}",
    },
    .{
        .name = "get_file_outline",
        .title = "Get file outline",
        .description = "Return a compact declaration outline for one exact repository-relative file. Results are " ++
            "filtered by optional exact labels, ordered deterministically by source position, and " ++
            "bounded with exact total/offset/limit pagination metadata. File/folder/container nodes " ++
            "are excluded.",
        .input_schema = "{\"type\":\"object\",\"properties\":{" ++
            "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\"," ++
            "\"description\":\"compact: indented key/value text with flat object arrays as " ++
            "tables; fewer bytes, same data.\"}," ++
            "\"project\":{\"type\":\"string\"}," ++
            "\"file_path\":{\"type\":\"string\",\"description\":\"Exact repository-relative " ++
            "file path\"},\"labels\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}," ++
            "\"maxItems\":16,\"description\":\"Optional exact node-label filter\"}," ++
            "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200,\"default\":100}," ++
            "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"default\":0}}," ++
            "\"required\":[\"project\",\"file_path\"]}",
    },
    .{
        .name = "get_graph_schema",
        .title = "Get graph schema",
        .description = "Get the schema of the knowledge graph (node labels, edge types)",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}},\"required\":[" ++
            "\"project\"]}",
    },
    .{
        .name = "compare_graphs",
        .title = "Compare graphs",
        .description = "Compare two indexed projects, for example two branches or worktrees of one repository. " ++
            "Returns target-only additions and base-only removals of nodes (qualified name without " ++
            "the project prefix, label, file path) and edges (source, target, type). Each result set " ++
            "is capped by limit and a fixed 512 KiB encoded-byte budget; exact totals and truncation " ++
            "reasons are always reported.",
        .input_schema = "{\"type\":\"object\",\"properties\":{" ++
            "\"base_project\":{\"type\":\"string\",\"minLength\":1}," ++
            "\"target_project\":{\"type\":\"string\",\"minLength\":1}," ++
            "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000,\"default\":200}," ++
            "\"scan_limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000000," ++
            "\"default\":2000000}},\"required\":[\"base_project\",\"target_project\"]," ++
            "\"additionalProperties\":false}",
    },
    .{
        .name = "get_architecture",
        .title = "Get architecture",
        .description = "Get high-level architecture overview — packages, services, dependencies, and project " ++
            "structure at a glance. Includes 'clusters': Leiden community detection over the call/import " ++
            "graph, surfacing the de-facto modules (each with a label, member count, cohesion score, " ++
            "representative top_nodes, and the packages/edge_types that bind it) — use these to grasp " ++
            "the real architectural seams, which often cut across the folder layout. Optional path scopes " ++
            "analysis to nodes under that directory prefix (file_path).",
        .input_schema = "{\"type\":\"object\",\"properties\":{" ++
            "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\"," ++
            "\"description\":\"compact: indented key/value text with flat object arrays as " ++
            "tables; fewer bytes, same data.\"}," ++
            "\"project\":{\"type\":\"string\"},\"path\":{\"type\":" ++
            "\"string\",\"description\":\"Optional directory prefix to scope architecture (e.g. " ++
            "apps/hoa)\"}," ++
            "\"aspects\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"all\"," ++
            "\"overview\",\"structure\",\"dependencies\",\"routes\",\"languages\",\"packages\"," ++
            "\"entry_points\",\"hotspots\",\"boundaries\",\"layers\",\"file_tree\",\"clusters\"]}," ++
            "\"description\":\"Aspects to include. 'all' = everything; 'overview' = compact summary " ++
            "(all except file_tree); omit = all.\"}},\"required\":[\"project\"]}",
    },
    .{
        .name = "search_code",
        .title = "Search code",
        .description = "Graph-augmented code search. Finds text patterns via grep, then enriches results with " ++
            "the knowledge graph: deduplicates matches into containing functions, ranks by structural " ++
            "importance (definitions first, popular functions next, tests last). " ++
            "Modes: compact (default, signatures only — token efficient), full (with source), " ++
            "files (just file paths). Use path_filter regex to scope results. " ++
            "TRUNCATION: enriched results are capped at limit (default 10). Response carries " ++
            "'total_grep_matches' (raw grep hit count) and 'total_results' (deduplicated function " ++
            "count) — compare to limit to detect truncation. There is no offset parameter; to see " ++
            "more, raise limit or narrow the query with file_pattern / path_filter.",
        .input_schema = "{\"type\":\"object\",\"properties\":{" ++
            "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\"," ++
            "\"description\":\"compact: indented key/value text with flat object arrays as " ++
            "tables; fewer bytes, same data.\"}," ++
            "\"pattern\":{\"type\":\"string\"},\"project\":{\"type\":" ++
            "\"string\"},\"file_pattern\":{\"type\":\"string\",\"description\":\"Glob for grep " ++
            "--include (e.g. *.go)\"},\"path_filter\":{\"type\":\"string\",\"description\":\"Regex " ++
            "filter on result file paths (e.g. ^src/ or \\\\.(go|ts)$)\"},\"mode\":{\"type\":\"string\"," ++
            "\"enum\":[\"compact\",\"full\",\"files\"],\"default\":\"compact\",\"description\":\"compact: " ++
            "signatures+metadata (default). full: with source. files: just file list.\"}," ++
            "\"context\":{\"type\":\"integer\",\"description\":\"Lines of context around each match " ++
            "(like grep -C). Only used in compact mode.\"}," ++
            "\"regex\":{\"type\":\"boolean\",\"default\":false},\"limit\":{\"type\":\"integer\"," ++
            "\"description\":\"Max enriched results per call. Default 10. Response includes " ++
            "'total_grep_matches' and 'total_results' so callers can detect truncation. No " ++
            "offset parameter — raise limit or narrow with file_pattern / path_filter to see more." ++
            "\",\"default\":10}},\"required\":[\"pattern\",\"project\"]}",
    },
    .{
        .name = "list_projects",
        .title = "List projects",
        .description = "List indexed projects ordered by name. Pass limit/offset to page; each response carries " ++
            "catalog_token, and passing it back reports catalog_changed when projects were added, " ++
            "deleted, renamed or changed health between pages (restart from offset 0).",
        .input_schema = "{\"type\":\"object\",\"properties\":{" ++
            "\"offset\":{\"type\":\"integer\",\"default\":0,\"minimum\":0}," ++
            "\"limit\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":500,\"description\":" ++
            "\"Page size. Omit or 0 for every project.\"}," ++
            "\"include_details\":{\"type\":\"boolean\",\"default\":false,\"description\":" ++
            "\"Add node and edge counts per project.\"}," ++
            "\"catalog_token\":{\"type\":\"string\",\"description\":" ++
            "\"catalog_token from the previous page.\"}," ++
            "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\"," ++
            "\"description\":\"compact: indented key/value text with flat object arrays as " ++
            "tables; fewer bytes, same data.\"}}}",
    },
    .{
        .name = "delete_project",
        .title = "Delete project",
        .description = "Delete a project from the index",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}},\"required\":[" ++
            "\"project\"]}",
    },
    .{
        .name = "index_status",
        .title = "Index status",
        .description = "Get the indexing status of a project",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}},\"required\":[" ++
            "\"project\"]}",
    },
    .{
        .name = "check_index_coverage",
        .title = "Check index coverage",
        .description = "Check per-file extraction coverage of a project's active index generation for exact " ++
            "repository-relative paths and path scopes. Use it for files you cite or act on, and on " ++
            "scopes before negative or exhaustive claims: files the parser skipped or only partly " ++
            "understood can be missing from graph results. Reports status (no_recorded_issue, partial " ++
            "with parse_error_ranges, unusable when one range covers most of the file, skipped, " ++
            "excluded, not_indexed, missing, coverage_unavailable), " ++
            "filesystem freshness against the indexed metadata, and a recommended_action. Best-effort " ++
            "and about extraction only. With neither paths nor scopes it summarizes the whole project " ++
            "(the same as scopes [\".\"]).",
        .input_schema = "{\"type\":\"object\",\"properties\":{" ++
            "\"project\":{\"type\":\"string\"}," ++
            "\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":128," ++
            "\"description\":\"Repository-relative files to check exactly.\"}," ++
            "\"scopes\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":32," ++
            "\"description\":\"Repository-relative path prefixes; . for the whole project.\"}," ++
            "\"scope_limit\":{\"type\":\"integer\",\"default\":200,\"minimum\":1," ++
            "\"maximum\":1000,\"description\":\"Max gap entries per scope.\"}," ++
            "\"scope_offset\":{\"type\":\"integer\",\"default\":0,\"minimum\":0}," ++
            "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\"," ++
            "\"description\":\"compact: indented key/value text with flat object arrays as " ++
            "tables; fewer bytes, same data.\"}}," ++
            "\"required\":[\"project\"]}",
    },
    .{
        .name = "detect_changes",
        .title = "Detect changes",
        .description = "Detect code changes and their impact",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"},\"scope\":{\"type\":" ++
            "\"string\"},\"depth\":{\"type\":\"integer\",\"default\":2},\"base_branch\":{\"type\":" ++
            "\"string\",\"description\":\"Default: origin/HEAD, else main, else master, else the upstream.\"},\"since\":{\"type\":\"string\",\"description\":" ++
            "\"Git ref or tag to compare from (e.g. HEAD~5, v0.5.0). Diffs <ref>...HEAD.\"}}," ++
            "\"required\":" ++
            "[\"project\"]}",
    },
};

const TOOL_COUNT: c_int = tools.len;
const MCP_TOOLS_PAGE_SIZE = 8;

// MCP_TOOL_OUTPUT_SCHEMA removed (upstream f59d24fd, #1522): no tool
// declares outputSchema; see addToolDef. Kept as comment so a future re-add
// is a conscious, reviewed decision.

fn addToolDef(doc: ?*MutDoc, arr: ?*MutVal, t: *const ToolDef) void {
    const tool = c.yyjson_mut_obj(doc);
    _ = c.yyjson_mut_obj_add_str(doc, tool, "name", t.name);
    _ = c.yyjson_mut_obj_add_str(doc, tool, "title", t.title);
    _ = c.yyjson_mut_obj_add_str(doc, tool, "description", t.description);
    addParsed(doc, tool, "inputSchema", t.input_schema);
    // Deliberately no outputSchema (upstream f59d24fd, #1522). Tool output is
    // format-parameter-polymorphic (tree text by default, JSON object under
    // format:"json"), so no static schema is truthful — and a declared schema
    // makes spec-honoring clients read structuredContent as the authoritative
    // result, rendering every text reply as "{}".
    _ = c.yyjson_mut_arr_add_val(arr, tool);
}

fn toolsListRange(offset_in: c_int, limit_in: c_int, include_next_cursor: bool) HeapStr {
    const doc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(doc);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    const arr = c.yyjson_mut_arr(doc);

    const offset = std.math.clamp(offset_in, 0, TOOL_COUNT);
    const limit = if (limit_in < 0 or limit_in > TOOL_COUNT) TOOL_COUNT else limit_in;
    const end = @min(offset + limit, TOOL_COUNT);
    for (tools[@intCast(offset)..@intCast(end)]) |*t| addToolDef(doc, arr, t);

    _ = c.yyjson_mut_obj_add_val(doc, root, "tools", arr);
    if (include_next_cursor and end < TOOL_COUNT) {
        var cursor: [32]u8 = undefined;
        _ = c.yyjson_mut_obj_add_strcpy(doc, root, "nextCursor", fmtZ(&cursor, "{d}", .{end}));
    }
    return docToStr(doc);
}

pub export fn cbm_mcp_tools_list() [*c]u8 {
    return toolsListRange(0, TOOL_COUNT, false);
}

/// Return the JSON input_schema string for a tool by name, or NULL if
/// unknown. Used by the CLI to build --flag arguments and per-tool --help from
/// the same source of truth the MCP tools/list advertises. Static lifetime;
/// do not free.
pub export fn cbm_mcp_tool_input_schema(tool_name: [*c]const u8) [*c]const u8 {
    if (tool_name == null) return null;
    for (&tools) |*t| {
        if (cEql(t.name, tool_name)) return t.input_schema;
    }
    return null;
}

pub export fn cbm_mcp_tool_count() c_int {
    return TOOL_COUNT;
}

pub export fn cbm_mcp_tool_name(i: c_int) [*c]const u8 {
    return if (i >= 0 and i < TOOL_COUNT) tools[@intCast(i)].name else null;
}

pub export fn cbm_mcp_tool_description(i: c_int) [*c]const u8 {
    return if (i >= 0 and i < TOOL_COUNT) tools[@intCast(i)].description else null;
}

fn toolsCursorOffset(params_json: CStr) c_int {
    const doc = readJson(params_json) orelse return 0;
    defer c.yyjson_doc_free(doc);
    const root = c.yyjson_doc_get_root(doc);
    const cursor = if (root != null) c.yyjson_obj_get(root, "cursor") else null;
    if (cursor == null) return 0;
    if (c.yyjson_is_str(cursor)) {
        const cursor_str = c.yyjson_get_str(cursor);
        if (cursor_str != null and cursor_str[0] != 0) {
            const parsed = std.fmt.parseInt(c_long, std.mem.span(cursor_str), 10) catch return TOOL_COUNT;
            if (parsed >= 0) return if (parsed > TOOL_COUNT) TOOL_COUNT else @intCast(parsed);
        }
    }
    return TOOL_COUNT;
}

pub fn toolsListPage(params_json: CStr) HeapStr {
    return toolsListRange(toolsCursorOffset(params_json), MCP_TOOLS_PAGE_SIZE, true);
}

/// Supported protocol versions, newest first. The server picks the newest
/// version that it shares with the client (per MCP spec version negotiation).
const supported_protocol_versions = [_]Str{
    "2025-11-25",
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
};

pub export fn cbm_mcp_initialize_response(params_json: [*c]const u8) [*c]u8 {
    // Determine protocol version: if client requests a version we support,
    // echo it back; otherwise respond with our latest.
    var version: Str = supported_protocol_versions[0];
    if (readJson(params_json)) |pdoc| {
        defer c.yyjson_doc_free(pdoc);
        const pv = c.yyjson_obj_get(c.yyjson_doc_get_root(pdoc), "protocolVersion");
        if (pv != null and c.yyjson_is_str(pv)) {
            const requested = c.yyjson_get_str(pv);
            for (supported_protocol_versions) |v| {
                if (cEql(requested, v)) {
                    version = v;
                    break;
                }
            }
        }
    }

    const doc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(doc);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);

    _ = c.yyjson_mut_obj_add_str(doc, root, "protocolVersion", version);

    const impl = c.yyjson_mut_obj(doc);
    _ = c.yyjson_mut_obj_add_str(doc, impl, "name", "codebase-memory-mcp");
    _ = c.yyjson_mut_obj_add_str(doc, impl, "version", c.cbm_cli_get_version());
    _ = c.yyjson_mut_obj_add_val(doc, root, "serverInfo", impl);

    const caps = c.yyjson_mut_obj(doc);
    const tools_cap = c.yyjson_mut_obj(doc);
    _ = c.yyjson_mut_obj_add_bool(doc, tools_cap, "listChanged", false);
    _ = c.yyjson_mut_obj_add_val(doc, caps, "tools", tools_cap);
    _ = c.yyjson_mut_obj_add_val(doc, root, "capabilities", caps);
    return docToStr(doc);
}

// ══════════════════════════════════════════════════════════════════
//  ARGUMENT EXTRACTION
// ══════════════════════════════════════════════════════════════════

pub export fn cbm_mcp_get_tool_name(params_json: [*c]const u8) [*c]u8 {
    const doc = readJson(params_json) orelse return null;
    defer c.yyjson_doc_free(doc);
    const name = c.yyjson_obj_get(c.yyjson_doc_get_root(doc), "name");
    return if (name != null and c.yyjson_is_str(name)) dup(c.yyjson_get_str(name)) else null;
}

pub export fn cbm_mcp_get_arguments(params_json: [*c]const u8) [*c]u8 {
    const doc = readJson(params_json) orelse return null;
    defer c.yyjson_doc_free(doc);
    const args = c.yyjson_obj_get(c.yyjson_doc_get_root(doc), "arguments");
    const result = if (args != null) c.yyjson_val_write(args, 0, null) else null;
    return if (result != null) result else dup("{}");
}

pub export fn cbm_mcp_get_string_arg(args_json: [*c]const u8, key: [*c]const u8) [*c]u8 {
    const doc = readJson(args_json) orelse return null;
    defer c.yyjson_doc_free(doc);
    const val = c.yyjson_obj_get(c.yyjson_doc_get_root(doc), key);
    return if (val != null and c.yyjson_is_str(val)) dup(c.yyjson_get_str(val)) else null;
}

pub export fn cbm_mcp_get_int_arg(args_json: [*c]const u8, key: [*c]const u8, default_val: c_int) c_int {
    const doc = readJson(args_json) orelse return default_val;
    defer c.yyjson_doc_free(doc);
    const val = c.yyjson_obj_get(c.yyjson_doc_get_root(doc), key);
    // The C took yyjson_get_int's int and truncated nothing; keep its value.
    return if (val != null and c.yyjson_is_int(val)) c.yyjson_get_int(val) else default_val;
}

pub export fn cbm_mcp_get_bool_arg(args_json: [*c]const u8, key: [*c]const u8) bool {
    const doc = readJson(args_json) orelse return false;
    defer c.yyjson_doc_free(doc);
    const val = c.yyjson_obj_get(c.yyjson_doc_get_root(doc), key);
    return val != null and c.yyjson_is_bool(val) and c.yyjson_get_bool(val);
}

/// Shorthands for the handlers.
pub const strArg = cbm_mcp_get_string_arg;
pub const intArg = cbm_mcp_get_int_arg;
pub const boolArg = cbm_mcp_get_bool_arg;

pub fn canonicalizeRepoPathIfExists(repo_path: HeapStr) HeapStr {
    if (repo_path == null) return null;
    var root_syntax = true;
    for (std.mem.span(repo_path)) |ch| {
        if (ch != '/' and ch != '\\' and ch != ':') {
            root_syntax = false;
            break;
        }
    }
    if (root_syntax) return repo_path;

    var real: [4096]u8 = undefined;
    if (realPath(repo_path, &real)) {
        _ = c.cbm_normalize_path_sep(&real);
        const canonical = dup(&real);
        if (canonical != null) {
            c.free(repo_path);
            return canonical;
        }
    }
    return repo_path;
}

extern fn _fullpath(abs: [*]u8, rel: [*:0]const u8, max: usize) ?[*:0]u8;
extern fn _access(path: [*:0]const u8, mode: c_int) c_int;

/// realpath(3), or _access + _fullpath on Windows. True when out holds the
/// resolved path of an existing file.
pub fn realPath(path: CStr, out: *[4096]u8) bool {
    if (is_windows) {
        return _access(path, 0) == 0 and _fullpath(out, path, out.len) != null;
    }
    return c.realpath(path, out) != null;
}

fn normalizeProjectArg(project_in: HeapStr) HeapStr {
    var project = project_in;
    if (project == null or (c.strchr(project, '/') == null and c.strchr(project, '\\') == null)) return project;

    // Parent-qualified public selectors intentionally contain '/'. Only an
    // existing absolute filesystem path is a legacy path-form project arg.
    const absolute_path = project[0] == '/' or
        (c.strlen(project) >= 3 and project[1] == ':' and (project[2] == '/' or project[2] == '\\'));
    if (!absolute_path or !c.cbm_file_exists(project)) return project;

    project = canonicalizeRepoPathIfExists(project);
    const normalized = c.cbm_project_name_from_path(project);
    if (normalized != null) {
        c.free(project);
        return normalized;
    }
    return project;
}

/// Resolve the project argument, accepting the canonical "project" key plus
/// the aliases a caller naturally reaches for (#640): list_projects surfaces
/// the field as "name" and the not-found hint says "pass the project name", so
/// "project_name" is the usual guess; "project_id" / "projectName" are
/// accepted too. NOT bare "name" — index_repository uses "name" for an
/// explicit project-name override. Caller must free() the result.
pub fn getProjectArg(args_json: CStr) HeapStr {
    var p = strArg(args_json, "project");
    if (p == null) p = strArg(args_json, "project_name");
    if (p == null) p = strArg(args_json, "project_id");
    if (p == null) p = strArg(args_json, "projectName");
    return normalizeProjectArg(p);
}

pub const ProjectScope = struct {
    project: HeapStr = null,
    projects: [*c]HeapStr = null,
    project_count: usize = 0,
    wildcard: bool = false,

    pub fn deinit(scope: *ProjectScope) void {
        c.free(scope.project);
        for (0..scope.project_count) |i| c.free(scope.projects[i]);
        c.free(@ptrCast(scope.projects));
        scope.* = .{};
    }

    pub fn names(scope: *const ProjectScope) []const HeapStr {
        return if (scope.project_count == 0) &.{} else scope.projects[0..scope.project_count];
    }
};

/// Parse project/projects. Returns 0, or -1 with error set to a static
/// message.
pub fn projectScopeParse(args_json: CStr, scope: *ProjectScope, err: *?Str) c_int {
    scope.* = .{};
    err.* = null;

    const doc = readJson(args_json);
    defer c.yyjson_doc_free(doc);
    const root = if (doc) |d| c.yyjson_doc_get_root(d) else null;
    const projects = if (root != null) c.yyjson_obj_get(root, "projects") else null;
    scope.project = getProjectArg(args_json);
    if (scope.project != null and projects != null) {
        err.* = "project and projects are mutually exclusive";
        return -1;
    }
    if (projects == null) {
        if (scope.project == null or scope.project[0] == 0) {
            err.* = "exactly one of project or projects is required";
            return -1;
        }
        return 0;
    }
    if (!c.yyjson_is_arr(projects) or c.yyjson_arr_size(projects) == 0) {
        err.* = "projects must be a non-empty array";
        return -1;
    }

    scope.projects = interop.callocN(HeapStr, c.yyjson_arr_size(projects));
    if (scope.projects == null) {
        err.* = "out of memory while parsing projects";
        return -1;
    }
    var it: c.yyjson_arr_iter = undefined;
    _ = c.yyjson_arr_iter_init(projects, &it);
    outer: while (c.yyjson_arr_iter_next(&it)) |value| {
        if (!c.yyjson_is_str(value) or c.yyjson_get_str(value)[0] == 0) {
            err.* = "projects entries must be non-empty strings";
            return -1;
        }
        const name = c.yyjson_get_str(value);
        for (scope.names()) |existing| {
            if (cEql(existing, name)) continue :outer;
        }
        scope.projects[scope.project_count] = c.strdup(name);
        if (scope.projects[scope.project_count] == null) {
            err.* = "out of memory while parsing projects";
            return -1;
        }
        scope.project_count += 1;
    }
    scope.wildcard = scope.project_count == 1 and cEql(scope.projects[0], "*");
    if (!scope.wildcard) {
        for (scope.names()) |name| {
            if (cEql(name, "*")) {
                err.* = "projects wildcard must be the sole array entry";
                return -1;
            }
        }
    }
    return 0;
}

// ══════════════════════════════════════════════════════════════════
//  MCP SERVER
// ══════════════════════════════════════════════════════════════════

pub const Server = struct {
    store: ?*c.cbm_store_t, // currently open project store (or NULL)
    owns_store: bool, // true if we opened the store
    current_project: HeapStr, // which project store is open for (heap)
    store_last_used: c.time_t, // last time resolveStore was called for a named project
    update_notice: [256]u8, // one-shot update notice, cleared after first injection
    update_checked: bool, // true after background check has been launched
    update_tid: c.cbm_thread_t, // background update check thread
    update_thread_active: bool, // true if update thread was started and needs joining

    // Session + auto-index state
    session_root: [1024]u8, // detected project root path
    session_project: [256]u8, // derived project name
    session_detected: bool, // true after first detection attempt
    watcher: ?*c.struct_cbm_watcher, // external watcher ref (not owned)
    config: ?*c.struct_cbm_config, // external config ref (not owned)
    autoindex_tid: c.cbm_thread_t,
    autoindex_active: bool, // true if auto-index thread was started

    // Active pipeline tracking for cancellation support
    active_pipeline: ?*c.cbm_pipeline_t, // non-NULL while index_repository runs
    active_request_id: i64, // JSON-RPC id of the in-progress tool call
    active_request_id_str: HeapStr, // string JSON-RPC id of the in-progress tool call
    index_queue_mutex: c.cbm_mutex_t,
    index_queue_next_ticket: u64,
    index_queue_serving_ticket: u64,
    index_queue_waiters: std.atomic.Value(u64),
};

pub fn srvOf(srv: ?*c.cbm_mcp_server_t) ?*Server {
    return @ptrCast(@alignCast(srv));
}

fn srvOfConst(srv: ?*const c.cbm_mcp_server_t) ?*const Server {
    return @ptrCast(@alignCast(srv));
}

pub export fn cbm_mcp_server_new() ?*c.cbm_mcp_server_t {
    const srv: *Server = @ptrCast(@alignCast(c.calloc(1, @sizeOf(Server)) orelse return null));
    srv.store = c.cbm_store_open_memory();
    srv.owns_store = true;
    srv.index_queue_waiters = .init(0);
    c.cbm_mutex_init(&srv.index_queue_mutex);
    return @ptrCast(srv);
}

pub export fn cbm_mcp_server_store(srv_c: ?*c.cbm_mcp_server_t) ?*c.cbm_store_t {
    const srv = srvOf(srv_c) orelse return null;
    return srv.store;
}

pub export fn cbm_mcp_server_set_project(srv_c: ?*c.cbm_mcp_server_t, project: [*c]const u8) void {
    const srv = srvOf(srv_c) orelse return;
    c.free(srv.current_project);
    srv.current_project = if (project != null) dup(project) else null;
}

pub export fn cbm_mcp_server_set_watcher(srv_c: ?*c.cbm_mcp_server_t, w: ?*c.struct_cbm_watcher) void {
    if (srvOf(srv_c)) |srv| srv.watcher = w;
}

pub export fn cbm_mcp_server_set_config(srv_c: ?*c.cbm_mcp_server_t, cfg: ?*c.struct_cbm_config) void {
    if (srvOf(srv_c)) |srv| srv.config = cfg;
}

pub export fn cbm_mcp_server_free(srv_c: ?*c.cbm_mcp_server_t) void {
    const srv = srvOf(srv_c) orelse return;
    if (srv.update_thread_active) _ = c.cbm_thread_join(&srv.update_tid);
    if (srv.autoindex_active) _ = c.cbm_thread_join(&srv.autoindex_tid);
    if (srv.owns_store and srv.store != null) c.cbm_store_close(srv.store);
    c.free(srv.current_project);
    c.free(srv.active_request_id_str);
    c.cbm_mutex_destroy(&srv.index_queue_mutex);
    c.free(srv);
}

// ── Idle store eviction ──────────────────────────────────────────

pub export fn cbm_mcp_server_evict_idle(srv_c: ?*c.cbm_mcp_server_t, timeout_s: c_int) void {
    const srv = srvOf(srv_c) orelse return;
    if (srv.store == null) return;
    // Protect initial in-memory stores that were never accessed via a named
    // project. store_last_used stays 0 until resolveStore is called with a
    // non-NULL project.
    if (srv.store_last_used == 0) return;

    const now = c.time(null);
    if (now - srv.store_last_used < timeout_s) return;

    if (srv.owns_store) c.cbm_store_close(srv.store);
    srv.store = null;
    c.free(srv.current_project);
    srv.current_project = null;
    srv.store_last_used = 0;
}

pub export fn cbm_mcp_server_has_cached_store(srv_c: ?*c.cbm_mcp_server_t) bool {
    const srv = srvOf(srv_c) orelse return false;
    return srv.store != null;
}

pub export fn cbm_mcp_server_active_pipeline(srv_c: ?*c.cbm_mcp_server_t) ?*c.cbm_pipeline_t {
    const srv = srvOf(srv_c) orelse return null;
    return srv.active_pipeline;
}

// ── Index job queue ──────────────────────────────────────────────

pub export fn cbm_index_job_queue_waiter_count(srv_c: ?*const c.cbm_mcp_server_t) u64 {
    const srv = srvOfConst(srv_c) orelse return 0;
    return srv.index_queue_waiters.load(.seq_cst);
}

// ── Store resolution ──────────────────────────────────────────────

/// The workspace id of project in the shared cbm.zova at path, validated.
/// False when the project or database is missing.
fn projectWorkspaceId(path: *[1024]u8, project: CStr, workspace_id: *[c.CBM_ZOVA_WORKSPACE_ID_MAX]u8) bool {
    if (c.cbm_zova_user_database_path(path, path.len) != 0) return false;
    const repo = c.cbm_zova_repository_open(path, project) orelse return false;
    _ = bprint(workspace_id, "{s}", .{sp(c.cbm_zova_repository_workspace_id(repo))});
    c.cbm_zova_repository_close(repo);
    return c.cbm_zova_workspace_id_validate(workspace_id) == 0;
}

/// Open the project's workspace store in the shared cbm.zova for query tools.
/// Tracks last-access time so the event loop can evict idle stores.
pub fn resolveStore(srv: *Server, project: CStr) ?*c.cbm_store_t {
    if (project == null) return null; // project is required — no implicit fallback

    srv.store_last_used = c.time(null);

    var path: [1024]u8 = undefined;
    var workspace_id: [c.CBM_ZOVA_WORKSPACE_ID_MAX]u8 = undefined;
    if (!projectWorkspaceId(&path, project, &workspace_id)) return null;
    if (srv.owns_store and srv.store != null) c.cbm_store_close(srv.store);
    srv.store = c.cbm_store_open_zova_workspace_query(&path, &workspace_id);
    srv.owns_store = srv.store != null;
    c.free(srv.current_project);
    srv.current_project = if (srv.store != null) dup(project) else null;
    return srv.store;
}

/// compare_graphs bypasses resolveStore(): it needs two independently-owned
/// request-scoped stores, while resolveStore caches one on the server.
pub fn openProjectStore(project: CStr) ?*c.cbm_store_t {
    var path: [1024]u8 = undefined;
    var workspace_id: [c.CBM_ZOVA_WORKSPACE_ID_MAX]u8 = undefined;
    if (!projectWorkspaceId(&path, project, &workspace_id)) return null;
    return c.cbm_store_open_zova_workspace_query(&path, &workspace_id);
}

/// Write the shared cbm.zova catalog's project selectors into out as
/// comma-separated quoted names. Returns the number of projects written.
fn collectZovaProjectNames(out: []u8) c_int {
    var path: [1024]u8 = undefined;
    if (c.cbm_zova_user_database_path(&path, path.len) != 0 or !c.cbm_file_exists(&path)) return 0;
    const catalog = c.cbm_zova_catalog_open(&path);
    var listed = std.mem.zeroes(c.cbm_zova_catalog_scope_t);
    if (catalog == null or c.cbm_zova_catalog_list(catalog, &listed) != c.CBM_STORE_OK) {
        c.cbm_zova_catalog_close(catalog);
        return 0;
    }
    defer {
        c.cbm_zova_catalog_scope_free(&listed);
        c.cbm_zova_catalog_close(catalog);
    }
    var count: c_int = 0;
    var off: usize = 0;
    for (0..@intCast(@max(listed.count, 0))) |i| {
        const name = sp(listed.projects[i].selector);
        // Element-boundary write: only emit this name if the WHOLE element —
        // optional leading comma + "name" — plus the NUL fits in what remains.
        // Never truncate mid-token; a partial name would corrupt the JSON
        // array (issue #235). Stop cleanly at the last name that fits: the
        // array then always holds complete names and `count` == its length.
        const need = name.len + 2 + @as(usize, if (count > 0) 1 else 0);
        if (off + need + 1 > out.len) break;
        if (count > 0) {
            out[off] = ',';
            off += 1;
        }
        off += bprint(out[off..], "\"{s}\"", .{name});
        count += 1;
    }
    return count;
}

pub fn addGitContextJson(doc: ?*MutDoc, obj: ?*MutVal, root_path: CStr) void {
    var ctx = std.mem.zeroes(c.cbm_git_context_t);
    _ = c.cbm_git_context_resolve(root_path, &ctx);
    defer c.cbm_git_context_free(&ctx);

    const git = c.yyjson_mut_obj(doc);
    _ = c.yyjson_mut_obj_add_bool(doc, git, "is_git", ctx.is_git);
    _ = c.yyjson_mut_obj_add_bool(doc, git, "is_worktree", ctx.is_worktree);
    _ = c.yyjson_mut_obj_add_bool(doc, git, "is_detached", ctx.is_detached);
    _ = c.yyjson_mut_obj_add_bool(doc, git, "root_exists", ctx.root_exists);
    const strs = .{
        .{ "worktree_root", ctx.worktree_root },
        .{ "git_dir", ctx.git_dir },
        .{ "git_common_dir", ctx.git_common_dir },
        .{ "canonical_root", ctx.canonical_root },
        .{ "branch", ctx.branch },
        .{ "branch_slug", ctx.branch_slug },
        .{ "head_sha", ctx.head_sha },
        .{ "base_sha", ctx.base_sha },
    };
    inline for (strs) |kv| {
        if (kv[1] != null) {
            _ = c.yyjson_mut_obj_add_strcpy(doc, git, kv[0], kv[1]);
        } else {
            _ = c.yyjson_mut_obj_add_null(doc, git, kv[0]);
        }
    }
    _ = c.yyjson_mut_obj_add_val(doc, obj, "git", git);
}

/// Build a helpful error listing available projects. Caller must free().
pub fn buildProjectListError(reason: []const u8) HeapStr {
    var projects = std.mem.zeroes([4096]u8);
    const count = collectZovaProjectNames(&projects);
    var buf: [5120]u8 = undefined;
    if (count > 0) {
        return dup(fmtZ(&buf, "{{\"error\":\"{s}\",\"hint\":\"Use list_projects to see all indexed projects, " ++
            "then pass it as the \\\"project\\\" " ++
            "argument.\",\"available_projects\":[{s}],\"count\":{d}}}", .{ reason, bufStr(&projects), count }));
    }
    return dup(fmtZ(&buf, "{{\"error\":\"{s}\",\"hint\":\"No projects indexed yet. " ++
        "Call index_repository first.\"}}", .{reason}));
}

/// Distinct from "unknown project": the caller omitted the project argument
/// entirely (no recognized key). Name the literal "project" key so the fix is
/// obvious (#640). Caller must free() result.
fn buildMissingProjectError() HeapStr {
    return dup("{\"error\":\"missing required argument: project\",\"hint\":\"Pass " ++
        "the project as the \\\"project\\\" argument, e.g. " ++
        "{\\\"project\\\":\\\"<name from list_projects>\\\"}. Run " ++
        "list_projects to see indexed projects.\"}");
}

/// The error result for a missing or unknown project: a NULL project means
/// the argument was missing (clearer message); a non-NULL project that didn't
/// resolve is unknown/unindexed (list the available ones). Frees project.
pub fn projectNotFoundResult(project: HeapStr) HeapStr {
    const err = if (project != null and project[0] != 0)
        buildProjectListError("project not found or not indexed")
    else
        buildMissingProjectError();
    c.free(project);
    return textResultOwned(err, true);
}

// ── Tool handler implementations ─────────────────────────────────

const LIST_PROJECTS_MAX_LIMIT = 500;

/// Fingerprint of what decides list_projects paging: membership, order
/// (selectors are the sort key and change on collision renames) and health.
/// A new generation of an existing workspace does not move any row.
fn catalogPageToken(listed: *const c.cbm_zova_catalog_scope_t, out: []u8) [*:0]u8 {
    var h: u64 = 1469598103934665603; // FNV-1a 64
    for (0..@intCast(@max(listed.count, 0))) |i| {
        const project = &listed.projects[i];
        const parts = [_]CStr{ &project.workspace_id, project.selector, if (project.ready) "r" else "-", if (project.healthy) "h" else "-" };
        for (parts) |part| {
            if (part != null) {
                for (std.mem.span(part)) |ch| h = (h ^ ch) *% 1099511628211;
            }
            h = (h ^ 0x1f) *% 1099511628211; // field separator
        }
    }
    return fmtZ(out, "{x:0>16}", .{h});
}

fn addProjectDetails(doc: ?*MutDoc, entry: ?*MutVal, db_path: CStr, selector: CStr) void {
    const repo = c.cbm_zova_repository_open(db_path, selector);
    var nodes: c_int = 0;
    var edges: c_int = 0;
    const ok = repo != null and c.cbm_zova_repository_counts(repo, c.cbm_zova_repository_workspace_id(repo), &nodes, &edges) == c.CBM_STORE_OK;
    c.cbm_zova_repository_close(repo);
    if (ok) {
        _ = c.yyjson_mut_obj_add_int(doc, entry, "nodes", nodes);
        _ = c.yyjson_mut_obj_add_int(doc, entry, "edges", edges);
    } else {
        _ = c.yyjson_mut_obj_add_str(doc, entry, "details_error", "counts unavailable");
    }
}

/// list_projects: the workspace catalog in the shared cbm.zova, ordered by
/// selector. offset/limit page through it (no limit = every project, the
/// original output); catalog_token from a previous page detects a catalog
/// that changed between pages.
pub fn handleListProjects(_: *Server, args: CStr) HeapStr {
    const offset = intArg(args, "offset", 0);
    var limit = intArg(args, "limit", 0);
    const include_details = boolArg(args, "include_details");
    const page_token = strArg(args, "catalog_token");
    defer c.free(page_token);
    if (offset < 0 or limit < 0) return textResult("offset and limit must be >= 0", true);
    if (limit > LIST_PROJECTS_MAX_LIMIT) limit = LIST_PROJECTS_MAX_LIMIT;

    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    const projects = c.yyjson_mut_arr(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_val(doc, root, "projects", projects);

    var path: [1024]u8 = undefined;
    var catalog: ?*c.cbm_zova_catalog_t = null;
    var listed = std.mem.zeroes(c.cbm_zova_catalog_scope_t);
    const have_db = c.cbm_zova_user_database_path(&path, path.len) == 0 and c.cbm_file_exists(&path);
    if (have_db) {
        catalog = c.cbm_zova_catalog_open(&path);
        if (catalog == null or c.cbm_zova_catalog_list(catalog, &listed) != c.CBM_STORE_OK) {
            c.cbm_zova_catalog_close(catalog);
            c.yyjson_mut_doc_free(doc);
            return textResult("{\"error\":\"cannot read Zova project catalog\"}", true);
        }
    }
    defer c.cbm_zova_catalog_scope_free(&listed);
    // The catalog snapshot is taken; release the read before per-project
    // detail reads open their own.
    c.cbm_zova_catalog_close(catalog);

    const start = @min(offset, listed.count);
    const end = if (limit > 0 and limit < listed.count - start) start + limit else listed.count;
    var i = start;
    while (i < end) : (i += 1) {
        const project = &listed.projects[@intCast(i)];
        const entry = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_strcpy(doc, entry, "name", project.selector);
        // The name index_repository reports and qualified names start with.
        _ = c.yyjson_mut_obj_add_strcpy(doc, entry, "project", project.project);
        _ = c.yyjson_mut_obj_add_strcpy(doc, entry, "root_path", project.root_path);
        _ = c.yyjson_mut_obj_add_sint(doc, entry, "generation", project.generation);
        _ = c.yyjson_mut_obj_add_bool(doc, entry, "ready", project.ready);
        _ = c.yyjson_mut_obj_add_bool(doc, entry, "healthy", project.healthy);
        _ = c.yyjson_mut_obj_add_strcpy(doc, entry, "health_reason", project.health_reason);
        _ = c.yyjson_mut_obj_add_strcpy(doc, entry, "indexed_at", project.indexed_at);
        _ = c.yyjson_mut_obj_add_int(doc, entry, "vector_dimensions", project.vector_dimensions);
        if (include_details) addProjectDetails(doc, entry, &path, project.selector);
        _ = c.yyjson_mut_arr_add_val(projects, entry);
    }

    var token_buf: [32]u8 = undefined;
    const token = catalogPageToken(&listed, &token_buf);
    _ = c.yyjson_mut_obj_add_int(doc, root, "total", listed.count);
    _ = c.yyjson_mut_obj_add_int(doc, root, "offset", start);
    _ = c.yyjson_mut_obj_add_int(doc, root, "returned", end - start);
    _ = c.yyjson_mut_obj_add_bool(doc, root, "has_more", end < listed.count);
    if (end < listed.count) _ = c.yyjson_mut_obj_add_int(doc, root, "next_offset", end);
    _ = c.yyjson_mut_obj_add_strcpy(doc, root, "catalog_token", token);
    if (page_token != null and !cEql(page_token, token)) {
        // Workspaces were added, deleted, renamed or changed health since the
        // page that issued page_token: offsets no longer line up.
        _ = c.yyjson_mut_obj_add_bool(doc, root, "catalog_changed", true);
        _ = c.yyjson_mut_obj_add_str(doc, root, "hint", "The project catalog changed since the previous page. Restart " ++
            "from offset 0 with the new catalog_token.");
    } else if (listed.count == 0) {
        _ = c.yyjson_mut_obj_add_str(doc, root, "hint", "No projects indexed. Call index_repository(repo_path=...) first.");
    }
    return docResult(doc);
}

/// Returns a heap-allocated error result when the named project has not been
/// indexed yet, or NULL when the project exists. resolveStore returns NULL
/// for a project missing from cbm.zova (projectNotFoundResult fires first).
/// This catches the remaining case: the store opened but has no project row.
pub fn verifyProjectIndexed(store: ?*c.cbm_store_t, project: CStr) HeapStr {
    var proj_check = std.mem.zeroes(c.cbm_project_t);
    if (c.cbm_store_get_project(store, project, &proj_check) != c.CBM_STORE_OK) {
        return textResultOwned(buildProjectListError("project not indexed — run index_repository first"), true);
    }
    c.cbm_project_free_fields(&proj_check);
    return null;
}

// ── Shared handler helpers ───────────────────────────────────────

/// _fullpath(3) on Windows (separators normalized to '/'), realpath(3)
/// elsewhere. False when the path cannot be resolved.
pub fn resolvePath(path: CStr, out: *[4096]u8) bool {
    if (is_windows) {
        if (_fullpath(out, path, out.len) == null) return false;
        _ = c.cbm_normalize_path_sep(out);
        return true;
    }
    return c.realpath(path, out) != null;
}

/// True only when abs_path, after realpath/_fullpath resolution (which
/// collapses `..` and resolves symlinks/junctions), stays within root_path.
/// This is the single containment guard every MCP file-read sink must pass
/// before reading a file into a tool response: both the snippet path and the
/// search path route through it, so a result whose indexed path escapes the
/// project root — via a `..` segment, or a symlink / Windows junction picked
/// up during discovery — is never read back out. Not in mcp.h: the tests
/// declare it themselves.
export fn cbm_path_within_root(root_path: [*c]const u8, abs_path: [*c]const u8) bool {
    return pathWithinRoot(root_path, abs_path);
}

pub fn pathWithinRoot(root_path: CStr, abs_path: CStr) bool {
    if (root_path == null or abs_path == null) return false;
    var real_root: [4096]u8 = undefined;
    var real_file: [4096]u8 = undefined;
    if (!resolvePath(root_path, &real_root) or !resolvePath(abs_path, &real_file)) return false;
    const root = bufStr(&real_root);
    const file = bufStr(&real_file);
    return std.mem.startsWith(u8, file, root) and (file.len == root.len or file[root.len] == '/');
}

pub export fn cbm_mcp_check_index_root(repo_path: [*c]const u8, canonical_out: [*c]u8, canonical_sz: usize, out_reason: [*c][*c]const u8) c_int {
    if (out_reason != null) out_reason.* = null;
    var real: [4096]u8 = undefined;
    var resolved: CStr = repo_path;
    if (repo_path != null and repo_path[0] != 0 and resolvePath(repo_path, &real)) resolved = &real;
    if (canonical_out != null and canonical_sz > 0) {
        _ = bprint(canonical_out[0..canonical_sz], "{s}", .{sp(resolved)});
    }
    // Missing path is handled by the caller (a required-field error), not a
    // boundary decision.
    if (repo_path == null or repo_path[0] == 0) return 0;
    const allowed_root = c.getenv("CBM_ALLOWED_ROOT");
    if (allowed_root != null and allowed_root[0] != 0 and !pathWithinRoot(allowed_root, resolved)) {
        // Keep the exact wording the existing MCP fixture matches, with
        // guidance appended (upstream 7fa5b077): a boundary is about scope
        // inside an allowed root, not sensitivity.
        if (out_reason != null) out_reason.* = "repo_path is outside the allowed root; set CBM_ALLOWED_ROOT so the index " ++
            "root is contained within it";
        return -1;
    }
    return 0;
}

fn utf8IsCont(ch: u8) bool {
    return (ch & 0xC0) == 0x80;
}

/// A heap copy of s with every invalid UTF-8 byte replaced by U+FFFD. NULL
/// for NULL input or on allocation failure.
pub fn sanitizeUtf8Lossy(s: CStr) HeapStr {
    if (s == null) return null;
    const in = std.mem.span(s);
    if (in.len > (std.math.maxInt(usize) - 1) / 3) return null;
    const out = interop.mallocN(u8, in.len * 3 + 1);
    if (out == null) return null;
    var i: usize = 0;
    var o: usize = 0;
    while (i < in.len) {
        const ch = in[i];
        const left = in.len - i;
        const n: usize = if (ch < 0x80)
            1
        else if (ch >= 0xC2 and ch <= 0xDF and left > 1 and utf8IsCont(in[i + 1]))
            2
        else if (ch == 0xE0 and left > 2 and in[i + 1] >= 0xA0 and in[i + 1] <= 0xBF and utf8IsCont(in[i + 2]))
            3
        else if (ch >= 0xE1 and ch <= 0xEC and left > 2 and utf8IsCont(in[i + 1]) and utf8IsCont(in[i + 2]))
            3
        else if (ch == 0xED and left > 2 and in[i + 1] >= 0x80 and in[i + 1] <= 0x9F and utf8IsCont(in[i + 2]))
            3
        else if (ch >= 0xEE and ch <= 0xEF and left > 2 and utf8IsCont(in[i + 1]) and utf8IsCont(in[i + 2]))
            3
        else if (ch == 0xF0 and left > 3 and in[i + 1] >= 0x90 and in[i + 1] <= 0xBF and utf8IsCont(in[i + 2]) and utf8IsCont(in[i + 3]))
            4
        else if (ch >= 0xF1 and ch <= 0xF3 and left > 3 and utf8IsCont(in[i + 1]) and utf8IsCont(in[i + 2]) and utf8IsCont(in[i + 3]))
            4
        else if (ch == 0xF4 and left > 3 and in[i + 1] >= 0x80 and in[i + 1] <= 0x8F and utf8IsCont(in[i + 2]) and utf8IsCont(in[i + 3]))
            4
        else
            0;
        if (n > 0) {
            @memcpy(out[o .. o + n], in[i .. i + n]);
            o += n;
            i += n;
        } else {
            out[o] = 0xEF;
            out[o + 1] = 0xBF;
            out[o + 2] = 0xBD;
            o += 3;
            i += 1;
        }
    }
    out[o] = 0;
    return out;
}

/// Enrich a mutable JSON object with key-value pairs from a node's
/// properties_json. Returns the parsed doc: the caller frees it AFTER
/// serialization, because the added strings point into it (zero-copy).
pub fn enrichNodeProperties(doc: ?*MutDoc, obj: ?*MutVal, properties_json: CStr) ?*Doc {
    if (properties_json == null or properties_json[0] == 0) return null;
    const props_doc = readJson(properties_json) orelse return null;
    const props_root = c.yyjson_doc_get_root(props_doc);
    if (props_root == null or !c.yyjson_is_obj(props_root)) {
        c.yyjson_doc_free(props_doc);
        return null;
    }
    var iter: c.yyjson_obj_iter = undefined;
    _ = c.yyjson_obj_iter_init(props_root, &iter);
    while (c.yyjson_obj_iter_next(&iter)) |key| {
        const val = c.yyjson_obj_iter_get_val(key);
        const k = c.yyjson_get_str(key);
        if (k == null) continue;
        if (c.yyjson_is_str(val)) {
            _ = c.yyjson_mut_obj_add_str(doc, obj, k, c.yyjson_get_str(val));
        } else if (c.yyjson_is_bool(val)) {
            _ = c.yyjson_mut_obj_add_bool(doc, obj, k, c.yyjson_get_bool(val));
        } else if (c.yyjson_is_int(val)) {
            _ = c.yyjson_mut_obj_add_int(doc, obj, k, c.yyjson_get_int(val));
        } else if (c.yyjson_is_real(val)) {
            _ = c.yyjson_mut_obj_add_real(doc, obj, k, c.yyjson_get_real(val));
        } else if (c.yyjson_is_arr(val) or c.yyjson_is_obj(val)) {
            const copy = c.yyjson_val_mut_copy(doc, val);
            if (copy != null) _ = c.yyjson_mut_obj_add_val(doc, obj, k, copy);
        }
    }
    return props_doc;
}

/// Add a string array to a JSON object (no-op if count == 0).
pub fn addStringArray(doc: ?*MutDoc, obj: ?*MutVal, key: Str, strings: [*c][*c]u8, count: c_int) void {
    if (count <= 0) return;
    const arr = c.yyjson_mut_arr(doc);
    for (0..@intCast(count)) |i| _ = c.yyjson_mut_arr_add_str(doc, arr, strings[i]);
    _ = c.yyjson_mut_obj_add_val(doc, obj, key, arr);
}

/// "" for NULL, for the yyjson add calls that must not see NULL.
pub fn orEmpty(s: CStr) CStr {
    return if (s != null) s else "";
}

/// Open the shared cbm.zova repository for project. On success path holds
/// the database path.
pub fn openRepository(path: *[1024]u8, project: CStr) ?*c.cbm_zova_repository_t {
    if (project == null or c.cbm_zova_user_database_path(path, path.len) != 0) return null;
    return c.cbm_zova_repository_open(path, project);
}
