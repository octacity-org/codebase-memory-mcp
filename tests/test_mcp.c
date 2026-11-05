/*
 * test_mcp.c — Tests for the MCP server module.
 *
 * Covers: JSON-RPC parsing, MCP protocol, tool dispatch, tool handlers.
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h" /* cbm_unlink / cbm_rmdir */
#include "../src/foundation/constants.h"
#include "../src/foundation/compat_thread.h"
#include "../src/foundation/log.h"
#include "../src/foundation/mem.h" /* cbm_mem_set_budget_for_tests: over-budget seam */
#include "../src/foundation/sha256.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <cli/cli.h>
#include <mcp/index_supervisor.h> /* spawn-count hook — #845 in-process guard */
#include <mcp/mcp.h>
#include <pipeline/pipeline.h>
#include <store/store.h>
#include <watcher/watcher.h>
#include <zova/cbm_zova.h>
#include <zova/cbm_zova_route.h>
#include <sqlite3.h>
#include <yyjson/yyjson.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/stat.h> /* chmod / stat for read-only query reproductions */
#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

typedef struct {
    cbm_mcp_server_t *srv;
    atomic_int entered;
} index_queue_test_ctx_t;

static void *index_queue_test_contender(void *arg) {
    index_queue_test_ctx_t *ctx = arg;
    cbm_index_job_queue_enter(ctx->srv);
    atomic_store_explicit(&ctx->entered, 1, memory_order_release);
    cbm_index_job_queue_leave(ctx->srv);
    return NULL;
}

TEST(index_job_queue_is_fifo_and_blocks_second_contender) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    index_queue_test_ctx_t ctx = {.srv = srv};
    atomic_init(&ctx.entered, 0);
    cbm_index_job_queue_enter(srv);
    cbm_thread_t thread;
    ASSERT_EQ(cbm_thread_create(&thread, 0, index_queue_test_contender, &ctx), 0);
    for (int i = 0; i < 100 && cbm_index_job_queue_waiter_count(srv) != 1; ++i) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        cbm_nanosleep(&delay, NULL);
    }
    ASSERT_EQ(cbm_index_job_queue_waiter_count(srv), 1);
    ASSERT_EQ(atomic_load_explicit(&ctx.entered, memory_order_acquire), 0);
    cbm_index_job_queue_leave(srv);
    ASSERT_EQ(cbm_thread_join(&thread), 0);
    ASSERT_EQ(atomic_load_explicit(&ctx.entered, memory_order_acquire), 1);
    cbm_mcp_server_free(srv);
    PASS();
}
#ifdef _WIN32
#include <direct.h>
#define cbm_chdir _chdir
#define cbm_getcwd _getcwd
#else
#include <signal.h>
#include <sys/wait.h> /* waitpid — #845 fork+alarm harness */
#include <unistd.h>
#define cbm_chdir chdir
#define cbm_getcwd getcwd
#endif

static char mcp_log_buf[4096];

static void mcp_capture_log(const char *line) {
    snprintf(mcp_log_buf, sizeof(mcp_log_buf), "%s", line ? line : "");
}

static bool response_contains_json_fragment(const char *response, const char *fragment) {
    if (!response || !fragment) {
        return false;
    }
    if (strstr(response, fragment)) {
        return true;
    }

    char escaped[512];
    size_t out = 0;
    for (size_t i = 0; fragment[i] && out + 2 < sizeof(escaped); i++) {
        if (fragment[i] == '"') {
            escaped[out++] = '\\';
        }
        escaped[out++] = fragment[i];
    }
    escaped[out] = '\0';
    return strstr(response, escaped) != NULL;
}

static bool mcp_test_path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int mcp_test_file_digest(const char *path, uint8_t out[CBM_SHA256_DIGEST_LEN]) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    uint8_t buffer[4096];
    size_t read_len = 0;
    while ((read_len = fread(buffer, 1, sizeof(buffer), file)) > 0)
        cbm_sha256_update(&hash, buffer, read_len);
    int rc = ferror(file) ? -1 : 0;
    fclose(file);
    if (rc == 0) cbm_sha256_final(&hash, out);
    return rc;
}

static void restore_cache_dir(const char *saved_copy) {
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_parse_request) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"capabilities\":{}}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.jsonrpc, "2.0");
    ASSERT_STR_EQ(req.method, "initialize");
    ASSERT_EQ(req.id, 1);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.params_raw);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_notification) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "notifications/initialized");
    ASSERT_FALSE(req.has_id);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_invalid) {
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("not json", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_tools_call) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
                       "\"params\":{\"name\":\"search_graph\","
                       "\"arguments\":{\"label\":\"Function\",\"limit\":5}}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "tools/call");
    ASSERT_EQ(req.id, 42);
    ASSERT_NOT_NULL(req.params_raw);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

/* issue #253: JSON-RPC 2.0 §4 permits string ids (Claude Desktop sends them
 * for "initialize"). Previously strtol-coerced to 0; must be preserved. */
TEST(jsonrpc_parse_string_id_issue253) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":\"init-abc\",\"method\":\"initialize\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.id_str);
    ASSERT_STR_EQ(req.id_str, "init-abc");
    cbm_jsonrpc_request_free(&req);

    /* A purely non-numeric string would have become 0 under strtol. */
    const char *line2 = "{\"jsonrpc\":\"2.0\",\"id\":\"xyz\",\"method\":\"ping\"}";
    cbm_jsonrpc_request_t req2 = {0};
    ASSERT_EQ(cbm_jsonrpc_parse(line2, &req2), 0);
    ASSERT_NOT_NULL(req2.id_str);
    ASSERT_STR_EQ(req2.id_str, "xyz");
    cbm_jsonrpc_request_free(&req2);
    PASS();
}

/* issue #253: the response must echo the string id verbatim, not as a number. */
TEST(jsonrpc_format_response_string_id_issue253) {
    cbm_jsonrpc_response_t resp = {
        .id_str = "init-abc",
        .result_json = "{\"ok\":true}",
    };
    char *json = cbm_jsonrpc_format_response(&resp);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"id\":\"init-abc\""));
    /* Must NOT have coerced to a numeric id. */
    ASSERT_NULL(strstr(json, "\"id\":0"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC FORMATTING
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_format_response) {
    cbm_jsonrpc_response_t resp = {
        .id = 1,
        .result_json = "{\"name\":\"codebase-memory-mcp\"}",
    };
    char *json = cbm_jsonrpc_format_response(&resp);
    ASSERT_NOT_NULL(json);
    /* Should contain jsonrpc, id, and result */
    ASSERT_NOT_NULL(strstr(json, "\"jsonrpc\":\"2.0\""));
    ASSERT_NOT_NULL(strstr(json, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(json, "\"result\""));
    free(json);
    PASS();
}

TEST(jsonrpc_format_error) {
    char *json = cbm_jsonrpc_format_error(5, -32600, "Invalid Request");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"id\":5"));
    ASSERT_NOT_NULL(strstr(json, "\"error\""));
    ASSERT_NOT_NULL(strstr(json, "-32600"));
    ASSERT_NOT_NULL(strstr(json, "Invalid Request"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP PROTOCOL HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_initialize_response) {
    cbm_cli_set_version("9.8.7-test");

    /* Default (no params): returns latest supported version */
    char *json = cbm_mcp_initialize_response(NULL);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "codebase-memory-mcp"));
    ASSERT_NOT_NULL(strstr(json, "\"version\":\"9.8.7-test\""));
    ASSERT_NOT_NULL(strstr(json, "capabilities"));
    ASSERT_NOT_NULL(strstr(json, "tools"));
    ASSERT_NOT_NULL(strstr(json, "\"listChanged\":false"));
    ASSERT_NOT_NULL(strstr(json, "2025-11-25"));
    free(json);

    /* Client requests a supported version: server echoes it */
    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"2024-11-05\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2024-11-05"));
    free(json);

    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"2025-06-18\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2025-06-18"));
    free(json);

    /* Client requests unknown version: server returns its latest */
    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"9999-01-01\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2025-11-25"));
    free(json);
    cbm_cli_set_version("dev");
    PASS();
}

TEST(mcp_tools_list) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    /* Should contain all 15 tools */
    ASSERT_NOT_NULL(strstr(json, "index_repository"));
    ASSERT_NOT_NULL(strstr(json, "search_graph"));
    ASSERT_NOT_NULL(strstr(json, "query_graph"));
    ASSERT_NOT_NULL(strstr(json, "trace_path"));
    ASSERT_NOT_NULL(strstr(json, "get_code_snippet"));
    ASSERT_NOT_NULL(strstr(json, "get_file_outline"));
    ASSERT_NOT_NULL(strstr(json, "compare_graphs"));
    ASSERT_NOT_NULL(strstr(json, "get_graph_schema"));
    ASSERT_NOT_NULL(strstr(json, "get_architecture"));
    ASSERT_NOT_NULL(strstr(json, "search_code"));
    ASSERT_NOT_NULL(strstr(json, "list_projects"));
    ASSERT_NOT_NULL(strstr(json, "delete_project"));
    ASSERT_NOT_NULL(strstr(json, "index_status"));
    ASSERT_NOT_NULL(strstr(json, "detect_changes"));
    free(json);
    PASS();
}

TEST(mcp_tools_list_latest_metadata) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"title\":\"Search graph\""));
    ASSERT_NOT_NULL(strstr(json, "\"title\":\"Index repository\""));
    /* No tool declares outputSchema (upstream f59d24fd, #1522): output is
     * format-polymorphic, and a declared schema makes spec-honoring clients
     * render text replies as "{}". */
    ASSERT_NULL(strstr(json, "\"outputSchema\""));
    free(json);
    PASS();
}

TEST(mcp_index_repository_declares_name_override_issue571) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"index_repository\""));
    ASSERT_NOT_NULL(strstr(json, "\"name\":{\"type\":\"string\""));
    ASSERT_NOT_NULL(strstr(json, "Non-ASCII bytes are encoded"));
    free(json);
    PASS();
}

TEST(mcp_tools_array_schemas_have_items) {
    /* VS Code 1.112+ rejects array schemas without "items" (see
     * https://github.com/microsoft/vscode/issues/248810).
     * Walk every tool's inputSchema and verify that every "type":"array"
     * property also contains "items". */
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);

    /* Scan for all occurrences of "type":"array" — each must be followed
     * by "items" before the next closing brace of that property. */
    const char *p = json;
    while ((p = strstr(p, "\"type\":\"array\"")) != NULL) {
        /* Find the enclosing '}' for this property object */
        const char *end = strchr(p, '}');
        ASSERT_NOT_NULL(end);
        /* "items" must appear between p and end */
        size_t span = (size_t)(end - p);
        char *segment = malloc(span + 1);
        memcpy(segment, p, span);
        segment[span] = '\0';
        ASSERT_NOT_NULL(strstr(segment, "\"items\"")); /* array missing items */
        free(segment);
        p = end;
    }

    free(json);
    PASS();
}

/* Guard for PR #560 (schema enum): the get_architecture aspects items schema
 * must carry an enum of the valid tokens — including the new "overview" —
 * mirroring VALID_ASPECTS in mcp.c. Parsed structurally. */
TEST(mcp_get_architecture_aspects_schema_enum_pr560) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);

    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_NOT_NULL(root);
    yyjson_val *tools = yyjson_obj_get(root, "tools");
    ASSERT_NOT_NULL(tools);
    ASSERT_TRUE(yyjson_is_arr(tools));

    yyjson_val *tool;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(tools, &iter);
    yyjson_val *get_arch = NULL;
    while ((tool = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *name = yyjson_obj_get(tool, "name");
        if (name && yyjson_is_str(name) && strcmp(yyjson_get_str(name), "get_architecture") == 0) {
            get_arch = tool;
            break;
        }
    }
    ASSERT_NOT_NULL(get_arch);

    yyjson_val *input_schema = yyjson_obj_get(get_arch, "inputSchema");
    ASSERT_NOT_NULL(input_schema);
    yyjson_val *properties = yyjson_obj_get(input_schema, "properties");
    ASSERT_NOT_NULL(properties);
    yyjson_val *aspects = yyjson_obj_get(properties, "aspects");
    ASSERT_NOT_NULL(aspects);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(aspects, "type")), "array");
    yyjson_val *items = yyjson_obj_get(aspects, "items");
    ASSERT_NOT_NULL(items);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(items, "type")), "string");
    yyjson_val *enum_arr = yyjson_obj_get(items, "enum");
    ASSERT_NOT_NULL(enum_arr);
    ASSERT_TRUE(yyjson_is_arr(enum_arr));

    /* The enum must be exactly the valid-token set — no more, no less. */
    static const char *expected[] = {"all",      "overview",   "structure", "dependencies",
                                     "routes",   "languages",  "packages",  "entry_points",
                                     "hotspots", "boundaries", "layers",    "file_tree",
                                     "clusters"};
    size_t expected_count = sizeof(expected) / sizeof(expected[0]);
    ASSERT_EQ(yyjson_arr_size(enum_arr), expected_count);
    for (size_t i = 0; i < expected_count; i++) {
        bool found = false;
        yyjson_val *ev;
        yyjson_arr_iter eiter;
        yyjson_arr_iter_init(enum_arr, &eiter);
        while ((ev = yyjson_arr_iter_next(&eiter)) != NULL) {
            if (yyjson_is_str(ev) && strcmp(yyjson_get_str(ev), expected[i]) == 0) {
                found = true;
                break;
            }
        }
        ASSERT_TRUE(found);
    }

    yyjson_doc_free(doc);
    free(json);
    PASS();
}

TEST(mcp_text_result) {
    char *json = cbm_mcp_text_result("{\"total\":5}", false);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"type\":\"text\""));
    /* The text value is JSON-escaped inside the "text" field */
    ASSERT_NOT_NULL(strstr(json, "total"));
    ASSERT_NOT_NULL(strstr(json, "\"structuredContent\":{\"total\":5}"));
    ASSERT_NOT_NULL(strstr(json, "\"isError\":false"));
    ASSERT_NULL(strstr(json, "\"isError\":true"));
    free(json);
    PASS();
}

TEST(mcp_text_result_skips_structured_content_for_plain_text) {
    char *json = cbm_mcp_text_result("plain text", false);
    ASSERT_NOT_NULL(json);
    ASSERT_NULL(strstr(json, "\"structuredContent\""));
    ASSERT_NOT_NULL(strstr(json, "\"isError\":false"));
    free(json);
    PASS();
}

TEST(mcp_cancel_matches_request_id) {
    ASSERT_TRUE(cbm_mcp_cancel_request_matches("{\"requestId\":7}", 7, NULL));
    ASSERT_FALSE(cbm_mcp_cancel_request_matches("{\"requestId\":8}", 7, NULL));
    ASSERT_TRUE(cbm_mcp_cancel_request_matches("{\"requestId\":\"call-1\"}", -1, "call-1"));
    ASSERT_FALSE(cbm_mcp_cancel_request_matches("{\"requestId\":\"call-2\"}", -1, "call-1"));
    ASSERT_FALSE(cbm_mcp_cancel_request_matches("{\"requestId\":7}", -1, "7"));
    ASSERT_FALSE(cbm_mcp_cancel_request_matches("{}", 7, NULL));
    PASS();
}

TEST(mcp_text_result_error) {
    char *json = cbm_mcp_text_result("something failed", true);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(json, "something failed"));
    /* Error envelope keeps bounded machine-readable structure (#1522). */
    ASSERT_NOT_NULL(strstr(json, "\"structuredContent\":{\"error\":\"something failed\"}"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_get_tool_name) {
    const char *params = "{\"name\":\"search_graph\",\"arguments\":{\"label\":\"Function\"}}";
    char *name = cbm_mcp_get_tool_name(params);
    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name, "search_graph");
    free(name);
    PASS();
}

TEST(mcp_get_arguments) {
    const char *params =
        "{\"name\":\"search_graph\",\"arguments\":{\"label\":\"Function\",\"limit\":5}}";
    char *args = cbm_mcp_get_arguments(params);
    ASSERT_NOT_NULL(args);
    ASSERT_NOT_NULL(strstr(args, "\"label\":\"Function\""));
    ASSERT_NOT_NULL(strstr(args, "\"limit\":5"));
    free(args);
    PASS();
}

TEST(mcp_get_string_arg) {
    const char *args = "{\"label\":\"Function\",\"name_pattern\":\".*Order.*\"}";
    char *val = cbm_mcp_get_string_arg(args, "label");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "Function");
    free(val);

    val = cbm_mcp_get_string_arg(args, "name_pattern");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, ".*Order.*");
    free(val);

    val = cbm_mcp_get_string_arg(args, "nonexistent");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_int_arg) {
    const char *args = "{\"limit\":10,\"offset\":5}";
    int val = cbm_mcp_get_int_arg(args, "limit", 0);
    ASSERT_EQ(val, 10);
    val = cbm_mcp_get_int_arg(args, "offset", 0);
    ASSERT_EQ(val, 5);
    val = cbm_mcp_get_int_arg(args, "missing", 42);
    ASSERT_EQ(val, 42);
    PASS();
}

TEST(mcp_get_bool_arg) {
    const char *args = "{\"include_connected\":true,\"regex\":false}";
    bool val = cbm_mcp_get_bool_arg(args, "include_connected");
    ASSERT_TRUE(val);
    val = cbm_mcp_get_bool_arg(args, "regex");
    ASSERT_FALSE(val);
    val = cbm_mcp_get_bool_arg(args, "missing");
    ASSERT_FALSE(val);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SERVER HANDLE — PROTOCOL FLOW
 * ══════════════════════════════════════════════════════════════════ */

TEST(server_handle_initialize) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                                   "\"params\":{\"capabilities\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(resp, "codebase-memory-mcp"));
    ASSERT_NOT_NULL(strstr(resp, "capabilities"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_initialized_notification) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    /* Notification has no id → no response */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    ASSERT_NULL(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_list) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":2"));
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    ASSERT_NOT_NULL(strstr(resp, "query_graph"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_list_paginates) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":200,\"method\":\"tools/list\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":200"));
    ASSERT_NOT_NULL(strstr(resp, "\"nextCursor\":\"8\""));
    ASSERT_NOT_NULL(strstr(resp, "index_repository"));
    ASSERT_NULL(strstr(resp, "detect_changes"));
    free(resp);

    resp = cbm_mcp_server_handle(
        srv,
        "{\"jsonrpc\":\"2.0\",\"id\":201,\"method\":\"tools/list\",\"params\":{\"cursor\":\"8\"}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":201"));
    ASSERT_NULL(strstr(resp, "\"nextCursor\""));
    ASSERT_NOT_NULL(strstr(resp, "detect_changes"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_logs_request_without_params) {
    mcp_log_buf[0] = '\0';
    CBMLogLevel prev_level = cbm_log_get_level();
    cbm_log_set_level(CBM_LOG_DEBUG);
    cbm_log_set_format(CBM_LOG_FORMAT_TEXT);
    cbm_log_set_sink_ex(mcp_capture_log, CBM_LOG_SINK_REPLACE);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":210,\"method\":\"tools/list\","
                                   "\"params\":{\"token\":\"secret\"}}");
    ASSERT_NOT_NULL(resp);
    free(resp);
    cbm_mcp_server_free(srv);

    cbm_log_set_sink(NULL);
    cbm_log_set_level(prev_level);

    ASSERT_NOT_NULL(strstr(mcp_log_buf, "msg=mcp.request"));
    ASSERT_NOT_NULL(strstr(mcp_log_buf, "protocol=jsonrpc"));
    ASSERT_NOT_NULL(strstr(mcp_log_buf, "method=tools/list"));
    ASSERT_NOT_NULL(strstr(mcp_log_buf, "status=ok"));
    ASSERT_NULL(strstr(mcp_log_buf, "token"));
    ASSERT_NULL(strstr(mcp_log_buf, "secret"));
    PASS();
}

TEST(server_handle_unknown_method) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"unknown/method\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    ASSERT_NOT_NULL(strstr(resp, "-32601")); /* Method not found */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  TOOL HANDLERS (via server_handle)
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: create a server with an in-memory store populated with test data */
static cbm_mcp_server_t *setup_mcp_with_data(void) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(); /* NULL = in-memory */
    return srv;
}

TEST(tool_list_projects_empty) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"list_projects\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":10"));
    /* Should return a result (possibly empty list) */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_graph_schema_empty) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_graph_schema\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_unknown_tool) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"nonexistent_tool\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return result with isError */
    ASSERT_NOT_NULL(strstr(resp, "isError"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_graph_basic) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();
    cbm_mcp_server_set_project(srv, "basic-search");
    ASSERT_EQ(cbm_store_upsert_project(cbm_mcp_server_store(srv), "basic-search",
                                       "/tmp/basic-search"),
              CBM_STORE_OK);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_graph\","
                                   "\"arguments\":{\"project\":\"basic-search\","
                                   "\"label\":\"Function\",\"limit\":10}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* A semantic-only search_graph call (semantic_query, no structural filter)
 * must not also run the UNFILTERED structural search: that prepended up to
 * `limit` unrelated enriched nodes to a purely semantic request. Covers both
 * the compatibility route. (Issue #25 / upstream 108740d8.) */
static char *extract_text_content(const char *mcp_result);

/* Upstream 0b9df183: get_file_outline lists one file's declarations with
 * exact label filters and total/offset/limit/has_more pagination metadata. */
TEST(tool_get_file_outline_returns_bounded_filtered_rows) {
    th_remove_zova_user_database();
    cbm_mcp_server_t *srv = setup_mcp_with_data();
    cbm_store_t *store = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, "outline-project", "/tmp/outline-project"),
              CBM_STORE_OK);
    cbm_mcp_server_set_project(srv, "outline-project");

    cbm_node_t nodes[] = {
        {.project = "outline-project", .label = "Module", .name = "main",
         .qualified_name = "outline-project.main", .file_path = "src/main.c",
         .start_line = 1, .end_line = 80},
        {.project = "outline-project", .label = "Function", .name = "alpha",
         .qualified_name = "outline-project.src.main.alpha", .file_path = "src/main.c",
         .start_line = 10, .end_line = 14},
        {.project = "outline-project", .label = "Class", .name = "IgnoredClass",
         .qualified_name = "outline-project.src.main.IgnoredClass", .file_path = "src/main.c",
         .start_line = 15, .end_line = 40},
        {.project = "outline-project", .label = "Method", .name = "omega",
         .qualified_name = "outline-project.src.main.omega", .file_path = "src/main.c",
         .start_line = 30, .end_line = 33},
        {.project = "outline-project", .label = "Function", .name = "other",
         .qualified_name = "outline-project.src.other.other", .file_path = "src/other.c",
         .start_line = 1, .end_line = 2},
    };
    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        ASSERT_GT(cbm_store_upsert_node(store, &nodes[i]), 0);
    }
    ASSERT_EQ(th_publish_store_project(store, "outline-project"), 0);

    char *response =
        cbm_mcp_handle_tool(srv, "get_file_outline",
                            "{\"project\":\"outline-project\",\"file_path\":\"./src/main.c\","
                            "\"labels\":[\"Function\",\"Method\"],\"limit\":1}");
    ASSERT_NOT_NULL(response);
    char *inner = extract_text_content(response);
    ASSERT_NOT_NULL(inner);
    yyjson_doc *doc = yyjson_read(inner, strlen(inner), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(root, "file_path")), "src/main.c");
    yyjson_val *results = yyjson_obj_get(root, "results");
    ASSERT_EQ(yyjson_arr_size(results), 1U);
    yyjson_val *row = yyjson_arr_get_first(results);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(row, "name")), "alpha");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(row, "label")), "Function");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(row, "lines")), "10-14");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(row, "qn")), "outline-project.src.main.alpha");
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "total")), 2);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "returned")), 1);
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(root, "has_more")));
    yyjson_doc_free(doc);
    free(inner);
    free(response);

    /* compact renders the same rows as a table. */
    response = cbm_mcp_handle_tool(srv, "get_file_outline",
                                   "{\"project\":\"outline-project\",\"file_path\":\"src/main.c\","
                                   "\"format\":\"compact\"}");
    ASSERT_NOT_NULL(response);
    inner = extract_text_content(response);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "results[3]"));
    ASSERT_NOT_NULL(strstr(inner, "IgnoredClass"));
    ASSERT_NOT_NULL(strstr(inner, "omega"));
    ASSERT_NULL(strstr(inner, "other"));
    ASSERT_NOT_NULL(strstr(inner, "has_more: false"));
    free(inner);
    free(response);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_file_outline_validates_arguments) {
    th_remove_zova_user_database();
    cbm_mcp_server_t *srv = setup_mcp_with_data();
    cbm_store_t *store = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, "outline-controls", "/tmp/outline-controls"),
              CBM_STORE_OK);
    cbm_mcp_server_set_project(srv, "outline-controls");
    cbm_node_t node = {.project = "outline-controls", .label = "Function", .name = "bounded",
                       .qualified_name = "outline-controls.src.main.bounded",
                       .file_path = "src/main.c", .start_line = 7, .end_line = 9};
    ASSERT_GT(cbm_store_upsert_node(store, &node), 0);
    ASSERT_EQ(th_publish_store_project(store, "outline-controls"), 0);

    static const char *const bad_args[][2] = {
        {"{\"project\":\"outline-controls\",\"file_path\":\"../outside.c\"}",
         "repository-relative"},
        {"{\"project\":\"outline-controls\",\"file_path\":\"/abs/main.c\"}",
         "repository-relative"},
        {"{\"project\":\"outline-controls\"}", "file_path is required"},
        {"{\"project\":\"outline-controls\",\"file_path\":\"src/main.c\",\"limit\":201}",
         "between 1 and 200"},
        {"{\"project\":\"outline-controls\",\"file_path\":\"src/main.c\",\"limit\":\"5\"}",
         "between 1 and 200"},
        {"{\"project\":\"outline-controls\",\"file_path\":\"src/main.c\",\"offset\":-1}",
         "non-negative"},
        {"{\"project\":\"outline-controls\",\"file_path\":\"src/main.c\",\"labels\":\"Function\"}",
         "at most 16"},
        {"{\"project\":\"outline-controls\",\"file_path\":\"src/main.c\",\"labels\":[\"\"]}",
         "non-empty"},
        {"{\"project\":\"outline-controls\",\"file_path\":\"src/main.c\",\"format\":\"tree\"}",
         "format must be"},
    };
    for (size_t i = 0; i < sizeof(bad_args) / sizeof(bad_args[0]); i++) {
        char *response = cbm_mcp_handle_tool(srv, "get_file_outline", bad_args[i][0]);
        ASSERT_NOT_NULL(response);
        ASSERT_NOT_NULL(strstr(response, bad_args[i][1]));
        ASSERT_NOT_NULL(strstr(response, "\"isError\":true"));
        free(response);
    }

    /* A file with no declarations is an empty page, not an error. */
    char *response = cbm_mcp_handle_tool(
        srv, "get_file_outline", "{\"project\":\"outline-controls\",\"file_path\":\"none.c\"}");
    ASSERT_NOT_NULL(response);
    ASSERT_NULL(strstr(response, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(response, "total"));
    free(response);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Seed one compare_graphs side: a module, an unchanged function `keep` and the
 * given extra functions, with keep CALLS the first extra. Every qualified name
 * carries the project prefix, which the comparison must ignore. */
static int cmp_seed_project(cbm_store_t *store, const char *project, const char *root,
                            const char *const *extras, int extra_count) {
    if (cbm_store_upsert_project(store, project, root) != CBM_STORE_OK) {
        return -1;
    }
    char qn[128];
    snprintf(qn, sizeof(qn), "%s.pkg", project);
    cbm_node_t module = {.project = project, .label = "Module", .name = "pkg",
                         .qualified_name = qn, .file_path = "pkg.py", .start_line = 1,
                         .end_line = 20};
    int64_t module_id = cbm_store_upsert_node(store, &module);
    snprintf(qn, sizeof(qn), "%s.pkg.keep", project);
    cbm_node_t keep = {.project = project, .label = "Function", .name = "keep",
                       .qualified_name = qn, .file_path = "pkg.py", .start_line = 2,
                       .end_line = 3};
    int64_t keep_id = cbm_store_upsert_node(store, &keep);
    if (module_id <= 0 || keep_id <= 0) {
        return -1;
    }
    cbm_edge_t defines = {.project = project, .source_id = module_id, .target_id = keep_id,
                          .type = "DEFINES"};
    if (cbm_store_insert_edge(store, &defines) <= 0) {
        return -1;
    }
    for (int i = 0; i < extra_count; i++) {
        snprintf(qn, sizeof(qn), "%s.pkg.%s", project, extras[i]);
        cbm_node_t extra = {.project = project, .label = "Function", .name = extras[i],
                            .qualified_name = qn, .file_path = "pkg.py", .start_line = 5 + i,
                            .end_line = 5 + i};
        int64_t extra_id = cbm_store_upsert_node(store, &extra);
        if (extra_id <= 0) {
            return -1;
        }
        if (i == 0) {
            cbm_edge_t calls = {.project = project, .source_id = keep_id,
                                .target_id = extra_id, .type = "CALLS"};
            if (cbm_store_insert_edge(store, &calls) <= 0) {
                return -1;
            }
        }
    }
    return th_publish_store_project(store, project);
}

static yyjson_val *cmp_path(yyjson_val *root, const char *a, const char *b) {
    return yyjson_obj_get(yyjson_obj_get(root, a), b);
}

TEST(tool_compare_graphs_reports_bounded_project_independent_diff) {
    th_remove_zova_user_database();
    cbm_mcp_server_t *srv = setup_mcp_with_data();
    cbm_store_t *store = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(store);
    static const char *const base_extras[] = {"gone"};
    static const char *const target_extras[] = {"fresh", "fresh2"};
    ASSERT_EQ(cmp_seed_project(store, "cmp-base", "/tmp/cmp-base", base_extras, 1), 0);
    ASSERT_EQ(cmp_seed_project(store, "cmp-target", "/tmp/cmp-target", target_extras, 2), 0);

    char *response = cbm_mcp_handle_tool(
        srv, "compare_graphs", "{\"base_project\":\"cmp-base\",\"target_project\":\"cmp-target\"}");
    ASSERT_NOT_NULL(response);
    ASSERT_NULL(strstr(response, "\"isError\":true"));
    char *inner = extract_text_content(response);
    ASSERT_NOT_NULL(inner);
    yyjson_doc *doc = yyjson_read(inner, strlen(inner), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_EQ(yyjson_get_int(cmp_path(root, "base", "node_count")), 3);
    ASSERT_EQ(yyjson_get_int(cmp_path(root, "target", "node_count")), 4);

    /* The unchanged module, keep and DEFINES edge are not reported although
     * every qualified name differs by its project prefix. */
    yyjson_val *added =
        yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(root, "nodes"), "added"), "items");
    ASSERT_EQ(yyjson_arr_size(added), 2U);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(added, 0), "qualified_name")),
                  "cmp-target.pkg.fresh");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(added, 1), "qualified_name")),
                  "cmp-target.pkg.fresh2");
    yyjson_val *removed =
        yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(root, "nodes"), "removed"), "items");
    ASSERT_EQ(yyjson_arr_size(removed), 1U);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(removed, 0), "qualified_name")),
                  "cmp-base.pkg.gone");

    yyjson_val *edges_added = yyjson_obj_get(yyjson_obj_get(root, "edges"), "added");
    ASSERT_EQ(yyjson_get_uint(yyjson_obj_get(edges_added, "total")), 1U);
    yyjson_val *edge = yyjson_arr_get(yyjson_obj_get(edges_added, "items"), 0);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(edge, "type")), "CALLS");
    ASSERT_STR_EQ(yyjson_get_str(cmp_path(edge, "source", "qualified_name")),
                  "cmp-target.pkg.keep");
    ASSERT_STR_EQ(yyjson_get_str(cmp_path(edge, "target", "qualified_name")),
                  "cmp-target.pkg.fresh");
    yyjson_val *edges_removed = yyjson_obj_get(yyjson_obj_get(root, "edges"), "removed");
    ASSERT_EQ(yyjson_get_uint(yyjson_obj_get(edges_removed, "total")), 1U);
    ASSERT_STR_EQ(yyjson_get_str(cmp_path(yyjson_arr_get(yyjson_obj_get(edges_removed, "items"), 0),
                                          "target", "qualified_name")),
                  "cmp-base.pkg.gone");
    yyjson_doc_free(doc);
    free(inner);
    free(response);

    /* limit caps each set but the exact total and the reason stay visible. */
    response = cbm_mcp_handle_tool(srv, "compare_graphs",
                                   "{\"base_project\":\"cmp-base\",\"target_project\":"
                                   "\"cmp-target\",\"limit\":1}");
    ASSERT_NOT_NULL(response);
    inner = extract_text_content(response);
    ASSERT_NOT_NULL(inner);
    doc = yyjson_read(inner, strlen(inner), 0);
    ASSERT_NOT_NULL(doc);
    root = yyjson_doc_get_root(doc);
    yyjson_val *set = yyjson_obj_get(yyjson_obj_get(root, "nodes"), "added");
    ASSERT_EQ(yyjson_get_uint(yyjson_obj_get(set, "returned")), 1U);
    ASSERT_EQ(yyjson_get_uint(yyjson_obj_get(set, "total")), 2U);
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(set, "truncated")));
    ASSERT_STR_EQ(yyjson_get_str(yyjson_arr_get(yyjson_obj_get(set, "truncation_reasons"), 0)),
                  "limit");
    set = yyjson_obj_get(yyjson_obj_get(root, "nodes"), "removed");
    ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(set, "truncated")));
    yyjson_doc_free(doc);
    free(inner);
    free(response);

    /* The combined row bound fails closed instead of returning a partial diff. */
    response = cbm_mcp_handle_tool(srv, "compare_graphs",
                                   "{\"base_project\":\"cmp-base\",\"target_project\":"
                                   "\"cmp-target\",\"scan_limit\":5}");
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(response, "scan_limit_exceeded"));
    free(response);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_compare_graphs_validates_arguments) {
    th_remove_zova_user_database();
    cbm_mcp_server_t *srv = setup_mcp_with_data();
    static const char *const bad_args[][2] = {
        {"{\"base_project\":\"a\"}", "invalid_arguments"},
        {"{\"base_project\":\"a\",\"target_project\":\"a\"}", "must be distinct"},
        {"{\"base_project\":\"a\",\"target_project\":\"b\",\"extra\":1}", "unknown argument"},
        {"{\"base_project\":\"a\",\"target_project\":\"b\",\"limit\":0}",
         "limit must be between 1 and 1000"},
        {"{\"base_project\":\"a\",\"target_project\":\"b\",\"limit\":1001}",
         "limit must be between 1 and 1000"},
        {"{\"base_project\":\"a\",\"target_project\":\"b\",\"limit\":\"5\"}",
         "limit values must be integers"},
        {"{\"base_project\":\"a\",\"target_project\":\"b\",\"scan_limit\":0}",
         "scan_limit must be between 1 and 10000000"},
        {"{\"base_project\":\"a\",\"target_project\":\"b\"}", "project_not_indexed"},
    };
    for (size_t i = 0; i < sizeof(bad_args) / sizeof(bad_args[0]); i++) {
        char *response = cbm_mcp_handle_tool(srv, "compare_graphs", bad_args[i][0]);
        ASSERT_NOT_NULL(response);
        ASSERT_NOT_NULL(strstr(response, bad_args[i][1]));
        ASSERT_NOT_NULL(strstr(response, "\"isError\":true"));
        free(response);
    }
    cbm_mcp_server_free(srv);
    PASS();
}

