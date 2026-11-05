/*
 * mcp.c — MCP server: JSON-RPC 2.0 over stdio with 14 graph tools.
 *
 * Uses yyjson for fast JSON parsing/building.
 * Single-threaded event loop: read line → parse → dispatch → respond.
 */

// operations

#include "foundation/constants.h"

enum {
    MCP_FIELD_SIZE = 1040,
    MCP_TIMEOUT_MS = 1000,
    MCP_HALF_SEC_US = 500000,
    MCP_MAX_ROWS = 100,
    MCP_COL_2 = 2,
    MCP_COL_3 = 3,
    MCP_COL_4 = 4,
    MCP_COL_7 = 7,
    MCP_COL_10 = 10,
    MCP_COL_16 = 16,
    MCP_SEPARATOR = 2,   /* space for separator chars */
    MCP_DEFAULT_DEPTH = 3,
    MCP_DEFAULT_BFS_DEPTH = 2,
    MCP_DEFAULT_LIMIT = 10,
    MCP_BFS_LIMIT = 100,
    MCP_N_DEFAULTS_2 = 2,
    MCP_URI_PREFIX = 7,      /* strlen("file://") */
    MCP_CONTENT_PREFIX = 15, /* strlen("Content-Length:") */
    MCP_RETURN_2 = 2,
    MCP_TOOLS_PAGE_SIZE = 8,
};
#define MCP_MS_TO_US 1000LL
#define MCP_S_TO_US 1000000LL
/* search_code scan bounds (upstream 31bdd4d6): a pathological pattern or a huge
 * indexed tree must not pin the request forever or fill the scratch disk. */
#define MCP_SEARCH_SCAN_TIMEOUT_MS 30000
#define MCP_SEARCH_OUTPUT_MAX (64L * 1024L * 1024L)

#define SLEN(s) (sizeof(s) - 1)
#include "mcp/mcp.h"
#include "store/store.h"
#include <sqlite3.h>
#include "cypher/cypher.h"
#include "pipeline/pipeline.h"
#include "git/git_context.h"
#include "cli/cli.h"
#include "watcher/watcher.h"
#include "foundation/mem.h"
#include "foundation/diagnostics.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/log.h"
#include "foundation/limits.h"
#include "mcp/index_supervisor.h"
#include "foundation/subprocess.h"
#include "foundation/str_util.h"
#include "foundation/dump_verify.h"
#include "foundation/compat_regex.h"
#include "zova/cbm_zova_coordination.h"
#include "zova/cbm_zova_operations.h"
#include "zova/cbm_zova_repository.h"
#include "zova/cbm_zova_route.h"
#include "discover/discover.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#define getpid _getpid
#else
#include <fnmatch.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#endif
#include <yyjson/yyjson.h>
#include <stdint.h> // int64_t
#include <limits.h> // INT_MIN, INT_MAX
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>

/* ── Constants ────────────────────────────────────────────────── */

/* Default snippet fallback line count */
#define SNIPPET_DEFAULT_LINES 50

/* Idle store eviction: close cached project store after this many seconds
 * of inactivity to free SQLite memory during idle periods. */
#define STORE_IDLE_TIMEOUT_S 60

/* Directory permissions: rwxr-xr-x */

/* JSON-RPC 2.0 standard error codes */
#define JSONRPC_PARSE_ERROR (-32700)
#define JSONRPC_METHOD_NOT_FOUND (-32601)

/* ── Helpers ────────────────────────────────────────────────────── */

/* Write yyjson_mut_doc to heap-allocated JSON string.
 * ALLOW_INVALID_UNICODE: some database strings may contain non-UTF-8 bytes
 * from older indexing runs — don't fail serialization over it. */
static char *yy_doc_to_str(yyjson_mut_doc *doc) {
    size_t len = 0;
    char *s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
    return s;
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING
 * ══════════════════════════════════════════════════════════════════ */

int cbm_jsonrpc_parse(const char *line, cbm_jsonrpc_request_t *out) {
    memset(out, 0, sizeof(*out));
    out->id = CBM_NOT_FOUND;

    yyjson_doc *doc = yyjson_read(line, strlen(line), 0);
    if (!doc) {
        return CBM_NOT_FOUND;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return CBM_NOT_FOUND;
    }

    yyjson_val *v_jsonrpc = yyjson_obj_get(root, "jsonrpc");
    yyjson_val *v_method = yyjson_obj_get(root, "method");
    yyjson_val *v_id = yyjson_obj_get(root, "id");
    yyjson_val *v_params = yyjson_obj_get(root, "params");

    if (!v_method || !yyjson_is_str(v_method)) {
        yyjson_doc_free(doc);
        return CBM_NOT_FOUND;
    }

    out->jsonrpc =
        cbm_heap_strdup(v_jsonrpc && yyjson_is_str(v_jsonrpc) ? yyjson_get_str(v_jsonrpc) : "2.0");
    out->method = cbm_heap_strdup(yyjson_get_str(v_method));

    if (v_id) {
        out->has_id = true;
        if (yyjson_is_int(v_id)) {
            out->id = yyjson_get_int(v_id);
        } else if (yyjson_is_str(v_id)) {
            /* JSON-RPC 2.0 §4 permits string ids (Claude Desktop uses them).
             * Preserve verbatim instead of coercing via strtol (issue #253). */
            out->id_str = cbm_heap_strdup(yyjson_get_str(v_id));
        }
    }

    if (v_params) {
        out->params_raw = yyjson_val_write(v_params, 0, NULL);
    }

    yyjson_doc_free(doc);
    return 0;
}

void cbm_jsonrpc_request_free(cbm_jsonrpc_request_t *r) {
    if (!r) {
        return;
    }
    safe_str_free(&r->jsonrpc);
    safe_str_free(&r->method);
    safe_str_free(&r->id_str);
    safe_str_free(&r->params_raw);
    memset(r, 0, sizeof(*r));
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC FORMATTING
 * ══════════════════════════════════════════════════════════════════ */

char *cbm_jsonrpc_format_response(const cbm_jsonrpc_response_t *resp) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    if (resp->id_str) {
        yyjson_mut_obj_add_str(doc, root, "id", resp->id_str);
    } else {
        yyjson_mut_obj_add_int(doc, root, "id", resp->id);
    }

    if (resp->error_json) {
        /* Parse the error JSON and embed */
        yyjson_doc *err_doc = yyjson_read(resp->error_json, strlen(resp->error_json), 0);
        if (err_doc) {
            yyjson_mut_val *err_val = yyjson_val_mut_copy(doc, yyjson_doc_get_root(err_doc));
            yyjson_mut_obj_add_val(doc, root, "error", err_val);
            yyjson_doc_free(err_doc);
        }
    } else if (resp->result_json) {
        /* Parse the result JSON and embed */
        yyjson_doc *res_doc = yyjson_read(resp->result_json, strlen(resp->result_json), 0);
        if (res_doc) {
            yyjson_mut_val *res_val = yyjson_val_mut_copy(doc, yyjson_doc_get_root(res_doc));
            yyjson_mut_obj_add_val(doc, root, "result", res_val);
            yyjson_doc_free(res_doc);
        }
    } else {
        /* JSON-RPC 2.0 spec: response MUST contain "result" or "error" */
        yyjson_mut_obj_add_null(doc, root, "result");
    }

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

char *cbm_jsonrpc_format_error(int64_t id, int code, const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", id);

    yyjson_mut_val *err = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, err, "code", code);
    yyjson_mut_obj_add_str(doc, err, "message", message);
    yyjson_mut_obj_add_val(doc, root, "error", err);

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP PROTOCOL HELPERS
 * ══════════════════════════════════════════════════════════════════ */

char *cbm_mcp_text_result(const char *text, bool is_error) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *content = yyjson_mut_arr(doc);
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, item, "type", "text");
    yyjson_mut_obj_add_str(doc, item, "text", text ? text : "");
    yyjson_mut_arr_add_val(content, item);
    yyjson_mut_obj_add_val(doc, root, "content", content);

    if (!is_error && text) {
        yyjson_doc *structured_doc = yyjson_read(text, strlen(text), 0);
        if (structured_doc) {
            yyjson_val *structured_root = yyjson_doc_get_root(structured_doc);
            if (yyjson_is_obj(structured_root)) {
                yyjson_mut_val *structured = yyjson_val_mut_copy(doc, structured_root);
                yyjson_mut_obj_add_val(doc, root, "structuredContent", structured);
            }
            yyjson_doc_free(structured_doc);
        }
    }
    if (is_error) {
        /* Upstream f59d24fd (#1522): errors keep a bounded machine-readable
         * structuredContent.error. Text payloads carry no structuredContent
         * key at all (no duplication per f27f0a2a/#1375, no empty {} which
         * schema-honoring clients render as the whole result). JSON-object
         * payloads keep the parsed object above. */
        yyjson_mut_val *structured = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, structured, "error", text ? text : "");
        yyjson_mut_obj_add_val(doc, root, "structuredContent", structured);
    }
    yyjson_mut_obj_add_bool(doc, root, "isError", is_error);

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

bool cbm_mcp_cancel_request_matches(const char *params_json, int64_t active_id,
                                    const char *active_id_str) {
    if (!params_json) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(params_json, strlen(params_json), 0);
    if (!doc) {
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *request_id = yyjson_obj_get(root, "requestId");
    bool matches = false;
    if (request_id) {
        if (active_id_str) {
            matches =
                yyjson_is_str(request_id) && strcmp(yyjson_get_str(request_id), active_id_str) == 0;
        } else {
            matches = yyjson_is_int(request_id) && yyjson_get_int(request_id) == active_id;
        }
    }

    yyjson_doc_free(doc);
    return matches;
}

/* ── Tool definitions ─────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *title;
    const char *description;
    const char *input_schema; /* JSON string */
} tool_def_t;

static const tool_def_t TOOLS[] = {
    {"index_repository", "Index repository",
     "Index a repository into the knowledge graph.",
     "{\"type\":\"object\",\"properties\":{\"repo_path\":{\"type\":\"string\",\"description\":"
     "\"Path to the repository\"},"
     "\"path\":{\"type\":\"string\",\"description\":"
     "\"Alias for repo_path\"},"
     "\"mode\":{\"type\":\"string\","
     "\"enum\":[\"full\",\"moderate\",\"fast\"],"
     "\"default\":\"full\",\"description\":\"All modes run type-aware LSP call/usage "
     "resolution (per-file + cross-file). full: all files + similarity/semantic edges. "
     "moderate: filtered files + similarity/semantic. fast: filtered files, no "
     "similarity/semantic.\"},"
     "\"name\":{\"type\":\"string\",\"description\":"
     "\"Override the derived project name. Non-ASCII bytes are encoded and unsafe path characters "
     "are normalized.\"}"
     "},\"required\":[\"repo_path\"]}"},

    {"search_graph", "Search graph",
     "Search the code knowledge graph for functions, classes, routes, and variables. Use INSTEAD "
     "OF grep/glob when finding code definitions, implementations, or relationships. Three search "
     "modes: (1) query='update settings' for BM25 ranked full-text search with camelCase "
     "splitting and structural label boosting — recommended for natural-language discovery; "
     "(2) name_pattern='.*regex.*' for exact pattern matching; (3) semantic_query=[...] for "
     "vector cosine search that bridges vocabulary (finds 'publish' when you search 'send'). "
     "The three modes are independent and can be combined in a single call. "
     "PAGINATION: results are capped at limit (default 200) — broader queries are silently "
     "truncated. The response always includes 'total' (full match count before limit) and "
     "'has_more' (true when total > offset+returned). Detect truncation with has_more, then "
     "page by re-calling with offset=offset+limit until has_more is false, or pass next_cursor "
     "back as cursor; semantic rows page independently with semantic_offset/semantic_limit or "
     "semantic_next_cursor. Cursors are bound to the arguments and the index generation and "
     "fail with stale_cursor after a re-index instead of skipping or repeating rows. Narrow "
     "first via label/file_pattern/min_degree before paginating large result sets.",
     "{\"type\":\"object\",\"properties\":{"
     "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\","
     "\"description\":\"compact: indented key/value text with flat object arrays as "
     "tables; fewer bytes, same data.\"},"
     "\"project\":{\"type\":\"string\"},"
     "\"projects\":{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"string\"},"
     "\"description\":\"Project selectors to search together. Use [\\\"*\\\"] for all "
     "ready projects. Requires Zova full authority; ranking and pagination are global. "
     "Every result includes its owning project. Mutually exclusive with project.\"},"
     "\"query\":{\"type\":\"string\",\"description\":\"Natural-language or keyword full-text "
     "search using BM25 ranking. Tokens are split on whitespace; camelCase identifiers are "
     "indexed as individual words (updateCloudClient → update, cloud, client). Results are "
     "ranked with structural boosting: Functions/Methods +10, Routes +8, Classes/Interfaces +5. "
     "Noise labels (File/Folder/Module/Variable) are filtered out. When provided, name_pattern "
     "is ignored.\"},"
     "\"label\":{\"type\":\"string\"},\"name_pattern\":{\"type\":\"string\"},\"qn_pattern\":{"
     "\"type\":\"string\"},\"file_pattern\":{\"type\":\"string\"},"
     "\"relationship\":{\"type\":\"string\"},\"min_degree\":{\"type\":\"integer\"},"
     "\"max_degree\":{\"type\":\"integer\"},\"exclude_entry_points\":{\"type\":\"boolean\"},"
     "\"include_connected\":{\"type\":\"boolean\"},\"semantic_query\":{"
     "\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"MUST be an ARRAY of "
     "keyword strings (e.g. [\\\"send\\\",\\\"pubsub\\\",\\\"publish\\\"]) — NOT a single string. "
     "Each keyword is scored independently via per-keyword min-cosine; results reflect functions "
     "that score well on ALL keywords. Requires moderate/full index mode. Results appear in the "
     "'semantic_results' field (separate from 'results').\"},"
     "\"semantic_limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500,"
     "\"description\":\"Semantic rows per call. Default: limit, capped at 500.\"},"
     "\"semantic_offset\":{\"type\":\"integer\",\"default\":0,\"minimum\":0,"
     "\"maximum\":99998},"
     "\"semantic_cursor\":{\"type\":\"string\",\"description\":\"semantic_next_cursor "
     "from the previous page. Single-project only; mutually exclusive with "
     "semantic_offset.\"},"
     "\"cursor\":{\"type\":\"string\",\"description\":\"next_cursor from the previous "
     "page. Single-project only; mutually exclusive with offset.\"},"
     "\"limit\":{\"type\":"
     "\"integer\",\"description\":\"Max results per call. Default 200. Response carries "
     "'total' (full match count) and 'has_more' (true if truncated) so callers can "
     "detect the limit and paginate.\"},\"offset\":{\"type\":\"integer\",\"default\":0,"
     "\"description\":\"Skip the first N matching nodes. Combine with 'limit' to page: "
     "increment offset by limit and re-call while has_more is true.\"}},"
     "\"oneOf\":[{\"required\":[\"project\"]},{\"required\":[\"projects\"]}]}"},

    {"query_graph", "Query graph",
     "Execute a Cypher query against the knowledge graph for complex multi-hop patterns, "
     "aggregations, and cross-service analysis. The response includes 'total' (returned "
     "row count). There is a hard 100k row ceiling — for broad queries add LIMIT in the "
     "Cypher itself or use search_graph + offset/limit pagination instead. "
     "COMPLEXITY / BOTTLENECKS: every Function and Method node carries queryable complexity "
     "properties — cyclomatic (complexity), cognitive, loop_count, loop_depth (max nested-loop "
     "depth, a polynomial-degree proxy), plus interprocedural transitive_loop_depth (worst-case "
     "nested-loop degree propagated along CALLS edges) and a recursive flag. Additional "
     "hot-path signals: linear_scan_in_loop (count of find/contains/indexOf-style scans inside a "
     "loop — the hidden O(n^2) that loop_depth misses), alloc_in_loop (allocations/appends inside "
     "a loop), recursion_in_loop (a self-call inside a loop), unguarded_recursion (recursion with "
     "no conditionally-guarded base case), param_count and max_access_depth (structure smells). "
     "Find all hot-path candidates in one query, e.g. MATCH (f:Function) WHERE "
     "f.transitive_loop_depth >= 3 OR f.linear_scan_in_loop >= 1 RETURN f.qualified_name, "
     "f.transitive_loop_depth, f.linear_scan_in_loop ORDER BY f.transitive_loop_depth DESC.",
     "{\"type\":\"object\",\"properties\":{"
     "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\","
     "\"description\":\"compact: indented key/value text with flat object arrays as "
     "tables; fewer bytes, same data.\"},"
     "\"query\":{\"type\":\"string\",\"description\":\"Cypher "
     "query\"},\"project\":{\"type\":\"string\"},\"max_rows\":{\"type\":\"integer\","
     "\"description\":"
     "\"Optional row limit. Default: unlimited up to a 100k row "
     "ceiling. No offset support — use search_graph for paginated browsing.\"}},"
     "\"required\":[\"query\",\"project\"]}"},

    {"trace_path", "Trace path",
     "Trace paths through the code graph. Modes: calls (callers/callees), data_flow (value "
     "propagation with args at each hop), cross_service (through HTTP/async Route nodes). "
     "Use INSTEAD OF grep for callers, dependencies, impact analysis, or data flow tracing.",
     "{\"type\":\"object\",\"properties\":{"
     "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\","
     "\"description\":\"compact: indented key/value text with flat object arrays as "
     "tables; fewer bytes, same data.\"},"
     "\"function_name\":{\"type\":\"string\"},\"project\":{"
     "\"type\":\"string\"},\"file_path\":{\"type\":\"string\",\"description\":"
     "\"Keep only functions defined in this repository-relative file (or path suffix) when "
     "the name is ambiguous.\"},\"direction\":{\"type\":\"string\",\"enum\":[\"inbound\",\"outbound\","
     "\"both\"],\"default\":\"both\"},\"depth\":{\"type\":\"integer\",\"default\":3},\"mode\":{"
     "\"type\":\"string\",\"enum\":[\"calls\",\"data_flow\",\"cross_service\"],\"default\":"
     "\"calls\",\"description\":\"calls: follow CALLS edges. data_flow: follow CALLS+DATA_FLOWS "
     "with arg expressions. cross_service: follow HTTP_CALLS+ASYNC_CALLS+DATA_FLOWS through "
     "Routes.\"},\"parameter_name\":{\"type\":"
     "\"string\",\"description\":\"For data_flow mode: "
     "scope trace to a specific parameter name\"},\"edge_types\":{\"type\":\"array\",\"items\":{"
     "\"type\":\"string\"},\"description\":\"Replaces the mode's edge types. CALL_REFERENCE "
     "(a function passed or stored as a value) is not followed by mode calls; pass both "
     "CALLS and CALL_REFERENCE to follow it.\"},\"risk_labels\":{\"type\":\"boolean\",\"default\":false,"
     "\"description\":\"Add risk classification (CRITICAL/HIGH/MEDIUM/LOW) based on hop distance"
     "\"},\"include_tests\":{\"type\":\"boolean\",\"default\":false,"
     "\"description\":\"Include test files in results. When false (default), test files are "
     "filtered out. When true, test nodes are included with is_test=true marker."
     "\"},\"include_evidence\":{\"type\":\"boolean\",\"default\":false,"
     "\"description\":\"Add how each hop's edge was resolved: strategy (lsp | language_rule | "
     "heuristic | unresolved) and the resolver's confidence. Off by default. Use it to judge "
     "whether an edge is trustworthy, not to find edges.\"}},"
     "\"required\":[\"function_name\",\"project\"]}"},

    {"get_code_snippet", "Get code snippet",
     "Read source code for a function/class/symbol. IMPORTANT: First call search_graph to find the "
     "exact qualified_name, then pass it here. This is a read tool, not a search tool. Accepts "
     "full qualified_name (exact match) or short function name (returns suggestions if ambiguous).",
     "{\"type\":\"object\",\"properties\":{\"qualified_name\":{\"type\":\"string\",\"description\":"
     "\"Full qualified_name from search_graph, or short function name\"},\"project\":{"
     "\"type\":\"string\"},\"file_path\":{\"type\":\"string\",\"description\":"
     "\"Keep only matches defined in this repository-relative file (or path suffix) when a "
     "short name is ambiguous.\"},\"include_neighbors\":{"
     "\"type\":\"boolean\",\"default\":false}},\"required\":[\"qualified_name\",\"project\"]}"},

    {"get_file_outline", "Get file outline",
     "Return a compact declaration outline for one exact repository-relative file. Results are "
     "filtered by optional exact labels, ordered deterministically by source position, and "
     "bounded with exact total/offset/limit pagination metadata. File/folder/container nodes "
     "are excluded.",
     "{\"type\":\"object\",\"properties\":{"
     "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\","
     "\"description\":\"compact: indented key/value text with flat object arrays as "
     "tables; fewer bytes, same data.\"},"
     "\"project\":{\"type\":\"string\"},"
     "\"file_path\":{\"type\":\"string\",\"description\":\"Exact repository-relative "
     "file path\"},\"labels\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
     "\"maxItems\":16,\"description\":\"Optional exact node-label filter\"},"
     "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200,\"default\":100},"
     "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"default\":0}},"
     "\"required\":[\"project\",\"file_path\"]}"},

    {"get_graph_schema", "Get graph schema",
     "Get the schema of the knowledge graph (node labels, edge types)",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}},\"required\":["
     "\"project\"]}"},

    {"compare_graphs", "Compare graphs",
     "Compare two indexed projects, for example two branches or worktrees of one repository. "
     "Returns target-only additions and base-only removals of nodes (qualified name without "
     "the project prefix, label, file path) and edges (source, target, type). Each result set "
     "is capped by limit and a fixed 512 KiB encoded-byte budget; exact totals and truncation "
     "reasons are always reported.",
     "{\"type\":\"object\",\"properties\":{"
     "\"base_project\":{\"type\":\"string\",\"minLength\":1},"
     "\"target_project\":{\"type\":\"string\",\"minLength\":1},"
     "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000,\"default\":200},"
     "\"scan_limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000000,"
     "\"default\":2000000}},\"required\":[\"base_project\",\"target_project\"],"
     "\"additionalProperties\":false}"},

    {"get_architecture", "Get architecture",
     "Get high-level architecture overview — packages, services, dependencies, and project "
     "structure at a glance. Includes 'clusters': Leiden community detection over the call/import "
     "graph, surfacing the de-facto modules (each with a label, member count, cohesion score, "
     "representative top_nodes, and the packages/edge_types that bind it) — use these to grasp "
     "the real architectural seams, which often cut across the folder layout. Optional path scopes "
     "analysis to nodes under that directory prefix (file_path).",
     /* The aspects enum mirrors VALID_ASPECTS (see aspect_is_valid) — update both together. */
     "{\"type\":\"object\",\"properties\":{"
     "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\","
     "\"description\":\"compact: indented key/value text with flat object arrays as "
     "tables; fewer bytes, same data.\"},"
     "\"project\":{\"type\":\"string\"},\"path\":{\"type\":"
     "\"string\",\"description\":\"Optional directory prefix to scope architecture (e.g. "
     "apps/hoa)\"},"
     "\"aspects\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"all\","
     "\"overview\",\"structure\",\"dependencies\",\"routes\",\"languages\",\"packages\","
     "\"entry_points\",\"hotspots\",\"boundaries\",\"layers\",\"file_tree\",\"clusters\"]},"
     "\"description\":\"Aspects to include. 'all' = everything; 'overview' = compact summary "
     "(all except file_tree); omit = all.\"}},\"required\":[\"project\"]}"},

    {"search_code", "Search code",
     "Graph-augmented code search. Finds text patterns via grep, then enriches results with "
     "the knowledge graph: deduplicates matches into containing functions, ranks by structural "
     "importance (definitions first, popular functions next, tests last). "
     "Modes: compact (default, signatures only — token efficient), full (with source), "
     "files (just file paths). Use path_filter regex to scope results. "
     "TRUNCATION: enriched results are capped at limit (default 10). Response carries "
     "'total_grep_matches' (raw grep hit count) and 'total_results' (deduplicated function "
     "count) — compare to limit to detect truncation. There is no offset parameter; to see "
     "more, raise limit or narrow the query with file_pattern / path_filter.",
     "{\"type\":\"object\",\"properties\":{"
     "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\","
     "\"description\":\"compact: indented key/value text with flat object arrays as "
     "tables; fewer bytes, same data.\"},"
     "\"pattern\":{\"type\":\"string\"},\"project\":{\"type\":"
     "\"string\"},\"file_pattern\":{\"type\":\"string\",\"description\":\"Glob for grep "
     "--include (e.g. *.go)\"},\"path_filter\":{\"type\":\"string\",\"description\":\"Regex "
     "filter on result file paths (e.g. ^src/ or \\\\.(go|ts)$)\"},\"mode\":{\"type\":\"string\","
     "\"enum\":[\"compact\",\"full\",\"files\"],\"default\":\"compact\",\"description\":\"compact: "
     "signatures+metadata (default). full: with source. files: just file list.\"},"
     "\"context\":{\"type\":\"integer\",\"description\":\"Lines of context around each match "
     "(like grep -C). Only used in compact mode.\"},"
     "\"regex\":{\"type\":\"boolean\",\"default\":false},\"limit\":{\"type\":\"integer\","
     "\"description\":\"Max enriched results per call. Default 10. Response includes "
     "'total_grep_matches' and 'total_results' so callers can detect truncation. No "
     "offset parameter — raise limit or narrow with file_pattern / path_filter to see more."
     "\",\"default\":10}},\"required\":[\"pattern\",\"project\"]}"},

    {"list_projects", "List projects",
     "List indexed projects ordered by name. Pass limit/offset to page; each response carries "
     "catalog_token, and passing it back reports catalog_changed when projects were added, "
     "deleted, renamed or changed health between pages (restart from offset 0).",
     "{\"type\":\"object\",\"properties\":{"
     "\"offset\":{\"type\":\"integer\",\"default\":0,\"minimum\":0},"
     "\"limit\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":500,\"description\":"
     "\"Page size. Omit or 0 for every project.\"},"
     "\"include_details\":{\"type\":\"boolean\",\"default\":false,\"description\":"
     "\"Add node and edge counts per project.\"},"
     "\"catalog_token\":{\"type\":\"string\",\"description\":"
     "\"catalog_token from the previous page.\"},"
     "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\","
     "\"description\":\"compact: indented key/value text with flat object arrays as "
     "tables; fewer bytes, same data.\"}}}"},
    {"delete_project", "Delete project", "Delete a project from the index",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}},\"required\":["
     "\"project\"]}"},

    {"index_status", "Index status", "Get the indexing status of a project",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}},\"required\":["
     "\"project\"]}"},

    {"check_index_coverage", "Check index coverage",
     "Check per-file extraction coverage of a project's active index generation for exact "
     "repository-relative paths and path scopes. Use it for files you cite or act on, and on "
     "scopes before negative or exhaustive claims: files the parser skipped or only partly "
     "understood can be missing from graph results. Reports status (no_recorded_issue, partial "
     "with parse_error_ranges, unusable when one range covers most of the file, skipped, "
     "excluded, not_indexed, missing, coverage_unavailable), "
     "filesystem freshness against the indexed metadata, and a recommended_action. Best-effort "
     "and about extraction only. With neither paths nor scopes it summarizes the whole project "
     "(the same as scopes [\".\"]).",
     "{\"type\":\"object\",\"properties\":{"
     "\"project\":{\"type\":\"string\"},"
     "\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":128,"
     "\"description\":\"Repository-relative files to check exactly.\"},"
     "\"scopes\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":32,"
     "\"description\":\"Repository-relative path prefixes; . for the whole project.\"},"
     "\"scope_limit\":{\"type\":\"integer\",\"default\":200,\"minimum\":1,"
     "\"maximum\":1000,\"description\":\"Max gap entries per scope.\"},"
     "\"scope_offset\":{\"type\":\"integer\",\"default\":0,\"minimum\":0},"
     "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"compact\"],\"default\":\"json\","
     "\"description\":\"compact: indented key/value text with flat object arrays as "
     "tables; fewer bytes, same data.\"}},"
     "\"required\":[\"project\"]}"},

    {"detect_changes", "Detect changes", "Detect code changes and their impact",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"},\"scope\":{\"type\":"
     "\"string\"},\"depth\":{\"type\":\"integer\",\"default\":2},\"base_branch\":{\"type\":"
     "\"string\",\"default\":\"main\"},\"since\":{\"type\":\"string\",\"description\":"
     "\"Git ref or tag to compare from (e.g. HEAD~5, v0.5.0). Diffs <ref>...HEAD.\"}},"
     "\"required\":"
     "[\"project\"]}"},
};

static const int TOOL_COUNT = sizeof(TOOLS) / sizeof(TOOLS[0]);

/* MCP_TOOL_OUTPUT_SCHEMA removed (upstream f59d24fd, #1522): no tool
 * declares outputSchema; see mcp_add_tool_def. Kept as comment so a future
 * re-add is a conscious, reviewed decision. */

static void mcp_add_json_schema(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                                const char *schema_json) {
    yyjson_doc *schema_doc = yyjson_read(schema_json, strlen(schema_json), 0);
    if (schema_doc) {
        yyjson_mut_val *schema = yyjson_val_mut_copy(doc, yyjson_doc_get_root(schema_doc));
        if (schema) {
            yyjson_mut_obj_add_val(doc, obj, key, schema);
        }
        yyjson_doc_free(schema_doc);
    }
}

static void mcp_add_tool_def(yyjson_mut_doc *doc, yyjson_mut_val *tools, int i) {
    yyjson_mut_val *tool = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, tool, "name", TOOLS[i].name);
    yyjson_mut_obj_add_str(doc, tool, "title", TOOLS[i].title);
    yyjson_mut_obj_add_str(doc, tool, "description", TOOLS[i].description);

    mcp_add_json_schema(doc, tool, "inputSchema", TOOLS[i].input_schema);
    /* Deliberately no outputSchema (upstream f59d24fd, #1522). Tool output
     * is format-parameter-polymorphic (tree text by default, JSON object
     * under format:"json"), so no static schema is truthful — and a declared
     * schema makes spec-honoring clients read structuredContent as the
     * authoritative result, rendering every text reply as "{}". The blanket
     * {"type":"object","additionalProperties":true} it replaced validated
     * anything and informed nobody. */

    yyjson_mut_arr_add_val(tools, tool);
}

static char *cbm_mcp_tools_list_range(int offset, int limit, bool include_next_cursor) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *tools = yyjson_mut_arr(doc);

    if (offset < 0) {
        offset = 0;
    }
    if (offset > TOOL_COUNT) {
        offset = TOOL_COUNT;
    }
    if (limit < 0 || limit > TOOL_COUNT) {
        limit = TOOL_COUNT;
    }

    int end = offset + limit;
    if (end > TOOL_COUNT) {
        end = TOOL_COUNT;
    }

    for (int i = offset; i < end; i++) {
        mcp_add_tool_def(doc, tools, i);
    }

    yyjson_mut_obj_add_val(doc, root, "tools", tools);
    if (include_next_cursor && end < TOOL_COUNT) {
        char cursor[32];
        snprintf(cursor, sizeof(cursor), "%d", end);
        yyjson_mut_obj_add_strcpy(doc, root, "nextCursor", cursor);
    }

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

char *cbm_mcp_tools_list(void) {
    return cbm_mcp_tools_list_range(0, TOOL_COUNT, false);
}

/* Return the JSON input_schema string for a tool by name, or NULL if unknown.
 * Used by the CLI to build --flag arguments and per-tool --help from the same
 * source of truth the MCP tools/list advertises. Static lifetime; do not free. */
const char *cbm_mcp_tool_input_schema(const char *tool_name) {
    if (!tool_name) {
        return NULL;
    }
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (strcmp(TOOLS[i].name, tool_name) == 0) {
            return TOOLS[i].input_schema;
        }
    }
    return NULL;
}

int cbm_mcp_tool_count(void) {
    return TOOL_COUNT;
}

const char *cbm_mcp_tool_name(int i) {
    return i >= 0 && i < TOOL_COUNT ? TOOLS[i].name : NULL;
}

const char *cbm_mcp_tool_description(int i) {
    return i >= 0 && i < TOOL_COUNT ? TOOLS[i].description : NULL;
}

static int mcp_tools_cursor_offset(const char *params_json) {
    if (!params_json) {
        return 0;
    }

    yyjson_doc *doc = yyjson_read(params_json, strlen(params_json), 0);
    if (!doc) {
        return 0;
    }

    int offset = 0;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *cursor = root ? yyjson_obj_get(root, "cursor") : NULL;
    if (cursor) {
        offset = TOOL_COUNT;
        if (yyjson_is_str(cursor)) {
            const char *cursor_str = yyjson_get_str(cursor);
            if (cursor_str && *cursor_str != '\0') {
                char *endptr = NULL;
                errno = 0;
                long parsed = strtol(cursor_str, &endptr, 10);
                if (endptr && *endptr == '\0' && errno == 0 && parsed >= 0) {
                    offset = parsed > TOOL_COUNT ? TOOL_COUNT : (int)parsed;
                }
            }
        }
    }

    yyjson_doc_free(doc);
    return offset;
}

static char *cbm_mcp_tools_list_page(const char *params_json) {
    return cbm_mcp_tools_list_range(mcp_tools_cursor_offset(params_json), MCP_TOOLS_PAGE_SIZE,
                                    true);
}

/* Supported protocol versions, newest first. The server picks the newest
 * version that it shares with the client (per MCP spec version negotiation). */
static const char *SUPPORTED_PROTOCOL_VERSIONS[] = {
    "2025-11-25",
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
};
static const int SUPPORTED_VERSION_COUNT =
    (int)(sizeof(SUPPORTED_PROTOCOL_VERSIONS) / sizeof(SUPPORTED_PROTOCOL_VERSIONS[0]));

char *cbm_mcp_initialize_response(const char *params_json) {
    /* Determine protocol version: if client requests a version we support,
     * echo it back; otherwise respond with our latest. */
    const char *version = SUPPORTED_PROTOCOL_VERSIONS[0]; /* default: latest */
    if (params_json) {
        yyjson_doc *pdoc = yyjson_read(params_json, strlen(params_json), 0);
        if (pdoc) {
            yyjson_val *pv = yyjson_obj_get(yyjson_doc_get_root(pdoc), "protocolVersion");
            if (pv && yyjson_is_str(pv)) {
                const char *requested = yyjson_get_str(pv);
                for (int i = 0; i < SUPPORTED_VERSION_COUNT; i++) {
                    if (strcmp(requested, SUPPORTED_PROTOCOL_VERSIONS[i]) == 0) {
                        version = SUPPORTED_PROTOCOL_VERSIONS[i];
                        break;
                    }
                }
            }
            yyjson_doc_free(pdoc);
        }
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "protocolVersion", version);

    yyjson_mut_val *impl = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, impl, "name", "codebase-memory-mcp");
    yyjson_mut_obj_add_str(doc, impl, "version", cbm_cli_get_version());
    yyjson_mut_obj_add_val(doc, root, "serverInfo", impl);

    yyjson_mut_val *caps = yyjson_mut_obj(doc);
    yyjson_mut_val *tools_cap = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, tools_cap, "listChanged", false);
    yyjson_mut_obj_add_val(doc, caps, "tools", tools_cap);
    yyjson_mut_obj_add_val(doc, root, "capabilities", caps);

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION
 * ══════════════════════════════════════════════════════════════════ */

char *cbm_mcp_get_tool_name(const char *params_json) {
    yyjson_doc *doc = yyjson_read(params_json, strlen(params_json), 0);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *name = yyjson_obj_get(root, "name");
    char *result = NULL;
    if (name && yyjson_is_str(name)) {
        result = cbm_heap_strdup(yyjson_get_str(name));
    }
    yyjson_doc_free(doc);
    return result;
}

char *cbm_mcp_get_arguments(const char *params_json) {
    yyjson_doc *doc = yyjson_read(params_json, strlen(params_json), 0);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *args = yyjson_obj_get(root, "arguments");
    char *result = NULL;
    if (args) {
        result = yyjson_val_write(args, 0, NULL);
    }
    yyjson_doc_free(doc);
    return result ? result : cbm_heap_strdup("{}");
}

char *cbm_mcp_get_string_arg(const char *args_json, const char *key) {
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    char *result = NULL;
    if (val && yyjson_is_str(val)) {
        result = cbm_heap_strdup(yyjson_get_str(val));
    }
    yyjson_doc_free(doc);
    return result;
}

static char *canonicalize_repo_path_if_exists(char *repo_path) {
    if (!repo_path) {
        return NULL;
    }
    bool root_syntax = true;
    for (const char *p = repo_path; *p; p++) {
        if (*p != '/' && *p != '\\' && *p != ':') {
            root_syntax = false;
            break;
        }
    }
    if (root_syntax) {
        return repo_path;
    }

    char real[CBM_SZ_4K];
#ifdef _WIN32
    if (_access(repo_path, 0) == 0 && _fullpath(real, repo_path, sizeof(real))) {
        cbm_normalize_path_sep(real);
        char *canonical = cbm_heap_strdup(real);
        if (canonical) {
            free(repo_path);
            return canonical;
        }
    }
#else
    if (realpath(repo_path, real)) {
        cbm_normalize_path_sep(real);
        char *canonical = cbm_heap_strdup(real);
        if (canonical) {
            free(repo_path);
            return canonical;
        }
    }
#endif

    return repo_path;
}

static char *normalize_project_arg(char *project) {
    if (!project || (!strchr(project, '/') && !strchr(project, '\\'))) {
        return project;
    }

    /* Parent-qualified public selectors intentionally contain '/'. Only an
     * existing absolute filesystem path is a legacy path-form project arg. */
    bool absolute_path = project[0] == '/' ||
                         (strlen(project) >= 3 && project[1] == ':' &&
                          (project[2] == '/' || project[2] == '\\'));
    if (!absolute_path || !cbm_file_exists(project)) {
        return project;
    }

    project = canonicalize_repo_path_if_exists(project);
    char *normalized = cbm_project_name_from_path(project);
    if (normalized) {
        free(project);
        return normalized;
    }
    return project;
}

/* Resolve the project argument, accepting the canonical "project" key plus the
 * aliases a caller naturally reaches for (#640): list_projects surfaces the
 * field as "name" and the not-found hint says "pass the project name", so
 * "project_name" is the usual guess; "project_id" / "projectName" are accepted
 * too. NOT bare "name" — index_repository uses "name" for an explicit
 * project-name override. Caller must free() the result. */
static char *get_project_arg(const char *args_json) {
    char *p = cbm_mcp_get_string_arg(args_json, "project");
    if (!p) {
        p = cbm_mcp_get_string_arg(args_json, "project_name");
    }
    if (!p) {
        p = cbm_mcp_get_string_arg(args_json, "project_id");
    }
    if (!p) {
        p = cbm_mcp_get_string_arg(args_json, "projectName");
    }
    return normalize_project_arg(p);
}

typedef struct {
    char *project;
    char **projects;
    size_t project_count;
    bool wildcard;
} cbm_mcp_project_scope_t;

static void project_scope_free(cbm_mcp_project_scope_t *scope) {
    if (!scope) return;
    free(scope->project);
    for (size_t i = 0; i < scope->project_count; i++) free(scope->projects[i]);
    free(scope->projects);
    memset(scope, 0, sizeof(*scope));
}

static int project_scope_parse(const char *args_json, cbm_mcp_project_scope_t *scope,
                               const char **error) {
    if (!scope || !error) return -1;
    memset(scope, 0, sizeof(*scope));
    *error = NULL;

    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *projects = root ? yyjson_obj_get(root, "projects") : NULL;
    scope->project = get_project_arg(args_json);
    if (scope->project && projects) {
        *error = "project and projects are mutually exclusive";
        yyjson_doc_free(doc);
        return -1;
    }
    if (!projects) {
        if (!scope->project || !scope->project[0]) {
            *error = "exactly one of project or projects is required";
            yyjson_doc_free(doc);
            return -1;
        }
        yyjson_doc_free(doc);
        return 0;
    }
    if (!yyjson_is_arr(projects) || yyjson_arr_size(projects) == 0) {
        *error = "projects must be a non-empty array";
        yyjson_doc_free(doc);
        return -1;
    }

    size_t input_count = yyjson_arr_size(projects);
    scope->projects = calloc(input_count, sizeof(*scope->projects));
    if (!scope->projects) {
        *error = "out of memory while parsing projects";
        yyjson_doc_free(doc);
        return -1;
    }
    size_t index = 0;
    size_t maximum = 0;
    yyjson_val *value = NULL;
    yyjson_arr_foreach(projects, index, maximum, value) {
        if (!yyjson_is_str(value) || !yyjson_get_str(value)[0]) {
            *error = "projects entries must be non-empty strings";
            yyjson_doc_free(doc);
            return -1;
        }
        const char *name = yyjson_get_str(value);
        bool duplicate = false;
        for (size_t i = 0; i < scope->project_count; i++) {
            if (strcmp(scope->projects[i], name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        scope->projects[scope->project_count] = strdup(name);
        if (!scope->projects[scope->project_count]) {
            *error = "out of memory while parsing projects";
            yyjson_doc_free(doc);
            return -1;
        }
        scope->project_count++;
    }
    scope->wildcard = scope->project_count == 1 && strcmp(scope->projects[0], "*") == 0;
    if (!scope->wildcard) {
        for (size_t i = 0; i < scope->project_count; i++) {
            if (strcmp(scope->projects[i], "*") == 0) {
                *error = "projects wildcard must be the sole array entry";
                yyjson_doc_free(doc);
                return -1;
            }
        }
    }
    yyjson_doc_free(doc);
    return 0;
}

int cbm_mcp_get_int_arg(const char *args_json, const char *key, int default_val) {
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return default_val;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    int result = default_val;
    if (val && yyjson_is_int(val)) {
        /* Read the JSON integer as 64-bit and fall back to the default when it
         * does not fit an int. Casting first wraps silently: limit 4294967297
         * became 1, and 2147483648 became negative. */
        int64_t wide = yyjson_get_sint(val);
        if (wide >= (int64_t)INT_MIN && wide <= (int64_t)INT_MAX) {
            result = (int)wide;
        }
    }
    yyjson_doc_free(doc);
    return result;
}

bool cbm_mcp_get_bool_arg(const char *args_json, const char *key) {
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    bool result = false;
    if (val && yyjson_is_bool(val)) {
        result = yyjson_get_bool(val);
    }
    yyjson_doc_free(doc);
    return result;
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP SERVER
 * ══════════════════════════════════════════════════════════════════ */

struct cbm_mcp_server {
    cbm_store_t *store;             /* currently open project store (or NULL) */
    bool owns_store;                /* true if we opened the store */
    char *current_project;          /* which project store is open for (heap) */
    time_t store_last_used;         /* last time resolve_store was called for a named project */
    char update_notice[CBM_SZ_256]; /* one-shot update notice, cleared after first injection */
    bool update_checked;            /* true after background check has been launched */
    cbm_thread_t update_tid;        /* background update check thread */
    bool update_thread_active;      /* true if update thread was started and needs joining */

    /* Session + auto-index state */
    char session_root[CBM_SZ_1K];     /* detected project root path */
    char session_project[CBM_SZ_256]; /* derived project name */
    bool session_detected;            /* true after first detection attempt */
    struct cbm_watcher *watcher;      /* external watcher ref (not owned) */
    struct cbm_config *config;        /* external config ref (not owned) */
    cbm_thread_t autoindex_tid;
    bool autoindex_active; /* true if auto-index thread was started */

    /* Active pipeline tracking for cancellation support */
    cbm_pipeline_t *active_pipeline; /* non-NULL while index_repository runs */
    int64_t active_request_id;       /* JSON-RPC id of the in-progress tool call */
    char *active_request_id_str;     /* string JSON-RPC id of the in-progress tool call */
    cbm_mutex_t index_queue_mutex;
    uint64_t index_queue_next_ticket;
    uint64_t index_queue_serving_ticket;
    _Atomic uint64_t index_queue_waiters;
};

cbm_mcp_server_t *cbm_mcp_server_new(void) {
    cbm_mcp_server_t *srv = calloc(CBM_ALLOC_ONE, sizeof(*srv));
    if (!srv) {
        return NULL;
    }

    srv->store = cbm_store_open_memory();
    srv->owns_store = true;
    cbm_mutex_init(&srv->index_queue_mutex);

    return srv;
}

cbm_store_t *cbm_mcp_server_store(cbm_mcp_server_t *srv) {
    return srv ? srv->store : NULL;
}

void cbm_mcp_server_set_project(cbm_mcp_server_t *srv, const char *project) {
    if (!srv) {
        return;
    }
    free(srv->current_project);
    srv->current_project = project ? cbm_heap_strdup(project) : NULL;
}

void cbm_mcp_server_set_watcher(cbm_mcp_server_t *srv, struct cbm_watcher *w) {
    if (srv) {
        srv->watcher = w;
    }
}

void cbm_mcp_server_set_config(cbm_mcp_server_t *srv, struct cbm_config *cfg) {
    if (srv) {
        srv->config = cfg;
    }
}

void cbm_mcp_server_free(cbm_mcp_server_t *srv) {
    if (!srv) {
        return;
    }
    if (srv->update_thread_active) {
        cbm_thread_join(&srv->update_tid);
    }
    if (srv->autoindex_active) {
        cbm_thread_join(&srv->autoindex_tid);
    }
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
    }
    free(srv->current_project);
    free(srv->active_request_id_str);
    cbm_mutex_destroy(&srv->index_queue_mutex);
    free(srv);
}

/* ── Idle store eviction ──────────────────────────────────────── */

void cbm_mcp_server_evict_idle(cbm_mcp_server_t *srv, int timeout_s) {
    if (!srv || !srv->store) {
        return;
    }
    /* Protect initial in-memory stores that were never accessed via a named project.
     * store_last_used stays 0 until resolve_store is called with a non-NULL project. */
    if (srv->store_last_used == 0) {
        return;
    }

    time_t now = time(NULL);
    if ((now - srv->store_last_used) < timeout_s) {
        return;
    }

    if (srv->owns_store) {
        cbm_store_close(srv->store);
    }
    srv->store = NULL;
    free(srv->current_project);
    srv->current_project = NULL;
    srv->store_last_used = 0;
}

bool cbm_mcp_server_has_cached_store(cbm_mcp_server_t *srv) {
    return (srv && srv->store != NULL) != 0;
}

cbm_pipeline_t *cbm_mcp_server_active_pipeline(cbm_mcp_server_t *srv) {
    return srv ? srv->active_pipeline : NULL;
}

/* ── Store resolution ──────────────────────────────────────────── */

/* Open the project's workspace store in the shared cbm.zova for query tools.
 * Tracks last-access time so the event loop can evict idle stores. */
static cbm_store_t *resolve_store(cbm_mcp_server_t *srv, const char *project) {
    if (!project) {
        return NULL; /* project is required — no implicit fallback */
    }

    srv->store_last_used = time(NULL);

    char path[CBM_SZ_1K];
    cbm_zova_repository_t *repo = NULL;
    if (cbm_zova_user_database_path(path, sizeof(path)) != 0 ||
        !(repo = cbm_zova_repository_open(path, project))) {
        return NULL;
    }
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_id, sizeof(workspace_id), "%s",
             cbm_zova_repository_workspace_id(repo));
    cbm_zova_repository_close(repo);
    if (cbm_zova_workspace_id_validate(workspace_id) != 0) return NULL;
    if (srv->owns_store && srv->store) cbm_store_close(srv->store);
    srv->store = cbm_store_open_zova_workspace_query(path, workspace_id);
    srv->owns_store = srv->store != NULL;
    free(srv->current_project);
    srv->current_project = srv->store ? cbm_heap_strdup(project) : NULL;
    return srv->store;
}

/* Forward decl — definition lives below in handle_trace_call_path's helpers. */
static void free_node_contents(cbm_node_t *n);

/* Write the shared cbm.zova catalog's project selectors into out as
 * comma-separated quoted names. Returns the number of projects written. */
static int collect_zova_project_names(char *out, size_t out_sz) {
    int count = 0;
    int offset = 0;
    char path[CBM_SZ_1K];
    if (cbm_zova_user_database_path(path, sizeof(path)) != 0 || !cbm_file_exists(path)) {
        return 0;
    }
    cbm_zova_catalog_t *catalog = cbm_zova_catalog_open(path);
    cbm_zova_catalog_scope_t listed = {0};
    if (!catalog || cbm_zova_catalog_list(catalog, &listed) != CBM_STORE_OK) {
        cbm_zova_catalog_close(catalog);
        return 0;
    }
    for (int i = 0; i < listed.count; i++) {
        const char *name = listed.projects[i].selector;
        /* Element-boundary write: only emit this name if the WHOLE element —
         * optional leading comma + "name" — plus the NUL fits in what remains.
         * Never truncate mid-token; a partial name would corrupt the JSON array
         * (issue #235). Stop cleanly at the last name that fits: the array then
         * always holds complete names and `count` == its length. */
        size_t off = (size_t)offset;
        size_t need = strlen(name) + 2 /* quotes */ + (count > 0 ? 1u : 0u) /* comma */;
        if (off + need + 1 > out_sz) {
            break; /* would not fit entirely — stop at this element boundary */
        }
        if (count > 0) {
            out[offset++] = ',';
        }
        int wrote = snprintf(out + offset, out_sz - (size_t)offset, "\"%s\"", name);
        if (wrote > 0) {
            offset += wrote; /* guaranteed to fit (checked above) — no truncation */
        }
        count++;
    }
    cbm_zova_catalog_scope_free(&listed);
    cbm_zova_catalog_close(catalog);
    return count;
}

static void add_git_context_string(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                                   const char *value) {
    if (value) {
        yyjson_mut_obj_add_strcpy(doc, obj, key, value);
    } else {
        yyjson_mut_obj_add_null(doc, obj, key);
    }
}

static void add_git_context_json(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *root_path) {
    cbm_git_context_t ctx = {0};
    (void)cbm_git_context_resolve(root_path, &ctx);

    yyjson_mut_val *git = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, git, "is_git", ctx.is_git);
    yyjson_mut_obj_add_bool(doc, git, "is_worktree", ctx.is_worktree);
    yyjson_mut_obj_add_bool(doc, git, "is_detached", ctx.is_detached);
    yyjson_mut_obj_add_bool(doc, git, "root_exists", ctx.root_exists);
    add_git_context_string(doc, git, "worktree_root", ctx.worktree_root);
    add_git_context_string(doc, git, "git_dir", ctx.git_dir);
    add_git_context_string(doc, git, "git_common_dir", ctx.git_common_dir);
    add_git_context_string(doc, git, "canonical_root", ctx.canonical_root);
    add_git_context_string(doc, git, "branch", ctx.branch);
    add_git_context_string(doc, git, "branch_slug", ctx.branch_slug);
    add_git_context_string(doc, git, "head_sha", ctx.head_sha);
    add_git_context_string(doc, git, "base_sha", ctx.base_sha);
    yyjson_mut_obj_add_val(doc, obj, "git", git);

    cbm_git_context_free(&ctx);
}

/* Build a helpful error listing available projects. Caller must free() result. */
static char *build_project_list_error(const char *reason) {
    char projects[CBM_SZ_4K] = "";
    int count = collect_zova_project_names(projects, sizeof(projects));

    enum { ERR_BUF_SZ = 5120 };
    char buf[ERR_BUF_SZ];
    if (count > 0) {
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"%s\",\"hint\":\"Use list_projects to see all indexed projects, "
                 "then pass it as the \\\"project\\\" "
                 "argument.\",\"available_projects\":[%s],\"count\":%d}",
                 reason, projects, count);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"%s\",\"hint\":\"No projects indexed yet. "
                 "Call index_repository first.\"}",
                 reason);
    }
    return cbm_heap_strdup(buf);
}

/* Distinct from "unknown project": the caller omitted the project argument
 * entirely (no recognized key). Name the literal "project" key so the fix is
 * obvious (#640). Caller must free() result. */
static char *build_missing_project_error(void) {
    return cbm_heap_strdup("{\"error\":\"missing required argument: project\",\"hint\":\"Pass "
                           "the project as the \\\"project\\\" argument, e.g. "
                           "{\\\"project\\\":\\\"<name from list_projects>\\\"}. Run "
                           "list_projects to see indexed projects.\"}");
}

/* Pick the right no-store error: a NULL project means the argument was missing
 * (clearer message); a non-NULL project that didn't resolve means it's
 * unknown/unindexed (list the available ones). */
static char *build_no_store_error(const char *project) {
    return project ? build_project_list_error("project not found or not indexed")
                   : build_missing_project_error();
}

/* The error result for a missing or unknown project. Frees project. */
static char *project_not_found_result(char *project) {
    char *err = build_no_store_error(project && project[0] ? project : NULL);
    char *res = cbm_mcp_text_result(err, true);
    free(err);
    free(project);
    return res;
}

/* Bail with the right error when no store is available. */
#define REQUIRE_STORE(store, project)                  \
    do {                                               \
        if (!(store)) {                                \
            return project_not_found_result(project);  \
        }                                              \
    } while (0)

/* ── Tool handler implementations ─────────────────────────────── */

enum { LIST_PROJECTS_MAX_LIMIT = 500 };

/* Fingerprint of what decides list_projects paging: membership, order
 * (selectors are the sort key and change on collision renames) and health.
 * A new generation of an existing workspace does not move any row. */
static void catalog_page_token(const cbm_zova_catalog_scope_t *listed, char *out, size_t out_sz) {
    uint64_t h = 1469598103934665603ULL; /* FNV-1a 64 */
    for (int i = 0; i < listed->count; i++) {
        const cbm_zova_catalog_project_t *project = &listed->projects[i];
        const char *parts[] = {project->workspace_id, project->selector,
                               project->ready ? "r" : "-", project->healthy ? "h" : "-"};
        for (size_t k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
            for (const unsigned char *c = (const unsigned char *)parts[k]; c && *c; c++) {
                h = (h ^ *c) * 1099511628211ULL;
            }
            h = (h ^ 0x1fU) * 1099511628211ULL; /* field separator */
        }
    }
    snprintf(out, out_sz, "%016llx", (unsigned long long)h);
}

static void add_project_details(yyjson_mut_doc *doc, yyjson_mut_val *entry, const char *db_path,
                                const char *selector) {
    cbm_zova_repository_t *repo = cbm_zova_repository_open(db_path, selector);
    int nodes = 0;
    int edges = 0;
    bool ok = repo && cbm_zova_repository_counts(repo, cbm_zova_repository_workspace_id(repo),
                                                 &nodes, &edges) == CBM_STORE_OK;
    cbm_zova_repository_close(repo);
    if (ok) {
        yyjson_mut_obj_add_int(doc, entry, "nodes", nodes);
        yyjson_mut_obj_add_int(doc, entry, "edges", edges);
    } else {
        yyjson_mut_obj_add_str(doc, entry, "details_error", "counts unavailable");
    }
}

/* list_projects: the workspace catalog in the shared cbm.zova, ordered by
 * selector. offset/limit page through it (no limit = every project, the
 * original output); catalog_token from a previous page detects a catalog
 * that changed between pages. */
static char *handle_list_projects(cbm_mcp_server_t *srv, const char *args) {
    (void)srv;
    int offset = cbm_mcp_get_int_arg(args, "offset", 0);
    int limit = cbm_mcp_get_int_arg(args, "limit", 0);
    bool include_details = cbm_mcp_get_bool_arg(args, "include_details");
    char *page_token = cbm_mcp_get_string_arg(args, "catalog_token");
    if (offset < 0 || limit < 0) {
        free(page_token);
        return cbm_mcp_text_result("offset and limit must be >= 0", true);
    }
    if (limit > LIST_PROJECTS_MAX_LIMIT) {
        limit = LIST_PROJECTS_MAX_LIMIT;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_val *projects = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_val(doc, root, "projects", projects);

    char path[CBM_SZ_1K];
    cbm_zova_catalog_t *catalog = NULL;
    cbm_zova_catalog_scope_t listed = {0};
    bool have_db = cbm_zova_user_database_path(path, sizeof(path)) == 0 && cbm_file_exists(path);
    if (have_db) {
        catalog = cbm_zova_catalog_open(path);
        if (!catalog || cbm_zova_catalog_list(catalog, &listed) != CBM_STORE_OK) {
            cbm_zova_catalog_close(catalog);
            yyjson_mut_doc_free(doc);
            free(page_token);
            return cbm_mcp_text_result("{\"error\":\"cannot read Zova project catalog\"}",
                                       true);
        }
    }
    /* The catalog snapshot is taken; release the read before per-project
     * detail reads open their own. */
    cbm_zova_catalog_close(catalog);

    int start = offset < listed.count ? offset : listed.count;
    int end = limit > 0 && limit < listed.count - start ? start + limit : listed.count;
    for (int i = start; i < end; i++) {
        const cbm_zova_catalog_project_t *project = &listed.projects[i];
        yyjson_mut_val *entry = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, entry, "name", project->selector);
        /* The name index_repository reports and qualified names start with. */
        yyjson_mut_obj_add_strcpy(doc, entry, "project", project->project);
        yyjson_mut_obj_add_strcpy(doc, entry, "root_path", project->root_path);
        yyjson_mut_obj_add_sint(doc, entry, "generation", project->generation);
        yyjson_mut_obj_add_bool(doc, entry, "ready", project->ready);
        yyjson_mut_obj_add_bool(doc, entry, "healthy", project->healthy);
        yyjson_mut_obj_add_strcpy(doc, entry, "health_reason", project->health_reason);
        yyjson_mut_obj_add_strcpy(doc, entry, "indexed_at", project->indexed_at);
        yyjson_mut_obj_add_int(doc, entry, "vector_dimensions", project->vector_dimensions);
        if (include_details) {
            add_project_details(doc, entry, path, project->selector);
        }
        yyjson_mut_arr_add_val(projects, entry);
    }

    char token[CBM_SZ_32];
    catalog_page_token(&listed, token, sizeof(token));
    yyjson_mut_obj_add_int(doc, root, "total", listed.count);
    yyjson_mut_obj_add_int(doc, root, "offset", start);
    yyjson_mut_obj_add_int(doc, root, "returned", end - start);
    yyjson_mut_obj_add_bool(doc, root, "has_more", end < listed.count);
    if (end < listed.count) {
        yyjson_mut_obj_add_int(doc, root, "next_offset", end);
    }
    yyjson_mut_obj_add_strcpy(doc, root, "catalog_token", token);
    if (page_token && strcmp(page_token, token) != 0) {
        /* Workspaces were added, deleted, renamed or changed health since the
         * page that issued page_token: offsets no longer line up. */
        yyjson_mut_obj_add_bool(doc, root, "catalog_changed", true);
        yyjson_mut_obj_add_str(doc, root, "hint",
                               "The project catalog changed since the previous page. Restart "
                               "from offset 0 with the new catalog_token.");
    } else if (listed.count == 0) {
        yyjson_mut_obj_add_str(doc, root, "hint",
                               "No projects indexed. Call index_repository(repo_path=...) first.");
    }
    cbm_zova_catalog_scope_free(&listed);
    free(page_token);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* verify_project_indexed — returns a heap-allocated error JSON string when the
 * named project has not been indexed yet, or NULL when the project exists.
 * resolve_store returns NULL for a project missing from cbm.zova
 * (REQUIRE_STORE fires first). This function catches the remaining case: the
 * store opened but has no project row.
 * Callers that receive a non-NULL return value must free(project) themselves
 * before returning the error string. */
static char *verify_project_indexed(cbm_store_t *store, const char *project) {
    cbm_project_t proj_check = {0};
    if (cbm_store_get_project(store, project, &proj_check) != CBM_STORE_OK) {
        char *err = build_project_list_error("project not indexed — run index_repository first");
        char *res = cbm_mcp_text_result(err, true);
        free(err);
        return res;
    }
    cbm_project_free_fields(&proj_check);
    return NULL;
}

/* ── compare_graphs ─────────────────────────────────────────────── */

enum {
    MCP_COMPARE_DEFAULT_LIMIT = 200,
    MCP_COMPARE_MAX_LIMIT = 1000,
    MCP_COMPARE_DEFAULT_SCAN_LIMIT = 2000000,
    MCP_COMPARE_MAX_SCAN_LIMIT = 10000000,
    MCP_COMPARE_SET_BYTE_BUDGET = 512 * 1024,
};

/* compare_graphs bypasses resolve_store(): it needs two independently-owned
 * request-scoped stores, while resolve_store caches one on the server. */
static cbm_store_t *compare_open_project_store(const char *project) {
    char path[CBM_SZ_1K];
    cbm_zova_repository_t *repo = NULL;
    if (cbm_zova_user_database_path(path, sizeof(path)) != 0 ||
        !(repo = cbm_zova_repository_open(path, project))) {
        return NULL;
    }
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_id, sizeof(workspace_id), "%s", cbm_zova_repository_workspace_id(repo));
    cbm_zova_repository_close(repo);
    if (cbm_zova_workspace_id_validate(workspace_id) != 0) {
        return NULL;
    }
    return cbm_store_open_zova_workspace_query(path, workspace_id);
}

typedef struct {
    yyjson_mut_val *items;
    size_t returned;
    size_t encoded_bytes;
    bool budget_exhausted;
} compare_result_set_t;

typedef struct {
    yyjson_mut_doc *doc;
    size_t limit;
    compare_result_set_t nodes_added;
    compare_result_set_t nodes_removed;
    compare_result_set_t edges_added;
    compare_result_set_t edges_removed;
} compare_response_t;

static char *compare_graphs_error(const char *code, const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return cbm_mcp_text_result("compare_graphs failed: out of memory", true);
    }
    yyjson_mut_doc_set_root(doc, root);
    if (!yyjson_mut_obj_add_strcpy(doc, root, "error", message) ||
        !yyjson_mut_obj_add_strcpy(doc, root, "code", code)) {
        yyjson_mut_doc_free(doc);
        return cbm_mcp_text_result("compare_graphs failed: out of memory", true);
    }
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    if (!json) {
        return cbm_mcp_text_result("compare_graphs failed: out of memory", true);
    }
    char *result = cbm_mcp_text_result(json, true);
    free(json);
    return result;
}

static bool compare_arg_name_allowed(const char *name) {
    return strcmp(name, "base_project") == 0 || strcmp(name, "target_project") == 0 ||
           strcmp(name, "limit") == 0 || strcmp(name, "scan_limit") == 0;
}

static bool compare_parse_bounded_integer(yyjson_val *root, const char *key, int64_t default_value,
                                          int64_t maximum, uint64_t *out,
                                          const char **error_message) {
    yyjson_val *value = yyjson_obj_get(root, key);
    int64_t parsed = default_value;
    if (value) {
        if (!yyjson_is_int(value)) {
            *error_message = "limit values must be integers";
            return false;
        }
        parsed = yyjson_get_int(value);
    }
    if (parsed < 1 || parsed > maximum) {
        *error_message = strcmp(key, "limit") == 0 ? "limit must be between 1 and 1000"
                                                   : "scan_limit must be between 1 and 10000000";
        return false;
    }
    *out = (uint64_t)parsed;
    return true;
}

static bool compare_parse_arguments(const char *args, char **base_project, char **target_project,
                                    uint64_t *limit, uint64_t *scan_limit,
                                    const char **error_message) {
    *base_project = NULL;
    *target_project = NULL;
    yyjson_doc *doc = yyjson_read(args ? args : "{}", args ? strlen(args) : SLEN("{}"), 0);
    if (!doc) {
        *error_message = "arguments must be valid JSON";
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        *error_message = "arguments must be an object";
        yyjson_doc_free(doc);
        return false;
    }

    yyjson_obj_iter iterator = yyjson_obj_iter_with(root);
    yyjson_val *key = NULL;
    while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
        const char *name = yyjson_get_str(key);
        if (!name || !compare_arg_name_allowed(name)) {
            *error_message = "unknown argument";
            yyjson_doc_free(doc);
            return false;
        }
    }

    yyjson_val *base = yyjson_obj_get(root, "base_project");
    yyjson_val *target = yyjson_obj_get(root, "target_project");
    if (!base || !yyjson_is_str(base) || yyjson_get_len(base) == 0 || !target ||
        !yyjson_is_str(target) || yyjson_get_len(target) == 0) {
        *error_message = "base_project and target_project are required non-empty strings";
        yyjson_doc_free(doc);
        return false;
    }
    if (strcmp(yyjson_get_str(base), yyjson_get_str(target)) == 0) {
        *error_message = "base_project and target_project must be distinct";
        yyjson_doc_free(doc);
        return false;
    }
    if (!compare_parse_bounded_integer(root, "limit", MCP_COMPARE_DEFAULT_LIMIT,
                                       MCP_COMPARE_MAX_LIMIT, limit, error_message) ||
        !compare_parse_bounded_integer(root, "scan_limit", MCP_COMPARE_DEFAULT_SCAN_LIMIT,
                                       MCP_COMPARE_MAX_SCAN_LIMIT, scan_limit, error_message)) {
        yyjson_doc_free(doc);
        return false;
    }

    *base_project = cbm_heap_strdup(yyjson_get_str(base));
    *target_project = cbm_heap_strdup(yyjson_get_str(target));
    yyjson_doc_free(doc);
    if (!*base_project || !*target_project) {
        free(*base_project);
        free(*target_project);
        *base_project = NULL;
        *target_project = NULL;
        *error_message = "out of memory while validating arguments";
        return false;
    }
    return true;
}

static char *sanitize_utf8_lossy(const char *s);

static bool compare_add_identity_string(yyjson_mut_doc *doc, yyjson_mut_val *object,
                                        const char *key, const char *value) {
    char *sanitized = sanitize_utf8_lossy(value);
    if (!sanitized) {
        return false;
    }
    bool ok = yyjson_mut_obj_add_strcpy(doc, object, key, sanitized);
    free(sanitized);
    return ok;
}

static yyjson_mut_val *compare_node_json(yyjson_mut_doc *doc,
                                         const cbm_graph_node_identity_t *node) {
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    if (!object ||
        !compare_add_identity_string(doc, object, "qualified_name", node->qualified_name) ||
        !compare_add_identity_string(doc, object, "label", node->label) ||
        !compare_add_identity_string(doc, object, "file_path", node->file_path)) {
        return NULL;
    }
    return object;
}

static bool compare_append_item(compare_response_t *response, compare_result_set_t *set,
                                yyjson_mut_doc *item_doc, yyjson_mut_val *item) {
    if (!item_doc || !item) {
        yyjson_mut_doc_free(item_doc);
        return false;
    }
    char *encoded = yy_doc_to_str(item_doc);
    if (!encoded) {
        yyjson_mut_doc_free(item_doc);
        return false;
    }
    size_t encoded_len = strlen(encoded);
    size_t separator = set->returned > 0 ? 1U : 0U;
    free(encoded);

    if (set->encoded_bytes > MCP_COMPARE_SET_BYTE_BUDGET ||
        separator > MCP_COMPARE_SET_BYTE_BUDGET - set->encoded_bytes ||
        encoded_len > MCP_COMPARE_SET_BYTE_BUDGET - set->encoded_bytes - separator) {
        set->budget_exhausted = true;
        yyjson_mut_doc_free(item_doc);
        return true;
    }

    yyjson_mut_val *copy = yyjson_mut_val_mut_copy(response->doc, item);
    bool ok = copy && yyjson_mut_arr_add_val(set->items, copy);
    yyjson_mut_doc_free(item_doc);
    if (!ok) {
        return false;
    }
    set->encoded_bytes += separator + encoded_len;
    set->returned++;
    return true;
}

static bool compare_node_callback(void *context, bool added,
                                  const cbm_graph_node_identity_t *node) {
    compare_response_t *response = (compare_response_t *)context;
    compare_result_set_t *set = added ? &response->nodes_added : &response->nodes_removed;
    if (set->returned >= response->limit || set->budget_exhausted) {
        return true;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *item = doc ? compare_node_json(doc, node) : NULL;
    if (doc && item) {
        yyjson_mut_doc_set_root(doc, item);
    }
    return compare_append_item(response, set, doc, item);
}

static bool compare_edge_callback(void *context, bool added,
                                  const cbm_graph_edge_identity_t *edge) {
    compare_response_t *response = (compare_response_t *)context;
    compare_result_set_t *set = added ? &response->edges_added : &response->edges_removed;
    if (set->returned >= response->limit || set->budget_exhausted) {
        return true;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *item = doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *source = doc ? compare_node_json(doc, &edge->source) : NULL;
    yyjson_mut_val *target = doc ? compare_node_json(doc, &edge->target) : NULL;
    bool ok = item && source && target && yyjson_mut_obj_add_val(doc, item, "source", source) &&
              yyjson_mut_obj_add_val(doc, item, "target", target) &&
              compare_add_identity_string(doc, item, "type", edge->type) &&
              (!edge->local_name[0] ||
               compare_add_identity_string(doc, item, "local_name", edge->local_name));
    if (ok) {
        yyjson_mut_doc_set_root(doc, item);
    }
    return compare_append_item(response, set, doc, ok ? item : NULL);
}

static yyjson_mut_val *compare_project_json(yyjson_mut_doc *doc, const char *project,
                                            const cbm_graph_compare_project_t *metadata) {
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    if (!object || !yyjson_mut_obj_add_strcpy(doc, object, "project", project) ||
        !yyjson_mut_obj_add_sint(doc, object, "generation", metadata->generation) ||
        !compare_add_identity_string(doc, object, "indexed_at", metadata->indexed_at) ||
        !yyjson_mut_obj_add_sint(doc, object, "node_count", metadata->node_count) ||
        !yyjson_mut_obj_add_sint(doc, object, "edge_count", metadata->edge_count)) {
        return NULL;
    }
    return object;
}

static yyjson_mut_val *compare_set_json(yyjson_mut_doc *doc, compare_result_set_t *set,
                                        uint64_t total, size_t limit) {
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    yyjson_mut_val *reasons = yyjson_mut_arr(doc);
    bool truncated = total > (uint64_t)set->returned;
    if (!object || !reasons || !yyjson_mut_obj_add_val(doc, object, "items", set->items) ||
        !yyjson_mut_obj_add_uint(doc, object, "returned", set->returned) ||
        !yyjson_mut_obj_add_uint(doc, object, "total", total) ||
        !yyjson_mut_obj_add_bool(doc, object, "truncated", truncated)) {
        return NULL;
    }
    if (truncated && set->returned >= limit && !yyjson_mut_arr_add_strcpy(doc, reasons, "limit")) {
        return NULL;
    }
    if (truncated && set->budget_exhausted &&
        !yyjson_mut_arr_add_strcpy(doc, reasons, "encoded_byte_budget")) {
        return NULL;
    }
    if (!yyjson_mut_obj_add_val(doc, object, "truncation_reasons", reasons)) {
        return NULL;
    }
    return object;
}

static char *handle_compare_graphs(cbm_mcp_server_t *srv, const char *args) {
    (void)srv;
    char *base_project = NULL;
    char *target_project = NULL;
    uint64_t limit = 0;
    uint64_t scan_limit = 0;
    const char *argument_error = NULL;
    if (!compare_parse_arguments(args, &base_project, &target_project, &limit, &scan_limit,
                                 &argument_error)) {
        return compare_graphs_error("invalid_arguments", argument_error);
    }

    cbm_store_t *base_store = compare_open_project_store(base_project);
    if (!base_store) {
        free(base_project);
        free(target_project);
        return compare_graphs_error("project_not_indexed", "base project is not indexed");
    }
    cbm_store_t *target_store = compare_open_project_store(target_project);
    if (!target_store) {
        cbm_store_close(base_store);
        free(base_project);
        free(target_project);
        return compare_graphs_error("project_not_indexed", "target project is not indexed");
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    compare_response_t response = {
        .doc = doc,
        .limit = (size_t)limit,
        .nodes_added = {.items = doc ? yyjson_mut_arr(doc) : NULL, .encoded_bytes = 2U},
        .nodes_removed = {.items = doc ? yyjson_mut_arr(doc) : NULL, .encoded_bytes = 2U},
        .edges_added = {.items = doc ? yyjson_mut_arr(doc) : NULL, .encoded_bytes = 2U},
        .edges_removed = {.items = doc ? yyjson_mut_arr(doc) : NULL, .encoded_bytes = 2U},
    };
    if (!doc || !root || !response.nodes_added.items || !response.nodes_removed.items ||
        !response.edges_added.items || !response.edges_removed.items) {
        cbm_store_close(target_store);
        cbm_store_close(base_store);
        yyjson_mut_doc_free(doc);
        free(base_project);
        free(target_project);
        return compare_graphs_error("allocation_failed", "could not allocate comparison result");
    }
    yyjson_mut_doc_set_root(doc, root);

    cbm_graph_compare_result_t comparison = {0};
    int compare_rc = cbm_store_compare_graphs(base_store, base_project, target_store,
                                              target_project, scan_limit, compare_node_callback,
                                              compare_edge_callback, &response, &comparison);
    cbm_store_close(target_store);
    cbm_store_close(base_store);

    if (compare_rc != CBM_STORE_OK) {
        yyjson_mut_doc_free(doc);
        free(base_project);
        free(target_project);
        if (compare_rc == CBM_STORE_SCAN_LIMIT) {
            return compare_graphs_error("scan_limit_exceeded",
                                        "combined graph rows exceed scan_limit");
        }
        return compare_graphs_error("query_failed", "graph comparison failed");
    }

    yyjson_mut_val *base = compare_project_json(doc, base_project, &comparison.base);
    yyjson_mut_val *target = compare_project_json(doc, target_project, &comparison.target);
    yyjson_mut_val *nodes = yyjson_mut_obj(doc);
    yyjson_mut_val *edges = yyjson_mut_obj(doc);
    yyjson_mut_val *limits = yyjson_mut_obj(doc);
    yyjson_mut_val *nodes_added =
        compare_set_json(doc, &response.nodes_added, comparison.nodes_added_total, response.limit);
    yyjson_mut_val *nodes_removed = compare_set_json(
        doc, &response.nodes_removed, comparison.nodes_removed_total, response.limit);
    yyjson_mut_val *edges_added =
        compare_set_json(doc, &response.edges_added, comparison.edges_added_total, response.limit);
    yyjson_mut_val *edges_removed = compare_set_json(
        doc, &response.edges_removed, comparison.edges_removed_total, response.limit);
    bool built =
        base && target && nodes && edges && limits && nodes_added && nodes_removed && edges_added &&
        edges_removed && yyjson_mut_obj_add_int(doc, root, "schema_version", 1) &&
        yyjson_mut_obj_add_val(doc, root, "base", base) &&
        yyjson_mut_obj_add_val(doc, root, "target", target) &&
        yyjson_mut_obj_add_val(doc, nodes, "added", nodes_added) &&
        yyjson_mut_obj_add_val(doc, nodes, "removed", nodes_removed) &&
        yyjson_mut_obj_add_val(doc, root, "nodes", nodes) &&
        yyjson_mut_obj_add_val(doc, edges, "added", edges_added) &&
        yyjson_mut_obj_add_val(doc, edges, "removed", edges_removed) &&
        yyjson_mut_obj_add_val(doc, root, "edges", edges) &&
        yyjson_mut_obj_add_uint(doc, limits, "limit", limit) &&
        yyjson_mut_obj_add_uint(doc, limits, "scan_limit", scan_limit) &&
        yyjson_mut_obj_add_uint(doc, limits, "encoded_byte_budget", MCP_COMPARE_SET_BYTE_BUDGET) &&
        yyjson_mut_obj_add_val(doc, root, "limits", limits);
    free(base_project);
    free(target_project);
    if (!built) {
        yyjson_mut_doc_free(doc);
        return compare_graphs_error("allocation_failed", "could not allocate comparison result");
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    if (!json) {
        return compare_graphs_error("allocation_failed", "could not serialize comparison result");
    }
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_get_graph_schema(cbm_mcp_server_t *srv, const char *args) {
    char *project = get_project_arg(args);
    cbm_store_t *store = resolve_store(srv, project);
    REQUIRE_STORE(store, project);

    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(project);
        return not_indexed;
    }

    cbm_schema_info_t schema = {0};
    if (cbm_store_get_schema(store, project, &schema) != CBM_STORE_OK) {
        free(project);
        return cbm_mcp_text_result("failed to read the graph schema", true);
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *labels = yyjson_mut_arr(doc);
    for (int i = 0; i < schema.node_label_count; i++) {
        yyjson_mut_val *lbl = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, lbl, "label", schema.node_labels[i].label);
        yyjson_mut_obj_add_int(doc, lbl, "count", schema.node_labels[i].count);
        yyjson_mut_val *props = yyjson_mut_arr(doc);
        for (int j = 0; j < schema.node_labels[i].property_count; j++) {
            yyjson_mut_arr_add_str(doc, props, schema.node_labels[i].properties[j]);
        }
        yyjson_mut_obj_add_val(doc, lbl, "properties", props);
        yyjson_mut_arr_add_val(labels, lbl);
    }
    yyjson_mut_obj_add_val(doc, root, "node_labels", labels);

    yyjson_mut_val *types = yyjson_mut_arr(doc);
    for (int i = 0; i < schema.edge_type_count; i++) {
        yyjson_mut_val *typ = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, typ, "type", schema.edge_types[i].type);
        yyjson_mut_obj_add_int(doc, typ, "count", schema.edge_types[i].count);
        yyjson_mut_val *eprops = yyjson_mut_arr(doc);
        for (int j = 0; j < schema.edge_types[i].property_count; j++) {
            yyjson_mut_arr_add_str(doc, eprops, schema.edge_types[i].properties[j]);
        }
        yyjson_mut_obj_add_val(doc, typ, "properties", eprops);
        yyjson_mut_arr_add_val(types, typ);
    }
    yyjson_mut_obj_add_val(doc, root, "edge_types", types);


    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_store_schema_free(&schema);
    free(project);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* Validate edge type: uppercase letters + underscore only, max 64 chars. */
static bool validate_edge_type(const char *s) {
    if (!s || strlen(s) > CBM_SZ_64) {
        return false;
    }
    for (const char *c = s; *c; c++) {
        if (!(*c >= 'A' && *c <= 'Z') && *c != '_') {
            return false;
        }
    }
    return true;
}

/* Enrich search result with 1-hop connected node names. */
/* Add BFS results to a yyjson array (deduped by name). */
static void enrich_add_bfs(yyjson_mut_doc *doc, yyjson_mut_val *arr, cbm_traverse_result_t *tr) {
    for (int j = 0; j < tr->visited_count; j++) {
        if (tr->visited[j].node.name) {
            yyjson_mut_arr_add_strcpy(doc, arr, tr->visited[j].node.name);
        }
    }
}

/* Enrich search result with 1-hop connected node names (inbound + outbound). */
static void enrich_connected(yyjson_mut_doc *doc, yyjson_mut_val *item, cbm_store_t *store,
                             int64_t node_id, const char *relationship) {
    const char *et[] = {relationship ? relationship : "CALLS"};
    yyjson_mut_val *conn = yyjson_mut_arr(doc);

    /* BFS doesn't support "both" — run inbound + outbound separately. */
    cbm_traverse_result_t tr_in = {0};
    cbm_store_bfs(store, node_id, "inbound", et, SKIP_ONE, SKIP_ONE, MCP_DEFAULT_LIMIT, &tr_in);
    enrich_add_bfs(doc, conn, &tr_in);
    cbm_store_traverse_free(&tr_in);

    cbm_traverse_result_t tr_out = {0};
    cbm_store_bfs(store, node_id, "outbound", et, SKIP_ONE, SKIP_ONE, MCP_DEFAULT_LIMIT, &tr_out);
    enrich_add_bfs(doc, conn, &tr_out);
    cbm_store_traverse_free(&tr_out);

    if (yyjson_mut_arr_size(conn) > 0) {
        yyjson_mut_obj_add_val(doc, item, "connected_names", conn);
    }
}

/* Build an FTS5 MATCH expression from a free-form query string by splitting
 * on whitespace and joining the terms with OR.  Each token is also sanitized:
 * anything that isn't alnum or underscore is dropped, so the caller can't
 * inject FTS5 operators or double-quoted phrases.  Returns the number of
 * tokens emitted (0 if the query contained no usable terms). */
enum {
    BM25_MIN_BUF = 2, /* minimum buffer size: at least NUL + one char */
    BM25_SEP_RESERVE = 1,
    BM25_QUERY_BUF = 1024,
    BM25_DEFAULT_LIMIT = 100,
};

static int bm25_build_match(const char *query, char *out, size_t out_size) {
    if (!query || !out || out_size < BM25_MIN_BUF) {
        return 0;
    }
    size_t pos = 0;
    int tokens = 0;
    const char *p = query;
    while (*p) {
        while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_')) {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *tok_start = p;
        while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                      (*p >= '0' && *p <= '9') || *p == '_')) {
            p++;
        }
        size_t tok_len = (size_t)(p - tok_start);
        if (tok_len == 0) {
            continue;
        }
        const char *sep = (tokens > 0) ? " OR " : "";
        size_t sep_len = strlen(sep);
        if (pos + sep_len + tok_len + 2 + BM25_SEP_RESERVE >= out_size) {
            break; /* out of room — stop cleanly, keep what we have */
        }
        memcpy(out + pos, sep, sep_len);
        pos += sep_len;
        /* Quoted so a word such as OR or NOT is a term, not an FTS5 operator. */
        out[pos++] = '"';
        memcpy(out + pos, tok_start, tok_len);
        pos += tok_len;
        out[pos++] = '"';
        tokens++;
    }
    out[pos] = '\0';
    return tokens;
}

/* True when the query uses FTS5 expression syntax (a phrase, a prefix, grouping
 * or an uppercase operator) and should reach MATCH unchanged. */
static bool bm25_is_expression(const char *query) {
    if (strpbrk(query, "\"*()")) {
        return true;
    }
    static const char *const operators[] = {"AND", "OR", "NOT", "NEAR"};
    for (const char *p = query; *p;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t len = strcspn(p, " \t");
        for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); i++) {
            if (len == strlen(operators[i]) && strncmp(p, operators[i], len) == 0) {
                return true;
            }
        }
        p += len;
    }
    return false;
}

/* Match expression for the shared cbm.zova FTS index: plain words match any
 * word; FTS5 expressions pass through. Returns false when nothing is left. */
static bool zova_fts_match(const char *query, char *out, size_t out_size) {
    if (bm25_is_expression(query)) {
        return snprintf(out, out_size, "%s", query) < (int)out_size;
    }
    return bm25_build_match(query, out, out_size) > 0;
}

/* Forward declaration — defined later. enrich_node_properties parses the
 * node's properties_json and grafts the parsed values onto the result item.
 * It returns the parsed yyjson_doc which must outlive the serialization
 * because yyjson_mut_obj_add_val uses zero-copy strings into that doc. */
static yyjson_doc *enrich_node_properties(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                                          const char *properties_json);

/* Emit the cbm_store_search results as a JSON "results" array on the doc.
 * Property docs created via enrich_node_properties are collected in
 * *out_pdocs (count in *out_pdoc_count) and must be freed by the caller
 * AFTER serializing doc, since yyjson_mut strings are zero-copy pointers
 * into those parsed docs. The caller also frees out_pdocs itself. */
static void emit_search_results(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                const cbm_search_output_t *out, cbm_store_t *store,
                                const char *relationship, bool include_connected, int offset,
                                yyjson_doc ***out_pdocs, int *out_pdoc_count) {
    yyjson_doc **pdocs = out->count > 0 ? malloc((size_t)out->count * sizeof(yyjson_doc *)) : NULL;
    int pdoc_count = 0;
    yyjson_mut_obj_add_int(doc, root, "total", out->total);
    yyjson_mut_val *results = yyjson_mut_arr(doc);
    for (int i = 0; i < out->count; i++) {
        cbm_search_result_t *sr = &out->results[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "name", sr->node.name ? sr->node.name : "");
        yyjson_mut_obj_add_str(doc, item, "qualified_name",
                               sr->node.qualified_name ? sr->node.qualified_name : "");
        yyjson_mut_obj_add_str(doc, item, "label", sr->node.label ? sr->node.label : "");
        yyjson_mut_obj_add_str(doc, item, "file_path",
                               sr->node.file_path ? sr->node.file_path : "");
        yyjson_mut_obj_add_int(doc, item, "in_degree", sr->in_degree);
        yyjson_mut_obj_add_int(doc, item, "out_degree", sr->out_degree);
        if (include_connected && sr->node.id > 0) {
            enrich_connected(doc, item, store, sr->node.id, relationship);
        }
        yyjson_doc *pdoc = enrich_node_properties(doc, item, sr->node.properties_json);
        if (pdoc && pdocs) {
            pdocs[pdoc_count++] = pdoc;
        }
        yyjson_mut_arr_add_val(results, item);
    }
    yyjson_mut_obj_add_val(doc, root, "results", results);
    yyjson_mut_obj_add_bool(doc, root, "has_more", out->total > offset + out->count);
    *out_pdocs = pdocs;
    *out_pdoc_count = pdoc_count;
}

static bool run_semantic_query(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *args,
                               cbm_store_t *store, const char *project, int offset, int limit,
                               int *next_offset);

static bool search_args_has_semantic_query(const char *args) {
    yyjson_doc *args_doc = yyjson_read(args, strlen(args), 0);
    yyjson_val *args_root = args_doc ? yyjson_doc_get_root(args_doc) : NULL;
    bool present = args_root && yyjson_obj_get(args_root, "semantic_query");
    if (args_doc) {
        yyjson_doc_free(args_doc);
    }
    return present;
}

static void emit_flagged_fts_results(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                     const cbm_search_output_t *out, int offset) {
    yyjson_mut_obj_add_int(doc, root, "total", out->total);
    yyjson_mut_obj_add_str(doc, root, "search_mode", "bm25");
    yyjson_mut_val *results = yyjson_mut_arr(doc);
    for (int i = 0; i < out->count; i++) {
        const cbm_search_result_t *sr = &out->results[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "name", sr->node.name ? sr->node.name : "");
        yyjson_mut_obj_add_str(doc, item, "qualified_name",
                               sr->node.qualified_name ? sr->node.qualified_name : "");
        yyjson_mut_obj_add_str(doc, item, "label", sr->node.label ? sr->node.label : "");
        yyjson_mut_obj_add_str(doc, item, "file_path",
                               sr->node.file_path ? sr->node.file_path : "");
        yyjson_mut_obj_add_int(doc, item, "start_line", sr->node.start_line);
        yyjson_mut_obj_add_int(doc, item, "end_line", sr->node.end_line);
        yyjson_mut_obj_add_real(doc, item, "rank", sr->rank);
        yyjson_mut_arr_add_val(results, item);
    }
    yyjson_mut_obj_add_val(doc, root, "results", results);
    yyjson_mut_obj_add_bool(doc, root, "has_more", out->total > offset + out->count);
}

/* search_graph continuation cursors. A cursor names the next offset of one
 * result list (s = structural/BM25 rows, v = semantic rows) and binds it to the
 * query arguments (everything except paging and format) and to the Zova
 * generation it was issued against. A cursor replayed against other arguments
 * or a re-indexed graph fails closed instead of skipping or repeating rows.
 * Format: sg1.<kind>.<generation>.<query hash>.<offset> */
enum { SG_CURSOR_SZ = 96, SG_SEMANTIC_MAX_LIMIT = 500, SG_SEMANTIC_MAX_OFFSET = 99998 };

static bool sg_is_paging_key(const char *key) {
    static const char *const keys[] = {"project",         "offset",         "limit",
                                       "cursor",          "semantic_offset", "semantic_limit",
                                       "semantic_cursor", "format"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(key, keys[i]) == 0) {
            return true;
        }
    }
    return false;
}

static int sg_key_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static uint64_t sg_fnv(uint64_t h, const char *s) {
    for (const unsigned char *c = (const unsigned char *)s; c && *c; c++) {
        h = (h ^ *c) * 1099511628211ULL;
    }
    return (h ^ 0x1fU) * 1099511628211ULL; /* field separator */
}

/* FNV-1a over the project and every non-paging argument in key order, so a
 * client may reorder arguments between pages. Returns false on OOM. */
static bool sg_query_hash(yyjson_val *args_root, const char *project, uint64_t *out) {
    uint64_t h = sg_fnv(1469598103934665603ULL, project);
    size_t n = yyjson_is_obj(args_root) ? yyjson_obj_size(args_root) : 0;
    const char **keys = n ? calloc(n, sizeof(*keys)) : NULL;
    if (n && !keys) {
        return false;
    }
    size_t count = 0;
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *key = NULL;
    yyjson_val *value = NULL;
    if (n) {
        yyjson_obj_foreach(args_root, idx, max, key, value) {
            if (!sg_is_paging_key(yyjson_get_str(key))) {
                keys[count++] = yyjson_get_str(key);
            }
        }
    }
    qsort(keys, count, sizeof(*keys), sg_key_cmp);
    bool ok = true;
    for (size_t i = 0; i < count && ok; i++) {
        char *encoded = yyjson_val_write(yyjson_obj_get(args_root, keys[i]), 0, NULL);
        ok = encoded != NULL;
        h = sg_fnv(sg_fnv(h, keys[i]), encoded);
        free(encoded);
    }
    free(keys);
    *out = h;
    return ok;
}

static void sg_cursor_format(char *out, size_t out_sz, char kind, int64_t generation,
                             uint64_t hash, int offset) {
    snprintf(out, out_sz, "sg1.%c.%lld.%016llx.%d", kind, (long long)generation,
             (unsigned long long)hash, offset);
}

/* Resolve the start offset of one result list from its cursor or offset
 * argument. Returns NULL on success, otherwise an error written to err. */
static const char *sg_resume_offset(yyjson_val *args_root, const char *cursor_key,
                                    const char *offset_key, char kind, int64_t generation,
                                    uint64_t hash, int *offset, char *err, size_t err_sz) {
    yyjson_val *cursor = yyjson_obj_get(args_root, cursor_key);
    if (!cursor) {
        return NULL;
    }
    if (yyjson_obj_get(args_root, offset_key)) {
        snprintf(err, err_sz, "%s and %s are mutually exclusive", cursor_key, offset_key);
        return err;
    }
    const char *token = yyjson_get_str(cursor);
    char parsed_kind = 0;
    long long parsed_generation = 0;
    unsigned long long parsed_hash = 0;
    int parsed_offset = -1;
    char canonical[SG_CURSOR_SZ];
    if (!token || sscanf(token, "sg1.%c.%lld.%16llx.%d", &parsed_kind, &parsed_generation,
                         &parsed_hash, &parsed_offset) != 4 ||
        parsed_kind != kind || parsed_offset < 0) {
        snprintf(err, err_sz,
                 "invalid_cursor: %s is not a %s cursor from this tool; re-run the query "
                 "without it",
                 cursor_key, kind == 'v' ? "semantic" : "structural");
        return err;
    }
    sg_cursor_format(canonical, sizeof(canonical), parsed_kind, parsed_generation, parsed_hash,
                     parsed_offset);
    if (strcmp(canonical, token) != 0) {
        snprintf(err, err_sz, "invalid_cursor: %s is malformed; re-run the query without it",
                 cursor_key);
        return err;
    }
    if (parsed_hash != hash) {
        snprintf(err, err_sz,
                 "invalid_cursor: %s was issued for different query arguments; re-run the "
                 "query without it",
                 cursor_key);
        return err;
    }
    if (parsed_generation != generation) {
        snprintf(err, err_sz,
                 "stale_cursor: the project was re-indexed since %s was issued (generation "
                 "%lld, now %lld); re-run the query without it",
                 cursor_key, parsed_generation, (long long)generation);
        return err;
    }
    *offset = parsed_offset;
    return NULL;
}

static char *handle_search_graph_flagged(const char *args, char *project) {
    char path[CBM_SZ_1K];
    cbm_zova_repository_t *repo = NULL;
    if (!project || !project[0] || cbm_zova_user_database_path(path, sizeof(path)) != 0 ||
        !(repo = cbm_zova_repository_open(path, project))) {
        return project_not_found_result(project);
    }
    const char *workspace_id = cbm_zova_repository_workspace_id(repo);
    /* Cursors bind to the generation read before the search; the search is
     * accepted only if the same generation is still published after it. */
    int64_t generation = 0;
    char *indexed_at = NULL;
    int status_rc = cbm_zova_repository_index_status(repo, workspace_id, &generation, &indexed_at);
    free(indexed_at);
    yyjson_doc *args_doc = yyjson_read(args, strlen(args), 0);
    yyjson_val *args_root = args_doc ? yyjson_doc_get_root(args_doc) : NULL;
    uint64_t query_hash = 0;
    if (status_rc != CBM_STORE_OK || !sg_query_hash(args_root, project, &query_hash)) {
        yyjson_doc_free(args_doc);
        cbm_zova_repository_close(repo);
        free(project);
        return cbm_mcp_text_result("could not bind search_graph request to a generation", true);
    }
    int offset = cbm_mcp_get_int_arg(args, "offset", 0);
    int semantic_offset = cbm_mcp_get_int_arg(args, "semantic_offset", 0);
    int semantic_limit = cbm_mcp_get_int_arg(args, "semantic_limit", 0);
    char paging_error[CBM_SZ_512];
    const char *paging_failure =
        sg_resume_offset(args_root, "cursor", "offset", 's', generation, query_hash, &offset,
                         paging_error, sizeof(paging_error));
    if (!paging_failure) {
        paging_failure = sg_resume_offset(args_root, "semantic_cursor", "semantic_offset", 'v',
                                          generation, query_hash, &semantic_offset,
                                          paging_error, sizeof(paging_error));
    }
    if (!paging_failure && (semantic_offset < 0 || semantic_offset > SG_SEMANTIC_MAX_OFFSET)) {
        paging_failure = "semantic_offset must be between 0 and 99998";
    }
    bool semantic_limit_given = yyjson_obj_get(args_root, "semantic_limit") != NULL;
    if (!paging_failure && semantic_limit_given &&
        (semantic_limit < 1 || semantic_limit > SG_SEMANTIC_MAX_LIMIT)) {
        paging_failure = "semantic_limit must be between 1 and 500";
    }
    yyjson_doc_free(args_doc);
    if (paging_failure) {
        cbm_zova_repository_close(repo);
        free(project);
        return cbm_mcp_text_result(paging_failure, true);
    }
    char *query = cbm_mcp_get_string_arg(args, "query");
    char *name_pattern = cbm_mcp_get_string_arg(args, "name_pattern");
    char *qn_pattern = cbm_mcp_get_string_arg(args, "qn_pattern");
    char *label = cbm_mcp_get_string_arg(args, "label");
    char *file_pattern = cbm_mcp_get_string_arg(args, "file_pattern");
    char *relationship = cbm_mcp_get_string_arg(args, "relationship");
    cbm_search_params_t params = {
        .project = project,
        .label = label,
        .name_pattern = query && query[0] ? query : name_pattern,
        .qn_pattern = qn_pattern,
        .file_pattern = file_pattern,
        .relationship = relationship,
        .exclude_entry_points = cbm_mcp_get_bool_arg(args, "exclude_entry_points"),
        .include_connected = false,
        .limit = cbm_mcp_get_int_arg(args, "limit",
                                     query && query[0] ? BM25_DEFAULT_LIMIT
                                                       : CBM_DEFAULT_SEARCH_LIMIT),
        .offset = offset,
        .min_degree = cbm_mcp_get_int_arg(args, "min_degree", CBM_NOT_FOUND),
        .max_degree = cbm_mcp_get_int_arg(args, "max_degree", CBM_NOT_FOUND),
    };
    cbm_search_output_t out = {0};
    /* Semantic-only calls skip the structural search: with no structural
     * filter, running the UNFILTERED regex search would prepend unrelated
     * enriched nodes to a purely semantic request — same gate as the
     * compatibility route. (Upstream 108740d8.) */
    bool has_structural_filters = !search_args_has_semantic_query(args) || name_pattern ||
                                  qn_pattern || label || file_pattern ||
                                  relationship ||
                                  params.min_degree != CBM_NOT_FOUND ||
                                  params.max_degree != CBM_NOT_FOUND;
    int rc = CBM_STORE_OK;
    char fts_query[BM25_QUERY_BUF];
    if (query && query[0]) {
        if (zova_fts_match(query, fts_query, sizeof(fts_query))) {
            rc = cbm_zova_repository_search_fts(repo, workspace_id, fts_query, file_pattern,
                                                params.limit, params.offset, &out);
        }
    } else if (has_structural_filters) {
        rc = cbm_zova_repository_search(repo, workspace_id, &params, &out);
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_doc **props_docs = NULL;
    int props_doc_count = 0;
    if (rc == CBM_STORE_OK) {
        if (query && query[0]) {
            emit_flagged_fts_results(doc, root, &out, offset);
        } else if (has_structural_filters) {
            emit_search_results(doc, root, &out, NULL, relationship, false, offset,
                                &props_docs, &props_doc_count);
        }
    }
    bool structural_ran = (query && query[0]) || has_structural_filters;
    if (rc == CBM_STORE_OK && structural_ran && out.count > 0 &&
        out.total > offset + out.count) {
        char cursor[SG_CURSOR_SZ];
        sg_cursor_format(cursor, sizeof(cursor), 's', generation, query_hash,
                         offset + out.count);
        yyjson_mut_obj_add_strcpy(doc, root, "next_cursor", cursor);
    }
    if (!semantic_limit_given) {
        semantic_limit = params.limit > 0 ? (params.limit < SG_SEMANTIC_MAX_LIMIT
                                                 ? params.limit
                                                 : SG_SEMANTIC_MAX_LIMIT)
                                          : CBM_SZ_16;
    }
    bool semantic_type_error = false;
    int semantic_next = -1;
    cbm_store_t *vector_store = cbm_store_open_zova_workspace_query(path, workspace_id);
    if (rc == CBM_STORE_OK && vector_store) {
        semantic_type_error = run_semantic_query(doc, root, args, vector_store, project,
                                                 semantic_offset, semantic_limit,
                                                 &semantic_next);
    }
    if (vector_store) cbm_store_close(vector_store);
    if (semantic_next > 0) {
        char cursor[SG_CURSOR_SZ];
        sg_cursor_format(cursor, sizeof(cursor), 'v', generation, query_hash, semantic_next);
        yyjson_mut_obj_add_strcpy(doc, root, "semantic_next_cursor", cursor);
    }
    int64_t generation_after = 0;
    char *indexed_after = NULL;
    /* A publish that landed mid-request may have mixed generations. */
    bool generation_changed =
        rc == CBM_STORE_OK &&
        (cbm_zova_repository_index_status(repo, workspace_id, &generation_after,
                                          &indexed_after) != CBM_STORE_OK ||
         generation_after != generation);
    free(indexed_after);
    if (semantic_type_error) {
        for (int i = 0; i < props_doc_count; i++) yyjson_doc_free(props_docs[i]);
        free(props_docs);
        yyjson_mut_doc_free(doc);
        cbm_store_search_free(&out);
        cbm_zova_repository_close(repo);
        free(query); free(name_pattern); free(qn_pattern); free(label); free(file_pattern);
        free(relationship); free(project);
        return cbm_mcp_text_result(
            "semantic_query must be an array of keyword strings, e.g. "
            "[\"send\",\"pubsub\",\"publish\"] — not a single string. Split your query "
            "into individual keywords; each is scored independently via per-keyword "
            "min-cosine.", true);
    }
    char *json = rc == CBM_STORE_OK ? yy_doc_to_str(doc) : NULL;
    for (int i = 0; i < props_doc_count; i++) yyjson_doc_free(props_docs[i]);
    free(props_docs);
    yyjson_mut_doc_free(doc);
    cbm_store_search_free(&out);
    cbm_zova_repository_close(repo);
    free(query); free(name_pattern); free(qn_pattern); free(label); free(file_pattern);
    free(relationship); free(project);
    if (generation_changed) {
        free(json);
        return cbm_mcp_text_result(
            "the project was re-indexed during search_graph; retry the request", true);
    }
    if (!json) return cbm_mcp_text_result("shared cbm.zova search failed", true);
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* Extract keyword strings from a yyjson array into `keywords`.  Returns the
 * number of strings copied (capped at `max_out`). */
static int extract_semantic_keywords(yyjson_val *sq_val, const char **keywords, int max_out) {
    int kw_count = (int)yyjson_arr_size(sq_val);
    if (kw_count > max_out) {
        kw_count = max_out;
    }
    size_t kw_idx = 0;
    size_t kw_max = 0;
    yyjson_val *kw_val;
    int ki = 0;
    yyjson_arr_foreach(sq_val, kw_idx, kw_max, kw_val) {
        if (ki < kw_count && yyjson_is_str(kw_val)) {
            keywords[ki++] = yyjson_get_str(kw_val);
        }
    }
    return ki;
}

/* Emit cbm_vector_result_t entries as a "semantic_results" array on the doc. */
static void emit_semantic_results(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                  cbm_vector_result_t *vresults, int vcount) {
    yyjson_mut_val *sem_results = yyjson_mut_arr(doc);
    for (int v = 0; v < vcount; v++) {
        yyjson_mut_val *vitem = yyjson_mut_obj(doc);
        if (vresults[v].project)
            yyjson_mut_obj_add_strcpy(doc, vitem, "project", vresults[v].project);
        yyjson_mut_obj_add_strcpy(doc, vitem, "name", vresults[v].name);
        yyjson_mut_obj_add_strcpy(doc, vitem, "qualified_name", vresults[v].qualified_name);
        yyjson_mut_obj_add_strcpy(doc, vitem, "label", vresults[v].label);
        yyjson_mut_obj_add_strcpy(doc, vitem, "file_path", vresults[v].file_path);
        double score = round(vresults[v].score * 1e12) / 1e12;
        yyjson_mut_obj_add_real(doc, vitem, "score", score);
        yyjson_mut_arr_add_val(sem_results, vitem);
    }
    yyjson_mut_obj_add_val(doc, root, "semantic_results", sem_results);
}

/* Append one page of semantic_query vector-search results onto the doc:
 * ranks [offset, offset+limit). One extra ranked hit is materialized so
 * semantic_has_more is exact; semantic_total is exact ("eq") only when the
 * search ran out of hits, otherwise a lower bound ("gte"). *next_offset is the
 * next rank when more remain, else -1. Returns true if semantic_query was
 * provided as a non-array (type error — caller should surface to the user). */
static bool run_semantic_query(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *args,
                               cbm_store_t *store, const char *project, int offset, int limit,
                               int *next_offset) {
    *next_offset = -1;
    enum { MAX_KW_SEARCH = 32 };
    yyjson_doc *args_doc = yyjson_read(args, strlen(args), 0);
    yyjson_val *args_root = args_doc ? yyjson_doc_get_root(args_doc) : NULL;
    yyjson_val *sq_val = args_root ? yyjson_obj_get(args_root, "semantic_query") : NULL;
    bool type_error = false;
    if (sq_val && !yyjson_is_arr(sq_val)) {
        type_error = true;
    } else if (sq_val && yyjson_arr_size(sq_val) > 0) {
        const char *keywords[MAX_KW_SEARCH];
        int ki = extract_semantic_keywords(sq_val, keywords, MAX_KW_SEARCH);
        cbm_vector_result_t *vresults = NULL;
        int vcount = 0;
        int materialize = offset + limit + 1;
        if (cbm_store_vector_search(store, project, keywords, ki, materialize, &vresults,
                                    &vcount) == CBM_STORE_OK) {
            int returned = vcount > offset ? vcount - offset : 0;
            returned = returned < limit ? returned : limit;
            bool has_more = vcount > offset + returned;
            yyjson_mut_obj_add_int(doc, root, "semantic_total", vcount);
            yyjson_mut_obj_add_str(doc, root, "semantic_total_relation",
                                   vcount < materialize ? "eq" : "gte");
            yyjson_mut_obj_add_int(doc, root, "semantic_returned", returned);
            yyjson_mut_obj_add_bool(doc, root, "semantic_has_more", has_more);
            emit_semantic_results(doc, root, returned > 0 ? vresults + offset : NULL, returned);
            if (has_more && returned > 0) {
                *next_offset = offset + returned;
            }
            cbm_store_free_vector_results(vresults, vcount);
        }
    }
    if (args_doc) {
        yyjson_doc_free(args_doc);
    }
    return type_error;
}

static void emit_catalog_search_results(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                        const cbm_search_output_t *out, int offset,
                                        bool bm25) {
    yyjson_mut_obj_add_int(doc, root, "total", out->total);
    if (bm25) yyjson_mut_obj_add_str(doc, root, "search_mode", "bm25");
    yyjson_mut_val *results = yyjson_mut_arr(doc);
    for (int i = 0; i < out->count; i++) {
        const cbm_search_result_t *result = &out->results[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "project",
                               result->node.project ? result->node.project : "");
        yyjson_mut_obj_add_str(doc, item, "name", result->node.name ? result->node.name : "");
        yyjson_mut_obj_add_str(doc, item, "qualified_name",
                               result->node.qualified_name ? result->node.qualified_name : "");
        yyjson_mut_obj_add_str(doc, item, "label", result->node.label ? result->node.label : "");
        yyjson_mut_obj_add_str(doc, item, "file_path",
                               result->node.file_path ? result->node.file_path : "");
        yyjson_mut_obj_add_int(doc, item, "start_line", result->node.start_line);
        yyjson_mut_obj_add_int(doc, item, "end_line", result->node.end_line);
        if (bm25) {
            yyjson_mut_obj_add_real(doc, item, "rank", result->rank);
        } else {
            yyjson_mut_obj_add_int(doc, item, "in_degree", result->in_degree);
            yyjson_mut_obj_add_int(doc, item, "out_degree", result->out_degree);
        }
        yyjson_mut_arr_add_val(results, item);
    }
    yyjson_mut_obj_add_val(doc, root, "results", results);
    yyjson_mut_obj_add_bool(doc, root, "has_more", out->total > offset + out->count);
}

static void emit_catalog_scope(yyjson_mut_doc *doc, yyjson_mut_val *root,
                               const cbm_zova_catalog_scope_t *scope) {
    yyjson_mut_val *scope_json = yyjson_mut_obj(doc);
    yyjson_mut_val *projects = yyjson_mut_arr(doc);
    yyjson_mut_val *generations = yyjson_mut_obj(doc);
    for (int i = 0; i < scope->count; i++) {
        yyjson_mut_arr_add_strcpy(doc, projects, scope->projects[i].selector);
        yyjson_mut_obj_add_int(doc, generations, scope->projects[i].selector,
                               scope->projects[i].generation);
    }
    yyjson_mut_obj_add_val(doc, scope_json, "projects", projects);
    yyjson_mut_obj_add_val(doc, scope_json, "generations", generations);
    yyjson_mut_val *excluded = yyjson_mut_arr(doc);
    for (int i = 0; i < scope->excluded_count; i++) {
        const cbm_zova_catalog_project_t *project = &scope->excluded_projects[i];
        yyjson_mut_val *entry = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, entry, "project", project->selector);
        yyjson_mut_obj_add_str(doc, entry, "reason",
                               !project->ready ? "no ready generation"
                                               : (project->health_reason &&
                                                          project->health_reason[0]
                                                      ? project->health_reason
                                                      : "workspace unhealthy"));
        yyjson_mut_arr_add_val(excluded, entry);
    }
    yyjson_mut_obj_add_val(doc, scope_json, "excluded_projects", excluded);
    yyjson_mut_obj_add_val(doc, root, "scope", scope_json);
}

static char *handle_search_graph_catalog(const char *args,
                                         const cbm_mcp_project_scope_t *requested) {
    if (cbm_mcp_get_bool_arg(args, "include_connected")) {
        return cbm_mcp_text_result(
            "include_connected is not supported for multi-project search", true);
    }
    yyjson_doc *args_doc = yyjson_read(args, strlen(args), 0);
    yyjson_val *args_root = args_doc ? yyjson_doc_get_root(args_doc) : NULL;
    yyjson_val *semantic = args_root ? yyjson_obj_get(args_root, "semantic_query") : NULL;
    if (semantic && (!yyjson_is_arr(semantic) || yyjson_arr_size(semantic) == 0)) {
        yyjson_doc_free(args_doc);
        return cbm_mcp_text_result(
            "semantic_query must be a non-empty array of keyword strings", true);
    }
    if (semantic) {
        size_t index = 0;
        size_t maximum = 0;
        yyjson_val *keyword = NULL;
        yyjson_arr_foreach(semantic, index, maximum, keyword) {
            if (!yyjson_is_str(keyword) || !yyjson_get_str(keyword)[0]) {
                yyjson_doc_free(args_doc);
                return cbm_mcp_text_result(
                    "semantic_query entries must be non-empty strings", true);
            }
        }
    }

    char path[CBM_SZ_1K];
    if (cbm_zova_user_database_path(path, sizeof(path)) != 0) {
        yyjson_doc_free(args_doc);
        return cbm_mcp_text_result("cannot resolve shared Zova database", true);
    }
    cbm_zova_catalog_t *catalog = cbm_zova_catalog_open(path);
    if (!catalog) {
        yyjson_doc_free(args_doc);
        return cbm_mcp_text_result("cannot open shared Zova catalog", true);
    }
    cbm_zova_catalog_scope_t scope = {0};
    int resolve_status = cbm_zova_catalog_resolve(
        catalog, (const char *const *)requested->projects, (int)requested->project_count,
        requested->wildcard, &scope);
    if (resolve_status != CBM_STORE_OK || scope.count == 0) {
        const char *catalog_error = cbm_zova_catalog_error(catalog);
        char *error_copy = catalog_error[0] ? strdup(catalog_error) : NULL;
        cbm_zova_catalog_scope_free(&scope);
        cbm_zova_catalog_close(catalog);
        yyjson_doc_free(args_doc);
        char *result = cbm_mcp_text_result(
            error_copy ? error_copy
                       : (resolve_status == CBM_STORE_NOT_FOUND
                              ? "one or more requested projects were not found"
                              : "no ready healthy projects matched the scope"),
            true);
        free(error_copy);
        return result;
    }

    char *query = cbm_mcp_get_string_arg(args, "query");
    char *label = cbm_mcp_get_string_arg(args, "label");
    char *name_pattern = cbm_mcp_get_string_arg(args, "name_pattern");
    char *qn_pattern = cbm_mcp_get_string_arg(args, "qn_pattern");
    char *file_pattern = cbm_mcp_get_string_arg(args, "file_pattern");
    char *relationship = cbm_mcp_get_string_arg(args, "relationship");
    int offset = cbm_mcp_get_int_arg(args, "offset", 0);
    int limit = cbm_mcp_get_int_arg(args, "limit",
                                    query && query[0] ? BM25_DEFAULT_LIMIT
                                                      : CBM_DEFAULT_SEARCH_LIMIT);
    cbm_search_output_t output = {0};
    cbm_vector_result_t *semantic_results = NULL;
    int semantic_count = 0;
    int semantic_total = 0;
    int search_status = CBM_STORE_ERR;
    if (semantic) {
        enum { MAX_CATALOG_KEYWORDS = 32 };
        const char *keywords[MAX_CATALOG_KEYWORDS];
        int keyword_count = extract_semantic_keywords(
            semantic, keywords, MAX_CATALOG_KEYWORDS);
        search_status = keyword_count > 0
                            ? cbm_zova_catalog_search_semantic(
                                  catalog, path, &scope, keywords, keyword_count,
                                  limit, offset, &semantic_results, &semantic_count,
                                  &semantic_total)
                            : CBM_STORE_ERR;
    } else if (!relationship || validate_edge_type(relationship)) {
        char fts_query[BM25_QUERY_BUF];
        if (query && query[0]) {
            search_status = zova_fts_match(query, fts_query, sizeof(fts_query))
                                ? cbm_zova_catalog_search_fts(catalog, &scope, fts_query,
                                                              file_pattern, limit, offset, &output)
                                : CBM_STORE_OK;
        } else {
            cbm_search_params_t params = {
                .label = label,
                .name_pattern = name_pattern,
                .qn_pattern = qn_pattern,
                .file_pattern = file_pattern,
                .relationship = relationship,
                .exclude_entry_points = cbm_mcp_get_bool_arg(args, "exclude_entry_points"),
                .limit = limit,
                .offset = offset,
                .min_degree = cbm_mcp_get_int_arg(args, "min_degree", CBM_NOT_FOUND),
                .max_degree = cbm_mcp_get_int_arg(args, "max_degree", CBM_NOT_FOUND),
            };
            search_status = cbm_zova_catalog_search(catalog, &scope, &params, &output);
        }
    }

    char *json = NULL;
    if (search_status == CBM_STORE_OK) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        if (semantic) {
            emit_semantic_results(doc, root, semantic_results, semantic_count);
            yyjson_mut_obj_add_int(doc, root, "total", semantic_total);
            yyjson_mut_obj_add_bool(doc, root, "has_more",
                                    semantic_total > offset + semantic_count);
        } else {
            emit_catalog_search_results(doc, root, &output, offset, query && query[0]);
        }
        emit_catalog_scope(doc, root, &scope);
        json = yy_doc_to_str(doc);
        yyjson_mut_doc_free(doc);
    }
    cbm_store_search_free(&output);
    cbm_store_free_vector_results(semantic_results, semantic_count);
    free(query);
    free(label);
    free(name_pattern);
    free(qn_pattern);
    free(file_pattern);
    free(relationship);
    cbm_zova_catalog_scope_free(&scope);
    cbm_zova_catalog_close(catalog);
    yyjson_doc_free(args_doc);
    if (!json) {
        if (search_status == CBM_ZOVA_CATALOG_INCOMPATIBLE_MODELS)
            return cbm_mcp_text_result(
                "selected projects use incompatible semantic models or dimensions", true);
        if (search_status == CBM_ZOVA_CATALOG_STALE_SCOPE)
            return cbm_mcp_text_result(
                "project generations changed during semantic search; retry the request", true);
        return cbm_mcp_text_result("multi-project search failed", true);
    }
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_search_graph(cbm_mcp_server_t *srv, const char *args) {
    cbm_mcp_project_scope_t scope = {0};
    const char *scope_error = NULL;
    if (project_scope_parse(args, &scope, &scope_error) != 0) {
        project_scope_free(&scope);
        return cbm_mcp_text_result(scope_error ? scope_error : "invalid project scope", true);
    }
    if (scope.project_count > 0) {
        static const char *const single_project_args[] = {"cursor", "semantic_cursor",
                                                          "semantic_offset", "semantic_limit"};
        yyjson_doc *args_doc = yyjson_read(args, strlen(args), 0);
        yyjson_val *args_root = args_doc ? yyjson_doc_get_root(args_doc) : NULL;
        const char *rejected = NULL;
        for (size_t i = 0; i < sizeof(single_project_args) / sizeof(single_project_args[0]); i++) {
            if (!rejected && yyjson_obj_get(args_root, single_project_args[i])) {
                rejected = single_project_args[i];
            }
        }
        if (rejected) {
            char message[CBM_SZ_128];
            snprintf(message, sizeof(message), "%s is supported for single-project search only",
                     rejected);
            yyjson_doc_free(args_doc);
            project_scope_free(&scope);
            return cbm_mcp_text_result(message, true);
        }
        yyjson_doc_free(args_doc);
        char *result = handle_search_graph_catalog(args, &scope);
        project_scope_free(&scope);
        return result;
    }
    char *project = scope.project;
    scope.project = NULL;
    project_scope_free(&scope);
    return handle_search_graph_flagged(args, project);
}

static char *handle_query_graph(cbm_mcp_server_t *srv, const char *args) {
    char *query = cbm_mcp_get_string_arg(args, "query");
    char *project = get_project_arg(args);
    char path[CBM_SZ_1K];
    cbm_zova_repository_t *repo = NULL;
    if (!query) {
        free(project);
        return cbm_mcp_text_result("query is required", true);
    }
    if (!project || cbm_zova_user_database_path(path, sizeof(path)) != 0 ||
        !(repo = cbm_zova_repository_open(path, project))) {
        free(query);
        return project_not_found_result(project);
    }
    if (cbm_zova_workspace_id_validate(cbm_zova_repository_workspace_id(repo)) != 0) {
        cbm_zova_repository_close(repo);
        free(query); free(project);
        return cbm_mcp_text_result("workspace-scoped Zova Cypher request is invalid", true);
    }
    const char *workspace_id = cbm_zova_repository_workspace_id(repo);
    char workspace_copy[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_copy, sizeof(workspace_copy), "%s", workspace_id);
    int64_t generation = 0;
    char *indexed_at = NULL;
    if (cbm_zova_repository_index_status(repo, workspace_id, &generation,
                                         &indexed_at) != CBM_STORE_OK) {
        cbm_zova_repository_close(repo);
        free(query); free(project); free(indexed_at);
        return cbm_mcp_text_result("could not bind Cypher request generation", true);
    }
    free(indexed_at);
    cbm_query_t *plan = NULL;
    char *parse_error = NULL;
    if (cbm_cypher_parse(query, &plan, &parse_error) != 0) {
        char diagnostic[CBM_SZ_1K];
        snprintf(diagnostic, sizeof(diagnostic),
                 "route=unsupported workspace=%s operator=parser reason=%s",
                 workspace_copy, parse_error ? parse_error : "Cypher parse failed");
        cbm_zova_repository_close(repo);
        free(query); free(project);
        char *response = cbm_mcp_text_result(diagnostic, true);
        free(parse_error);
        return response;
    }
    const char *route_reason = NULL;
    cbm_cypher_route_t route = cbm_cypher_classify_plan(plan, &route_reason);
    cbm_query_free(plan);
    if (route == CBM_CYPHER_ROUTE_UNSUPPORTED) {
        char diagnostic[CBM_SZ_1K];
        snprintf(diagnostic, sizeof(diagnostic),
                 "route=unsupported workspace=%s operator=logical_plan reason=%s",
                 workspace_copy, route_reason ? route_reason : "unsupported query shape");
        cbm_zova_repository_close(repo);
        free(query); free(project); free(parse_error);
        return cbm_mcp_text_result(diagnostic, true);
    }
    free(parse_error);
    cbm_store_t *zova_store = cbm_store_open_zova_workspace_query(path, workspace_id);
    if (!zova_store) {
        cbm_zova_repository_close(repo);
        free(query); free(project);
        return cbm_mcp_text_result("could not open workspace-scoped Cypher session", true);
    }
    cbm_zova_repository_close(repo);
    int max_rows = cbm_mcp_get_int_arg(args, "max_rows", 1000);
    cbm_cypher_result_t result = {0};
    int rc = cbm_cypher_execute(zova_store, query, project, max_rows, &result);
    cbm_store_close(zova_store);
    free(query); free(project);
    if (rc != 0) {
        char *error = cbm_mcp_text_result(
            result.error ? result.error : "Cypher execution failed", true);
        cbm_cypher_result_free(&result);
        return error;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *cols = yyjson_mut_arr(doc);
    for (int i = 0; i < result.col_count; ++i) yyjson_mut_arr_add_str(doc, cols, result.columns[i]);
    yyjson_mut_obj_add_val(doc, root, "columns", cols);
    yyjson_mut_val *rows = yyjson_mut_arr(doc);
    for (int r = 0; r < result.row_count; ++r) {
        yyjson_mut_val *row = yyjson_mut_arr(doc);
        for (int c = 0; c < result.col_count; ++c)
            yyjson_mut_arr_add_str(doc, row, result.rows[r][c] ? result.rows[r][c] : "");
        yyjson_mut_arr_add_val(rows, row);
    }
    yyjson_mut_obj_add_val(doc, root, "rows", rows);
    yyjson_mut_obj_add_int(doc, root, "total", result.row_count);
    if (result.warning) {
        yyjson_mut_obj_add_str(doc, root, "warning", result.warning);
    }
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_cypher_result_free(&result);
    char *response = cbm_mcp_text_result(json, false);
    free(json);
    return response;
}

/* ── Index coverage ───────────────────────────────────────────────
 * Coverage is stamped on File node properties by the pipeline and published
 * with the generation (see CBM_COVERAGE_* in pipeline.h), so every read here
 * is scoped to one workspace and its active generation. */

bool cbm_path_within_root(const char *root_path, const char *abs_path); /* defined below */

enum {
    COVERAGE_PATH_MAX = 128,
    COVERAGE_SCOPE_MAX = 32,
    COVERAGE_SCOPE_DEFAULT_LIMIT = 200,
    COVERAGE_SCOPE_MAX_LIMIT = 1000,
    COVERAGE_RANGE_MAX = 256, /* matches CBM_MAX_ERROR_REGIONS */
    COVERAGE_STATUS_EXAMPLES = 5,
};

typedef enum {
    COVERAGE_PATH_OK = 0,
    COVERAGE_PATH_OUTSIDE,
    COVERAGE_PATH_INVALID,
} coverage_path_result_t;

/* What one File row says about extraction. */
typedef struct {
    const char *status; /* no_recorded_issue | partial | unusable | skipped */
    char *ranges;       /* partial/unusable: "12-40,88-90", ",+N" when clipped */
    char *phase;        /* skipped */
    char *reason;       /* skipped */
} coverage_file_t;

static void coverage_file_parse(const cbm_zova_file_coverage_t *row, coverage_file_t *out) {
    memset(out, 0, sizeof(*out));
    out->status = "no_recorded_issue";
    if (!row->properties) {
        /* Tracked by a file hash but without a File node: nothing extracted. */
        out->status = "skipped";
        out->phase = cbm_heap_strdup("structure");
        out->reason = cbm_heap_strdup("tracked file has no File node");
        return;
    }
    yyjson_doc *doc = yyjson_read(row->properties, strlen(row->properties), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *coverage = yyjson_get_str(yyjson_obj_get(root, "coverage"));
    if (coverage && strcmp(coverage, CBM_COVERAGE_PARTIAL) == 0) {
        const char *ranges = yyjson_get_str(yyjson_obj_get(root, "parse_error_ranges"));
        /* One range over most of the file: the ranges are no help, so the
         * file is reported as unusable rather than partial (upstream
         * a9535092). It was still indexed. */
        out->status = yyjson_get_bool(yyjson_obj_get(root, "parse_unusable")) ? "unusable"
                                                                               : "partial";
        out->ranges = cbm_heap_strdup(ranges ? ranges : "");
    } else if (coverage && strcmp(coverage, CBM_COVERAGE_MISSED) == 0) {
        const char *phase = yyjson_get_str(yyjson_obj_get(root, "coverage_phase"));
        const char *reason = yyjson_get_str(yyjson_obj_get(root, "coverage_reason"));
        out->status = "skipped";
        out->phase = cbm_heap_strdup(phase ? phase : "");
        out->reason = cbm_heap_strdup(reason ? reason : "");
    }
    yyjson_doc_free(doc);
}

static void coverage_file_clear(coverage_file_t *c) {
    free(c->ranges);
    free(c->phase);
    free(c->reason);
    memset(c, 0, sizeof(*c));
}

/* Normalize an untrusted repository-relative path without touching the
 * filesystem. Absolute paths, drive/UNC paths and any `..` component are
 * outside the project; control bytes are invalid. A root scope (`.`)
 * normalizes to the empty prefix. */
static coverage_path_result_t coverage_normalize_rel(const char *input, bool allow_root, char *out,
                                                     size_t out_size) {
    if (!input || !out || out_size == 0U) {
        return COVERAGE_PATH_INVALID;
    }
    out[0] = '\0';
    size_t len = strlen(input);
    if (len == 0U && allow_root) {
        return COVERAGE_PATH_OK;
    }
    if (len == 0U || len >= out_size) {
        return COVERAGE_PATH_INVALID;
    }
    if (input[0] == '/' || input[0] == '\\' ||
        (len >= 2U && isalpha((unsigned char)input[0]) && input[1] == ':')) {
        return COVERAGE_PATH_OUTSIDE;
    }
    size_t in = 0U;
    size_t written = 0U;
    while (in < len) {
        while (in < len && (input[in] == '/' || input[in] == '\\')) {
            in++;
        }
        if (in >= len) {
            break;
        }
        size_t start = in;
        while (in < len && input[in] != '/' && input[in] != '\\') {
            if ((unsigned char)input[in] < 0x20U) {
                return COVERAGE_PATH_INVALID;
            }
            in++;
        }
        size_t part_len = in - start;
        if (part_len == 1U && input[start] == '.') {
            continue;
        }
        if (part_len == 2U && input[start] == '.' && input[start + 1U] == '.') {
            return COVERAGE_PATH_OUTSIDE;
        }
        if (written > 0U) {
            out[written++] = '/';
        }
        memcpy(out + written, input + start, part_len);
        written += part_len;
    }
    out[written] = '\0';
    return written > 0U || allow_root ? COVERAGE_PATH_OK : COVERAGE_PATH_INVALID;
}

/* Same clock the pipeline records in cbm_file_hashes_v1.mtime_ns. */
static int64_t coverage_stat_mtime_ns(const struct stat *st) {
#ifdef __APPLE__
    return ((int64_t)st->st_mtimespec.tv_sec * (int64_t)CBM_NSEC_PER_SEC) +
           (int64_t)st->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (int64_t)st->st_mtime * (int64_t)CBM_NSEC_PER_SEC;
#else
    return ((int64_t)st->st_mtim.tv_sec * (int64_t)CBM_NSEC_PER_SEC) + (int64_t)st->st_mtim.tv_nsec;
#endif
}

/* stat root/rel. Returns 0 when it exists, -1 when it does not, -2 when it
 * resolves outside the root (symlink escape). */
static int coverage_stat(const char *root_path, const char *rel, struct stat *st) {
    char abs_path[CBM_SZ_4K];
    int n = snprintf(abs_path, sizeof(abs_path), "%s/%s", root_path, rel);
    if (n < 0 || (size_t)n >= sizeof(abs_path) || stat(abs_path, st) != 0) {
        return -1;
    }
    return cbm_path_within_root(root_path, abs_path) ? 0 : -2;
}

/* Filesystem metadata against the hash row published with the generation. */
static const char *coverage_freshness(const char *root_path, const cbm_zova_file_coverage_t *row) {
    if (!root_path || !root_path[0]) {
        return "unavailable";
    }
    struct stat st;
    int rc = coverage_stat(root_path, row->file_path, &st);
    if (rc == -1) {
        return "missing";
    }
    if (rc != 0) {
        return "unavailable";
    }
    if (!row->has_hash) {
        return "not_tracked";
    }
    return row->mtime_ns == coverage_stat_mtime_ns(&st) && row->size_bytes == (int64_t)st.st_size
               ? "metadata_match"
               : "metadata_changed";
}

/* Discovery rules that provably exclude rel: built-in skip directories and
 * suffixes, and the root .gitignore / .cbmignore. NULL when none applies;
 * nested ignore files and global excludes are not consulted. */
static const char *coverage_exclusion(const char *rel, const cbm_gitignore_t *gitignore,
                                      const cbm_gitignore_t *cbmignore) {
    char prefix[CBM_SZ_4K];
    snprintf(prefix, sizeof(prefix), "%s", rel);
    for (char *slash = strchr(prefix, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        const char *base = strrchr(prefix, '/');
        base = base ? base + 1 : prefix;
        if (cbm_should_skip_dir(base, CBM_MODE_FULL)) {
            return "skipped_directory";
        }
        if (gitignore && cbm_gitignore_matches(gitignore, prefix, true)) {
            return "gitignore";
        }
        if (cbmignore && cbm_gitignore_matches(cbmignore, prefix, true)) {
            return "cbmignore";
        }
        *slash = '/';
    }
    const char *base = strrchr(rel, '/');
    if (cbm_has_ignored_suffix(base ? base + 1 : rel, CBM_MODE_FULL)) {
        return "ignored_suffix";
    }
    if (gitignore && cbm_gitignore_matches(gitignore, rel, false)) {
        return "gitignore";
    }
    if (cbmignore && cbm_gitignore_matches(cbmignore, rel, false)) {
        return "cbmignore";
    }
    return NULL;
}

/* Ranges as {start,end} objects. The extractor ends a clipped list with
 * ",+N" (N ranges dropped by its cap); that, or this reader's own cap, sets
 * "truncated" so a clipped list never reads as a complete one. */
static void coverage_add_ranges(yyjson_mut_doc *doc, yyjson_mut_val *item, const char *ranges) {
    if (!ranges || !ranges[0]) {
        return;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    const char *p = ranges;
    int emitted = 0;
    long dropped = 0;
    bool truncated = false;
    while (*p) {
        if (*p == '+') {
            char *end = NULL;
            dropped = strtol(p + 1, &end, 10);
            truncated = end != p + 1 && dropped > 0;
            break;
        }
        if (emitted >= COVERAGE_RANGE_MAX) {
            truncated = true;
            break;
        }
        char *end = NULL;
        long start = strtol(p, &end, 10);
        if (end == p || start <= 0) {
            break;
        }
        long stop = start;
        p = end;
        if (*p == '-') {
            long parsed = strtol(p + 1, &end, 10);
            if (end == p + 1 || parsed < start) {
                break;
            }
            stop = parsed;
            p = end;
        }
        yyjson_mut_val *range = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, range, "start", start);
        yyjson_mut_obj_add_int(doc, range, "end", stop);
        yyjson_mut_arr_add_val(arr, range);
        emitted++;
        if (*p != ',') {
            break;
        }
        p++;
    }
    yyjson_mut_obj_add_val(doc, item, "parse_error_ranges", arr);
    if (truncated) {
        yyjson_mut_obj_add_bool(doc, item, "parse_error_ranges_truncated", true);
        if (dropped > 0) {
            yyjson_mut_obj_add_int(doc, item, "parse_error_ranges_dropped", dropped);
        }
    }
}

static void coverage_add_detail(yyjson_mut_doc *doc, yyjson_mut_val *item,
                                const coverage_file_t *c) {
    if (c->ranges) {
        coverage_add_ranges(doc, item, c->ranges);
    }
    if (c->phase) {
        yyjson_mut_obj_add_strcpy(doc, item, "phase", c->phase);
        yyjson_mut_obj_add_strcpy(doc, item, "reason", c->reason ? c->reason : "");
    }
}

static const char *coverage_recommended_action(const char *status, const char *freshness) {
    if (strcmp(status, "excluded") == 0) {
        return "read_source_or_change_ignore_rules";
    }
    if (strcmp(status, "not_indexed") == 0 || strcmp(status, "coverage_unavailable") == 0) {
        return "read_source_and_reindex";
    }
    if (!freshness || strcmp(freshness, "metadata_match") != 0) {
        return "read_source_and_reindex";
    }
    if (strcmp(status, "partial") == 0) {
        return "read_ranges_and_verify_scope";
    }
    if (strcmp(status, "skipped") == 0 || strcmp(status, "unusable") == 0) {
        return "read_source_directly";
    }
    return "use_graph_with_best_effort_caveat";
}

static bool coverage_in_scope(const char *path, const char *scope) {
    size_t n = strlen(scope);
    return n == 0 || (strncmp(path, scope, n) == 0 && (path[n] == '\0' || path[n] == '/'));
}

/* One exact path. rows/count is the File row for rel (0 or 1 rows). */
static void coverage_add_path_json(yyjson_mut_doc *doc, yyjson_mut_val *item, const char *rel,
                                   const cbm_zova_file_coverage_t *rows, int count,
                                   const char *root_path, bool recorded,
                                   const cbm_gitignore_t *gitignore,
                                   const cbm_gitignore_t *cbmignore) {
    yyjson_mut_obj_add_strcpy(doc, item, "path", rel);
    if (count == 0) {
        struct stat st;
        int rc = root_path ? coverage_stat(root_path, rel, &st) : -1;
        const char *excluded = rc == -2 ? NULL : coverage_exclusion(rel, gitignore, cbmignore);
        const char *status = rc == -2 ? "outside_project"
                             : excluded ? "excluded"
                             : rc == 0  ? "not_indexed"
                                        : "missing";
        yyjson_mut_obj_add_str(doc, item, "status", status);
        if (excluded) {
            yyjson_mut_obj_add_str(doc, item, "reason", excluded);
        }
        yyjson_mut_obj_add_str(doc, item, "freshness", rc == 0 ? "not_tracked" : "missing");
        yyjson_mut_obj_add_str(doc, item, "recommended_action",
                               rc == 0 || excluded ? coverage_recommended_action(status, NULL)
                                                   : "verify_path");
        return;
    }
    coverage_file_t c;
    coverage_file_parse(&rows[0], &c);
    const char *status =
        strcmp(c.status, "no_recorded_issue") == 0 && !recorded ? "coverage_unavailable"
                                                                : c.status;
    const char *freshness = coverage_freshness(root_path, &rows[0]);
    yyjson_mut_obj_add_str(doc, item, "status", status);
    yyjson_mut_obj_add_str(doc, item, "freshness", freshness);
    yyjson_mut_obj_add_str(doc, item, "recommended_action",
                           coverage_recommended_action(status, freshness));
    coverage_add_detail(doc, item, &c);
    coverage_file_clear(&c);
}

/* Scope counts and the paginated gap list. rows is the workspace's full
 * sorted File list. */
static void coverage_add_scope_json(yyjson_mut_doc *doc, yyjson_mut_val *item, const char *scope,
                                    const cbm_zova_file_coverage_t *rows, int count, bool recorded,
                                    int offset, int limit) {
    yyjson_mut_obj_add_strcpy(doc, item, "scope", scope[0] ? scope : ".");
    int files = 0;
    int partial = 0;
    int unusable = 0;
    int skipped = 0;
    int gaps = 0;
    yyjson_mut_val *entries = yyjson_mut_arr(doc);
    for (int i = 0; i < count; i++) {
        if (!coverage_in_scope(rows[i].file_path, scope)) {
            continue;
        }
        files++;
        coverage_file_t c;
        coverage_file_parse(&rows[i], &c);
        bool gap = strcmp(c.status, "no_recorded_issue") != 0;
        if (gap) {
            if (strcmp(c.status, "partial") == 0) {
                partial++;
            } else if (strcmp(c.status, "unusable") == 0) {
                unusable++;
            } else {
                skipped++;
            }
            if (gaps >= offset && gaps < offset + limit) {
                yyjson_mut_val *entry = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_strcpy(doc, entry, "path", rows[i].file_path);
                yyjson_mut_obj_add_str(doc, entry, "status", c.status);
                coverage_add_detail(doc, entry, &c);
                yyjson_mut_arr_add_val(entries, entry);
            }
            gaps++;
        }
        coverage_file_clear(&c);
    }
    const char *status = gaps > 0     ? "known_gaps"
                         : !recorded  ? "coverage_unavailable"
                         : files == 0 ? "no_indexed_files"
                                      : "no_recorded_issue";
    yyjson_mut_obj_add_str(doc, item, "status", status);
    yyjson_mut_obj_add_int(doc, item, "files", files);
    yyjson_mut_obj_add_int(doc, item, "no_recorded_issue", files - partial - unusable - skipped);
    yyjson_mut_obj_add_int(doc, item, "partial", partial);
    yyjson_mut_obj_add_int(doc, item, "unusable", unusable);
    yyjson_mut_obj_add_int(doc, item, "skipped", skipped);
    yyjson_mut_obj_add_int(doc, item, "total_gaps", gaps);
    int end = offset + limit < gaps ? offset + limit : gaps;
    yyjson_mut_obj_add_bool(doc, item, "has_more", end < gaps);
    if (end < gaps) {
        yyjson_mut_obj_add_int(doc, item, "next_offset", end);
    }
    yyjson_mut_obj_add_val(doc, item, "gaps", entries);
}

static char *handle_check_index_coverage(cbm_mcp_server_t *srv, const char *args) {
    (void)srv;
    char *project = get_project_arg(args);
    char db_path[CBM_SZ_1K];
    cbm_zova_repository_t *repo = NULL;
    if (!project || cbm_zova_user_database_path(db_path, sizeof(db_path)) != 0 ||
        !(repo = cbm_zova_repository_open(db_path, project))) {
        return project_not_found_result(project);
    }

    yyjson_doc *adoc = yyjson_read(args, strlen(args), 0);
    yyjson_val *aroot = adoc ? yyjson_doc_get_root(adoc) : NULL;
    yyjson_val *paths = yyjson_obj_get(aroot, "paths");
    yyjson_val *scopes = yyjson_obj_get(aroot, "scopes");
    size_t path_count = yyjson_is_arr(paths) ? yyjson_arr_size(paths) : 0U;
    size_t scope_count = yyjson_is_arr(scopes) ? yyjson_arr_size(scopes) : 0U;
    if (!aroot || (paths && !yyjson_is_arr(paths)) || (scopes && !yyjson_is_arr(scopes)) ||
        path_count > COVERAGE_PATH_MAX || scope_count > COVERAGE_SCOPE_MAX) {
        yyjson_doc_free(adoc);
        cbm_zova_repository_close(repo);
        free(project);
        return cbm_mcp_text_result(
            "paths and scopes must be arrays (max 128 paths and 32 scopes)", true);
    }
    /* Neither given: summarize the whole project, as scopes ["."] would. */
    bool project_summary = path_count == 0U && scope_count == 0U;
    if (project_summary) {
        scope_count = 1U;
    }
    int scope_limit = cbm_mcp_get_int_arg(args, "scope_limit", COVERAGE_SCOPE_DEFAULT_LIMIT);
    int scope_offset = cbm_mcp_get_int_arg(args, "scope_offset", 0);
    scope_limit = scope_limit < 1                          ? 1
                  : scope_limit > COVERAGE_SCOPE_MAX_LIMIT ? COVERAGE_SCOPE_MAX_LIMIT
                                                           : scope_limit;
    scope_offset = scope_offset < 0 ? 0 : scope_offset;

    const char *workspace_id = cbm_zova_repository_workspace_id(repo);
    char *root_path = NULL;
    int64_t generation = 0;
    bool recorded = false;
    cbm_zova_file_coverage_t *all = NULL;
    int all_count = 0;
    /* Scopes need the full list; exact paths read one row each. The first
     * read also fetches the root, generation and marker. */
    int rc = cbm_zova_repository_file_coverage(repo, workspace_id, scope_count ? NULL : "",
                                               &all, &all_count, &root_path, &generation,
                                               &recorded);

    cbm_gitignore_t *gitignore = NULL;
    cbm_gitignore_t *cbmignore = NULL;
    if (rc == CBM_STORE_OK && root_path && path_count > 0U) {
        char ignore_path[CBM_SZ_4K];
        snprintf(ignore_path, sizeof(ignore_path), "%s/.gitignore", root_path);
        gitignore = cbm_gitignore_load(ignore_path);
        snprintf(ignore_path, sizeof(ignore_path), "%s/.cbmignore", root_path);
        cbmignore = cbm_gitignore_load(ignore_path);
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "signal", "best_effort");
    yyjson_mut_obj_add_sint(doc, root, "generation", generation);
    yyjson_mut_obj_add_bool(doc, root, "coverage_recorded", recorded);

    yyjson_mut_val *path_results = yyjson_mut_arr(doc);
    size_t idx;
    size_t max;
    yyjson_val *value;
    yyjson_arr_foreach(paths, idx, max, value) {
        if (rc != CBM_STORE_OK) {
            break;
        }
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        const char *input = yyjson_get_str(value);
        yyjson_mut_obj_add_strcpy(doc, item, "requested_path", input ? input : "");
        char rel[CBM_SZ_4K];
        coverage_path_result_t normalized = coverage_normalize_rel(input, false, rel, sizeof(rel));
        if (normalized != COVERAGE_PATH_OK) {
            yyjson_mut_obj_add_str(doc, item, "status",
                                   normalized == COVERAGE_PATH_OUTSIDE ? "outside_project"
                                                                       : "invalid_path");
            yyjson_mut_obj_add_str(doc, item, "freshness", "unavailable");
            yyjson_mut_obj_add_str(doc, item, "recommended_action", "use_project_relative_path");
            yyjson_mut_arr_add_val(path_results, item);
            continue;
        }
        cbm_zova_file_coverage_t *one = NULL;
        int one_count = 0;
        if (scope_count) {
            /* The full list is sorted by path. */
            int lo = 0;
            int hi = all_count;
            while (lo < hi) {
                int mid = lo + ((hi - lo) / 2);
                if (strcmp(all[mid].file_path, rel) < 0) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            if (lo < all_count && strcmp(all[lo].file_path, rel) == 0) {
                coverage_add_path_json(doc, item, rel, &all[lo], 1, root_path, recorded,
                                       gitignore, cbmignore);
            } else {
                coverage_add_path_json(doc, item, rel, NULL, 0, root_path, recorded, gitignore,
                                       cbmignore);
            }
        } else if ((rc = cbm_zova_repository_file_coverage(repo, workspace_id, rel, &one,
                                                           &one_count, NULL, NULL, NULL)) ==
                   CBM_STORE_OK) {
            coverage_add_path_json(doc, item, rel, one, one_count, root_path, recorded,
                                   gitignore, cbmignore);
            cbm_zova_file_coverage_free(one, one_count);
        }
        yyjson_mut_arr_add_val(path_results, item);
    }
    yyjson_mut_obj_add_val(doc, root, "paths", path_results);

    yyjson_mut_val *scope_results = yyjson_mut_arr(doc);
    yyjson_arr_foreach(scopes, idx, max, value) {
        if (rc != CBM_STORE_OK) {
            break;
        }
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        const char *input = yyjson_get_str(value);
        yyjson_mut_obj_add_strcpy(doc, item, "requested_scope", input ? input : "");
        char scope[CBM_SZ_4K];
        coverage_path_result_t normalized =
            coverage_normalize_rel(input, true, scope, sizeof(scope));
        if (normalized != COVERAGE_PATH_OK) {
            yyjson_mut_obj_add_str(doc, item, "status",
                                   normalized == COVERAGE_PATH_OUTSIDE ? "outside_project"
                                                                       : "invalid_path");
        } else {
            coverage_add_scope_json(doc, item, scope, all, all_count, recorded, scope_offset,
                                    scope_limit);
        }
        yyjson_mut_arr_add_val(scope_results, item);
    }
    if (project_summary && rc == CBM_STORE_OK) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "requested_scope", ".");
        coverage_add_scope_json(doc, item, "", all, all_count, recorded, scope_offset, scope_limit);
        yyjson_mut_arr_add_val(scope_results, item);
    }
    yyjson_mut_obj_add_val(doc, root, "scopes", scope_results);
    if (scope_count) {
        yyjson_mut_obj_add_str(doc, root, "scope_note",
                               "Scopes list indexed files only. Files excluded by ignore rules "
                               "or unsupported languages are not enumerable; check them as "
                               "exact paths.");
    }
    yyjson_mut_obj_add_str(
        doc, root, "caveat",
        "Best-effort signal about extraction only. no_recorded_issue does not prove graph or "
        "source completeness; read flagged source and qualify claims when freshness is not "
        "metadata_match.");

    cbm_gitignore_free(gitignore);
    cbm_gitignore_free(cbmignore);
    cbm_zova_file_coverage_free(all, all_count);
    free(root_path);
    cbm_zova_repository_close(repo);
    yyjson_doc_free(adoc);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(project);
    if (rc != CBM_STORE_OK) {
        free(json);
        return cbm_mcp_text_result("shared cbm.zova coverage read failed", true);
    }
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* index_status summary: counts over the active generation plus a few gap
 * examples. Leaves root untouched when the read fails. */
static void add_coverage_summary(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                 cbm_zova_repository_t *repo, const char *workspace_id) {
    cbm_zova_file_coverage_t *rows = NULL;
    int count = 0;
    int64_t generation = 0;
    bool recorded = false;
    if (cbm_zova_repository_file_coverage(repo, workspace_id, NULL, &rows, &count, NULL,
                                          &generation, &recorded) != CBM_STORE_OK) {
        return;
    }
    yyjson_mut_val *cov = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_sint(doc, cov, "generation", generation);
    yyjson_mut_obj_add_bool(doc, cov, "recorded", recorded);
    coverage_add_scope_json(doc, cov, "", rows, count, recorded, 0, COVERAGE_STATUS_EXAMPLES);
    yyjson_mut_obj_remove_key(cov, "scope");
    yyjson_mut_obj_add_str(doc, cov, "note",
                           "Extraction completeness only. Use check_index_coverage for exact "
                           "paths, scopes and paging.");
    yyjson_mut_obj_add_val(doc, root, "coverage", cov);
    cbm_zova_file_coverage_free(rows, count);
}

/* get_code_snippet: flag a partially parsed or skipped source file. NULL when
 * the file has no recorded gap. Caller frees. */
static char *coverage_snippet_note(cbm_zova_repository_t *repo, const char *workspace_id,
                                   const char *file_path) {
    cbm_zova_file_coverage_t *rows = NULL;
    int count = 0;
    if (!file_path || !file_path[0] ||
        cbm_zova_repository_file_coverage(repo, workspace_id, file_path, &rows, &count, NULL,
                                          NULL, NULL) != CBM_STORE_OK) {
        return NULL;
    }
    char *note = NULL;
    if (count > 0) {
        coverage_file_t c;
        coverage_file_parse(&rows[0], &c);
        char buf[CBM_SZ_1K];
        if (c.ranges) {
            snprintf(buf, sizeof(buf),
                     "The parser recovered from errors in this file (lines %s); the graph may "
                     "miss definitions or calls there. Read the source directly.",
                     c.ranges[0] ? c.ranges : "unknown");
            note = cbm_heap_strdup(buf);
        } else if (c.phase) {
            snprintf(buf, sizeof(buf),
                     "This file was not extracted (%s%s%s); the graph may miss its definitions. "
                     "Read the source directly.",
                     c.phase, c.reason && c.reason[0] ? ": " : "", c.reason ? c.reason : "");
            note = cbm_heap_strdup(buf);
        }
        coverage_file_clear(&c);
    }
    cbm_zova_file_coverage_free(rows, count);
    return note;
}

static char *handle_index_status(cbm_mcp_server_t *srv, const char *args) {
    char *project = get_project_arg(args);
    char path[CBM_SZ_1K];
    cbm_zova_repository_t *repo = NULL;
    if (!project || cbm_zova_user_database_path(path, sizeof(path)) != 0 ||
        !(repo = cbm_zova_repository_open(path, project))) {
        return project_not_found_result(project);
    }
    int nodes = 0, edges = 0;
    cbm_project_t info = {0};
    const char *workspace_id = cbm_zova_repository_workspace_id(repo);
    int rc = cbm_zova_repository_counts(repo, workspace_id, &nodes, &edges);
    if (rc == CBM_STORE_OK)
        rc = cbm_zova_repository_get_project(repo, workspace_id, &info);
    if (rc != CBM_STORE_OK) {
        cbm_zova_repository_close(repo);
        free(project);
        return cbm_mcp_text_result("shared cbm.zova status read failed", true);
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_int(doc, root, "nodes", nodes);
    yyjson_mut_obj_add_int(doc, root, "edges", edges);
    yyjson_mut_obj_add_str(doc, root, "status", nodes > 0 ? "ready" : "empty");
    yyjson_mut_obj_add_strcpy(doc, root, "root_path", info.root_path ? info.root_path : "");
    add_coverage_summary(doc, root, repo, workspace_id);
    cbm_zova_repository_close(repo);
    add_git_context_json(doc, root, info.root_path);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    safe_str_free(&info.name); safe_str_free(&info.indexed_at); safe_str_free(&info.root_path);
    free(project);
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

enum { DELETE_RETRY_MS = 50 };

/* delete_project: delete the project's workspace from the shared cbm.zova. */
static char *handle_delete_project(cbm_mcp_server_t *srv, const char *args) {
    char *name = get_project_arg(args);
    if (!name) {
        return cbm_mcp_text_result("project is required", true);
    }

    /* Close store if it's the project being deleted */
    if (srv->current_project && strcmp(srv->current_project, name) == 0) {
        if (srv->owns_store && srv->store) {
            cbm_store_close(srv->store);
            srv->store = NULL;
        }
        free(srv->current_project);
        srv->current_project = NULL;
    }

    char path[CBM_SZ_1K];
    const char *status = "not_found";
    const char *error_detail = NULL;
    bool is_error = false;
    cbm_zova_operation_report_t report = {0};

    char workspace_id[CBM_SZ_256] = {0};
    cbm_zova_repository_t *repo = NULL;
    if (cbm_zova_user_database_path(path, sizeof(path)) == 0 &&
        (repo = cbm_zova_repository_open(path, name))) {
        snprintf(workspace_id, sizeof(workspace_id), "%s",
                 cbm_zova_repository_workspace_id(repo));
        cbm_zova_repository_close(repo);
    }
    /* The delete takes the workspace lease without waiting; wait here (bounded)
     * for an index run of this workspace to finish. */
    cbm_zova_operation_code_t code = CBM_ZOVA_OPERATION_NOOP;
    for (int64_t wait_left_ms = cbm_zova_coord_default_wait_ms(); workspace_id[0];
         wait_left_ms -= DELETE_RETRY_MS) {
        code = cbm_zova_workspace_delete(path, workspace_id, workspace_id, &report);
        if (code != CBM_ZOVA_OPERATION_BUSY || strcmp(report.reason, "workspace_busy") != 0 ||
            wait_left_ms <= 0) {
            break;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = DELETE_RETRY_MS * 1000000L};
        cbm_nanosleep(&pause, NULL);
    }
    if (code == CBM_ZOVA_OPERATION_OK) {
        status = "deleted";
    } else if (code == CBM_ZOVA_OPERATION_NOOP) {
        is_error = true;
    } else {
        status = "delete_failed";
        error_detail = report.reason;
        is_error = true;
    }

    if (srv->watcher) {
        cbm_watcher_unwatch(srv->watcher, name);
    }

    cbm_mem_collect(); /* return freed pages to OS after closing database */

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", name);
    yyjson_mut_obj_add_str(doc, root, "status", status);
    if (error_detail) {
        yyjson_mut_obj_add_str(doc, root, "error", error_detail);
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(name);

    char *result = cbm_mcp_text_result(json, is_error);
    free(json);
    return result;
}

/* Canonical list of valid aspect tokens for get_architecture. Single source
 * of truth for the server-side validation (authoritative); the JSON-Schema
 * enum in the TOOLS entry above is the advisory client-side mirror — update
 * both together when the aspect set changes. */
static const char *VALID_ASPECTS[] = {
    "all",          "overview", "structure",  "dependencies", "routes",    "languages", "packages",
    "entry_points", "hotspots", "boundaries", "layers",       "file_tree", "clusters",  NULL};

static bool aspect_is_valid(const char *name) {
    if (!name) {
        return false;
    }
    for (int i = 0; VALID_ASPECTS[i]; i++) {
        if (strcmp(name, VALID_ASPECTS[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Check if an aspect is requested. NULL aspects = all. The array can contain
 * "all" (everything), "overview" (everything except file_tree — see
 * cbm_store_arch_aspect_in_overview in store.c), or the aspect name itself. */
static bool aspect_wanted(yyjson_doc *aspects_doc, yyjson_val *aspects_arr, const char *name) {
    if (!aspects_arr) {
        return true; /* no filter = all */
    }
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(aspects_arr, &iter);
    yyjson_val *val;
    while ((val = yyjson_arr_iter_next(&iter)) != NULL) {
        const char *s = yyjson_get_str(val);
        if (!s) {
            continue;
        }
        if (strcmp(s, "all") == 0) {
            return true;
        }
        if (strcmp(s, "overview") == 0 && cbm_store_arch_aspect_in_overview(name)) {
            return true;
        }
        if (strcmp(s, name) == 0) {
            return true;
        }
    }
    (void)aspects_doc;
    return false;
}

static char *handle_get_architecture(cbm_mcp_server_t *srv, const char *args) {
    char *project = get_project_arg(args);
    char *scope_path = cbm_mcp_get_string_arg(args, "path");
    cbm_store_t *store = resolve_store(srv, project);
    REQUIRE_STORE(store, project);

    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(project);
        free(scope_path);
        return not_indexed;
    }

    /* Parse aspects array from args */
    yyjson_doc *aspects_doc = NULL;
    yyjson_val *aspects_arr = NULL;
    {
        yyjson_doc *args_doc = yyjson_read(args, strlen(args), 0);
        if (args_doc) {
            yyjson_val *aval = yyjson_obj_get(yyjson_doc_get_root(args_doc), "aspects");
            if (yyjson_is_arr(aval)) {
                aspects_doc = args_doc; /* keep alive */
                aspects_arr = aval;
            } else {
                yyjson_doc_free(args_doc);
            }
        }
    }

    /* Build a C string array from aspects for cbm_store_get_architecture.
     * Strings point into aspects_doc memory so aspects_doc must outlive this array. */
    const char *aspects_strs[MCP_COL_16];
    int aspects_strs_count = 0;
    if (aspects_arr) {
        size_t aspect_idx;
        size_t aspect_max;
        yyjson_val *aspect_val;
        yyjson_arr_foreach(aspects_arr, aspect_idx, aspect_max, aspect_val) {
            const char *s = yyjson_get_str(aspect_val);
            if (s && aspects_strs_count < MCP_COL_16) {
                aspects_strs[aspects_strs_count++] = s;
            }
        }
    }

    /* Server-side validation: reject unknown aspect tokens with an isError
     * result listing the valid values. The JSON-Schema enum is advisory —
     * many MCP clients do not validate arguments against tool schemas — so
     * without this check a typo degraded to a silent near-empty payload. */
    for (int i = 0; i < aspects_strs_count; i++) {
        if (!aspect_is_valid(aspects_strs[i])) {
            char valid_list[CBM_SZ_256];
            size_t off = 0;
            for (int j = 0; VALID_ASPECTS[j] && off < sizeof(valid_list); j++) {
                int n = snprintf(valid_list + off, sizeof(valid_list) - off, "%s%s",
                                 j > 0 ? ", " : "", VALID_ASPECTS[j]);
                if (n < 0) {
                    break;
                }
                off += (size_t)n;
            }
            char msg[CBM_SZ_512];
            snprintf(msg, sizeof(msg), "Unknown aspect '%s'. Valid: %s.", aspects_strs[i],
                     valid_list);
            char *err = cbm_mcp_text_result(msg, true);
            free(project);
            free(scope_path);
            if (aspects_doc) {
                yyjson_doc_free(aspects_doc);
            }
            return err;
        }
    }

    cbm_schema_info_t schema = {0};
    /* Counts-only: this handler renders label/type counts but never property
     * keys, and full key discovery json_each-scans every row (seconds-to-
     * minutes on multi-million-node graphs). */
    int schema_rc = cbm_store_get_schema_counts_scoped(store, project, scope_path, &schema);

    cbm_architecture_info_t arch = {0};
    int arch_rc = cbm_store_get_architecture(store, project, scope_path,
                                             aspects_strs_count > 0 ? aspects_strs : NULL,
                                             aspects_strs_count, &arch);

    int node_count = cbm_store_count_nodes_scoped(store, project, scope_path);
    int edge_count = cbm_store_count_edges_scoped(store, project, scope_path);
    if (schema_rc != CBM_STORE_OK || arch_rc != CBM_STORE_OK || node_count < 0 ||
        edge_count < 0) {
        cbm_store_schema_free(&schema);
        cbm_store_architecture_free(&arch);
        free(project);
        free(scope_path);
        if (aspects_doc) {
            yyjson_doc_free(aspects_doc);
        }
        return cbm_mcp_text_result("failed to read the project architecture", true);
    }
    char norm_path[CBM_SZ_512];
    bool path_scoped = cbm_store_normalize_arch_path(scope_path, norm_path, sizeof(norm_path));

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (project) {
        yyjson_mut_obj_add_str(doc, root, "project", project);
    }
    if (path_scoped) {
        yyjson_mut_obj_add_str(doc, root, "path", norm_path);
        int root_nodes = cbm_store_count_nodes(store, project);
        int root_edges = cbm_store_count_edges(store, project);
        yyjson_mut_obj_add_int(doc, root, "root_total_nodes", root_nodes);
        yyjson_mut_obj_add_int(doc, root, "root_total_edges", root_edges);
        yyjson_mut_obj_add_int(doc, root, "scoped_total_nodes", node_count);
        yyjson_mut_obj_add_int(doc, root, "scoped_total_edges", edge_count);
    }
    yyjson_mut_obj_add_int(doc, root, "total_nodes", node_count);
    yyjson_mut_obj_add_int(doc, root, "total_edges", edge_count);

    /* Node label summary */
    if (aspect_wanted(aspects_doc, aspects_arr, "structure")) {
        yyjson_mut_val *labels = yyjson_mut_arr(doc);
        for (int i = 0; i < schema.node_label_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "label", schema.node_labels[i].label);
            yyjson_mut_obj_add_int(doc, item, "count", schema.node_labels[i].count);
            yyjson_mut_arr_add_val(labels, item);
        }
        yyjson_mut_obj_add_val(doc, root, "node_labels", labels);
    }

    /* Edge type summary */
    if (aspect_wanted(aspects_doc, aspects_arr, "dependencies")) {
        yyjson_mut_val *types = yyjson_mut_arr(doc);
        for (int i = 0; i < schema.edge_type_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "type", schema.edge_types[i].type);
            yyjson_mut_obj_add_int(doc, item, "count", schema.edge_types[i].count);
            yyjson_mut_arr_add_val(types, item);
        }
        yyjson_mut_obj_add_val(doc, root, "edge_types", types);
    }

    /* Relationship patterns */
    if (aspect_wanted(aspects_doc, aspects_arr, "routes") && schema.rel_pattern_count > 0) {
        yyjson_mut_val *pats = yyjson_mut_arr(doc);
        for (int i = 0; i < schema.rel_pattern_count; i++) {
            yyjson_mut_arr_add_str(doc, pats, schema.rel_patterns[i]);
        }
        yyjson_mut_obj_add_val(doc, root, "relationship_patterns", pats);
    }

    /* Languages */
    if (arch.language_count > 0) {
        yyjson_mut_val *langs = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.language_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "language",
                                   arch.languages[i].language ? arch.languages[i].language : "");
            yyjson_mut_obj_add_int(doc, item, "file_count", arch.languages[i].file_count);
            yyjson_mut_arr_add_val(langs, item);
        }
        yyjson_mut_obj_add_val(doc, root, "languages", langs);
    }

    /* Packages */
    if (arch.package_count > 0) {
        yyjson_mut_val *pkgs = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.package_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name",
                                   arch.packages[i].name ? arch.packages[i].name : "");
            yyjson_mut_obj_add_int(doc, item, "node_count", arch.packages[i].node_count);
            yyjson_mut_obj_add_int(doc, item, "fan_in", arch.packages[i].fan_in);
            yyjson_mut_obj_add_int(doc, item, "fan_out", arch.packages[i].fan_out);
            yyjson_mut_arr_add_val(pkgs, item);
        }
        yyjson_mut_obj_add_val(doc, root, "packages", pkgs);
    }

    /* Entry points */
    if (arch.entry_point_count > 0) {
        yyjson_mut_val *eps = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.entry_point_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name",
                                   arch.entry_points[i].name ? arch.entry_points[i].name : "");
            yyjson_mut_obj_add_str(
                doc, item, "qualified_name",
                arch.entry_points[i].qualified_name ? arch.entry_points[i].qualified_name : "");
            yyjson_mut_obj_add_str(doc, item, "file",
                                   arch.entry_points[i].file ? arch.entry_points[i].file : "");
            yyjson_mut_arr_add_val(eps, item);
        }
        yyjson_mut_obj_add_val(doc, root, "entry_points", eps);
    }

    /* HTTP routes */
    if (arch.route_count > 0) {
        yyjson_mut_val *routes = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.route_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "method",
                                   arch.routes[i].method ? arch.routes[i].method : "");
            yyjson_mut_obj_add_str(doc, item, "path",
                                   arch.routes[i].path ? arch.routes[i].path : "");
            yyjson_mut_obj_add_str(doc, item, "handler",
                                   arch.routes[i].handler ? arch.routes[i].handler : "");
            yyjson_mut_arr_add_val(routes, item);
        }
        yyjson_mut_obj_add_val(doc, root, "routes", routes);
    }

    /* Hotspots */
    if (arch.hotspot_count > 0) {
        yyjson_mut_val *hotspots = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.hotspot_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name",
                                   arch.hotspots[i].name ? arch.hotspots[i].name : "");
            yyjson_mut_obj_add_str(doc, item, "qualified_name",
                                   arch.hotspots[i].qualified_name ? arch.hotspots[i].qualified_name
                                                                   : "");
            yyjson_mut_obj_add_int(doc, item, "fan_in", arch.hotspots[i].fan_in);
            yyjson_mut_arr_add_val(hotspots, item);
        }
        yyjson_mut_obj_add_val(doc, root, "hotspots", hotspots);
    }

    /* Cross-package boundaries */
    if (arch.boundary_count > 0) {
        yyjson_mut_val *boundaries = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.boundary_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "from",
                                   arch.boundaries[i].from ? arch.boundaries[i].from : "");
            yyjson_mut_obj_add_str(doc, item, "to",
                                   arch.boundaries[i].to ? arch.boundaries[i].to : "");
            yyjson_mut_obj_add_int(doc, item, "call_count", arch.boundaries[i].call_count);
            yyjson_mut_arr_add_val(boundaries, item);
        }
        yyjson_mut_obj_add_val(doc, root, "boundaries", boundaries);
    }

    /* Cross-service links (HTTP/async between services) */
    if (arch.service_count > 0) {
        yyjson_mut_val *services = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.service_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "from",
                                   arch.services[i].from ? arch.services[i].from : "");
            yyjson_mut_obj_add_str(doc, item, "to", arch.services[i].to ? arch.services[i].to : "");
            yyjson_mut_obj_add_str(doc, item, "type",
                                   arch.services[i].type ? arch.services[i].type : "");
            yyjson_mut_obj_add_int(doc, item, "count", arch.services[i].count);
            yyjson_mut_arr_add_val(services, item);
        }
        yyjson_mut_obj_add_val(doc, root, "services", services);
    }

    /* Package layers */
    if (arch.layer_count > 0) {
        yyjson_mut_val *layers = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.layer_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name",
                                   arch.layers[i].name ? arch.layers[i].name : "");
            yyjson_mut_obj_add_str(doc, item, "layer",
                                   arch.layers[i].layer ? arch.layers[i].layer : "");
            yyjson_mut_obj_add_str(doc, item, "reason",
                                   arch.layers[i].reason ? arch.layers[i].reason : "");
            yyjson_mut_arr_add_val(layers, item);
        }
        yyjson_mut_obj_add_val(doc, root, "layers", layers);
    }

    /* Clusters (community detection) */
    if (arch.cluster_count > 0) {
        yyjson_mut_val *clusters = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.cluster_count; i++) {
            const cbm_cluster_info_t *c = &arch.clusters[i];
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_int(doc, item, "id", c->id);
            yyjson_mut_obj_add_str(doc, item, "label", c->label ? c->label : "");
            yyjson_mut_obj_add_int(doc, item, "members", c->members);
            yyjson_mut_obj_add_real(doc, item, "cohesion", c->cohesion);
            yyjson_mut_val *top = yyjson_mut_arr(doc);
            for (int j = 0; j < c->top_node_count; j++) {
                yyjson_mut_arr_add_str(doc, top, c->top_nodes[j] ? c->top_nodes[j] : "");
            }
            yyjson_mut_obj_add_val(doc, item, "top_nodes", top);
            yyjson_mut_val *pkgs = yyjson_mut_arr(doc);
            for (int j = 0; j < c->package_count; j++) {
                yyjson_mut_arr_add_str(doc, pkgs, c->packages[j] ? c->packages[j] : "");
            }
            yyjson_mut_obj_add_val(doc, item, "packages", pkgs);
            yyjson_mut_val *etypes = yyjson_mut_arr(doc);
            for (int j = 0; j < c->edge_type_count; j++) {
                yyjson_mut_arr_add_str(doc, etypes, c->edge_types[j] ? c->edge_types[j] : "");
            }
            yyjson_mut_obj_add_val(doc, item, "edge_types", etypes);
            yyjson_mut_arr_add_val(clusters, item);
        }
        yyjson_mut_obj_add_val(doc, root, "clusters", clusters);
    }

    /* File tree */
    if (arch.file_tree_count > 0) {
        yyjson_mut_val *file_tree = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.file_tree_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "path",
                                   arch.file_tree[i].path ? arch.file_tree[i].path : "");
            yyjson_mut_obj_add_str(doc, item, "type",
                                   arch.file_tree[i].type ? arch.file_tree[i].type : "");
            yyjson_mut_obj_add_int(doc, item, "children", arch.file_tree[i].children);
            yyjson_mut_arr_add_val(file_tree, item);
        }
        yyjson_mut_obj_add_val(doc, root, "file_tree", file_tree);
    }


    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_store_architecture_free(&arch);
    cbm_store_schema_free(&schema);
    if (aspects_doc) {
        yyjson_doc_free(aspects_doc);
    }
    free(project);
    free(scope_path);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* Resolve edge types from args: explicit array > mode-based > default ("CALLS").
 * Writes types into out_types (max 16). Returns the parsed yyjson_doc if explicit
 * edge_types were found (caller must keep alive until types are consumed), or NULL. */
static yyjson_doc *resolve_trace_edge_types(const char *args, const char *mode,
                                            const char **out_types, int *out_count) {
    static const char *mode_calls[] = {"CALLS"};
    static const char *mode_data_flow[] = {"CALLS", "DATA_FLOWS"};
    static const char *mode_cross_svc[] = {"HTTP_CALLS", "ASYNC_CALLS", "DATA_FLOWS", "CALLS"};

    *out_count = 0;

    yyjson_doc *et_doc = yyjson_read(args, strlen(args), 0);
    if (et_doc) {
        yyjson_val *et_arr = yyjson_obj_get(yyjson_doc_get_root(et_doc), "edge_types");
        if (et_arr && yyjson_is_arr(et_arr)) {
            size_t idx2;
            size_t max2;
            yyjson_val *val2;
            yyjson_arr_foreach(et_arr, idx2, max2, val2) {
                if (yyjson_is_str(val2) && *out_count < MCP_COL_16) {
                    out_types[(*out_count)++] = yyjson_get_str(val2);
                }
            }
        }
    }

    if (*out_count > 0) {
        return et_doc; /* caller must keep alive — pointers reference doc memory */
    }

    yyjson_doc_free(et_doc); /* no explicit types found, free */

    const char **defaults = mode_calls;
    int n_defaults = SKIP_ONE;
    if (mode && strcmp(mode, "data_flow") == 0) {
        defaults = mode_data_flow;
        n_defaults = MCP_N_DEFAULTS_2;
    } else if (mode && strcmp(mode, "cross_service") == 0) {
        defaults = mode_cross_svc;
        n_defaults = (int)(sizeof(mode_cross_svc) / sizeof(mode_cross_svc[0]));
    }
    for (int i = 0; i < n_defaults; i++) {
        out_types[i] = defaults[i];
    }
    *out_count = n_defaults;
    return NULL;
}

/* Check if a file path looks like a test file. */
static bool is_test_file(const char *path) {
    if (!path) {
        return false;
    }
    return strstr(path, "/test") != NULL || strstr(path, "test_") != NULL ||
           strstr(path, "_test.") != NULL || strstr(path, "/tests/") != NULL ||
           strstr(path, "/spec/") != NULL || strstr(path, ".test.") != NULL ||
           /* project-root-relative test trees have no leading slash (#1294) */
           strncmp(path, "tests/", SLEN("tests/")) == 0 ||
           strncmp(path, "test/", SLEN("test/")) == 0 ||
           strncmp(path, "spec/", SLEN("spec/")) == 0 ||
           strncmp(path, "__tests__/", SLEN("__tests__/")) == 0;
}

static yyjson_mut_val *trace_json_append(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                         const char *name, const char *qualified_name,
                                         const char *file_path, int hop, bool risk_labels,
                                         bool include_tests) {
    bool test = is_test_file(file_path);
    if (!include_tests && test) {
        return NULL;
    }
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, item, "name", name ? name : "");
    yyjson_mut_obj_add_str(doc, item, "qualified_name", qualified_name ? qualified_name : "");
    yyjson_mut_obj_add_int(doc, item, "hop", hop);
    if (risk_labels) {
        yyjson_mut_obj_add_str(doc, item, "risk", cbm_risk_label(cbm_hop_to_risk(hop)));
    }
    if (test) {
        yyjson_mut_obj_add_bool(doc, item, "is_test", true);
    }
    yyjson_mut_arr_add_val(arr, item);
    return item;
}

/* Convert BFS traversal results into a yyjson_mut array. */
/* Find the CALLS-edge "args" JSON (the serialized arg expressions) on the edge
 * that leads to the given hop node, so data_flow mode can surface argument
 * expressions (#514). Returns the borrowed substring "[...]" inside the edge's
 * properties_json, with its length, or NULL when no args are recorded. */
static const char *bfs_edge_args_for_hop(cbm_traverse_result_t *tr, int64_t hop_node_id,
                                         size_t *out_len) {
    for (int e = 0; e < tr->edge_count; e++) {
        /* The hop node is the edge endpoint reached from the root side: for an
         * outbound trace it is the target, for inbound it is the source. Match
         * on either so both directions surface their args. */
        if (tr->edges[e].target_id != hop_node_id && tr->edges[e].source_id != hop_node_id) {
            continue;
        }
        const char *pj = tr->edges[e].properties_json;
        if (!pj) {
            continue;
        }
        const char *args = strstr(pj, "\"args\"");
        if (!args) {
            continue;
        }
        const char *open = strchr(args, '[');
        if (!open) {
            continue;
        }
        int depth = 0;
        const char *p = open;
        for (; *p; p++) {
            if (*p == '[') {
                depth++;
            } else if (*p == ']') {
                depth--;
                if (depth == 0) {
                    p++;
                    break;
                }
            }
        }
        *out_len = (size_t)(p - open);
        return open;
    }
    return NULL;
}

/* Map an internal resolver strategy (recorded on CALLS and CALL_REFERENCE
 * edges) to the closed public class trace_path publishes with
 * include_evidence (upstream 73cfb61d): lsp | language_rule | heuristic |
 * unresolved. Publishing the raw ~20 internal names would make each resolver
 * name public API by accident. NULL only for a NULL/empty strategy. */
const char *cbm_mcp_edge_strategy_class(const char *strategy) {
    if (!strategy || !strategy[0]) {
        return NULL;
    }
    /* lsp_unresolved is an LSP attempt that did NOT resolve. */
    if (strcmp(strategy, "lsp_unresolved") == 0 || strcmp(strategy, "unknown") == 0) {
        return "unresolved";
    }
    if (strncmp(strategy, "lsp_", SLEN("lsp_")) == 0) {
        return "lsp";
    }
    if (strncmp(strategy, "php_", SLEN("php_")) == 0 ||
        strncmp(strategy, "perl_", SLEN("perl_")) == 0) {
        return "language_rule";
    }
    return "heuristic"; /* callee_suffix, unique_name, field_type_hint, ... */
}

/* Resolution evidence on the edge leading to the given hop node, matched like
 * bfs_edge_args_for_hop. tr is the trace result, so evidence covers only the
 * rows returned. False when the edge records no strategy. */
static bool bfs_edge_evidence_for_hop(cbm_traverse_result_t *tr, int64_t hop_node_id,
                                      const char **class_out, double *confidence_out) {
    for (int e = 0; e < tr->edge_count; e++) {
        if (tr->edges[e].target_id != hop_node_id && tr->edges[e].source_id != hop_node_id) {
            continue;
        }
        const char *pj = tr->edges[e].properties_json;
        yyjson_doc *pdoc = pj ? yyjson_read(pj, strlen(pj), 0) : NULL;
        yyjson_val *proot = yyjson_doc_get_root(pdoc);
        const char *cls =
            cbm_mcp_edge_strategy_class(yyjson_get_str(yyjson_obj_get(proot, "strategy")));
        yyjson_val *conf = yyjson_obj_get(proot, "confidence");
        double confidence = yyjson_is_num(conf) ? yyjson_get_num(conf) : -1.0;
        yyjson_doc_free(pdoc);
        if (cls) {
            *class_out = cls;
            *confidence_out = confidence;
            return true;
        }
    }
    return false;
}

static yyjson_mut_val *bfs_to_json_array(yyjson_mut_doc *doc, cbm_traverse_result_t *tr,
                                         bool risk_labels, bool include_tests, bool data_flow,
                                         bool include_evidence) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < tr->visited_count; i++) {
        const char *fp = tr->visited[i].node.file_path;
        yyjson_mut_val *item =
            trace_json_append(doc, arr, tr->visited[i].node.name, tr->visited[i].node.qualified_name,
                              fp, tr->visited[i].hop, risk_labels, include_tests);
        if (!item) {
            continue;
        }
        const char *ev_class = NULL;
        double ev_conf = -1.0;
        if (include_evidence &&
            bfs_edge_evidence_for_hop(tr, tr->visited[i].node.id, &ev_class, &ev_conf)) {
            yyjson_mut_obj_add_str(doc, item, "strategy", ev_class);
            if (ev_conf >= 0.0) {
                yyjson_mut_obj_add_real(doc, item, "confidence", ev_conf);
            }
        }
        /* data_flow mode promises argument expressions at each call site; surface
         * the CALLS edge's serialized args array as a raw JSON value (#514). */
        if (data_flow) {
            size_t alen = 0;
            const char *args = bfs_edge_args_for_hop(tr, tr->visited[i].node.id, &alen);
            if (args && alen > 0) {
                yyjson_mut_val *av = yyjson_mut_rawn(doc, args, alen);
                if (av) {
                yyjson_mut_obj_add_val(doc, item, "args", av);
                }
            }
        }
    }
    return arr;
}

static char *snippet_suggestions(const char *input, cbm_node_t *nodes, int count);

/* Rank a candidate for name resolution. The label tier (callable > class-like >
 * module/file) is the primary key; WITHIN a tier the larger definition by line
 * span wins. In practice the .c-over-.h and C-main-over-shell-main preferences
 * come primarily from span (the real definition has the larger body), since the
 * competing matches usually share a tier — no file extension is hardcoded.
 * Consequence: two same-tier candidates with equal span tie and are reported
 * ambiguous (see pick_resolved_node) rather than guessed. */
enum {
    RES_RANK_CALLABLE = 2,     /* Function / Method */
    RES_RANK_OTHER = 1,        /* Class / Struct / etc. */
    RES_RANK_MODULE = 0,       /* Module / File */
    RES_LABEL_WEIGHT = 1000000 /* label tier dominates span */
};
static long node_resolution_score(const cbm_node_t *n) {
    long label_rank = RES_RANK_MODULE;
    if (n->label) {
        if (strcmp(n->label, "Function") == 0 || strcmp(n->label, "Method") == 0) {
            label_rank = RES_RANK_CALLABLE;
        } else if (strcmp(n->label, "Module") != 0 && strcmp(n->label, "File") != 0) {
            label_rank = RES_RANK_OTHER;
        }
    }
    /* Tie rule (CBM_MACRO_QN_SUFFIX): a name that is both a definition and a C
     * macro -- a typedef or enumerator next to its rename macro -- resolves to
     * the definition. The macro scores just under every other definition, and
     * still above Module/File; without this a one-line typedef and its macro
     * tie on span and the name reads as ambiguous. */
    size_t qn_len = n->qualified_name ? strlen(n->qualified_name) : 0;
    size_t fence_len = sizeof(CBM_MACRO_QN_SUFFIX) - SKIP_ONE;
    if (label_rank == RES_RANK_OTHER && qn_len > fence_len &&
        strcmp(n->qualified_name + qn_len - fence_len, CBM_MACRO_QN_SUFFIX) == 0) {
        return RES_RANK_OTHER * (long)RES_LABEL_WEIGHT - SKIP_ONE;
    }
    long span = (long)n->end_line - (long)n->start_line;
    if (span < 0) {
        span = 0;
    }
    return label_rank * (long)RES_LABEL_WEIGHT + span;
}

/* A "real" callable definition: a Function/Method node with a non-empty body
 * span (end_line > start_line). A body-less node (start_line == end_line) is an
 * ambient declaration / signature stub — e.g. a TypeScript `.d.ts` declaration
 * — which is a *fragment* of one logical symbol, not a distinct definition. The
 * distinction lets pick_resolved_node union a stub with its real implementation
 * (#546) while still treating two genuinely-different same-named functions as
 * ambiguous rather than conflating their caller sets. */
static bool node_is_real_callable_def(const cbm_node_t *n) {
    if (!n->label) {
        return false;
    }
    if (strcmp(n->label, "Function") != 0 && strcmp(n->label, "Method") != 0) {
        return false;
    }
    return (long)n->end_line - (long)n->start_line > 0;
}

/* Keep the nodes defined in `file`: its exact repository-relative path, or a
 * trailing path suffix of one ("db.zig" matches "src/db.zig"). Dropped nodes
 * are freed; returns the new count. */
static int filter_nodes_by_file(cbm_node_t *nodes, int count, const char *file) {
    if (!nodes || !file || !file[0]) {
        return count;
    }
    size_t fl = strlen(file);
    int kept = 0;
    for (int i = 0; i < count; i++) {
        const char *fp = nodes[i].file_path ? nodes[i].file_path : "";
        size_t pl = strlen(fp);
        bool match = strcmp(fp, file) == 0 ||
                     (pl > fl && fp[pl - fl - SKIP_ONE] == '/' && strcmp(fp + pl - fl, file) == 0);
        if (!match) {
            free_node_contents(&nodes[i]);
        } else if (kept != i) {
            nodes[kept] = nodes[i];
            memset(&nodes[i], 0, sizeof(nodes[i]));
            kept++;
        } else {
            kept++;
        }
    }
    return kept;
}

/* Pick the best-resolving node among name matches. Sets *ambiguous when the
 * matches can't be reduced to one logical symbol, so resolution never silently
 * traces (or conflates) the wrong same-named node:
 *   1. the top score is shared by >1 candidate (a genuine rank/span tie), or
 *   2. two or more *real* callable definitions share the name — distinct
 *      implementations, not a definition plus its body-less stub(s).
 * Rule 2 completes rule 1: without it, two same-named functions whose bodies
 * differ in length score differently, dodge the tie, and get their caller sets
 * unioned by bfs_union_same_name (#546) into one confidently-conflated answer.
 * Body-less .d.ts stubs still union with their implementation (#650). */
static int pick_resolved_node(const cbm_node_t *nodes, int count, bool *ambiguous) {
    *ambiguous = false;
    if (count <= 1) {
        return 0;
    }
    int best = 0;
    long best_score = node_resolution_score(&nodes[0]);
    for (int i = 1; i < count; i++) {
        long s = node_resolution_score(&nodes[i]);
        if (s > best_score) {
            best_score = s;
            best = i;
        }
    }
    int top_count = 0;
    int real_def_count = 0;
    for (int i = 0; i < count; i++) {
        if (node_resolution_score(&nodes[i]) == best_score) {
            top_count++;
        }
        if (node_is_real_callable_def(&nodes[i])) {
            real_def_count++;
        }
    }
    if (real_def_count > 1) {
        *ambiguous = true;
    }
    if (top_count > 1) {
        *ambiguous = true;
    }
    return best;
}

static int bfs_visit_text_compare(const char *left, const char *right) {
    return strcmp(left ? left : "", right ? right : "");
}

static int bfs_visit_compare(const void *left, const void *right) {
    const cbm_node_hop_t *a = left;
    const cbm_node_hop_t *b = right;
    if (a->hop != b->hop) {
        return a->hop < b->hop ? -1 : 1;
    }
    int cmp = bfs_visit_text_compare(a->node.file_path, b->node.file_path);
    if (cmp != 0) {
        return cmp;
    }
    cmp = bfs_visit_text_compare(a->node.qualified_name, b->node.qualified_name);
    if (cmp != 0) {
        return cmp;
    }
    cmp = bfs_visit_text_compare(a->node.label, b->node.label);
    if (cmp != 0) {
        return cmp;
    }
    if (a->node.start_line != b->node.start_line) {
        return a->node.start_line < b->node.start_line ? -1 : 1;
    }
    if (a->node.end_line != b->node.end_line) {
        return a->node.end_line < b->node.end_line ? -1 : 1;
    }
    if (a->node.id == b->node.id) {
        return 0;
    }
    return a->node.id < b->node.id ? -1 : 1;
}

/* BFS from EVERY node sharing the resolved name and merge the results, so the
 * caller/callee set is complete even when one logical symbol is represented by
 * more than one graph node — e.g. a real .ts implementation plus an ambient
 * .d.ts stub, whose inbound CALLS edges are otherwise split across the two
 * nodes and silently truncated by tracing only one (#546). visited hops are
 * deduped by node id; edges are concatenated. Ownership of all heap fields
 * transfers into *out, freed by cbm_store_traverse_free. */
static void bfs_union_same_name(cbm_store_t *store, const cbm_node_t *nodes, int node_count,
                                const char *direction, const char **edge_types, int edge_type_count,
                                int depth, bool need_edges, cbm_traverse_result_t *out) {
    memset(out, 0, sizeof(*out));
    int vcap = 0, ecap = 0;
    for (int k = 0; k < node_count; k++) {
        cbm_traverse_result_t tr = {0};
        (need_edges ? cbm_store_bfs_with_edges : cbm_store_bfs)(
            store, nodes[k].id, direction, edge_types, edge_type_count, depth, MCP_BFS_LIMIT, &tr);
        for (int i = 0; i < tr.visited_count; i++) {
            int duplicate_index = -1;
            for (int j = 0; j < out->visited_count; j++) {
                if (out->visited[j].node.id == tr.visited[i].node.id) {
                    duplicate_index = j;
                    break;
                }
            }
            if (duplicate_index >= 0) {
                if (tr.visited[i].hop < out->visited[duplicate_index].hop) {
                    cbm_node_free_fields(&out->visited[duplicate_index].node);
                    out->visited[duplicate_index] = tr.visited[i];
                    memset(&tr.visited[i], 0, sizeof(tr.visited[i]));
                }
                continue;
            }
            if (out->visited_count >= vcap) {
                vcap = vcap ? vcap * 2 : 8;
                out->visited = safe_realloc(out->visited, vcap * sizeof(cbm_node_hop_t));
            }
            out->visited[out->visited_count++] = tr.visited[i];
            memset(&tr.visited[i], 0, sizeof(tr.visited[i])); /* ownership moved */
        }
        for (int i = 0; i < tr.edge_count; i++) {
            if (out->edge_count >= ecap) {
                ecap = ecap ? ecap * 2 : 8;
                out->edges = safe_realloc(out->edges, ecap * sizeof(cbm_edge_info_t));
            }
            out->edges[out->edge_count++] = tr.edges[i];
            memset(&tr.edges[i], 0, sizeof(tr.edges[i])); /* ownership moved */
        }
        cbm_store_traverse_free(&tr); /* frees only the un-moved (root + dup) fields */
    }
    if (out->visited_count > 1) {
        qsort(out->visited, (size_t)out->visited_count, sizeof(*out->visited), bfs_visit_compare);
    }
}

/* Clamp a client-supplied traversal depth to the MCP ceiling (cbm_mcp_max_depth),
 * WARN-logging when it does so — never a silent truncation (#887). An unclamped
 * `depth` would drive the shared cbm_store_bfs to an arbitrary hop count. */
static int clamp_mcp_depth(int depth, const char *tool) {
    int cap = cbm_mcp_max_depth();
    if (depth > cap) {
        char req_buf[16];
        char cap_buf[16];
        snprintf(req_buf, sizeof(req_buf), "%d", depth);
        snprintf(cap_buf, sizeof(cap_buf), "%d", cap);
        cbm_log_warn("mcp.depth_capped", "tool", tool, "requested", req_buf, "cap", cap_buf);
        return cap;
    }
    return depth;
}

static char *handle_trace_call_path(cbm_mcp_server_t *srv, const char *args) {
    char *func_name = cbm_mcp_get_string_arg(args, "function_name");
    char *project = get_project_arg(args);
    cbm_store_t *store = resolve_store(srv, project);
    char *direction = cbm_mcp_get_string_arg(args, "direction");
    char *mode = cbm_mcp_get_string_arg(args, "mode");
    char *param_name = cbm_mcp_get_string_arg(args, "parameter_name");
    int depth = cbm_mcp_get_int_arg(args, "depth", MCP_DEFAULT_DEPTH);
    depth = clamp_mcp_depth(depth, "trace_call_path");
    bool risk_labels = cbm_mcp_get_bool_arg(args, "risk_labels");
    bool include_tests = cbm_mcp_get_bool_arg(args, "include_tests");
    /* Off by default so the common response stays the same size. */
    bool include_evidence = cbm_mcp_get_bool_arg(args, "include_evidence");

    if (!func_name) {
        free(project);
        free(direction);
        free(mode);
        free(param_name);
        return cbm_mcp_text_result("function_name is required", true);
    }
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(func_name);
        free(project);
        free(direction);
        free(mode);
        free(param_name);
        return _res;
    }

    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(func_name);
        free(project);
        free(direction);
        free(mode);
        free(param_name);
        return not_indexed;
    }

    if (!direction) {
        direction = cbm_heap_strdup("both");
    }

    /* Find the node by name. If the bare-name lookup misses, fall back to
     * qualified_name so callers passing a fully-qualified identifier (which
     * the not-found hint actually recommends) hit the same path. The QN
     * lookup uses the same scan_node helper as the bare lookup, so the
     * shallow struct copy below transfers ownership of the strdup'd string
     * fields cleanly and cbm_store_free_nodes will free them. */
    cbm_node_t *nodes = NULL;
    int node_count = 0;
    cbm_store_find_nodes_by_name(store, project, func_name, &nodes, &node_count);

    if (node_count == 0) {
        cbm_node_t qn_node = {0};
        if (cbm_store_find_node_by_qn(store, project, func_name, &qn_node) == CBM_STORE_OK) {
            nodes = malloc(sizeof(cbm_node_t));
            if (nodes) {
                nodes[0] = qn_node;
                node_count = 1;
            } else {
                free_node_contents(&qn_node);
            }
        }
    }

    char *file_filter = cbm_mcp_get_string_arg(args, "file_path");
    if (file_filter && node_count > 0) {
        node_count = filter_nodes_by_file(nodes, node_count, file_filter);
        if (node_count == 0) {
            char hint[CBM_SZ_512];
            snprintf(hint, sizeof(hint),
                     "function %s is not defined in file_path %s. Omit file_path to list "
                     "every match.",
                     func_name, file_filter);
            free(file_filter);
            free(func_name);
            free(project);
            free(direction);
            free(mode);
            free(param_name);
            cbm_store_free_nodes(nodes, 0);
            return cbm_mcp_text_result(hint, true);
        }
    }
    free(file_filter);

    if (node_count == 0) {
        enum { HINT_BUF_SZ = 512 };
        char hint[HINT_BUF_SZ];
        snprintf(hint, sizeof(hint),
                 "{\"error\":\"function not found\",\"function_name\":\"%s\","
                 "\"hint\":\"Use search_graph(name_pattern=\\\".*%s.*\\\") to find the exact "
                 "name, then pass it to trace_path.\"}",
                 func_name, func_name);
        free(func_name);
        free(project);
        free(direction);
        free(mode);
        free(param_name);
        cbm_store_free_nodes(nodes, 0);
        return cbm_mcp_text_result(hint, true);
    }

    /* Disambiguate same-named matches: prefer the real definition, and report
     * ambiguity (rather than silently tracing nodes[0]) on a genuine tie — e.g.
     * a C main() vs a same-named shell-script main(). */
    bool trace_ambiguous = false;
    int sel = pick_resolved_node(nodes, node_count, &trace_ambiguous);
    if (trace_ambiguous) {
        char *result = snippet_suggestions(func_name, nodes, node_count);
        free(func_name);
        free(project);
        free(direction);
        free(mode);
        free(param_name);
        cbm_store_free_nodes(nodes, node_count);
        return result;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "function", func_name);
    yyjson_mut_obj_add_str(doc, root, "direction", direction);
    if (mode) {
        yyjson_mut_obj_add_str(doc, root, "mode", mode);
    }

    /* Edge types: explicit > mode-based > default */
    const char *edge_types[MCP_COL_16];
    int edge_type_count = 0;
    yyjson_doc *et_doc_keep = resolve_trace_edge_types(args, mode, edge_types, &edge_type_count);

    /* Run BFS for each requested direction.
     * IMPORTANT: yyjson_mut_obj_add_str borrows pointers — we must keep
     * traversal results alive until after yy_doc_to_str serialization. */
    bool do_outbound = strcmp(direction, "outbound") == 0 || strcmp(direction, "both") == 0;
    bool do_inbound = strcmp(direction, "inbound") == 0 || strcmp(direction, "both") == 0;

    cbm_traverse_result_t tr_out = {0};
    cbm_traverse_result_t tr_in = {0};

    bool data_flow = mode && strcmp(mode, "data_flow") == 0;
    (void)sel; /* union across all same-name nodes — see bfs_union_same_name (#546) */

    if (do_outbound) {
        bfs_union_same_name(store, nodes, node_count, "outbound", edge_types, edge_type_count,
                            depth, data_flow || include_evidence, &tr_out);
        yyjson_mut_obj_add_val(doc, root, "callees",
                               bfs_to_json_array(doc, &tr_out, risk_labels, include_tests,
                                                 data_flow, include_evidence));
    }
    if (do_inbound) {
        bfs_union_same_name(store, nodes, node_count, "inbound", edge_types, edge_type_count,
                            depth, data_flow || include_evidence, &tr_in);
        yyjson_mut_obj_add_val(doc, root, "callers",
                               bfs_to_json_array(doc, &tr_in, risk_labels, include_tests,
                                                 data_flow, include_evidence));
    }

    /* Serialize BEFORE freeing traversal results (yyjson borrows strings) */
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    /* Now safe to free traversal data */
    if (do_outbound) {
        cbm_store_traverse_free(&tr_out);
    }
    if (do_inbound) {
        cbm_store_traverse_free(&tr_in);
    }

    cbm_store_free_nodes(nodes, node_count);
    free(func_name);
    free(project);
    free(direction);
    free(mode);
    free(param_name);
    if (et_doc_keep) {
        yyjson_doc_free(et_doc_keep);
    }

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* ── Helper: free heap fields of a stack-allocated node ────────── */

static void free_node_contents(cbm_node_t *n) {
    safe_str_free(&n->project);
    safe_str_free(&n->label);
    safe_str_free(&n->name);
    safe_str_free(&n->qualified_name);
    safe_str_free(&n->file_path);
    safe_str_free(&n->properties_json);
    memset(n, 0, sizeof(*n));
}

/* ── Helper: read lines [start, end] from a file ─────────────── */

static char *read_file_lines(const char *path, int start, int end) {
    FILE *fp = cbm_fopen(path, "r");
    if (!fp) {
        return NULL;
    }

    size_t cap = CBM_SZ_4K;
    char *buf = malloc(cap);
    size_t len = 0;
    buf[0] = '\0';

    char line[CBM_SZ_2K];
    int lineno = 0;
    /* A line longer than the buffer arrives in several fgets() chunks; only the
     * first chunk starts a new line, so count lines by newline, not by chunk. */
    bool at_line_start = true;
    while (fgets(line, sizeof(line), fp)) {
        size_t ll = strlen(line);
        if (at_line_start) {
            lineno++;
        }
        at_line_start = ll > 0 && line[ll - SKIP_ONE] == '\n';
        if (lineno < start) {
            continue;
        }
        if (lineno > end) {
            break;
        }
        while (len + ll + SKIP_ONE > cap) {
            cap *= PAIR_LEN;
            buf = safe_realloc(buf, cap);
        }
        memcpy(buf + len, line, ll);
        len += ll;
        buf[len] = '\0';
    }

    (void)fclose(fp);
    if (len == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* ── Helper: get project root_path from store ─────────────────── */

static char *get_project_root(cbm_mcp_server_t *srv, const char *project) {
    if (!project) {
        return NULL;
    }
    char path[CBM_SZ_1K];
    if (cbm_zova_user_database_path(path, sizeof(path)) != 0) return NULL;
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, project);
    if (!repo) return NULL;
    cbm_project_t proj = {0};
    const char *workspace_id = cbm_zova_repository_workspace_id(repo);
    char *root = cbm_zova_repository_get_project(repo, workspace_id, &proj) == CBM_STORE_OK
                     ? cbm_heap_strdup(proj.root_path)
                     : NULL;
    safe_str_free(&proj.name);
    safe_str_free(&proj.indexed_at);
    safe_str_free(&proj.root_path);
    cbm_zova_repository_close(repo);
    return root;
}

/* ── index_repository ─────────────────────────────────────────── */

/* Cap on excluded dir paths listed in the response — keep it compact on large
 * repos (node_modules / vendor / etc. can produce many skip points). The full
 * count is still reported via "count" + "truncated". */
enum { INDEX_EXCLUDED_DIR_CAP = 25 };

/* Attach a compact summary of directory subtrees skipped during discovery (#411).
 * Shape: "excluded": {"dirs": [up to 25 rel-paths], "count": <total>, "truncated": <bool>}.
 * No-op when nothing was excluded. excluded_dirs[] is borrowed (copied into doc). */
static void add_excluded_summary(yyjson_mut_doc *doc, yyjson_mut_val *root, char **excluded_dirs,
                                 int excluded_count) {
    if (!excluded_dirs || excluded_count <= 0) {
        return;
    }
    yyjson_mut_val *excluded = yyjson_mut_obj(doc);
    yyjson_mut_val *dirs = yyjson_mut_arr(doc);
    int shown = excluded_count < INDEX_EXCLUDED_DIR_CAP ? excluded_count : INDEX_EXCLUDED_DIR_CAP;
    for (int i = 0; i < shown; i++) {
        if (excluded_dirs[i]) {
            yyjson_mut_arr_add_strcpy(doc, dirs, excluded_dirs[i]);
        }
    }
    yyjson_mut_obj_add_val(doc, excluded, "dirs", dirs);
    yyjson_mut_obj_add_int(doc, excluded, "count", excluded_count);
    yyjson_mut_obj_add_bool(doc, excluded, "truncated", excluded_count > INDEX_EXCLUDED_DIR_CAP);
    yyjson_mut_obj_add_val(doc, root, "excluded", excluded);
}

/* Cap on per-file skips embedded in the JSON response — keep it compact on
 * large repos. The FULL, uncapped list always goes to the per-run logfile;
 * the JSON carries "count" + "truncated" so nothing is silently hidden. */
enum { INDEX_SKIPPED_FILE_CAP = 50 };

/* Attach a summary of per-file skips (Stage 2 / Track B). Always emits a
 * top-level "skipped_count" (0 on clean runs) so consumers can rely on it.
 * When there are skips, also emits:
 *   "skipped": {"files":[{path,reason,phase}..(<=50)], "count":N, "truncated":bool}
 * and, if a per-run logfile was written, "logfile": "<path>".
 * Files the parser only partly understood (phase "parse_partial", or
 * "parse_unusable" when one range covers most of the file) were still indexed,
 * so they are reported apart as "parse_partial" with their error line ranges
 * and do not count as skipped.
 * The run status stays "indexed" — a skipped file is the expected handled
 * outcome, not a failure. errs[] is borrowed (copied into doc). */
static void add_skipped_summary(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                const cbm_file_error_t *errs, int count, const char *logfile) {
    int skipped_count = 0;
    int partial_count = 0;
    for (int i = 0; errs && i < count; i++) {
        if (cbm_pipeline_phase_is_partial(errs[i].phase)) {
            partial_count++;
        } else {
            skipped_count++;
        }
    }
    yyjson_mut_obj_add_int(doc, root, "skipped_count", skipped_count);
    if (!errs || count <= 0) {
        return;
    }
    for (int kind = 0; kind < 2; kind++) {
        bool partial = kind == 1;
        int total = partial ? partial_count : skipped_count;
        if (total == 0) {
            continue;
        }
        yyjson_mut_val *group = yyjson_mut_obj(doc);
        yyjson_mut_val *files = yyjson_mut_arr(doc);
        int shown = 0;
        for (int i = 0; i < count && shown < INDEX_SKIPPED_FILE_CAP; i++) {
            bool is_partial = cbm_pipeline_phase_is_partial(errs[i].phase);
            if (is_partial != partial) {
                continue;
            }
            yyjson_mut_val *fe = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, fe, "path", errs[i].path ? errs[i].path : "");
            if (partial) {
                yyjson_mut_obj_add_strcpy(doc, fe, "error_ranges",
                                          errs[i].reason ? errs[i].reason : "");
                if (strcmp(errs[i].phase, "parse_unusable") == 0) {
                    yyjson_mut_obj_add_bool(doc, fe, "parse_unusable", true);
                }
            } else {
                yyjson_mut_obj_add_strcpy(doc, fe, "reason", errs[i].reason ? errs[i].reason : "");
                yyjson_mut_obj_add_strcpy(doc, fe, "phase", errs[i].phase ? errs[i].phase : "");
            }
            yyjson_mut_arr_add_val(files, fe);
            shown++;
        }
        yyjson_mut_obj_add_val(doc, group, "files", files);
        yyjson_mut_obj_add_int(doc, group, "count", total);
        yyjson_mut_obj_add_bool(doc, group, "truncated", total > INDEX_SKIPPED_FILE_CAP);
        yyjson_mut_obj_add_val(doc, root, partial ? "parse_partial" : "skipped", group);
    }
    if (logfile && logfile[0]) {
        yyjson_mut_obj_add_strcpy(doc, root, "logfile", logfile);
    }
}

/* Write the FULL (uncapped) skip list to a per-run logfile — ONLY when >=1 file
 * was skipped (no logfile on a clean run). Location:
 *   $CBM_INDEX_LOG (override) else <cache_dir>/logs/<project>-<epoch>.log
 * Returns true and fills out_path on success. */
static bool write_skip_logfile(const char *project, const cbm_file_error_t *errs, int count,
                               char *out_path, size_t out_sz) {
    if (!errs || count <= 0) {
        return false;
    }
    char path[CBM_SZ_1K];
    const char *override = getenv("CBM_INDEX_LOG");
    if (override && override[0]) {
        snprintf(path, sizeof(path), "%s", override);
    } else {
        const char *cdir = cbm_resolve_cache_dir();
        if (!cdir) {
            return false;
        }
        char logdir[CBM_SZ_1K];
        snprintf(logdir, sizeof(logdir), "%s/logs", cdir);
        cbm_mkdir_p(logdir, 0755);
        snprintf(path, sizeof(path), "%s/%s-%lld.log", logdir, project ? project : "index",
                 (long long)time(NULL));
    }
    FILE *f = cbm_fopen(path, "wb");
    if (!f) {
        cbm_log_warn("index.logfile_open_fail", "path", path);
        return false;
    }
    (void)fprintf(f, "# codebase-memory-mcp index skip report\n");
    (void)fprintf(f, "# project=%s skipped=%d\n", project ? project : "", count);
    (void)fprintf(f, "# columns: phase\treason\tpath\n");
    for (int i = 0; i < count; i++) {
        (void)fprintf(f, "%s\t%s\t%s\n", errs[i].phase ? errs[i].phase : "",
                      errs[i].reason ? errs[i].reason : "", errs[i].path ? errs[i].path : "");
    }
    (void)fclose(f);
    if (out_path && out_sz) {
        snprintf(out_path, out_sz, "%s", path);
    }
    return true;
}

/* Build the success portion of the index_repository response.
 * Returns true when status should be "degraded" (#334 plausibility gate). */
static bool build_index_success_response(cbm_mcp_server_t *srv, yyjson_mut_doc *doc,
                                         yyjson_mut_val *root, const char *project_name,
                                         const char *repo_path, cbm_pipeline_t *p,
                                         char **excluded_dirs, int excluded_count,
                                         const cbm_file_error_t *file_errors, int file_error_count,
                                         const char *logfile) {
    add_excluded_summary(doc, root, excluded_dirs, excluded_count);
    add_skipped_summary(doc, root, file_errors, file_error_count, logfile);

    /* Discovery could not fully materialize the tree (bounded walk stack full
     * / allocation failure): the index covers only the files actually found,
     * so the run must not report a complete-looking "indexed" status (#17,
     * upstream 03dc9c91). */
    bool degraded = cbm_pipeline_discover_degraded(p);

    int exp_nodes = -1;
    int exp_edges = -1;
    cbm_pipeline_get_committed_counts(p, &exp_nodes, &exp_edges);

    const double ratio = cbm_dump_verify_min_ratio();
    const int min_floor = CBM_DUMP_VERIFY_MIN_FLOOR;

    int nodes = 0;
    int edges = 0;

    char zova_path[CBM_SZ_1K];
    cbm_zova_repository_t *repository = NULL;
    if (cbm_zova_user_database_path(zova_path, sizeof(zova_path)) != 0 ||
        !(repository = cbm_zova_repository_open(zova_path, project_name))) {
        degraded = true;
    } else {
        const char *workspace_id = cbm_zova_repository_workspace_id(repository);
        if (cbm_zova_repository_counts(repository, workspace_id, &nodes, &edges) != CBM_STORE_OK ||
            nodes < 0 || edges < 0) {
            nodes = 0;
            edges = 0;
            degraded = true;
        }
        /* The published generation's coverage, not just this run's: an
         * incremental run re-parses only changed files, so its own skip list
         * would make older gaps look healed. */
        add_coverage_summary(doc, root, repository, workspace_id);
        cbm_zova_repository_close(repository);
    }
    if (!degraded)
        degraded = cbm_dump_verify_is_degraded(exp_nodes, nodes, ratio, min_floor);

    yyjson_mut_obj_add_int(doc, root, "nodes", nodes);
    yyjson_mut_obj_add_int(doc, root, "edges", edges);
    if (exp_nodes >= 0) {
        yyjson_mut_obj_add_int(doc, root, "expected_nodes", exp_nodes);
        yyjson_mut_obj_add_int(doc, root, "expected_edges", exp_edges);
    }

    if (degraded) {
        if (cbm_pipeline_discover_degraded(p)) {
            yyjson_mut_obj_add_str(
                doc, root, "hint",
                "File discovery was incomplete (bounded walk budget or allocation "
                "failure) — the index covers only the files that were found. "
                "Re-run index_repository(repo_path=...) to rebuild.");
            cbm_log_warn("dump.verify", "reason", "discover_degraded");
        } else {
            yyjson_mut_obj_add_str(
                doc, root, "hint",
                "Committed shared Zova workspace failed verification. "
                "Re-run index_repository(repo_path=...) to rebuild.");
            cbm_log_warn("dump.verify", "reason", "zova_workspace_verification_failed",
                         "expected_nodes", exp_nodes >= 0 ? "set" : "unknown");
        }
    }

    return degraded;
}

/* Build the response for a worker that crashed/hung/failed without producing a
 * result. The crash is already contained (this process survived); we report it
 * rather than dying. Precise skip-and-continue (quarantine the culprit, index the
 * rest) is layered on in the probe stage. */
static char *build_worker_failure_response(const char *args, cbm_proc_outcome_t outcome) {
    char *repo_path = cbm_mcp_get_string_arg(args, "repo_path");
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "error");
    yyjson_mut_obj_add_str(doc, root, "outcome", cbm_proc_outcome_str(outcome));
    yyjson_mut_obj_add_str(
        doc, root, "hint",
        outcome == CBM_PROC_HANG
            ? "Indexing worker timed out (a file made no progress). The worker was "
              "terminated and the server survived. Re-run to retry."
            : "Indexing worker crashed on a file. The crash was contained (the server "
              "survived). Re-run to retry; a future release isolates the culprit file.");
    if (repo_path) {
        yyjson_mut_obj_add_strcpy(doc, root, "repo_path", repo_path);
    }
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(repo_path);
    char *result = cbm_mcp_text_result(json, true);
    free(json);
    return result;
}

/* Drop the cached store so the next query reopens whatever the worker published
 * to cbm.zova. NULL-safe: the background watcher path (main.c) has no MCP server
 * / cached store — the child publishes and the parent only needs the return
 * code, so there is nothing to invalidate. */
static void supervisor_invalidate_store(cbm_mcp_server_t *srv) {
    if (!srv) {
        return;
    }
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
        srv->store = NULL;
    }
    free(srv->current_project);
    srv->current_project = NULL;
}

/* Resolve a per-supervisor-run temp path <cache_dir>/logs/.supervisor-<pid><suffix>
 * (falls back to the CWD if the cache dir is unresolvable). Used for the crash-
 * attribution marker and the quarantine list during the recovery re-run. */
static void supervisor_tmp_path(char *out, size_t out_sz, const char *suffix) {
    const char *cdir = cbm_resolve_cache_dir();
    if (cdir && cdir[0]) {
        char logdir[CBM_SZ_1K];
        snprintf(logdir, sizeof(logdir), "%s/logs", cdir);
        cbm_mkdir_p(logdir, 0755);
        snprintf(out, out_sz, "%s/.supervisor-%d%s", logdir, (int)getpid(), suffix);
    } else {
        snprintf(out, out_sz, ".supervisor-%d%s", (int)getpid(), suffix);
    }
}

/* Parse the worker's marker JOURNAL ("S <rel>" / "D <rel>" lines, one event
 * per line — see cbm_index_mark_start/done) into the crash/hang SUSPECT set:
 * files whose last event is an S with no closing D, i.e. the in-flight set
 * at kill time. Recovery runs are PARALLEL, so there are up to worker_count
 * suspects; a torn final line (no trailing newline) is discarded by design.
 * Returns a malloc'd array of malloc'd rel paths, OLDEST OPEN S FIRST (for a
 * hang, the oldest still-open file IS the stuck one). Caller frees via
 * supervisor_free_suspects. */
static char **supervisor_read_suspects(const char *path, int *out_n) {
    *out_n = 0;
    FILE *f = cbm_fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char **open_paths = NULL; /* open (S-without-D) files in first-S order */
    int open_n = 0;
    int open_cap = 0;
    char line[CBM_SZ_1K];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len == 0 || line[len - 1] != '\n') {
            break; /* torn final line — discard and stop */
        }
        line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') {
            line[--len] = '\0';
        }
        if (len < 3 || (line[0] != 'S' && line[0] != 'D') || line[1] != ' ') {
            continue;
        }
        const char *rel = line + 2;
        if (line[0] == 'S') {
            bool already = false;
            for (int i = 0; i < open_n && !already; i++) {
                already = strcmp(open_paths[i], rel) == 0;
            }
            if (already) {
                continue;
            }
            if (open_n == open_cap) {
                int ncap = open_cap ? open_cap * 2 : 16;
                char **np = (char **)realloc(open_paths, (size_t)ncap * sizeof(char *));
                if (!np) {
                    break;
                }
                open_paths = np;
                open_cap = ncap;
            }
            open_paths[open_n++] = cbm_strdup(rel);
        } else {
            for (int i = 0; i < open_n; i++) {
                if (strcmp(open_paths[i], rel) == 0) {
                    free(open_paths[i]);
                    memmove(&open_paths[i], &open_paths[i + 1],
                            (size_t)(open_n - i - 1) * sizeof(char *));
                    open_n--;
                    break;
                }
            }
        }
    }
    (void)fclose(f);
    if (open_n == 0) {
        free(open_paths);
        return NULL;
    }
    *out_n = open_n;
    return open_paths;
}

static void supervisor_free_suspects(char **s, int n) {
    if (!s) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(s[i]);
    }
    free(s);
}

static bool supervisor_suspect_contains(char **s, int n, const char *rel) {
    for (int i = 0; i < n; i++) {
        if (s[i] && strcmp(s[i], rel) == 0) {
            return true;
        }
    }
    return false;
}

/* Append one quarantine entry "rel\tphase\n" (phase = "crash"|"hang") to the
 * quarantine list. The worker's loader parses this back and reports the skip's
 * phase in skipped[]; a bare "rel" line is still tolerated there (defaults crash). */
static bool supervisor_append_quarantine(const char *path, const char *rel, const char *phase) {
    FILE *f = cbm_fopen(path, "ab");
    if (!f) {
        return false;
    }
    (void)fprintf(f, "%s\t%s\n", rel, phase);
    (void)fclose(f);
    return true;
}

/* How many times a failed index worker may be re-run, as
 * CBM_INDEX_MAX_RESTARTS sets it. Default 100; 0 means no restarts. An
 * unreadable value keeps the default and is logged (upstream 6b801eec). */
static int index_restart_cap(void) {
    enum { INDEX_RESTART_CAP_DEFAULT = 100 };
    long v = 0;
    if (!cbm_env_long("CBM_INDEX_MAX_RESTARTS", &v)) {
        char raw[CBM_SZ_64] = {0};
        if (cbm_safe_getenv("CBM_INDEX_MAX_RESTARTS", raw, sizeof(raw), NULL) && raw[0]) {
            cbm_log_warn("index.restart_cap.ignored", "value", raw, "action", "using_default");
        }
        return INDEX_RESTART_CAP_DEFAULT;
    }
    if (v < 0 || v > INT_MAX) {
        cbm_log_warn("index.restart_cap.out_of_range", "action", "using_default");
        return INDEX_RESTART_CAP_DEFAULT;
    }
    return (int)v;
}

int cbm_index_restart_cap_for_testing(void) {
    return index_restart_cap();
}

/* Run index_repository in a supervised worker subprocess with skip-and-continue
 * (Stage 3c). Returns the response string (caller frees):
 *   - the worker's own response on a clean first run (the common path);
 *   - after a crash/hang, the response from a clean single-threaded RECOVERY run
 *     that quarantines the culprit file(s) — status="indexed" with them listed in
 *     skipped[] as phase="crash"/"hang", and the good files indexed;
 *   - a best-effort PARTIAL index (one final quarantine-only run) if the recovery
 *     loop cannot converge but at least one file was quarantined;
 *   - a contained-failure response only if even that cannot produce a clean run.
 * Returns NULL only when the worker could not be spawned at all, so the caller
 * degrades to the in-process path. */
static char *index_run_supervised_impl(cbm_mcp_server_t *srv, const char *args) {
    supervisor_invalidate_store(srv);

    /* First attempt: normal parallel run. */
    cbm_index_worker_result_t wr;
    int rc = cbm_index_spawn_worker(args, false, NULL, NULL, &wr);

    if (rc != 0 || wr.outcome == CBM_PROC_SPAWN_FAILED) {
        cbm_index_worker_result_free(&wr);
        supervisor_invalidate_store(srv);
        return NULL; /* degrade to in-process */
    }
    if (wr.outcome == CBM_PROC_EXIT_NONZERO && wr.response) {
        /* The worker failed gracefully and said why (e.g. over the memory
         * budget). That is not a crash: report it rather than re-running. */
        char *resp = wr.response;
        wr.response = NULL;
        cbm_index_worker_result_free(&wr);
        supervisor_invalidate_store(srv);
        return resp;
    }
    if (wr.outcome == CBM_PROC_CLEAN) {
        /* Clean exit → transfer the worker's response (the common path). If the
         * worker exited clean but wrote no response (a degenerate case, e.g. a
         * self binary that does not act as an index worker), resp is NULL and the
         * caller degrades to the in-process path — a clean run never needs the
         * crash-recovery loop. */
        char *resp = wr.response; /* transfer ownership to caller (may be NULL) */
        wr.response = NULL;
        cbm_index_worker_result_free(&wr);
        supervisor_invalidate_store(srv);
        return resp;
    }

    /* Crash / hang / nonzero exit → skip-and-continue recovery. Re-run the
     * worker PARALLEL (there are no sequential production runs) with the
     * per-file marker JOURNAL armed; after each failed run the journal's
     * open-S set is the in-flight SUSPECT set. A file is quarantined only
     * when it appears in the suspect sets of TWO CONSECUTIVE failed runs
     * (intersection — a stale or merely unlucky in-flight file rotates out),
     * and only ONE file per round: the OLDEST open S in the intersection
     * (for a hang the oldest still-open file IS the stuck one; for a crash
     * it is the longest-running suspect — the best single deterministic
     * pick). A clean run then indexes the good files and reports the
     * quarantined ones as phase="crash"/"hang" skips via the ordinary
     * Stage-2 skip plumbing. The old design re-ran SINGLE-THREADED to keep
     * one exact marker; at scale that fell into the sequential crawl, went
     * quiet, was killed as a hang mid-pass, and the stale marker got FOUR
     * innocent ms-typescript fixtures quarantined one 15-minute retry at a
     * time. */
    cbm_proc_outcome_t last_outcome = wr.outcome;
    cbm_index_worker_result_free(&wr);

    char marker_path[CBM_SZ_1K];
    char quarantine_path[CBM_SZ_1K];
    supervisor_tmp_path(marker_path, sizeof(marker_path), ".marker");
    supervisor_tmp_path(quarantine_path, sizeof(quarantine_path), ".quarantine");
    (void)remove(marker_path);
    /* Start the quarantine list empty (truncate any stale file). */
    FILE *qinit = cbm_fopen(quarantine_path, "wb");
    if (qinit) {
        (void)fclose(qinit);
    }

    int cap = index_restart_cap();

    char *resp = NULL;
    int quarantined = 0;         /* files pinned + added to the quarantine list so far */
    char **prev_suspects = NULL; /* previous failed round's in-flight set */
    int prev_n = 0;
    for (int i = 0; i < cap; i++) {
        cbm_index_worker_result_t wr2;
        int rc2 = cbm_index_spawn_worker(args, /*single_thread=*/false, marker_path,
                                         quarantine_path, &wr2);
        if (rc2 != 0) {
            last_outcome = wr2.outcome;
            cbm_index_worker_result_free(&wr2);
            break; /* spawn failed mid-recovery — give up */
        }
        if ((wr2.outcome == CBM_PROC_CLEAN || wr2.outcome == CBM_PROC_EXIT_NONZERO) &&
            wr2.response) {
            resp = wr2.response; /* transfer ownership to caller */
            wr2.response = NULL;
            cbm_index_worker_result_free(&wr2);
            break; /* good files indexed; quarantined files reported as crash/hang */
        }
        if (wr2.outcome == CBM_PROC_CRASH || wr2.outcome == CBM_PROC_HANG) {
            last_outcome = wr2.outcome;
            cbm_index_worker_result_free(&wr2);
            /* crash vs hang: the phase this file is quarantined under and
             * reported as in skipped[]. A fault signal → "crash"; a
             * no-progress kill → "hang". */
            const char *phase = (last_outcome == CBM_PROC_HANG) ? "hang" : "crash";
            int sus_n = 0;
            char **suspects = supervisor_read_suspects(marker_path, &sus_n);
            (void)remove(marker_path); /* fresh journal for the next re-run */
            if (!suspects || sus_n == 0) {
                supervisor_free_suspects(suspects, sus_n);
                cbm_log_warn("index.supervisor.unattributable", "action", "give_up");
                break;
            }
            if (prev_suspects) {
                /* Two-consecutive-strikes: quarantine the OLDEST open S that
                 * was also in flight in the previous failed round. */
                const char *pick = NULL;
                for (int k = 0; k < sus_n && !pick; k++) {
                    if (supervisor_suspect_contains(prev_suspects, prev_n, suspects[k])) {
                        pick = suspects[k];
                    }
                }
                if (!pick) {
                    /* Disjoint consecutive in-flight sets: the failure is not
                     * attributable to a recurring file (systemic) — stop
                     * rather than quarantine an innocent. */
                    supervisor_free_suspects(suspects, sus_n);
                    cbm_log_warn("index.supervisor.unattributable", "action", "give_up");
                    break;
                }
                if (!supervisor_append_quarantine(quarantine_path, pick, phase)) {
                    cbm_log_warn("index.supervisor.quarantine_write_fail", "path", pick);
                    supervisor_free_suspects(suspects, sus_n);
                    break;
                }
                quarantined++;
                char attempt_buf[MCP_FIELD_SIZE];
                snprintf(attempt_buf, sizeof(attempt_buf), "%d", i + 1);
                cbm_log_warn("index.file_quarantined", "path", pick, "outcome", phase, "attempt",
                             attempt_buf);
            }
            supervisor_free_suspects(prev_suspects, prev_n);
            prev_suspects = suspects;
            prev_n = sus_n;
            continue;
        }
        /* SPAWN_FAILED / nonzero exit / non-fault kill → not a crash we can
         * attribute; stop and report a contained failure. */
        last_outcome = wr2.outcome;
        cbm_index_worker_result_free(&wr2);
        break;
    }
    supervisor_free_suspects(prev_suspects, prev_n);

    (void)remove(marker_path); /* marker no longer needed */

    /* Terminal best-effort-partial: the loop exited WITHOUT a clean run (cap
     * exhausted, or an unattributable failure) but at least one file was already
     * quarantined. Try ONE final PARALLEL spawn with the accumulated quarantine
     * and NO marker — every known-bad file short-circuits, so a clean run yields
     * a PARTIAL index (all good files indexed, all known crashers/hangs reported
     * as skips) rather than a hard failure. Bounded by the same quiet-timeout,
     * so it cannot itself hang. Rare given monotonic progress. */
    if (!resp && quarantined > 0) {
        cbm_index_worker_result_t wrp;
        int rcp =
            cbm_index_spawn_worker(args, /*single_thread=*/false, NULL, quarantine_path, &wrp);
        if (rcp == 0 && wrp.outcome == CBM_PROC_CLEAN && wrp.response) {
            resp = wrp.response; /* transfer ownership to caller */
            wrp.response = NULL;
            char qn[MCP_FIELD_SIZE];
            snprintf(qn, sizeof(qn), "%d", quarantined);
            cbm_log_error("index.supervisor.partial", "quarantined", qn, "outcome",
                          cbm_proc_outcome_str(last_outcome));
        }
        cbm_index_worker_result_free(&wrp);
    }

    (void)remove(quarantine_path);
    supervisor_invalidate_store(srv);

    if (resp) {
        return resp;
    }
    return build_worker_failure_response(args, last_outcome);
}

void cbm_index_job_queue_enter(cbm_mcp_server_t *srv) {
    if (!srv) return;
    cbm_mutex_lock(&srv->index_queue_mutex);
    uint64_t ticket = srv->index_queue_next_ticket++;
    atomic_fetch_add_explicit(&srv->index_queue_waiters, 1, memory_order_relaxed);
    while (ticket != srv->index_queue_serving_ticket) {
        cbm_mutex_unlock(&srv->index_queue_mutex);
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
        cbm_nanosleep(&pause, NULL);
        cbm_mutex_lock(&srv->index_queue_mutex);
    }
    atomic_fetch_sub_explicit(&srv->index_queue_waiters, 1, memory_order_relaxed);
    cbm_mutex_unlock(&srv->index_queue_mutex);
}

void cbm_index_job_queue_leave(cbm_mcp_server_t *srv) {
    if (!srv) return;
    cbm_mutex_lock(&srv->index_queue_mutex);
    srv->index_queue_serving_ticket++;
    cbm_mutex_unlock(&srv->index_queue_mutex);
}

uint64_t cbm_index_job_queue_waiter_count(const cbm_mcp_server_t *srv) {
    return srv ? atomic_load_explicit(&srv->index_queue_waiters, memory_order_relaxed) : 0;
}

static char *index_run_supervised(cbm_mcp_server_t *srv, const char *args) {
    if (srv) cbm_index_job_queue_enter(srv);
    char *result = index_run_supervised_impl(srv, args);
    if (srv) cbm_index_job_queue_leave(srv);
    return result;
}

/* Build a minimal {"repo_path": "<root>"} args object (path safely escaped) and
 * run it through index_run_supervised. Shared by the session auto-index (srv
 * present → its cached store is invalidated) and the watcher re-index (srv NULL).
 * Returns the worker's response string (caller frees) or NULL to degrade. */
static char *index_run_supervised_path(cbm_mcp_server_t *srv, const char *root_path) {
    if (!root_path || !root_path[0]) {
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "repo_path", root_path);
    char *args = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    if (!args) {
        return NULL;
    }
    char *resp = index_run_supervised(srv, args);
    free(args);
    return resp;
}

/* Public entry (see mcp.h): the watcher re-index in main.c has no MCP server, so
 * it reaches the supervised runner through this srv-less wrapper. */
char *cbm_mcp_index_run_supervised_path(const char *root_path) {
    return index_run_supervised_path(NULL, root_path);
}

bool cbm_path_within_root(const char *root_path, const char *abs_path); /* defined below */

/* Index hold for root_path (ADR-0001): the preparation barrier shared, then the
 * workspace lease. The root is canonicalized the way handle_index_repository
 * canonicalizes repo_path, so the lease names the workspace the run publishes. */
cbm_zova_coord_status_t cbm_mcp_index_hold_acquire(const char *root_path, int64_t wait_ms,
                                                   cbm_zova_index_hold_t *out,
                                                   char *out_workspace_id,
                                                   size_t out_workspace_id_size) {
    char *root = root_path ? cbm_heap_strdup(root_path) : NULL;
    if (!root) {
        if (out) memset(out, 0, sizeof(*out));
        return CBM_ZOVA_COORD_ERROR;
    }
    cbm_normalize_path_sep(root);
    root = canonicalize_repo_path_if_exists(root);
    cbm_zova_coord_status_t status =
        cbm_zova_index_hold_acquire(root, wait_ms, out, out_workspace_id, out_workspace_id_size);
    free(root);
    return status;
}

const char *cbm_mcp_index_hold_refusal_reason(cbm_zova_coord_status_t status,
                                              const cbm_zova_index_hold_t *hold) {
    switch (status) {
    case CBM_ZOVA_COORD_OK:
        return "ok";
    case CBM_ZOVA_COORD_CANCELLED:
        return "cancelled";
    case CBM_ZOVA_COORD_BUSY:
        return hold && hold->barrier_busy ? "database_replacement_in_progress" : "workspace_busy";
    default:
        return "coordination_unavailable";
    }
}

/* Machine-readable refusal, in the BUSY + reason shape of the Zova operations. */
static char *index_hold_refusal(cbm_zova_coord_status_t status, const cbm_zova_index_hold_t *hold,
                                const char *workspace_id) {
    const char *reason = cbm_mcp_index_hold_refusal_reason(status, hold);
    const char *hint =
        status == CBM_ZOVA_COORD_ERROR
            ? "Cannot use the coordination lock file next to cbm.zova; check the cache "
              "directory permissions."
        : status == CBM_ZOVA_COORD_CANCELLED ? "The server is shutting down."
        : strcmp(reason, "workspace_busy") == 0
            ? "Another process is indexing this workspace. Retry when it finishes; "
              "CBM_INDEX_LEASE_TIMEOUT_S sets how long a request waits."
            : "A restore, import or repack is replacing cbm.zova. Retry when it finishes.";
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", status == CBM_ZOVA_COORD_ERROR ? "error" : "busy");
    yyjson_mut_obj_add_str(doc, root, "reason", reason);
    if (workspace_id && workspace_id[0]) {
        yyjson_mut_obj_add_strcpy(doc, root, "workspace_id", workspace_id);
    }
    yyjson_mut_obj_add_str(doc, root, "hint", hint);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json ? json : "{\"status\":\"busy\"}", true);
    free(json);
    return result;
}

static char *index_repository_run(cbm_mcp_server_t *srv, const char *args);

/* The process that received the request holds the index hold for the whole
 * run, including a supervised worker's life. The worker itself never takes it:
 * POSIX record locks are not inherited, so it would conflict with its parent. */
static char *handle_index_repository(cbm_mcp_server_t *srv, const char *args) {
    if (cbm_index_worker_active()) {
        return index_repository_run(srv, args);
    }
    char *repo_path = cbm_mcp_get_string_arg(args, "repo_path");
    if (!repo_path) {
        repo_path = cbm_mcp_get_string_arg(args, "path");
    }
    if (!repo_path) {
        return index_repository_run(srv, args); /* reports the missing field */
    }
    cbm_zova_index_hold_t hold;
    char workspace_id[CBM_SZ_128] = {0};
    cbm_zova_coord_status_t status = cbm_mcp_index_hold_acquire(
        repo_path, cbm_zova_coord_default_wait_ms(), &hold, workspace_id, sizeof(workspace_id));
    free(repo_path);
    if (status != CBM_ZOVA_COORD_OK) {
        cbm_log_warn("index.hold.refused", "reason",
                     cbm_mcp_index_hold_refusal_reason(status, &hold), "workspace", workspace_id);
        return index_hold_refusal(status, &hold, workspace_id);
    }
    char *result = index_repository_run(srv, args);
    cbm_zova_index_hold_release(&hold);
    return result;
}

static char *index_repository_run(cbm_mcp_server_t *srv, const char *args) {
    /* Supervisor gate: run the index in a crash/hang-isolating worker subprocess
     * unless this process IS the worker or the kill switch (CBM_INDEX_SUPERVISOR=0)
     * is set. On spawn failure, fall through to the in-process path (degrade). */
    if (cbm_index_supervisor_should_wrap()) {
        char *supervised = index_run_supervised(srv, args);
        if (supervised) {
            return supervised;
        }
    }

    char *repo_path = cbm_mcp_get_string_arg(args, "repo_path");
    if (!repo_path) {
        repo_path = cbm_mcp_get_string_arg(args, "path");
    }
    char *mode_str = cbm_mcp_get_string_arg(args, "mode");
    char *name_override = cbm_mcp_get_string_arg(args, "name");

    if (!repo_path) {
        free(mode_str);
        free(name_override);
        return cbm_mcp_text_result("required field: repo_path; alias accepted: path; example: "
                                   "{\"repo_path\":\"/absolute/path/to/repo\"}",
                                   true);
    }
    cbm_normalize_path_sep(repo_path);

    repo_path = canonicalize_repo_path_if_exists(repo_path);

    /* Workspace boundary (upstream 7fa5b077, #17): cbm_mcp_check_index_root
     * enforces CBM_ALLOWED_ROOT. Unset by default, so the standard "index the
     * path I gave you" behaviour is unchanged. */
    const char *boundary_reason = NULL;
    char canonbuf[CBM_SZ_4K];
    if (cbm_mcp_check_index_root(repo_path, canonbuf, sizeof(canonbuf), &boundary_reason) != 0) {
        free(mode_str);
        free(name_override);
        free(repo_path);
        return cbm_mcp_text_result(
            boundary_reason ? boundary_reason : "repo_path is outside the allowed root", true);
    }

    cbm_index_mode_t mode = CBM_MODE_FULL;
    if (mode_str && strcmp(mode_str, "fast") == 0) {
        mode = CBM_MODE_FAST;
    } else if (mode_str && strcmp(mode_str, "moderate") == 0) {
        mode = CBM_MODE_MODERATE;
    }
    free(mode_str);

    cbm_pipeline_t *p = cbm_pipeline_new(repo_path, NULL, mode);
    if (!p) {
        free(name_override);
        free(repo_path);
        return cbm_mcp_text_result("failed to create pipeline", true);
    }
    if (name_override && name_override[0] && !cbm_pipeline_set_project_name(p, name_override)) {
        cbm_pipeline_free(p);
        free(name_override);
        free(repo_path);
        return cbm_mcp_text_result("invalid project name", true);
    }
    free(name_override);

    char *project_name = cbm_heap_strdup(cbm_pipeline_project_name(p));

    /* Close cached store — the pipeline republishes the project to cbm.zova */
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
        srv->store = NULL;
    }
    free(srv->current_project);
    srv->current_project = NULL;

    /* Same-workspace exclusion is the workspace lease held by
     * handle_index_repository. Track active pipeline so signal handler and
     * notifications/cancelled can cancel it mid-run. */
    srv->active_pipeline = p;
    int rc = cbm_pipeline_run(p);
    srv->active_pipeline = NULL;

    /* Capture the excluded-subtree list (#411) while the pipeline (which owns
     * the strings) is still alive — the response builder copies them into the
     * JSON doc, so they need only outlive that call, not cbm_pipeline_free. */
    char **excluded_dirs = NULL;
    int excluded_count = 0;
    cbm_pipeline_get_excluded(p, &excluded_dirs, &excluded_count);

    /* Capture the per-file skip list (Stage 2 / Track B) while the pipeline
     * still owns the strings; the response builder copies them into the doc. */
    cbm_file_error_t *file_errors = NULL;
    int file_error_count = 0;
    cbm_pipeline_get_file_errors(p, &file_errors, &file_error_count);

    cbm_mem_collect(); /* return mimalloc pages to OS after large indexing */

    /* Invalidate cached store so next query reopens the fresh database */
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
        srv->store = NULL;
    }
    free(srv->current_project);
    srv->current_project = NULL;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "project", project_name);

    if (rc == 0) {
        /* Write the per-run logfile ONLY when there were skips (no logfile on a
         * clean run). The FULL list goes to the file; the JSON caps at 50. */
        char logfile_path[CBM_SZ_1K];
        logfile_path[0] = '\0';
        bool has_logfile = write_skip_logfile(project_name, file_errors, file_error_count,
                                              logfile_path, sizeof(logfile_path));
        bool degraded = build_index_success_response(
            srv, doc, root, project_name, repo_path, p, excluded_dirs, excluded_count,
            file_errors, file_error_count, has_logfile ? logfile_path : NULL);
        yyjson_mut_obj_add_str(doc, root, "status", degraded ? "degraded" : "indexed");
        if (cbm_pipeline_had_format_migration(p)) {
            yyjson_mut_obj_add_bool(doc, root, "format_migration", true);
        }
    } else if (rc == CBM_PIPELINE_ABORT_OVER_BUDGET) {
        /* Resident memory stayed above the budget after back-pressure and one
         * confirmation cycle, so the run stopped before publication. Name the
         * cause, the numbers and the fact that the previous index still
         * serves; the generic "check repo_path" hint below would mislead. */
        int budget_mb = (int)(cbm_mem_budget() / (1024 * 1024));
        int peak_rss_mb = (int)(cbm_mem_peak_rss() / (1024 * 1024));
        char budget_text[CBM_SZ_32];
        char peak_text[CBM_SZ_32];
        (void)snprintf(budget_text, sizeof(budget_text), "%d", budget_mb);
        (void)snprintf(peak_text, sizeof(peak_text), "%d", peak_rss_mb);
        cbm_log_error("index.abort", "reason", "over_memory_budget", "project", project_name,
                      "budget_mb", budget_text, "peak_rss_mb", peak_text);
        yyjson_mut_obj_add_str(doc, root, "status", "error");
        yyjson_mut_obj_add_str(doc, root, "reason", "over_memory_budget");
        yyjson_mut_obj_add_str(doc, root, "previous_index", "preserved");
        yyjson_mut_obj_add_int(doc, root, "budget_mb", budget_mb);
        yyjson_mut_obj_add_int(doc, root, "peak_rss_mb", peak_rss_mb);
        yyjson_mut_obj_add_str(doc, root, "hint",
                               cbm_mem_budget_explicit()
                                   ? "Indexing stopped: resident memory stayed above "
                                     "CBM_MEM_BUDGET_MB after backpressure; no partial graph was "
                                     "published and the previous index still serves. Raise "
                                     "CBM_MEM_BUDGET_MB or exclude large subtrees."
                                   : "Indexing stopped: resident memory stayed above the detected "
                                     "RAM budget after backpressure; no partial graph was "
                                     "published and the previous index still serves. Raise "
                                     "CBM_MEM_BUDGET_MB, lower CBM_WORKERS, or exclude large "
                                     "subtrees.");
    } else {
        yyjson_mut_obj_add_str(doc, root, "status", "error");
        yyjson_mut_obj_add_str(doc, root, "hint",
                               "Pipeline failed. Check repo_path exists and contains source files. "
                               "Try mode='fast' for a quicker diagnostic run.");
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    /* Free the pipeline only after the response doc copied the excluded list.
     * Supervised worker: skip the deep free — the process exits right after
     * handing over the response (main.c fast-exits), and piecemeal-freeing a
     * multi-GB graph before process death costs minutes on kernel-scale repos;
     * the OS reclaims it wholesale at exit. In-process paths (tests, kill
     * switch, degrade) still free normally. */
    if (cbm_index_worker_active()) {
        cbm_log_info("index.worker.fast_exit", "skip", "pipeline_free");
    } else {
        cbm_pipeline_free(p);
    }
    free(project_name);
    free(repo_path);

    char *result = cbm_mcp_text_result(json, rc != 0);
    free(json);
    return result;
}

/* ── get_code_snippet ─────────────────────────────────────────── */

/* Copy a node from an array into a heap-allocated standalone node. */
static void copy_node(const cbm_node_t *src, cbm_node_t *dst) {
    dst->id = src->id;
    dst->project = cbm_heap_strdup(src->project);
    dst->label = cbm_heap_strdup(src->label);
    dst->name = cbm_heap_strdup(src->name);
    dst->qualified_name = cbm_heap_strdup(src->qualified_name);
    dst->file_path = cbm_heap_strdup(src->file_path);
    dst->start_line = src->start_line;
    dst->end_line = src->end_line;
    dst->properties_json = src->properties_json ? cbm_heap_strdup(src->properties_json) : NULL;
}

/* Build a JSON suggestions response for ambiguous or fuzzy results. */
static int compare_snippet_suggestions(const void *left, const void *right) {
    const cbm_node_t *a = left;
    const cbm_node_t *b = right;
    const char *a_qn = a->qualified_name ? a->qualified_name : "";
    const char *b_qn = b->qualified_name ? b->qualified_name : "";
    int cmp = strcmp(a_qn, b_qn);
    if (cmp == 0)
        cmp = strcmp(a->file_path ? a->file_path : "", b->file_path ? b->file_path : "");
    if (cmp == 0)
        cmp = strcmp(a->label ? a->label : "", b->label ? b->label : "");
    return cmp;
}

static char *snippet_suggestions(const char *input, cbm_node_t *nodes, int count) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "status", "ambiguous");

    char msg[CBM_SZ_512];
    snprintf(msg, sizeof(msg),
             "%d matches for \"%s\". Pick a qualified_name from suggestions below, "
             "pass file_path to choose by file, or use search_graph(name_pattern=\"...\") "
             "to narrow results.",
             count, input);
    yyjson_mut_obj_add_str(doc, root, "message", msg);

    if (count > 1) {
        qsort(nodes, (size_t)count, sizeof(*nodes), compare_snippet_suggestions);
    }
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *s = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, s, "qualified_name",
                               nodes[i].qualified_name ? nodes[i].qualified_name : "");
        yyjson_mut_obj_add_str(doc, s, "name", nodes[i].name ? nodes[i].name : "");
        yyjson_mut_obj_add_str(doc, s, "label", nodes[i].label ? nodes[i].label : "");
        yyjson_mut_obj_add_str(doc, s, "file_path", nodes[i].file_path ? nodes[i].file_path : "");
        yyjson_mut_arr_append(arr, s);
    }
    yyjson_mut_obj_add_val(doc, root, "suggestions", arr);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* Enrich a mutable JSON object with key-value pairs from a node's properties_json.
 * Returns the parsed yyjson_doc (caller frees AFTER serialization — zero-copy). */
static yyjson_doc *enrich_node_properties(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                                          const char *properties_json) {
    if (!properties_json || properties_json[0] == '\0') {
        return NULL;
    }
    yyjson_doc *props_doc = yyjson_read(properties_json, strlen(properties_json), 0);
    if (!props_doc) {
        return NULL;
    }
    yyjson_val *props_root = yyjson_doc_get_root(props_doc);
    if (!props_root || !yyjson_is_obj(props_root)) {
        yyjson_doc_free(props_doc);
        return NULL;
    }
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(props_root, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
        yyjson_val *val = yyjson_obj_iter_get_val(key);
        const char *k = yyjson_get_str(key);
        if (!k) {
            continue;
        }
        if (yyjson_is_str(val)) {
            yyjson_mut_obj_add_str(doc, obj, k, yyjson_get_str(val));
        } else if (yyjson_is_bool(val)) {
            yyjson_mut_obj_add_bool(doc, obj, k, yyjson_get_bool(val));
        } else if (yyjson_is_int(val)) {
            yyjson_mut_obj_add_int(doc, obj, k, yyjson_get_int(val));
        } else if (yyjson_is_real(val)) {
            yyjson_mut_obj_add_real(doc, obj, k, yyjson_get_real(val));
        } else if (yyjson_is_arr(val) || yyjson_is_obj(val)) {
            yyjson_mut_val *copy = yyjson_val_mut_copy(doc, val);
            if (copy) {
                yyjson_mut_obj_add_val(doc, obj, k, copy);
            }
        }
    }
    return props_doc; /* caller frees after serialization */
}

/* Resolve an absolute path from root_path + file_path, verify containment,
 * and read source lines. Sets *out_abs_path (caller frees). Returns source
 * string (caller frees) or NULL if path is invalid/unreadable. */
/* True only when abs_path, after realpath/_fullpath resolution (which collapses
 * `..` and resolves symlinks/junctions), stays within root_path. This is the
 * single containment guard every MCP file-read sink must pass before reading a
 * file into a tool response: both the snippet path (resolve_snippet_source) and
 * the search path (attach_result_source) route through it, so a result whose
 * indexed path escapes the project root — via a `..` segment, or a symlink /
 * Windows junction picked up during discovery — is never read back out. */
bool cbm_path_within_root(const char *root_path, const char *abs_path) {
    if (!root_path || !abs_path) {
        return false;
    }
    char real_root[CBM_SZ_4K];
    char real_file[CBM_SZ_4K];
#ifdef _WIN32
    if (_fullpath(real_root, root_path, sizeof(real_root)) &&
        _fullpath(real_file, abs_path, sizeof(real_file))) {
        cbm_normalize_path_sep(real_root);
        cbm_normalize_path_sep(real_file);
#else
    if (realpath(root_path, real_root) && realpath(abs_path, real_file)) {
#endif
        size_t root_len = strlen(real_root);
        if (strncmp(real_file, real_root, root_len) == 0 &&
            (real_file[root_len] == '/' || real_file[root_len] == '\0')) {
            return true;
        }
    }
    return false;
}

int cbm_mcp_check_index_root(const char *repo_path, char *canonical_out, size_t canonical_sz,
                             const char **out_reason) {
    if (out_reason) {
        *out_reason = NULL;
    }
    /* Function scope: `resolved` may point here until the end. */
    char real[CBM_SZ_4K];
    const char *resolved = repo_path;
    if (repo_path && repo_path[0]) {
#ifdef _WIN32
        if (_fullpath(real, repo_path, sizeof(real))) {
            cbm_normalize_path_sep(real);
            resolved = real;
        }
#else
        if (realpath(repo_path, real)) {
            resolved = real;
        }
#endif
    }
    if (canonical_out && canonical_sz > 0) {
        (void)snprintf(canonical_out, canonical_sz, "%s", resolved ? resolved : "");
    }
    if (!repo_path || !repo_path[0]) {
        /* Missing path is handled by the caller (a required-field error), not
         * a boundary decision. */
        return 0;
    }
    const char *allowed_root = getenv("CBM_ALLOWED_ROOT");
    if (allowed_root && allowed_root[0] && !cbm_path_within_root(allowed_root, resolved)) {
        if (out_reason) {
            /* Keep the exact wording the existing MCP fixture matches, with
             * guidance appended (upstream 7fa5b077): a boundary is about scope
             * inside an allowed root, not sensitivity. */
            *out_reason =
                "repo_path is outside the allowed root; set CBM_ALLOWED_ROOT so the index "
                "root is contained within it";
        }
        return -1;
    }
    return 0;
}

static char *resolve_snippet_source(const char *root_path, const char *file_path, int start,
                                    int end, char **out_abs_path) {
    *out_abs_path = NULL;
    if (!root_path || !file_path) {
        return NULL;
    }
    size_t apsz = strlen(root_path) + strlen(file_path) + MCP_SEPARATOR;
    char *abs_path = malloc(apsz);
    snprintf(abs_path, apsz, "%s/%s", root_path, file_path);

    *out_abs_path = abs_path;
    if (cbm_path_within_root(root_path, abs_path)) {
        return read_file_lines(abs_path, start, end);
    }
    return NULL;
}

static bool utf8_is_cont(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

static char *sanitize_utf8_lossy(const char *s) {
    enum {
        UTF8_REPLACEMENT_LEN = 3,
        UTF8_THREE_BYTE_LEN = 3,
        UTF8_FOUR_BYTE_LEN = 4,
        UTF8_FOURTH_BYTE = 3,
    };
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    if (len > (((size_t)-1) - SKIP_ONE) / UTF8_REPLACEMENT_LEN) {
        return NULL;
    }
    char *out = malloc(len * UTF8_REPLACEMENT_LEN + SKIP_ONE);
    if (!out) {
        return NULL;
    }

    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + len;
    unsigned char *dst = (unsigned char *)out;
    while (p < end) {
        unsigned char c = *p;
        size_t n = 0;
        if (c < 0x80) {
            n = 1;
        } else if (c >= 0xC2 && c <= 0xDF && p + 1 < end && utf8_is_cont(p[1])) {
            n = 2;
        } else if (c == 0xE0 && p + 2 < end && p[1] >= 0xA0 && p[1] <= 0xBF && utf8_is_cont(p[2])) {
            n = UTF8_THREE_BYTE_LEN;
        } else if (c >= 0xE1 && c <= 0xEC && p + 2 < end && utf8_is_cont(p[1]) &&
                   utf8_is_cont(p[2])) {
            n = UTF8_THREE_BYTE_LEN;
        } else if (c == 0xED && p + 2 < end && p[1] >= 0x80 && p[1] <= 0x9F && utf8_is_cont(p[2])) {
            n = UTF8_THREE_BYTE_LEN;
        } else if (c >= 0xEE && c <= 0xEF && p + 2 < end && utf8_is_cont(p[1]) &&
                   utf8_is_cont(p[2])) {
            n = UTF8_THREE_BYTE_LEN;
        } else if (c == 0xF0 && p + UTF8_FOURTH_BYTE < end && p[1] >= 0x90 && p[1] <= 0xBF &&
                   utf8_is_cont(p[2]) && utf8_is_cont(p[UTF8_FOURTH_BYTE])) {
            n = UTF8_FOUR_BYTE_LEN;
        } else if (c >= 0xF1 && c <= 0xF3 && p + UTF8_FOURTH_BYTE < end && utf8_is_cont(p[1]) &&
                   utf8_is_cont(p[2]) && utf8_is_cont(p[UTF8_FOURTH_BYTE])) {
            n = UTF8_FOUR_BYTE_LEN;
        } else if (c == 0xF4 && p + UTF8_FOURTH_BYTE < end && p[1] >= 0x80 && p[1] <= 0x8F &&
                   utf8_is_cont(p[2]) && utf8_is_cont(p[UTF8_FOURTH_BYTE])) {
            n = UTF8_FOUR_BYTE_LEN;
        }

        if (n > 0) {
            memcpy(dst, p, n);
            dst += n;
            p += n;
        } else {
            *dst++ = 0xEF;
            *dst++ = 0xBF;
            *dst++ = 0xBD;
            p++;
        }
    }
    *dst = '\0';
    return out;
}

/* Build an enriched snippet response for a resolved node. */
/* Add a string array to a JSON object (no-op if count == 0). */
static void add_string_array(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                             char **strings, int count) {
    if (count <= 0) {
        return;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_arr_add_str(doc, arr, strings[i]);
    }
    yyjson_mut_obj_add_val(doc, obj, key, arr);
}

static char *build_snippet_response(cbm_mcp_server_t *srv, cbm_node_t *node,
                                    const char *match_method, bool include_neighbors,
                                    cbm_node_t *alternatives, int alt_count) {
    char *root_path = get_project_root(srv, node->project);

    int start = node->start_line > 0 ? node->start_line : SKIP_ONE;
    int end = node->end_line > start ? node->end_line : start + SNIPPET_DEFAULT_LINES;
    char *abs_path = NULL;
    char *source = resolve_snippet_source(root_path, node->file_path, start, end, &abs_path);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    yyjson_mut_obj_add_str(doc, root_obj, "name", node->name ? node->name : "");
    yyjson_mut_obj_add_str(doc, root_obj, "qualified_name",
                           node->qualified_name ? node->qualified_name : "");
    yyjson_mut_obj_add_str(doc, root_obj, "label", node->label ? node->label : "");

    const char *display_path = "";
    if (abs_path) {
        display_path = abs_path;
    } else if (node->file_path) {
        display_path = node->file_path;
    }
    yyjson_mut_obj_add_str(doc, root_obj, "file_path", display_path);
    yyjson_mut_obj_add_int(doc, root_obj, "start_line", start);
    yyjson_mut_obj_add_int(doc, root_obj, "end_line", end);

    if (source) {
        char *safe_source = sanitize_utf8_lossy(source);
        if (safe_source) {
            yyjson_mut_obj_add_strcpy(doc, root_obj, "source", safe_source);
            free(safe_source);
        } else {
            yyjson_mut_obj_add_str(doc, root_obj, "source", "(source not available)");
        }
    } else {
        yyjson_mut_obj_add_str(doc, root_obj, "source", "(source not available)");
    }

    /* match_method — omitted for exact matches */
    if (match_method) {
        yyjson_mut_obj_add_str(doc, root_obj, "match_method", match_method);
    }

    /* coverage_note — only when the source file has a recorded parse gap */
    char *coverage_note = NULL;
    char db_path[CBM_SZ_1K];
    cbm_zova_repository_t *repo = NULL;
    if (srv->current_project && cbm_zova_user_database_path(db_path, sizeof(db_path)) == 0 &&
        (repo = cbm_zova_repository_open(db_path, srv->current_project))) {
        coverage_note = coverage_snippet_note(repo, cbm_zova_repository_workspace_id(repo),
                                              node->file_path);
        cbm_zova_repository_close(repo);
    }
    if (coverage_note) {
        yyjson_mut_obj_add_strcpy(doc, root_obj, "coverage_note", coverage_note);
        free(coverage_note);
    }

    /* Enrich with node properties (freed AFTER serialization — zero-copy). */
    yyjson_doc *props_doc = enrich_node_properties(doc, root_obj, node->properties_json);

    /* Caller/callee counts — store already resolved by calling handler */
    cbm_store_t *store = srv->store;
    int in_deg = 0;
    int out_deg = 0;
    cbm_store_node_degree(store, node->id, &in_deg, &out_deg);
    yyjson_mut_obj_add_int(doc, root_obj, "callers", in_deg);
    yyjson_mut_obj_add_int(doc, root_obj, "callees", out_deg);

    char **nb_callers = NULL;
    int nb_caller_count = 0;
    char **nb_callees = NULL;
    int nb_callee_count = 0;
    if (include_neighbors) {
        cbm_store_node_neighbor_names(store, node->id, MCP_DEFAULT_LIMIT, &nb_callers,
                                      &nb_caller_count, &nb_callees, &nb_callee_count);
        add_string_array(doc, root_obj, "caller_names", nb_callers, nb_caller_count);
        add_string_array(doc, root_obj, "callee_names", nb_callees, nb_callee_count);
    }

    /* Alternatives (when auto-resolved from ambiguous) */
    if (alternatives && alt_count > 0) {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        for (int i = 0; i < alt_count; i++) {
            yyjson_mut_val *a = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, a, "qualified_name",
                                   alternatives[i].qualified_name ? alternatives[i].qualified_name
                                                                  : "");
            yyjson_mut_obj_add_str(doc, a, "file_path",
                                   alternatives[i].file_path ? alternatives[i].file_path : "");
            yyjson_mut_arr_append(arr, a);
        }
        yyjson_mut_obj_add_val(doc, root_obj, "alternatives", arr);
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(props_doc); /* safe if NULL */
    for (int i = 0; i < nb_caller_count; i++) {
        free(nb_callers[i]);
    }
    for (int i = 0; i < nb_callee_count; i++) {
        free(nb_callees[i]);
    }
    free(nb_callers);
    free(nb_callees);
    free(root_path);
    free(abs_path);
    free(source);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* ── get_file_outline ─────────────────────────────────────────── */

#define MCP_FILE_OUTLINE_OUTPUT_MAX ((size_t)2U * 1024U * 1024U)

static char *file_outline_error(char *project, yyjson_doc *args_doc, const char *msg) {
    free(project);
    yyjson_doc_free(args_doc);
    return cbm_mcp_text_result(msg, true);
}

static char *handle_get_file_outline(cbm_mcp_server_t *srv, const char *args) {
    const char *args_text = args ? args : "{}";
    yyjson_doc *args_doc = yyjson_read(args_text, strlen(args_text), 0);
    yyjson_val *root = args_doc ? yyjson_doc_get_root(args_doc) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        return file_outline_error(NULL, args_doc,
                                  "get_file_outline arguments must be a JSON object");
    }

    char *project = get_project_arg(args_text);
    if (!project) {
        return file_outline_error(NULL, args_doc, "project is required");
    }
    yyjson_val *file_value = yyjson_obj_get(root, "file_path");
    const char *file_path = yyjson_is_str(file_value) ? yyjson_get_str(file_value) : NULL;
    if (!file_path || !file_path[0]) {
        return file_outline_error(project, args_doc, "file_path is required");
    }
    char normalized_path[CBM_SZ_4K];
    if (coverage_normalize_rel(file_path, false, normalized_path, sizeof(normalized_path)) !=
        COVERAGE_PATH_OK) {
        return file_outline_error(project, args_doc,
                                  "file_path must be a repository-relative path without '..'");
    }

    int limit = 100;
    yyjson_val *limit_value = yyjson_obj_get(root, "limit");
    if (limit_value) {
        int64_t parsed = yyjson_is_int(limit_value) ? yyjson_get_sint(limit_value) : 0;
        if (parsed < 1 || parsed > CBM_STORE_FILE_OUTLINE_MAX_LIMIT) {
            return file_outline_error(project, args_doc,
                                      "limit must be an integer between 1 and 200");
        }
        limit = (int)parsed;
    }
    int offset = 0;
    yyjson_val *offset_value = yyjson_obj_get(root, "offset");
    if (offset_value) {
        int64_t parsed = yyjson_is_int(offset_value) ? yyjson_get_sint(offset_value) : -1;
        if (parsed < 0 || parsed > INT_MAX) {
            return file_outline_error(project, args_doc,
                                      "offset must be a non-negative integer");
        }
        offset = (int)parsed;
    }

    const char *labels[CBM_STORE_FILE_OUTLINE_MAX_LABELS];
    int label_count = 0;
    yyjson_val *labels_value = yyjson_obj_get(root, "labels");
    if (labels_value) {
        if (!yyjson_is_arr(labels_value) ||
            yyjson_arr_size(labels_value) > CBM_STORE_FILE_OUTLINE_MAX_LABELS) {
            return file_outline_error(project, args_doc,
                                      "labels must be an array of at most 16 strings");
        }
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *item;
        yyjson_arr_foreach(labels_value, idx, max, item) {
            const char *label = yyjson_is_str(item) ? yyjson_get_str(item) : NULL;
            if (!label || !label[0] || strlen(label) >= CBM_SZ_128) {
                return file_outline_error(project, args_doc,
                                          "labels must contain only non-empty bounded strings");
            }
            labels[label_count++] = label;
        }
    }

    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        char *error = build_project_list_error("project not found or not indexed");
        char *result = cbm_mcp_text_result(error, true);
        free(error);
        free(project);
        yyjson_doc_free(args_doc);
        return result;
    }
    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(project);
        yyjson_doc_free(args_doc);
        return not_indexed;
    }

    cbm_file_outline_row_t *rows = NULL;
    int row_count = 0;
    int total = 0;
    int rc = cbm_store_get_file_outline(store, project, normalized_path, labels, label_count,
                                        limit, offset, &rows, &row_count, &total);
    if (rc != CBM_STORE_OK) {
        return file_outline_error(project, args_doc,
                                  rc == CBM_STORE_SCAN_LIMIT
                                      ? "get_file_outline exceeded its fixed output safety limit"
                                      : "get_file_outline query failed");
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    yyjson_mut_obj_add_str(doc, obj, "file_path", normalized_path);
    yyjson_mut_val *json_rows = yyjson_mut_arr(doc);
    for (int i = 0; i < row_count; i++) {
        char lines[CBM_SZ_32];
        if (rows[i].start_line > 0) {
            snprintf(lines, sizeof(lines), "%d-%d", rows[i].start_line,
                     rows[i].end_line > rows[i].start_line ? rows[i].end_line
                                                           : rows[i].start_line);
        } else {
            lines[0] = '\0';
        }
        yyjson_mut_val *row = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, row, "name", rows[i].name);
        yyjson_mut_obj_add_strcpy(doc, row, "label", rows[i].label);
        yyjson_mut_obj_add_strcpy(doc, row, "lines", lines);
        yyjson_mut_obj_add_strcpy(doc, row, "qn", rows[i].qualified_name);
        yyjson_mut_arr_add_val(json_rows, row);
    }
    yyjson_mut_obj_add_val(doc, obj, "results", json_rows);
    yyjson_mut_obj_add_int(doc, obj, "total", total);
    yyjson_mut_obj_add_int(doc, obj, "offset", offset);
    yyjson_mut_obj_add_int(doc, obj, "limit", limit);
    yyjson_mut_obj_add_int(doc, obj, "returned", row_count);
    yyjson_mut_obj_add_bool(doc, obj, "has_more", (int64_t)offset + row_count < total);
    char *payload = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_store_free_file_outline(rows, row_count);
    if (!payload) {
        return file_outline_error(project, args_doc, "get_file_outline output allocation failed");
    }
    if (strlen(payload) > MCP_FILE_OUTLINE_OUTPUT_MAX) {
        free(payload);
        return file_outline_error(project, args_doc,
                                  "get_file_outline exceeded its fixed output safety limit");
    }
    free(project);
    yyjson_doc_free(args_doc);
    char *result = cbm_mcp_text_result(payload, false);
    free(payload);
    return result;
}

static char *handle_get_code_snippet(cbm_mcp_server_t *srv, const char *args) {
    char *qn = cbm_mcp_get_string_arg(args, "qualified_name");
    char *project = get_project_arg(args);
    bool include_neighbors = cbm_mcp_get_bool_arg(args, "include_neighbors");

    if (!qn) {
        free(project);
        return cbm_mcp_text_result("qualified_name is required", true);
    }

    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(qn);
        free(project);
        return _res;
    }

    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(qn);
        free(project);
        return not_indexed;
    }

    /* Default to current project (same as all other tools) */
    const char *effective_project = project ? project : srv->current_project;

    /* Tier 1: Exact QN match */
    cbm_node_t node = {0};
    int rc = cbm_store_find_node_by_qn(store, effective_project, qn, &node);
    if (rc == CBM_STORE_OK) {
        char *result = build_snippet_response(srv, &node, NULL, include_neighbors, NULL, 0);
        free_node_contents(&node);
        free(qn);
        free(project);
        return result;
    }

    /* Tier 2: Suffix match — handles partial QNs ("main.HandleRequest")
     * and short names ("ProcessOrder") via LIKE '%.X'. */
    cbm_node_t *suffix_nodes = NULL;
    int suffix_count = 0;
    cbm_store_find_nodes_by_qn_suffix(store, effective_project, qn, &suffix_nodes, &suffix_count);

    /* Tier 3: the C-macro namespace. A macro's QN is `<module>.<NAME>#macro`
     * (CBM_MACRO_QN_SUFFIX), out of reach of '%.X'. Tried only when no
     * definition answered to the name above (tie rule), for a short name, a
     * partial QN, or the macro's plain QN. */
    if (suffix_count == 0) {
        char macro_qn[CBM_SZ_512];
        int macro_len = snprintf(macro_qn, sizeof(macro_qn), "%s" CBM_MACRO_QN_SUFFIX, qn);
        if (macro_len > 0 && (size_t)macro_len < sizeof(macro_qn)) {
            cbm_store_free_nodes(suffix_nodes, suffix_count);
            suffix_nodes = NULL;
            cbm_store_find_nodes_by_qn_suffix(store, effective_project, macro_qn, &suffix_nodes,
                                              &suffix_count);
        }
    }

    char *file_filter = cbm_mcp_get_string_arg(args, "file_path");
    if (file_filter && suffix_count > 0) {
        suffix_count = filter_nodes_by_file(suffix_nodes, suffix_count, file_filter);
        if (suffix_count == 0) {
            char msg[CBM_SZ_512];
            snprintf(msg, sizeof(msg),
                     "symbol %s is not defined in file_path %s. Omit file_path to list every "
                     "match.",
                     qn, file_filter);
            free(file_filter);
            cbm_store_free_nodes(suffix_nodes, 0);
            free(qn);
            free(project);
            return cbm_mcp_text_result(msg, true);
        }
    }
    free(file_filter);

    if (suffix_count == SKIP_ONE) {
        copy_node(&suffix_nodes[0], &node);
        cbm_store_free_nodes(suffix_nodes, suffix_count);
        char *result = build_snippet_response(srv, &node, "suffix", include_neighbors, NULL, 0);
        free_node_contents(&node);
        free(qn);
        free(project);
        return result;
    }

    if (suffix_count > SKIP_ONE) {
        /* Prefer the real definition (a .c body over a .h declaration, a Function
         * over a Module) so an unambiguous-by-preference match resolves directly
         * instead of forcing a disambiguation round trip; only a genuine tie still
         * returns suggestions. */
        bool snip_ambiguous = false;
        int ssel = pick_resolved_node(suffix_nodes, suffix_count, &snip_ambiguous);
        if (!snip_ambiguous) {
            copy_node(&suffix_nodes[ssel], &node);
            cbm_store_free_nodes(suffix_nodes, suffix_count);
            char *result = build_snippet_response(srv, &node, "suffix", include_neighbors, NULL, 0);
            free_node_contents(&node);
            free(qn);
            free(project);
            return result;
        }
        char *result = snippet_suggestions(qn, suffix_nodes, suffix_count);
        cbm_store_free_nodes(suffix_nodes, suffix_count);
        free(qn);
        free(project);
        return result;
    }

    cbm_store_free_nodes(suffix_nodes, suffix_count);
    free(qn);
    free(project);

    /* Nothing found — guide the caller toward search_graph */
    return cbm_mcp_text_result(
        "symbol not found. Use search_graph(name_pattern=\"...\") first to discover "
        "the exact qualified_name, then pass it to get_code_snippet.",
        true);
}

/* ── search_code v2: graph-augmented code search ─────────────── */

/* Strip non-ASCII bytes to guarantee valid UTF-8 JSON output */
enum { ASCII_MAX = 127 };
static void sanitize_ascii(char *s) {
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (*p > ASCII_MAX) {
            *p = '?';
        }
    }
}

/* Intermediate grep match */
typedef struct {
    char file[CBM_SZ_512];
    int line;
    char content[CBM_SZ_1K];
} grep_match_t;

/* Deduped result: one per containing graph node */
typedef struct {
    int64_t node_id; /* 0 = raw match (no containing node) */
    char node_name[CBM_SZ_256];
    char qualified_name[CBM_SZ_512];
    char label[CBM_SZ_64];
    char file[CBM_SZ_512];
    int start_line;
    int end_line;
    int in_degree;
    int out_degree;
    int score;
    int match_lines[CBM_SZ_64];
    int match_count;
} search_result_t;

/* Score a result for ranking: project source first, vendored last, tests lowest */
enum { SCORE_FUNC = 10, SCORE_ROUTE = 15, SCORE_VENDORED = -50, SCORE_TEST = -5 };
enum { MAX_LINE_SPAN = 999999 };

static int compute_search_score(const search_result_t *r) {
    int score = r->in_degree;
    if (strcmp(r->label, "Function") == 0 || strcmp(r->label, "Method") == 0) {
        score += SCORE_FUNC;
    }
    if (strcmp(r->label, "Route") == 0) {
        score += SCORE_ROUTE;
    }
    if (strstr(r->file, "vendored/") || strstr(r->file, "vendor/") ||
        strstr(r->file, "node_modules/")) {
        score += SCORE_VENDORED;
    }
    /* Penalize test files */
    if (strstr(r->file, "test") || strstr(r->file, "spec") || strstr(r->file, "_test.")) {
        score += SCORE_TEST;
    }
    return score;
}

static int search_result_cmp(const void *a, const void *b) {
    const search_result_t *ra = (const search_result_t *)a;
    const search_result_t *rb = (const search_result_t *)b;
    return rb->score - ra->score; /* descending */
}

/* Build the grep/search command string based on scoped vs recursive mode.
 * On Windows, the PowerShell Select-String script (tab-delimited output),
 * run as `powershell -Command <script>`. It holds no double quote, so it
 * stays one argument through cbm_build_win_cmdline.
 * On POSIX, a grep sh command with colon-delimited output. */
static void build_grep_cmd(char *cmd, size_t cmd_sz, bool use_regex, bool scoped,
                           const char *file_pattern, const char *tmpfile, const char *filelist,
                           const char *root_path) {
#ifdef _WIN32
    const char *sm = use_regex ? "" : " -SimpleMatch";
    if (scoped) {
        if (file_pattern) {
            snprintf(
                cmd, cmd_sz,
                "$pat = Get-Content '%s'; "
                "Get-Content '%s' | ForEach-Object { Select-String -LiteralPath $_ -Pattern $pat%s "
                "-ErrorAction SilentlyContinue }"
                " | Where-Object { $_.Path -like '*%s' }"
                " | ForEach-Object { $_.Path + [char]9 + $_.LineNumber + [char]9 + $_.Line }",
                tmpfile, filelist, sm, file_pattern);
        } else {
            snprintf(
                cmd, cmd_sz,
                "$pat = Get-Content '%s'; "
                "Get-Content '%s' | ForEach-Object { Select-String -LiteralPath $_ -Pattern $pat%s "
                "-ErrorAction SilentlyContinue }"
                " | ForEach-Object { $_.Path + [char]9 + $_.LineNumber + [char]9 + $_.Line }",
                tmpfile, filelist, sm);
        }
    } else {
        if (file_pattern) {
            snprintf(
                cmd, cmd_sz,
                "Get-ChildItem -Recurse -Path '%s\\*' -Include '%s' -File "
                "-ErrorAction SilentlyContinue"
                " | Select-String -Pattern (Get-Content '%s')%s -ErrorAction SilentlyContinue"
                " | ForEach-Object { $_.Path + [char]9 + $_.LineNumber + [char]9 + $_.Line }",
                root_path, file_pattern, tmpfile, sm);
        } else {
            snprintf(
                cmd, cmd_sz,
                "Get-ChildItem -Recurse -Path '%s\\*' -File -ErrorAction "
                "SilentlyContinue"
                " | Select-String -Pattern (Get-Content '%s')%s -ErrorAction SilentlyContinue"
                " | ForEach-Object { $_.Path + [char]9 + $_.LineNumber + [char]9 + $_.Line }",
                root_path, tmpfile, sm);
        }
    }
#else
    const char *flag = use_regex ? "-E" : "-F";
    if (scoped) {
        /* -0: read NUL-separated paths from the filelist so paths containing
         * spaces stay one argument (issue #687). Pairs with the NUL separator
         * written by write_scoped_filelist. file_pattern was already applied to
         * that list in C (upstream 7a1e069b): BusyBox grep — the static
         * Alpine/musl runtime — has no GNU --include option. */
        snprintf(cmd, cmd_sz, "xargs -0 grep -Hn %s -f '%s' < '%s' 2>/dev/null", flag, tmpfile,
                 filelist);
    } else {
        if (file_pattern) {
            snprintf(cmd, cmd_sz, "grep -rn %s --include='%s' -f '%s' '%s' 2>/dev/null", flag,
                     file_pattern, tmpfile, root_path);
        } else {
            snprintf(cmd, cmd_sz, "grep -rn %s -f '%s' '%s' 2>/dev/null", flag, tmpfile, root_path);
        }
    }
#endif
}

/* Build deduplicated file list from search results + raw matches. */
static yyjson_mut_val *build_dedup_files_array(yyjson_mut_doc *doc, search_result_t *sr,
                                               int output_count, grep_match_t *raw, int raw_count) {
    yyjson_mut_val *files_arr = yyjson_mut_arr(doc);
    char *seen_files[CBM_SZ_512];
    int seen_count = 0;
    for (int fi = 0; fi < output_count; fi++) {
        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen_files[j], sr[fi].file) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup && seen_count < CBM_SZ_512) {
            seen_files[seen_count++] = sr[fi].file;
            yyjson_mut_arr_add_str(doc, files_arr, sr[fi].file);
        }
    }
    for (int fi = 0; fi < raw_count && seen_count < CBM_SZ_512; fi++) {
        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen_files[j], raw[fi].file) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            seen_files[seen_count++] = raw[fi].file;
            yyjson_mut_arr_add_str(doc, files_arr, raw[fi].file);
        }
    }
    return files_arr;
}

/* Attach source or context lines to a search result JSON item. */
static void attach_result_source(yyjson_mut_doc *doc, yyjson_mut_val *item, search_result_t *r,
                                 int mode, int context_lines, const char *root_path) {
    enum { MODE_FULL = 1 };
    if (r->start_line <= 0 || r->end_line <= 0) {
        return;
    }
    char abs_path[CBM_SZ_1K];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", root_path, r->file);

    /* Containment: a search result whose indexed path resolves outside the
     * project root (a `..` segment, or a symlink/junction that discovery
     * followed) must not be read back into the response. Same guard the
     * snippet path already uses. */
    if (!cbm_path_within_root(root_path, abs_path)) {
        return;
    }

    if (mode == MODE_FULL) {
        char *source = read_file_lines(abs_path, r->start_line, r->end_line);
        if (source) {
            sanitize_ascii(source);
            yyjson_mut_obj_add_strcpy(doc, item, "source", source);
            free(source);
        }
    } else if (context_lines > 0 && r->match_count > 0) {
        int ctx_start = r->match_lines[0] - context_lines;
        int ctx_end = r->match_lines[r->match_count - SKIP_ONE] + context_lines;
        if (ctx_start < SKIP_ONE) {
            ctx_start = SKIP_ONE;
        }
        char *ctx = read_file_lines(abs_path, ctx_start, ctx_end);
        if (ctx) {
            sanitize_ascii(ctx);
            yyjson_mut_obj_add_strcpy(doc, item, "context", ctx);
            yyjson_mut_obj_add_int(doc, item, "context_start", ctx_start);
            free(ctx);
        }
    }
}

/* Build directory distribution object from search results (top-level dir → count). */
static yyjson_mut_val *build_dir_distribution(yyjson_mut_doc *doc, search_result_t *sr,
                                              int sr_count) {
    yyjson_mut_val *dirs = yyjson_mut_obj(doc);
    char dir_names[CBM_SZ_64][CBM_SZ_128];
    int dir_counts[CBM_SZ_64];
    int dir_n = 0;
    for (int di = 0; di < sr_count; di++) {
        char top[CBM_SZ_128] = "";
        const char *slash = strchr(sr[di].file, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - sr[di].file + SKIP_ONE);
            if (dlen >= sizeof(top)) {
                dlen = sizeof(top) - SKIP_ONE;
            }
            memcpy(top, sr[di].file, dlen);
            top[dlen] = '\0';
        } else {
            snprintf(top, sizeof(top), "%s", sr[di].file);
        }
        int found = CBM_NOT_FOUND;
        for (int d = 0; d < dir_n; d++) {
            if (strcmp(dir_names[d], top) == 0) {
                found = d;
                break;
            }
        }
        if (found >= 0) {
            dir_counts[found]++;
        } else if (dir_n < CBM_SZ_64) {
            snprintf(dir_names[dir_n], sizeof(dir_names[0]), "%s", top);
            dir_counts[dir_n] = SKIP_ONE;
            dir_n++;
        }
    }
    for (int d = 0; d < dir_n; d++) {
        yyjson_mut_val *key = yyjson_mut_strcpy(doc, dir_names[d]);
        yyjson_mut_val *val = yyjson_mut_int(doc, dir_counts[d]);
        yyjson_mut_obj_add(dirs, key, val);
    }
    return dirs;
}

/* Phase 4: assemble JSON output from search results */
static char *assemble_search_output(search_result_t *sr, int sr_count, grep_match_t *raw,
                                    int raw_count, int gm_count, int limit, int mode,
                                    int context_lines, const char *root_path,
                                    bool warn_literal_pipe, uint64_t elapsed_ms) {
    enum { MODE_COMPACT = 0, MODE_FULL = 1, MODE_FILES = 2, SEARCH_SLOW_MS = 5000 };

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    int output_count = sr_count < limit ? sr_count : limit;

    if (mode == MODE_FILES) {
        yyjson_mut_obj_add_val(doc, root_obj, "files",
                               build_dedup_files_array(doc, sr, output_count, raw, raw_count));
    } else {
        yyjson_mut_val *results_arr = yyjson_mut_arr(doc);
        for (int ri = 0; ri < output_count; ri++) {
            search_result_t *r = &sr[ri];
            yyjson_mut_val *item = yyjson_mut_obj(doc);

            yyjson_mut_obj_add_str(doc, item, "node", r->node_name);
            yyjson_mut_obj_add_str(doc, item, "qualified_name", r->qualified_name);
            yyjson_mut_obj_add_str(doc, item, "label", r->label);
            yyjson_mut_obj_add_str(doc, item, "file", r->file);
            yyjson_mut_obj_add_int(doc, item, "start_line", r->start_line);
            yyjson_mut_obj_add_int(doc, item, "end_line", r->end_line);
            yyjson_mut_obj_add_int(doc, item, "in_degree", r->in_degree);
            yyjson_mut_obj_add_int(doc, item, "out_degree", r->out_degree);

            yyjson_mut_val *ml = yyjson_mut_arr(doc);
            for (int j = 0; j < r->match_count; j++) {
                yyjson_mut_arr_add_int(doc, ml, r->match_lines[j]);
            }
            yyjson_mut_obj_add_val(doc, item, "match_lines", ml);
            attach_result_source(doc, item, r, mode, context_lines, root_path);
            yyjson_mut_arr_add_val(results_arr, item);
        }
        yyjson_mut_obj_add_val(doc, root_obj, "results", results_arr);

        enum { MAX_RAW = 20 };
        yyjson_mut_val *raw_arr = yyjson_mut_arr(doc);
        int raw_output = raw_count < MAX_RAW ? raw_count : MAX_RAW;
        for (int ri = 0; ri < raw_output; ri++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "file", raw[ri].file);
            yyjson_mut_obj_add_int(doc, item, "line", raw[ri].line);
            yyjson_mut_obj_add_str(doc, item, "content", raw[ri].content);
            yyjson_mut_arr_add_val(raw_arr, item);
        }
        yyjson_mut_obj_add_val(doc, root_obj, "raw_matches", raw_arr);
    }

    yyjson_mut_obj_add_val(doc, root_obj, "directories", build_dir_distribution(doc, sr, sr_count));

    /* Summary stats */
    yyjson_mut_obj_add_int(doc, root_obj, "total_grep_matches", gm_count);
    yyjson_mut_obj_add_int(doc, root_obj, "total_results", sr_count);
    yyjson_mut_obj_add_int(doc, root_obj, "raw_match_count", raw_count);
    yyjson_mut_obj_add_int(doc, root_obj, "elapsed_ms", (int)elapsed_ms);
    if (sr_count > 0 && gm_count > 0) {
        char ratio[CBM_SZ_32];
        snprintf(ratio, sizeof(ratio), "%.1fx", (double)gm_count / (double)(sr_count + raw_count));
        yyjson_mut_obj_add_strcpy(doc, root_obj, "dedup_ratio", ratio);
    }

    /* Warnings: surface common foot-guns instead of leaving them silent. */
    yyjson_mut_val *warnings = yyjson_mut_arr(doc);
    if (warn_literal_pipe) {
        yyjson_mut_arr_add_strcpy(
            doc, warnings,
            "pattern contains '|' but regex=false, so it is matched literally (not as "
            "alternation). Pass regex=true for 'foo|bar' to mean 'foo OR bar'.");
    }
    if (elapsed_ms >= SEARCH_SLOW_MS) {
        char slow[CBM_SZ_128];
        snprintf(slow, sizeof(slow),
                 "search took %dms (>%ds); narrow file_pattern/path_filter or use a more "
                 "specific pattern",
                 (int)elapsed_ms, SEARCH_SLOW_MS / 1000);
        yyjson_mut_arr_add_strcpy(doc, warnings, slow);
        char ems[CBM_SZ_32];
        snprintf(ems, sizeof(ems), "%d", (int)elapsed_ms);
        cbm_log_warn("search.slow", "elapsed_ms", ems); /* visibility in logs */
    }
    if (yyjson_mut_arr_size(warnings) > 0) {
        yyjson_mut_obj_add_val(doc, root_obj, "warnings", warnings);
    }

    char *json = yy_doc_to_str(doc);
    if (json) {
        sanitize_ascii(json);
    }
    yyjson_mut_doc_free(doc);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* Read grep output from fp, parse file:line:content format, apply path filter,
 * and return a dynamically-allocated grep_match_t array. */
/* Strip root path prefix from a file path. */
static const char *strip_root_prefix(const char *path, const char *root, size_t root_len) {
    if (strncmp(path, root, root_len) != 0) {
        return path;
    }
    const char *p = path + root_len;
    if (*p == '/') {
        p++;
    }
    return p;
}

static grep_match_t *collect_grep_matches(FILE *fp, const char *root_path, size_t root_len,
                                          bool has_path_filter, cbm_regex_t *path_regex,
                                          int grep_limit, int *out_count) {
    int gm_cap = CBM_SZ_64;
    int gm_count = 0;
    grep_match_t *gm = malloc(gm_cap * sizeof(grep_match_t));
    char line[CBM_SZ_2K];
    /* grep prints "path:line:content"; a match line longer than the buffer
     * arrives in several chunks. Parse only the first chunk of each line and
     * skip the rest, so a continuation is never read as a new match. */
    bool at_line_start = true;

    while (fgets(line, sizeof(line), fp) && gm_count < grep_limit) {
        size_t len = strlen(line);
        bool continuation = !at_line_start;
        at_line_start = len > 0 && line[len - SKIP_ONE] == '\n';
        if (continuation) {
            continue;
        }
        while (len > 0 && (line[len - SKIP_ONE] == '\n' || line[len - SKIP_ONE] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        /* PowerShell output uses tab as delimiter (paths may contain colons
         * on Windows, e.g. C:\dir\file). Unix grep uses colon. */
#ifdef _WIN32
        char sep = '\t';
#else
        char sep = ':';
#endif
        char *sep1 = strchr(line, (unsigned char)sep);
        if (!sep1) {
            continue;
        }
        char *sep2 = strchr(sep1 + SKIP_ONE, (unsigned char)sep);
        if (!sep2) {
            continue;
        }
        *sep1 = '\0';
        *sep2 = '\0';

#ifdef _WIN32
        cbm_normalize_path_sep(line);
#endif
        const char *path = line;
        const char *file = strip_root_prefix(path, root_path, root_len);

        if (has_path_filter && cbm_regexec(path_regex, file, 0, NULL, 0) != CBM_REG_OK) {
            continue;
        }

        safe_grow(gm, gm_count, gm_cap, PAIR_LEN);
        snprintf(gm[gm_count].file, sizeof(gm[0].file), "%s", file);
        gm[gm_count].line = (int)strtol(sep1 + SKIP_ONE, NULL, CBM_DECIMAL_BASE);
        snprintf(gm[gm_count].content, sizeof(gm[0].content), "%s", sep2 + SKIP_ONE);
        sanitize_ascii(gm[gm_count].content);
        gm_count++;
    }

    *out_count = gm_count;
    return gm;
}

/* ── In-process content scan ────────────────────────────────────────
 * The scoped search_code path scans the indexed file list here instead of
 * spawning `xargs grep` (POSIX) or a PowerShell Select-String pipeline
 * (Windows), and fills grep_match_t directly, stopping at the match limit:
 *   - patterns: the pattern split on '\n', as `grep -f` reads its file; an
 *     empty pattern matches every line;
 *   - literal mode is a substring match (grep -F); regex mode runs each line
 *     through cbm_regex, the platform's POSIX ERE engine (grep -E), with the
 *     GNU escapes (\b \< \w \s) grep accepts;
 *   - a NUL byte in the first 32 KiB marks the file binary and yields no hit,
 *     as grep prints no lines for it; after a later NUL nothing more is
 *     reported, as GNU grep stops line output there;
 *   - a listed file that cannot be read is skipped, as grep's error exit was
 *     ignored.
 * Ported from upstream 8df0639c without the cancellation checks (the server
 * handles one request at a time) and without the intermediate grep text. */
enum {
    SEARCH_SCAN_BINARY_PROBE = 32768,
    SEARCH_SCAN_CHECK_EVERY = 4096, /* lines between deadline checks */
};

typedef struct {
    char **items;
    size_t *lengths;
    int count;
    cbm_regex_t *regexes; /* regex mode: one per non-empty pattern */
    int compiled;
    bool any_empty; /* an empty pattern: every line matches */
} search_scan_patterns_t;

typedef struct {
    grep_match_t *gm;
    int count;
    int cap;
    int limit;
    const char *root_path;
    size_t root_len;
} search_scan_hits_t;

static void search_scan_patterns_free(search_scan_patterns_t *patterns) {
    for (int i = 0; i < patterns->count; i++) {
        free(patterns->items[i]);
    }
    for (int i = 0; patterns->regexes && i < patterns->compiled; i++) {
        if (patterns->lengths[i] > 0) {
            cbm_regfree(&patterns->regexes[i]);
        }
    }
    free(patterns->items);
    free(patterns->lengths);
    free(patterns->regexes);
    memset(patterns, 0, sizeof(*patterns));
}

static bool search_scan_patterns_parse(search_scan_patterns_t *patterns, const char *pattern,
                                       bool use_regex) {
    memset(patterns, 0, sizeof(*patterns));
    int count = 1;
    for (const char *p = pattern; *p; p++) {
        count += *p == '\n' ? SKIP_ONE : 0;
    }
    patterns->items = calloc((size_t)count, sizeof(char *));
    patterns->lengths = calloc((size_t)count, sizeof(size_t));
    patterns->regexes = use_regex ? calloc((size_t)count, sizeof(cbm_regex_t)) : NULL;
    if (!patterns->items || !patterns->lengths || (use_regex && !patterns->regexes)) {
        search_scan_patterns_free(patterns);
        return false;
    }
    const char *start = pattern;
    for (int i = 0; i < count; i++) {
        const char *end = strchr(start, '\n');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        patterns->items[i] = malloc(length + SKIP_ONE);
        if (!patterns->items[i]) {
            patterns->count = i;
            search_scan_patterns_free(patterns);
            return false;
        }
        memcpy(patterns->items[i], start, length);
        patterns->items[i][length] = '\0';
        patterns->lengths[i] = length;
        patterns->any_empty = patterns->any_empty || length == 0;
        start = end ? end + SKIP_ONE : start + length;
    }
    patterns->count = count;
    for (int i = 0; use_regex && i < count; i++) {
        if (patterns->lengths[i] > 0 &&
            cbm_regcomp(&patterns->regexes[i], patterns->items[i],
                        CBM_REG_EXTENDED | CBM_REG_ENHANCED | CBM_REG_NOSUB) != CBM_REG_OK) {
            patterns->compiled = i;
            search_scan_patterns_free(patterns);
            return false;
        }
        patterns->compiled = i + SKIP_ONE;
    }
    return true;
}

/* First occurrence of needle in hay; memmem where the C library has it. */
static const char *search_scan_find(const char *hay, size_t hay_length, const char *needle,
                                    size_t needle_length) {
    if (needle_length == 0) {
        return hay;
    }
    if (needle_length > hay_length) {
        return NULL;
    }
#ifdef _WIN32
    const char *last = hay + (hay_length - needle_length);
    for (const char *p = hay; p <= last;) {
        p = memchr(p, (unsigned char)needle[0], (size_t)(last - p) + SKIP_ONE);
        if (!p) {
            return NULL;
        }
        if (memcmp(p, needle, needle_length) == 0) {
            return p;
        }
        p++;
    }
    return NULL;
#else
    return memmem(hay, hay_length, needle, needle_length);
#endif
}

static size_t search_scan_count_newlines(const char *from, const char *to) {
    size_t count = 0;
    while (from < to) {
        const char *newline = memchr(from, '\n', (size_t)(to - from));
        if (!newline) {
            break;
        }
        count++;
        from = newline + SKIP_ONE;
    }
    return count;
}

static bool search_scan_line_matches(const search_scan_patterns_t *patterns, bool use_regex,
                                     const char *line) {
    if (patterns->any_empty) {
        return true;
    }
    for (int i = 0; i < patterns->count; i++) {
        if (use_regex ? cbm_regexec(&patterns->regexes[i], line, 0, NULL, 0) == CBM_REG_OK
                      : strstr(line, patterns->items[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* Append one hit the way collect_grep_matches stores a grep record: the path
 * relative to the root, and the line without trailing CRs, truncated to the
 * content buffer. False on allocation failure. */
static bool search_scan_add_hit(search_scan_hits_t *hits, const char *path, size_t line_number,
                                const char *line, size_t line_length) {
    safe_grow(hits->gm, hits->count, hits->cap, PAIR_LEN);
    if (!hits->gm) {
        return false;
    }
    grep_match_t *m = &hits->gm[hits->count];
    snprintf(m->file, sizeof(m->file), "%s",
             strip_root_prefix(path, hits->root_path, hits->root_len));
#ifdef _WIN32
    cbm_normalize_path_sep(m->file);
#endif
    m->line = line_number > (size_t)INT_MAX ? INT_MAX : (int)line_number;
    while (line_length > 0 && line[line_length - SKIP_ONE] == '\r') {
        line_length--;
    }
    if (line_length >= sizeof(m->content)) {
        line_length = sizeof(m->content) - SKIP_ONE;
    }
    memcpy(m->content, line, line_length);
    m->content[line_length] = '\0';
    sanitize_ascii(m->content);
    hits->count++;
    return true;
}

typedef enum {
    SEARCH_SCAN_OK = 0,
    SEARCH_SCAN_DEADLINE = 1,
    SEARCH_SCAN_FAILED = 2,
} search_scan_rc_t;

/* Scan one file. `buffer` holds its bytes plus one spare byte. Stops early once
 * hits->count reaches hits->limit. */
static search_scan_rc_t search_scan_buffer(search_scan_hits_t *hits,
                                           const search_scan_patterns_t *patterns, bool use_regex,
                                           const char *path, char *buffer, size_t length,
                                           uint64_t deadline_ms) {
    size_t probe = length < SEARCH_SCAN_BINARY_PROBE ? length : SEARCH_SCAN_BINARY_PROBE;
    if (memchr(buffer, '\0', probe)) {
        return SEARCH_SCAN_OK;
    }
    const char *nul = memchr(buffer, '\0', length);
    if (nul) {
        /* Lines up to the last complete line before the NUL. */
        size_t keep = 0;
        for (size_t i = (size_t)(nul - buffer); i > 0; i--) {
            if (buffer[i - SKIP_ONE] == '\n') {
                keep = i;
                break;
            }
        }
        length = keep;
    }
    buffer[length] = '\0';
    size_t line_number = 1;
    size_t since_check = 0;
    bool single_literal = !use_regex && !patterns->any_empty && patterns->count == 1;
    size_t pos = 0;
    while (pos < length && hits->count < hits->limit) {
        if (++since_check >= SEARCH_SCAN_CHECK_EVERY) {
            since_check = 0;
            if (cbm_now_ms() >= deadline_ms) {
                return SEARCH_SCAN_DEADLINE;
            }
        }
        size_t line_start = pos;
        if (single_literal) {
            /* Jump to the next occurrence instead of visiting every line. */
            const char *hit = search_scan_find(buffer + pos, length - pos, patterns->items[0],
                                               patterns->lengths[0]);
            if (!hit) {
                break;
            }
            line_start = (size_t)(hit - buffer);
            while (line_start > pos && buffer[line_start - SKIP_ONE] != '\n') {
                line_start--;
            }
            line_number += search_scan_count_newlines(buffer + pos, buffer + line_start);
        }
        char *newline = memchr(buffer + line_start, '\n', length - line_start);
        size_t line_end = newline ? (size_t)(newline - buffer) : length;
        /* Windows matched Select-String's Line, which has no trailing CR, so
         * `foo$` matched a CRLF line there; POSIX grep matches with the CR. */
        size_t match_end = line_end;
#ifdef _WIN32
        if (match_end > line_start && buffer[match_end - SKIP_ONE] == '\r') {
            match_end--;
        }
#endif
        char saved = buffer[match_end];
        buffer[match_end] = '\0';
        bool matched =
            single_literal || search_scan_line_matches(patterns, use_regex, buffer + line_start);
        buffer[match_end] = saved;
        if (matched && !search_scan_add_hit(hits, path, line_number, buffer + line_start,
                                            line_end - line_start)) {
            return SEARCH_SCAN_FAILED;
        }
        pos = line_end + SKIP_ONE;
        line_number++;
    }
    return SEARCH_SCAN_OK;
}

/* Read a whole file into *buffer (grown as needed), leaving one spare byte. */
static bool search_scan_read_file(const char *path, char **buffer, size_t *capacity,
                                  size_t *length) {
    FILE *fp = cbm_fopen(path, "rb");
    if (!fp) {
        return false;
    }
    size_t used = 0;
    bool ok = true;
    for (;;) {
        if (*capacity - used < CBM_SZ_64K + SKIP_ONE) {
            size_t grown = *capacity ? *capacity * PAIR_LEN : (size_t)CBM_SZ_64K * PAIR_LEN;
            if (grown <= *capacity || grown > (size_t)INT_MAX) {
                ok = false;
                break;
            }
            char *next = realloc(*buffer, grown);
            if (!next) {
                ok = false;
                break;
            }
            *buffer = next;
            *capacity = grown;
        }
        size_t want = *capacity - used - SKIP_ONE;
        size_t got = fread(*buffer + used, 1, want, fp);
        used += got;
        if (got < want) {
            ok = !ferror(fp);
            break;
        }
    }
    (void)fclose(fp);
    *length = used;
    return ok;
}

/* Scan every file in filelist_path (the write_scoped_filelist format) and
 * return up to `limit` hits in file-list order. *out_rc is SEARCH_SCAN_OK,
 * SEARCH_SCAN_DEADLINE once cbm_now_ms() reaches deadline_ms, or
 * SEARCH_SCAN_FAILED (bad regex, unreadable list, allocation failure); the
 * array is returned only on SEARCH_SCAN_OK. */
static grep_match_t *search_scan_in_process(const char *filelist_path, const char *root_path,
                                            const char *pattern, bool use_regex, int limit,
                                            uint64_t deadline_ms, int *out_count,
                                            search_scan_rc_t *out_rc) {
#ifdef _WIN32
    const char record_separator = '\n';
#else
    const char record_separator = '\0';
#endif
    *out_count = 0;
    *out_rc = SEARCH_SCAN_FAILED;
    search_scan_patterns_t patterns;
    if (!search_scan_patterns_parse(&patterns, pattern, use_regex)) {
        return NULL;
    }
    char *list = NULL;
    size_t list_capacity = 0;
    size_t list_length = 0;
    search_scan_hits_t hits = {
        .cap = CBM_SZ_64, .limit = limit, .root_path = root_path, .root_len = strlen(root_path)};
    hits.gm = malloc((size_t)hits.cap * sizeof(grep_match_t));
    if (!hits.gm || !search_scan_read_file(filelist_path, &list, &list_capacity, &list_length)) {
        free(hits.gm);
        free(list);
        search_scan_patterns_free(&patterns);
        return NULL;
    }
    char *buffer = NULL;
    size_t capacity = 0;
    search_scan_rc_t rc = SEARCH_SCAN_OK;
    size_t pos = 0;
    while (rc == SEARCH_SCAN_OK && pos < list_length && hits.count < limit) {
        const char *end = memchr(list + pos, record_separator, list_length - pos);
        size_t record_end = end ? (size_t)(end - list) : list_length;
        list[record_end] = '\0';
        const char *path = list + pos;
        pos = record_end + SKIP_ONE;
        if (!path[0]) {
            continue;
        }
        if (cbm_now_ms() >= deadline_ms) {
            rc = SEARCH_SCAN_DEADLINE;
            break;
        }
        size_t length = 0;
        if (!search_scan_read_file(path, &buffer, &capacity, &length)) {
            continue;
        }
        rc = search_scan_buffer(&hits, &patterns, use_regex, path, buffer, length, deadline_ms);
    }
    free(buffer);
    free(list);
    search_scan_patterns_free(&patterns);
    *out_rc = rc;
    if (rc != SEARCH_SCAN_OK) {
        free(hits.gm);
        return NULL;
    }
    *out_count = hits.count;
    return hits.gm;
}

int cbm_mcp_search_scan_for_testing(const char *filelist_path, const char *root_path,
                                    const char *pattern, bool use_regex, int limit,
                                    uint64_t deadline_ms, char *out, size_t out_size) {
    int count = 0;
    search_scan_rc_t rc = SEARCH_SCAN_FAILED;
    grep_match_t *gm = search_scan_in_process(filelist_path, root_path, pattern, use_regex, limit,
                                              deadline_ms, &count, &rc);
    size_t used = 0;
    if (out_size > 0) {
        out[0] = '\0';
    }
    for (int i = 0; i < count && used < out_size; i++) {
        int n = snprintf(out + used, out_size - used, "%s:%d:%s\n", gm[i].file, gm[i].line,
                         gm[i].content);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }
    free(gm);
    return (int)rc;
}

/* Find the tightest node containing a line in a file. Returns index or -1. */
static int find_tightest_node(cbm_node_t *nodes, int count, int line) {
    int best = CBM_NOT_FOUND;
    int best_span = MAX_LINE_SPAN;
    for (int j = 0; j < count; j++) {
        if (nodes[j].start_line <= line && nodes[j].end_line >= line) {
            int span = nodes[j].end_line - nodes[j].start_line;
            if (span < best_span) {
                best = j;
                best_span = span;
            }
        }
    }
    return best;
}

/* Add a grep hit to the search result set (merge into existing or create new). */
static void add_to_search_results(search_result_t **sr, int *sr_count, int *sr_cap, cbm_node_t *n,
                                  int line) {
    for (int j = 0; j < *sr_count; j++) {
        if ((*sr)[j].node_id == n->id) {
            if ((*sr)[j].match_count < CBM_SZ_64) {
                (*sr)[j].match_lines[(*sr)[j].match_count++] = line;
            }
            return;
        }
    }
    if (*sr_count >= *sr_cap) {
        *sr_cap *= PAIR_LEN;
        *sr = safe_realloc(*sr, *sr_cap * sizeof(search_result_t));
        memset(&(*sr)[*sr_count], 0, (*sr_cap - *sr_count) * sizeof(search_result_t));
    }
    search_result_t *r = &(*sr)[*sr_count];
    r->node_id = n->id;
    snprintf(r->node_name, sizeof(r->node_name), "%s", n->name ? n->name : "");
    snprintf(r->qualified_name, sizeof(r->qualified_name), "%s",
             n->qualified_name ? n->qualified_name : "");
    snprintf(r->label, sizeof(r->label), "%s", n->label ? n->label : "");
    snprintf(r->file, sizeof(r->file), "%s", n->file_path ? n->file_path : "");
    r->start_line = n->start_line;
    r->end_line = n->end_line;
    r->match_lines[0] = line;
    r->match_count = SKIP_ONE;
    (*sr_count)++;
}

/* Match a single grep hit to the tightest containing node, then add to sr or raw. */
static void classify_grep_hit(grep_match_t *hit, cbm_node_t *file_nodes, int file_node_count,
                              search_result_t **sr, int *sr_count, int *sr_cap, grep_match_t **raw,
                              int *raw_count, int *raw_cap) {
    int best = find_tightest_node(file_nodes, file_node_count, hit->line);
    if (best >= 0) {
        add_to_search_results(sr, sr_count, sr_cap, &file_nodes[best], hit->line);
    } else {
        if (*raw_count >= *raw_cap) {
            *raw_cap = (*raw_cap == 0) ? CBM_SZ_32 : *raw_cap * PAIR_LEN;
            *raw = safe_realloc(*raw, *raw_cap * sizeof(grep_match_t));
        }
        if (*raw) {
            (*raw)[(*raw_count)++] = *hit;
        }
    }
}

/* Free a file_nodes array returned from cbm_store_find_nodes_by_file. */
static void free_file_nodes(cbm_node_t *nodes, int count) {
    for (int j = 0; j < count; j++) {
        safe_str_free(&nodes[j].project);
        safe_str_free(&nodes[j].label);
        safe_str_free(&nodes[j].name);
        safe_str_free(&nodes[j].qualified_name);
        safe_str_free(&nodes[j].file_path);
        safe_str_free(&nodes[j].properties_json);
    }
    free(nodes);
}

/* Classify all grep matches file-by-file into search results and raw hits. */
static void classify_all_grep_hits(grep_match_t *gm, int gm_count, cbm_store_t *store,
                                   const char *project, search_result_t **sr, int *sr_count,
                                   int *sr_cap, grep_match_t **raw, int *raw_count, int *raw_cap) {
    if (gm_count > 1) {
        qsort(gm, gm_count, sizeof(grep_match_t),
              (int (*)(const void *, const void *))strcmp);
    }
    int i = 0;
    while (i < gm_count) {
        const char *cur_file = gm[i].file;
        int file_start = i;
        while (i < gm_count && strcmp(gm[i].file, cur_file) == 0) {
            i++;
        }
        cbm_node_t *file_nodes = NULL;
        int file_node_count = 0;
        if (store) {
            cbm_store_find_nodes_by_file(store, project, cur_file, &file_nodes, &file_node_count);
        }
        for (int mi = file_start; mi < i; mi++) {
            classify_grep_hit(&gm[mi], file_nodes, file_node_count, sr, sr_count, sr_cap, raw,
                              raw_count, raw_cap);
        }
        free_file_nodes(file_nodes, file_node_count);
    }
}

/* Write indexed file list for scoped grep. Returns true if scoped.
 * When a path_filter is provided, apply it here — before grep — so large
 * indexed projects do not scan files only for collect_grep_matches to discard
 * them later. The predicate is IDENTICAL to the post-grep filter: the same
 * compiled regex run against the same root-relative path (separators
 * normalized on Windows first), so prefiltering can only skip files whose
 * hits would be dropped anyway — results-preserving by construction.
 * *out_written receives the number of records written (0 = the filter
 * excluded every indexed file). */
static bool write_scoped_filelist(cbm_mcp_server_t *srv, const char *project, const char *root_path,
                                  const char *filelist, bool has_path_filter,
                                  cbm_regex_t *path_regex, const char *file_pattern,
                                  int *out_written) {
    *out_written = 0;
    cbm_store_t *pre_store = resolve_store(srv, project);
    if (!pre_store) {
        return false;
    }
    char **indexed_files = NULL;
    int indexed_count = 0;
    if (cbm_store_list_files(pre_store, project, &indexed_files, &indexed_count) != CBM_STORE_OK ||
        indexed_count == 0) {
        for (int fi = 0; fi < indexed_count; fi++) {
            free(indexed_files[fi]);
        }
        free(indexed_files);
        return false;
    }
#ifdef _WIN32
    (void)file_pattern; /* Windows keeps its PowerShell -like filter */
#endif
    FILE *fl = fopen(filelist, "wb");
    bool ok = false;
    int written = 0;
    if (fl) {
        for (int fi = 0; fi < indexed_count; fi++) {
            /* A source path never legitimately contains a newline or carriage
             * return. Those bytes are exactly the record separator on the
             * Windows filelist (and would split naive line readers elsewhere),
             * so a crafted indexed path with an embedded newline could inject
             * an extra entry into the scan set. Skip such paths entirely. */
            if (strpbrk(indexed_files[fi], "\r\n") != NULL) {
                continue;
            }
            if (has_path_filter && path_regex) {
#ifdef _WIN32
                cbm_normalize_path_sep(indexed_files[fi]);
#endif
                if (cbm_regexec(path_regex, indexed_files[fi], 0, NULL, 0) != CBM_REG_OK) {
                    continue;
                }
            }
#ifndef _WIN32
            /* grep --include semantics: a basename glob. */
            const char *base = strrchr(indexed_files[fi], '/');
            base = base ? base + 1 : indexed_files[fi];
            if (file_pattern && fnmatch(file_pattern, base, 0) != 0) {
                continue;
            }
#endif
            /* Write "<root>/<file>" piece-by-piece (no fixed-size buffer, so an
             * arbitrarily long absolute path cannot overflow). Forward slash join
             * so xargs doesn't treat Windows backslashes as escapes; binary mode
             * (wb) prevents CRLF translation. Record separator differs by platform:
             *   - Unix: NUL, consumed by `xargs -0` — handles spaces in paths (a
             *     newline separator would split plain xargs on the space).
             *   - Windows: newline, consumed by PowerShell `Get-Content |
             *     Select-String -LiteralPath` (NUL bytes break Get-Content). */
            (void)fwrite(root_path, 1, strlen(root_path), fl);
            (void)fputc('/', fl);
            (void)fwrite(indexed_files[fi], 1, strlen(indexed_files[fi]), fl);
#ifdef _WIN32
            (void)fputc('\n', fl);
#else
            (void)fputc('\0', fl);
#endif
            written++;
        }
        (void)fclose(fl);
        ok = true;
    }
    for (int fi = 0; fi < indexed_count; fi++) {
        free(indexed_files[fi]);
    }
    free(indexed_files);
    *out_written = written;
    return ok;
}

/* Parse search mode string (0=compact, 1=full, 2=files). */
static int parse_search_mode(const char *mode_str) {
    if (!mode_str) {
        return 0;
    }
    if (strcmp(mode_str, "full") == 0) {
        return SKIP_ONE;
    }
    if (strcmp(mode_str, "files") == 0) {
        return MCP_RETURN_2;
    }
    return 0;
}

/* Validate shell-safe arguments for search. */
/* Search/grep paths and globs are ALWAYS single-quoted (POSIX sh) or
 * double-/single-quoted (Windows cmd/PowerShell) on the command line, which
 * neutralises '&' — a very common character in real paths (R&D, "Foo & Bar",
 * OneDrive). Accept '&' here while still rejecting every metacharacter that
 * could break out of the quoting (#272). */
static bool validate_search_path_arg(const char *s) {
    if (!s) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '\'':
        case '"':
        case ';':
        case '|':
        case '$':
        case '`':
        case '<':
        case '>':
        case '\n':
        case '\r':
#ifndef _WIN32
        case '\\':
#endif
            return false;
        default:
            break;
        }
    }
    return true;
}

static bool validate_search_args(const char *root_path, const char *file_pattern) {
    if (!validate_search_path_arg(root_path)) {
        return false;
    }
    if (file_pattern && !validate_search_path_arg(file_pattern)) {
        return false;
    }
    return true;
}

/* Write pattern to the temp file for grep -f. Returns true on success. */
static bool write_pattern_file(const char *tmpfile, const char *pattern) {
    FILE *tf = fopen(tmpfile, "w");
    if (!tf) {
        return false;
    }
    (void)fprintf(tf, "%s\n", pattern);
    (void)fclose(tf);
    return true;
}

/* Compile a path filter regex. Returns true if compiled successfully. */
static bool compile_path_filter(const char *filter, cbm_regex_t *re) {
    if (!filter || !filter[0]) {
        return false;
    }
    return cbm_regcomp(re, filter, CBM_REG_EXTENDED | CBM_REG_NOSUB) == CBM_REG_OK;
}

static char *handle_search_code(cbm_mcp_server_t *srv, const char *args) {
    char *pattern = cbm_mcp_get_string_arg(args, "pattern");
    char *project = get_project_arg(args);
    char *file_pattern = cbm_mcp_get_string_arg(args, "file_pattern");
    char *path_filter = cbm_mcp_get_string_arg(args, "path_filter");
    char *mode_str = cbm_mcp_get_string_arg(args, "mode");
    int limit = cbm_mcp_get_int_arg(args, "limit", MCP_DEFAULT_LIMIT);
    int context_lines = cbm_mcp_get_int_arg(args, "context", 0);
    bool use_regex = cbm_mcp_get_bool_arg(args, "regex");
    uint64_t search_t0 = cbm_now_ms();
    /* In literal (non-regex) mode a '|' is matched as a byte, not alternation —
     * a common silent 0-match trap; flagged in the result warnings (#282). */
    bool pat_has_pipe = pattern && strchr(pattern, '|') != NULL;

    int mode = parse_search_mode(mode_str);
    free(mode_str);

    cbm_regex_t path_regex;
    bool has_path_filter = compile_path_filter(path_filter, &path_regex);
    free(path_filter);
    path_filter = NULL;

    if (!pattern) {
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result("pattern is required", true);
    }

    /* Project is required */
    if (!project) {
        free(pattern);
        free(file_pattern);
        char *_err = build_project_list_error("project is required");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        return _res;
    }

    char *root_path = get_project_root(srv, project);
    if (!root_path) {
        free(pattern);
        free(project);
        free(file_pattern);
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        return _res;
    }

    if (!validate_search_args(root_path, file_pattern)) {
        if (has_path_filter) {
            cbm_regfree(&path_regex);
        }
        free(root_path);
        free(pattern);
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result("path or file_pattern contains invalid characters", true);
    }

    /* issue #283: when regex=true, a syntactically invalid pattern (e.g. an
     * unclosed group) makes the underlying grep fail, which the handler would
     * otherwise report as an empty result set — indistinguishable from a
     * legitimate no-match. Validate the user's regex up front and return an
     * explicit error so callers can tell "broken pattern" from "no matches". */
    if (use_regex) {
        cbm_regex_t probe;
        if (cbm_regcomp(&probe, pattern, CBM_REG_EXTENDED | CBM_REG_ENHANCED | CBM_REG_NOSUB) !=
            CBM_REG_OK) {
            if (has_path_filter) {
                cbm_regfree(&path_regex);
            }
            free(root_path);
            free(pattern);
            free(project);
            free(file_pattern);
            return cbm_mcp_text_result(
                "invalid regex pattern (regex=true): check for unbalanced (), [], or {}", true);
        }
        cbm_regfree(&probe);
    }

    /* ── Phase 0.5: Multi-word → regex conversion ───────────── */
    /* If pattern contains whitespace and is not already a regex, convert to a
     * regex that matches all words in order: "foo bar baz" → "foo.*bar.*baz".
     * This avoids requiring the exact phrase as a contiguous substring. */
    if (!use_regex && strchr(pattern, ' ')) {
        size_t plen = strlen(pattern);
        /* Worst case: every char is a space → ".*" between each char */
        char *regex_pat = malloc(plen * 3 + 1);
        if (regex_pat) {
            char *dst = regex_pat;
            const char *src = pattern;
            bool in_space = false;
            while (*src) {
                if (*src == ' ' || *src == '\t') {
                    if (!in_space) {
                        *dst++ = '.';
                        *dst++ = '*';
                        in_space = true;
                    }
                } else {
                    /* Escape regex metacharacters from user input */
                    if (strchr("\\^$.|?*+()[]{}", *src)) {
                        *dst++ = '\\';
                    }
                    *dst++ = *src;
                    in_space = false;
                }
                src++;
            }
            *dst = '\0';
            free(pattern);
            pattern = regex_pat;
            use_regex = true;
        }
    }

    /* ── Phase 1: Grep scan ──────────────────────────────────── */
    char tmpfile[CBM_SZ_256];
    snprintf(tmpfile, sizeof(tmpfile), "%s/cbm_search_%d.pat", cbm_tmpdir(), (int)getpid());

    /* No grep-level match limit — let grep find all matches, then dedup and
     * cap in our code. The -m flag caused results from large vendored files
     * to exhaust the quota before reaching project source files. */
    enum { GREP_MAX_MATCHES = 500 };
    int grep_limit = GREP_MAX_MATCHES;

    /* Scope grep to indexed files only — avoids scanning vendored/generated code.
     * Query the graph for distinct file paths, write them to a temp file,
     * then use xargs to pass them to grep. Falls back to recursive grep if
     * no indexed files found (project not fully indexed). */
    char filelist[CBM_SZ_256];
    snprintf(filelist, sizeof(filelist), "%s.files", tmpfile);
    bool scoped = false;
    int scoped_written = 0;

    scoped = write_scoped_filelist(srv, project, root_path, filelist, has_path_filter,
                                   has_path_filter ? &path_regex : NULL, file_pattern,
                                   &scoped_written);

    /* The indexed file list is scanned in process. On Windows the list does not
     * apply file_pattern (the PowerShell -like filter does), so that case keeps
     * the subprocess. */
#ifdef _WIN32
    bool scan_in_process = scoped && !file_pattern;
#else
    bool scan_in_process = scoped;
#endif

    /* Collect grep matches into array */
    int gm_count = 0;
    grep_match_t *gm = NULL;
    if (scoped && scoped_written == 0) {
        /* The path_filter (or POSIX file_pattern) excluded every indexed file — nothing to scan.
         * Skip the grep subprocess: xargs on an empty filelist is
         * platform-dependent (GNU execs grep once with no operands, BSD
         * skips), and the post-grep filter would drop every hit anyway. */
        gm = malloc(sizeof(grep_match_t)); /* empty set; freed below */
        cbm_unlink(filelist);
    } else if (scan_in_process) {
        search_scan_rc_t scan_rc = SEARCH_SCAN_FAILED;
        gm = search_scan_in_process(filelist, root_path, pattern, use_regex, grep_limit,
                                    cbm_now_ms() + MCP_SEARCH_SCAN_TIMEOUT_MS, &gm_count, &scan_rc);
        cbm_unlink(filelist);
        if (!gm) {
            if (has_path_filter) {
                cbm_regfree(&path_regex);
            }
            free(root_path);
            free(pattern);
            free(project);
            free(file_pattern);
            return cbm_mcp_text_result(scan_rc == SEARCH_SCAN_DEADLINE
                                           ? "search_code scan exceeded its execution deadline"
                                           : "search failed",
                                       true);
        }
    } else {
        if (!write_pattern_file(tmpfile, pattern)) {
            char errmsg[CBM_SZ_256];
            snprintf(errmsg, sizeof(errmsg), "search failed: cannot create temp file (%s)",
                     strerror(errno));
            if (scoped) {
                cbm_unlink(filelist);
            }
            if (has_path_filter) {
                cbm_regfree(&path_regex);
            }
            free(root_path);
            free(pattern);
            free(project);
            free(file_pattern);
            return cbm_mcp_text_result(errmsg, true);
        }
        char cmd[CBM_SZ_4K];
        build_grep_cmd(cmd, sizeof(cmd), use_regex, scoped, file_pattern, tmpfile, filelist,
                       root_path);

        /* Run the scan supervised instead of through popen: a wall-clock
         * deadline and an output cap, with the kill taking the whole
         * sh/xargs/grep group. Either bound fails the request closed rather
         * than returning a silently partial match set. grep's own exit status
         * stays ignored, as with popen (1 = no match, 2 = unreadable file).
         * On Windows the scan is one PowerShell process with no children, so
         * the TerminateProcess kill covers it. */
        char outfile[CBM_SZ_256 + 8];
        snprintf(outfile, sizeof(outfile), "%s.out", tmpfile);
#ifdef _WIN32
        const char *const scan_argv[] = {"powershell", "-Command", cmd, NULL};
#else
        const char *const scan_argv[] = {"/bin/sh", "-c", cmd, NULL};
#endif
        cbm_proc_opts_t scan_opts = {0};
        scan_opts.bin = scan_argv[0];
        scan_opts.argv = scan_argv;
        scan_opts.log_file = outfile;
        scan_opts.deadline_ms = MCP_SEARCH_SCAN_TIMEOUT_MS;
        scan_opts.max_log_bytes = MCP_SEARCH_OUTPUT_MAX;
        scan_opts.own_process_group = true;
        cbm_proc_result_t scan_result;
        int scan_rc = cbm_subprocess_run(&scan_opts, &scan_result);
        const char *scan_err = NULL;
        if (scan_rc != 0) {
            scan_err = "search failed";
        } else if (scan_result.outcome == CBM_PROC_HANG) {
            scan_err = "search_code scan exceeded its execution deadline";
        } else if (scan_result.outcome == CBM_PROC_OUTPUT_LIMIT) {
            scan_err = "search failed: output exceeded the 67108864-byte safety limit";
        } else if (scan_result.outcome == CBM_PROC_CRASH ||
                   scan_result.outcome == CBM_PROC_KILLED) {
            scan_err = "search failed: the scan command did not complete";
        }
        FILE *fp = scan_err ? NULL : fopen(outfile, "r");
        if (!fp) {
            (void)cbm_unlink(outfile);
            cbm_unlink(tmpfile);
            if (scoped) {
                cbm_unlink(filelist);
            }
            if (has_path_filter) {
                cbm_regfree(&path_regex);
            }
            free(root_path);
            free(pattern);
            free(project);
            free(file_pattern);
            return cbm_mcp_text_result(scan_err ? scan_err : "search failed", true);
        }

        gm = collect_grep_matches(fp, root_path, strlen(root_path), has_path_filter, &path_regex,
                                  grep_limit, &gm_count);
        (void)fclose(fp);
        (void)cbm_unlink(outfile);
        cbm_unlink(tmpfile);
        if (scoped) {
            cbm_unlink(filelist);
        }
    }

    /* ── Phase 2+3: Block expansion + graph ranking ──────────── */
    /* Sort grep matches by file for contiguous processing.
     * Then: one SQL query per unique file for nodes, one batch query for all degrees. */

    cbm_store_t *store = resolve_store(srv, project);

    int sr_cap = CBM_SZ_32;
    int sr_count = 0;
    search_result_t *sr = calloc(sr_cap, sizeof(search_result_t));

    int raw_cap = CBM_SZ_32;
    int raw_count = 0;
    grep_match_t *raw = malloc(raw_cap * sizeof(grep_match_t));

    /* Sort matches by file path for contiguous per-file processing */
    if (gm_count > 1) {
        qsort(gm, gm_count, sizeof(grep_match_t),
              (int (*)(const void *, const void *))strcmp);
    }

    classify_all_grep_hits(gm, gm_count, store, project, &sr, &sr_count, &sr_cap, &raw, &raw_count,
                           &raw_cap);

    /* Phase 3: batch degree query — ONE query for all results instead of 2×N */
    if (store && sr_count > 0) {
        int64_t *ids = malloc(sr_count * sizeof(int64_t));
        int *in_degs = malloc(sr_count * sizeof(int));
        int *out_degs = malloc(sr_count * sizeof(int));
        for (int j = 0; j < sr_count; j++) {
            ids[j] = sr[j].node_id;
        }
        if (cbm_store_batch_count_degrees(store, ids, sr_count, "CALLS", in_degs, out_degs) ==
            CBM_STORE_OK) {
            for (int j = 0; j < sr_count; j++) {
                sr[j].in_degree = in_degs[j];
                sr[j].out_degree = out_degs[j];
            }
        }
        free(ids);
        free(in_degs);
        free(out_degs);
    }

    /* Compute scores and sort */
    for (int j = 0; j < sr_count; j++) {
        sr[j].score = compute_search_score(&sr[j]);
    }
    if (sr_count > SKIP_ONE) {
        qsort(sr, sr_count, sizeof(search_result_t), search_result_cmp);
    }

    /* ── Phase 4: Context assembly (extracted helper) ─────────── */

    char *result =
        assemble_search_output(sr, sr_count, raw, raw_count, gm_count, limit, mode, context_lines,
                               root_path, pat_has_pipe && !use_regex, cbm_now_ms() - search_t0);
    free(gm);
    free(sr);
    free(raw);
    free(root_path);
    free(pattern);
    free(project);
    free(file_pattern);
    if (has_path_filter) {
        cbm_regfree(&path_regex);
    }
    return result;
}

/* ── detect_changes ───────────────────────────────────────────── */

/* Find symbols defined in a file and add them to the impacted array. */
static void detect_add_impacted_symbols(cbm_store_t *store, const char *project, const char *file,
                                        yyjson_mut_doc *doc, yyjson_mut_val *impacted) {
    cbm_node_t *nodes = NULL;
    int ncount = 0;
    cbm_store_find_nodes_by_file(store, project, file, &nodes, &ncount);
    for (int i = 0; i < ncount; i++) {
        if (nodes[i].label && strcmp(nodes[i].label, "File") != 0 &&
            strcmp(nodes[i].label, "Folder") != 0 && strcmp(nodes[i].label, "Project") != 0) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, item, "name", nodes[i].name ? nodes[i].name : "");
            yyjson_mut_obj_add_strcpy(doc, item, "label", nodes[i].label);
            yyjson_mut_obj_add_strcpy(doc, item, "file", file);
            yyjson_mut_arr_add_val(impacted, item);
        }
    }
    cbm_store_free_nodes(nodes, ncount);
}

static char *handle_detect_changes(cbm_mcp_server_t *srv, const char *args) {
    char *project = get_project_arg(args);
    char *base_branch = cbm_mcp_get_string_arg(args, "base_branch");
    char *since = cbm_mcp_get_string_arg(args, "since");
    char *scope = cbm_mcp_get_string_arg(args, "scope");
    int depth = cbm_mcp_get_int_arg(args, "depth", MCP_DEFAULT_BFS_DEPTH);
    depth = clamp_mcp_depth(depth, "detect_changes");

    /* scope: "files" = just changed files, "symbols" = files + symbols (default) */
    bool want_symbols = !scope || strcmp(scope, "symbols") == 0 || strcmp(scope, "impact") == 0;

    /* `since` (e.g. "HEAD~10", "v0.5.0") is the documented diff base but was
     * previously parsed and never used: it takes precedence over base_branch.
     * Route it through base_branch so the shared shell-arg validation and the
     * existing `<base>...HEAD` (three-dot) diff apply unchanged — `since` thus
     * adopts the same merge-base semantics base_branch already uses. */
    if (since && since[0]) {
        free(base_branch);
        base_branch = since; /* transfer ownership */
        since = NULL;
    }
    free(since); /* no-op after the swap (since is NULL); frees it otherwise */

    if (!base_branch) {
        base_branch = cbm_heap_strdup("main");
    }

    /* Reject shell metacharacters, and a leading '-', in the user-supplied
     * branch name. base_branch is spliced into `git diff --name-only
     * "<base>"...HEAD`; a value starting with '-' would be read by git as an
     * option rather than a ref (e.g. `--output=<path>` writes the diff to an
     * arbitrary file). A real git ref never begins with '-'. */
    if (!cbm_validate_shell_arg(base_branch) || base_branch[0] == '-') {
        free(project);
        free(base_branch);
        free(scope);
        return cbm_mcp_text_result("base_branch contains invalid characters", true);
    }

    char *root_path = get_project_root(srv, project);
    if (!root_path) {
        char *err = build_no_store_error(project);
        char *res = cbm_mcp_text_result(err, true);
        free(err);
        free(project);
        free(base_branch);
        free(scope);
        return res;
    }

    if (!validate_search_path_arg(root_path)) {
        free(root_path);
        free(project);
        free(base_branch);
        free(scope);
        return cbm_mcp_text_result("project path contains invalid characters", true);
    }

    /* Get changed files via git (-C avoids cd + quoting issues on Windows).
     * Three sources are merged:
     *   1. committed changes vs base   (diff <base>...HEAD)
     *   2. unstaged tracked changes    (diff)
     *   3. untracked + staged-new files (status --porcelain) — these are
     *      invisible to `git diff` and were silently missed before, so a
     *      brand-new file never appeared until a manual re-index (#520).
     * status --porcelain prefixes each path with a 2-char code + space
     * ("?? path", "A  path"); the prefix is stripped when parsing below. */
    char cmd[CBM_SZ_2K];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
             "git -C \"%s\" diff --name-only \"%s\"...HEAD 2>NUL & "
             "git -C \"%s\" diff --name-only 2>NUL & "
             "git --no-optional-locks -C \"%s\" status --porcelain "
             "--untracked-files=normal 2>NUL",
             root_path, base_branch, root_path, root_path);
#else
    snprintf(cmd, sizeof(cmd),
             "{ git -C '%s' diff --name-only '%s'...HEAD 2>/dev/null; "
             "git -C '%s' diff --name-only 2>/dev/null; "
             "git --no-optional-locks -C '%s' status --porcelain "
             "--untracked-files=normal 2>/dev/null; } | sort -u",
             root_path, base_branch, root_path, root_path);
#endif

    FILE *fp = cbm_popen_git(cmd);
    if (!fp) {
        char errmsg[CBM_SZ_256];
        snprintf(errmsg, sizeof(errmsg),
                 "git diff failed: cannot execute command (%s). Check that git is installed.",
                 strerror(errno));
        free(root_path);
        free(project);
        free(base_branch);
        free(scope);
        return cbm_mcp_text_result(errmsg, true);
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    yyjson_mut_val *changed = yyjson_mut_arr(doc);
    yyjson_mut_val *impacted = yyjson_mut_arr(doc);

    cbm_store_t *store = resolve_store(srv, project);

    char line[CBM_SZ_1K];
    int file_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - SKIP_ONE] == '\n' || line[len - SKIP_ONE] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        /* `git status --porcelain` prefixes each path with a two-character
         * status code and a space ("?? path", "A  path", " M path"). The two
         * `git diff --name-only` sources emit bare paths. Strip the porcelain
         * prefix when present so all three sources yield clean paths; for a
         * rename ("R  old -> new") keep the post-arrow destination path. */
        char *path_line = line;
        if (len > PAIR_LEN && line[PAIR_LEN] == ' ' && strchr(" MADRCU?!", line[0]) &&
            strchr(" MADRCU?!", line[1])) {
            path_line = line + PAIR_LEN + SKIP_ONE;
            char *arrow = strstr(path_line, " -> ");
            if (arrow) {
                enum { ARROW_LEN = 4 }; /* length of " -> " */
                path_line = arrow + ARROW_LEN;
            }
        }
        if (path_line[0] == '\0') {
            continue;
        }

        yyjson_mut_arr_add_strcpy(doc, changed, path_line);
        file_count++;

        if (want_symbols && store) {
            detect_add_impacted_symbols(store, project, path_line, doc, impacted);
        }
    }
    int git_status = cbm_pclose(fp);

    bool is_error = false;
    if (git_status != 0 && file_count == 0) {
        char hint_buf[CBM_SZ_256];
        snprintf(hint_buf, sizeof(hint_buf),
                 "git diff exited with status %d. Check that branch '%s' exists.", git_status,
                 base_branch);
        yyjson_mut_obj_add_strcpy(doc, root_obj, "hint", hint_buf);
        is_error = true;
    }

    yyjson_mut_obj_add_val(doc, root_obj, "changed_files", changed);
    yyjson_mut_obj_add_int(doc, root_obj, "changed_count", file_count);
    yyjson_mut_obj_add_val(doc, root_obj, "impacted_symbols", impacted);
    yyjson_mut_obj_add_int(doc, root_obj, "depth", depth);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(root_path);
    free(project);
    free(base_branch);
    free(scope);

    char *result = cbm_mcp_text_result(json, is_error);
    free(json);
    return result;
}

/* ── Compact output ───────────────────────────────────────────────
 * Opt-in format:"compact" for discovery tools. The JSON result is rendered as
 * indented "key: value" lines; an array of flat objects becomes a table with
 * one header of column names and one space-separated row per element, which
 * is where most of the saving comes from. Strings are quoted (JSON escapes)
 * whenever a bare word would be ambiguous, null prints as null and a cell an
 * element lacks prints as "-", so the rendering is lossless. The compact
 * result carries no structuredContent copy. */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} compact_buf_t;

static void compact_put(compact_buf_t *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : CBM_SZ_4K;
        while (b->len + n + 1 > cap) {
            cap *= 2;
        }
        char *grown = realloc(b->buf, cap);
        if (!grown) {
            return;
        }
        b->buf = grown;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void compact_puts(compact_buf_t *b, const char *s) {
    compact_put(b, s, strlen(s));
}

static void compact_indent(compact_buf_t *b, int depth) {
    for (int i = 0; i < depth; i++) {
        compact_put(b, "  ", 2);
    }
}

static bool compact_bare_ok(const char *s) {
    if (!s[0] || strcmp(s, "-") == 0 || strcmp(s, "null") == 0 || strcmp(s, "true") == 0 ||
        strcmp(s, "false") == 0) {
        return false;
    }
    char *end = NULL;
    (void)strtod(s, &end);
    if (end && *end == '\0') {
        return false; /* would read back as a number */
    }
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c <= ' ' || *c == '"' || *c == 0x7f) {
            return false;
        }
    }
    return true;
}

static void compact_scalar(compact_buf_t *b, yyjson_val *v) {
    if (yyjson_is_str(v) && compact_bare_ok(yyjson_get_str(v))) {
        compact_puts(b, yyjson_get_str(v));
        return;
    }
    char *text = yyjson_val_write(v, 0, NULL);
    if (text) {
        compact_puts(b, text);
        free(text);
    }
}

static bool compact_is_scalar(yyjson_val *v) {
    return !yyjson_is_obj(v) && !yyjson_is_arr(v);
}

/* An array whose elements are all objects with only scalar values. */
static bool compact_is_table(yyjson_val *arr) {
    if (yyjson_arr_size(arr) == 0) {
        return false;
    }
    size_t i;
    size_t n;
    yyjson_val *el;
    yyjson_arr_foreach(arr, i, n, el) {
        if (!yyjson_is_obj(el)) {
            return false;
        }
        size_t k;
        size_t m;
        yyjson_val *key;
        yyjson_val *val;
        yyjson_obj_foreach(el, k, m, key, val) {
            (void)key;
            if (!compact_is_scalar(val)) {
                return false;
            }
        }
    }
    return true;
}

static void compact_value(compact_buf_t *b, const char *key, yyjson_val *v, int depth);

static void compact_table(compact_buf_t *b, yyjson_val *arr, int depth) {
    enum { MAX_COLS = 64 };
    const char *cols[MAX_COLS];
    int ncols = 0;
    size_t i;
    size_t n;
    yyjson_val *el;
    yyjson_arr_foreach(arr, i, n, el) {
        size_t k;
        size_t m;
        yyjson_val *key;
        yyjson_val *val;
        yyjson_obj_foreach(el, k, m, key, val) {
            (void)val;
            const char *name = yyjson_get_str(key);
            bool seen = false;
            for (int c = 0; c < ncols && !seen; c++) {
                seen = strcmp(cols[c], name) == 0;
            }
            if (!seen && ncols < MAX_COLS) {
                cols[ncols++] = name;
            }
        }
    }
    compact_puts(b, "  (");
    for (int c = 0; c < ncols; c++) {
        compact_puts(b, c ? " " : "");
        compact_puts(b, cols[c]);
    }
    compact_puts(b, ")\n");
    yyjson_arr_foreach(arr, i, n, el) {
        compact_indent(b, depth + 1);
        for (int c = 0; c < ncols; c++) {
            compact_puts(b, c ? " " : "");
            yyjson_val *cell = yyjson_obj_get(el, cols[c]);
            if (cell) {
                compact_scalar(b, cell);
            } else {
                compact_puts(b, "-");
            }
        }
        compact_puts(b, "\n");
    }
}

static void compact_value(compact_buf_t *b, const char *key, yyjson_val *v, int depth) {
    compact_indent(b, depth);
    if (key) {
        compact_puts(b, key);
    }
    if (compact_is_scalar(v)) {
        compact_puts(b, key ? ": " : "");
        compact_scalar(b, v);
        compact_puts(b, "\n");
        return;
    }
    if (yyjson_is_obj(v)) {
        compact_puts(b, key ? ":\n" : "{}\n");
        size_t i;
        size_t n;
        yyjson_val *k;
        yyjson_val *child;
        yyjson_obj_foreach(v, i, n, k, child) {
            compact_value(b, yyjson_get_str(k), child, depth + 1);
        }
        return;
    }
    char count[CBM_SZ_32];
    snprintf(count, sizeof(count), "[%zu]", yyjson_arr_size(v));
    compact_puts(b, count);
    if (compact_is_table(v)) {
        compact_table(b, v, depth);
        return;
    }
    size_t i;
    size_t n;
    yyjson_val *el;
    bool all_scalar = true;
    yyjson_arr_foreach(v, i, n, el) {
        all_scalar = all_scalar && compact_is_scalar(el);
    }
    if (all_scalar) {
        compact_puts(b, ":");
        yyjson_arr_foreach(v, i, n, el) {
            compact_puts(b, " ");
            compact_scalar(b, el);
        }
        compact_puts(b, "\n");
        return;
    }
    compact_puts(b, ":\n");
    yyjson_arr_foreach(v, i, n, el) {
        if (yyjson_is_obj(el)) {
            compact_indent(b, depth + 1);
            compact_puts(b, "-\n");
            size_t k;
            size_t m;
            yyjson_val *ck;
            yyjson_val *cv;
            yyjson_obj_foreach(el, k, m, ck, cv) {
                compact_value(b, yyjson_get_str(ck), cv, depth + 2);
            }
        } else {
            compact_value(b, "-", el, depth + 1);
        }
    }
}

/* Render a JSON object as compact text. NULL when json is not an object. */
char *cbm_mcp_render_compact(const char *json) {
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return NULL;
    }
    compact_buf_t b = {0};
    size_t i;
    size_t n;
    yyjson_val *k;
    yyjson_val *v;
    yyjson_obj_foreach(root, i, n, k, v) {
        compact_value(&b, yyjson_get_str(k), v, 0);
    }
    yyjson_doc_free(doc);
    return b.buf ? b.buf : cbm_heap_strdup("");
}

/* Tools whose schema advertises format:"compact". */
static bool tool_supports_compact(const char *tool_name) {
    static const char *const names[] = {"search_graph",     "query_graph", "trace_path",
                                        "trace_call_path",  "search_code", "get_architecture",
                                        "list_projects",    "check_index_coverage",
                                        "get_file_outline"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(tool_name, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Replace a successful JSON tool result with its compact rendering. Results
 * that are errors or not JSON objects pass through unchanged. */
static char *compact_tool_result(char *result) {
    yyjson_doc *doc = result ? yyjson_read(result, strlen(result), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *content = yyjson_arr_get_first(yyjson_obj_get(root, "content"));
    const char *text = yyjson_get_str(yyjson_obj_get(content, "text"));
    char *compact = yyjson_get_bool(yyjson_obj_get(root, "isError")) || !text
                        ? NULL
                        : cbm_mcp_render_compact(text);
    yyjson_doc_free(doc);
    if (!compact) {
        return result;
    }
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *mroot = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, mroot);
    yyjson_mut_val *items = yyjson_mut_arr(mdoc);
    yyjson_mut_val *item = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_str(mdoc, item, "type", "text");
    yyjson_mut_obj_add_str(mdoc, item, "text", compact);
    yyjson_mut_arr_add_val(items, item);
    yyjson_mut_obj_add_val(mdoc, mroot, "content", items);
    yyjson_mut_obj_add_bool(mdoc, mroot, "isError", false);
    char *out = yy_doc_to_str(mdoc);
    yyjson_mut_doc_free(mdoc);
    free(compact);
    free(result);
    return out;
}

/* ── Tool dispatch ────────────────────────────────────────────── */

static char *dispatch_tool(cbm_mcp_server_t *srv, const char *tool_name, const char *args_json);

char *cbm_mcp_handle_tool(cbm_mcp_server_t *srv, const char *tool_name, const char *args_json) {
    if (!tool_name) {
        return cbm_mcp_text_result("missing tool name", true);
    }
    char *format = args_json ? cbm_mcp_get_string_arg(args_json, "format") : NULL;
    bool compact = false;
    if (format && tool_supports_compact(tool_name)) {
        if (strcmp(format, "compact") == 0) {
            compact = true;
        } else if (strcmp(format, "json") != 0) {
            free(format);
            return cbm_mcp_text_result("format must be \"json\" or \"compact\"", true);
        }
    }
    free(format);
    char *result = dispatch_tool(srv, tool_name, args_json);
    return compact ? compact_tool_result(result) : result;
}

static char *dispatch_tool(cbm_mcp_server_t *srv, const char *tool_name, const char *args_json) {

    if (strcmp(tool_name, "list_projects") == 0) {
        return handle_list_projects(srv, args_json);
    }
    if (strcmp(tool_name, "compare_graphs") == 0) {
        return handle_compare_graphs(srv, args_json);
    }
    if (strcmp(tool_name, "get_graph_schema") == 0) {
        return handle_get_graph_schema(srv, args_json);
    }
    if (strcmp(tool_name, "search_graph") == 0) {
        return handle_search_graph(srv, args_json);
    }
    if (strcmp(tool_name, "query_graph") == 0) {
        return handle_query_graph(srv, args_json);
    }
    if (strcmp(tool_name, "index_status") == 0) {
        return handle_index_status(srv, args_json);
    }
    if (strcmp(tool_name, "check_index_coverage") == 0) {
        return handle_check_index_coverage(srv, args_json);
    }
    if (strcmp(tool_name, "delete_project") == 0) {
        return handle_delete_project(srv, args_json);
    }
    if (strcmp(tool_name, "trace_path") == 0 || strcmp(tool_name, "trace_call_path") == 0) {
        return handle_trace_call_path(srv, args_json);
    }
    if (strcmp(tool_name, "get_architecture") == 0) {
        return handle_get_architecture(srv, args_json);
    }

    /* Pipeline-dependent tools */
    if (strcmp(tool_name, "index_repository") == 0) {
        return handle_index_repository(srv, args_json);
    }
    if (strcmp(tool_name, "get_code_snippet") == 0) {
        return handle_get_code_snippet(srv, args_json);
    }
    if (strcmp(tool_name, "get_file_outline") == 0) {
        return handle_get_file_outline(srv, args_json);
    }
    if (strcmp(tool_name, "search_code") == 0) {
        return handle_search_code(srv, args_json);
    }
    if (strcmp(tool_name, "detect_changes") == 0) {
        return handle_detect_changes(srv, args_json);
    }
    char msg[CBM_SZ_256];
    snprintf(msg, sizeof(msg), "unknown tool: %s", tool_name);
    return cbm_mcp_text_result(msg, true);
}

/* ── Session detection + auto-index ────────────────────────────── */

/* Detect session root from CWD (fallback: single indexed project from DB). */
static void detect_session(cbm_mcp_server_t *srv) {
    if (srv->session_detected) {
        return;
    }
    srv->session_detected = true;

    /* 1. Try CWD */
    char cwd[CBM_SZ_1K];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        const char *home = cbm_get_home_dir();
        /* Skip useless roots: / and $HOME */
        if (strcmp(cwd, "/") != 0 && (home == NULL || strcmp(cwd, home) != 0)) {
            snprintf(srv->session_root, sizeof(srv->session_root), "%s", cwd);
            cbm_log_info("session.root.cwd", "path", cwd);
        }
    }

    /* Derive project name from path — must match cbm_project_name_from_path
     * used by the pipeline, otherwise session queries look for a project
     * that doesn't match the indexed project name. */
    if (srv->session_root[0]) {
        char *pname = cbm_project_name_from_path(srv->session_root);
        if (pname) {
            snprintf(srv->session_project, sizeof(srv->session_project), "%s", pname);
            free(pname);
        }
    }
}

/* auto_watch config: gates background watcher registration (default on).
 * Multi-project users can contain a session to its own project with
 * `config set auto_watch false`. */
static bool auto_watch_enabled(cbm_mcp_server_t *srv) {
    if (!srv->config) {
        return true; /* default on */
    }
    return cbm_config_get_bool(srv->config, CBM_CONFIG_AUTO_WATCH, true);
}

/* Register the session project with the background watcher for ongoing
 * change detection — unless auto_watch is disabled. */
static void register_watcher_if_enabled(cbm_mcp_server_t *srv) {
    if (!srv->watcher || srv->session_project[0] == '\0' || srv->session_root[0] == '\0') {
        return;
    }
    if (!auto_watch_enabled(srv)) {
        cbm_log_info("watcher.register.skipped", "reason", "auto_watch_off", "project",
                     srv->session_project);
        return;
    }
    cbm_watcher_watch(srv->watcher, srv->session_project, srv->session_root);
}

/* Background auto-index thread function */
static void *autoindex_thread(void *arg) {
    cbm_mcp_server_t *srv = (cbm_mcp_server_t *)arg;

    cbm_log_info("autoindex.start", "project", srv->session_project, "path", srv->session_root);

    /* Background work does not wait: if another process is already indexing
     * this workspace, its result is the one we would produce. */
    cbm_zova_index_hold_t hold;
    cbm_zova_coord_status_t hold_status =
        cbm_mcp_index_hold_acquire(srv->session_root, 0, &hold, NULL, 0);
    if (hold_status != CBM_ZOVA_COORD_OK) {
        cbm_log_info("autoindex.skip", "reason",
                     cbm_mcp_index_hold_refusal_reason(hold_status, &hold), "project",
                     srv->session_project);
        if (hold_status == CBM_ZOVA_COORD_BUSY) {
            register_watcher_if_enabled(srv);
        }
        return NULL;
    }

    /* #832: prefer the supervised worker subprocess. Indexing the whole session in
     * this long-lived server thread ratchets RSS (mimalloc v3 does not reclaim the
     * pages worker threads abandon at exit); running it in a child that exits hands
     * 100% of that memory back to the OS every cycle. Degrade to the in-process
     * pipeline below when the supervisor is off (kill switch) or the spawn fails. */
    if (cbm_index_supervisor_should_wrap()) {
        char *resp = index_run_supervised_path(srv, srv->session_root);
        if (resp) {
            free(resp);
            cbm_log_info("autoindex.done", "project", srv->session_project, "mode", "supervised");
            /* Register with watcher for ongoing change detection — gated on
             * auto_watch (#849), same as the in-process branch below. A bare
             * `if (srv->watcher)` would register even when the user set
             * `config set auto_watch false`, since srv->watcher is always set. */
            cbm_zova_index_hold_release(&hold);
            register_watcher_if_enabled(srv);
            return NULL;
        }
        /* resp == NULL → spawn-failure degrade → fall through to in-process. */
    }

    cbm_pipeline_t *p = cbm_pipeline_new(srv->session_root, NULL, CBM_MODE_FULL);
    if (!p) {
        cbm_zova_index_hold_release(&hold);
        cbm_log_warn("autoindex.err", "msg", "pipeline_create_failed");
        return NULL;
    }

    int rc = cbm_pipeline_run(p);
    cbm_zova_index_hold_release(&hold);

    cbm_pipeline_free(p);
    cbm_mem_collect(); /* return mimalloc pages to OS after indexing (in-process only) */

    if (rc == 0) {
        cbm_log_info("autoindex.done", "project", srv->session_project);
        register_watcher_if_enabled(srv);
    } else {
        cbm_log_warn("autoindex.err", "msg", "pipeline_run_failed");
    }
    return NULL;
}

/* Start auto-indexing if configured and project not yet indexed. */
static void maybe_auto_index(cbm_mcp_server_t *srv) {
    if (srv->session_root[0] == '\0') {
        return; /* no session root detected */
    }

    /* Check if the project is already indexed in the shared cbm.zova */
    char zova_path[CBM_SZ_1K];
    cbm_zova_repository_t *repo =
        cbm_zova_user_database_path(zova_path, sizeof(zova_path)) == 0
            ? cbm_zova_repository_open(zova_path, srv->session_project)
            : NULL;
    if (repo) {
        cbm_zova_repository_close(repo);
        /* Already indexed → register watcher for change detection */
        cbm_log_info("autoindex.skip", "reason", "already_indexed", "project",
                     srv->session_project);
        register_watcher_if_enabled(srv);
        return;
    }

/* Default file limit for auto-indexing new projects */
#define DEFAULT_AUTO_INDEX_LIMIT 50000

    /* Check auto_index config */
    bool auto_index = false;
    int file_limit = DEFAULT_AUTO_INDEX_LIMIT;
    if (srv->config) {
        auto_index = cbm_config_get_bool(srv->config, CBM_CONFIG_AUTO_INDEX, false);
        file_limit =
            cbm_config_get_int(srv->config, CBM_CONFIG_AUTO_INDEX_LIMIT, DEFAULT_AUTO_INDEX_LIMIT);
    }

    if (!auto_index) {
        cbm_log_info("autoindex.skip", "reason", "disabled", "hint",
                     "run: codebase-memory-mcp config set auto_index true");
        return;
    }

    /* Quick file count check to avoid OOM on massive repos */
    if (!cbm_validate_shell_arg(srv->session_root)) {
        cbm_log_warn("autoindex.skip", "reason", "path contains shell metacharacters");
        return;
    }
    char cmd[CBM_SZ_1K];
    snprintf(cmd, sizeof(cmd), "git -C '%s' ls-files 2>/dev/null | wc -l", srv->session_root);
    FILE *fp = cbm_popen_git(cmd);
    if (fp) {
        char line[CBM_SZ_64];
        if (fgets(line, sizeof(line), fp)) {
            int count = (int)strtol(line, NULL, CBM_DECIMAL_BASE);
            if (count > file_limit) {
                cbm_log_warn("autoindex.skip", "reason", "too_many_files", "files", line, "limit",
                             CBM_CONFIG_AUTO_INDEX_LIMIT);
                cbm_pclose(fp);
                return;
            }
        }
        cbm_pclose(fp);
    }

    /* Launch auto-index in background */
    if (cbm_thread_create(&srv->autoindex_tid, 0, autoindex_thread, srv) == 0) {
        srv->autoindex_active = true;
    }
}

/* ── Background update check ──────────────────────────────────── */

#define UPDATE_CHECK_URL "https://api.github.com/repos/octacity-org/codebase-memory-mcp/releases/latest"

static void *update_check_thread(void *arg) {
    cbm_mcp_server_t *srv = (cbm_mcp_server_t *)arg;

    /* Use curl with 5s timeout to fetch latest release tag */
    FILE *fp = cbm_popen("curl -sf --max-time 5 -H 'Accept: application/vnd.github+json' "
                         "'" UPDATE_CHECK_URL "' 2>/dev/null",
                         "r");
    if (!fp) {
        srv->update_checked = true;
        return NULL;
    }

    char buf[CBM_SZ_4K];
    size_t total = 0;
    while (total < sizeof(buf) - SKIP_ONE) {
        size_t n = fread(buf + total, SKIP_ONE, sizeof(buf) - SKIP_ONE - total, fp);
        if (n == 0) {
            break;
        }
        total += n;
    }
    buf[total] = '\0';
    cbm_pclose(fp);

    /* Parse tag_name from JSON response */
    yyjson_doc *doc = yyjson_read(buf, total, 0);
    if (!doc) {
        srv->update_checked = true;
        return NULL;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *tag = yyjson_obj_get(root, "tag_name");
    const char *tag_str = yyjson_get_str(tag);

    if (tag_str) {
        const char *current = cbm_cli_get_version();
        if (cbm_compare_versions(tag_str, current) > 0) {
            snprintf(srv->update_notice, sizeof(srv->update_notice),
                     "Update available: %s -> %s -- run: codebase-memory-mcp update  |  "
                     "Enjoying codebase-memory-mcp? Please leave a star: "
                     "https://github.com/DeusData/codebase-memory-mcp",
                     current, tag_str);
            cbm_log_info("update.available", "current", current, "latest", tag_str);
        }
    }

    yyjson_doc_free(doc);
    srv->update_checked = true;
    return NULL;
}

static void start_update_check(cbm_mcp_server_t *srv) {
    if (srv->update_checked) {
        return;
    }
    srv->update_checked = true; /* prevent double-launch */
    if (cbm_thread_create(&srv->update_tid, 0, update_check_thread, srv) == 0) {
        srv->update_thread_active = true;
    }
}

/* Prepend update notice to a tool result, then clear it (one-shot). */
static char *inject_update_notice(cbm_mcp_server_t *srv, char *result_json) {
    if (srv->update_notice[0] == '\0') {
        return result_json;
    }

    /* Parse existing result, prepend notice text, rebuild */
    yyjson_doc *doc = yyjson_read(result_json, strlen(result_json), 0);
    if (!doc) {
        return result_json;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return result_json;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    /* Find the "content" array */
    yyjson_mut_val *content = yyjson_mut_obj_get(root, "content");
    if (content && yyjson_mut_is_arr(content)) {
        /* Prepend a text content item with the update notice */
        yyjson_mut_val *notice_item = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_str(mdoc, notice_item, "type", "text");
        yyjson_mut_obj_add_str(mdoc, notice_item, "text", srv->update_notice);
        yyjson_mut_arr_prepend(content, notice_item);
    }

    size_t len;
    char *new_json = yyjson_mut_write(mdoc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
    yyjson_mut_doc_free(mdoc);

    if (new_json) {
        free(result_json);
        srv->update_notice[0] = '\0'; /* clear — one-shot */
        return new_json;
    }
    return result_json;
}

/* ── Server request handler ───────────────────────────────────── */

char *cbm_mcp_server_handle(cbm_mcp_server_t *srv, const char *line) {
    cbm_jsonrpc_request_t req = {0};
    if (cbm_jsonrpc_parse(line, &req) < 0) {
        return cbm_jsonrpc_format_error(0, JSONRPC_PARSE_ERROR, "Parse error");
    }

    /* Notifications (no id) → handle cancellation, then no response */
    if (!req.has_id) {
        if (req.method && strcmp(req.method, "notifications/cancelled") == 0) {
            if (srv->active_pipeline &&
                cbm_mcp_cancel_request_matches(req.params_raw, srv->active_request_id,
                                               srv->active_request_id_str)) {
                cbm_pipeline_cancel(srv->active_pipeline);
                cbm_log_info("mcp.cancelled", "match", "true");
            }
        }
        cbm_jsonrpc_request_free(&req);
        return NULL;
    }

    struct timespec req_t0;
    cbm_clock_gettime(CLOCK_MONOTONIC, &req_t0);
    char *result_json = NULL;
    bool request_logged = false;

    if (strcmp(req.method, "initialize") == 0) {
        result_json = cbm_mcp_initialize_response(req.params_raw);
        start_update_check(srv);
        detect_session(srv);
        maybe_auto_index(srv);
    } else if (strcmp(req.method, "ping") == 0) {
        result_json = cbm_heap_strdup("{}");
    } else if (strcmp(req.method, "tools/list") == 0) {
        result_json = cbm_mcp_tools_list_page(req.params_raw);
    } else if (strcmp(req.method, "tools/call") == 0) {
        char *tool_name = req.params_raw ? cbm_mcp_get_tool_name(req.params_raw) : NULL;
        char *tool_args =
            req.params_raw ? cbm_mcp_get_arguments(req.params_raw) : cbm_heap_strdup("{}");
        srv->active_request_id = req.id;
        free(srv->active_request_id_str);
        srv->active_request_id_str = req.id_str ? cbm_heap_strdup(req.id_str) : NULL;

        struct timespec t0;
        cbm_clock_gettime(CLOCK_MONOTONIC, &t0);
        result_json = cbm_mcp_handle_tool(srv, tool_name, tool_args);
        srv->active_request_id = CBM_NOT_FOUND;
        free(srv->active_request_id_str);
        srv->active_request_id_str = NULL;
        struct timespec t1;
        cbm_clock_gettime(CLOCK_MONOTONIC, &t1);
        long long dur_us = ((long long)(t1.tv_sec - t0.tv_sec) * MCP_S_TO_US) +
                           ((long long)(t1.tv_nsec - t0.tv_nsec) / MCP_MS_TO_US);
        bool is_err = (result_json != NULL) && (strstr(result_json, "\"isError\":true") != NULL);
        cbm_diag_record_query(dur_us, is_err);
        long long request_dur_us = ((long long)(t1.tv_sec - req_t0.tv_sec) * MCP_S_TO_US) +
                                   ((long long)(t1.tv_nsec - req_t0.tv_nsec) / MCP_MS_TO_US);
        cbm_log_mcp_request(req.method, tool_name, is_err, request_dur_us);
        request_logged = true;

        result_json = inject_update_notice(srv, result_json);
        free(tool_name);
        free(tool_args);
    } else {
        /* Echo the original id (string or numeric, issue #253) on the error. */
        char err_obj[160];
        snprintf(err_obj, sizeof(err_obj), "{\"code\":%d,\"message\":\"Method not found\"}",
                 JSONRPC_METHOD_NOT_FOUND);
        cbm_jsonrpc_response_t err_resp = {
            .id = req.id,
            .id_str = req.id_str,
            .error_json = err_obj,
        };
        char *err = cbm_jsonrpc_format_response(&err_resp);
        struct timespec t1;
        cbm_clock_gettime(CLOCK_MONOTONIC, &t1);
        long long dur_us = ((long long)(t1.tv_sec - req_t0.tv_sec) * MCP_S_TO_US) +
                           ((long long)(t1.tv_nsec - req_t0.tv_nsec) / MCP_MS_TO_US);
        cbm_log_mcp_request(req.method, NULL, true, dur_us);
        cbm_jsonrpc_request_free(&req);
        return err;
    }

    if (!request_logged) {
        struct timespec t1;
        cbm_clock_gettime(CLOCK_MONOTONIC, &t1);
        long long dur_us = ((long long)(t1.tv_sec - req_t0.tv_sec) * MCP_S_TO_US) +
                           ((long long)(t1.tv_nsec - req_t0.tv_nsec) / MCP_MS_TO_US);
        cbm_log_mcp_request(req.method, NULL, false, dur_us);
    }

    cbm_jsonrpc_response_t resp = {
        .id = req.id,
        .id_str = req.id_str,
        .result_json = result_json,
    };
    char *out = cbm_jsonrpc_format_response(&resp);
    free(result_json);
    cbm_jsonrpc_request_free(&req);
    return out;
}

/* Handle a Content-Length-framed message (LSP-style transport).
 * Reads headers, body, processes request, writes framed response. */
static void handle_content_length_frame(cbm_mcp_server_t *srv, FILE *in, FILE *out, char **line,
                                        size_t *cap, int content_len) {
    /* Skip blank line(s) between header and body */
    while (cbm_getline(line, cap, in) > 0) {
        size_t hlen = strlen(*line);
        while (hlen > 0 && ((*line)[hlen - SKIP_ONE] == '\n' || (*line)[hlen - SKIP_ONE] == '\r')) {
            (*line)[--hlen] = '\0';
        }
        if (hlen == 0) {
            break;
        }
    }

    char *body = malloc((size_t)content_len + SKIP_ONE);
    if (!body) {
        return;
    }
    size_t nread = fread(body, SKIP_ONE, (size_t)content_len, in);
    body[nread] = '\0';

    char *resp = cbm_mcp_server_handle(srv, body);
    free(body);

    if (resp) {
        size_t rlen = strlen(resp);
        (void)fprintf(out, "Content-Length: %zu\r\n\r\n%s", rlen, resp);
        (void)fflush(out);
        free(resp);
    }
}

#ifndef _WIN32
/* Unix 3-phase poll: non-blocking fd check, FILE* buffer peek, blocking poll.
 * Returns: 1 = data ready, 0 = timeout (evicted idle stores), -1 = error/EOF. */
static int poll_for_input_unix(cbm_mcp_server_t *srv, int fd, FILE *in) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, SKIP_ONE, 0); /* Phase 1: non-blocking */

    if (pr < 0) {
        return CBM_NOT_FOUND;
    }
    if (pr > 0) {
        return SKIP_ONE;
    }

    /* Phase 2: peek FILE* buffer */
    int saved_flags = fcntl(fd, F_GETFL);
    if (saved_flags < 0) {
        /* fcntl failed — fall through to a short blocking poll (see the Phase-3
         * note below on why the interval is bounded, not the full idle timeout) */
        pr = poll(&pfd, SKIP_ONE, MCP_TIMEOUT_MS);
        if (pr < 0) {
            return CBM_NOT_FOUND;
        }
        if (pr == 0) {
            cbm_mcp_server_evict_idle(srv, STORE_IDLE_TIMEOUT_S);
            return 0;
        }
        return SKIP_ONE;
    }

    (void)fcntl(fd, F_SETFL, saved_flags | O_NONBLOCK);
    int c = fgetc(in);
    (void)fcntl(fd, F_SETFL, saved_flags);

    if (c == EOF) {
        if (feof(in)) {
            return CBM_NOT_FOUND; /* true EOF */
        }
        clearerr(in);
        /* Phase 3: blocking poll, bounded to a SHORT interval (not the full idle
         * timeout). macOS poll()/select() do NOT report POLLIN/POLLHUP when a
         * FIFO's last writer closes — only read() returns 0 there (verified). A
         * 60s poll would therefore leave the server blocked up to a full idle
         * timeout after stdin EOF (a client that closes the pipe would appear to
         * hang). Waking every MCP_TIMEOUT_MS lets the Phase-2 read() above detect
         * the EOF within ~1s. Idle-store eviction (threshold STORE_IDLE_TIMEOUT_S)
         * is idempotent, so checking it on each short tick is harmless. */
        pr = poll(&pfd, SKIP_ONE, MCP_TIMEOUT_MS);
        if (pr < 0) {
            return CBM_NOT_FOUND;
        }
        if (pr == 0) {
            cbm_mcp_server_evict_idle(srv, STORE_IDLE_TIMEOUT_S);
            return 0;
        }
        return SKIP_ONE;
    }

    (void)ungetc(c, in);
    return SKIP_ONE;
}
#endif

/* ── Event loop ───────────────────────────────────────────────── */

int cbm_mcp_server_run(cbm_mcp_server_t *srv, FILE *in, FILE *out) {
    char *line = NULL;
    size_t cap = 0;
    int fd = cbm_fileno(in);

    for (;;) {
        /* Poll with idle timeout so we can evict unused stores between requests.
         *
         * IMPORTANT: poll() operates on the raw fd, but getline() reads from a
         * buffered FILE*. When a client sends multiple messages in rapid
         * succession, the first getline() call may drain ALL kernel data into
         * libc's internal FILE* buffer. Subsequent poll() calls then see an
         * empty kernel fd and block for STORE_IDLE_TIMEOUT_S seconds even
         * though the next messages are already in the FILE* buffer.
         *
         * Fix (Unix): use a three-phase approach —
         *   Phase 1: non-blocking poll (timeout=0) to check the kernel fd.
         *   Phase 2: if Phase 1 returns 0, peek the FILE* buffer via fgetc/
         *            ungetc to detect data buffered by a prior getline() call.
         *            The fd is temporarily set O_NONBLOCK so fgetc() returns
         *            immediately (EAGAIN → EOF + ferror) instead of blocking
         *            when the FILE* buffer is empty, which would otherwise
         *            bypass the Phase 3 idle eviction timeout.
         *   Phase 3: only if both phases confirm no data, do blocking poll. */
#ifdef _WIN32
        /* Windows: WaitForSingleObject on stdin handle */
        HANDLE hStdin = (HANDLE)_get_osfhandle(fd);
        DWORD wr = WaitForSingleObject(hStdin, STORE_IDLE_TIMEOUT_S * MCP_TIMEOUT_MS);
        if (wr == WAIT_FAILED) {
            break;
        }
        if (wr == WAIT_TIMEOUT) {
            cbm_mcp_server_evict_idle(srv, STORE_IDLE_TIMEOUT_S);
            continue;
        }
#else
        int pr = poll_for_input_unix(srv, fd, in);
        if (pr < 0) {
            break;
        }
        if (pr == 0) {
            continue; /* timeout — idle stores evicted */
        }
#endif

        if (cbm_getline(&line, &cap, in) <= 0) {
            break;
        }

        /* Trim trailing newline/CR */
        size_t len = strlen(line);
        while (len > 0 && (line[len - SKIP_ONE] == '\n' || line[len - SKIP_ONE] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        /* Content-Length framing (LSP-style transport) */
        if (strncmp(line, "Content-Length:", SLEN("Content-Length:")) == 0) {
            int content_len = (int)strtol(line + MCP_CONTENT_PREFIX, NULL, CBM_DECIMAL_BASE);
            if (content_len > 0 && content_len <= MCP_DEFAULT_LIMIT * CBM_SZ_1K * CBM_SZ_1K) {
                handle_content_length_frame(srv, in, out, &line, &cap, content_len);
            }
            continue;
        }

        char *resp = cbm_mcp_server_handle(srv, line);
        if (resp) {
            (void)fprintf(out, "%s\n", resp);
            (void)fflush(out);
            free(resp);
        }
    }

    free(line);
    return 0;
}

/* ── cbm_parse_file_uri ──────────────────────────────────────── */

bool cbm_parse_file_uri(const char *uri, char *out_path, int out_size) {
    if (!uri || !out_path || out_size <= 0) {
        if (out_path && out_size > 0) {
            out_path[0] = '\0';
        }
        return false;
    }

    /* Must start with file:// */
    if (strncmp(uri, "file://", SLEN("file://")) != 0) {
        out_path[0] = '\0';
        return false;
    }

    const char *path = uri + MCP_URI_PREFIX;

    /* On Windows, file:///C:/path → /C:/path. Strip leading / before drive letter. */
    if (path[0] == '/' && path[SKIP_ONE] &&
        ((path[SKIP_ONE] >= 'A' && path[SKIP_ONE] <= 'Z') ||
         (path[SKIP_ONE] >= 'a' && path[SKIP_ONE] <= 'z')) &&
        path[PAIR_LEN] == ':') {
        path++; /* skip the leading / */
    }

    snprintf(out_path, out_size, "%s", path);
    return true;
}