/* Every reported edge must touch the one function that differs, so unchanged
 * native edges (including IMPORTS with a local_name) match across projects. */
static bool cmp_edges_touch(yyjson_val *set, const char *suffix) {
    yyjson_val *items = yyjson_obj_get(set, "items");
    size_t idx, max;
    yyjson_val *edge;
    yyjson_arr_foreach(items, idx, max, edge) {
        const char *s = yyjson_get_str(cmp_path(edge, "source", "qualified_name"));
        const char *t = yyjson_get_str(cmp_path(edge, "target", "qualified_name"));
        size_t n = strlen(suffix);
        bool hit = (s && strlen(s) >= n && strcmp(s + strlen(s) - n, suffix) == 0) ||
                   (t && strlen(t) >= n && strcmp(t + strlen(t) - n, suffix) == 0);
        if (!hit) {
            return false;
        }
    }
    return yyjson_arr_size(items) > 0;
}

TEST(tool_compare_graphs_diffs_two_indexed_repositories) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    th_remove_zova_user_database();
    char roots[2][512];
    char sources[2][640];
    char cache[512];
    static const char *const bodies[2] = {
        "import os\n\ndef keep():\n    return os.getcwd()\n\ndef gone():\n    return keep()\n",
        "import os\n\ndef keep():\n    return os.getcwd()\n\ndef fresh():\n    return keep()\n",
    };
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-cmp-cache-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    for (int i = 0; i < 2; i++) {
        snprintf(roots[i], sizeof(roots[i]), "%s/cbm-mcp-cmp-repo-XXXXXX", cbm_tmpdir());
        ASSERT_NOT_NULL(cbm_mkdtemp(roots[i]));
        snprintf(sources[i], sizeof(sources[i]), "%s/app.py", roots[i]);
        FILE *source = fopen(sources[i], "wb");
        ASSERT_NOT_NULL(source);
        fputs(bodies[i], source);
        ASSERT_EQ(fclose(source), 0);
    }
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    bool indexed = true;
    static const char *const names[2] = {"cmp-e2e-base", "cmp-e2e-target"};
    for (int i = 0; i < 2; i++) {
        char args[1024];
        snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\",\"name\":\"%s\"}",
                 roots[i], names[i]);
        char *indexed_response = cbm_mcp_handle_tool(srv, "index_repository", args);
        indexed = indexed && indexed_response &&
                  response_contains_json_fragment(indexed_response, "\"status\":\"indexed\"");
        free(indexed_response);
    }
    char *response = cbm_mcp_handle_tool(
        srv, "compare_graphs",
        "{\"base_project\":\"cmp-e2e-base\",\"target_project\":\"cmp-e2e-target\"}");
    char *inner = response ? extract_text_content(response) : NULL;
    yyjson_doc *doc = inner ? yyjson_read(inner, strlen(inner), 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *nodes_added = cmp_path(root, "nodes", "added");
    yyjson_val *nodes_removed = cmp_path(root, "nodes", "removed");
    const char *added_qn = yyjson_get_str(yyjson_obj_get(
        yyjson_arr_get(yyjson_obj_get(nodes_added, "items"), 0), "qualified_name"));
    const char *removed_qn = yyjson_get_str(yyjson_obj_get(
        yyjson_arr_get(yyjson_obj_get(nodes_removed, "items"), 0), "qualified_name"));
    bool nodes_ok = yyjson_get_uint(yyjson_obj_get(nodes_added, "total")) == 1 &&
                    yyjson_get_uint(yyjson_obj_get(nodes_removed, "total")) == 1 &&
                    added_qn && strncmp(added_qn, "cmp-e2e-target.", 15) == 0 &&
                    strstr(added_qn, ".fresh") && removed_qn &&
                    strncmp(removed_qn, "cmp-e2e-base.", 13) == 0 && strstr(removed_qn, ".gone");
    bool edges_ok = cmp_edges_touch(cmp_path(root, "edges", "added"), ".fresh") &&
                    cmp_edges_touch(cmp_path(root, "edges", "removed"), ".gone");
    if (!nodes_ok || !edges_ok) {
        printf("  compare_graphs: %s\n", inner ? inner : "(null)");
    }
    yyjson_doc_free(doc);
    free(inner);
    free(response);

    cbm_mcp_server_free(srv);
    char user_db[640];
    ASSERT_EQ(cbm_zova_user_database_path(user_db, sizeof(user_db)), 0);
    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    static const char *const db_suffixes[] = {"", "-wal", "-shm", ".writer.lock"};
    for (size_t i = 0; i < sizeof(db_suffixes) / sizeof(db_suffixes[0]); i++) {
        char path[672];
        snprintf(path, sizeof(path), "%s%s", user_db, db_suffixes[i]);
        cbm_unlink(path);
    }
    for (int i = 0; i < 2; i++) {
        cbm_unlink(sources[i]);
        cbm_rmdir(roots[i]);
    }
    cbm_rmdir(cache);

    ASSERT_TRUE(indexed);
    ASSERT_TRUE(nodes_ok);
    ASSERT_TRUE(edges_ok);
    PASS();
#endif
}

TEST(tool_search_graph_semantic_only_skips_structural_results_issue1295) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "semantic-only";
    cbm_mcp_server_set_project(srv, proj);
    ASSERT_EQ(cbm_store_upsert_project(st, proj, "/tmp/semantic-only"), CBM_STORE_OK);

    cbm_node_t unrelated = {.project = proj,
                            .label = "Function",
                            .name = "unrelated_node",
                            .qualified_name = "semantic-only.unrelated_node",
                            .file_path = "unrelated.c",
                            .start_line = 1,
                            .end_line = 2};
    ASSERT_GT(cbm_store_upsert_node(st, &unrelated), 0);

    char *resp = cbm_mcp_handle_tool(
        srv, "search_graph",
        "{\"project\":\"semantic-only\",\"semantic_query\":[\"publish\"],\"limit\":5}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "unrelated_node"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_graph_empty_structural_search_keeps_envelope) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    cbm_mcp_server_set_project(srv, "empty-structural");
    ASSERT_EQ(cbm_store_upsert_project(cbm_mcp_server_store(srv), "empty-structural",
                                       "/tmp/empty-structural"),
              CBM_STORE_OK);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), "empty-structural"), 0);
    char *resp = cbm_mcp_handle_tool(
        srv, "search_graph",
        "{\"project\":\"empty-structural\",\"name_pattern\":\"does-not-exist\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"total\":0"));
    ASSERT_NOT_NULL(strstr(resp, "\"results\":[]"));
    ASSERT_NOT_NULL(strstr(resp, "\"has_more\":false"));

    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(search_graph_schema_accepts_project_or_projects) {
    const char *schema = cbm_mcp_tool_input_schema("search_graph");
    ASSERT_NOT_NULL(schema);
    yyjson_doc *doc = yyjson_read(schema, strlen(schema), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *properties = yyjson_obj_get(root, "properties");
    ASSERT_NOT_NULL(yyjson_obj_get(properties, "project"));
    yyjson_val *projects = yyjson_obj_get(properties, "projects");
    ASSERT_NOT_NULL(projects);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(projects, "type")), "array");
    ASSERT_NULL(yyjson_obj_get(root, "required"));
    ASSERT_NOT_NULL(yyjson_obj_get(root, "oneOf"));
    yyjson_doc_free(doc);
    PASS();
}

TEST(tool_search_graph_rejects_project_and_projects) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":131,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"project\":\"one\",\"projects\":[\"one\",\"two\"]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "project and projects are mutually exclusive"));
    ASSERT_NOT_NULL(strstr(resp, "isError"));
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_graph_rejects_empty_projects) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":132,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{\"projects\":[]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "projects must be a non-empty array"));
    ASSERT_NOT_NULL(strstr(resp, "isError"));
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

#if CBM_WITH_ZOVA
enum { CURSOR_TEST_NODES = 5 };

/* Publishes CURSOR_TEST_NODES functions with vectors that all match the
 * token "request" at distinct scores. Each call publishes a new generation. */
static int cursor_publish_project(const char *path, const char *indexed_at) {
    static int8_t values[CURSOR_TEST_NODES][768];
    static int8_t token_values[768];
    CBMDumpNode nodes[CURSOR_TEST_NODES];
    CBMDumpVector vectors[CURSOR_TEST_NODES];
    char names[CURSOR_TEST_NODES][16];
    char qns[CURSOR_TEST_NODES][48];
    memset(token_values, 0, sizeof(token_values));
    token_values[0] = 127;
    for (int i = 0; i < CURSOR_TEST_NODES; i++) {
        memset(values[i], 0, sizeof(values[i]));
        values[i][0] = 127;
        values[i][1 + i] = (int8_t)(20 * (i + 1));
        snprintf(names[i], sizeof(names[i]), "F%d", i);
        snprintf(qns[i], sizeof(qns[i]), "cursor-proj.F%d", i);
        nodes[i] = (CBMDumpNode){.id = i + 1, .project = "cursor-proj", .label = "Function",
                                 .name = names[i], .qualified_name = qns[i],
                                 .file_path = "src/f.c", .start_line = i + 1,
                                 .end_line = i + 1, .properties = "{}"};
        vectors[i] = (CBMDumpVector){.node_id = i + 1, .project = "cursor-proj",
                                     .vector = (const uint8_t *)values[i],
                                     .vector_len = sizeof(values[i])};
    }
    const CBMDumpTokenVec token = {.id = 1, .project = "cursor-proj", .token = "request",
                                   .vector = (const uint8_t *)token_values,
                                   .vector_len = sizeof(token_values), .idf = 1.0f};
    const cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/cursor-proj", .project = "cursor-proj", .indexed_at = indexed_at,
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 768,
        .nodes = nodes, .node_count = CURSOR_TEST_NODES, .node_vectors = vectors,
        .node_vector_count = CURSOR_TEST_NODES, .token_vectors = &token,
        .token_vector_count = 1};
    cbm_zova_workspace_generation_result_t published = {0};
    return cbm_zova_user_database_publish_workspace(path, &input, &published);
}

/* Runs one search_graph page. Appends each row's qualified_name from
 * rows_key to seen (counting duplicates against `unique`) and copies the
 * continuation from cursor_key into next ("" when absent). Returns the
 * number of rows, or -1 when the call failed. */
static int cursor_page(cbm_mcp_server_t *srv, const char *args, const char *rows_key,
                       const char *cursor_key, char seen[][48], int *seen_count, char *next,
                       size_t next_sz) {
    char *response = cbm_mcp_handle_tool(srv, "search_graph", args);
    char *inner = response && !strstr(response, "\"isError\":true")
                      ? extract_text_content(response) : NULL;
    yyjson_doc *doc = inner ? yyjson_read(inner, strlen(inner), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    int rows = -1;
    if (root) {
        rows = 0;
        size_t idx, max;
        yyjson_val *row;
        yyjson_arr_foreach(yyjson_obj_get(root, rows_key), idx, max, row) {
            const char *qn = yyjson_get_str(yyjson_obj_get(row, "qualified_name"));
            if (qn && *seen_count < CURSOR_TEST_NODES * 2) {
                snprintf(seen[(*seen_count)++], 48, "%s", qn);
            }
            rows++;
        }
        const char *cursor = yyjson_get_str(yyjson_obj_get(root, cursor_key));
        snprintf(next, next_sz, "%s", cursor ? cursor : "");
    } else {
        printf("  search_graph %s -> %s\n", args, response ? response : "(null)");
    }
    yyjson_doc_free(doc);
    free(inner);
    free(response);
    return rows;
}

static bool cursor_all_seen_once(char seen[][48], int seen_count) {
    if (seen_count != CURSOR_TEST_NODES) {
        return false;
    }
    for (int i = 0; i < seen_count; i++) {
        for (int j = i + 1; j < seen_count; j++) {
            if (strcmp(seen[i], seen[j]) == 0) {
                return false;
            }
        }
    }
    return true;
}

static bool cursor_call_fails_with(cbm_mcp_server_t *srv, const char *args,
                                   const char *message) {
    char *response = cbm_mcp_handle_tool(srv, "search_graph", args);
    bool ok = response && strstr(response, "\"isError\":true") && strstr(response, message);
    if (!ok) {
        printf("  expected '%s' from %s, got %s\n", message, args,
               response ? response : "(null)");
    }
    free(response);
    return ok;
}
#endif

TEST(tool_search_graph_cursors_page_structural_and_semantic_rows_once) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char cache[512];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-cursor-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    char path[512];
    ASSERT_EQ(cbm_zova_user_database_path(path, sizeof(path)), 0);
    ASSERT_EQ(cursor_publish_project(path, "2026-10-15T00:00:00Z"), 0);
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    /* Structural rows: follow next_cursor with the arguments reordered. */
    char seen[CURSOR_TEST_NODES * 2][48];
    int seen_count = 0;
    char next[128];
    char first_cursor[128];
    char args[512];
    int pages = 0;
    int rows = cursor_page(srv, "{\"project\":\"cursor-proj\",\"label\":\"Function\",\"limit\":2}",
                           "results", "next_cursor", seen, &seen_count, next, sizeof(next));
    snprintf(first_cursor, sizeof(first_cursor), "%s", next);
    while (rows > 0 && next[0] && pages < 10) {
        pages++;
        snprintf(args, sizeof(args),
                 "{\"limit\":2,\"cursor\":\"%s\",\"label\":\"Function\",\"project\":"
                 "\"cursor-proj\"}",
                 next);
        rows = cursor_page(srv, args, "results", "next_cursor", seen, &seen_count, next,
                           sizeof(next));
    }
    bool structural_ok = rows > 0 && pages == 2 && cursor_all_seen_once(seen, seen_count);

    /* Semantic rows page independently through semantic_next_cursor. */
    char semantic_cursor[128];
    seen_count = 0;
    pages = 0;
    rows = cursor_page(srv,
                       "{\"project\":\"cursor-proj\",\"semantic_query\":[\"request\"],"
                       "\"semantic_limit\":2}",
                       "semantic_results", "semantic_next_cursor", seen, &seen_count, next,
                       sizeof(next));
    snprintf(semantic_cursor, sizeof(semantic_cursor), "%s", next);
    while (rows > 0 && next[0] && pages < 10) {
        pages++;
        snprintf(args, sizeof(args),
                 "{\"project\":\"cursor-proj\",\"semantic_query\":[\"request\"],"
                 "\"semantic_limit\":2,\"semantic_cursor\":\"%s\"}",
                 next);
        rows = cursor_page(srv, args, "semantic_results", "semantic_next_cursor", seen,
                           &seen_count, next, sizeof(next));
    }
    bool semantic_ok = rows > 0 && pages == 2 && cursor_all_seen_once(seen, seen_count);

    /* Cursors fail closed: other arguments, both offset forms, garbage, the
     * wrong kind, bad semantic paging, multi-project scope, and a re-index. */
    bool rejects = first_cursor[0] && semantic_cursor[0];
    snprintf(args, sizeof(args),
             "{\"project\":\"cursor-proj\",\"label\":\"Method\",\"limit\":2,\"cursor\":\"%s\"}",
             first_cursor);
    rejects = cursor_call_fails_with(srv, args, "different query arguments") && rejects;
    snprintf(args, sizeof(args),
             "{\"project\":\"cursor-proj\",\"label\":\"Function\",\"offset\":2,"
             "\"cursor\":\"%s\"}",
             first_cursor);
    rejects = cursor_call_fails_with(srv, args, "mutually exclusive") && rejects;
    rejects = cursor_call_fails_with(srv,
                                     "{\"project\":\"cursor-proj\",\"label\":\"Function\","
                                     "\"cursor\":\"sg1.s.1.zz.2\"}",
                                     "invalid_cursor") &&
              rejects;
    snprintf(args, sizeof(args),
             "{\"project\":\"cursor-proj\",\"semantic_query\":[\"request\"],"
             "\"semantic_limit\":2,\"cursor\":\"%s\"}",
             semantic_cursor);
    rejects = cursor_call_fails_with(srv, args, "not a structural cursor") && rejects;
    rejects = cursor_call_fails_with(srv,
                                     "{\"project\":\"cursor-proj\",\"semantic_query\":"
                                     "[\"request\"],\"semantic_limit\":0}",
                                     "semantic_limit must be between 1 and 500") &&
              rejects;
    rejects = cursor_call_fails_with(srv,
                                     "{\"projects\":[\"cursor-proj\"],\"query\":\"F0\","
                                     "\"cursor\":\"x\"}",
                                     "single-project search only") &&
              rejects;
    ASSERT_EQ(cursor_publish_project(path, "2026-10-15T01:00:00Z"), 0);
    snprintf(args, sizeof(args),
             "{\"project\":\"cursor-proj\",\"label\":\"Function\",\"limit\":2,\"cursor\":\"%s\"}",
             first_cursor);
    rejects = cursor_call_fails_with(srv, args, "stale_cursor") && rejects;
    cbm_mcp_server_free(srv);

    static const char *const db_suffixes[] = {"", "-wal", "-shm", ".writer.lock"};
    for (size_t i = 0; i < sizeof(db_suffixes) / sizeof(db_suffixes[0]); i++) {
        char db_file[600];
        snprintf(db_file, sizeof(db_file), "%s%s", path, db_suffixes[i]);
        cbm_unlink(db_file);
    }
    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(structural_ok);
    ASSERT_TRUE(semantic_ok);
    ASSERT_TRUE(rejects);
    PASS();
#endif
}

TEST(tool_search_graph_searches_two_zova_projects_globally) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char cache[512];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-cross-project-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char path[512];
    ASSERT_EQ(cbm_zova_user_database_path(path, sizeof(path)), 0);
    const CBMDumpNode client = {
        .id = 1, .project = "internal-client", .label = "Function", .name = "ClientCall",
        .qualified_name = "internal-client.ClientCall", .file_path = "src/client.c",
        .start_line = 1, .end_line = 2, .properties = "{}"};
    const CBMDumpNode service = {
        .id = 1, .project = "internal-service", .label = "Function",
        .name = "ServiceHandler", .qualified_name = "internal-service.ServiceHandler",
        .file_path = "src/service.c", .start_line = 1, .end_line = 2, .properties = "{}"};
    int8_t client_values[768] = {0};
    int8_t service_values[768] = {0};
    client_values[0] = 127;
    service_values[1] = 127;
    const CBMDumpVector client_vector = {
        .node_id=1,.project="internal-client",.vector=(const uint8_t *)client_values,
        .vector_len=sizeof(client_values)};
    const CBMDumpVector service_vector = {
        .node_id=1,.project="internal-service",.vector=(const uint8_t *)service_values,
        .vector_len=sizeof(service_values)};
    const CBMDumpTokenVec client_token = {
        .id=1,.project="internal-client",.token="request",
        .vector=(const uint8_t *)client_values,.vector_len=sizeof(client_values),.idf=1.0f};
    const CBMDumpTokenVec service_token = {
        .id=1,.project="internal-service",.token="request",
        .vector=(const uint8_t *)client_values,.vector_len=sizeof(client_values),.idf=1.0f};
    const cbm_zova_workspace_generation_input_t client_input = {
        .root_path="/tmp/Primary/shared-project",.project="internal-client",
        .indexed_at="2026-07-22T00:00:00Z",.model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions=768,.nodes=&client,.node_count=1,
        .node_vectors=&client_vector,.node_vector_count=1,
        .token_vectors=&client_token,.token_vector_count=1};
    const cbm_zova_workspace_generation_input_t service_input = {
        .root_path="/tmp/AnotherFolder/shared-project",.project="internal-service",
        .indexed_at="2026-07-22T00:00:00Z",.model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions=768,.nodes=&service,.node_count=1,
        .node_vectors=&service_vector,.node_vector_count=1,
        .token_vectors=&service_token,.token_vector_count=1};
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &client_input, &published), 0);
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &service_input, &published), 0);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    char *response = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":133,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"projects\":[\"shared-project\",\"AnotherFolder/shared-project\"],"
             "\"query\":\"Client OR Service\",\"limit\":10}}}");
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "ClientCall"));
    ASSERT_NOT_NULL(strstr(response, "ServiceHandler"));
    ASSERT_NOT_NULL(strstr(response, "AnotherFolder/shared-project"));
    ASSERT_NOT_NULL(strstr(response, "scope"));
    ASSERT_NOT_NULL(strstr(response, "generations"));
    free(response);

    response = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":134,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"list_projects\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "shared-project"));
    ASSERT_NOT_NULL(strstr(response, "AnotherFolder/shared-project"));
    ASSERT_NOT_NULL(strstr(response, "/tmp/Primary/shared-project"));
    ASSERT_NOT_NULL(strstr(response, "generation"));
    ASSERT_NOT_NULL(strstr(response, "healthy"));
    free(response);

    response = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":135,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"projects\":[\"shared-project\",\"AnotherFolder/shared-project\"],"
             "\"semantic_query\":[\"request\"],\"limit\":10}}}");
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "semantic_results"));
    ASSERT_NOT_NULL(strstr(response, "ClientCall"));
    ASSERT_NOT_NULL(strstr(response, "ServiceHandler"));
    ASSERT_NOT_NULL(strstr(response, "\"project\":\"shared-project\""));
    free(response);

    /* A generation published without the coverage marker must not read as
     * clean. */
    response = cbm_mcp_handle_tool(srv, "check_index_coverage",
                                   "{\"project\":\"shared-project\",\"scopes\":[\".\"]}");
    ASSERT_TRUE(response_contains_json_fragment(response, "\"coverage_recorded\":false"));
    ASSERT_TRUE(response_contains_json_fragment(response, "\"status\":\"coverage_unavailable\""));
    free(response);
    cbm_mcp_server_free(srv);

    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    cbm_unlink(path);
    cbm_rmdir(cache);
    PASS();
#endif
}

TEST(tool_index_repository_reports_zova_committed_counts) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char root[512];
    char cache[512];
    snprintf(root, sizeof(root), "%s/cbm-mcp-zova-index-status-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-zova-index-cache-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    char source_path[640];
    snprintf(source_path, sizeof(source_path), "%s/main.c", root);
    FILE *source = fopen(source_path, "wb");
    ASSERT_NOT_NULL(source);
    fputs("int helper(void) { return 42; }\nint main(void) { return helper(); }\n", source);
    ASSERT_EQ(fclose(source), 0);

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    char args[1024];
    snprintf(args, sizeof(args),
             "{\"repo_path\":\"%s\",\"mode\":\"fast\",\"name\":\"zova-status\"}",
             root);
    char *response = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool indexed = response && response_contains_json_fragment(response, "\"status\":\"indexed\"");
    bool counts_match = response && response_contains_json_fragment(response, "\"nodes\":6") &&
                        response_contains_json_fragment(response, "\"edges\":6");
    bool degraded = response && strstr(response, "degraded");

    char user_db[640];
    ASSERT_EQ(cbm_zova_user_database_path(user_db, sizeof(user_db)), 0);
    bool shared_exists = access(user_db, F_OK) == 0;

    char *deleted_response =
        cbm_mcp_handle_tool(srv, "delete_project", "{\"project\":\"zova-status\"}");
    bool deleted = deleted_response &&
                   response_contains_json_fragment(deleted_response, "\"status\":\"deleted\"");
    char *status_response =
        cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"zova-status\"}");
    bool gone = status_response && strstr(status_response, "not indexed");
    char *repeated_response =
        cbm_mcp_handle_tool(srv, "delete_project", "{\"project\":\"zova-status\"}");
    bool repeated_not_found = repeated_response && strstr(repeated_response, "not_found");
    free(deleted_response);
    free(status_response);
    free(repeated_response);

    free(response);
    cbm_mcp_server_free(srv);
    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    cbm_unlink(user_db);
    char wal[672];
    char shm[672];
    char lock[672];
    snprintf(wal, sizeof(wal), "%s-wal", user_db);
    snprintf(shm, sizeof(shm), "%s-shm", user_db);
    snprintf(lock, sizeof(lock), "%s.writer.lock", user_db);
    cbm_unlink(wal);
    cbm_unlink(shm);
    cbm_unlink(lock);
    cbm_unlink(source_path);
    cbm_rmdir(root);
    cbm_rmdir(cache);

    ASSERT_TRUE(indexed);
    ASSERT_TRUE(counts_match);
    ASSERT_FALSE(degraded);
    ASSERT_TRUE(shared_exists);
    ASSERT_TRUE(deleted);
    ASSERT_TRUE(gone);
    ASSERT_TRUE(repeated_not_found);
    PASS();
#endif
}

/* check_index_coverage: returns the field `key` of the paths[] entry whose
 * requested_path is `requested`, or NULL. Caller frees. */
static char *coverage_path_field(const char *response, const char *requested, const char *key) {
    char *text = extract_text_content(response);
    yyjson_doc *doc = text ? yyjson_read(text, strlen(text), 0) : NULL;
    free(text);
    yyjson_val *paths = yyjson_obj_get(yyjson_doc_get_root(doc), "paths");
    char *out = NULL;
    size_t idx;
    size_t max;
    yyjson_val *item;
    yyjson_arr_foreach(paths, idx, max, item) {
        const char *req = yyjson_get_str(yyjson_obj_get(item, "requested_path"));
        if (req && strcmp(req, requested) == 0) {
            yyjson_val *v = yyjson_obj_get(item, key);
            if (yyjson_is_str(v)) {
                out = strdup(yyjson_get_str(v));
            } else if (v) {
                out = yyjson_val_write(v, 0, NULL);
            }
            break;
        }
    }
    yyjson_doc_free(doc);
    return out;
}

static bool coverage_path_is(const char *response, const char *requested, const char *key,
                             const char *expected) {
    char *value = coverage_path_field(response, requested, key);
    bool ok = value && strcmp(value, expected) == 0;
    if (!ok) {
        printf("  %s.%s = %s (want %s)\n", requested, key, value ? value : "(null)", expected);
    }
    free(value);
    return ok;
}

#define COVERAGE_BROKEN_C(name)                                                                \
    "int " name "(void) { return 1; }\n"                                                       \
    "\n"                                                                                       \
    "int broken( { @@ ### $$ ;;\n"                                                             \
    "}}} )) ((\n"

/* #12: per-file parse coverage is recorded with the generation, survives an
 * incremental run for unchanged files, clears for fixed files, stays scoped
 * to its workspace and is surfaced by check_index_coverage, index_status,
 * index_repository and get_code_snippet. */
TEST(tool_check_index_coverage_reports_recorded_gaps) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char root[512];
    char other[512];
    char cache[512];
    snprintf(root, sizeof(root), "%s/cbm-mcp-coverage-XXXXXX", cbm_tmpdir());
    snprintf(other, sizeof(other), "%s/cbm-mcp-coverage-other-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-coverage-cache-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    ASSERT_NOT_NULL(cbm_mkdtemp(other));
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    ASSERT_EQ(th_write_file(TH_PATH(root, "good.c"), "int helper(void) { return 42; }\n"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "broken.c"), COVERAGE_BROKEN_C("still_ok")), 0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "broken2.c"), COVERAGE_BROKEN_C("other_ok")), 0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "sub/inner.c"), "int inner(void) { return 0; }\n"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "gen.c"), "int generated(void) { return 0; }\n"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(root, ".gitignore"), "gen.c\n"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(other, "clean.c"), "int clean(void) { return 0; }\n"), 0);

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\",\"name\":\"cov-a\"}",
             root);
    char *indexed = cbm_mcp_handle_tool(srv, "index_repository", args);
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\",\"name\":\"cov-b\"}",
             other);
    char *indexed_other = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool index_ok = response_contains_json_fragment(indexed, "\"status\":\"indexed\"") &&
                    response_contains_json_fragment(indexed_other, "\"status\":\"indexed\"");
    /* The index response groups partial parses apart from skipped files and
     * carries the persisted coverage summary. */
    bool index_reports_partial = indexed && strstr(indexed, "parse_partial") &&
                                 strstr(indexed, "broken2.c") &&
                                 response_contains_json_fragment(indexed, "\"partial\":2");

    const char *paths_args =
        "{\"project\":\"cov-a\",\"paths\":[\"good.c\",\"broken.c\",\"./sub/inner.c\",\"gen.c\","
        "\"nope.c\",\"../x.c\",\"/etc/hosts\",\"\"]}";
    char *cov = cbm_mcp_handle_tool(srv, "check_index_coverage", paths_args);
    bool paths_ok =
        response_contains_json_fragment(cov, "\"coverage_recorded\":true") &&
        coverage_path_is(cov, "good.c", "status", "no_recorded_issue") &&
        coverage_path_is(cov, "good.c", "freshness", "metadata_match") &&
        coverage_path_is(cov, "broken.c", "status", "partial") &&
        coverage_path_is(cov, "broken.c", "recommended_action", "read_ranges_and_verify_scope") &&
        coverage_path_is(cov, "./sub/inner.c", "path", "sub/inner.c") &&
        coverage_path_is(cov, "./sub/inner.c", "status", "no_recorded_issue") &&
        coverage_path_is(cov, "gen.c", "status", "excluded") &&
        coverage_path_is(cov, "gen.c", "reason", "gitignore") &&
        coverage_path_is(cov, "nope.c", "status", "missing") &&
        coverage_path_is(cov, "../x.c", "status", "outside_project") &&
        coverage_path_is(cov, "/etc/hosts", "status", "outside_project") &&
        coverage_path_is(cov, "", "status", "invalid_path");
    char *ranges = coverage_path_field(cov, "broken.c", "parse_error_ranges");
    bool ranges_ok = ranges && strstr(ranges, "\"start\":") && strstr(ranges, "\"end\":");
    free(ranges);
    free(cov);

    cov = cbm_mcp_handle_tool(srv, "check_index_coverage",
                              "{\"project\":\"cov-a\",\"scopes\":[\".\"],\"scope_limit\":1}");
    bool scope_ok = response_contains_json_fragment(cov, "\"status\":\"known_gaps\"") &&
                    response_contains_json_fragment(cov, "\"files\":4") &&
                    response_contains_json_fragment(cov, "\"partial\":2") &&
                    response_contains_json_fragment(cov, "\"total_gaps\":2") &&
                    response_contains_json_fragment(cov, "\"has_more\":true") &&
                    response_contains_json_fragment(cov, "\"next_offset\":1");
    free(cov);

    /* Neither paths nor scopes: the whole-project summary. */
    cov = cbm_mcp_handle_tool(srv, "check_index_coverage", "{\"project\":\"cov-a\"}");
    bool summary_ok = response_contains_json_fragment(cov, "\"requested_scope\":\".\"") &&
                      response_contains_json_fragment(cov, "\"status\":\"known_gaps\"") &&
                      response_contains_json_fragment(cov, "\"partial\":2") &&
                      response_contains_json_fragment(cov, "\"total_gaps\":2");
    free(cov);

    char *status = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"cov-a\"}");
    bool status_ok = response_contains_json_fragment(status, "\"recorded\":true") &&
                     response_contains_json_fragment(status, "\"total_gaps\":2");
    free(status);

    char *snippet = cbm_mcp_handle_tool(srv, "get_code_snippet",
                                        "{\"project\":\"cov-a\",\"qualified_name\":\"still_ok\"}");
    bool snippet_ok = snippet && strstr(snippet, "coverage_note");
    free(snippet);

    /* Workspace isolation: cov-b never sees cov-a's rows. */
    cov = cbm_mcp_handle_tool(srv, "check_index_coverage",
                              "{\"project\":\"cov-b\",\"scopes\":[\".\"],\"paths\":[\"broken.c\"]}");
    bool isolated = response_contains_json_fragment(cov, "\"status\":\"no_recorded_issue\"") &&
                    response_contains_json_fragment(cov, "\"files\":1") &&
                    coverage_path_is(cov, "broken.c", "status", "missing");
    free(cov);

    /* A changed file reports stale metadata until the next index run. */
    ASSERT_EQ(th_write_file(TH_PATH(root, "good.c"),
                            "int helper(void) { return 42; }\nint more(void) { return 1; }\n"),
              0);
    cov = cbm_mcp_handle_tool(srv, "check_index_coverage",
                              "{\"project\":\"cov-a\",\"paths\":[\"good.c\"]}");
    bool stale_ok = coverage_path_is(cov, "good.c", "freshness", "metadata_changed") &&
                    coverage_path_is(cov, "good.c", "recommended_action",
                                     "read_source_and_reindex");
    free(cov);

    /* Incremental run: broken.c is fixed, broken2.c is unchanged and keeps its
     * stamp, and the response summary still reports the remaining gap. */
    ASSERT_EQ(th_write_file(TH_PATH(root, "broken.c"), "int still_ok(void) { return 1; }\n"), 0);
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\",\"name\":\"cov-a\"}",
             root);
    char *reindexed = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool reindex_ok = response_contains_json_fragment(reindexed, "\"status\":\"indexed\"") &&
                      response_contains_json_fragment(reindexed, "\"total_gaps\":1");
    cov = cbm_mcp_handle_tool(srv, "check_index_coverage",
                              "{\"project\":\"cov-a\",\"paths\":[\"broken.c\",\"broken2.c\","
                              "\"good.c\"]}");
    bool incremental_ok = coverage_path_is(cov, "broken.c", "status", "no_recorded_issue") &&
                          coverage_path_is(cov, "broken2.c", "status", "partial") &&
                          coverage_path_is(cov, "good.c", "freshness", "metadata_match");
    free(cov);

    free(indexed);
    free(indexed_other);
    free(reindexed);
    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    th_rmtree(root);
    th_rmtree(other);
    th_rmtree(cache);

    ASSERT_TRUE(index_ok);
    ASSERT_TRUE(index_reports_partial);
    ASSERT_TRUE(paths_ok);
    ASSERT_TRUE(ranges_ok);
    ASSERT_TRUE(scope_ok);
    ASSERT_TRUE(summary_ok);
    ASSERT_TRUE(status_ok);
    ASSERT_TRUE(snippet_ok);
    ASSERT_TRUE(isolated);
    ASSERT_TRUE(stale_ok);
    ASSERT_TRUE(reindex_ok);
    ASSERT_TRUE(incremental_ok);
    PASS();
#endif
}

/* #12 (upstream 2235af41/b5c89ee5): index_repository caps its parse_partial
 * list, but check_index_coverage reads File rows per exact path, so a path
 * past the cap keeps its exact status and a clean path stays recorded. */
TEST(tool_check_index_coverage_exact_paths_beyond_partial_cap) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    enum { BROKEN_FILES = 55 };
    char root[512];
    char cache[512];
    snprintf(root, sizeof(root), "%s/cbm-mcp-coverage-cap-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-coverage-cap-cache-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    ASSERT_EQ(th_write_file(TH_PATH(root, "good.c"), "int helper(void) { return 42; }\n"), 0);
    for (int i = 0; i < BROKEN_FILES; i++) {
        char rel[32];
        snprintf(rel, sizeof(rel), "b%02d.c", i);
        ASSERT_EQ(th_write_file(TH_PATH(root, rel), COVERAGE_BROKEN_C("ok_fn")), 0);
    }

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\",\"name\":\"cov-cap\"}",
             root);
    char *indexed = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool index_ok = response_contains_json_fragment(indexed, "\"status\":\"indexed\"") &&
                    response_contains_json_fragment(indexed, "\"count\":55") &&
                    response_contains_json_fragment(indexed, "\"truncated\":true");

    char *cov = cbm_mcp_handle_tool(
        srv, "check_index_coverage",
        "{\"project\":\"cov-cap\",\"paths\":[\"b00.c\",\"b54.c\",\"good.c\"]}");
    bool paths_ok = response_contains_json_fragment(cov, "\"coverage_recorded\":true") &&
                    coverage_path_is(cov, "b00.c", "status", "partial") &&
                    coverage_path_is(cov, "b54.c", "status", "partial") &&
                    coverage_path_is(cov, "good.c", "status", "no_recorded_issue") &&
                    coverage_path_is(cov, "good.c", "freshness", "metadata_match");
    char *ranges = coverage_path_field(cov, "b54.c", "parse_error_ranges");
    bool ranges_ok = ranges && strstr(ranges, "\"start\":");
    free(ranges);
    free(cov);

    cov = cbm_mcp_handle_tool(srv, "check_index_coverage",
                              "{\"project\":\"cov-cap\",\"scopes\":[\".\"]}");
    bool scope_ok = response_contains_json_fragment(cov, "\"status\":\"known_gaps\"") &&
                    response_contains_json_fragment(cov, "\"files\":56") &&
                    response_contains_json_fragment(cov, "\"partial\":55");
    free(cov);

    free(indexed);
    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    th_rmtree(root);
    th_rmtree(cache);

    ASSERT_TRUE(index_ok);
    ASSERT_TRUE(paths_ok);
    ASSERT_TRUE(ranges_ok);
    ASSERT_TRUE(scope_ok);
    PASS();
#endif
}

/* Upstream #963 (a9535092): a file whose one range covers most of it is
 * reported as unusable, and a range list clipped by the extractor cap says how
 * many ranges it lost instead of reading as complete. */
TEST(tool_check_index_coverage_reports_unusable_and_truncated_ranges) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    enum { GARBAGE_BLOCKS = 400 };
    char root[512];
    char cache[512];
    snprintf(root, sizeof(root), "%s/cbm-mcp-coverage-unusable-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-coverage-unusable-cache-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    ASSERT_EQ(th_write_file(TH_PATH(root, "whole.py"), ")))\n((( \n]]] [[[\ndef x(:\n"), 0);
    char *src = malloc(GARBAGE_BLOCKS * 96 + 1);
    ASSERT_NOT_NULL(src);
    size_t off = 0;
    for (int i = 0; i < GARBAGE_BLOCKS; i++) {
        off += (size_t)snprintf(src + off, 96, "def ok%d():\n    return %d\n%%%%%% g%d ((( %%%%%%\n",
                                i, i, i);
    }
    ASSERT_EQ(th_write_file(TH_PATH(root, "many.py"), src), 0);
    free(src);

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\",\"name\":\"cov-unus\"}",
             root);
    char *indexed = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool index_ok = response_contains_json_fragment(indexed, "\"status\":\"indexed\"") &&
                    response_contains_json_fragment(indexed, "\"parse_unusable\":true");

    char *cov = cbm_mcp_handle_tool(srv, "check_index_coverage",
                                    "{\"project\":\"cov-unus\",\"paths\":[\"whole.py\",\"many.py\"]}");
    bool whole_ok = coverage_path_is(cov, "whole.py", "status", "unusable") &&
                    coverage_path_is(cov, "whole.py", "recommended_action", "read_source_directly");
    bool many_ok = coverage_path_is(cov, "many.py", "status", "partial") &&
                   coverage_path_is(cov, "many.py", "parse_error_ranges_truncated", "true");
    char *dropped = coverage_path_field(cov, "many.py", "parse_error_ranges_dropped");
    bool dropped_ok = dropped && atoi(dropped) > 0;
    free(dropped);
    free(cov);

    cov = cbm_mcp_handle_tool(srv, "check_index_coverage",
                              "{\"project\":\"cov-unus\",\"scopes\":[\".\"]}");
    bool scope_ok = response_contains_json_fragment(cov, "\"unusable\":1") &&
                    response_contains_json_fragment(cov, "\"partial\":1") &&
                    response_contains_json_fragment(cov, "\"total_gaps\":2");
    free(cov);

    free(indexed);
    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    th_rmtree(root);
    th_rmtree(cache);

    ASSERT_TRUE(index_ok);
    ASSERT_TRUE(whole_ok);
    ASSERT_TRUE(many_ok);
    ASSERT_TRUE(dropped_ok);
    ASSERT_TRUE(scope_ok);
    PASS();
#endif
}

/* #12 (upstream d024f41e): coverage survives an index-mode downgrade. A
 * fast re-index of a full index keeps files that fast discovery skips (the
 * additive mode_skipped contract in pipeline_incremental.c), so their
 * recorded gaps must survive a changed incremental publish rather than
 * vanish or look healed; a real deletion still removes them. */
TEST(tool_check_index_coverage_survives_mode_downgrade) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char root[512];
    char cache[512];
    snprintf(root, sizeof(root), "%s/cbm-mcp-coverage-mode-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-coverage-mode-cache-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    ASSERT_EQ(th_write_file(TH_PATH(root, "src/ok.c"), "int helper(void) { return 42; }\n"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "examples/demo.c"), COVERAGE_BROKEN_C("demo_ok")), 0);

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    const char *cov_args = "{\"project\":\"cov-mode\",\"paths\":[\"examples/demo.c\","
                           "\"src/ok.c\"],\"scopes\":[\".\"]}";
    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"full\",\"name\":\"cov-mode\"}",
             root);
    char *full = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool full_ok = response_contains_json_fragment(full, "\"status\":\"indexed\"");
    free(full);
    char *cov = cbm_mcp_handle_tool(srv, "check_index_coverage", cov_args);
    bool full_cov = coverage_path_is(cov, "examples/demo.c", "status", "partial") &&
                    response_contains_json_fragment(cov, "\"total_gaps\":1");
    free(cov);

    /* Fast re-index with a changed file: an incremental publish that must
     * carry the fast-skipped file's row and gap forward. */
    ASSERT_EQ(th_write_file(TH_PATH(root, "src/ok.c"),
                            "int helper(void) { return 42; }\nint more(void) { return 1; }\n"),
              0);
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\",\"name\":\"cov-mode\"}",
             root);
    char *fast = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool fast_ok = response_contains_json_fragment(fast, "\"status\":\"indexed\"") &&
                   response_contains_json_fragment(fast, "\"total_gaps\":1");
    free(fast);
    cov = cbm_mcp_handle_tool(srv, "check_index_coverage", cov_args);
    bool fast_cov = coverage_path_is(cov, "examples/demo.c", "status", "partial") &&
                    coverage_path_is(cov, "examples/demo.c", "freshness", "metadata_match") &&
                    coverage_path_is(cov, "src/ok.c", "status", "no_recorded_issue") &&
                    coverage_path_is(cov, "src/ok.c", "freshness", "metadata_match") &&
                    response_contains_json_fragment(cov, "\"files\":2") &&
                    response_contains_json_fragment(cov, "\"total_gaps\":1");
    free(cov);

    /* Deleting the skipped file is a real deletion, not a mode skip. */
    ASSERT_EQ(remove(TH_PATH(root, "examples/demo.c")), 0);
    fast = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool delete_ok = response_contains_json_fragment(fast, "\"status\":\"indexed\"");
    free(fast);
    cov = cbm_mcp_handle_tool(srv, "check_index_coverage", cov_args);
    bool delete_cov = coverage_path_is(cov, "examples/demo.c", "status", "missing") &&
                      response_contains_json_fragment(cov, "\"files\":1") &&
                      response_contains_json_fragment(cov, "\"total_gaps\":0");
    free(cov);

    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    th_rmtree(root);
    th_rmtree(cache);

    ASSERT_TRUE(full_ok);
    ASSERT_TRUE(full_cov);
    ASSERT_TRUE(fast_ok);
    ASSERT_TRUE(fast_cov);
    ASSERT_TRUE(delete_ok);
    ASSERT_TRUE(delete_cov);
    PASS();
#endif
}

/* #6: compact rendering is lossless for tables with missing cells and quotes
 * strings that would read back as another type. */
TEST(mcp_render_compact_tables_and_scalars) {
    char *text = cbm_mcp_render_compact(
        "{\"a\":1,\"rows\":[{\"x\":1,\"y\":\"two\"},{\"x\":2}],\"s\":\"null\",\"n\":null,"
        "\"tags\":[\"p q\",\"r\"],\"o\":{\"k\":\"v\"}}");
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "a: 1\n"
                        "rows[2]  (x y)\n"
                        "  1 two\n"
                        "  2 -\n"
                        "s: \"null\"\n"
                        "n: null\n"
                        "tags[2]: \"p q\" r\n"
                        "o:\n"
                        "  k: v\n");
    free(text);
    ASSERT_NULL(cbm_mcp_render_compact("[1,2]"));
    PASS();
}

static char *lp_text(const char *response) {
    return extract_text_content(response);
}

static char *lp_token(const char *response) {
    char *text = lp_text(response);
    yyjson_doc *doc = text ? yyjson_read(text, strlen(text), 0) : NULL;
    free(text);
    const char *token = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "catalog_token"));
    char *out = token ? strdup(token) : NULL;
    yyjson_doc_free(doc);
    return out;
}

static int lp_publish(const char *path, const char *project, const char *root) {
    char qn[128];
    snprintf(qn, sizeof(qn), "%s.Fn", project);
    const CBMDumpNode node = {.id = 1, .project = project, .label = "Function", .name = "Fn",
                              .qualified_name = qn, .file_path = "src/fn.c", .start_line = 1,
                              .end_line = 2, .properties = "{}"};
    const cbm_zova_workspace_generation_input_t input = {
        .root_path = root, .project = project, .indexed_at = "2026-10-15T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 768,
        .nodes = &node, .node_count = 1};
    cbm_zova_workspace_generation_result_t published = {0};
    return cbm_zova_user_database_publish_workspace(path, &input, &published);
}

/* #6: list_projects pages in selector order, flags a catalog that changed
 * between pages (but not a reindex), and compact output is opt-in. */
TEST(tool_list_projects_pages_and_detects_catalog_changes) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char cache[512];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-list-pages-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    char path[512];
    ASSERT_EQ(cbm_zova_user_database_path(path, sizeof(path)), 0);
    ASSERT_EQ(lp_publish(path, "lp-gamma", "/tmp/lp/gamma"), 0);
    ASSERT_EQ(lp_publish(path, "lp-alpha", "/tmp/lp/alpha"), 0);
    ASSERT_EQ(lp_publish(path, "lp-beta", "/tmp/lp/beta"), 0);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    /* Default output is unchanged: every project, plus paging fields. */
    char *all = cbm_mcp_handle_tool(srv, "list_projects", "{}");
    char *all_text = lp_text(all);
    ASSERT_NOT_NULL(all_text);
    ASSERT_NOT_NULL(strstr(all_text, "\"total\":3"));
    ASSERT_NOT_NULL(strstr(all_text, "\"has_more\":false"));
    ASSERT_NOT_NULL(strstr(all, "structuredContent"));
    char *alpha = strstr(all_text, "\"name\":\"alpha\"");
    char *beta = strstr(all_text, "\"name\":\"beta\"");
    char *gamma = strstr(all_text, "\"name\":\"gamma\"");
    ASSERT(alpha && beta && gamma && alpha < beta && beta < gamma);
    ASSERT_NULL(strstr(all_text, "\"nodes\":"));
    free(all_text);

    char *page1 = cbm_mcp_handle_tool(srv, "list_projects", "{\"limit\":2}");
    char *text = lp_text(page1);
    ASSERT_NOT_NULL(strstr(text, "\"returned\":2"));
    ASSERT_NOT_NULL(strstr(text, "\"has_more\":true"));
    ASSERT_NOT_NULL(strstr(text, "\"next_offset\":2"));
    ASSERT_NOT_NULL(strstr(text, "\"name\":\"beta\""));
    ASSERT_NULL(strstr(text, "\"name\":\"gamma\""));
    free(text);
    char *token = lp_token(page1);
    ASSERT_NOT_NULL(token);
    free(page1);

    /* A new generation of a listed workspace keeps the pages valid. */
    ASSERT_EQ(lp_publish(path, "lp-alpha", "/tmp/lp/alpha"), 0);
    char args[256];
    snprintf(args, sizeof(args), "{\"offset\":2,\"limit\":2,\"catalog_token\":\"%s\"}", token);
    char *page2 = cbm_mcp_handle_tool(srv, "list_projects", args);
    text = lp_text(page2);
    ASSERT_NOT_NULL(strstr(text, "\"name\":\"gamma\""));
    ASSERT_NOT_NULL(strstr(text, "\"has_more\":false"));
    ASSERT_NULL(strstr(text, "catalog_changed"));
    free(text);
    free(page2);

    /* An added workspace shifts the offsets. */
    ASSERT_EQ(lp_publish(path, "lp-aardvark", "/tmp/lp/aardvark"), 0);
    page2 = cbm_mcp_handle_tool(srv, "list_projects", args);
    text = lp_text(page2);
    ASSERT_NOT_NULL(strstr(text, "\"catalog_changed\":true"));
    free(text);
    free(page2);
    free(token);

    char *details = cbm_mcp_handle_tool(srv, "list_projects",
                                        "{\"limit\":1,\"include_details\":true}");
    text = lp_text(details);
    ASSERT_NOT_NULL(strstr(text, "\"name\":\"aardvark\""));
    ASSERT_NOT_NULL(strstr(text, "\"nodes\":1"));
    free(text);
    free(details);

    /* Compact output: same rows, no structuredContent copy, fewer bytes. */
    char *json_list = cbm_mcp_handle_tool(srv, "list_projects", "{}");
    char *compact_list = cbm_mcp_handle_tool(srv, "list_projects", "{\"format\":\"compact\"}");
    ASSERT_NOT_NULL(compact_list);
    ASSERT_NULL(strstr(compact_list, "structuredContent"));
    text = lp_text(compact_list);
    ASSERT_NOT_NULL(strstr(text, "projects[4]  (name project root_path"));
    ASSERT_NOT_NULL(strstr(text, "gamma lp-gamma /tmp/lp/gamma"));
    free(text);
    printf("  list_projects bytes: json=%zu compact=%zu\n", strlen(json_list),
           strlen(compact_list));
    ASSERT(strlen(compact_list) < strlen(json_list));
    free(json_list);
    free(compact_list);

    char *json_search = cbm_mcp_handle_tool(
        srv, "search_graph", "{\"projects\":[\"*\"],\"name_pattern\":\".*\",\"limit\":10}");
    char *compact_search = cbm_mcp_handle_tool(
        srv, "search_graph",
        "{\"projects\":[\"*\"],\"name_pattern\":\".*\",\"limit\":10,\"format\":\"compact\"}");
    ASSERT_NOT_NULL(compact_search);
    ASSERT_NULL(strstr(compact_search, "isError\":true"));
    ASSERT_NOT_NULL(strstr(compact_search, "lp-gamma.Fn"));
    printf("  search_graph bytes: json=%zu compact=%zu\n", strlen(json_search),
           strlen(compact_search));
    ASSERT(strlen(compact_search) < strlen(json_search));
    free(json_search);
    free(compact_search);

    char *bad = cbm_mcp_handle_tool(srv, "list_projects", "{\"format\":\"xml\"}");
    ASSERT_NOT_NULL(strstr(bad, "format must be"));
    ASSERT_NOT_NULL(strstr(bad, "\"isError\":true"));
    free(bad);

    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    th_rmtree(cache);
    PASS();
#endif
}

/* ADR-0001: index_repository refuses with a machine-readable busy result while
 * another process (or this one) holds the workspace lease, and delete_project
 * refuses while an index run holds it. The holder dying frees the workspace. */
TEST(tool_index_repository_refuses_while_workspace_lease_is_held) {
#if !CBM_WITH_ZOVA || defined(_WIN32)
    PASS();
#else
    char root[512], cache[512];
    snprintf(root, sizeof(root), "%s/cbm-mcp-lease-root-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-lease-cache-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    char source_path[640];
    snprintf(source_path, sizeof(source_path), "%s/main.c", root);
    FILE *source = fopen(source_path, "wb");
    ASSERT_NOT_NULL(source);
    fputs("int main(void) { return 0; }\n", source);
    ASSERT_EQ(fclose(source), 0);

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    const char *saved_wait = getenv("CBM_INDEX_LEASE_TIMEOUT_S");
    char *saved_wait_copy = saved_wait ? strdup(saved_wait) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_setenv("CBM_INDEX_LEASE_TIMEOUT_S", "0", 1);
    char user_db[640];
    ASSERT_EQ(cbm_zova_user_database_path(user_db, sizeof(user_db)), 0);

    int ready[2];
    ASSERT_EQ(pipe(ready), 0);
    pid_t holder = fork();
    ASSERT_TRUE(holder >= 0);
    if (holder == 0) {
        close(ready[0]);
        cbm_zova_index_hold_t hold;
        if (cbm_mcp_index_hold_acquire(root, 0, &hold, NULL, 0) != CBM_ZOVA_COORD_OK) _exit(10);
        (void)write(ready[1], "R", 1);
        for (;;) pause();
    }
    close(ready[1]);
    char byte = 0;
    ssize_t got = read(ready[0], &byte, 1);
    close(ready[0]);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\",\"name\":\"lease\"}",
             root);
    char *refused = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool refused_busy = refused &&
                        response_contains_json_fragment(refused, "\"status\":\"busy\"") &&
                        response_contains_json_fragment(refused, "\"reason\":\"workspace_busy\"");
    bool untouched = access(user_db, F_OK) != 0;
    kill(holder, SIGKILL);
    waitpid(holder, NULL, 0);

    char *indexed = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool indexed_ok =
        indexed && response_contains_json_fragment(indexed, "\"status\":\"indexed\"");

    cbm_zova_index_hold_t hold;
    char workspace_id[64] = {0};
    cbm_zova_coord_status_t held =
        cbm_mcp_index_hold_acquire(root, 0, &hold, workspace_id, sizeof(workspace_id));
    char *in_process = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool in_process_busy =
        in_process &&
        response_contains_json_fragment(in_process, "\"reason\":\"workspace_busy\"");
    char *delete_refused = cbm_mcp_handle_tool(srv, "delete_project", "{\"project\":\"lease\"}");
    bool delete_busy =
        delete_refused &&
        response_contains_json_fragment(delete_refused, "\"status\":\"delete_failed\"") &&
        response_contains_json_fragment(delete_refused, "\"error\":\"workspace_busy\"");
    cbm_zova_index_hold_release(&hold);
    char *deleted = cbm_mcp_handle_tool(srv, "delete_project", "{\"project\":\"lease\"}");
    bool deleted_ok = deleted && response_contains_json_fragment(deleted, "\"status\":\"deleted\"");

    free(refused);
    free(indexed);
    free(in_process);
    free(delete_refused);
    free(deleted);
    cbm_mcp_server_free(srv);
    const char *suffixes[] = {"", "-wal", "-shm", ".writer.lock", ".prepare.lock",
                              ".prepare.turnstile.lock"};
    char file[768];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        snprintf(file, sizeof(file), "%s%s", user_db, suffixes[i]);
        cbm_unlink(file);
    }
    snprintf(file, sizeof(file), "%s.ws-%s.lock", user_db, workspace_id);
    cbm_unlink(file);
    cbm_unlink(source_path);
    cbm_rmdir(root);
    cbm_rmdir(cache);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    if (saved_wait_copy) cbm_setenv("CBM_INDEX_LEASE_TIMEOUT_S", saved_wait_copy, 1);
    else cbm_unsetenv("CBM_INDEX_LEASE_TIMEOUT_S");
    free(saved_wait_copy);

    ASSERT_EQ(got, 1);
    ASSERT_TRUE(refused_busy);
    ASSERT_TRUE(untouched);
    ASSERT_TRUE(indexed_ok);
    ASSERT_EQ(held, CBM_ZOVA_COORD_OK);
    ASSERT_TRUE(in_process_busy);
    ASSERT_TRUE(delete_busy);
    ASSERT_TRUE(deleted_ok);
    PASS();
#endif
}

/* Forward declarations for helpers defined later in this file */
static cbm_mcp_server_t *setup_snippet_server(char *tmp_dir, size_t tmp_sz);
static void cleanup_snippet_dir(const char *tmp_dir);
static char *extract_text_content(const char *mcp_result);

TEST(tool_search_graph_includes_node_properties) {
    /* search_graph results must surface each node's properties_json
     * payload so callers don't have to round-trip through get_code_snippet
     * just to read them. The setup_snippet_server inserts HandleRequest
     * with a signature/return_type/is_exported property blob; this test
     * pins that those keys reach the MCP response. */
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\","
             "\"arguments\":{\"project\":\"test-project\",\"label\":\"Function\","
             "\"name_pattern\":\"HandleRequest\",\"limit\":5}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    /* Properties from HandleRequest's properties_json must appear. */
    ASSERT_NOT_NULL(strstr(inner, "signature"));
    ASSERT_NOT_NULL(strstr(inner, "func HandleRequest"));
    ASSERT_NOT_NULL(strstr(inner, "is_exported"));
    ASSERT_NOT_NULL(strstr(inner, "\"tags\":[\"http\",\"public\"]"));
    ASSERT_NOT_NULL(strstr(inner, "\"metadata\":{\"owner\":\"api\"}"));
    free(inner);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_search_graph_query_honors_file_pattern_issue552) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "issue-552";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/issue-552");

    cbm_node_t lib_status = {0};
    lib_status.project = proj;
    lib_status.label = "Function";
    lib_status.name = "status";
    lib_status.qualified_name = "issue-552.src.lib.status";
    lib_status.file_path = "src/lib/status.c";
    lib_status.start_line = 1;
    lib_status.end_line = 3;
    ASSERT_GT(cbm_store_upsert_node(st, &lib_status), 0);

    cbm_node_t component_status = {0};
    component_status.project = proj;
    component_status.label = "Function";
    component_status.name = "status";
    component_status.qualified_name = "issue-552.src.components.status";
    component_status.file_path = "src/components/status.c";
    component_status.start_line = 1;
    component_status.end_line = 3;
    ASSERT_GT(cbm_store_upsert_node(st, &component_status), 0);

    cbm_store_exec(st, "INSERT INTO nodes_fts(nodes_fts) VALUES('delete-all');");
    ASSERT_EQ(cbm_store_exec(st,
                             "INSERT INTO nodes_fts(rowid, name, qualified_name, label, "
                             "file_path) "
                             "SELECT id, cbm_camel_split(name), qualified_name, label, file_path "
                             "FROM nodes;"),
              CBM_STORE_OK);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":552,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_graph\","
                                   "\"arguments\":{\"project\":\"issue-552\",\"query\":\"status\","
                                   "\"file_pattern\":\"src/lib/*\",\"limit\":10}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"search_mode\":\"bm25\""));
    ASSERT_NOT_NULL(strstr(inner, "\"file_path\":\"src/lib/status.c\""));
    ASSERT_NULL(strstr(inner, "src/components/status.c"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_query_graph_basic) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"query_graph\","
             "\"arguments\":{\"query\":\"MATCH (f:Function) RETURN f.name\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Upstream #1196: max_rows limits projected rows only. Two caps applied it to
 * the input instead, so count() saw a prefix: the unlabeled scan read only
 * max_rows*10 nodes, and each hop kept only bind_cap*10 expanded rows even
 * when the source was labeled. 2 Class sources x 30 CALLS = 60 edges and 32
 * nodes, queried with max_rows=2: the caps reported 20 nodes and 20 edges. */
TEST(tool_query_graph_count_ignores_max_rows) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "count-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/count");

    int64_t callee[30];
    for (int i = 0; i < 30; i++) {
        char name[8];
        char qn[32];
        snprintf(name, sizeof(name), "f%02d", i);
        snprintf(qn, sizeof(qn), "count-proj.f%02d", i);
        cbm_node_t n = {.project = proj,
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "callee.c",
                        .start_line = i + 1,
                        .end_line = i + 1};
        callee[i] = cbm_store_upsert_node(st, &n);
    }
    for (int c = 0; c < 2; c++) {
        char name[8];
        char qn[32];
        snprintf(name, sizeof(name), "C%d", c);
        snprintf(qn, sizeof(qn), "count-proj.C%d", c);
        cbm_node_t n = {.project = proj,
                        .label = "Class",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "caller.c",
                        .start_line = c + 1,
                        .end_line = c + 1};
        int64_t id = cbm_store_upsert_node(st, &n);
        for (int i = 0; i < 30; i++) {
            cbm_edge_t e = {
                .project = proj, .source_id = id, .target_id = callee[i], .type = "CALLS"};
            cbm_store_insert_edge(st, &e);
        }
    }

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    const char *cases[][2] = {
        {"MATCH (n) RETURN count(n) AS n", "[\"32\"]"},
        {"MATCH (a:Class)-[r:CALLS]->(b) RETURN count(r) AS n", "[\"60\"]"},
        {"MATCH (a)-[r:CALLS]->(b) RETURN count(r) AS n", "[\"60\"]"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char req[512];
        snprintf(req, sizeof(req),
                 "{\"jsonrpc\":\"2.0\",\"id\":72,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"query_graph\",\"arguments\":{\"project\":\"count-proj\","
                 "\"query\":\"%s\",\"max_rows\":2}}}",
                 cases[i][0]);
        char *resp = cbm_mcp_server_handle(srv, req);
        ASSERT_NOT_NULL(resp);
        char *inner = extract_text_content(resp);
        ASSERT_NOT_NULL(inner);
        if (!strstr(inner, cases[i][1])) {
            printf("  %s -> %s\n", cases[i][0], inner);
        }
        ASSERT_NOT_NULL(strstr(inner, cases[i][1]));
        free(inner);
        free(resp);
    }

    /* The list form still stops at max_rows. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":73,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"query_graph\",\"arguments\":{\"project\":\"count-proj\","
             "\"query\":\"MATCH (a:Class)-[r:CALLS]->(b) RETURN a.name, b.name\","
             "\"max_rows\":2}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"total\":2"));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_status_no_project) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_status\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or empty status */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_status_includes_git_metadata) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_status\","
                                   "\"arguments\":{\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"root_path\""));
    ASSERT_NOT_NULL(strstr(inner, "\"git\""));
    ASSERT_NOT_NULL(strstr(inner, "\"is_git\":false"));
    ASSERT_NOT_NULL(strstr(inner, "\"root_exists\":true"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  TOOL HANDLERS WITH DATA
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_trace_call_path_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"trace_call_path\","
                                   "\"arguments\":{\"function_name\":\"NonExistent\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about project not found */
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_trace_missing_function_name) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"trace_call_path\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression: two same-named definitions with equal rank must be reported
 * ambiguous, not silently traced (trace_path previously took nodes[0]). */
TEST(tool_trace_call_path_ambiguous) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "amb-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/amb");
    cbm_node_t a = {.project = proj,
                    .label = "Function",
                    .name = "amb",
                    .qualified_name = "amb-proj.a.amb",
                    .file_path = "a.c",
                    .start_line = 10,
                    .end_line = 20};
    cbm_node_t b = {.project = proj,
                    .label = "Function",
                    .name = "amb",
                    .qualified_name = "amb-proj.b.amb",
                    .file_path = "b.c",
                    .start_line = 10,
                    .end_line = 20}; /* equal span -> genuine tie */
    ASSERT_GT(cbm_store_upsert_node(st, &b), 0);
    ASSERT_GT(cbm_store_upsert_node(st, &a), 0);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":61,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\","
             "\"arguments\":{\"function_name\":\"amb\",\"project\":\"amb-proj\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "ambiguous"));
    ASSERT_NOT_NULL(strstr(inner, "suggestions"));
    ASSERT_NULL(strstr(inner, "\"callees\""));
    const char *a_suggestion = strstr(inner, "amb-proj.a.amb");
    const char *b_suggestion = strstr(inner, "amb-proj.b.amb");
    ASSERT_NOT_NULL(a_suggestion);
    ASSERT_NOT_NULL(b_suggestion);
    ASSERT_TRUE(a_suggestion < b_suggestion);
    free(inner);
    free(resp);

    /* file_path picks one of the tied definitions; a file that defines
     * neither is an error, not an empty trace. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":62,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":"
             "\"amb\",\"project\":\"amb-proj\",\"file_path\":\"b.c\"}}}");
    inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NULL(strstr(inner, "ambiguous"));
    ASSERT_NOT_NULL(strstr(inner, "\"function\":\"amb\""));
    free(inner);
    free(resp);
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":63,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":"
             "\"amb\",\"project\":\"amb-proj\",\"file_path\":\"c.c\"}}}");
    inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "not defined in file_path c.c"));
    free(inner);
    free(resp);
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":64,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_code_snippet\",\"arguments\":{\"qualified_name\":"
             "\"amb\",\"project\":\"amb-proj\",\"file_path\":\"a.c\"}}}");
    inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NULL(strstr(inner, "ambiguous"));
    ASSERT_NOT_NULL(strstr(inner, "amb-proj.a.amb"));
    ASSERT_NULL(strstr(inner, "amb-proj.b.amb"));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression: when same-named nodes differ in rank, trace must pick the real
 * definition (callable, larger body) — NOT nodes[0]. The Module is inserted
 * first; if trace took nodes[0] the outbound trace would be empty. */
TEST(tool_trace_call_path_prefers_definition) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "pref-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/pref");
    /* nodes[0]: the WRONG match (a Module, tiny span), inserted first. */
    cbm_node_t wrong = {.project = proj,
                        .label = "Module",
                        .name = "dup",
                        .qualified_name = "pref-proj.dup",
                        .file_path = "dup.x",
                        .start_line = 1,
                        .end_line = 1};
    /* the real definition: a Function with a body. */
    cbm_node_t def = {.project = proj,
                      .label = "Function",
                      .name = "dup",
                      .qualified_name = "pref-proj.src.dup",
                      .file_path = "src/dup.c",
                      .start_line = 10,
                      .end_line = 50};
    cbm_node_t callee = {.project = proj,
                         .label = "Function",
                         .name = "callee",
                         .qualified_name = "pref-proj.src.callee",
                         .file_path = "src/dup.c",
                         .start_line = 60,
                         .end_line = 70};
    ASSERT_GT(cbm_store_upsert_node(st, &wrong), 0);
    int64_t id_def = cbm_store_upsert_node(st, &def);
    int64_t id_callee = cbm_store_upsert_node(st, &callee);
    ASSERT_GT(id_def, 0);
    ASSERT_GT(id_callee, 0);
    cbm_edge_t e = {.project = proj, .source_id = id_def, .target_id = id_callee, .type = "CALLS"};
    cbm_store_insert_edge(st, &e);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":62,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":\"dup\","
             "\"project\":\"pref-proj\",\"direction\":\"outbound\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NULL(strstr(inner, "ambiguous"));
    /* picked the Function definition -> its outbound CALLS edge to "callee" shows */
    ASSERT_NOT_NULL(strstr(inner, "callee"));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Upstream cc7b4a88 (#1294): a caller under a project-root tests/ tree
 * (no test_ naming) is hidden from trace_call_path by default. The path
 * filter only matched a nested ".../tests/..." path. */
TEST(tool_trace_call_path_hides_root_tests_subtree_issue1294) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "t1294-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/t1294");
    cbm_node_t tgt = {.project = proj, .label = "Function", .name = "tgt1294",
                      .qualified_name = "t1294-proj.a.tgt1294", .file_path = "a.c",
                      .start_line = 1, .end_line = 5};
    cbm_node_t prod = {.project = proj, .label = "Function", .name = "prod_caller",
                       .qualified_name = "t1294-proj.a.prod_caller", .file_path = "a.c",
                       .start_line = 10, .end_line = 15};
    cbm_node_t tst = {.project = proj, .label = "Function", .name = "repro_caller",
                      .qualified_name = "t1294-proj.t.repro_caller",
                      .file_path = "tests/repro/helper.c", .start_line = 1, .end_line = 5};
    int64_t tid = cbm_store_upsert_node(st, &tgt);
    int64_t pid = cbm_store_upsert_node(st, &prod);
    int64_t xid = cbm_store_upsert_node(st, &tst);
    ASSERT_GT(tid, 0);
    ASSERT_GT(pid, 0);
    ASSERT_GT(xid, 0);
    cbm_edge_t e1 = {.project = proj, .source_id = pid, .target_id = tid, .type = "CALLS"};
    cbm_edge_t e2 = {.project = proj, .source_id = xid, .target_id = tid, .type = "CALLS"};
    ASSERT_GT(cbm_store_insert_edge(st, &e1), 0);
    ASSERT_GT(cbm_store_insert_edge(st, &e2), 0);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(st, proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"trace_call_path\",\"arguments\":{\"project\":\"t1294-proj\","
             "\"function_name\":\"tgt1294\",\"direction\":\"inbound\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    free(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "prod_caller"));
    ASSERT_NULL(strstr(inner, "repro_caller"));
    free(inner);

    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"trace_call_path\",\"arguments\":{\"project\":\"t1294-proj\","
             "\"function_name\":\"tgt1294\",\"direction\":\"inbound\","
             "\"include_tests\":true}}}");
    ASSERT_NOT_NULL(resp);
    inner = extract_text_content(resp);
    free(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "prod_caller"));
    ASSERT_NOT_NULL(strstr(inner, "repro_caller"));
    free(inner);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first (#887): the client-supplied `depth` on trace_call_path must be
 * clamped to the MCP ceiling (cbm_mcp_max_depth(), default 15). On origin/main
 * an MCP_MAX_DEPTH=15 constant was defined but never applied — `depth` flowed
 * straight into bfs_union_same_name, so an unbounded value drives the shared
 * cbm_store_bfs to arbitrary depth. Over an 18-node call chain, depth=1000
 * reaches n16/n17 (RED); with the clamp the walk stops at hop 15, so n15 is
 * reached but n16 is not (GREEN). Quoted tokens ("n15"/"n16") match only the
 * node-name field, never the qualified_name (preceded by '.'), so the boundary
 * check is exact. */
TEST(tool_trace_call_path_depth_clamped) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "depth-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/depth");

    /* Linear call chain n00 -CALLS-> n01 -> ... -> n17 (18 nodes). */
    int64_t ids[18];
    for (int i = 0; i < 18; i++) {
        char name[8];
        char qn[32];
        snprintf(name, sizeof(name), "n%02d", i);
        snprintf(qn, sizeof(qn), "depth-proj.n%02d", i);
        cbm_node_t n = {.project = proj,
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "chain.c",
                        .start_line = 1,
                        .end_line = 2};
        ids[i] = cbm_store_upsert_node(st, &n);
    }
    for (int i = 0; i < 17; i++) {
        cbm_edge_t e = {
            .project = proj, .source_id = ids[i], .target_id = ids[i + 1], .type = "CALLS"};
        cbm_store_insert_edge(st, &e);
    }

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":71,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":\"n00\","
             "\"project\":\"depth-proj\",\"direction\":\"outbound\",\"depth\":1000}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* Reached within the ceiling (proves the traversal ran) but clamped at 15. */
    ASSERT_NOT_NULL(strstr(inner, "\"n15\""));
    ASSERT_NULL(strstr(inner, "\"n16\""));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first (#650, distilled): two GENUINELY-DIFFERENT same-named functions
 * whose bodies differ in length score differently, so the old exact-tie check did
 * not flag them ambiguous — and bfs_union_same_name (#546) then merged the caller
 * sets of both into one confidently-conflated answer (the mirror of #546's under-
 * report). The fix: 2+ real callable defs => ambiguous (disambiguate), never union
 * distinct symbols. RED before the pick_resolved_node real_def_count rule (response
 * merged callerA+callerB), GREEN after (response is ambiguous, no "callers"). */
TEST(tool_trace_call_path_distinct_defs_not_over_unioned) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "ou-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/ou");
    /* two unrelated real definitions of "dupreal", DIFFERENT body spans */
    cbm_node_t da = {.project = proj,
                     .label = "Function",
                     .name = "dupreal",
                     .qualified_name = "ou-proj.a.dupreal",
                     .file_path = "a.c",
                     .start_line = 10,
                     .end_line = 20}; /* span 10 */
    cbm_node_t db = {.project = proj,
                     .label = "Function",
                     .name = "dupreal",
                     .qualified_name = "ou-proj.b.dupreal",
                     .file_path = "b.c",
                     .start_line = 10,
                     .end_line = 40}; /* span 30 (no tie) */
    cbm_node_t ca = {.project = proj,
                     .label = "Function",
                     .name = "callerA",
                     .qualified_name = "ou-proj.a.callerA",
                     .file_path = "a.c",
                     .start_line = 30,
                     .end_line = 40};
    cbm_node_t cb = {.project = proj,
                     .label = "Function",
                     .name = "callerB",
                     .qualified_name = "ou-proj.b.callerB",
                     .file_path = "b.c",
                     .start_line = 50,
                     .end_line = 60};
    int64_t id_da = cbm_store_upsert_node(st, &da);
    int64_t id_db = cbm_store_upsert_node(st, &db);
    int64_t id_ca = cbm_store_upsert_node(st, &ca);
    int64_t id_cb = cbm_store_upsert_node(st, &cb);
    ASSERT_GT(id_da, 0);
    ASSERT_GT(id_db, 0);
    ASSERT_GT(id_ca, 0);
    ASSERT_GT(id_cb, 0);
    cbm_edge_t ea = {.project = proj, .source_id = id_ca, .target_id = id_da, .type = "CALLS"};
    cbm_edge_t eb = {.project = proj, .source_id = id_cb, .target_id = id_db, .type = "CALLS"};
    cbm_store_insert_edge(st, &ea);
    cbm_store_insert_edge(st, &eb);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv,
        "{\"jsonrpc\":\"2.0\",\"id\":63,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":\"dupreal\","
        "\"project\":\"ou-proj\",\"direction\":\"inbound\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    /* distinct symbols must be disambiguated, not merged into one caller set */
    ASSERT_NOT_NULL(strstr(inner, "ambiguous"));
    ASSERT_NOT_NULL(strstr(inner, "suggestions"));
    ASSERT_NULL(strstr(inner, "\"callers\""));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Guard that the ambiguity gate does NOT regress the #546 fix: a real .ts
 * implementation plus a body-less ambient .d.ts stub is ONE logical symbol
 * (one real callable def + a fragment), so it must stay non-ambiguous and the
 * caller sets from both nodes must be unioned. */
TEST(tool_trace_call_path_dts_stub_unions_with_impl) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "dts-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/dts");
    cbm_node_t impl = {.project = proj,
                       .label = "Function",
                       .name = "sym546",
                       .qualified_name = "dts-proj.impl.sym546",
                       .file_path = "src/sym.ts",
                       .start_line = 10,
                       .end_line = 30}; /* real body */
    cbm_node_t stub = {.project = proj,
                       .label = "Function",
                       .name = "sym546",
                       .qualified_name = "dts-proj.stub.sym546",
                       .file_path = "types/sym.d.ts",
                       .start_line = 5,
                       .end_line = 5}; /* body-less ambient decl */
    cbm_node_t crel = {.project = proj,
                       .label = "Function",
                       .name = "callerRel",
                       .qualified_name = "dts-proj.callerRel",
                       .file_path = "src/rel.ts",
                       .start_line = 1,
                       .end_line = 8};
    cbm_node_t cali = {.project = proj,
                       .label = "Function",
                       .name = "callerAlias",
                       .qualified_name = "dts-proj.callerAlias",
                       .file_path = "src/ali.ts",
                       .start_line = 1,
                       .end_line = 8};
    int64_t id_impl = cbm_store_upsert_node(st, &impl);
    int64_t id_stub = cbm_store_upsert_node(st, &stub);
    int64_t id_crel = cbm_store_upsert_node(st, &crel);
    int64_t id_cali = cbm_store_upsert_node(st, &cali);
    ASSERT_GT(id_impl, 0);
    ASSERT_GT(id_stub, 0);
    ASSERT_GT(id_crel, 0);
    ASSERT_GT(id_cali, 0);
    /* callers split by import style: relative -> impl, path-alias -> stub */
    cbm_edge_t er = {.project = proj, .source_id = id_crel, .target_id = id_impl, .type = "CALLS"};
    cbm_edge_t el = {.project = proj, .source_id = id_cali, .target_id = id_stub, .type = "CALLS"};
    cbm_store_insert_edge(st, &er);
    cbm_store_insert_edge(st, &el);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":64,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":\"sym546\","
             "\"project\":\"dts-proj\",\"direction\":\"inbound\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NULL(strstr(inner, "ambiguous"));
    /* union across impl + stub: BOTH callers appear (this is the #546 fix) */
    ASSERT_NOT_NULL(strstr(inner, "callerRel"));
    ASSERT_NOT_NULL(strstr(inner, "callerAlias"));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Same-name union must preserve the public traversal ordering after it merges
 * per-root BFS results. The implementation is inserted first and reaches Zeta;
 * the stub is inserted second and reaches Alpha, so append-order is wrong. */
TEST(tool_trace_call_path_same_name_union_orders_visits) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "ordered-union";
    cbm_mcp_server_set_project(srv, proj);
    ASSERT_EQ(cbm_store_upsert_project(st, proj, "/tmp/ordered-union"), CBM_STORE_OK);

    cbm_node_t impl = {.project = proj,
                       .label = "Function",
                       .name = "shared",
                       .qualified_name = "ordered.impl.shared",
                       .file_path = "src/shared.ts",
                       .start_line = 10,
                       .end_line = 30};
    cbm_node_t stub = {.project = proj,
                       .label = "Function",
                       .name = "shared",
                       .qualified_name = "ordered.stub.shared",
                       .file_path = "types/shared.d.ts",
                       .start_line = 5,
                       .end_line = 5};
    cbm_node_t zeta = {.project = proj,
                       .label = "Function",
                       .name = "Zeta",
                       .qualified_name = "ordered.Zeta",
                       .file_path = "zeta.ts",
                       .start_line = 1,
                       .end_line = 4};
    cbm_node_t alpha = {.project = proj,
                        .label = "Function",
                        .name = "Alpha",
                        .qualified_name = "ordered.Alpha",
                        .file_path = "alpha.ts",
                        .start_line = 1,
                        .end_line = 4};
    int64_t impl_id = cbm_store_upsert_node(st, &impl);
    int64_t stub_id = cbm_store_upsert_node(st, &stub);
    int64_t zeta_id = cbm_store_upsert_node(st, &zeta);
    int64_t alpha_id = cbm_store_upsert_node(st, &alpha);
    ASSERT_GT(impl_id, 0);
    ASSERT_GT(stub_id, 0);
    ASSERT_GT(zeta_id, 0);
    ASSERT_GT(alpha_id, 0);
    cbm_edge_t impl_zeta = {.project = proj, .source_id = impl_id, .target_id = zeta_id,
                             .type = "CALLS"};
    cbm_edge_t stub_alpha = {.project = proj, .source_id = stub_id, .target_id = alpha_id,
                              .type = "CALLS"};
    ASSERT_GT(cbm_store_insert_edge(st, &impl_zeta), 0);
    ASSERT_GT(cbm_store_insert_edge(st, &stub_alpha), 0);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":65,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":\"shared\","
             "\"project\":\"ordered-union\",\"direction\":\"outbound\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    const char *alpha_pos = strstr(inner, "\"name\":\"Alpha\"");
    const char *zeta_pos = strstr(inner, "\"name\":\"Zeta\"");
    ASSERT_NOT_NULL(alpha_pos);
    ASSERT_NOT_NULL(zeta_pos);
    ASSERT_TRUE(alpha_pos < zeta_pos);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_delete_project_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"delete_project\","
                                   "\"arguments\":{\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not_found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_architecture_empty) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_architecture\","
                                   "\"arguments\":{\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* No store for nonexistent project — should return project error */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression for #281: handle_get_architecture must actually call
 * cbm_store_get_architecture and surface its sections. Before the fix
 * only label/edge histograms were emitted regardless of which aspects
 * were requested. The store-side arch_entry_points query reads
 * properties.is_entry_point on Function nodes, so we tag one node and
 * assert the resulting JSON surfaces an "entry_points" array containing
 * the tagged function — which is impossible without the wiring. */
TEST(tool_get_architecture_emits_populated_sections) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch-test";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch-test");

    cbm_node_t main_fn = {0};
    main_fn.project = proj;
    main_fn.label = "Function";
    main_fn.name = "main";
    main_fn.qualified_name = "arch-test.cmd.main";
    main_fn.file_path = "cmd/main.go";
    main_fn.start_line = 1;
    main_fn.end_line = 3;
    main_fn.properties_json = "{\"is_entry_point\":true}";
    ASSERT_GT(cbm_store_upsert_node(st, &main_fn), 0);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":91,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch-test\",\"aspects\":[\"all\"]}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* The handler always emits node/edge counts and schema histograms;
     * those existed before #281. The "entry_points" array only appears
     * when cbm_store_get_architecture is actually called and its result
     * is serialized — which is exactly what #281 wires up. */
    ASSERT_NOT_NULL(strstr(inner, "\"entry_points\""));
    ASSERT_NOT_NULL(strstr(inner, "main"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Distills PR #560 (overview subset): "overview" must expand to a compact
 * subset — every aspect EXCEPT file_tree. Before the fix, "overview" was not
 * registered in either aspect gate (want_aspect in store.c, aspect_wanted in
 * mcp.c), so aspects=["overview"] silently degraded to just
 * {total_nodes,total_edges}. RED on unfixed code: no "entry_points" key. */
TEST(tool_get_architecture_overview_compact_subset_pr560) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch560";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch560");

    cbm_node_t main_fn = {0};
    main_fn.project = proj;
    main_fn.label = "Function";
    main_fn.name = "main";
    main_fn.qualified_name = "arch560.cmd.main";
    main_fn.file_path = "cmd/main.go";
    main_fn.start_line = 1;
    main_fn.end_line = 3;
    main_fn.properties_json = "{\"is_entry_point\":true}";
    ASSERT_GT(cbm_store_upsert_node(st, &main_fn), 0);

    /* A File node so the file_tree aspect has real content — makes the
     * "overview drops file_tree" assertion below non-vacuous. */
    cbm_node_t file_node = {.project = proj,
                            .label = "File",
                            .name = "main.go",
                            .qualified_name = "arch560.cmd.main.go",
                            .file_path = "cmd/main.go"};
    ASSERT_GT(cbm_store_upsert_node(st, &file_node), 0);

    /* Sanity: with "all", both entry_points and file_tree surface. */
    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp_all = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":560,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch560\",\"aspects\":[\"all\"]}}}");
    ASSERT_NOT_NULL(resp_all);
    char *inner_all = extract_text_content(resp_all);
    ASSERT_NOT_NULL(inner_all);
    ASSERT_NOT_NULL(strstr(inner_all, "\"entry_points\""));
    ASSERT_NOT_NULL(strstr(inner_all, "\"file_tree\""));
    free(inner_all);
    free(resp_all);

    /* "overview": substantive content (entry_points, node_labels) but NO
     * file_tree section. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":561,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch560\",\"aspects\":[\"overview\"]}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"entry_points\""));
    ASSERT_NOT_NULL(strstr(inner, "\"node_labels\""));
    ASSERT_NULL(strstr(inner, "\"file_tree\""));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Distills PR #560 (server-side validation): unknown aspect tokens must be
 * rejected with an isError result listing the valid values. Before the fix
 * the JSON-Schema accepted any string and both aspect gates simply never
 * matched, so a typo like "bogus_aspect" produced a silent near-empty payload
 * with isError:false. RED on unfixed code: no isError, no "Unknown aspect". */
TEST(tool_get_architecture_rejects_unknown_aspect_pr560) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch560v";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch560v");

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":562,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch560v\",\"aspects\":[\"bogus_aspect\"]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(resp, "Unknown aspect 'bogus_aspect'"));
    /* The error must teach the valid vocabulary, including the new token. */
    ASSERT_NOT_NULL(strstr(resp, "overview"));
    ASSERT_NOT_NULL(strstr(resp, "file_tree"));

    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first for #640: query handlers must accept the `project_name`
 * alias, not only the canonical `project` key. list_projects surfaces the field
 * as "name" and the error hint says "pass the project name", so a caller
 * naturally passes `project_name`. With no alias, the handler reads key
 * "project" -> NULL -> resolve_store bails before opening the store -> "project
 * not found or not indexed" even though the project is indexed. Mirrors
 * tool_get_architecture_emits_populated_sections but with the alias key. */
TEST(tool_get_architecture_accepts_project_name_alias_issue640) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "alias640";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/alias640");

    cbm_node_t main_fn = {0};
    main_fn.project = proj;
    main_fn.label = "Function";
    main_fn.name = "main";
    main_fn.qualified_name = "alias640.cmd.main";
    main_fn.file_path = "cmd/main.go";
    main_fn.start_line = 1;
    main_fn.end_line = 3;
    main_fn.properties_json = "{\"is_entry_point\":true}";
    ASSERT_GT(cbm_store_upsert_node(st, &main_fn), 0);

    /* Caller passes `project_name` (the natural guess) instead of `project`. */
    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":640,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project_name\":\"alias640\",\"aspects\":[\"all\"]}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* RED before the alias: inner is the "project not found" error.
     * GREEN after: the alias resolves and architecture sections surface. */
    ASSERT_NULL(strstr(inner, "project not found"));
    ASSERT_NOT_NULL(strstr(inner, "\"entry_points\""));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first for #640: the alias must apply across query handlers, not
 * just get_architecture. search_graph with `project_name` must resolve too. */
TEST(tool_search_graph_accepts_project_name_alias_issue640) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "alias640b";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/alias640b");

    cbm_node_t fn = {0};
    fn.project = proj;
    fn.label = "Function";
    fn.name = "WidgetHandler";
    fn.qualified_name = "alias640b.svc.WidgetHandler";
    fn.file_path = "svc/widget.go";
    fn.start_line = 1;
    fn.end_line = 2;
    ASSERT_GT(cbm_store_upsert_node(st, &fn), 0);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":641,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\","
             "\"arguments\":{\"project_name\":\"alias640b\",\"name_pattern\":\"Widget.*\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    ASSERT_NULL(strstr(inner, "project not found"));
    ASSERT_NOT_NULL(strstr(inner, "WidgetHandler"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression for #604: path scopes architecture totals and content. */
TEST(tool_get_architecture_path_scoping) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch-path";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch-path");

    cbm_node_t pkg_global = {.project = proj,
                             .label = "Package",
                             .name = "Django",
                             .qualified_name = "arch-path.Django",
                             .file_path = "vendor/django/__init__.py"};
    cbm_store_upsert_node(st, &pkg_global);

    cbm_node_t pkg_local = {.project = proj,
                            .label = "Package",
                            .name = "hoa",
                            .qualified_name = "arch-path.hoa",
                            .file_path = "apps/hoa/main.go"};
    cbm_store_upsert_node(st, &pkg_local);

    cbm_node_t f_hoa = {.project = proj,
                        .label = "File",
                        .name = "main.go",
                        .qualified_name = "arch-path.apps.hoa.main.go",
                        .file_path = "apps/hoa/main.go"};
    cbm_store_upsert_node(st, &f_hoa);

    cbm_node_t f_other = {.project = proj,
                          .label = "File",
                          .name = "other.go",
                          .qualified_name = "arch-path.other.go",
                          .file_path = "lib/other.go"};
    cbm_store_upsert_node(st, &f_other);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp_root = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch-path\",\"aspects\":[\"packages\"]}}}");
    ASSERT_NOT_NULL(resp_root);
    char *inner_root = extract_text_content(resp_root);
    ASSERT_NOT_NULL(inner_root);
    ASSERT_NOT_NULL(strstr(inner_root, "Django"));

    char *resp_scoped =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_architecture\","
                                   "\"arguments\":{\"project\":\"arch-path\",\"path\":\"apps/hoa\","
                                   "\"aspects\":[\"packages\"]}}}");
    ASSERT_NOT_NULL(resp_scoped);
    char *inner_scoped = extract_text_content(resp_scoped);
    ASSERT_NOT_NULL(inner_scoped);

    ASSERT_NOT_NULL(strstr(inner_scoped, "root_total_nodes"));
    ASSERT_NOT_NULL(strstr(inner_scoped, "scoped_total_nodes"));
    ASSERT_NOT_NULL(strstr(inner_scoped, "\"path\""));
    ASSERT_NOT_NULL(strstr(inner_scoped, "hoa"));
    ASSERT_NULL(strstr(inner_scoped, "Django"));

    int root_nodes = 0;
    int scoped_nodes = 0;
    const char *rt = strstr(inner_scoped, "\"root_total_nodes\":");
    const char *stn = strstr(inner_scoped, "\"scoped_total_nodes\":");
    if (rt) {
        sscanf(rt, "\"root_total_nodes\":%d", &root_nodes);
    }
    if (stn) {
        sscanf(stn, "\"scoped_total_nodes\":%d", &scoped_nodes);
    }
    ASSERT_TRUE(root_nodes > scoped_nodes);
    ASSERT_TRUE(scoped_nodes > 0);

    free(inner_scoped);
    free(resp_scoped);
    free(inner_root);
    free(resp_root);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_query_graph_missing_query) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"query_graph\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about missing query */
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PIPELINE-DEPENDENT TOOL HANDLERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_index_repository_missing_path) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_repository\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    ASSERT_NOT_NULL(strstr(resp, "repo_path"));
    ASSERT_NOT_NULL(strstr(resp, "example"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_repository_accepts_path_alias) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-index-path-alias-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-index-path-alias-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return 'ok'\n", fp);
    fclose(fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    char args[1024];
    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"mode\":\"fast\"}", tmp_dir);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    free(resp);

    cbm_mcp_server_free(srv);
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    th_rmtree(tmp_dir);
    th_rmtree(cache);
    PASS();
}

TEST(tool_get_code_snippet_missing_qn) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{\"qualified_name\":\"nonexistent.func\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_missing_pattern) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func main\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* No project indexed → error */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed") ||
                strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(search_code_multi_word) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* Multi-word query "HandleRequest error" — should find the line
     * "func HandleRequest() error {" via regex conversion. */
    char req[512];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":90,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest error\","
             "\"project\":\"test-project\"}}}");

    char *resp = cbm_mcp_server_handle(srv, req);
    ASSERT_NOT_NULL(resp);
    /* Should find at least one result (not zero) */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL);
    /* Should NOT contain an error about "not found" */
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first (#687): scoped content search over a repo whose ROOT PATH
 * contains a space. write_scoped_filelist emits "<root>/<file>" records that the
 * Unix pipeline pipes to grep via xargs. With plain `xargs` (newline-split) the
 * space splits one path into several bogus args -> grep finds nothing ->
 * total_grep_matches == 0 (RED on the unfixed code). The fix writes NUL-separated
 * records + uses `xargs -0`, so the path stays a single argument -> match found
 * (GREEN). On Windows the scoped path uses PowerShell Get-Content -LiteralPath,
 * which already handles spaces, so this asserts correct behavior there too. */
TEST(search_code_scoped_path_with_spaces_issue687) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_srch_space_XXXXXX");
    if (!cbm_mkdtemp(tmp)) {
        FAIL("cbm_mkdtemp failed");
    }

    /* Project root deliberately contains a space. */
    char proj_dir[640];
    snprintf(proj_dir, sizeof(proj_dir), "%s/my project", tmp);
    cbm_mkdir(proj_dir);

    char src_path[768];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = fopen(src_path, "w");
    if (!fp) {
        rmdir(proj_dir);
        rmdir(tmp);
        FAIL("cannot write source file under spaced path");
    }
    fprintf(fp, "package main\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    const char *proj = "space-search";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, proj_dir);

    /* A node so the file is "indexed" (cbm_store_list_files -> scoped grep path)
     * and the grep hit classifies to a result. */
    cbm_node_t n = {.project = proj,
                    .label = "Function",
                    .name = "HandleRequest",
                    .qualified_name = "space-search.main.HandleRequest",
                    .file_path = "main.go",
                    .start_line = 3,
                    .end_line = 5};
    ASSERT_GT(cbm_store_upsert_node(st, &n), 0);

    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":94,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"space-search\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* grep must have found the match despite the space in the root path. */
    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    }
    ASSERT_TRUE(grep_matches > 0);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    unlink(src_path);
    rmdir(proj_dir);
    rmdir(tmp);
    PASS();
}

/* Upstream cc9c2f4c: the scoped scan list is the canonical File-node set —
 * Folder directories and synthetic paths such as <python-builtins> are graph
 * identities, not scannable files. */
TEST(store_list_files_prefers_canonical_file_nodes) {
    cbm_store_t *st = cbm_store_open_memory();
    ASSERT_NOT_NULL(st);
    const char *proj = "list-files-canonical";
    ASSERT_EQ(cbm_store_upsert_project(st, proj, "/nonexistent"), CBM_STORE_OK);
    cbm_node_t folder = {.project = proj,
                         .label = "Folder",
                         .name = "src",
                         .qualified_name = "list-files-canonical.src",
                         .file_path = "src"};
    cbm_node_t builtin = {.project = proj,
                          .label = "Function",
                          .name = "len",
                          .qualified_name = "builtins.len",
                          .file_path = "<python-builtins>",
                          .start_line = 1,
                          .end_line = 1};
    cbm_node_t file = {.project = proj,
                       .label = "File",
                       .name = "main.c",
                       .qualified_name = "list-files-canonical.src.main.c",
                       .file_path = "src/main.c"};
    ASSERT_GT(cbm_store_upsert_node(st, &folder), 0);
    ASSERT_GT(cbm_store_upsert_node(st, &builtin), 0);
    ASSERT_GT(cbm_store_upsert_node(st, &file), 0);

    char **files = NULL;
    int count = 0;
    ASSERT_EQ(cbm_store_list_files(st, proj, &files, &count), CBM_STORE_OK);
    bool only_canonical = count == 1 && strcmp(files[0], "src/main.c") == 0;
    for (int i = 0; i < count; i++) {
        free(files[i]);
    }
    free(files);
    cbm_store_close(st);
    ASSERT_TRUE(only_canonical);
    PASS();
}

/* Write the 64-file Go fixture. Generation 2 gives every file a second definition,
 * so a re-run must re-extract all of them. */
static bool budget_fixture_write(const char *repo, int generation) {
    for (int i = 0; i < 64; i++) {
        char name[16];
        char body[256];
        snprintf(name, sizeof(name), "f%02d.go", i);
        if (generation == 1) {
            snprintf(body, sizeof(body), "package main\n\nfunc F%02d() int {\n\treturn %d\n}\n", i,
                     i);
        } else {
            snprintf(body, sizeof(body),
                     "package main\n\nfunc F%02d() int {\n\treturn %d\n}\n\n"
                     "func G%02d() int {\n\treturn F%02d() + 1\n}\n",
                     i, i, i, i);
        }
        if (th_write_file(TH_PATH(repo, name), body) != 0) {
            return false;
        }
    }
    return true;
}

/* Integer field of the JSON object inside an MCP tool result. */
static int budget_result_int(const char *result, const char *key) {
    char *text = extract_text_content(result);
    yyjson_doc *doc = text ? yyjson_read(text, strlen(text), 0) : NULL;
    yyjson_val *val = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), key) : NULL;
    int out = val && yyjson_is_int(val) ? (int)yyjson_get_int(val) : -1;
    yyjson_doc_free(doc);
    free(text);
    return out;
}

/* Upstream d72250a9 (#1997 #832): an attempt that stays over the memory
 * budget after back-pressure fails whole and the response names the cause,
 * the numbers and that the previous index still serves, instead of the
 * generic "check repo_path" error. The budget seam stands in for a 1 MiB
 * machine. */
TEST(index_repository_over_budget_reports_named_reason) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char repo[512];
    char cache[512];
    snprintf(repo, sizeof(repo), "%s/cbm-mcp-budget-repo-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-budget-cache-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(repo));
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    const char *saved_workers = getenv("CBM_WORKERS");
    char *saved_workers_copy = saved_workers ? strdup(saved_workers) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_setenv("CBM_WORKERS", "4", 1);

    bool files_ok = budget_fixture_write(repo, 1);
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"name\":\"budget\"}", repo);

    /* Generation 1 at the process budget publishes. */
    char *first = srv && files_ok ? cbm_mcp_handle_tool(srv, "index_repository", args) : NULL;
    bool first_indexed = response_contains_json_fragment(first, "\"status\":\"indexed\"");
    int nodes_first = budget_result_int(first, "nodes");
    free(first);

    /* Generation 2 at 1 MiB over a changed repo fails whole. */
    files_ok = files_ok && budget_fixture_write(repo, 2);
    size_t saved_budget = cbm_mem_budget();
    bool saved_explicit = cbm_mem_budget_explicit();
    cbm_mem_set_budget_for_tests((size_t)1024 * 1024, true);
    char *second = srv && files_ok ? cbm_mcp_handle_tool(srv, "index_repository", args) : NULL;
    cbm_mem_set_budget_for_tests(saved_budget, saved_explicit);
    bool named = response_contains_json_fragment(second, "\"status\":\"error\"") &&
                 response_contains_json_fragment(second, "\"reason\":\"over_memory_budget\"") &&
                 response_contains_json_fragment(second, "\"previous_index\":\"preserved\"") &&
                 second && strstr(second, "CBM_MEM_BUDGET_MB") != NULL;
    int budget_mb = budget_result_int(second, "budget_mb");
    int peak_rss_mb = budget_result_int(second, "peak_rss_mb");
    if (!named) {
        printf("    over-budget response: %.400s\n", second ? second : "(null)");
    }
    free(second);

    /* The previous generation still answers with its full graph. */
    char *status =
        srv ? cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"budget\"}") : NULL;
    int nodes_after = budget_result_int(status, "nodes");
    free(status);

    cbm_mcp_server_free(srv);
    if (saved_cache_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    if (saved_workers_copy) {
        cbm_setenv("CBM_WORKERS", saved_workers_copy, 1);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    free(saved_cache_copy);
    free(saved_workers_copy);
    th_rmtree(repo);
    th_rmtree(cache);

    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(files_ok);
    ASSERT_TRUE(first_indexed);
    ASSERT_GT(nodes_first, 0);
    ASSERT_TRUE(named);
    ASSERT_EQ(budget_mb, 1);
    ASSERT_TRUE(peak_rss_mb > 0);
    ASSERT_EQ(nodes_after, nodes_first);
    PASS();
#endif
}

/* Upstream 7a1e069b: the scoped scan must not depend on GNU grep's --include
 * (BusyBox grep in the static Alpine/musl runtime has none). A fake grep that
 * rejects --include stands in for BusyBox; file_pattern is applied in C. */
TEST(search_code_scoped_file_pattern_is_busybox_portable) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX BusyBox-compatible grep contract");
#else
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s/cbm_srch_busybox_XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(tmp));
    char bin_dir[640];
    char fake_grep[700];
    char source_path[700];
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", tmp);
    snprintf(fake_grep, sizeof(fake_grep), "%s/grep", bin_dir);
    snprintf(source_path, sizeof(source_path), "%s/R&D-main.c", tmp);
    ASSERT_EQ(cbm_mkdir(bin_dir), 0);
    ASSERT_EQ(th_write_file(fake_grep, "#!/bin/sh\n"
                                       "for arg in \"$@\"; do\n"
                                       "  case \"$arg\" in --include*) exit 97 ;; esac\n"
                                       "done\n"
                                       "if [ -x /usr/bin/grep ]; then exec /usr/bin/grep \"$@\"; fi\n"
                                       "exec /bin/grep \"$@\"\n"),
              0);
    ASSERT_EQ(chmod(fake_grep, 0700), 0);
    ASSERT_EQ(th_write_file(source_path, "int BUSYBOX_GREP_NEEDLE = 1;\n"), 0);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    const char *proj = "search-busybox";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, tmp);
    cbm_node_t file = {.project = proj,
                       .label = "File",
                       .name = "R&D-main.c",
                       .qualified_name = "search-busybox.R&D-main.c",
                       .file_path = "R&D-main.c"};
    cbm_node_t symbol = {.project = proj,
                         .label = "Variable",
                         .name = "BUSYBOX_GREP_NEEDLE",
                         .qualified_name = "search-busybox.BUSYBOX_GREP_NEEDLE",
                         .file_path = "R&D-main.c",
                         .start_line = 1,
                         .end_line = 1};
    ASSERT_GT(cbm_store_upsert_node(st, &file), 0);
    ASSERT_GT(cbm_store_upsert_node(st, &symbol), 0);
    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(cbm_mcp_server_store(srv), proj), 0);

    const char *old_path = getenv("PATH");
    char *saved_path = old_path ? strdup(old_path) : NULL;
    char forced_path[1600];
    snprintf(forced_path, sizeof(forced_path), "%s:/usr/bin:/bin", bin_dir);
    ASSERT_EQ(cbm_setenv("PATH", forced_path, 1), 0);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":95,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\",\"arguments\":{\"pattern\":"
             "\"BUSYBOX_GREP_NEEDLE\",\"project\":\"search-busybox\","
             "\"file_pattern\":\"*R&D*.c\"}}}");
    if (saved_path) {
        (void)cbm_setenv("PATH", saved_path, 1);
    } else {
        (void)unsetenv("PATH");
    }
    free(saved_path);
    char *inner = resp ? extract_text_content(resp) : NULL;
    bool portable_success = resp && !strstr(resp, "\"isError\":true") && inner &&
                            strstr(inner, "\"total_grep_matches\":1") &&
                            strstr(inner, "BUSYBOX_GREP_NEEDLE");

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    unlink(source_path);
    unlink(fake_grep);
    rmdir(bin_dir);
    rmdir(tmp);
    ASSERT_TRUE(portable_success);
    PASS();
#endif
}

/* Shared fixture for the path_filter prefilter tests (PR #756 distilled):
 * a project with two indexed files that both contain the search pattern —
 * src/handler.go (inside the filter) and vendor/other.go (outside it). */
static cbm_mcp_server_t *setup_prefilter_server(char *tmp, size_t tmp_sz, char *src_path,
                                                size_t src_sz, char *vendor_path,
                                                size_t vendor_sz) {
    snprintf(tmp, tmp_sz, "/tmp/cbm_srch_pref_XXXXXX");
    if (!cbm_mkdtemp(tmp)) {
        return NULL;
    }
    char dir[640];
    snprintf(dir, sizeof(dir), "%s/src", tmp);
    cbm_mkdir(dir);
    snprintf(dir, sizeof(dir), "%s/vendor", tmp);
    cbm_mkdir(dir);

    snprintf(src_path, src_sz, "%s/src/handler.go", tmp);
    snprintf(vendor_path, vendor_sz, "%s/vendor/other.go", tmp);
    FILE *fp = fopen(src_path, "w");
    if (!fp) {
        return NULL;
    }
    fprintf(fp, "package main\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(fp);
    fp = fopen(vendor_path, "w");
    if (!fp) {
        return NULL;
    }
    fprintf(fp, "package vendored\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    if (!srv) {
        return NULL;
    }
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "prefilter-search";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, tmp);

    cbm_node_t n1 = {.project = proj,
                     .label = "Function",
                     .name = "HandleRequest",
                     .qualified_name = "prefilter-search.main.HandleRequest",
                     .file_path = "src/handler.go",
                     .start_line = 3,
                     .end_line = 5};
    cbm_node_t n2 = {.project = proj,
                     .label = "Function",
                     .name = "HandleRequest",
                     .qualified_name = "prefilter-search.vendored.HandleRequest",
                     .file_path = "vendor/other.go",
                     .start_line = 3,
                     .end_line = 5};
    if (cbm_store_upsert_node(st, &n1) <= 0 || cbm_store_upsert_node(st, &n2) <= 0) {
        cbm_mcp_server_free(srv);
        return NULL;
    }
    th_remove_zova_user_database();
    if (th_publish_store_project(st, proj) != 0) {
        cbm_mcp_server_free(srv);
        return NULL;
    }
    return srv;
}

static void cleanup_prefilter_dir(const char *tmp, const char *src_path, const char *vendor_path) {
    char dir[640];
    unlink(src_path);
    unlink(vendor_path);
    snprintf(dir, sizeof(dir), "%s/src", tmp);
    rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/vendor", tmp);
    rmdir(dir);
    rmdir(tmp);
}

/* PR #756 (distilled): scoped search_code prefilters the indexed filelist by
 * path_filter before grep runs. POSITIVE invariant guard: a path_filter that
 * matches the file containing the hit must still return that hit (guards
 * against over-filtering — the prefilter predicate must stay IDENTICAL to the
 * post-grep filter in collect_grep_matches), and files outside the filter
 * stay excluded. Green on pre-prefilter main too (the post-grep filter alone
 * produced the same results): the change is results-preserving perf-only. */
TEST(search_code_path_filter_prefilter_keeps_matches) {
    char tmp[512], src_path[768], vendor_path[768];
    cbm_mcp_server_t *srv = setup_prefilter_server(tmp, sizeof(tmp), src_path, sizeof(src_path),
                                                   vendor_path, sizeof(vendor_path));
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":95,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"prefilter-search\","
             "\"path_filter\":\"^src/\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* The in-filter hit is returned; the out-of-filter file is not. */
    ASSERT_NOT_NULL(strstr(inner, "src/handler.go"));
    ASSERT_TRUE(strstr(inner, "vendor/other.go") == NULL);

    /* Exactly the one in-filter grep match survives (same count before and
     * after the prefilter — predicate identity). */
    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    }
    ASSERT_EQ(grep_matches, 1);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_prefilter_dir(tmp, src_path, vendor_path);
    PASS();
}

/* PR #756 (distilled): path_filter matching ZERO indexed files. With the
 * prefilter the scoped filelist has 0 records, and handle_search_code now
 * skips the grep subprocess entirely (xargs on an empty filelist is
 * platform-dependent: GNU execs grep once with no operands, BSD skips) and
 * returns the empty result directly. Must be a clean zero-result response —
 * no error. Green on pre-prefilter main too (there the full filelist is
 * grepped and the post-grep filter drops every hit — an empty filelist is
 * unreachable on main): guards the edge the prefilter introduces. */
TEST(search_code_path_filter_matches_nothing) {
    char tmp[512], src_path[768], vendor_path[768];
    cbm_mcp_server_t *srv = setup_prefilter_server(tmp, sizeof(tmp), src_path, sizeof(src_path),
                                                   vendor_path, sizeof(vendor_path));
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":96,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"prefilter-search\","
             "\"path_filter\":\"^no_such_dir/\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    }
    ASSERT_EQ(grep_matches, 0);
    int results = -1;
    const char *r = strstr(inner, "\"total_results\":");
    if (r) {
        sscanf(r, "\"total_results\":%d", &results);
    }
    ASSERT_EQ(results, 0);
    ASSERT_TRUE(strstr(inner, "handler.go") == NULL);
    ASSERT_TRUE(strstr(inner, "other.go") == NULL);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_prefilter_dir(tmp, src_path, vendor_path);
    PASS();
}

/* issue #283: search_code with regex=true and a syntactically invalid pattern
 * must return an explicit error, not an empty result indistinguishable from a
 * legitimate no-match. */
TEST(search_code_invalid_regex_errors_issue283) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* Unclosed group under regex=true → must be flagged as an error. */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":91,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func(\",\"regex\":true,"
                                   "\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(resp, "invalid regex"));
    free(resp);

    /* Same pattern as a literal (regex=false) must NOT error. */
    resp = cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"tools/call\","
                                      "\"params\":{\"name\":\"search_code\","
                                      "\"arguments\":{\"pattern\":\"func(\",\"regex\":false,"
                                      "\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "invalid regex") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* issue #282: a literal '|' under regex=false is a silent 0-match trap. It must
 * now be surfaced as a warning (and the result carries elapsed_ms). */
TEST(search_code_literal_pipe_warns_issue282) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"HandleRequest|Nope\","
                                   "\"regex\":false,\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "warnings"));   /* surfaced, not silent */
    ASSERT_NOT_NULL(strstr(resp, "regex=true")); /* the hint names the fix */
    ASSERT_NOT_NULL(strstr(resp, "elapsed_ms")); /* timing is reported */
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* issue #272: '&' in a path / file_pattern is neutralised by the command's
 * quoting and must no longer be rejected as "invalid characters". */
TEST(search_code_ampersand_accepted_issue272) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":94,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"HandleRequest\","
                                   "\"file_pattern\":\"*R&D*.go\",\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "invalid characters") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* ── In-process search_code scanner (#60) ─────────────────────── */

enum { SCAN_OK = 0, SCAN_DEADLINE = 1, SCAN_FAILED = 2, SCAN_OUT_SIZE = 64 * 1024 };

static bool scan_write_file(const char *root, const char *name, const char *data, size_t len) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return false;
    }
    bool ok = fwrite(data, 1, len, fp) == len;
    return fclose(fp) == 0 && ok;
}

/* Write <root>/<name> records in write_scoped_filelist's format to <root>/list. */
static bool scan_write_list(const char *root, const char *const *names, int count, char *list,
                            size_t list_size) {
    snprintf(list, list_size, "%s/list", root);
    FILE *fp = fopen(list, "wb");
    if (!fp) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        fprintf(fp, "%s/%s", root, names[i]);
#ifdef _WIN32
        fputc('\n', fp);
#else
        fputc('\0', fp);
#endif
    }
    return fclose(fp) == 0;
}

static int scan_run(const char *list, const char *root, const char *pattern, bool use_regex,
                    char *out) {
    return cbm_mcp_search_scan_for_testing(list, root, pattern, use_regex, 500, UINT64_MAX, out,
                                           SCAN_OUT_SIZE);
}

TEST(search_scan_matches_literal_multi_regex_and_empty) {
    char root[256];
    snprintf(root, sizeof(root), "%s", th_mktempdir("cbm_scan_basic"));
    const char *text = "alpha\nbeta\ngamma alpha\n";
    ASSERT_TRUE(scan_write_file(root, "a.c", text, strlen(text)));
    const char *names[] = {"a.c"};
    char list[512];
    ASSERT_TRUE(scan_write_list(root, names, 1, list, sizeof(list)));
    char *out = malloc(SCAN_OUT_SIZE);
    ASSERT_NOT_NULL(out);

    ASSERT_EQ(scan_run(list, root, "alpha", false, out), SCAN_OK);
    ASSERT_STR_EQ(out, "a.c:1:alpha\na.c:3:gamma alpha\n");
    /* grep -f semantics: each '\n'-separated item is its own pattern. */
    ASSERT_EQ(scan_run(list, root, "beta\ngamma", false, out), SCAN_OK);
    ASSERT_STR_EQ(out, "a.c:2:beta\na.c:3:gamma alpha\n");
    ASSERT_EQ(scan_run(list, root, "^gam+a", true, out), SCAN_OK);
    ASSERT_STR_EQ(out, "a.c:3:gamma alpha\n");
    ASSERT_EQ(scan_run(list, root, "", false, out), SCAN_OK);
    ASSERT_STR_EQ(out, "a.c:1:alpha\na.c:2:beta\na.c:3:gamma alpha\n");
    ASSERT_EQ(scan_run(list, root, "delta", false, out), SCAN_OK);
    ASSERT_STR_EQ(out, "");
    ASSERT_EQ(scan_run(list, root, "a(", true, out), SCAN_FAILED);

    free(out);
    th_cleanup(root);
    PASS();
}

/* Regex mode accepts the GNU escapes grep -E does (\b \< \> \w \s) on every
 * platform; macOS libc needs REG_ENHANCED for them. */
TEST(search_scan_regex_gnu_escapes) {
    char root[256];
    snprintf(root, sizeof(root), "%s", th_mktempdir("cbm_scan_gnu"));
    const char *text = "  free(p);\nfreedom\n_foo bar\n";
    ASSERT_TRUE(scan_write_file(root, "a.c", text, strlen(text)));
    const char *names[] = {"a.c"};
    char list[512];
    ASSERT_TRUE(scan_write_list(root, names, 1, list, sizeof(list)));
    char *out = malloc(SCAN_OUT_SIZE);
    ASSERT_NOT_NULL(out);

    ASSERT_EQ(scan_run(list, root, "\\bfree\\b", true, out), SCAN_OK);
    ASSERT_STR_EQ(out, "a.c:1:  free(p);\n");
    ASSERT_EQ(scan_run(list, root, "\\<free\\>", true, out), SCAN_OK);
    ASSERT_STR_EQ(out, "a.c:1:  free(p);\n");
    ASSERT_EQ(scan_run(list, root, "^\\w+o\\sbar$", true, out), SCAN_OK);
    ASSERT_STR_EQ(out, "a.c:3:_foo bar\n");

    free(out);
    th_cleanup(root);
    PASS();
}

/* A NUL in the first 32 KiB marks the file binary (no hits); after a later
 * NUL only the complete lines before it are scanned. */
TEST(search_scan_skips_binary_and_stops_at_late_nul) {
    char root[256];
    snprintf(root, sizeof(root), "%s", th_mktempdir("cbm_scan_nul"));
    const char bin[] = "ab\0cd match\n";
    ASSERT_TRUE(scan_write_file(root, "bin.c", bin, sizeof(bin) - 1));

    enum { PAD_LINES = 10240 }; /* "pad\n" x 10240 = 40 KiB */
    size_t cap = PAD_LINES * 4 + 128;
    char *late = malloc(cap);
    ASSERT_NOT_NULL(late);
    size_t len = 0;
    for (int i = 0; i < PAD_LINES; i++) {
        memcpy(late + len, "pad\n", 4);
        len += 4;
    }
    const char tail[] = "match before\nmatch cut\0x\nmatch after\n";
    memcpy(late + len, tail, sizeof(tail) - 1);
    len += sizeof(tail) - 1;
    ASSERT_TRUE(scan_write_file(root, "late.c", late, len));
    free(late);

    const char *names[] = {"bin.c", "late.c"};
    char list[512];
    ASSERT_TRUE(scan_write_list(root, names, 2, list, sizeof(list)));
    char *out = malloc(SCAN_OUT_SIZE);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ(scan_run(list, root, "match", false, out), SCAN_OK);
    ASSERT_STR_EQ(out, "late.c:10241:match before\n");

    free(out);
    th_cleanup(root);
    PASS();
}

/* Hits never carry the CR. `foo$` matches a CRLF line only on Windows, where
 * Select-String matched the line without its CR; POSIX grep keeps it. */
TEST(search_scan_crlf_lines) {
    char root[256];
    snprintf(root, sizeof(root), "%s", th_mktempdir("cbm_scan_crlf"));
    const char *text = "foo\r\nbar\r\n";
    ASSERT_TRUE(scan_write_file(root, "crlf.c", text, strlen(text)));
    const char *names[] = {"crlf.c"};
    char list[512];
    ASSERT_TRUE(scan_write_list(root, names, 1, list, sizeof(list)));
    char *out = malloc(SCAN_OUT_SIZE);
    ASSERT_NOT_NULL(out);

    ASSERT_EQ(scan_run(list, root, "foo", false, out), SCAN_OK);
    ASSERT_STR_EQ(out, "crlf.c:1:foo\n");
    ASSERT_EQ(scan_run(list, root, "foo$", true, out), SCAN_OK);
#ifdef _WIN32
    ASSERT_STR_EQ(out, "crlf.c:1:foo\n");
#else
    ASSERT_STR_EQ(out, "");
#endif

    free(out);
    th_cleanup(root);
    PASS();
}

/* 600 hits over two files: exactly the first 500, in file-list order. */
TEST(search_scan_stops_at_limit_in_list_order) {
    char root[256];
    snprintf(root, sizeof(root), "%s", th_mktempdir("cbm_scan_limit"));
    char *text = malloc(300 * 8 + 1);
    ASSERT_NOT_NULL(text);
    size_t len = 0;
    for (int i = 0; i < 300; i++) {
        memcpy(text + len, "hit\n", 4);
        len += 4;
    }
    ASSERT_TRUE(scan_write_file(root, "one.c", text, len));
    ASSERT_TRUE(scan_write_file(root, "two.c", text, len));
    free(text);
    const char *names[] = {"one.c", "two.c"};
    char list[512];
    ASSERT_TRUE(scan_write_list(root, names, 2, list, sizeof(list)));
    char *out = malloc(SCAN_OUT_SIZE);
    ASSERT_NOT_NULL(out);

    ASSERT_EQ(scan_run(list, root, "hit", false, out), SCAN_OK);
    int lines = 0;
    for (const char *p = out; *p; p++) {
        lines += *p == '\n';
    }
    ASSERT_EQ(lines, 500);
    ASSERT_TRUE(strncmp(out, "one.c:1:hit\n", 12) == 0);
    ASSERT_NOT_NULL(strstr(out, "one.c:300:hit\ntwo.c:1:hit\n"));
    size_t out_len = strlen(out);
    ASSERT_TRUE(out_len > 14 && strcmp(out + out_len - 14, "two.c:200:hit\n") == 0);

    free(out);
    th_cleanup(root);
    PASS();
}

/* A listed file that no longer exists is skipped, as grep's error exit was
 * ignored; later files are still scanned. */
TEST(search_scan_skips_missing_file) {
    char root[256];
    snprintf(root, sizeof(root), "%s", th_mktempdir("cbm_scan_missing"));
    const char *text = "match\n";
    ASSERT_TRUE(scan_write_file(root, "a.c", text, strlen(text)));
    const char *names[] = {"gone.c", "a.c"};
    char list[512];
    ASSERT_TRUE(scan_write_list(root, names, 2, list, sizeof(list)));
    char *out = malloc(SCAN_OUT_SIZE);
    ASSERT_NOT_NULL(out);

    ASSERT_EQ(scan_run(list, root, "match", false, out), SCAN_OK);
    ASSERT_STR_EQ(out, "a.c:1:match\n");

    free(out);
    th_cleanup(root);
    PASS();
}

TEST(search_scan_reports_deadline) {
    char root[256];
    snprintf(root, sizeof(root), "%s", th_mktempdir("cbm_scan_deadline"));
    const char *text = "match\n";
    ASSERT_TRUE(scan_write_file(root, "a.c", text, strlen(text)));
    const char *names[] = {"a.c"};
    char list[512];
    ASSERT_TRUE(scan_write_list(root, names, 1, list, sizeof(list)));
    char *out = malloc(SCAN_OUT_SIZE);
    ASSERT_NOT_NULL(out);

    ASSERT_EQ(
        cbm_mcp_search_scan_for_testing(list, root, "match", false, 500, 1, out, SCAN_OUT_SIZE),
        SCAN_DEADLINE);

    free(out);
    th_cleanup(root);
    PASS();
}

/* A 5,000-byte line: its hit is cut to the 1023-byte content buffer and the
 * next line keeps its number. */
TEST(search_scan_long_line) {
    char root[256];
    snprintf(root, sizeof(root), "%s", th_mktempdir("cbm_scan_long"));
    enum { LONG_LEN = 5000 };
    char *text = malloc(LONG_LEN + 64);
    ASSERT_NOT_NULL(text);
    memset(text, 'x', LONG_LEN);
    const char tail[] = " match\nmatch two\n";
    memcpy(text + LONG_LEN, tail, sizeof(tail) - 1);
    ASSERT_TRUE(scan_write_file(root, "long.c", text, LONG_LEN + sizeof(tail) - 1));
    free(text);
    const char *names[] = {"long.c"};
    char list[512];
    ASSERT_TRUE(scan_write_list(root, names, 1, list, sizeof(list)));
    char *out = malloc(SCAN_OUT_SIZE);
    ASSERT_NOT_NULL(out);

    ASSERT_EQ(scan_run(list, root, "match", false, out), SCAN_OK);
    ASSERT_TRUE(strncmp(out, "long.c:1:", 9) == 0);
    const char *content = out + 9;
    const char *eol = strchr(content, '\n');
    ASSERT_NOT_NULL(eol);
    ASSERT_EQ((int)(eol - content), 1023);
    ASSERT_STR_EQ(eol + 1, "long.c:2:match two\n");

    free(out);
    th_cleanup(root);
    PASS();
}

/* #60 acceptance: mode=full after a line longer than any read buffer returns
 * the function's exact source. */
TEST(search_code_full_mode_after_long_line) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", th_mktempdir("cbm_search_long"));
    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "%s/project", tmp);
    cbm_mkdir(proj_dir);

    char src_path[640];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("var data = \"", fp);
    for (int i = 0; i < 5000; i++) {
        fputc('x', fp);
    }
    fputs("\"\n\nfunc Hello() {\n\treturn\n}\n", fp);
    fclose(fp);

    th_remove_zova_user_database();
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    const char *proj = "search-long-line";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, proj_dir);
    cbm_node_t n = {.project = proj,
                    .label = "Function",
                    .name = "Hello",
                    .qualified_name = "search-long-line.main.Hello",
                    .file_path = "main.go",
                    .start_line = 3,
                    .end_line = 5};
    ASSERT_GT(cbm_store_upsert_node(st, &n), 0);
    ASSERT_EQ(th_publish_store_project(st, proj), 0);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":97,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func Hello\",\"mode\":\"full\","
                                   "\"project\":\"search-long-line\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"source\":\"func Hello() {\\n\\treturn\\n}\\n\""));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    th_cleanup(tmp);
    PASS();
}

/* Literal search is case-sensitive on every platform. The Windows scoped scan
 * used Select-String, which ignores case, before #60. */
TEST(search_code_literal_is_case_sensitive) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":98,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"handlerequest\","
                                   "\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    }
    ASSERT_EQ(grep_matches, 0);

    free(inner);
    free(resp);
    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* A project with no indexed files falls back to the recursive grep scan. */
TEST(search_code_unindexed_project_scans_recursively) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", th_mktempdir("cbm_search_unindexed"));
    const char *text = "package main\n\nfunc HandleRequest() error {\n\treturn nil\n}\n";
    ASSERT_TRUE(scan_write_file(tmp, "main.go", text, strlen(text)));

    th_remove_zova_user_database();
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    const char *proj = "search-unindexed";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, tmp);
    ASSERT_EQ(th_publish_store_project(st, proj), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"search-unindexed\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    }
    ASSERT_TRUE(grep_matches > 0);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    th_cleanup(tmp);
    PASS();
}

TEST(tool_detect_changes_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"detect_changes\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "missing required argument: project"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_repository_dot_uses_absolute_project_key) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-index-dot-adr-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-index-dot-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return helper()\n\ndef helper():\n    return 1\n", fp);
    fclose(fp);

    char old_cwd[CBM_SZ_4K];
    ASSERT_NOT_NULL(cbm_getcwd(old_cwd, sizeof(old_cwd)));

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    ASSERT_EQ(cbm_chdir(tmp_dir), 0);
    char *resp =
        cbm_mcp_handle_tool(srv, "index_repository", "{\"repo_path\":\".\",\"mode\":\"fast\"}");
    ASSERT_EQ(cbm_chdir(old_cwd), 0);
    ASSERT_NOT_NULL(resp);
    if (!response_contains_json_fragment(resp, "\"status\":\"indexed\"")) {
        free(resp);
        cbm_mcp_server_free(srv);
        th_remove_zova_user_database();
        restore_cache_dir(saved_copy);
        free(saved_copy);
        free(project);
        remove(src_path);
        cbm_rmdir(cache);
        cbm_rmdir(tmp_dir);
        PASS();
    }
    ASSERT_NOT_NULL(strstr(resp, project));
    ASSERT(!response_contains_json_fragment(resp, "\"project\":\"root\""));
    free(resp);

    cbm_mcp_server_free(srv);
    th_remove_zova_user_database();
    restore_cache_dir(saved_copy);
    free(saved_copy);
    free(project);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);
    PASS();
}

TEST(tool_detect_changes_not_found_rich_error) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-detect-missing-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_handle_tool(srv, "detect_changes", "{\"project\":\"cbm-no-such-project-zzz\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "or not indexed"));
    ASSERT_NOT_NULL(strstr(resp, "hint"));
    free(resp);

    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);
    PASS();
}

/* detect_changes must read symbols from the project's own store. The fixture
 * is published to cbm.zova from a separate store so the server's placeholder
 * store stays empty. */
TEST(tool_detect_changes_reports_impacted_symbols) {
    const char *proj = "detect-impact";
    char *tmp = th_mktempdir("cbm_detect_impact");
    ASSERT_NOT_NULL(tmp);
    char cmd[CBM_SZ_1K];
    snprintf(cmd, sizeof(cmd), "git -C '%s' init -q >/dev/null 2>&1", tmp);
    ASSERT_EQ(system(cmd), 0);
    ASSERT_EQ(th_write_file(TH_PATH(tmp, "handler.go"),
                            "package main\n\nfunc HandleRequest() error {\n\treturn nil\n}\n"),
              0);

    cbm_store_t *fixture = cbm_store_open_memory();
    ASSERT_NOT_NULL(fixture);
    ASSERT_EQ(cbm_store_upsert_project(fixture, proj, tmp), CBM_STORE_OK);
    cbm_node_t n = {.project = proj,
                    .label = "Function",
                    .name = "HandleRequest",
                    .qualified_name = "detect-impact.main.HandleRequest",
                    .file_path = "handler.go",
                    .start_line = 3,
                    .end_line = 5};
    ASSERT_GT(cbm_store_upsert_node(fixture, &n), 0);
    th_remove_zova_user_database();
    ASSERT_EQ(th_publish_store_project(fixture, proj), 0);
    cbm_store_close(fixture);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_handle_tool(srv, "detect_changes", "{\"project\":\"detect-impact\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "handler.go"));
    ASSERT_NOT_NULL(strstr(resp, "HandleRequest"));
    free(resp);

    cbm_mcp_server_free(srv);
    th_cleanup(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  IDLE STORE EVICTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(store_idle_eviction) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_mcp_server_set_project(srv, "test-evict");

    /* Trigger resolve_store via a tool call to set store_last_used */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with 0s timeout → should evict immediately */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_no_eviction_within_timeout) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_mcp_server_set_project(srv, "test-evict");

    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with large timeout → should NOT evict */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_protects_initial_store) {
    /* Evicting with NULL server should not crash */
    cbm_mcp_server_evict_idle(NULL, 0);

    /* Evicting server whose store was never accessed via a named project
     * should NOT evict the initial in-memory store (store_last_used == 0). */
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_access_resets_timer) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_mcp_server_set_project(srv, "test-evict");

    /* First access */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    /* Second access (resets timer) */
    resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With large timeout, store should survive */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With 0 timeout, store should be evicted */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  URI HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_unix) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/home/user/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///tmp/test", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/tmp/test");

    ASSERT_TRUE(cbm_parse_file_uri("file:///", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/");
    PASS();
}

TEST(parse_file_uri_windows) {
    char path[256];
    /* Windows drive letter — leading / stripped */
    ASSERT_TRUE(cbm_parse_file_uri("file:///C:/Users/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "C:/Users/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///D:/Projects/myapp", path, sizeof(path)));
    ASSERT_STR_EQ(path, "D:/Projects/myapp");
    PASS();
}

TEST(parse_file_uri_invalid) {
    char path[256];
    /* Non-file URI */
    ASSERT_FALSE(cbm_parse_file_uri("https://example.com", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* Empty string */
    ASSERT_FALSE(cbm_parse_file_uri("", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* NULL */
    ASSERT_FALSE(cbm_parse_file_uri(NULL, path, sizeof(path)));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SNIPPET TESTS — Port of internal/tools/snippet_test.go
 * ══════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Create an MCP server pre-populated with nodes/edges matching Go testSnippetServer.
 * Writes a source file to tmp_dir/project/main.go.
 * Caller must free the server with cbm_mcp_server_free and
 * unlink the source file + rmdir manually. */
static cbm_mcp_server_t *setup_snippet_server(char *tmp_dir, size_t tmp_sz) {
    /* Create temp dir */
    snprintf(tmp_dir, tmp_sz, "/tmp/cbm_snippet_test_XXXXXX");
    if (!cbm_mkdtemp(tmp_dir))
        return NULL;

    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "%s/project", tmp_dir);
    cbm_mkdir(proj_dir);

    /* Write sample source file */
    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = fopen(src_path, "w");
    if (!fp)
        return NULL;
    fprintf(fp, "package main\n"
                "\n"
                "func HandleRequest() error {\n"
                "\treturn nil\n"
                "}\n"
                "\n"
                "func ProcessOrder(id int) {\n"
                "\t// process\n"
                "}\n"
                "\n"
                "func Run() {\n"
                "\t// server\n"
                "}\n");
    fclose(fp);

    th_remove_zova_user_database();

    /* Create server with in-memory store */
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    if (!srv)
        return NULL;

    cbm_store_t *st = cbm_mcp_server_store(srv);
    if (!st) {
        cbm_mcp_server_free(srv);
        return NULL;
    }

    const char *proj_name = "test-project";
    cbm_mcp_server_set_project(srv, proj_name);
    cbm_store_upsert_project(st, proj_name, proj_dir);

    /* Create nodes */
    cbm_node_t n_hr = {0};
    n_hr.project = proj_name;
    n_hr.label = "Function";
    n_hr.name = "HandleRequest";
    n_hr.qualified_name = "test-project.cmd.server.main.HandleRequest";
    n_hr.file_path = "main.go";
    n_hr.start_line = 3;
    n_hr.end_line = 5;
    n_hr.properties_json = "{\"signature\":\"func HandleRequest() error\","
                           "\"return_type\":\"error\","
                           "\"is_exported\":true,"
                           "\"tags\":[\"http\",\"public\"],"
                           "\"metadata\":{\"owner\":\"api\"}}";
    int64_t id_hr = cbm_store_upsert_node(st, &n_hr);

    cbm_node_t n_po = {0};
    n_po.project = proj_name;
    n_po.label = "Function";
    n_po.name = "ProcessOrder";
    n_po.qualified_name = "test-project.cmd.server.main.ProcessOrder";
    n_po.file_path = "main.go";
    n_po.start_line = 7;
    n_po.end_line = 9;
    n_po.properties_json = "{\"signature\":\"func ProcessOrder(id int)\"}";
    int64_t id_po = cbm_store_upsert_node(st, &n_po);

    cbm_node_t n_run1 = {0};
    n_run1.project = proj_name;
    n_run1.label = "Function";
    n_run1.name = "Run";
    n_run1.qualified_name = "test-project.cmd.server.Run";
    n_run1.file_path = "main.go";
    n_run1.start_line = 11;
    n_run1.end_line = 13;
    int64_t id_run1 = cbm_store_upsert_node(st, &n_run1);

    cbm_node_t n_run2 = {0};
    n_run2.project = proj_name;
    n_run2.label = "Function";
    n_run2.name = "Run";
    n_run2.qualified_name = "test-project.cmd.worker.Run";
    n_run2.file_path = "main.go";
    n_run2.start_line = 11;
    n_run2.end_line = 13;
    cbm_store_upsert_node(st, &n_run2);

    /* Create edges: HandleRequest -> ProcessOrder, HandleRequest -> Run1 */
    cbm_edge_t e1 = {.project = proj_name, .source_id = id_hr, .target_id = id_po, .type = "CALLS"};
    cbm_store_insert_edge(st, &e1);

    cbm_edge_t e2 = {
        .project = proj_name, .source_id = id_hr, .target_id = id_run1, .type = "CALLS"};
    cbm_store_insert_edge(st, &e2);
    (void)id_run1; /* run1 used for edge above */

    if (th_publish_store_project(st, proj_name) != 0) {
        cbm_mcp_server_free(srv);
        return NULL;
    }
    return srv;
}

/* Cleanup temp files created by setup_snippet_server */
static void cleanup_snippet_dir(const char *tmp_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/project/main.go", tmp_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/project", tmp_dir);
    rmdir(path);
    rmdir(tmp_dir);
}

/* Extract the inner "text" value from an MCP tool result JSON.
 * The MCP envelope is: {"content":[{"type":"text","text":"<inner json>"}]}
 * This returns the unescaped inner JSON. Caller must free. */
static char *extract_text_content(const char *mcp_result) {
    if (!mcp_result)
        return NULL;
    yyjson_doc *doc = yyjson_read(mcp_result, strlen(mcp_result), 0);
    if (!doc)
        return strdup(mcp_result); /* fallback */
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (!content) {
        /* Handle JSON-RPC wrapper: {"jsonrpc":...,"result":{"content":[...]}} */
        yyjson_val *rpc_result = yyjson_obj_get(root, "result");
        if (rpc_result) {
            content = yyjson_obj_get(rpc_result, "content");
        }
    }
    if (!content || !yyjson_is_arr(content)) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *item = yyjson_arr_get(content, 0);
    if (!item) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *text = yyjson_obj_get(item, "text");
    const char *str = yyjson_get_str(text);
    char *result = str ? strdup(str) : strdup(mcp_result);
    yyjson_doc_free(doc);
    return result;
}

/* Call get_code_snippet and extract inner text content.
 * Caller must free returned string. */
static char *call_snippet(cbm_mcp_server_t *srv, const char *args_json) {
    char *raw = cbm_mcp_handle_tool(srv, "get_code_snippet", args_json);
    char *text = extract_text_content(raw);
    free(raw);
    return text;
}

static bool is_valid_json_response(const char *json) {
    if (!json) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_doc_free(doc);
    return true;
}

static bool snippet_source_has_replacement(const char *json) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *source = yyjson_obj_get(root, "source");
    const char *source_str = yyjson_get_str(source);
    bool found = source_str && strstr(source_str, "\xEF\xBF\xBD");
    yyjson_doc_free(doc);
    return found;
}

/* ── TestSnippet_ExactQN ──────────────────────────────────────── */

TEST(snippet_exact_qn) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* Exact match should NOT have match_method */
    ASSERT_NULL(strstr(resp, "\"match_method\""));
    /* Enriched properties */
    ASSERT_NOT_NULL(strstr(resp, "\"signature\":\"func HandleRequest() error\""));
    ASSERT_NOT_NULL(strstr(resp, "\"return_type\":\"error\""));
    /* Caller/callee counts: 0 callers, 2 callees */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\":0"));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\":2"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── Issue #76: a line longer than the 2 KiB read buffer is still one line ── */

TEST(snippet_line_longer_than_read_buffer) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_snippet_long_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmp));
    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "%s/project", tmp);
    cbm_mkdir(proj_dir);

    /* Line 1 is about 3000 bytes; Hello() sits on lines 3-5. */
    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("var data = \"", fp);
    for (int i = 0; i < 3000; i++) {
        fputc('x', fp);
    }
    fputs("\"\n\nfunc Hello() {\n\treturn\n}\n", fp);
    fclose(fp);

    th_remove_zova_user_database();
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    const char *proj = "long-line-project";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, proj_dir);

    cbm_node_t n = {0};
    n.project = proj;
    n.label = "Function";
    n.name = "Hello";
    n.qualified_name = "long-line-project.main.Hello";
    n.file_path = "main.go";
    n.start_line = 3;
    n.end_line = 5;
    cbm_store_upsert_node(st, &n);
    ASSERT_EQ(th_publish_store_project(st, proj), 0);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"long-line-project.main.Hello\","
                                   "\"project\":\"long-line-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"source\":\"func Hello() {\\n\\treturn\\n}\\n\""));
    free(resp);

    cbm_mcp_server_free(srv);
    unlink(src_path);
    rmdir(proj_dir);
    rmdir(tmp);
    PASS();
}

/* ── TestSnippet_QNSuffix ─────────────────────────────────────── */

TEST(snippet_qn_suffix) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"main.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_UniqueShortName ──────────────────────────────── */

TEST(snippet_unique_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "ProcessOrder" is unique — suffix tier matches (QN ends with .ProcessOrder) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"ProcessOrder\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"ProcessOrder\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── C-macro namespace (PR C1) ────────────────────────────────── */

/* A C macro's QN ends in "#macro", which no caller spells. get_code_snippet
 * still returns the macro for its short name, for a dotted suffix and for the
 * QN it had before the fence (`<module>.<NAME>`); when a definition owns that
 * QN or that name, the definition is the answer. */
TEST(snippet_c_macro_namespace_c1) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);

    cbm_node_t only_macro = {.project = "test-project",
                             .label = "Macro",
                             .name = "MAX_LEN",
                             .qualified_name = "test-project.lib.cfg.MAX_LEN#macro",
                             .file_path = "main.go",
                             .start_line = 7,
                             .end_line = 8};
    cbm_node_t type = {.project = "test-project",
                       .label = "Type",
                       .name = "state_t",
                       .qualified_name = "test-project.lib.xx.state_t",
                       .file_path = "main.go",
                       .start_line = 3,
                       .end_line = 3};
    cbm_node_t shadowed_macro = {.project = "test-project",
                                 .label = "Macro",
                                 .name = "state_t",
                                 .qualified_name = "test-project.lib.xx.state_t#macro",
                                 .file_path = "main.go",
                                 .start_line = 11,
                                 .end_line = 12};
    ASSERT_GT(cbm_store_upsert_node(st, &only_macro), 0);
    ASSERT_GT(cbm_store_upsert_node(st, &type), 0);
    ASSERT_GT(cbm_store_upsert_node(st, &shadowed_macro), 0);
    ASSERT_EQ(th_publish_store_project(st, "test-project"), 0);

    static const char *const macro_inputs[] = {
        "{\"qualified_name\":\"MAX_LEN\",\"project\":\"test-project\"}",
        "{\"qualified_name\":\"cfg.MAX_LEN\",\"project\":\"test-project\"}",
        "{\"qualified_name\":\"test-project.lib.cfg.MAX_LEN\",\"project\":\"test-project\"}",
        "{\"qualified_name\":\"test-project.lib.cfg.MAX_LEN#macro\",\"project\":\"test-project\"}"};
    for (size_t i = 0; i < sizeof(macro_inputs) / sizeof(macro_inputs[0]); i++) {
        char *resp = call_snippet(srv, macro_inputs[i]);
        ASSERT_NOT_NULL(resp);
        if (!strstr(resp, "\"qualified_name\":\"test-project.lib.cfg.MAX_LEN#macro\"")) {
            fprintf(stderr, "  [c1-snippet] %s -> %.300s\n", macro_inputs[i], resp);
        }
        ASSERT_NOT_NULL(strstr(resp, "\"qualified_name\":\"test-project.lib.cfg.MAX_LEN#macro\""));
        ASSERT_NOT_NULL(strstr(resp, "\"label\":\"Macro\""));
        free(resp);
    }

    static const char *const definition_inputs[] = {
        "{\"qualified_name\":\"test-project.lib.xx.state_t\",\"project\":\"test-project\"}",
        "{\"qualified_name\":\"xx.state_t\",\"project\":\"test-project\"}",
        "{\"qualified_name\":\"state_t\",\"project\":\"test-project\"}"};
    for (size_t i = 0; i < sizeof(definition_inputs) / sizeof(definition_inputs[0]); i++) {
        char *resp = call_snippet(srv, definition_inputs[i]);
        ASSERT_NOT_NULL(resp);
        if (!strstr(resp, "\"label\":\"Type\"")) {
            fprintf(stderr, "  [c1-snippet] %s -> %.300s\n", definition_inputs[i], resp);
        }
        ASSERT_NOT_NULL(strstr(resp, "\"qualified_name\":\"test-project.lib.xx.state_t\""));
        ASSERT_NOT_NULL(strstr(resp, "\"label\":\"Type\""));
        free(resp);
    }

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NameTier ─────────────────────────────────────── */

TEST(snippet_name_tier) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "HandleRequest" — suffix tier finds it (QN ends with .HandleRequest) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AmbiguousShortName ───────────────────────────── */

TEST(snippet_ambiguous_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" matches 2 nodes — should return suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NOT_NULL(strstr(resp, "\"message\""));
    ASSERT_NOT_NULL(strstr(resp, "\"suggestions\""));
    /* Must NOT have "error" key */
    ASSERT_NULL(strstr(resp, "\"error\""));
    /* Must NOT have "source" */
    ASSERT_NULL(strstr(resp, "\"source\""));
    /* Should have at least 2 suggestions with qualified_name */
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.server.Run"));
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.worker.Run"));
    free(resp);

    /* Both Run nodes sit in main.go: that file_path keeps both; a file that
     * defines neither is an error. */
    resp = call_snippet(srv, "{\"qualified_name\":\"Run\",\"project\":\"test-project\","
                             "\"file_path\":\"main.go\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    free(resp);
    resp = call_snippet(srv, "{\"qualified_name\":\"Run\",\"project\":\"test-project\","
                             "\"file_path\":\"other.go\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not defined in file_path other.go"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NotFound ─────────────────────────────────────── */

TEST(snippet_not_found) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"CompletelyNonexistentFunctionXYZ123\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or suggestions */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "suggestions"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzySuggestions ─────────────────────────────── */

TEST(snippet_fuzzy_suggestions) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Handle" is not an exact QN or suffix — should get not-found guidance */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Handle\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should guide user to search_graph */
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_EnrichedProperties ───────────────────────────── */

TEST(snippet_enriched_properties) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"signature\""));
    ASSERT_NOT_NULL(strstr(resp, "\"return_type\""));
    ASSERT_NOT_NULL(strstr(resp, "\"is_exported\":true"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzyLastSegment ─────────────────────────────── */

TEST(snippet_fuzzy_last_segment) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "auth.handlers.HandleRequest" — suffix match should find HandleRequest */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"auth.handlers.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should either find it via suffix or guide to search_graph */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL || strstr(resp, "search_graph") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Default ──────────────────────────── */

TEST(snippet_auto_resolve_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" is ambiguous (2 candidates). Without auto_resolve → suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Enabled ──────────────────────────── */

TEST(snippet_auto_resolve_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" — suffix match should find candidates or guide to search */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* "Run" matches multiple nodes via suffix → should get suggestions or source */
    ASSERT_TRUE(strstr(resp, "Run") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Default ─────────────────────── */

TEST(snippet_include_neighbors_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Without include_neighbors → NO caller_names/callee_names */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    ASSERT_NULL(strstr(resp, "\"callee_names\""));
    /* But should still have counts */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\""));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Enabled ─────────────────────── */

TEST(snippet_include_neighbors_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"include_neighbors\":true,\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* HandleRequest has 0 callers → no caller_names array */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    /* HandleRequest has 2 callees: ProcessOrder and Run */
    ASSERT_NOT_NULL(strstr(resp, "\"callee_names\""));
    ASSERT_NOT_NULL(strstr(resp, "ProcessOrder"));
    ASSERT_NOT_NULL(strstr(resp, "Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_SourceInvalidUtf8 ────────────────────────────── */

TEST(snippet_source_invalid_utf8) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/project/main.go", tmp);
    FILE *fp = fopen(src_path, "wb");
    ASSERT_NOT_NULL(fp);
    const unsigned char source[] = {
        'p',  'a',  'c', 'k', 'a', 'g',  'e',  ' ',  'm',  'a',  'i',  'n', '\n', '\n',
        'f',  'u',  'n', 'c', ' ', 'H',  'a',  'n',  'd',  'l',  'e',  'R', 'e',  'q',
        'u',  'e',  's', 't', '(', ')',  ' ',  'e',  'r',  'r',  'o',  'r', ' ',  '{',
        '\n', '\t', '/', '/', ' ', 0xC0, 0xD4, 0xB7, 0xC2, '\n', '\t', 'r', 'e',  't',
        'u',  'r',  'n', ' ', 'n', 'i',  'l',  '\n', '}',  '\n'};
    ASSERT_EQ(fwrite(source, 1, sizeof(source), fp), sizeof(source));
    ASSERT_EQ(fclose(fp), 0);

    char *raw =
        cbm_mcp_handle_tool(srv, "get_code_snippet",
                            "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                            "\"project\":\"test-project\"}");
    ASSERT_TRUE(is_valid_json_response(raw));
    char *resp = extract_text_content(raw);
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(is_valid_json_response(resp));
    ASSERT_NULL(strstr(resp, "\xC0\xD4"));
    ASSERT_NOT_NULL(strstr(resp, "HandleRequest"));
    ASSERT_NOT_NULL(strstr(resp, "return nil"));
    ASSERT_TRUE(snippet_source_has_replacement(resp));

    free(resp);
    free(raw);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_parse_empty_string) {
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_missing_jsonrpc_field) {
    /* jsonrpc field absent — parser defaults to "2.0" if method present */
    const char *line = "{\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.jsonrpc, "2.0");
    ASSERT_STR_EQ(req.method, "initialize");
    ASSERT_TRUE(req.has_id);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_missing_method) {
    /* method is required — should fail */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"params\":{}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_string_id) {
    /* JSON-RPC §4: string and numeric ids are distinct. A string id is
     * preserved verbatim (issue #253), never coerced to a number. */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":\"99\",\"method\":\"tools/list\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.id_str);
    ASSERT_STR_EQ(req.id_str, "99");
    ASSERT_STR_EQ(req.method, "tools/list");
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_no_params) {
    /* Request with no params field — params_raw should be NULL */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(req.params_raw);
    ASSERT_EQ(req.id, 5);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_extra_whitespace) {
    /* Leading/trailing whitespace and internal spacing in JSON */
    const char *line = "  { \"jsonrpc\" : \"2.0\" , \"id\" : 7 , \"method\" : \"ping\" }  ";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(req.id, 7);
    ASSERT_STR_EQ(req.method, "ping");
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_array_not_object) {
    /* JSON array at root — not a valid JSON-RPC request */
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("[1,2,3]", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_get_string_arg_empty_json) {
    /* Empty JSON string — yyjson_read fails → NULL */
    char *val = cbm_mcp_get_string_arg("", "key");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_string_arg_empty_object) {
    /* Valid JSON with no keys → NULL for any key */
    char *val = cbm_mcp_get_string_arg("{}", "key");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_string_arg_nested_value) {
    /* Value is an object, not a string → should return NULL */
    const char *args = "{\"config\":{\"nested\":true},\"name\":\"hello\"}";
    char *val = cbm_mcp_get_string_arg(args, "config");
    ASSERT_NULL(val); /* not a string type */
    val = cbm_mcp_get_string_arg(args, "name");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "hello");
    free(val);
    PASS();
}

TEST(mcp_get_string_arg_int_value) {
    /* Value is an integer, not a string → NULL */
    char *val = cbm_mcp_get_string_arg("{\"count\":42}", "count");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_int_arg_empty_json) {
    int val = cbm_mcp_get_int_arg("", "key", 99);
    ASSERT_EQ(val, 99);
    PASS();
}

TEST(mcp_get_int_arg_string_value) {
    /* Value is a string, not int → should return default */
    int val = cbm_mcp_get_int_arg("{\"limit\":\"ten\"}", "limit", 5);
    ASSERT_EQ(val, 5);
    PASS();
}

TEST(mcp_get_int_arg_bool_value) {
    /* Value is a bool, not int → default */
    int val = cbm_mcp_get_int_arg("{\"flag\":true}", "flag", -1);
    ASSERT_EQ(val, -1);
    PASS();
}

TEST(mcp_get_int_arg_out_of_range) {
    /* A JSON integer wider than int must fall back to the default instead of
     * wrapping: 4294967297 used to become 1 and 2147483648 became negative. */
    int val = cbm_mcp_get_int_arg("{\"limit\":4294967297}", "limit", 7);
    ASSERT_EQ(val, 7);
    val = cbm_mcp_get_int_arg("{\"limit\":2147483648}", "limit", 7);
    ASSERT_EQ(val, 7);
    val = cbm_mcp_get_int_arg("{\"limit\":-2147483649}", "limit", 7);
    ASSERT_EQ(val, 7);
    /* In-range boundaries are still accepted. */
    val = cbm_mcp_get_int_arg("{\"limit\":2147483647}", "limit", 7);
    ASSERT_EQ(val, 2147483647);
    val = cbm_mcp_get_int_arg("{\"limit\":-2147483648}", "limit", 7);
    ASSERT_EQ(val, -2147483648);
    PASS();
}

TEST(mcp_get_bool_arg_empty_json) {
    bool val = cbm_mcp_get_bool_arg("", "key");
    ASSERT_FALSE(val);
    PASS();
}

TEST(mcp_get_bool_arg_int_value) {
    /* Value is int 1, not bool → should return false */
    bool val = cbm_mcp_get_bool_arg("{\"flag\":1}", "flag");
    ASSERT_FALSE(val);
    PASS();
}

TEST(mcp_get_tool_name_empty_json) {
    char *name = cbm_mcp_get_tool_name("");
    ASSERT_NULL(name);
    PASS();
}

TEST(mcp_get_tool_name_missing_name) {
    char *name = cbm_mcp_get_tool_name("{\"arguments\":{}}");
    ASSERT_NULL(name);
    PASS();
}

TEST(mcp_get_arguments_empty_json) {
    char *args = cbm_mcp_get_arguments("");
    ASSERT_NULL(args);
    PASS();
}

TEST(mcp_get_arguments_no_arguments_key) {
    /* No "arguments" key → returns "{}" */
    char *args = cbm_mcp_get_arguments("{\"name\":\"tool\"}");
    ASSERT_NOT_NULL(args);
    ASSERT_STR_EQ(args, "{}");
    free(args);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  FILE URI PARSING — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_http_scheme) {
    char path[256];
    ASSERT_FALSE(cbm_parse_file_uri("http://example.com/path", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");
    PASS();
}

TEST(parse_file_uri_ftp_scheme) {
    char path[256];
    ASSERT_FALSE(cbm_parse_file_uri("ftp://server/file.txt", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");
    PASS();
}

TEST(parse_file_uri_buffer_too_small) {
    char path[5]; /* only 5 bytes — path gets truncated */
    ASSERT_TRUE(cbm_parse_file_uri("file:///usr/local/bin", path, sizeof(path)));
    /* snprintf truncates to 4 chars + NUL */
    ASSERT_EQ(strlen(path), 4);
    ASSERT_STR_EQ(path, "/usr");
    PASS();
}

TEST(parse_file_uri_spaces_in_path) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/my%20project", path, sizeof(path)));
    /* Raw percent-encoding is preserved (not decoded) */
    ASSERT_STR_EQ(path, "/home/user/my%20project");
    PASS();
}

TEST(parse_file_uri_null_out_path) {
    /* NULL out_path — should not crash */
    ASSERT_FALSE(cbm_parse_file_uri("file:///tmp", NULL, 256));
    PASS();
}

TEST(parse_file_uri_zero_size) {
    char path[256] = "garbage";
    /* out_size=0 → should fail safely */
    ASSERT_FALSE(cbm_parse_file_uri("file:///tmp", path, 0));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SERVER HANDLE — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(server_handle_invalid_json) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    char *resp = cbm_mcp_server_handle(srv, "this is not json at all");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    ASSERT_NOT_NULL(strstr(resp, "-32700")); /* Parse error */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_empty_object) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    /* Valid JSON but no method field → parse error */
    char *resp = cbm_mcp_server_handle(srv, "{}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_call_missing_name) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();

    /* tools/call with no tool name in params */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"tools/call\","
                                   "\"params\":{\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about unknown/missing tool */
    ASSERT_NOT_NULL(strstr(resp, "\"id\":50"));
    ASSERT_TRUE(strstr(resp, "error") || strstr(resp, "isError") || strstr(resp, "unknown"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  POLL/GETLINE FILE* BUFFERING FIX
 * ══════════════════════════════════════════════════════════════════ */

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>

/* Signal handler used by alarm() to abort the test if it hangs */
static void alarm_handler(int sig) {
    (void)sig;
    /* Writing to stderr is async-signal-safe */
    const char msg[] = "FAIL: mcp_server_run_rapid_messages timed out (>5s)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

TEST(mcp_server_run_rapid_messages) {
    /* Simulate a client sending initialize + notifications/initialized +
     * tools/list all at once (no delays), which exercises the FILE*
     * buffering fix: the first getline() over-reads kernel data into the
     * libc buffer; without the fix, subsequent poll() calls block for 60s.
     *
     * We use alarm(5) to abort the test process if the server hangs. */
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    /* Write all 3 messages to the write end in one shot */
    const char *msgs = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n";
    ssize_t written = write(fds[1], msgs, strlen(msgs));
    ASSERT_TRUE(written > 0);
    close(fds[1]); /* EOF signals end of input to the server */

    FILE *in_fp = fdopen(fds[0], "r");
    ASSERT_NOT_NULL(in_fp);

    FILE *out_fp = tmpfile();
    ASSERT_NOT_NULL(out_fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);

    /* Install alarm to fail the test if cbm_mcp_server_run blocks */
    signal(SIGALRM, alarm_handler);
    alarm(5);

    int rc = cbm_mcp_server_run(srv, in_fp, out_fp);

    alarm(0); /* cancel alarm */
    signal(SIGALRM, SIG_DFL);

    ASSERT_EQ(rc, 0);

    /* Verify both responses are present:
     *   id:1 — initialize response
     *   id:2 — tools/list response (notifications/initialized produces none)
     * and that the tools list payload is included. */
    rewind(out_fp);
    char buf[4096] = {0};
    size_t nread = fread(buf, 1, sizeof(buf) - 1, out_fp);
    ASSERT_TRUE(nread > 0);
    ASSERT_NOT_NULL(strstr(buf, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(buf, "\"id\":2"));
    ASSERT_NOT_NULL(strstr(buf, "tools"));

    cbm_mcp_server_free(srv);
    fclose(out_fp);
    /* in_fp already EOF; fclose cleans up */
    fclose(in_fp);
    PASS();
}
#endif /* !_WIN32 */

/* Publish n projects named "<prefix>_NN_bbb…" (~120 chars) into the shared
 * cbm.zova under cache. Returns 0 on success. */
static int issue235_publish_projects(const char *cache, const char *prefix, int n) {
    for (int i = 0; i < n; i++) {
        char name[256];
        snprintf(name, sizeof(name),
                 "%s_%02d_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                 prefix, i);
        cbm_store_t *st = cbm_store_open_memory();
        int rc = st && cbm_store_upsert_project(st, name, cache) == CBM_STORE_OK
                     ? th_publish_store_project(st, name)
                     : -1;
        if (st) {
            cbm_store_close(st);
        }
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

/* Issue #235: passing an unrecognised project name to a tool crashed the
 * binary with a buffer overflow while building the "available_projects"
 * error list — the project-name collector overflowed projects[CBM_SZ_4K] via
 * an unsigned underflow on (out_sz - offset) once the listed names exceeded
 * the buffer. Publish enough long-named projects to exceed 4 KB, then hit the
 * bad-project path. Under ASan a regression aborts here; the fixed
 * bounds-check keeps it clean and returns a normal error. */
TEST(tool_bad_project_name_no_overflow_issue235) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-badproj-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS(); /* skip if mkdtemp fails */
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* 40 * ~120-char names overflows the 4 KB available-projects buffer. */
    int published = issue235_publish_projects(cache, "proj", 40);

    char *resp = NULL;
    if (published == 0) {
        cbm_mcp_server_t *srv = cbm_mcp_server_new();
        resp = srv ? cbm_mcp_server_handle(
                         srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
                              "\"name\":\"get_graph_schema\",\"arguments\":{"
                              "\"project\":\"definitely-not-a-real-project-xyz\"}}}")
                   : NULL;
        cbm_mcp_server_free(srv);
    }
    bool not_found = resp && strstr(resp, "not found");
    free(resp);

    restore_cache_dir(saved_copy);
    free(saved_copy);
    th_rmtree(cache);

    ASSERT_EQ(published, 0);
    ASSERT_TRUE(not_found);
    PASS();
}

/* Issue #235 (follow-up): with many long-named projects indexed, the
 * project-name collector overflowed projects[CBM_SZ_4K] and truncated the
 * LAST name MID-TOKEN, then clamped offset to out_sz-1 — emitting malformed,
 * unterminated JSON like
 *   ...,"available_projects":["a",...,"vjson_49_bbb],"count":50}
 * (unclosed string + unclosed array). build_project_list_error wrapped that
 * invalid body into the tool error, so a "project not found" reply was NOT
 * valid JSON once enough projects were indexed.
 *
 * Publish enough long-named projects to overflow the 4 KB buffer, hit the
 * bad-project path, then assert the ERROR BODY (the inner MCP text content)
 * parses as valid JSON and that available_projects is a JSON array whose
 * length == count. The element-boundary fix only ever writes whole "name"
 * tokens. */
TEST(tool_bad_project_error_valid_json_issue235) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-badproj-vjson-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS(); /* skip if mkdtemp fails */
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* 50 * ~120-char names >> 4 KB → the available_projects buffer overflows
     * and the last name would be cut mid-token without the fix. */
    int published = issue235_publish_projects(cache, "vjson", 50);

    char *resp = NULL;
    if (published == 0) {
        cbm_mcp_server_t *srv = cbm_mcp_server_new();
        resp = srv ? cbm_mcp_server_handle(
                         srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
                              "\"name\":\"get_graph_schema\",\"arguments\":{"
                              "\"project\":\"definitely-not-a-real-project-xyz\"}}}")
                   : NULL;
        cbm_mcp_server_free(srv);
    }
    bool not_found = resp && strstr(resp, "not found");

    /* The inner MCP text content is the error body built by
     * build_project_list_error. Capture its validity BEFORE cleanup so a
     * failure still restores the environment. */
    char *body = resp ? extract_text_content(resp) : NULL;
    bool body_valid = false;
    bool aps_ok = false; /* available_projects is an array whose len == count */
    bool truncated = false;
    if (body) {
        yyjson_doc *bdoc = yyjson_read(body, strlen(body), 0);
        if (bdoc) {
            body_valid = true;
            yyjson_val *broot = yyjson_doc_get_root(bdoc);
            yyjson_val *aps = yyjson_obj_get(broot, "available_projects");
            yyjson_val *cnt = yyjson_obj_get(broot, "count");
            if (aps && yyjson_is_arr(aps) && cnt && yyjson_is_int(cnt)) {
                aps_ok = (yyjson_arr_size(aps) == (size_t)yyjson_get_int(cnt));
                truncated = yyjson_arr_size(aps) < 50;
            }
            yyjson_doc_free(bdoc);
        }
    }
    free(body);
    free(resp);

    restore_cache_dir(saved_copy);
    free(saved_copy);
    th_rmtree(cache);

    ASSERT_EQ(published, 0);
    ASSERT_TRUE(not_found);
    ASSERT_TRUE(body_valid);
    ASSERT_TRUE(aps_ok);
    ASSERT_TRUE(truncated); /* the 4 KB buffer was actually exceeded */
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  #845 — supervisor gate must not wrap embedders of cbm_mcp_handle_tool
 * ══════════════════════════════════════════════════════════════════ */

/* Child-side check: index a tiny fixture and verify it ran IN-PROCESS.
 * Distinct exit codes so the parent can report the exact failure mode. */
enum {
    IDX845_OK = 0,
    IDX845_SPAWNED = 41,     /* a worker subprocess was spawned — the #845 bug */
    IDX845_NO_RESULT = 42,   /* handle_tool returned NULL */
    IDX845_NOT_INDEXED = 43, /* response lacks status=indexed */
};

static int idx845_index_inprocess_check(const char *repo_dir) {
    int spawns_before = cbm_index_supervisor_spawn_count();

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    if (!srv) {
        return IDX845_NO_RESULT;
    }
    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", repo_dir);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);

    int code = IDX845_OK;
    if (cbm_index_supervisor_spawn_count() != spawns_before) {
        code = IDX845_SPAWNED;
    } else if (!resp) {
        code = IDX845_NO_RESULT;
    } else if (!response_contains_json_fragment(resp, "\"status\":\"indexed\"")) {
        code = IDX845_NOT_INDEXED;
    }
    free(resp);
    cbm_mcp_server_free(srv);
    return code;
}

TEST(index_supervisor_gate_requires_marked_host_issue845) {
    /* #845: index_repository via cbm_mcp_handle_tool from an EMBEDDER (this test
     * binary) must index IN-PROCESS even with CBM_INDEX_SUPERVISOR unset. The
     * supervisor gate may only wrap a process that called
     * cbm_index_supervisor_mark_host() — i.e. the real binary's main(). Before
     * the fix, should_wrap() was true for ANY embedder: the gate resolved the
     * CURRENT binary (this test runner!) and spawned
     * '<test-runner> cli --index-worker index_repository …', which a test binary
     * interprets as suite-filter args → it re-runs test suites in the child →
     * recursive spawn chains (observed 11-min hangs; kernel VM-map load during
     * the 2026-07-04 host panics).
     *
     * POSIX: run the call in a forked child under alarm(20) so the pre-fix
     * recursive behaviour cannot hang the runner; the child reports via exit
     * code. Windows: no fork — run in-process (safe once the gate is fixed; the
     * pre-fix redness is demonstrated on POSIX). */
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-idx845-repo-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-idx845-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* The point of the guard: NO kill switch. The gate itself must keep an
     * unmarked host in-process. Save + restore the ambient value. */
    const char *saved_sv = getenv("CBM_INDEX_SUPERVISOR");
    char *saved_sv_copy = saved_sv ? strdup(saved_sv) : NULL;
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return 'ok'\n", fp);
    fclose(fp);

    int code = -1;
    bool signalled = false;
    int sig = 0;
#ifdef _WIN32
    code = idx845_index_inprocess_check(tmp_dir);
#else
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(20); /* pre-fix spawn chain must die here, not hang the runner */
        _exit(idx845_index_inprocess_check(tmp_dir));
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalled = true;
        sig = WTERMSIG(status);
    }
#endif

    /* Restore env BEFORE asserting so a red run doesn't leak state. */
    if (saved_sv_copy) {
        cbm_setenv("CBM_INDEX_SUPERVISOR", saved_sv_copy, 1);
        free(saved_sv_copy);
    } else {
        cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    }
    th_remove_zova_user_database();
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);

    if (signalled) {
        printf("    child killed by signal %d (alarm => recursive spawn chain hang)\n", sig);
    } else if (code != IDX845_OK) {
        printf("    child exit code %d (41=worker spawned, 42=no result, 43=not indexed)\n", code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, IDX845_OK);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  #832 — background auto-index + watcher re-index must run in the
 *         supervised worker SUBPROCESS (RSS isolation)
 * ══════════════════════════════════════════════════════════════════ */

/* The long-lived server ran the full index pipeline in-process on two background
 * paths (session auto-index in mcp.c, watcher re-index in main.c). Worker-thread
 * mimalloc heaps abandon pages at thread exit and mimalloc v3
 * (page_reclaim_on_free=0) does not reclaim them when the main thread later frees
 * their blocks, so RSS ratchets across re-index cycles (#832). The fix routes both
 * paths through cbm_mcp_index_run_supervised_path() — the SAME supervised worker
 * subprocess the index_repository tool uses — so the child hands 100%% of its RSS
 * back to the OS on exit.
 *
 * This guard proves the ROUTING: on a supervisor-marked host with the kill switch
 * OFF, the shared entry the watcher/auto-index now call must (a) spawn a worker
 * child (cbm_index_supervisor_spawn_count() increases) and (b) actually index the
 * fixture (the worker child writes the Function node). RED on the unfixed
 * in-process routing: it calls cbm_pipeline_run directly, so spawn_count is
 * unchanged → IDX832_NO_SPAWN. */
enum {
    IDX832_OK = 0,
    IDX832_NO_SPAWN = 51,    /* spawn_count unchanged — routed in-process (RED) */
    IDX832_NULL_RESP = 52,   /* supervised entry degraded to NULL */
    IDX832_NOT_INDEXED = 53, /* response/store lacks the indexed Function node */
    IDX832_SERVER_FAIL = 54,
};

#ifndef _WIN32 /* helper used only by the POSIX fork harness below */
static int idx832_supervised_route_check(const char *repo_dir) {
    /* Become a supervisor host with the kill switch OFF — exactly the real MCP
     * server's state. Done in the FORKED CHILD only (see the harness) so the
     * parent test-runner's process-wide host mark stays clear and the #845
     * unmarked-embedder guard is unaffected. Bound the recovery loop + worker
     * quiet-timeout so a stuck child cannot run long under the fork+alarm net. */
    cbm_index_supervisor_mark_host();
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    cbm_setenv("CBM_INDEX_MAX_RESTARTS", "1", 1);
    cbm_setenv("CBM_INDEX_WORKER_TIMEOUT_S", "30", 1);

    int spawns_before = cbm_index_supervisor_spawn_count();
    char *resp = cbm_mcp_index_run_supervised_path(repo_dir);
    int spawns_after = cbm_index_supervisor_spawn_count();

    if (spawns_after == spawns_before) {
        free(resp);
        return IDX832_NO_SPAWN; /* the discriminating assertion: RED in-process */
    }
    if (!resp) {
        return IDX832_NULL_RESP;
    }
    bool indexed = response_contains_json_fragment(resp, "\"status\":\"indexed\"");
    free(resp);
    if (!indexed) {
        return IDX832_NOT_INDEXED;
    }

    /* Store-level proof the worker child did real work: the Function node it wrote
     * must be queryable from a fresh server reading the DB the child produced. */
    char *project = cbm_project_name_from_path(repo_dir);
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    if (!srv) {
        free(project);
        return IDX832_SERVER_FAIL;
    }
    int code = IDX832_OK;
    if (project) {
        char q[512];
        snprintf(q, sizeof(q),
                 "{\"project\":\"%s\",\"name_pattern\":\"idx832_fn\",\"label\":\"Function\"}",
                 project);
        char *sr = cbm_mcp_handle_tool(srv, "search_graph", q);
        if (!sr || !strstr(sr, "idx832_fn")) {
            code = IDX832_NOT_INDEXED;
        }
        free(sr);
    }
    cbm_mcp_server_free(srv);
    free(project);
    return code;
}
#endif /* !_WIN32 */

TEST(index_bg_paths_route_through_supervisor_issue832) {
#ifdef _WIN32
    /* The guard marks the process as a supervisor host, which cannot be undone.
     * POSIX isolates that in a forked child; without fork we would pollute the
     * shared test-runner (breaking the #845 unmarked-embedder guard). The routing
     * logic is platform-independent and covered on POSIX CI; Windows containment
     * is covered by the end-to-end crash-containment test. */
    SKIP_PLATFORM("supervisor-host guard needs fork isolation (POSIX-only)");
#else
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-idx832-repo-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-idx832-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1); /* inherited by the worker child */

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def idx832_fn():\n    return 'ok'\n", fp);
    fclose(fp);

    int code = -1;
    bool signalled = false;
    int sig = 0;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(60); /* a stuck worker dies here instead of hanging the runner */
        _exit(idx832_supervised_route_check(tmp_dir));
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalled = true;
        sig = WTERMSIG(status);
    }

    th_remove_zova_user_database();
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);

    if (signalled) {
        printf("    child killed by signal %d (alarm => worker hang)\n", sig);
    } else if (code != IDX832_OK) {
        printf("    child exit code %d (51=no spawn/in-process=RED, 52=null resp, "
               "53=not indexed, 54=server fail)\n",
               code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, IDX832_OK);
    PASS();
#endif
}

/* A supervised worker that stops over the memory budget exits nonzero after
 * writing its over_memory_budget response. The supervisor must hand that
 * response back as-is: before, it read the response only on a clean exit,
 * re-ran the whole index as crash recovery, and then reported the abort as
 * "Indexing worker crashed on a file". */
enum {
    OVB_OK = 0,
    OVB_NULL_RESP = 61,  /* supervised entry degraded to NULL */
    OVB_NOT_NAMED = 62,  /* response lacks the over_memory_budget reason */
    OVB_RE_RAN = 63,     /* a graceful error was treated as a crash and re-run */
};

#ifndef _WIN32
static int ovb_supervised_over_budget_check(const char *repo_dir) {
    cbm_index_supervisor_mark_host();
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    cbm_setenv("CBM_INDEX_MAX_RESTARTS", "1", 1);
    cbm_setenv("CBM_INDEX_WORKER_TIMEOUT_S", "30", 1);
    cbm_setenv("CBM_MEM_BUDGET_MB", "1", 1); /* inherited: the worker child runs at 1 MiB */
    cbm_setenv("CBM_WORKERS", "4", 1);

    int spawns_before = cbm_index_supervisor_spawn_count();
    char *resp = cbm_mcp_index_run_supervised_path(repo_dir);
    int spawns = cbm_index_supervisor_spawn_count() - spawns_before;
    if (!resp) {
        return OVB_NULL_RESP;
    }
    bool named = response_contains_json_fragment(resp, "\"reason\":\"over_memory_budget\"");
    if (!named) {
        printf("    supervised over-budget response: %.400s\n", resp);
    }
    free(resp);
    if (!named) {
        return OVB_NOT_NAMED;
    }
    return spawns == 1 ? OVB_OK : OVB_RE_RAN;
}
#endif /* !_WIN32 */

TEST(index_supervised_over_budget_passes_worker_response) {
#if defined(_WIN32) || !CBM_WITH_ZOVA
    PASS();
#else
    char repo[512];
    char cache[512];
    snprintf(repo, sizeof(repo), "%s/cbm-ovb-repo-XXXXXX", cbm_tmpdir());
    snprintf(cache, sizeof(cache), "%s/cbm-ovb-cache-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(repo));
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1); /* inherited by the worker child */
    ASSERT_TRUE(budget_fixture_write(repo, 1));

    int code = -1;
    bool signalled = false;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(120); /* a stuck worker dies here instead of hanging the runner */
        _exit(ovb_supervised_over_budget_check(repo));
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalled = true;
    }

    th_remove_zova_user_database();
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    th_rmtree(repo);
    th_rmtree(cache);

    if (code != OVB_OK) {
        printf("    child exit code %d (61=null resp, 62=reason not named, 63=re-ran)\n", code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, OVB_OK);
    PASS();
#endif
}

/* ══════════════════════════════════════════════════════════════════
 *  Parallel-only crash recovery (ms-typescript cascade fix)
 * ══════════════════════════════════════════════════════════════════ */

/* The old recovery loop re-ran the worker SINGLE-THREADED to keep one exact
 * crash marker. At scale that fell into the sequential crawl, was killed as
 * a hang mid-pass, and the stale marker quarantined FOUR innocent
 * ms-typescript fixtures, one 15-minute retry at a time. The reworked loop
 * re-runs PARALLEL with a marker journal; a file is quarantined only when
 * it is in-flight across two consecutive failed runs.
 *
 * This guard proves the CONTRACT: with an injected crasher among good
 * files, the supervised index must (a) never spawn a single-threaded worker
 * (cbm_index_supervisor_spawn_st_count stays 0 — RED on the old loop),
 * (b) quarantine exactly the crasher, (c) leave the innocents indexed and
 * NOT quarantined. */
enum {
    IDXPAR_OK = 0,
    IDXPAR_ST_SPAWN = 61,      /* single-threaded recovery spawn happened (RED) */
    IDXPAR_NULL_RESP = 62,     /* supervised entry degraded to NULL */
    IDXPAR_NOT_INDEXED = 63,   /* response lacks status indexed */
    IDXPAR_NO_QUARANTINE = 64, /* crasher missing from skipped[] */
    IDXPAR_INNOCENT_HIT = 65,  /* a good file was quarantined/skipped */
    IDXPAR_GOOD_MISSING = 66,  /* good file's Function absent from the store */
};

#ifndef _WIN32
static int idxpar_recovery_check(const char *repo_dir) {
    cbm_index_supervisor_mark_host();
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    /* Rounds needed: fail+record, fail+quarantine, clean. Generous cap. */
    cbm_setenv("CBM_INDEX_MAX_RESTARTS", "5", 1);
    cbm_setenv("CBM_INDEX_WORKER_TIMEOUT_S", "30", 1);
    cbm_setenv("CBM_TEST_CRASH_ON", "idxpar_crasher", 1);

    int st_before = cbm_index_supervisor_spawn_st_count();
    char *resp = cbm_mcp_index_run_supervised_path(repo_dir);
    int st_after = cbm_index_supervisor_spawn_st_count();
    cbm_unsetenv("CBM_TEST_CRASH_ON");

    if (st_after != st_before) {
        free(resp);
        return IDXPAR_ST_SPAWN; /* discriminating assertion: RED on the old loop */
    }
    if (!resp) {
        return IDXPAR_NULL_RESP;
    }
    bool indexed = response_contains_json_fragment(resp, "\"status\":\"indexed\"");
    bool crasher_skipped = strstr(resp, "idxpar_crasher.py") != NULL;
    bool innocent_hit =
        strstr(resp, "idxpar_good_a.py") != NULL || strstr(resp, "idxpar_good_b.py") != NULL;
    free(resp);
    if (!indexed) {
        return IDXPAR_NOT_INDEXED;
    }
    if (!crasher_skipped) {
        return IDXPAR_NO_QUARANTINE;
    }
    if (innocent_hit) {
        return IDXPAR_INNOCENT_HIT;
    }

    /* Store proof: an innocent's Function node exists. */
    char *project = cbm_project_name_from_path(repo_dir);
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    int code = IDXPAR_OK;
    if (srv && project) {
        char q[512];
        snprintf(q, sizeof(q),
                 "{\"project\":\"%s\",\"name_pattern\":\"idxpar_good_fn\",\"label\":\"Function\"}",
                 project);
        char *sr = cbm_mcp_handle_tool(srv, "search_graph", q);
        if (!sr || !strstr(sr, "idxpar_good_fn")) {
            code = IDXPAR_GOOD_MISSING;
        }
        free(sr);
    }
    if (srv) {
        cbm_mcp_server_free(srv);
    }
    free(project);
    return code;
}
#endif /* !_WIN32 */

TEST(index_recovery_parallel_quarantines_crasher) {
#ifdef _WIN32
    SKIP_PLATFORM("parallel-recovery guard needs fork isolation (POSIX-only)");
#else
    const char *tmp_base = getenv("TMPDIR");
    if (!tmp_base || !tmp_base[0]) {
        tmp_base = "/tmp";
    }
    char tmp_dir[CBM_SZ_256];
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/cbm-idxpar-XXXXXX", tmp_base);
    if (!cbm_mkdtemp(tmp_dir)) {
        FAIL("mkdtemp failed");
    }
    char cache[CBM_SZ_256];
    snprintf(cache, sizeof(cache), "%s/cbm-idxpar-cache-XXXXXX", tmp_base);
    if (!cbm_mkdtemp(cache)) {
        FAIL("mkdtemp cache failed");
    }
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? cbm_strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char p1[CBM_SZ_512];
    char p2[CBM_SZ_512];
    char pc[CBM_SZ_512];
    snprintf(p1, sizeof(p1), "%s/idxpar_good_a.py", tmp_dir);
    snprintf(p2, sizeof(p2), "%s/idxpar_good_b.py", tmp_dir);
    snprintf(pc, sizeof(pc), "%s/idxpar_crasher.py", tmp_dir);
    FILE *f = fopen(p1, "w");
    ASSERT_NOT_NULL(f);
    fputs("def idxpar_good_fn():\n    return 'ok'\n", f);
    fclose(f);
    f = fopen(p2, "w");
    ASSERT_NOT_NULL(f);
    fputs("def idxpar_good_fn_b():\n    return 'ok'\n", f);
    fclose(f);
    f = fopen(pc, "w");
    ASSERT_NOT_NULL(f);
    fputs("def idxpar_crash_fn():\n    return 'boom'\n", f);
    fclose(f);

    int code = -1;
    bool signalled = false;
    int sig = 0;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(120); /* generous: three supervised rounds + clean run */
        _exit(idxpar_recovery_check(tmp_dir));
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalled = true;
        sig = WTERMSIG(status);
    }

    th_remove_zova_user_database();
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    remove(p1);
    remove(p2);
    remove(pc);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);

    if (signalled) {
        printf("    child killed by signal %d (alarm => recovery loop hang)\n", sig);
    } else if (code != IDXPAR_OK) {
        printf("    child exit code %d (61=ST spawn/RED, 62=null resp, 63=not indexed, "
               "64=no quarantine, 65=innocent hit, 66=good missing)\n",
               code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, IDXPAR_OK);
    PASS();
#endif
}

/* ══════════════════════════════════════════════════════════════════
 *  AUTO_WATCH GATE  (distilled from PR #625)
 *
 *  Background watcher registration on session connect is gated by the
 *  `auto_watch` config key (default TRUE = existing behavior).
 * ══════════════════════════════════════════════════════════════════ */

/* Drive the already-indexed connect path (initialize → maybe_auto_index →
 * watcher registration) and return the resulting watch count.
 * auto_watch_value: NULL leaves the key unset (exercises the default),
 * otherwise the key is set to that value before initialize.
 * Returns a negative code on fixture setup failure. */
static int auto_watch_connect_watch_count(const char *auto_watch_value) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-autowatch-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        return -1;
    }

    char repodir[512];
    snprintf(repodir, sizeof(repodir), "%s/repo", cache);
    if (th_mkdir_p(repodir) != 0) {
        th_rmtree(cache);
        return -2;
    }

    /* Same derivation detect_session uses on the cwd — realpath-based, so
     * the name matches even where /tmp is a symlink (macOS). */
    char *project = cbm_project_name_from_path(repodir);
    if (!project) {
        th_rmtree(cache);
        return -3;
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* Publish the project to <cache>/cbm.zova so maybe_auto_index takes the
     * "already indexed" branch — the watcher-registration site under test. */
    cbm_store_t *fixture = cbm_store_open_memory();
    int published = fixture && cbm_store_upsert_project(fixture, project, repodir) == CBM_STORE_OK
                        ? th_publish_store_project(fixture, project)
                        : -1;
    if (fixture) {
        cbm_store_close(fixture);
    }
    free(project);
    if (published != 0) {
        restore_cache_dir(saved_copy);
        free(saved_copy);
        th_rmtree(cache);
        return -4;
    }

    char old_cwd[1024];
    if (!cbm_getcwd(old_cwd, sizeof(old_cwd)) || cbm_chdir(repodir) != 0) {
        restore_cache_dir(saved_copy);
        free(saved_copy);
        th_rmtree(cache);
        return -5;
    }

    int count = -6;
    cbm_config_t *cfg = cbm_config_open(cache);
    cbm_store_t *wstore = cbm_store_open_memory();
    cbm_watcher_t *watcher = wstore ? cbm_watcher_new(wstore, NULL, NULL) : NULL;
    if (cfg && watcher) {
        if (auto_watch_value) {
            cbm_config_set(cfg, CBM_CONFIG_AUTO_WATCH, auto_watch_value);
        }

        cbm_mcp_server_t *srv = cbm_mcp_server_new();
        if (srv) {
            cbm_mcp_server_set_watcher(srv, watcher);
            cbm_mcp_server_set_config(srv, cfg);
            char *resp = cbm_mcp_server_handle(
                srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
            free(resp);
            count = cbm_watcher_watch_count(watcher);
            cbm_mcp_server_free(srv);
        }
    }

    if (watcher) {
        cbm_watcher_free(watcher);
    }
    if (wstore) {
        cbm_store_close(wstore);
    }
    if (cfg) {
        cbm_config_close(cfg);
    }

    (void)cbm_chdir(old_cwd);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    th_rmtree(cache);
    return count;
}

/* Default (key unset) → watcher registered on connect. Guards the
 * no-behavior-change promise of the auto_watch gate: existing users keep
 * background auto-sync without touching config. */
TEST(mcp_auto_watch_default_registers_watcher_on_connect) {
    int count = auto_watch_connect_watch_count(NULL);
    if (count < 0) {
        PASS(); /* fixture setup failed (tmpdir/cwd unavailable) — skip */
    }
    ASSERT_EQ(count, 1);
    PASS();
}

/* auto_watch=false → NO watcher registered on connect. RED on pre-gate code
 * (registration was unconditional and the key did not exist). */
TEST(mcp_auto_watch_false_skips_watcher_on_connect) {
    int count = auto_watch_connect_watch_count("false");
    if (count < 0) {
        PASS(); /* fixture setup failed (tmpdir/cwd unavailable) — skip */
    }
    ASSERT_EQ(count, 0);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  #853 — auto_watch=false must ALSO gate the SUPERVISED fresh-index
 *          watcher registration (keystone × #849 merge interaction)
 * ══════════════════════════════════════════════════════════════════ */

/* #849 routed ALL watcher registration through register_watcher_if_enabled()
 * (auto_watch gate). The #832 keystone then added a SECOND registration site in
 * autoindex_thread's supervised-success branch, but wired it as a DIRECT
 * cbm_watcher_watch() guarded only by `if (srv->watcher)` — srv->watcher is set
 * unconditionally, so that guard does NOT honour `config set auto_watch false`.
 * The above tests only cover the already-indexed on-connect path
 * (register_watcher_if_enabled); this guard covers the fresh-index SUPERVISED
 * autoindex_thread branch that #832 introduced.
 *
 * Drive the real public entry initialize → maybe_auto_index → autoindex_thread on
 * a supervisor-marked host (kill switch off) with a FRESH project (not yet in cbm.zova)
 * and auto_watch=false. cbm_mcp_server_free() joins the autoindex thread, so the
 * (buggy or gated) registration decision has run before we read the watch count.
 *
 * RED on the unfixed ungated block: the supervised success branch calls
 * cbm_watcher_watch() unconditionally → watch_count == 1 → IDX853_WATCHER_REGISTERED.
 * GREEN once it calls register_watcher_if_enabled() → auto_watch_off skip → 0.
 * spawn_count is asserted to have advanced so the assertion cannot pass vacuously
 * (i.e. green only because the supervised branch was never entered). */
enum {
    IDX853_OK = 0,                  /* watch_count==0, supervised branch ran → GREEN */
    IDX853_WATCHER_REGISTERED = 61, /* watch_count==1 → RED: ungated cbm_watcher_watch */
    IDX853_NO_SPAWN = 62,           /* spawn_count unchanged → supervised path not exercised */
    IDX853_SETUP_FAIL = 63,         /* config/watcher/server/cwd setup failed */
    IDX853_BAD_COUNT = 64,          /* unexpected watch_count (<0 or >1) */
};

#ifndef _WIN32 /* helper used only by the POSIX fork harness below */
static int idx853_supervised_autowatch_check(const char *repo_dir, const char *cache_dir) {
    /* Become a supervisor host with the kill switch OFF — the real prod MCP
     * server's state. Done in the FORKED CHILD only (see harness) so the parent
     * test-runner's process-wide host mark stays clear (#845 invariant). Bound the
     * worker so a stuck spawn cannot run long under the fork+alarm net. */
    cbm_index_supervisor_mark_host();
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    cbm_setenv("CBM_INDEX_MAX_RESTARTS", "1", 1);
    cbm_setenv("CBM_INDEX_WORKER_TIMEOUT_S", "30", 1);

    cbm_config_t *cfg = cbm_config_open(cache_dir);
    cbm_store_t *wstore = cbm_store_open_memory();
    cbm_watcher_t *watcher = wstore ? cbm_watcher_new(wstore, NULL, NULL) : NULL;
    if (!cfg || !watcher) {
        if (watcher) {
            cbm_watcher_free(watcher);
        }
        if (wstore) {
            cbm_store_close(wstore);
        }
        if (cfg) {
            cbm_config_close(cfg);
        }
        return IDX853_SETUP_FAIL;
    }
    /* auto_index=true → maybe_auto_index launches autoindex_thread for the fresh
     * project; auto_watch=false → the gate this guard exercises. */
    cbm_config_set(cfg, CBM_CONFIG_AUTO_INDEX, "true");
    cbm_config_set(cfg, CBM_CONFIG_AUTO_WATCH, "false");

    /* detect_session derives session_root/session_project from the cwd. */
    char old_cwd[1024];
    if (!cbm_getcwd(old_cwd, sizeof(old_cwd)) || cbm_chdir(repo_dir) != 0) {
        cbm_watcher_free(watcher);
        cbm_store_close(wstore);
        cbm_config_close(cfg);
        return IDX853_SETUP_FAIL;
    }

    int spawns_before = cbm_index_supervisor_spawn_count();
    int code = IDX853_SETUP_FAIL;

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    if (srv) {
        cbm_mcp_server_set_watcher(srv, watcher);
        cbm_mcp_server_set_config(srv, cfg);
        char *resp = cbm_mcp_server_handle(
            srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
        free(resp);
        /* free() joins the autoindex thread → the supervised worker has finished
         * and the registration decision (buggy or gated) has executed. */
        cbm_mcp_server_free(srv);

        int spawns_after = cbm_index_supervisor_spawn_count();
        int watch_count = cbm_watcher_watch_count(watcher);

        if (spawns_after == spawns_before) {
            code = IDX853_NO_SPAWN; /* supervised branch never ran — not a valid probe */
        } else if (watch_count == 1) {
            code = IDX853_WATCHER_REGISTERED; /* the discriminating RED assertion */
        } else if (watch_count == 0) {
            code = IDX853_OK;
        } else {
            code = IDX853_BAD_COUNT;
        }
    }

    (void)cbm_chdir(old_cwd);
    cbm_watcher_free(watcher);
    cbm_store_close(wstore);
    cbm_config_close(cfg);
    return code;
}
#endif /* !_WIN32 */

TEST(mcp_auto_watch_false_skips_supervised_autoindex_issue853) {
#ifdef _WIN32
    /* Marks the process as a supervisor host (irreversible); POSIX isolates that
     * in a forked child. The gate logic is platform-independent and covered on
     * POSIX CI. */
    SKIP_PLATFORM("supervisor-host guard needs fork isolation (POSIX-only)");
#else
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-idx853-repo-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-idx853-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1); /* inherited by the worker child */

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def idx853_fn():\n    return 'ok'\n", fp);
    fclose(fp);

    int code = -1;
    bool signalled = false;
    int sig = 0;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(60); /* a stuck worker dies here instead of hanging the runner */
        _exit(idx853_supervised_autowatch_check(tmp_dir, cache));
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalled = true;
        sig = WTERMSIG(status);
    }

    th_remove_zova_user_database();
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);

    if (signalled) {
        printf("    child killed by signal %d (alarm => worker hang)\n", sig);
    } else if (code != IDX853_OK) {
        printf("    child exit code %d (61=watcher registered under auto_watch=false=RED, "
               "62=no spawn, 63=setup fail, 64=bad count)\n",
               code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, IDX853_OK);
    PASS();
#endif
}

/* The containment guard both MCP file-read sinks route through
 * (resolve_snippet_source for get_code_snippet, attach_result_source for
 * search_code). A result path that resolves outside the indexed project root
 * — via a `..` segment or a followed symlink/junction — must be rejected so
 * its contents never reach a tool response. */
extern bool cbm_path_within_root(const char *root_path, const char *abs_path);

TEST(mcp_path_within_root_rejects_escape) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX realpath repro; the Windows _fullpath branch is the same guard");
#else
    char root[512];
    snprintf(root, sizeof(root), "%s/cbm_pwr_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(root)) {
        FAIL("cbm_mkdtemp failed");
    }
    char inside[700];
    snprintf(inside, sizeof(inside), "%s/inside.c", root);
    FILE *fp = fopen(inside, "w");
    ASSERT_NOT_NULL(fp);
    fputs("int x;\n", fp);
    fclose(fp);

    /* The abs_path a sink builds for an in-root result stays contained; a `..`
     * escape to an existing outside file (/etc/hosts) resolves out and must be
     * rejected. */
    char escape[900];
    snprintf(escape, sizeof(escape), "%s/../../../../etc/hosts", root);
    ASSERT_TRUE(cbm_path_within_root(root, inside));
    ASSERT_FALSE(cbm_path_within_root(root, escape));
    ASSERT_FALSE(cbm_path_within_root(root, "/etc/hosts"));

    remove(inside);
    cbm_rmdir(root);
    PASS();
#endif
}

/* base_branch is spliced into a `git diff --name-only "<base>"...HEAD` command;
 * a value starting with '-' would be taken by git as an option (e.g.
 * --output=<path> writes the diff to an arbitrary file) rather than a ref. It
 * must be rejected up front, alongside the shell-metacharacter check. */
TEST(detect_changes_rejects_option_like_base_branch) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":77,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"detect_changes\","
             "\"arguments\":{\"project\":\"p\",\"base_branch\":\"--output=/tmp/cbm_pwn\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "invalid characters"));
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Opt-in workspace boundary: when CBM_ALLOWED_ROOT is set, index_repository
 * must refuse a repo_path that resolves outside it. Unset (the default) imposes
 * no restriction. */
TEST(index_repository_honors_allowed_root) {
    char allowed[512];
    snprintf(allowed, sizeof(allowed), "%s/cbm_allowed_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(allowed)) {
        FAIL("cbm_mkdtemp failed");
    }
    cbm_setenv("CBM_ALLOWED_ROOT", allowed, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    char args[1024];
    snprintf(args, sizeof(args),
             "{\"jsonrpc\":\"2.0\",\"id\":88,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"index_repository\","
             "\"arguments\":{\"repo_path\":\"%s/../..\"}}}",
             allowed); /* resolves to a parent, outside the allowed root */
    char *resp = cbm_mcp_server_handle(srv, args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "outside the allowed root"));
    free(resp);

    cbm_unsetenv("CBM_ALLOWED_ROOT");
    cbm_mcp_server_free(srv);
    cbm_rmdir(allowed);
    PASS();
}

/* #17 boundary semantics (upstream 7fa5b077): cbm_mcp_check_index_root is the
 * guard behind the MCP index_repository handler. It must use path-COMPONENT
 * boundaries, so a sibling prefix (/home/alice-evil vs root /home/alice) and an
 * already-in-root path are distinguished, and symlink aliases resolve before
 * judging. */
TEST(index_root_boundary_is_component_aware) {
    char base[512];
    snprintf(base, sizeof(base), "%s/cbm_bound_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(base)) {
        FAIL("cbm_mkdtemp(base) failed");
    }
    char inside[640];
    snprintf(inside, sizeof(inside), "%s/inside", base);
    ASSERT_TRUE(cbm_mkdir_p(inside, 0755));
    char sibling[640];
    snprintf(sibling, sizeof(sibling), "%s_evil", base); /* sibling prefix escape */
    ASSERT_TRUE(cbm_mkdir_p(sibling, 0755));

    /* 1. Allowed root unset => every existing path permitted (default). */
    char canon[CBM_SZ_4K];
    const char *reason = NULL;
    ASSERT_EQ(cbm_mcp_check_index_root(inside, canon, sizeof(canon), &reason), 0);
    ASSERT_NULL(reason);

    /* 2. Allowed root set, path inside => permitted, canonicalized. */
    cbm_setenv("CBM_ALLOWED_ROOT", base, 1);
    reason = NULL;
    ASSERT_EQ(cbm_mcp_check_index_root(inside, canon, sizeof(canon), &reason), 0);
    ASSERT_NULL(reason);
    ASSERT_NOT_NULL(strstr(canon, "/inside"));

    /* 3. Sibling prefix escape => REFUSED: /base_evil is NOT inside /base. */
    reason = NULL;
    ASSERT_LT(cbm_mcp_check_index_root(sibling, canon, sizeof(canon), &reason), 0);
    ASSERT_NOT_NULL(reason);
    ASSERT_NOT_NULL(strstr(reason, "outside the allowed root"));

    /* 4. A path outside every allowed root => REFUSED (use the temp dir above
     * base, which is never inside base). */
    reason = NULL;
    ASSERT_LT(cbm_mcp_check_index_root(cbm_tmpdir(), canon, sizeof(canon), &reason), 0);
    ASSERT_NOT_NULL(strstr(reason, "outside the allowed root"));

#ifndef _WIN32
    /* 5. Symlink aliases are judged by their target, and the canonical output
     * is the resolved path (it must outlive the resolution buffer). */
    char real_inside[CBM_SZ_4K];
    ASSERT_NOT_NULL(realpath(inside, real_inside));
    char out_link[640];
    snprintf(out_link, sizeof(out_link), "%s_link_in", base); /* outside -> inside */
    ASSERT_EQ(symlink(inside, out_link), 0);
    char in_link[640];
    snprintf(in_link, sizeof(in_link), "%s/link_out", base); /* inside -> sibling */
    ASSERT_EQ(symlink(sibling, in_link), 0);

    reason = NULL;
    ASSERT_EQ(cbm_mcp_check_index_root(out_link, canon, sizeof(canon), &reason), 0);
    ASSERT_NULL(reason);
    ASSERT_STR_EQ(canon, real_inside);
    reason = NULL;
    ASSERT_LT(cbm_mcp_check_index_root(in_link, canon, sizeof(canon), &reason), 0);
    ASSERT_NOT_NULL(reason);

    cbm_unlink(out_link);
    cbm_unlink(in_link);
#endif

    cbm_unsetenv("CBM_ALLOWED_ROOT");
    cbm_rmdir(inside);
    cbm_rmdir(sibling);
    cbm_rmdir(base);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SUITE
 * ══════════════════════════════════════════════════════════════════ */

/* Tie rule for the C-macro namespace (PR C1): a typedef and its rename macro
 * (`typedef struct state_s\n    state_t;` + `#define state_t NS(state_t)`) are
 * two nodes with one name, `<module>.state_t` and `<module>.state_t#macro`, and
 * here the same line span. The name resolves to the definition: without the
 * macro ranking below it, the two tie and trace_path answers "ambiguous". */
TEST(tool_trace_path_definition_beats_c_macro_c1) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "tie-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/tie");
    cbm_node_t type = {.project = proj,
                       .label = "Type",
                       .name = "state_t",
                       .qualified_name = "tie-proj.xxhash.state_t",
                       .file_path = "xxhash.h",
                       .start_line = 653,
                       .end_line = 654};
    cbm_node_t macro = {.project = proj,
                        .label = "Macro",
                        .name = "state_t",
                        .qualified_name = "tie-proj.xxhash.state_t#macro",
                        .file_path = "xxhash.h",
                        .start_line = 429,
                        .end_line = 430}; /* equal span: a tie without the rule */
    ASSERT_GT(cbm_store_upsert_node(st, &type), 0);
    ASSERT_GT(cbm_store_upsert_node(st, &macro), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":62,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\","
             "\"arguments\":{\"function_name\":\"state_t\",\"project\":\"tie-proj\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NULL(strstr(inner, "ambiguous"));
    ASSERT_NULL(strstr(inner, "suggestions"));
    ASSERT_NULL(strstr(inner, "function not found"));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

SUITE(mcp) {
    RUN_TEST(index_job_queue_is_fifo_and_blocks_second_contender);
    RUN_TEST(mcp_path_within_root_rejects_escape);
    RUN_TEST(detect_changes_rejects_option_like_base_branch);
    RUN_TEST(index_repository_honors_allowed_root);
    RUN_TEST(index_root_boundary_is_component_aware);
    /* JSON-RPC parsing */
    RUN_TEST(jsonrpc_parse_request);
    RUN_TEST(jsonrpc_parse_notification);
    RUN_TEST(jsonrpc_parse_invalid);
    RUN_TEST(jsonrpc_parse_tools_call);
    RUN_TEST(jsonrpc_parse_string_id_issue253);
    RUN_TEST(jsonrpc_format_response_string_id_issue253);

    /* JSON-RPC parsing — edge cases */
    RUN_TEST(jsonrpc_parse_empty_string);
    RUN_TEST(jsonrpc_parse_missing_jsonrpc_field);
    RUN_TEST(jsonrpc_parse_missing_method);
    RUN_TEST(jsonrpc_parse_string_id);
    RUN_TEST(jsonrpc_parse_no_params);
    RUN_TEST(jsonrpc_parse_extra_whitespace);
    RUN_TEST(jsonrpc_parse_array_not_object);

    /* JSON-RPC formatting */
    RUN_TEST(jsonrpc_format_response);
    RUN_TEST(jsonrpc_format_error);

    /* MCP protocol helpers */
    RUN_TEST(mcp_initialize_response);
    RUN_TEST(mcp_tools_list);
    RUN_TEST(mcp_tools_list_latest_metadata);
    RUN_TEST(mcp_index_repository_declares_name_override_issue571);
    RUN_TEST(mcp_tools_array_schemas_have_items);
    RUN_TEST(mcp_get_architecture_aspects_schema_enum_pr560);
    RUN_TEST(mcp_text_result);
    RUN_TEST(mcp_text_result_skips_structured_content_for_plain_text);
    RUN_TEST(mcp_cancel_matches_request_id);
    RUN_TEST(mcp_text_result_error);

    /* Argument extraction */
    RUN_TEST(mcp_get_tool_name);
    RUN_TEST(mcp_get_arguments);
    RUN_TEST(mcp_get_string_arg);
    RUN_TEST(mcp_get_int_arg);
    RUN_TEST(mcp_get_bool_arg);

    /* Argument extraction — edge cases */
    RUN_TEST(mcp_get_string_arg_empty_json);
    RUN_TEST(mcp_get_string_arg_empty_object);
    RUN_TEST(mcp_get_string_arg_nested_value);
    RUN_TEST(mcp_get_string_arg_int_value);
    RUN_TEST(mcp_get_int_arg_empty_json);
    RUN_TEST(mcp_get_int_arg_string_value);
    RUN_TEST(mcp_get_int_arg_bool_value);
    RUN_TEST(mcp_get_int_arg_out_of_range);
    RUN_TEST(mcp_get_bool_arg_empty_json);
    RUN_TEST(mcp_get_bool_arg_int_value);
    RUN_TEST(mcp_get_tool_name_empty_json);
    RUN_TEST(mcp_get_tool_name_missing_name);
    RUN_TEST(mcp_get_arguments_empty_json);
    RUN_TEST(mcp_get_arguments_no_arguments_key);

    /* Server protocol handling */
    RUN_TEST(server_handle_initialize);
    RUN_TEST(server_handle_initialized_notification);
    RUN_TEST(server_handle_tools_list);
    RUN_TEST(server_handle_tools_list_paginates);
    RUN_TEST(server_handle_logs_request_without_params);
    RUN_TEST(server_handle_unknown_method);

    /* Server handle — edge cases */
    RUN_TEST(server_handle_invalid_json);
    RUN_TEST(server_handle_empty_object);
    RUN_TEST(server_handle_tools_call_missing_name);

    /* Tool handlers */
    RUN_TEST(tool_list_projects_empty);
    RUN_TEST(tool_get_graph_schema_empty);
    RUN_TEST(tool_unknown_tool);
    RUN_TEST(tool_get_file_outline_returns_bounded_filtered_rows);
    RUN_TEST(tool_get_file_outline_validates_arguments);
    RUN_TEST(tool_compare_graphs_reports_bounded_project_independent_diff);
    RUN_TEST(tool_compare_graphs_validates_arguments);
    RUN_TEST(tool_compare_graphs_diffs_two_indexed_repositories);
    RUN_TEST(tool_search_graph_basic);
    RUN_TEST(tool_search_graph_semantic_only_skips_structural_results_issue1295);
    RUN_TEST(tool_search_graph_empty_structural_search_keeps_envelope);
    RUN_TEST(search_graph_schema_accepts_project_or_projects);
    RUN_TEST(tool_search_graph_rejects_project_and_projects);
    RUN_TEST(tool_search_graph_rejects_empty_projects);
    RUN_TEST(tool_search_graph_searches_two_zova_projects_globally);
    RUN_TEST(tool_search_graph_cursors_page_structural_and_semantic_rows_once);
    RUN_TEST(tool_index_repository_reports_zova_committed_counts);
    RUN_TEST(tool_check_index_coverage_reports_recorded_gaps);
    RUN_TEST(tool_check_index_coverage_exact_paths_beyond_partial_cap);
    RUN_TEST(tool_check_index_coverage_reports_unusable_and_truncated_ranges);
    RUN_TEST(tool_check_index_coverage_survives_mode_downgrade);
    RUN_TEST(mcp_render_compact_tables_and_scalars);
    RUN_TEST(tool_list_projects_pages_and_detects_catalog_changes);
    RUN_TEST(tool_index_repository_refuses_while_workspace_lease_is_held);
    RUN_TEST(tool_search_graph_includes_node_properties);
    RUN_TEST(tool_search_graph_query_honors_file_pattern_issue552);
    RUN_TEST(tool_query_graph_basic);
    RUN_TEST(tool_query_graph_count_ignores_max_rows);
    RUN_TEST(tool_index_status_no_project);
    RUN_TEST(tool_index_status_includes_git_metadata);

    /* Tool handlers with validation */
    RUN_TEST(tool_trace_call_path_not_found);
    RUN_TEST(tool_trace_missing_function_name);
    RUN_TEST(tool_trace_call_path_ambiguous);
    RUN_TEST(tool_trace_call_path_prefers_definition);
    RUN_TEST(tool_trace_call_path_hides_root_tests_subtree_issue1294);
    RUN_TEST(tool_trace_call_path_depth_clamped);
    RUN_TEST(tool_trace_call_path_distinct_defs_not_over_unioned);
    RUN_TEST(tool_trace_call_path_dts_stub_unions_with_impl);
    RUN_TEST(tool_trace_call_path_same_name_union_orders_visits);
    RUN_TEST(tool_delete_project_not_found);
    RUN_TEST(tool_get_architecture_empty);
    RUN_TEST(tool_get_architecture_emits_populated_sections);
    RUN_TEST(tool_get_architecture_overview_compact_subset_pr560);
    RUN_TEST(tool_get_architecture_rejects_unknown_aspect_pr560);
    RUN_TEST(tool_get_architecture_accepts_project_name_alias_issue640);
    RUN_TEST(tool_search_graph_accepts_project_name_alias_issue640);
    RUN_TEST(tool_get_architecture_path_scoping);
    RUN_TEST(tool_query_graph_missing_query);

    /* Pipeline-dependent tool handlers */
    RUN_TEST(tool_index_repository_missing_path);
    RUN_TEST(tool_index_repository_accepts_path_alias);
    RUN_TEST(tool_get_code_snippet_missing_qn);
    RUN_TEST(tool_get_code_snippet_not_found);
    RUN_TEST(tool_search_code_missing_pattern);
    RUN_TEST(tool_search_code_no_project);
    RUN_TEST(search_code_multi_word);
    RUN_TEST(search_code_scoped_path_with_spaces_issue687);
    RUN_TEST(search_code_scoped_file_pattern_is_busybox_portable);
    RUN_TEST(store_list_files_prefers_canonical_file_nodes);
    RUN_TEST(index_repository_over_budget_reports_named_reason);
    RUN_TEST(index_supervised_over_budget_passes_worker_response);
    RUN_TEST(search_code_path_filter_prefilter_keeps_matches);
    RUN_TEST(search_code_path_filter_matches_nothing);
    RUN_TEST(search_code_invalid_regex_errors_issue283);
    RUN_TEST(search_code_literal_pipe_warns_issue282);
    RUN_TEST(search_code_ampersand_accepted_issue272);
    RUN_TEST(search_scan_matches_literal_multi_regex_and_empty);
    RUN_TEST(search_scan_regex_gnu_escapes);
    RUN_TEST(search_scan_skips_binary_and_stops_at_late_nul);
    RUN_TEST(search_scan_crlf_lines);
    RUN_TEST(search_scan_stops_at_limit_in_list_order);
    RUN_TEST(search_scan_skips_missing_file);
    RUN_TEST(search_scan_reports_deadline);
    RUN_TEST(search_scan_long_line);
    RUN_TEST(search_code_full_mode_after_long_line);
    RUN_TEST(search_code_literal_is_case_sensitive);
    RUN_TEST(search_code_unindexed_project_scans_recursively);
    RUN_TEST(tool_detect_changes_no_project);
    RUN_TEST(tool_index_repository_dot_uses_absolute_project_key);
    RUN_TEST(index_supervisor_gate_requires_marked_host_issue845);
    RUN_TEST(index_bg_paths_route_through_supervisor_issue832);
    RUN_TEST(index_recovery_parallel_quarantines_crasher);
    RUN_TEST(tool_detect_changes_not_found_rich_error);
    RUN_TEST(tool_detect_changes_reports_impacted_symbols);

    /* Query store read-only (data integrity) */

    /* Idle store eviction */
    RUN_TEST(store_idle_eviction);
    RUN_TEST(store_idle_no_eviction_within_timeout);
    RUN_TEST(store_idle_evict_protects_initial_store);
    RUN_TEST(store_idle_evict_access_resets_timer);

    /* URI helpers */
    RUN_TEST(parse_file_uri_unix);
    RUN_TEST(parse_file_uri_windows);
    RUN_TEST(parse_file_uri_invalid);

    /* URI helpers — edge cases */
    RUN_TEST(parse_file_uri_http_scheme);
    RUN_TEST(parse_file_uri_ftp_scheme);
    RUN_TEST(parse_file_uri_buffer_too_small);
    RUN_TEST(parse_file_uri_spaces_in_path);
    RUN_TEST(parse_file_uri_null_out_path);
    RUN_TEST(parse_file_uri_zero_size);

    /* Poll/getline FILE* buffering fix */
#ifndef _WIN32
    RUN_TEST(mcp_server_run_rapid_messages);
#endif

    /* Snippet resolution (port of snippet_test.go) */
    RUN_TEST(snippet_exact_qn);
    RUN_TEST(snippet_line_longer_than_read_buffer);
    RUN_TEST(snippet_qn_suffix);
    RUN_TEST(snippet_unique_short_name);
    RUN_TEST(snippet_c_macro_namespace_c1);
    RUN_TEST(snippet_name_tier);
    RUN_TEST(snippet_ambiguous_short_name);
    RUN_TEST(snippet_not_found);
    RUN_TEST(snippet_fuzzy_suggestions);
    RUN_TEST(snippet_enriched_properties);
    RUN_TEST(snippet_fuzzy_last_segment);
    RUN_TEST(snippet_auto_resolve_default);
    RUN_TEST(snippet_auto_resolve_enabled);
    RUN_TEST(snippet_include_neighbors_default);
    RUN_TEST(snippet_include_neighbors_enabled);
    RUN_TEST(snippet_source_invalid_utf8);
    RUN_TEST(tool_bad_project_name_no_overflow_issue235);
    RUN_TEST(tool_bad_project_error_valid_json_issue235);

    /* auto_watch gate (distilled from PR #625) */
    RUN_TEST(mcp_auto_watch_default_registers_watcher_on_connect);
    RUN_TEST(mcp_auto_watch_false_skips_watcher_on_connect);
    RUN_TEST(mcp_auto_watch_false_skips_supervised_autoindex_issue853);
    RUN_TEST(tool_trace_path_definition_beats_c_macro_c1);
}
