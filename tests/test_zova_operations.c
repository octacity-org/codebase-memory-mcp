#include "test_framework.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/sha256.h"
#include "mcp/mcp.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_coordination.h"
#include "zova/cbm_zova_operations.h"
#include "zova/cbm_zova_repository.h"
#include "zova/cbm_zova_writer_gate.h"

#if CBM_WITH_ZOVA
#include "zova.h"
#endif

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#endif
#include <unistd.h>

enum { OPERATIONS_PATH_MAX = 512 };

#if CBM_WITH_ZOVA
typedef struct {
    cbm_mcp_server_t *server;
    const char *path;
    char label;
    cbm_zova_workspace_generation_input_t input;
    cbm_zova_workspace_generation_result_t result;
    _Atomic int *entry_release;
    _Atomic int *publisher_release;
    _Atomic int *publisher_held;
    _Atomic int *entry_count;
    char *entry_log;
    int rc;
} operations_queue_publisher_t;

static void operations_wait_for_atomic(_Atomic int *value, int expected) {
    for (int i = 0; i < 5000 && atomic_load_explicit(value, memory_order_acquire) != expected;
         ++i) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        cbm_nanosleep(&pause, NULL);
    }
}

static void *operations_queue_publish(void *arg) {
    operations_queue_publisher_t *publisher = arg;
    cbm_index_job_queue_enter(publisher->server);
    int entry = atomic_fetch_add_explicit(publisher->entry_count, 1, memory_order_acq_rel);
    if (entry >= 0 && entry < 3) publisher->entry_log[entry] = publisher->label;
    operations_wait_for_atomic(publisher->entry_release, 1);

    zova_database *db = NULL;
    zova_message error = {0};
    publisher->rc = -1;
    if (zova_database_open(&(zova_database_open_request){
            .path = publisher->path, .out_db = &db, .out_error_message = &error}) == ZOVA_OK &&
        db && zova_database_begin_immediate(&(zova_database_simple_request){.db = db}) == ZOVA_OK &&
        cbm_zova_user_database_publish_workspace_tx(db, &publisher->input,
                                                    &publisher->result) == 0) {
        if (publisher->publisher_held) {
            atomic_store_explicit(publisher->publisher_held, 1, memory_order_release);
            operations_wait_for_atomic(publisher->publisher_release, 1);
        }
        publisher->rc =
            zova_database_commit(&(zova_database_simple_request){.db = db}) == ZOVA_OK ? 0 : -1;
    }
    if (publisher->rc != 0 && db)
        (void)zova_database_rollback(&(zova_database_simple_request){.db = db});
    if (db) (void)zova_database_close(db);
    zova_message_free(&error);
    cbm_index_job_queue_leave(publisher->server);
    return NULL;
}

static int operations_repository_inventory(const char *path, const char *project,
                                           int64_t expected_generation, int expected_nodes,
                                           int expected_edges, int expected_node_vectors,
                                           int expected_token_vectors) {
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, project);
    if (!repo) return -1;
    const char *workspace_id = cbm_zova_repository_workspace_id(repo);
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    char token_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    int64_t generation = 0;
    char *indexed_at = NULL;
    int nodes = -1, edges = -1;
    cbm_search_output_t fts = {0};
    int rc = cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) == 0 &&
                     cbm_zova_workspace_node_vector_collection_name(
                         workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 2, node_collection,
                         sizeof(node_collection)) == 0 &&
                     cbm_zova_workspace_token_vector_collection_name(
                         workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 2, token_collection,
                         sizeof(token_collection)) == 0 &&
                     cbm_zova_repository_index_status(repo, workspace_id, &generation,
                                                      &indexed_at) == CBM_STORE_OK &&
                     cbm_zova_repository_counts(repo, workspace_id, &nodes, &edges) ==
                         CBM_STORE_OK &&
                     cbm_zova_repository_search_fts(repo, workspace_id, "root", NULL, 10, 0,
                                                    &fts) == CBM_STORE_OK &&
                     generation == expected_generation && nodes == expected_nodes &&
                     edges == expected_edges && fts.total == expected_nodes &&
                     fts.count == expected_nodes
                 ? 0
                 : -1;
    zova_database *db = NULL;
    zova_message error = {0};
    zova_graph_info graph = {0};
    zova_vector_collection_info node_vectors = {0};
    zova_vector_collection_info token_vectors = {0};
    if (rc == 0 &&
        (zova_database_open_with_options(&(zova_database_open_options_request){
             .path = path, .flags = ZOVA_OPEN_READ_ONLY, .busy_timeout_ms = 5000,
             .out_db = &db, .out_error_message = &error}) != ZOVA_OK ||
         !db ||
         zova_graph_info_get(&(zova_graph_info_get_request){
             .db = db, .name = graph_name, .out_info = &graph}) != ZOVA_OK ||
         zova_vector_collection_info_get(&(zova_vector_collection_info_get_request){
             .db = db, .name = node_collection, .out_info = &node_vectors}) != ZOVA_OK ||
         zova_vector_collection_info_get(&(zova_vector_collection_info_get_request){
             .db = db, .name = token_collection, .out_info = &token_vectors}) != ZOVA_OK ||
         graph.node_count != (uint64_t)expected_nodes ||
         graph.edge_count != (uint64_t)expected_edges ||
         node_vectors.vector_count != (uint64_t)expected_node_vectors ||
         token_vectors.vector_count != (uint64_t)expected_token_vectors)) {
        rc = -1;
    }
    zova_graph_info_free(&graph);
    zova_vector_collection_info_free(&node_vectors);
    zova_vector_collection_info_free(&token_vectors);
    if (db) (void)zova_database_close(db);
    zova_message_free(&error);
    free(indexed_at);
    cbm_store_search_free(&fts);
    cbm_zova_repository_close(repo);
    return rc;
}

static int operations_run_three_publishers(operations_queue_publisher_t publishers[3],
                                           _Atomic int entry_release[3],
                                           _Atomic int *publisher_release,
                                           _Atomic int *publisher_held,
                                           _Atomic int *entry_count) {
    cbm_thread_t threads[3];
    int created = 0;
    int rc = -1;
    if (cbm_thread_create(&threads[0], 0, operations_queue_publish, &publishers[0]) != 0)
        goto cleanup;
    created = 1;
    operations_wait_for_atomic(entry_count, 1);
    if (atomic_load_explicit(entry_count, memory_order_acquire) != 1) goto cleanup;
    atomic_store_explicit(&entry_release[0], 1, memory_order_release);
    operations_wait_for_atomic(publisher_held, 1);
    if (atomic_load_explicit(publisher_held, memory_order_acquire) != 1) goto cleanup;

    if (cbm_thread_create(&threads[1], 0, operations_queue_publish, &publishers[1]) != 0)
        goto cleanup;
    created = 2;
    for (int i = 0;
         i < 5000 && cbm_index_job_queue_waiter_count(publishers[0].server) != 1; ++i) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        cbm_nanosleep(&pause, NULL);
    }
    if (cbm_index_job_queue_waiter_count(publishers[0].server) != 1) goto cleanup;
    if (cbm_thread_create(&threads[2], 0, operations_queue_publish, &publishers[2]) != 0)
        goto cleanup;
    created = 3;
    for (int i = 0;
         i < 5000 && cbm_index_job_queue_waiter_count(publishers[0].server) != 2; ++i) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        cbm_nanosleep(&pause, NULL);
    }
    if (cbm_index_job_queue_waiter_count(publishers[0].server) != 2) goto cleanup;

    for (int i = 0; i < 20; ++i) {
        if (operations_repository_inventory(publishers[0].path, "queue-a", 1, 1, 0, 1, 1) !=
                0 ||
            operations_repository_inventory(publishers[0].path, "queue-b", 1, 1, 0, 1, 1) !=
                0)
            goto cleanup;
        cbm_zova_repository_t *absent =
            cbm_zova_repository_open(publishers[0].path, "queue-c");
        if (absent) {
            cbm_zova_repository_close(absent);
            goto cleanup;
        }
    }

    atomic_store_explicit(publisher_release, 1, memory_order_release);
    operations_wait_for_atomic(entry_count, 2);
    if (atomic_load_explicit(entry_count, memory_order_acquire) != 2) goto cleanup;
    atomic_store_explicit(&entry_release[1], 1, memory_order_release);
    operations_wait_for_atomic(entry_count, 3);
    if (atomic_load_explicit(entry_count, memory_order_acquire) != 3) goto cleanup;
    atomic_store_explicit(&entry_release[2], 1, memory_order_release);
    rc = 0;

cleanup:
    atomic_store_explicit(publisher_release, 1, memory_order_release);
    for (int i = 0; i < 3; ++i)
        atomic_store_explicit(&entry_release[i], 1, memory_order_release);
    for (int i = 0; i < created; ++i) {
        if (cbm_thread_join(&threads[i]) != 0) rc = -1;
    }
    if (created != 3 || publishers[0].rc != 0 || publishers[1].rc != 0 ||
        publishers[2].rc != 0)
        rc = -1;
    return rc;
}

static int operations_publish_fixture(const char *path, const char *root_path,
                                      const char *project, int node_count,
                                      cbm_zova_workspace_generation_result_t *out) {
    const CBMDumpNode nodes[] = {
        {.id = 1, .label = "Function", .name = "root", .qualified_name = "fixture.root_one",
         .file_path = "root1.c", .properties = "{}"},
        {.id = 2, .label = "Function", .name = "root", .qualified_name = "fixture.root_two",
         .file_path = "root2.c", .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1, .source_id = 1, .target_id = 2, .type = "CALLS", .properties = "{}",
         .url_path = "", .local_name = ""},
    };
    const int8_t first[] = {127, 0};
    const int8_t second[] = {0, 127};
    const CBMDumpVector node_vectors[] = {
        {.node_id = 1, .vector = (const uint8_t *)first, .vector_len = 2},
        {.node_id = 2, .vector = (const uint8_t *)second, .vector_len = 2},
    };
    const CBMDumpTokenVec token_vectors[] = {
        {.id = 1, .token = "root-one", .vector = (const uint8_t *)first,
         .vector_len = 2, .idf = 1.0f},
        {.id = 2, .token = "root-two", .vector = (const uint8_t *)second,
         .vector_len = 2, .idf = 1.0f},
    };
    CBMDumpNode scoped_nodes[2];
    CBMDumpEdge scoped_edges[1];
    CBMDumpVector scoped_node_vectors[2];
    CBMDumpTokenVec scoped_token_vectors[2];
    memcpy(scoped_nodes, nodes, sizeof(nodes));
    memcpy(scoped_edges, edges, sizeof(edges));
    memcpy(scoped_node_vectors, node_vectors, sizeof(node_vectors));
    memcpy(scoped_token_vectors, token_vectors, sizeof(token_vectors));
    for (int i = 0; i < 2; ++i) {
        scoped_nodes[i].project = project;
        scoped_node_vectors[i].project = project;
        scoped_token_vectors[i].project = project;
    }
    scoped_edges[0].project = project;
    cbm_zova_workspace_generation_input_t input = {
        .root_path = root_path, .project = project, .indexed_at = "2026-07-13T01:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 2,
        .nodes = scoped_nodes, .node_count = node_count,
        .edges = node_count == 2 ? scoped_edges : NULL, .edge_count = node_count == 2 ? 1 : 0,
        .node_vectors = scoped_node_vectors, .node_vector_count = node_count,
        .token_vectors = scoped_token_vectors, .token_vector_count = node_count,
    };
    return cbm_zova_user_database_publish_workspace(path, &input, out);
}

static int operations_publish_rich_fixture_named(
    const char *path, const char *root_path, const char *project, const char *summary,
    cbm_zova_workspace_generation_result_t *out) {
    const CBMDumpNode nodes[] = {
        {.id = 11, .project = project, .label = "Function",
         .name = "parse::alpha/beta?", .qualified_name = "rich.parse::alpha/beta?",
         .file_path = "src/rich.c", .start_line = 3, .end_line = 9,
         .properties = "{\"visibility\":\"public\"}"},
        {.id = 12, .project = project, .label = "Variable",
         .name = "<local>#value", .qualified_name = "rich.parse::<local>#value",
         .file_path = "src/rich.c", .start_line = 5, .end_line = 5,
         .properties = "{\"scope\":\"local\"}"},
        {.id = 13, .project = project, .label = "Function",
         .name = "sink!", .qualified_name = "rich.sink!", .file_path = "src/sink.c",
         .start_line = 20, .end_line = 25, .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 21, .project = project, .source_id = 11, .target_id = 13,
         .type = "CALLS", .properties = "{\"site\":1}", .url_path = "/v1/a?x=1",
         .local_name = "first::call"},
        {.id = 22, .project = project, .source_id = 11, .target_id = 13,
         .type = "CALLS", .properties = "{\"site\":2}", .url_path = "/v1/a?x=2",
         .local_name = "second/call"},
    };
    const int8_t first[] = {127, -128, 9};
    const int8_t second[] = {-7, 0, 7};
    const int8_t third[] = {1, 2, 3};
    const CBMDumpVector node_vectors[] = {
        {.node_id = 11, .project = project, .vector = (const uint8_t *)first,
         .vector_len = 3},
        {.node_id = 12, .project = project, .vector = (const uint8_t *)second,
         .vector_len = 3},
        {.node_id = 13, .project = project, .vector = (const uint8_t *)third,
         .vector_len = 3},
    };
    const CBMDumpTokenVec token_vectors[] = {
        {.id = 31, .project = project, .token = "alpha/beta?",
         .vector = (const uint8_t *)first, .vector_len = 3, .idf = 1.25f},
        {.id = 32, .project = project, .token = "local#value",
         .vector = (const uint8_t *)second, .vector_len = 3, .idf = 2.5f},
    };
    const cbm_zova_file_hash_input_t hashes[] = {
        {.file_path = "src/rich.c", .content_hash = "rich-hash", .mtime_ns = 101,
         .size_bytes = 202},
        {.file_path = "src/sink.c", .content_hash = "sink-hash", .mtime_ns = 303,
         .size_bytes = 404},
    };
    const cbm_zova_workspace_generation_input_t input = {
        .root_path = root_path, .project = project,
        .indexed_at = "2026-07-13T02:03:04Z",
        .model_fingerprint = "rich_model_v2", .vector_dimensions = 3,
        .nodes = nodes, .node_count = 3, .edges = edges, .edge_count = 2,
        .node_vectors = node_vectors, .node_vector_count = 3,
        .token_vectors = token_vectors, .token_vector_count = 2,
        .file_hashes = hashes, .file_hash_count = 2,
        .project_summary = {.present = true, .summary = summary,
                            .source_hash = "summary-hash",
                            .created_at = "2026-07-13T02:03:04Z",
                            .updated_at = "2026-07-13T02:04:05Z"},
    };
    return cbm_zova_user_database_publish_workspace(path, &input, out);
}

static int operations_publish_rich_fixture_variant(
    const char *path, const char *summary,
    cbm_zova_workspace_generation_result_t *out) {
    return operations_publish_rich_fixture_named(path, "/tmp/workspace-rich", "workspace-rich",
                                                  summary, out);
}

static int operations_publish_rich_fixture(const char *path,
                                           cbm_zova_workspace_generation_result_t *out) {
    return operations_publish_rich_fixture_variant(path, "{\"summary\":\"rich!\"}", out);
}
#endif

static void operations_path(char *out, size_t out_size, const char *name) {
    snprintf(out, out_size, "%s/cbm-zova-operations-%s-%d.zova", cbm_tmpdir(), name,
             (int)getpid());
    cbm_unlink(out);
}

static void operations_archive_path(char *out, size_t out_size, const char *name) {
    snprintf(out, out_size, "%s/cbm-zova-operations-%s-%d", cbm_tmpdir(), name,
             (int)getpid());
}

static void operations_archive_cleanup(const char *archive) {
    char path[OPERATIONS_PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/manifest.json", archive); cbm_unlink(path);
    snprintf(path, sizeof(path), "%s/data.zova-wal", archive); cbm_unlink(path);
    snprintf(path, sizeof(path), "%s/data.zova-shm", archive); cbm_unlink(path);
    snprintf(path, sizeof(path), "%s/data.zova-journal", archive); cbm_unlink(path);
    snprintf(path, sizeof(path), "%s/data.zova", archive); cbm_unlink(path);
    (void)rmdir(archive);
    snprintf(path, sizeof(path), "%s.partial/manifest.json", archive); cbm_unlink(path);
    snprintf(path, sizeof(path), "%s.partial/data.zova", archive); cbm_unlink(path);
    snprintf(path, sizeof(path), "%s.partial", archive); (void)rmdir(path);
}

static int operations_archive_has_two_exact_members(const char *archive) {
    cbm_dir_t *directory = cbm_opendir(archive);
    if (!directory) return 0;
    int data = 0, manifest = 0, unexpected = 0;
    cbm_dirent_t *entry = NULL;
    while ((entry = cbm_readdir(directory)) != NULL) {
        if (strcmp(entry->name, "data.zova") == 0) data++;
        else if (strcmp(entry->name, "manifest.json") == 0) manifest++;
        else unexpected++;
    }
    cbm_closedir(directory);
    return data == 1 && manifest == 1 && unexpected == 0;
}

static int operations_manifest_replace_char(const char *archive, const char *needle,
                                            size_t offset, char replacement) {
    char path[OPERATIONS_PATH_MAX + 64], buffer[16384];
    snprintf(path, sizeof(path), "%s/manifest.json", archive);
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
    int rc = ferror(file) ? -1 : 0;
    fclose(file);
    buffer[length] = '\0';
    char *match = rc == 0 ? strstr(buffer, needle) : NULL;
    if (!match || (size_t)(match - buffer) + offset >= length) return -1;
    match[offset] = match[offset] == replacement ? (replacement == '0' ? '1' : '0') : replacement;
    file = fopen(path, "wb");
    if (!file) return -1;
    rc = fwrite(buffer, 1, length, file) == length && fflush(file) == 0 ? 0 : -1;
    fclose(file);
    return rc;
}

static int operations_manifest_read(const char *archive, char *out, size_t out_size,
                                    size_t *out_length) {
    char path[OPERATIONS_PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/manifest.json", archive);
    FILE *file = fopen(path, "rb");
    if (!file || !out || out_size == 0) {
        if (file) fclose(file);
        return -1;
    }
    size_t length = fread(out, 1, out_size - 1, file);
    int rc = !ferror(file) && feof(file) ? 0 : -1;
    fclose(file);
    out[length] = '\0';
    if (rc == 0 && out_length) *out_length = length;
    return rc;
}

static int operations_manifest_write(const char *archive, const char *text, size_t length) {
    char path[OPERATIONS_PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/manifest.json", archive);
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    int rc = fwrite(text, 1, length, file) == length && fflush(file) == 0 ? 0 : -1;
    fclose(file);
    return rc;
}

static int operations_sql(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    sqlite3_close(db);
    return rc;
}

static int64_t operations_scalar(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int64_t value = -1;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

static int64_t operations_sqlite_scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement = NULL;
    int64_t value = -1;
    if (db && sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    return value;
}

static int operations_text(const char *path, const char *sql, char *out, size_t out_size) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (out && out_size > 0) out[0] = '\0';
    if (out && out_size > 0 &&
        sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *value = sqlite3_column_text(stmt, 0);
        if (value && snprintf(out, out_size, "%s", value) < (int)out_size) rc = 0;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

static int operations_file_digest(const char *path, uint8_t out[CBM_SHA256_DIGEST_LEN]) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    uint8_t buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        cbm_sha256_update(&hash, buffer, count);
    int rc = ferror(file) ? -1 : 0;
    fclose(file);
    if (rc == 0) cbm_sha256_final(&hash, out);
    return rc;
}

static int operations_refresh_archive_database_digest(const char *archive) {
    char data[OPERATIONS_PATH_MAX + 64], manifest[OPERATIONS_PATH_MAX + 64], buffer[16384];
    snprintf(data, sizeof(data), "%s/data.zova", archive);
    snprintf(manifest, sizeof(manifest), "%s/manifest.json", archive);
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    if (operations_file_digest(data, digest) != 0) return -1;
    char hex[CBM_SHA256_HEX_LEN + 1];
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[CBM_SHA256_HEX_LEN] = '\0';
    FILE *file = fopen(manifest, "rb");
    if (!file) return -1;
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
    int rc = ferror(file) ? -1 : 0;
    fclose(file);
    buffer[length] = '\0';
    char *value = rc == 0 ? strstr(buffer, "\"sha256\":\"") : NULL;
    if (!value) return -1;
    value += strlen("\"sha256\":\"");
    if ((size_t)(value - buffer) + CBM_SHA256_HEX_LEN > length) return -1;
    memcpy(value, hex, CBM_SHA256_HEX_LEN);
    file = fopen(manifest, "wb");
    if (!file) return -1;
    rc = fwrite(buffer, 1, length, file) == length && fflush(file) == 0 ? 0 : -1;
    fclose(file);
    return rc;
}

#if CBM_WITH_ZOVA
typedef struct {
    cbm_zova_workspace_snapshot_t snapshot;
    cbm_search_output_t fts;
} operations_workspace_state_t;

typedef struct {
    int fts_count;
    char fts_qualified_name[128];
    int edge_count;
    char edge_type[64];
    int vector_count;
    int64_t vector_node_id;
    uint8_t vector_first_byte;
} operations_workspace_public_state_t;

static int operations_workspace_public_state_capture(
    const char *path, const char *workspace_id, const char *project,
    operations_workspace_public_state_t *out) {
    memset(out, 0, sizeof(*out));
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, project);
    if (!repo) return -1;
    cbm_search_output_t fts = {0};
    int rc = cbm_zova_repository_search_fts(repo, workspace_id, "alpha beta", NULL, 10, 0,
                                             &fts) == CBM_STORE_OK &&
                     fts.count == 1 && fts.results[0].node.qualified_name
                 ? 0 : -1;
    if (rc == 0) {
        out->fts_count = fts.count;
        snprintf(out->fts_qualified_name, sizeof(out->fts_qualified_name), "%s",
                 fts.results[0].node.qualified_name);
    }
    char stable_id[128] = {0};
    if (rc == 0) {
        cbm_node_t node = {0};
        rc = cbm_zova_repository_find_node_by_qn(
                 repo, workspace_id, "rich.parse::alpha/beta?", &node);
        if (rc == CBM_STORE_OK)
            rc = cbm_zova_repository_stable_id_for_numeric_id(
                repo, workspace_id, node.id, stable_id, sizeof(stable_id));
        cbm_node_free_fields(&node);
    }
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    if (rc == 0)
        rc = cbm_zova_repository_find_edges(repo, workspace_id, stable_id, "outbound", &edges,
                                             &edge_count) == CBM_STORE_OK && edge_count == 2
                 ? 0 : -1;
    if (rc == 0) {
        out->edge_count = edge_count;
        snprintf(out->edge_type, sizeof(out->edge_type), "%s", edges[0].type);
    }
    cbm_store_free_edges(edges, edge_count);
    cbm_store_search_free(&fts);
    cbm_zova_repository_close(repo);

    cbm_zova_workspace_snapshot_t snapshot = {0};
    if (rc == 0)
        rc = cbm_zova_repository_export_snapshot(path, workspace_id, &snapshot) == 0 &&
                     snapshot.node_vector_count == 3 && snapshot.node_vectors[0].vector_len == 3 &&
                     snapshot.node_vectors[0].vector
                 ? 0 : -1;
    if (rc == 0) {
        out->vector_count = snapshot.node_vector_count;
        out->vector_node_id = snapshot.node_vectors[0].node_id;
        out->vector_first_byte = snapshot.node_vectors[0].vector[0];
    }
    cbm_zova_workspace_snapshot_free(&snapshot);
    return rc;
}

static int operations_stable_id_for_qn(const char *path, const char *project,
                                       const char *workspace_id, const char *qualified_name,
                                       char *out, size_t out_size) {
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, project);
    if (!repo) return -1;
    cbm_node_t node = {0};
    int rc = cbm_zova_repository_find_node_by_qn(repo, workspace_id, qualified_name, &node);
    if (rc == CBM_STORE_OK)
        rc = cbm_zova_repository_stable_id_for_numeric_id(
            repo, workspace_id, node.id, out, out_size);
    cbm_node_free_fields(&node);
    cbm_zova_repository_close(repo);
    return rc == CBM_STORE_OK ? 0 : -1;
}

static int operations_workspace_public_state_equal(
    const operations_workspace_public_state_t *left,
    const operations_workspace_public_state_t *right) {
    return left->fts_count == right->fts_count && left->edge_count == right->edge_count &&
           left->vector_count == right->vector_count &&
           strcmp(left->fts_qualified_name, right->fts_qualified_name) == 0 &&
           strcmp(left->edge_type, right->edge_type) == 0 &&
           left->vector_node_id == right->vector_node_id &&
           left->vector_first_byte == right->vector_first_byte;
}

static int operations_workspace_state_capture(const char *path, const char *workspace_id,
                                              operations_workspace_state_t *out) {
    memset(out, 0, sizeof(*out));
    if (cbm_zova_repository_export_snapshot(path, workspace_id, &out->snapshot) != 0) return -1;
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, out->snapshot.project);
    int rc = repo && cbm_zova_repository_search_fts(repo, workspace_id, "root", NULL, 20, 0,
                                                    &out->fts) == CBM_STORE_OK ? 0 : -1;
    if (repo) cbm_zova_repository_close(repo);
    if (rc != 0) {
        cbm_store_search_free(&out->fts);
        cbm_zova_workspace_snapshot_free(&out->snapshot);
    }
    return rc;
}

static void operations_workspace_state_free(operations_workspace_state_t *state) {
    cbm_store_search_free(&state->fts);
    cbm_zova_workspace_snapshot_free(&state->snapshot);
}

static int operations_workspace_state_equal(const operations_workspace_state_t *a,
                                            const operations_workspace_state_t *b) {
    const cbm_zova_workspace_snapshot_t *x = &a->snapshot, *y = &b->snapshot;
    if (strcmp(x->workspace_id, y->workspace_id) != 0 || x->generation != y->generation ||
        x->node_count != y->node_count || x->edge_count != y->edge_count ||
        x->node_vector_count != y->node_vector_count ||
        x->token_vector_count != y->token_vector_count ||
        x->file_hash_count != y->file_hash_count ||
        strcmp(x->integrity.metadata_sha256, y->integrity.metadata_sha256) != 0 ||
        strcmp(x->integrity.fts_sha256, y->integrity.fts_sha256) != 0 ||
        strcmp(x->integrity.topology_sha256, y->integrity.topology_sha256) != 0 ||
        strcmp(x->integrity.node_vector_sha256, y->integrity.node_vector_sha256) != 0 ||
        strcmp(x->integrity.token_vector_sha256, y->integrity.token_vector_sha256) != 0 ||
        a->fts.count != b->fts.count || a->fts.total != b->fts.total) return 0;
    for (int i = 0; i < x->node_count; i++)
        if (x->nodes[i].id != y->nodes[i].id ||
            strcmp(x->nodes[i].qualified_name, y->nodes[i].qualified_name) != 0) return 0;
    for (int i = 0; i < x->node_vector_count; i++)
        if (x->node_vectors[i].node_id != y->node_vectors[i].node_id ||
            x->node_vectors[i].vector_len != y->node_vectors[i].vector_len ||
            memcmp(x->node_vectors[i].vector, y->node_vectors[i].vector,
                   (size_t)x->node_vectors[i].vector_len) != 0) return 0;
    for (int i = 0; i < x->token_vector_count; i++)
        if (strcmp(x->token_vectors[i].token, y->token_vectors[i].token) != 0 ||
            x->token_vectors[i].vector_len != y->token_vectors[i].vector_len ||
            memcmp(x->token_vectors[i].vector, y->token_vectors[i].vector,
                   (size_t)x->token_vectors[i].vector_len) != 0) return 0;
    for (int i = 0; i < a->fts.count; i++)
        if (strcmp(a->fts.results[i].node.qualified_name,
                   b->fts.results[i].node.qualified_name) != 0 ||
            a->fts.results[i].rank != b->fts.results[i].rank) return 0;
    return 1;
}

static int operations_prepare_rich_archive(
    const char *source, const char *archive, int generations, const char *summary,
    cbm_zova_workspace_generation_result_t *out) {
    operations_archive_cleanup(archive);
    for (int i = 0; i < generations; i++)
        if (operations_publish_rich_fixture_variant(source, summary, out) != 0) return -1;
    cbm_zova_operation_report_t report = {0};
    return cbm_zova_workspace_export(source, out->workspace_id, archive, &report) ==
                   CBM_ZOVA_OPERATION_OK ? 0 : -1;
}

static int operations_seed_collision_target(
    const char *target, int a_generations, const char *a_summary, const char *b_root,
    cbm_zova_workspace_generation_result_t *out_a,
    cbm_zova_workspace_generation_result_t *out_b) {
    for (int i = 0; i < a_generations; i++)
        if (operations_publish_rich_fixture_variant(target, a_summary, out_a) != 0) return -1;
    return operations_publish_fixture(target, b_root, "collision-b", 2, out_b);
}
#endif

static int operations_make_v4(const char *path) {
    return cbm_zova_user_database_init(path) == 0 &&
                   operations_sql(path,
                                  "DROP TABLE IF EXISTS cbm_workspace_health_v1;"
                                  "UPDATE cbm_database_schema_v1 SET schema_version=4 WHERE id=1") ==
                       0
               ? 0
               : -1;
}

static int operations_add_ready_workspace(const char *path) {
    return operations_sql(
        path,
        "INSERT INTO cbm_workspace_registry"
        "(workspace_id,canonical_root,id_format_version,active_generation)"
        "VALUES('w1_ops','/tmp/operations-ready',2,7);"
        "INSERT INTO cbm_database_generation_v1(workspace_key,generation,state)"
        "SELECT workspace_key,7,'ready' FROM cbm_workspace_registry "
        "WHERE workspace_id='w1_ops';"
        "INSERT INTO cbm_generation_integrity_v2("
        "workspace_key,generation,graph_nodes,graph_edges,metadata_nodes,metadata_edges,"
        "metadata_topology_edges,fts_rows,node_vector_rows,token_vector_rows,node_vectors,"
        "token_vectors,metadata_sha256,fts_sha256,topology_sha256,node_vector_sha256,"
        "token_vector_sha256) SELECT workspace_key,7,1,2,3,4,5,6,7,8,9,10,"
        "'metadata','fts','topology','node-vector','token-vector' "
        "FROM cbm_workspace_registry WHERE workspace_id='w1_ops'");
}

static int operations_ready_digest(const char *path, char *out, size_t out_size) {
    return operations_text(
        path,
        "SELECT metadata_sha256||'|'||fts_sha256||'|'||topology_sha256||'|'||"
        "node_vector_sha256||'|'||token_vector_sha256 FROM cbm_generation_integrity_v2 "
        "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
        "WHERE workspace_id='w1_ops') AND generation=7",
        out, out_size);
}

TEST(zova_operations_report_contract_and_code_names_are_stable) {
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(CBM_ZOVA_OPERATIONS_ARCHIVE_VERSION, 1);
    ASSERT_EQ(sizeof(report.operation), 32);
    ASSERT_EQ(sizeof(report.reason), 64);
    ASSERT_EQ(sizeof(report.workspace_id), CBM_ZOVA_WORKSPACE_ID_MAX);
    const struct { cbm_zova_operation_code_t code; const char *name; } cases[] = {
        {CBM_ZOVA_OPERATION_OK, "ok"},
        {CBM_ZOVA_OPERATION_NOOP, "noop"},
        {CBM_ZOVA_OPERATION_INVALID, "invalid"},
        {CBM_ZOVA_OPERATION_BUSY, "busy"},
        {CBM_ZOVA_OPERATION_INCOMPATIBLE, "incompatible"},
        {CBM_ZOVA_OPERATION_VERIFY_FAILED, "verify_failed"},
        {CBM_ZOVA_OPERATION_DISK_REFUSED, "disk_refused"},
        {CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED, "confirmation_required"},
        {CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED, "workspace_rebuild_required"},
        {CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED, "whole_file_recovery_required"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        ASSERT_STR_EQ(cbm_zova_operation_code_name(cases[i].code), cases[i].name);
    ASSERT_STR_EQ(cbm_zova_operation_code_name((cbm_zova_operation_code_t)99), "unknown");
    PASS();
}

#if CBM_WITH_ZOVA && !defined(_WIN32)
TEST(zova_operations_database_status_reports_exact_checked_disk_accounting) {
    char path[OPERATIONS_PATH_MAX], wal_path[OPERATIONS_PATH_MAX + 16];
    operations_path(path, sizeof(path), "database-status-accounting");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    sqlite3 *writer = NULL;
    ASSERT_EQ(sqlite3_open(path, &writer), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(writer,
                           "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
                           "CREATE TABLE status_filler(payload BLOB);"
                           "INSERT INTO status_filler VALUES(zeroblob(262144));"
                           "DROP TABLE status_filler;",
                           NULL, NULL, NULL), SQLITE_OK);
    int64_t page_size = operations_sqlite_scalar(writer, "PRAGMA page_size");
    int64_t page_count = operations_sqlite_scalar(writer, "PRAGMA page_count");
    int64_t freelist_count = operations_sqlite_scalar(writer, "PRAGMA freelist_count");
    ASSERT_EQ(page_size, 65536);
    ASSERT_GT(page_count, 0);
    ASSERT_GT(freelist_count, 0);
    struct stat database_stat = {0}, wal_stat = {0};
    snprintf(wal_path, sizeof(wal_path), "%s-wal", path);
    ASSERT_EQ(stat(path, &database_stat), 0);
    ASSERT_EQ(stat(wal_path, &wal_stat), 0);
    ASSERT_GT(wal_stat.st_size, 0);
    struct statvfs filesystem = {0};
    ASSERT_EQ(statvfs(path, &filesystem), 0);
    uint64_t free_bytes = (uint64_t)filesystem.f_bavail * (uint64_t)filesystem.f_frsize;

    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_status(path, &report), CBM_ZOVA_OPERATION_OK);
    ASSERT_STR_EQ(report.operation, "status");
    ASSERT_STR_EQ(report.reason, "ok");
    ASSERT_EQ(report.schema_version, CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    ASSERT_EQ(report.database_bytes, (uint64_t)database_stat.st_size);
    ASSERT_EQ(report.wal_bytes, (uint64_t)wal_stat.st_size);
    ASSERT_EQ(report.free_bytes, free_bytes);
    ASSERT_EQ(report.page_size, (uint64_t)page_size);
    ASSERT_EQ(report.page_count, (uint64_t)page_count);
    ASSERT_EQ(report.freelist_count, (uint64_t)freelist_count);
    ASSERT_EQ(report.reclaimable_bytes,
              (uint64_t)page_size * (uint64_t)freelist_count);

    sqlite3_close(writer);
    snprintf(wal_path, sizeof(wal_path), "%s-wal", path); cbm_unlink(wal_path);
    snprintf(wal_path, sizeof(wal_path), "%s-shm", path); cbm_unlink(wal_path);
    snprintf(wal_path, sizeof(wal_path), "%s.writer.lock", path); cbm_unlink(wal_path);
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_database_status_refuses_untrusted_inputs_without_writes) {
    char valid[OPERATIONS_PATH_MAX], alias[OPERATIONS_PATH_MAX + 16];
    char missing[OPERATIONS_PATH_MAX], future[OPERATIONS_PATH_MAX], malformed[OPERATIONS_PATH_MAX];
    operations_path(valid, sizeof(valid), "database-status-valid");
    operations_path(missing, sizeof(missing), "database-status-missing");
    operations_path(future, sizeof(future), "database-status-future");
    operations_path(malformed, sizeof(malformed), "database-status-malformed");
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_status(missing, &report), CBM_ZOVA_OPERATION_INVALID);
    struct stat missing_stat = {0};
    ASSERT_NEQ(lstat(missing, &missing_stat), 0);

    ASSERT_EQ(cbm_zova_user_database_init(valid), 0);
    snprintf(alias, sizeof(alias), "%s.alias", valid);
    cbm_unlink(alias);
    ASSERT_EQ(symlink(valid, alias), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(valid, before), 0);
    ASSERT_EQ(cbm_zova_database_status(alias, &report), CBM_ZOVA_OPERATION_INVALID);
    ASSERT_EQ(operations_file_digest(valid, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(unlink(alias), 0);

    ASSERT_EQ(cbm_zova_user_database_init(future), 0);
    ASSERT_EQ(operations_sql(future,
                             "UPDATE cbm_database_schema_v1 SET schema_version=8 WHERE id=1"), 0);
    ASSERT_EQ(operations_file_digest(future, before), 0);
    ASSERT_EQ(cbm_zova_database_status(future, &report), CBM_ZOVA_OPERATION_INCOMPATIBLE);
    ASSERT_EQ(operations_file_digest(future, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);

    ASSERT_EQ(cbm_zova_user_database_init(malformed), 0);
    ASSERT_EQ(operations_sql(malformed, "DROP TABLE cbm_workspace_health_v1"), 0);
    ASSERT_EQ(operations_file_digest(malformed, before), 0);
    ASSERT_EQ(cbm_zova_database_status(malformed, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    ASSERT_EQ(operations_file_digest(malformed, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);

    cbm_unlink(malformed);
    cbm_unlink(future);
    cbm_unlink(valid);
    PASS();
}

TEST(zova_operations_delete_primitive_removes_health_before_registry) {
    char path[OPERATIONS_PATH_MAX], sql[512];
    operations_path(path, sizeof(path), "delete-primitive-health");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    snprintf(sql, sizeof(sql),
             "INSERT INTO cbm_workspace_health_v1"
             "(workspace_key,state,reason,checked_generation,checked_at) "
             "SELECT workspace_key,'healthy','checked',1,'2026-07-14T00:00:00Z' "
             "FROM cbm_workspace_registry WHERE workspace_id='%s'",
             rich.workspace_id);
    ASSERT_EQ(operations_sql(path, sql), 0);

    ASSERT_EQ(cbm_zova_user_database_delete_workspace(path, rich.workspace_id), 0);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_workspace_health_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             rich.workspace_id);
    ASSERT_EQ(operations_scalar(path, sql), 0);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_workspace_registry WHERE workspace_id='%s'",
             rich.workspace_id);
    ASSERT_EQ(operations_scalar(path, sql), 0);

    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_workspace_delete_requires_exact_confirmation_without_writes) {
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "workspace-delete-confirmation");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(path, before), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_delete(path, rich.workspace_id, NULL, &report),
              CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED);
    ASSERT_STR_EQ(report.operation, "delete_workspace");
    ASSERT_STR_EQ(report.reason, "confirmation_required");
    ASSERT_EQ(operations_file_digest(path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(cbm_zova_workspace_delete(path, rich.workspace_id, "wrong_workspace", &report),
              CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED);
    ASSERT_STR_EQ(report.reason, "confirmation_mismatch");
    ASSERT_EQ(operations_file_digest(path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    cbm_unlink(path);
    PASS();
}

/* ADR-0001: a workspace mutation refuses while that workspace's index lease is
 * held, and a database replacement refuses while an index run holds the
 * preparation barrier. Neither touches the file when it refuses. */
TEST(zova_operations_refuse_while_index_coordination_is_held) {
    const char *saved_wait = getenv("CBM_INDEX_LEASE_TIMEOUT_S");
    char *saved_wait_copy = saved_wait ? strdup(saved_wait) : NULL;
    cbm_setenv("CBM_INDEX_LEASE_TIMEOUT_S", "0", 1);
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "coordination-refusal");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(path, before), 0);

    cbm_zova_coord_hold_t barrier = {0}, lease = {0};
    ASSERT_EQ(cbm_zova_prepare_barrier_acquire(path, false, 0, &barrier), CBM_ZOVA_COORD_OK);
    ASSERT_EQ(cbm_zova_workspace_lease_acquire(path, rich.workspace_id, 0, &lease),
              CBM_ZOVA_COORD_OK);

    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_delete(path, rich.workspace_id, rich.workspace_id, &report),
              CBM_ZOVA_OPERATION_BUSY);
    ASSERT_STR_EQ(report.operation, "delete_workspace");
    ASSERT_STR_EQ(report.reason, "workspace_busy");
    ASSERT_EQ(cbm_zova_workspace_recover(path, rich.workspace_id, "/tmp/unused", &report),
              CBM_ZOVA_OPERATION_BUSY);
    ASSERT_STR_EQ(report.reason, "workspace_busy");
    ASSERT_EQ(cbm_zova_database_restore(path, "/tmp/operations-coordination-backup", true,
                                        &report),
              CBM_ZOVA_OPERATION_BUSY);
    ASSERT_STR_EQ(report.operation, "restore");
    ASSERT_STR_EQ(report.reason, "indexing_in_progress");
    cbm_zova_repack_request_t repack = {.live_path = path};
    cbm_zova_repack_report_t repack_report;
    ASSERT_EQ(cbm_zova_database_repack(&repack, &repack_report), CBM_ZOVA_OPERATION_BUSY);
    ASSERT_EQ(repack_report.code, CBM_ZOVA_OPERATION_BUSY);
    ASSERT_EQ(operations_file_digest(path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);

    /* Lease released, barrier still shared: delete proceeds, restore still waits. */
    cbm_zova_coord_release(&lease);
    ASSERT_EQ(cbm_zova_database_restore(path, "/tmp/operations-coordination-backup", true,
                                        &report),
              CBM_ZOVA_OPERATION_BUSY);
    ASSERT_EQ(cbm_zova_workspace_delete(path, rich.workspace_id, rich.workspace_id, &report),
              CBM_ZOVA_OPERATION_OK);
    cbm_zova_coord_release(&barrier);
    ASSERT_EQ(cbm_zova_database_restore(path, "/tmp/operations-coordination-backup", true,
                                        &report),
              CBM_ZOVA_OPERATION_INVALID);

    char lock_path[OPERATIONS_PATH_MAX + 128];
    snprintf(lock_path, sizeof(lock_path), "%s.prepare.lock", path);
    cbm_unlink(lock_path);
    snprintf(lock_path, sizeof(lock_path), "%s.prepare.turnstile.lock", path);
    cbm_unlink(lock_path);
    snprintf(lock_path, sizeof(lock_path), "%s.ws-%s.lock", path, rich.workspace_id);
    cbm_unlink(lock_path);
    cbm_unlink(path);
    if (saved_wait_copy) cbm_setenv("CBM_INDEX_LEASE_TIMEOUT_S", saved_wait_copy, 1);
    else cbm_unsetenv("CBM_INDEX_LEASE_TIMEOUT_S");
    free(saved_wait_copy);
    PASS();
}

TEST(zova_operations_workspace_delete_is_isolated_reported_and_idempotent) {
    char path[OPERATIONS_PATH_MAX], sql[2048];
    operations_path(path, sizeof(path), "workspace-delete-isolation");
    cbm_zova_workspace_generation_result_t rich_a = {0}, rich_b = {0};
    ASSERT_EQ(operations_publish_rich_fixture_named(
                  path, "/tmp/workspace-delete-rich-a", "workspace-delete-rich-a",
                  "{\"summary\":\"delete-a\"}", &rich_a), 0);
    ASSERT_EQ(operations_publish_rich_fixture_named(
                  path, "/tmp/workspace-delete-rich-b", "workspace-delete-rich-b",
                  "{\"summary\":\"keep-b\"}", &rich_b), 0);
    snprintf(sql, sizeof(sql),
             "INSERT INTO cbm_workspace_health_v1"
             "(workspace_key,state,reason,checked_generation,checked_at) "
             "SELECT workspace_key,'healthy','checked',1,'2026-07-14T00:00:00Z' "
             "FROM cbm_workspace_registry WHERE workspace_id='%s'",
             rich_a.workspace_id);
    ASSERT_EQ(operations_sql(path, sql), 0);

    operations_workspace_state_t before_b = {0}, after_b = {0}, repeated_b = {0};
    operations_workspace_public_state_t before_public_b = {0}, after_public_b = {0};
    ASSERT_EQ(operations_workspace_state_capture(path, rich_b.workspace_id, &before_b), 0);
    ASSERT_EQ(operations_workspace_public_state_capture(
                  path, rich_b.workspace_id, "workspace-delete-rich-b", &before_public_b), 0);
    cbm_zova_operation_report_t before = {0}, report = {0}, after = {0};
    ASSERT_EQ(cbm_zova_database_status(path, &before), CBM_ZOVA_OPERATION_OK);

    ASSERT_EQ(cbm_zova_workspace_delete(path, rich_a.workspace_id, rich_a.workspace_id, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT_STR_EQ(report.operation, "delete_workspace");
    ASSERT_STR_EQ(report.reason, "ok");
    ASSERT_STR_EQ(report.workspace_id, rich_a.workspace_id);
    ASSERT_EQ(cbm_zova_database_status(path, &after), CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(report.reclaimable_bytes,
              after.reclaimable_bytes >= before.reclaimable_bytes
                  ? after.reclaimable_bytes - before.reclaimable_bytes : 0);
    ASSERT_EQ(after.page_count, before.page_count);
    ASSERT_EQ(after.database_bytes, before.database_bytes);

    snprintf(sql, sizeof(sql),
             "SELECT (SELECT count(*) FROM cbm_workspace_registry WHERE workspace_id='%s')+"
             "(SELECT count(*) FROM pragma_foreign_key_check)", rich_a.workspace_id);
    ASSERT_EQ(operations_scalar(path, sql), 0);
    char graph_name[160], node_collection[256], token_collection[256];
    ASSERT_EQ(cbm_zova_workspace_graph_name(rich_a.workspace_id, graph_name,
                                            sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  rich_a.workspace_id, "rich_model_v2", 3, node_collection,
                  sizeof(node_collection)), 0);
    ASSERT_EQ(cbm_zova_workspace_token_vector_collection_name(
                  rich_a.workspace_id, "rich_model_v2", 3, token_collection,
                  sizeof(token_collection)), 0);
    snprintf(sql, sizeof(sql),
             "SELECT (SELECT count(*) FROM _zova_graphs WHERE name='%s')+"
             "(SELECT count(*) FROM _zova_vector_collections WHERE name IN ('%s','%s'))",
             graph_name, node_collection, token_collection);
    ASSERT_EQ(operations_scalar(path, sql), 0);
    ASSERT_NULL(cbm_zova_repository_open(path, "workspace-delete-rich-a"));

    ASSERT_EQ(operations_workspace_state_capture(path, rich_b.workspace_id, &after_b), 0);
    ASSERT(operations_workspace_state_equal(&before_b, &after_b));
    ASSERT_EQ(operations_workspace_public_state_capture(
                  path, rich_b.workspace_id, "workspace-delete-rich-b", &after_public_b), 0);
    ASSERT(operations_workspace_public_state_equal(&before_public_b, &after_public_b));

    ASSERT_EQ(cbm_zova_workspace_delete(path, rich_a.workspace_id, rich_a.workspace_id, &report),
              CBM_ZOVA_OPERATION_NOOP);
    ASSERT_STR_EQ(report.reason, "already_absent");
    ASSERT_EQ(report.reclaimable_bytes, 0);
    ASSERT_EQ(operations_workspace_state_capture(path, rich_b.workspace_id, &repeated_b), 0);
    ASSERT(operations_workspace_state_equal(&before_b, &repeated_b));

    operations_workspace_state_free(&repeated_b);
    operations_workspace_state_free(&after_b);
    operations_workspace_state_free(&before_b);
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_database_compact_noops_below_policy_threshold) {
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "database-compact-noop");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    cbm_zova_operation_report_t before = {0}, report = {0}, after = {0};
    ASSERT_EQ(cbm_zova_database_status(path, &before), CBM_ZOVA_OPERATION_OK);
    ASSERT_TRUE(before.reclaimable_bytes < 64ULL * 1024ULL * 1024ULL);
    ASSERT_TRUE(before.database_bytes == 0 ||
                before.reclaimable_bytes < before.database_bytes / 10ULL);
    uint8_t digest_before[CBM_SHA256_DIGEST_LEN], digest_after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(path, digest_before), 0);

    ASSERT_EQ(cbm_zova_database_compact(path, &report), CBM_ZOVA_OPERATION_NOOP);
    ASSERT_STR_EQ(report.operation, "compact");
    ASSERT_STR_EQ(report.reason, "below_threshold");
    ASSERT_EQ(operations_file_digest(path, digest_after), 0);
    ASSERT_EQ(memcmp(digest_before, digest_after, sizeof(digest_before)), 0);
    ASSERT_EQ(cbm_zova_database_status(path, &after), CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(after.database_bytes, before.database_bytes);
    ASSERT_EQ(after.page_count, before.page_count);

    cbm_unlink(path);
    PASS();
}


/* Each tests/fixtures/zova-format-<n>/cbm.zova is a one-workspace database
 * with CBM schema 7 written by an older release:
 *   format 9:  release v1.0.1 (Zova 0.24.0), 18 nodes;
 *   format 12: CBM built against Zova 1.3.0, 23 nodes.
 * The tests migrate the fixture in place, so they work on a copy. */
static int operations_copy_format_fixture(const char *fixture, const char *destination) {
    const char *directory = getenv("CBM_TEST_FIXTURE_DIR");
    char source[OPERATIONS_PATH_MAX];
    snprintf(source, sizeof(source), "%s/%s/cbm.zova",
             directory && directory[0] ? directory : "tests/fixtures", fixture);
    FILE *in = fopen(source, "rb");
    if (!in) return -1;
    FILE *out = fopen(destination, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[64 * 1024];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}

static int operations_migrate_format_fixture(const char *fixture, int expected_nodes) {
    char path[OPERATIONS_PATH_MAX], leftover[OPERATIONS_PATH_MAX + 64];
    operations_path(path, sizeof(path), fixture);
    ASSERT_EQ(operations_copy_format_fixture(fixture, path), 0);
    ASSERT_EQ(cbm_zova_database_format_status(path), CBM_ZOVA_DATABASE_MIGRATION_REQUIRED);

    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_status(path, &report) == CBM_ZOVA_OPERATION_OK, 0);
    ASSERT_EQ(cbm_zova_database_migrate_format(path, &report), CBM_ZOVA_OPERATION_OK);
    ASSERT_STR_EQ(report.operation, "migrate-format");
    ASSERT_STR_EQ(report.reason, "ok");
    ASSERT_EQ(cbm_zova_database_format_status(path), CBM_ZOVA_DATABASE_COMPATIBLE);
    ASSERT_EQ(cbm_zova_database_status(path, &report), CBM_ZOVA_OPERATION_OK);

    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
                                 "SELECT (SELECT count(*) FROM cbm_workspace_registry),"
                                 "(SELECT count(*) FROM cbm_nodes_v1)",
                                 -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    ASSERT_EQ(sqlite3_column_int(stmt, 1), expected_nodes);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    static const char *const suffixes[] = {".format.tmp.zova", ".format.recovery.zova",
                                           ".format.tmp.zova.migration-recovery"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        struct stat ignored;
        snprintf(leftover, sizeof(leftover), "%.*s%s", (int)(strlen(path) - 5), path, suffixes[i]);
        ASSERT_TRUE(stat(leftover, &ignored) != 0);
    }

    ASSERT_EQ(cbm_zova_database_migrate_format(path, &report), CBM_ZOVA_OPERATION_NOOP);
    ASSERT_STR_EQ(report.reason, "current_format");
    cbm_unlink(path);
    return 0;
}

TEST(zova_operations_migrate_format_upgrades_format9_database) {
    ASSERT_EQ(operations_migrate_format_fixture("zova-format-9", 18), 0);
    PASS();
}

TEST(zova_operations_migrate_format_upgrades_format12_database) {
    ASSERT_EQ(operations_migrate_format_fixture("zova-format-12", 23), 0);
    PASS();
}

static cbm_zova_operation_code_t operations_compact_with_test_space(
    const char *path, cbm_zova_operation_report_t *report);

TEST(zova_operations_database_compact_reclaims_verified_database) {
    char path[OPERATIONS_PATH_MAX], sql[1024];
    operations_path(path, sizeof(path), "database-compact-success");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    operations_workspace_state_t workspace_before = {0}, workspace_after = {0};
    ASSERT_EQ(operations_workspace_state_capture(path, rich.workspace_id, &workspace_before), 0);
    snprintf(sql, sizeof(sql),
             "INSERT INTO cbm_workspace_health_v1"
             "(workspace_key,state,reason,checked_generation,checked_at) "
             "SELECT workspace_key,'healthy',hex(zeroblob(8388608)),%lld,"
             "'2026-07-14T00:00:00Z' FROM cbm_workspace_registry WHERE workspace_id='%s';"
             "DELETE FROM cbm_workspace_health_v1 WHERE workspace_key=(SELECT workspace_key "
             "FROM cbm_workspace_registry WHERE workspace_id='%s')",
             (long long)rich.generation, rich.workspace_id, rich.workspace_id);
    ASSERT_EQ(operations_sql(path, sql), 0);
    cbm_zova_operation_report_t before = {0}, report = {0}, after = {0};
    ASSERT_EQ(cbm_zova_database_status(path, &before), CBM_ZOVA_OPERATION_OK);
    ASSERT_TRUE(before.reclaimable_bytes >= 64ULL * 1024ULL * 1024ULL ||
                (before.database_bytes > 0 &&
                 before.reclaimable_bytes >= before.database_bytes / 10ULL));

    ASSERT_EQ(operations_compact_with_test_space(path, &report), CBM_ZOVA_OPERATION_OK);
    ASSERT_STR_EQ(report.operation, "compact");
    ASSERT_STR_EQ(report.reason, "ok");
    ASSERT_EQ(cbm_zova_database_status(path, &after), CBM_ZOVA_OPERATION_OK);
    ASSERT_LT(after.database_bytes, before.database_bytes);
    ASSERT_LT(after.page_count, before.page_count);
    ASSERT_LT(after.reclaimable_bytes, before.reclaimable_bytes);
    ASSERT_EQ(report.database_bytes, after.database_bytes);
    ASSERT_EQ(operations_workspace_state_capture(path, rich.workspace_id, &workspace_after), 0);
    ASSERT_TRUE(operations_workspace_state_equal(&workspace_before, &workspace_after));

    operations_workspace_state_free(&workspace_after);
    operations_workspace_state_free(&workspace_before);
    cbm_unlink(path);
    PASS();
}

static int operations_make_database_compactable(
    const char *path, const cbm_zova_workspace_generation_result_t *workspace) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO cbm_workspace_health_v1"
             "(workspace_key,state,reason,checked_generation,checked_at) "
             "SELECT workspace_key,'healthy',hex(zeroblob(8388608)),%lld,"
             "'2026-07-14T00:00:00Z' FROM cbm_workspace_registry WHERE workspace_id='%s';"
             "DELETE FROM cbm_workspace_health_v1 WHERE workspace_key=(SELECT workspace_key "
             "FROM cbm_workspace_registry WHERE workspace_id='%s')",
             (long long)workspace->generation, workspace->workspace_id,
             workspace->workspace_id);
    if (operations_sql(path, sql) != 0) return -1;
    cbm_zova_operation_report_t status = {0};
    if (cbm_zova_database_status(path, &status) != CBM_ZOVA_OPERATION_OK) return -1;
    return status.reclaimable_bytes >= 64ULL * 1024ULL * 1024ULL ||
                   (status.database_bytes > 0 &&
                    status.reclaimable_bytes >= status.database_bytes / 10ULL)
               ? 0 : -1;
}

static cbm_zova_operation_code_t operations_compact_with_test_space(
    const char *path, cbm_zova_operation_report_t *report) {
    const char *saved = getenv("CBM_ZOVA_TEST_COMPACT_FREE_BYTES");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_ZOVA_TEST_COMPACT_FREE_BYTES", "17179869184", 1);
    cbm_zova_operation_code_t code = cbm_zova_database_compact(path, report);
    if (saved_copy)
        cbm_setenv("CBM_ZOVA_TEST_COMPACT_FREE_BYTES", saved_copy, 1);
    else
        cbm_unsetenv("CBM_ZOVA_TEST_COMPACT_FREE_BYTES");
    free(saved_copy);
    return code;
}

TEST(zova_operations_database_compact_preserves_two_workspace_public_state) {
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "database-compact-two-workspaces");
    cbm_zova_workspace_generation_result_t rich_a = {0}, rich_b = {0};
    ASSERT_EQ(operations_publish_rich_fixture_named(
                  path, "/tmp/database-compact-rich-a", "database-compact-rich-a",
                  "{\"summary\":\"compact-a\"}", &rich_a), 0);
    ASSERT_EQ(operations_publish_rich_fixture_named(
                  path, "/tmp/database-compact-rich-b", "database-compact-rich-b",
                  "{\"summary\":\"compact-b\"}", &rich_b), 0);
    operations_workspace_state_t before_a = {0}, before_b = {0};
    operations_workspace_state_t after_a = {0}, after_b = {0};
    operations_workspace_public_state_t public_before_a = {0}, public_before_b = {0};
    operations_workspace_public_state_t public_after_a = {0}, public_after_b = {0};
    ASSERT_EQ(operations_workspace_state_capture(path, rich_a.workspace_id, &before_a), 0);
    ASSERT_EQ(operations_workspace_state_capture(path, rich_b.workspace_id, &before_b), 0);
    ASSERT_EQ(operations_workspace_public_state_capture(
                  path, rich_a.workspace_id, "database-compact-rich-a", &public_before_a), 0);
    ASSERT_EQ(operations_workspace_public_state_capture(
                  path, rich_b.workspace_id, "database-compact-rich-b", &public_before_b), 0);
    ASSERT_EQ(operations_make_database_compactable(path, &rich_a), 0);

    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(operations_compact_with_test_space(path, &report), CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(operations_workspace_state_capture(path, rich_a.workspace_id, &after_a), 0);
    ASSERT_EQ(operations_workspace_state_capture(path, rich_b.workspace_id, &after_b), 0);
    ASSERT_TRUE(operations_workspace_state_equal(&before_a, &after_a));
    ASSERT_TRUE(operations_workspace_state_equal(&before_b, &after_b));
    ASSERT_EQ(operations_workspace_public_state_capture(
                  path, rich_a.workspace_id, "database-compact-rich-a", &public_after_a), 0);
    ASSERT_EQ(operations_workspace_public_state_capture(
                  path, rich_b.workspace_id, "database-compact-rich-b", &public_after_b), 0);
    ASSERT_TRUE(operations_workspace_public_state_equal(&public_before_a, &public_after_a));
    ASSERT_TRUE(operations_workspace_public_state_equal(&public_before_b, &public_after_b));

    operations_workspace_state_free(&after_b);
    operations_workspace_state_free(&after_a);
    operations_workspace_state_free(&before_b);
    operations_workspace_state_free(&before_a);
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_database_compact_refuses_disk_and_active_writer_without_writes) {
    char disk_path[OPERATIONS_PATH_MAX], writer_path[OPERATIONS_PATH_MAX];
    operations_path(disk_path, sizeof(disk_path), "database-compact-disk");
    operations_path(writer_path, sizeof(writer_path), "database-compact-writer");
    cbm_zova_workspace_generation_result_t disk_workspace = {0}, writer_workspace = {0};
    const char *saved_free_bytes = getenv("CBM_ZOVA_TEST_COMPACT_FREE_BYTES");
    char *saved_free_bytes_copy = saved_free_bytes ? strdup(saved_free_bytes) : NULL;
    ASSERT_EQ(operations_publish_rich_fixture(disk_path, &disk_workspace), 0);
    ASSERT_EQ(operations_make_database_compactable(disk_path, &disk_workspace), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(disk_path, before), 0);
    cbm_setenv("CBM_ZOVA_TEST_COMPACT_FREE_BYTES", "8589934591", 1);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_compact(disk_path, &report),
              CBM_ZOVA_OPERATION_DISK_REFUSED);
    if (saved_free_bytes_copy)
        cbm_setenv("CBM_ZOVA_TEST_COMPACT_FREE_BYTES", saved_free_bytes_copy, 1);
    else
        cbm_unsetenv("CBM_ZOVA_TEST_COMPACT_FREE_BYTES");
    free(saved_free_bytes_copy);
    ASSERT_STR_EQ(report.reason, "insufficient_disk");
    ASSERT_EQ(operations_file_digest(disk_path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);

    ASSERT_EQ(operations_publish_rich_fixture(writer_path, &writer_workspace), 0);
    ASSERT_EQ(operations_make_database_compactable(writer_path, &writer_workspace), 0);
    ASSERT_EQ(operations_file_digest(writer_path, before), 0);
    cbm_zova_writer_guard_t guard = {0};
    ASSERT_EQ(cbm_zova_writer_guard_acquire(writer_path, &guard), 0);
    ASSERT_EQ(cbm_zova_database_compact(writer_path, &report), CBM_ZOVA_OPERATION_BUSY);
    ASSERT_STR_EQ(report.reason, "writer_busy");
    cbm_zova_writer_guard_release(&guard);
    ASSERT_EQ(operations_file_digest(writer_path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);

    cbm_unlink(writer_path);
    cbm_unlink(disk_path);
    PASS();
}

TEST(zova_operations_database_compact_refuses_checkpoint_blocked_reader_then_retries) {
    char path[OPERATIONS_PATH_MAX], sql[1024];
    operations_path(path, sizeof(path), "database-compact-reader-blocked");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    snprintf(sql, sizeof(sql),
             "INSERT INTO cbm_workspace_health_v1"
             "(workspace_key,state,reason,checked_generation,checked_at) "
             "SELECT workspace_key,'healthy',hex(zeroblob(8388608)),%lld,"
             "'2026-07-14T00:00:00Z' FROM cbm_workspace_registry WHERE workspace_id='%s'",
             (long long)rich.generation, rich.workspace_id);
    ASSERT_EQ(operations_sql(path, sql), 0);
    sqlite3 *reader = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &reader, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(reader, "BEGIN; SELECT reason FROM cbm_workspace_health_v1",
                           NULL, NULL, NULL), SQLITE_OK);
    snprintf(sql, sizeof(sql),
             "DELETE FROM cbm_workspace_health_v1 WHERE workspace_key=(SELECT workspace_key "
             "FROM cbm_workspace_registry WHERE workspace_id='%s')", rich.workspace_id);
    ASSERT_EQ(operations_sql(path, sql), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(operations_compact_with_test_space(path, &report), CBM_ZOVA_OPERATION_BUSY);
    ASSERT_STR_EQ(report.reason, "checkpoint_or_compact_busy");
    ASSERT_EQ(sqlite3_exec(reader, "ROLLBACK", NULL, NULL, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(reader), SQLITE_OK);
    ASSERT_EQ(operations_compact_with_test_space(path, &report), CBM_ZOVA_OPERATION_OK);

    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_database_compact_fault_phases_retry_to_verified_live_database) {
    static const char *const phases[] = {
        "compact_after_temp_creation",
        "compact_after_temp_verification",
        "compact_after_live_to_recovery",
        "compact_after_temp_to_live",
        "compact_before_recovery_cleanup",
    };
    for (size_t phase = 0; phase < sizeof(phases) / sizeof(phases[0]); phase++) {
        char path[OPERATIONS_PATH_MAX], temporary[OPERATIONS_PATH_MAX];
        char recovery[OPERATIONS_PATH_MAX];
        char label[64];
        snprintf(label, sizeof(label), "database-compact-fault-%zu", phase);
        operations_path(path, sizeof(path), label);
        size_t path_len = strlen(path);
        ASSERT_TRUE(path_len > 5);
        snprintf(temporary, sizeof(temporary), "%.*s.compact.tmp.zova",
                 (int)(path_len - 5), path);
        snprintf(recovery, sizeof(recovery), "%.*s.compact.recovery.zova",
                 (int)(path_len - 5), path);
        cbm_zova_workspace_generation_result_t rich = {0};
        ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
        ASSERT_EQ(operations_make_database_compactable(path, &rich), 0);
        operations_workspace_state_t before = {0}, after = {0};
        ASSERT_EQ(operations_workspace_state_capture(path, rich.workspace_id, &before), 0);
        cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", phases[phase], 1);
        cbm_zova_operation_report_t report = {0};
        ASSERT_EQ(operations_compact_with_test_space(path, &report),
                  CBM_ZOVA_OPERATION_VERIFY_FAILED);
        cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
        ASSERT_TRUE(access(path, F_OK) == 0 || access(recovery, F_OK) == 0);
        ASSERT_TRUE(access(path, F_OK) == 0 || access(temporary, F_OK) == 0);
        ASSERT_EQ(operations_compact_with_test_space(path, &report),
                  CBM_ZOVA_OPERATION_OK);
        ASSERT_EQ(operations_workspace_state_capture(path, rich.workspace_id, &after), 0);
        ASSERT_TRUE(operations_workspace_state_equal(&before, &after));
        ASSERT_TRUE(access(temporary, F_OK) != 0);
        ASSERT_TRUE(access(recovery, F_OK) != 0);
        operations_workspace_state_free(&after);
        operations_workspace_state_free(&before);
        cbm_unlink(path);
    }
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    PASS();
}

TEST(zova_operations_database_compact_keeps_open_committed_reader_valid) {
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "database-compact-open-reader");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    ASSERT_EQ(operations_make_database_compactable(path, &rich), 0);
    operations_workspace_state_t state_before = {0}, state_after = {0};
    ASSERT_EQ(operations_workspace_state_capture(path, rich.workspace_id, &state_before), 0);
    sqlite3 *reader = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &reader, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_NOT_NULL(reader);
    int64_t before_count = operations_sqlite_scalar(
        reader, "SELECT count(*) FROM cbm_workspace_registry");
    ASSERT_GT(before_count, 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(operations_compact_with_test_space(path, &report), CBM_ZOVA_OPERATION_OK);
    int64_t after_count = operations_sqlite_scalar(
        reader, "SELECT count(*) FROM cbm_workspace_registry");
    ASSERT_EQ(after_count, before_count);
    ASSERT_EQ(sqlite3_close(reader), SQLITE_OK);
    ASSERT_EQ(operations_workspace_state_capture(path, rich.workspace_id, &state_after), 0);
    ASSERT_TRUE(operations_workspace_state_equal(&state_before, &state_after));

    operations_workspace_state_free(&state_after);
    operations_workspace_state_free(&state_before);
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_health_classifies_workspace_defect_without_cross_workspace_damage) {
    char path[OPERATIONS_PATH_MAX], sql[512];
    operations_path(path, sizeof(path), "health-workspace-defect");
    cbm_zova_workspace_generation_result_t rich_a = {0}, rich_b = {0};
    ASSERT_EQ(operations_publish_rich_fixture_named(
                  path, "/tmp/health-rich-a", "health-rich-a",
                  "{\"summary\":\"health-a\"}", &rich_a), 0);
    ASSERT_EQ(operations_publish_rich_fixture_named(
                  path, "/tmp/health-rich-b", "health-rich-b",
                  "{\"summary\":\"health-b\"}", &rich_b), 0);
    operations_workspace_state_t before_b = {0}, after_b = {0};
    ASSERT_EQ(operations_workspace_state_capture(path, rich_b.workspace_id, &before_b), 0);
    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY;
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_health(path, rich_a.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_OK);

    ASSERT_EQ(cbm_zova_delete_workspace_graph(path, rich_a.workspace_id), 0);
    ASSERT_EQ(cbm_zova_database_health(path, rich_a.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_WORKSPACE_REBUILD);
    ASSERT_STR_EQ(report.reason, "workspace_public_state_invalid");
    ASSERT_EQ(cbm_zova_database_health(path, NULL, &health, &report),
              CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_WORKSPACE_REBUILD);
    ASSERT_STR_EQ(report.workspace_id, rich_a.workspace_id);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_workspace_health_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s') "
             "AND state='rebuild_required' AND checked_generation=%lld",
             rich_a.workspace_id, (long long)rich_a.generation);
    ASSERT_EQ(operations_scalar(path, sql), 1);
    ASSERT_NULL(cbm_zova_repository_open(path, "health-rich-a"));
    cbm_zova_repository_t *repo_b = cbm_zova_repository_open(path, "health-rich-b");
    ASSERT_NOT_NULL(repo_b);
    cbm_zova_repository_close(repo_b);
    ASSERT_EQ(cbm_zova_database_health(path, rich_b.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_OK);
    ASSERT_EQ(operations_workspace_state_capture(path, rich_b.workspace_id, &after_b), 0);
    ASSERT_TRUE(operations_workspace_state_equal(&before_b, &after_b));

    operations_workspace_state_free(&after_b);
    operations_workspace_state_free(&before_b);
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_health_classifies_shared_schema_and_truncated_file_as_whole_file) {
    char schema_path[OPERATIONS_PATH_MAX], truncated_path[OPERATIONS_PATH_MAX];
    operations_path(schema_path, sizeof(schema_path), "health-shared-schema");
    operations_path(truncated_path, sizeof(truncated_path), "health-truncated");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(schema_path, &rich), 0);
    ASSERT_EQ(operations_sql(schema_path, "DROP TABLE cbm_workspace_health_v1"), 0);
    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_OK;
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_health(schema_path, rich.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY);
    ASSERT_STR_EQ(report.reason, "shared_database_invalid");
    ASSERT_NULL(cbm_zova_repository_open(schema_path, "workspace-rich"));

    FILE *file = fopen(truncated_path, "wb");
    ASSERT_NOT_NULL(file);
    ASSERT_EQ(fwrite("not-a-zova", 1, 10, file), 10);
    ASSERT_EQ(fclose(file), 0);
    ASSERT_EQ(cbm_zova_database_health(truncated_path, NULL, &health, &report),
              CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY);
    ASSERT_STR_EQ(report.reason, "shared_database_invalid");

    cbm_unlink(truncated_path);
    cbm_unlink(schema_path);
    PASS();
}

TEST(zova_operations_health_classifies_failed_quick_check_as_whole_file) {
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "health-quick-check");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);

    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT (SELECT page_size FROM pragma_page_size),rootpage "
                  "FROM sqlite_master WHERE type='index' AND rootpage>1 ORDER BY rootpage "
                  "DESC LIMIT 1",
                  -1, &statement, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    int64_t page_size = sqlite3_column_int64(statement, 0);
    int64_t root_page = sqlite3_column_int64(statement, 1);
    ASSERT_GT(page_size, 0);
    ASSERT_GT(root_page, 1);
    ASSERT_EQ(sqlite3_finalize(statement), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(db), SQLITE_OK);

    FILE *file = fopen(path, "r+b");
    ASSERT_NOT_NULL(file);
    ASSERT_EQ(fseek(file, (long)((root_page - 1) * page_size), SEEK_SET), 0);
    const unsigned char invalid_page_header[16] = {0};
    ASSERT_EQ(fwrite(invalid_page_header, 1, sizeof(invalid_page_header), file),
              sizeof(invalid_page_header));
    ASSERT_EQ(fflush(file), 0);
    ASSERT_EQ(fclose(file), 0);

    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_OK;
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_health(path, rich.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY);
    ASSERT_STR_EQ(report.reason, "shared_database_invalid");

    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_health_classifies_foreign_key_violation_as_whole_file) {
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "health-foreign-key");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    ASSERT_EQ(operations_sql(
                  path,
                  "PRAGMA foreign_keys=OFF;"
                  "INSERT INTO cbm_workspace_health_v1"
                  "(workspace_key,state,reason,checked_generation,checked_at)"
                  "VALUES(9223372036854770000,'healthy','test',1,"
                  "'2026-07-14T00:00:00Z')"),
              0);

    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_OK;
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_health(path, rich.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY);
    ASSERT_STR_EQ(report.reason, "foreign_key_violation");

    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_health_marks_workspace_when_native_vector_collection_is_missing) {
    char path[OPERATIONS_PATH_MAX], collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    operations_path(path, sizeof(path), "health-vector-missing");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  rich.workspace_id, "rich_model_v2", 3, collection, sizeof(collection)),
              0);

    zova_database *db = NULL;
    zova_message error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path = path, .out_db = &db, .out_error_message = &error}),
              ZOVA_OK);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zova_vector_collection_delete(&(zova_vector_collection_delete_request){
                  .db = db, .name = collection}),
              ZOVA_OK);
    ASSERT_EQ(zova_database_close(db), ZOVA_OK);
    zova_message_free(&error);

    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_OK;
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_health(path, rich.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_WORKSPACE_REBUILD);
    ASSERT_STR_EQ(report.reason, "workspace_public_state_invalid");
    ASSERT_NULL(cbm_zova_repository_open(path, "workspace-rich"));

    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_health_detects_workspace_digest_mismatch) {
    char path[OPERATIONS_PATH_MAX], sql[768];
    operations_path(path, sizeof(path), "health-digest-mismatch");
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    snprintf(sql, sizeof(sql),
             "UPDATE cbm_nodes_v1 SET properties='{\"tampered\":true}' "
             "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
             "WHERE workspace_id='%s') AND qualified_name='rich.parse::alpha/beta?'",
             rich.workspace_id);
    ASSERT_EQ(operations_sql(path, sql), 0);
    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_OK;
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_health(path, rich.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_WORKSPACE_REBUILD);
    ASSERT_STR_EQ(report.reason, "workspace_digest_mismatch");
    ASSERT_NULL(cbm_zova_repository_open(path, "workspace-rich"));

    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_rebuild_health_clears_only_with_successful_publication) {
    char path[OPERATIONS_PATH_MAX], sql[512];
    operations_path(path, sizeof(path), "health-rebuild-publication");
    cbm_zova_workspace_generation_result_t rich = {0}, failed = {0}, rebuilt = {0};
    ASSERT_EQ(operations_publish_rich_fixture(path, &rich), 0);
    ASSERT_EQ(cbm_zova_delete_workspace_graph(path, rich.workspace_id), 0);
    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_OK;
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_health(path, rich.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED);

    cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", "user_before_commit", 1);
    ASSERT_EQ(operations_publish_rich_fixture(path, &failed), -1);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_workspace_health_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s') "
             "AND state='rebuild_required'", rich.workspace_id);
    ASSERT_EQ(operations_scalar(path, sql), 1);
    ASSERT_NULL(cbm_zova_repository_open(path, "workspace-rich"));

    ASSERT_EQ(operations_publish_rich_fixture(path, &rebuilt), 0);
    ASSERT_GT(rebuilt.generation, rich.generation);
    ASSERT_EQ(operations_scalar(path, sql), 0);
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, "workspace-rich");
    ASSERT_NOT_NULL(repo);
    cbm_zova_repository_close(repo);
    ASSERT_EQ(cbm_zova_database_health(path, rebuilt.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_OK);

    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_workspace_recover_runs_full_source_rebuild_and_retries) {
    char base[OPERATIONS_PATH_MAX], repo_path[OPERATIONS_PATH_MAX];
    char source_path[OPERATIONS_PATH_MAX], path[OPERATIONS_PATH_MAX];
    snprintf(base, sizeof(base), "%s/cbm-zova-recover-%d", cbm_tmpdir(), (int)getpid());
    snprintf(repo_path, sizeof(repo_path), "%s/tiny-recover", base);
    snprintf(source_path, sizeof(source_path), "%s/main.c", repo_path);
    snprintf(path, sizeof(path), "%s/cbm.zova", base);
    ASSERT_TRUE(cbm_mkdir_p(repo_path, 0700));
    FILE *source = fopen(source_path, "wb");
    ASSERT_NOT_NULL(source);
    const char *program = "int helper(void){return 1;} int main(void){return helper();}\n";
    ASSERT_EQ(fwrite(program, 1, strlen(program), source), strlen(program));
    ASSERT_EQ(fclose(source), 0);

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", base, 1);
    cbm_zova_workspace_generation_result_t seeded = {0};
    ASSERT_EQ(operations_publish_rich_fixture_named(
                  path, repo_path, "tiny-recover", "{\"summary\":\"before\"}", &seeded), 0);
    ASSERT_EQ(cbm_zova_delete_workspace_graph(path, seeded.workspace_id), 0);
    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_OK;
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_health(path, seeded.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_WORKSPACE_REBUILD_REQUIRED);

    cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", "user_before_commit", 1);
    ASSERT_EQ(cbm_zova_workspace_recover(path, seeded.workspace_id, repo_path, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    ASSERT_NULL(cbm_zova_repository_open(path, "tiny-recover"));
    ASSERT_EQ(cbm_zova_workspace_recover(path, seeded.workspace_id, repo_path, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT_STR_EQ(report.reason, "ok");
    ASSERT_GT(report.generation, seeded.generation);
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, "tiny-recover");
    ASSERT_NOT_NULL(repo);
    cbm_zova_repository_close(repo);
    ASSERT_EQ(cbm_zova_database_health(path, seeded.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_OK);

    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    char sidecar[OPERATIONS_PATH_MAX + 32];
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path); cbm_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path); cbm_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s.writer.lock", path); cbm_unlink(sidecar);
    cbm_unlink(path);
    cbm_unlink(source_path);
    (void)rmdir(repo_path);
    (void)rmdir(base);
    PASS();
}

TEST(zova_operations_whole_file_corruption_recovers_only_through_verified_restore) {
    char live[OPERATIONS_PATH_MAX], backup[OPERATIONS_PATH_MAX];
    operations_path(live, sizeof(live), "health-whole-live");
    operations_path(backup, sizeof(backup), "health-whole-backup");
    cbm_zova_workspace_generation_result_t rich_a = {0}, rich_b = {0};
    ASSERT_EQ(operations_publish_rich_fixture_named(
                  live, "/tmp/health-whole-a", "health-whole-a",
                  "{\"summary\":\"whole-a\"}", &rich_a), 0);
    ASSERT_EQ(operations_publish_rich_fixture_named(
                  live, "/tmp/health-whole-b", "health-whole-b",
                  "{\"summary\":\"whole-b\"}", &rich_b), 0);
    operations_workspace_state_t before_a = {0}, before_b = {0};
    operations_workspace_state_t after_a = {0}, after_b = {0};
    ASSERT_EQ(operations_workspace_state_capture(live, rich_a.workspace_id, &before_a), 0);
    ASSERT_EQ(operations_workspace_state_capture(live, rich_b.workspace_id, &before_b), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_backup(live, backup, &report), CBM_ZOVA_OPERATION_OK);

    FILE *corrupt = fopen(live, "wb");
    ASSERT_NOT_NULL(corrupt);
    ASSERT_EQ(fwrite("corrupt", 1, 7, corrupt), 7);
    ASSERT_EQ(fclose(corrupt), 0);
    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_OK;
    ASSERT_EQ(cbm_zova_database_health(live, rich_a.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY);
    ASSERT_NULL(cbm_zova_repository_open(live, "health-whole-a"));
    ASSERT_NULL(cbm_zova_repository_open(live, "health-whole-b"));

    ASSERT_EQ(cbm_zova_database_restore(live, backup, true, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(operations_workspace_state_capture(live, rich_a.workspace_id, &after_a), 0);
    ASSERT_EQ(operations_workspace_state_capture(live, rich_b.workspace_id, &after_b), 0);
    ASSERT_TRUE(operations_workspace_state_equal(&before_a, &after_a));
    ASSERT_TRUE(operations_workspace_state_equal(&before_b, &after_b));
    ASSERT_EQ(cbm_zova_database_health(live, rich_a.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(cbm_zova_database_health(live, rich_b.workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_OK);

    operations_workspace_state_free(&after_b);
    operations_workspace_state_free(&after_a);
    operations_workspace_state_free(&before_b);
    operations_workspace_state_free(&before_a);
    cbm_unlink(backup);
    cbm_unlink(live);
    PASS();
}

/* A database another connection holds locked is unavailable, not damaged: health
 * must say so, and recover must leave the file in place. */
TEST(zova_operations_recover_does_not_quarantine_a_busy_database) {
    char base[OPERATIONS_PATH_MAX], repo_path[OPERATIONS_PATH_MAX];
    char path[OPERATIONS_PATH_MAX], quarantine[OPERATIONS_PATH_MAX];
    snprintf(base, sizeof(base), "%s/cbm-zova-busy-recover-%d", cbm_tmpdir(), (int)getpid());
    snprintf(repo_path, sizeof(repo_path), "%s/busy-repo", base);
    snprintf(path, sizeof(path), "%s/cbm.zova", base);
    snprintf(quarantine, sizeof(quarantine), "%s/cbm.corrupt.zova", base);
    ASSERT_TRUE(cbm_mkdir_p(repo_path, 0700));
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(operations_publish_fixture(path, repo_path, "busy-recover", 1, &published), 0);
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    ASSERT_EQ(cbm_zova_workspace_id_for_root(repo_path, workspace_id, sizeof(workspace_id)), 0);
    ASSERT_STR_EQ(published.workspace_id, workspace_id);

    /* Exclusive locking mode keeps the write lock after the commit, so every
     * other connection gets SQLITE_BUSY until this one closes. */
    sqlite3 *holder = NULL;
    ASSERT_EQ(sqlite3_open(path, &holder), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(holder,
                           "PRAGMA locking_mode=EXCLUSIVE;BEGIN IMMEDIATE;"
                           "UPDATE cbm_database_schema_v1 SET id=id WHERE id=1;COMMIT;",
                           NULL, NULL, NULL),
              SQLITE_OK);

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", base, 1);
    cbm_zova_health_class_t health = CBM_ZOVA_HEALTH_OK;
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_health(path, workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_BUSY);
    ASSERT_STR_EQ(report.reason, "database_unavailable");
    ASSERT_EQ(cbm_zova_workspace_recover(path, workspace_id, repo_path, &report),
              CBM_ZOVA_OPERATION_BUSY);
    ASSERT_STR_EQ(report.reason, "database_unavailable");
    ASSERT_TRUE(access(path, F_OK) == 0);
    ASSERT_TRUE(access(quarantine, F_OK) != 0);

    sqlite3_close(holder);
    ASSERT_EQ(cbm_zova_database_health(path, workspace_id, &health, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(health, CBM_ZOVA_HEALTH_OK);

    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    cbm_unlink(path);
    char sidecar[OPERATIONS_PATH_MAX + 32];
    const char *suffixes[] = {"-wal", "-shm", ".writer.lock", ".prepare.lock",
                              ".prepare.turnstile.lock"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        snprintf(sidecar, sizeof(sidecar), "%s%s", path, suffixes[i]);
        cbm_unlink(sidecar);
    }
    snprintf(sidecar, sizeof(sidecar), "%s.ws-%s.lock", path, workspace_id);
    cbm_unlink(sidecar);
    (void)rmdir(repo_path);
    (void)rmdir(base);
    PASS();
}

TEST(zova_operations_workspace_recover_quarantines_whole_file_without_replacement) {
    char base[OPERATIONS_PATH_MAX], repo_path[OPERATIONS_PATH_MAX];
    char path[OPERATIONS_PATH_MAX], quarantine[OPERATIONS_PATH_MAX];
    snprintf(base, sizeof(base), "%s/cbm-zova-quarantine-%d", cbm_tmpdir(), (int)getpid());
    snprintf(repo_path, sizeof(repo_path), "%s/tiny-quarantine", base);
    snprintf(path, sizeof(path), "%s/cbm.zova", base);
    snprintf(quarantine, sizeof(quarantine), "%s/cbm.corrupt.zova", base);
    ASSERT_TRUE(cbm_mkdir_p(repo_path, 0700));
    FILE *corrupt = fopen(path, "wb");
    ASSERT_NOT_NULL(corrupt);
    const char bytes[] = "corrupt-without-backup";
    ASSERT_EQ(fwrite(bytes, 1, sizeof(bytes), corrupt), sizeof(bytes));
    ASSERT_EQ(fclose(corrupt), 0);
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    ASSERT_EQ(cbm_zova_workspace_id_for_root(repo_path, workspace_id, sizeof(workspace_id)), 0);

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", base, 1);
    cbm_zova_operation_report_t report = {0};
    /* Moving the file aside replaces the database under every workspace, so it
     * waits for index runs elsewhere (shared barrier holders) to finish. */
    cbm_zova_coord_hold_t other_run = {0};
    ASSERT_EQ(cbm_zova_prepare_barrier_acquire(path, false, 0, &other_run), CBM_ZOVA_COORD_OK);
    ASSERT_EQ(cbm_zova_workspace_recover(path, workspace_id, repo_path, &report),
              CBM_ZOVA_OPERATION_BUSY);
    ASSERT_STR_EQ(report.reason, "indexing_in_progress");
    ASSERT_TRUE(access(path, F_OK) == 0);
    ASSERT_TRUE(access(quarantine, F_OK) != 0);
    cbm_zova_coord_release(&other_run);
    ASSERT_EQ(cbm_zova_workspace_recover(path, workspace_id, repo_path, &report),
              CBM_ZOVA_OPERATION_WHOLE_FILE_RECOVERY_REQUIRED);
    ASSERT_STR_EQ(report.reason, "quarantined_no_verified_backup");
    ASSERT_TRUE(access(path, F_OK) != 0);
    ASSERT_TRUE(access(quarantine, F_OK) == 0);
    struct stat quarantined = {0};
    ASSERT_EQ(stat(quarantine, &quarantined), 0);
    ASSERT_EQ(quarantined.st_size, sizeof(bytes));

    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    cbm_unlink(quarantine);
    char lock[OPERATIONS_PATH_MAX + 32];
    snprintf(lock, sizeof(lock), "%s.writer.lock", path); cbm_unlink(lock);
    (void)rmdir(repo_path);
    (void)rmdir(base);
    PASS();
}
#endif

TEST(zova_operations_empty_bootstrap_creates_exact_v7_health_schema) {
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "empty-v5");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    char columns[128];
    ASSERT_EQ(operations_text(path,
                              "SELECT group_concat(name,',') FROM "
                              "pragma_table_info('cbm_workspace_health_v1')",
                              columns, sizeof(columns)),
              0);
    ASSERT_STR_EQ(columns, "workspace_key,state,reason,checked_generation,checked_at");
    char ddl[512];
    ASSERT_EQ(operations_text(path,
                              "SELECT sql FROM sqlite_master WHERE type='table' AND "
                              "name='cbm_workspace_health_v1'",
                              ddl, sizeof(ddl)),
              0);
    ASSERT_NOT_NULL(strstr(ddl, "state IN ('healthy','rebuild_required')"));
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_pre_v7_init_requires_repack_without_writes) {
    char path[OPERATIONS_PATH_MAX];
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    operations_path(path, sizeof(path), "ordered-v1");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(operations_sql(path,
                             "DROP TABLE IF EXISTS cbm_workspace_health_v1;"
                             "DROP TABLE cbm_workspace_migrations_v1;"
                             "UPDATE cbm_database_schema_v1 SET schema_version=1 WHERE id=1"),
              0);
    ASSERT_EQ(operations_file_digest(path, before), 0);
    ASSERT_EQ(cbm_zova_user_database_init(path), -1);
    ASSERT_EQ(operations_file_digest(path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              1);
    cbm_unlink(path);

    operations_path(path, sizeof(path), "direct-v4");
    ASSERT_EQ(operations_make_v4(path), 0);
    ASSERT_EQ(operations_file_digest(path, before), 0);
    ASSERT_EQ(cbm_zova_user_database_init(path), -1);
    ASSERT_EQ(operations_file_digest(path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              4);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                                "name='cbm_workspace_health_v1'"),
              0);
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_v7_init_is_idempotent) {
    char path[OPERATIONS_PATH_MAX];
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    operations_path(path, sizeof(path), "v5-idempotent");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(operations_file_digest(path, before), 0);
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(operations_file_digest(path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                                "name='cbm_workspace_health_v1'"),
              1);
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_malformed_v7_shape_is_rejected_without_writes) {
    char path[OPERATIONS_PATH_MAX];
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    operations_path(path, sizeof(path), "malformed-v5");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(operations_sql(
                  path,
                  "DROP TABLE cbm_workspace_health_v1;"
                  "CREATE TABLE cbm_workspace_health_v1("
                  "workspace_id TEXT PRIMARY KEY,"
                  "state TEXT NOT NULL CHECK(state IN ('healthy','rebuild_required')),"
                  "reason TEXT NOT NULL,checked_generation INTEGER NOT NULL,"
                  "checked_when TEXT NOT NULL,"
                  "FOREIGN KEY(workspace_id) REFERENCES cbm_workspace_registry(workspace_id))"),
              0);
    ASSERT_EQ(operations_file_digest(path, before), 0);
    ASSERT_EQ(cbm_zova_user_database_init(path), -1);
    ASSERT_EQ(operations_file_digest(path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_malformed_v4_and_future_v8_fail_closed_without_writes) {
    char path[OPERATIONS_PATH_MAX];
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    char digest_before[256], digest_after[256];
    operations_path(path, sizeof(path), "malformed-v4");
    ASSERT_EQ(operations_make_v4(path), 0);
    ASSERT_EQ(operations_add_ready_workspace(path), 0);
    ASSERT_EQ(operations_sql(path, "DROP TABLE cbm_workspace_migrations_v1"), 0);
    ASSERT_EQ(operations_ready_digest(path, digest_before, sizeof(digest_before)), 0);
    ASSERT_EQ(operations_file_digest(path, before), 0);
    ASSERT_EQ(cbm_zova_user_database_init(path), -1);
    ASSERT_EQ(operations_file_digest(path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_ready_digest(path, digest_after, sizeof(digest_after)), 0);
    ASSERT_STR_EQ(digest_before, digest_after);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              4);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT count(*) FROM sqlite_master WHERE "
                                "name='cbm_workspace_health_v1'"),
              0);
    cbm_unlink(path);

    operations_path(path, sizeof(path), "future-v8");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(operations_add_ready_workspace(path), 0);
    ASSERT_EQ(operations_sql(path,
                             "UPDATE cbm_database_schema_v1 SET schema_version=8 WHERE id=1"),
              0);
    ASSERT_EQ(operations_ready_digest(path, digest_before, sizeof(digest_before)), 0);
    ASSERT_EQ(operations_file_digest(path, before), 0);
    ASSERT_EQ(cbm_zova_user_database_init(path), -1);
    ASSERT_EQ(operations_file_digest(path, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_ready_digest(path, digest_after, sizeof(digest_after)), 0);
    ASSERT_STR_EQ(digest_before, digest_after);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              8);
    cbm_unlink(path);
    PASS();
}

TEST(zova_operations_interrupted_v7_bootstrap_rolls_back_and_retries_once) {
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "interrupt-v6");
    cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", "schema_v6_before_commit", 1);
    ASSERT_EQ(cbm_zova_user_database_init(path), -1);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    ASSERT_EQ(operations_scalar(path,
                                "SELECT count(*) FROM sqlite_master WHERE "
                                "name='cbm_database_schema_v1'"),
              0);
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    ASSERT_EQ(operations_scalar(path,
                                "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                                "name='cbm_workspace_health_v1'"),
              1);
    cbm_unlink(path);
    PASS();
}

#if CBM_WITH_ZOVA
TEST(zova_operations_repository_snapshot_is_owned_and_workspace_filtered) {
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "snapshot-owned");
    cbm_zova_workspace_generation_result_t a = {0}, b = {0};
    ASSERT_EQ(operations_publish_fixture(path, "/tmp/snapshot-owned-a", "snapshot-a", 2, &a),
              0);
    ASSERT_EQ(operations_publish_fixture(path, "/tmp/snapshot-owned-b", "snapshot-b", 1, &b),
              0);

    cbm_zova_workspace_snapshot_t snapshot = {0};
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path, a.workspace_id, &snapshot), 0);
    ASSERT_STR_EQ(snapshot.workspace_id, a.workspace_id);
    ASSERT_STR_EQ(snapshot.root_path, "/tmp/snapshot-owned-a");
    ASSERT_STR_EQ(snapshot.project, "snapshot-a");
    ASSERT_EQ(snapshot.generation, 1);
    ASSERT_EQ(snapshot.node_count, 2);
    ASSERT_EQ(snapshot.edge_count, 1);
    ASSERT_EQ(snapshot.node_vector_count, 2);
    ASSERT_EQ(snapshot.token_vector_count, 2);
    ASSERT_EQ(snapshot.integrity.generation, 1);
    ASSERT_STR_EQ(snapshot.integrity.workspace_id, a.workspace_id);
    for (int i = 0; i < snapshot.node_count; i++)
        ASSERT_STR_EQ(snapshot.nodes[i].project, "snapshot-a");
    for (int i = 0; i < snapshot.edge_count; i++)
        ASSERT_STR_EQ(snapshot.edges[i].project, "snapshot-a");
    ASSERT_EQ(snapshot.node_vectors[0].vector_len, 2);
    ASSERT_EQ(snapshot.token_vectors[0].vector_len, 2);

    cbm_unlink(path);
    int root_one_id = 0;
    int found_root_two = 0;
    for (int i = 0; i < snapshot.node_count; i++) {
        if (strcmp(snapshot.nodes[i].qualified_name, "fixture.root_one") == 0)
            root_one_id = (int)snapshot.nodes[i].id;
        if (strcmp(snapshot.nodes[i].qualified_name, "fixture.root_two") == 0)
            found_root_two = 1;
    }
    ASSERT(root_one_id > 0);
    ASSERT(found_root_two);
    int found_root_one_token = 0;
    int found_root_two_token = 0;
    for (int i = 0; i < snapshot.token_vector_count; i++) {
        if (strcmp(snapshot.token_vectors[i].token, "root-one") == 0) found_root_one_token = 1;
        if (strcmp(snapshot.token_vectors[i].token, "root-two") == 0) found_root_two_token = 1;
    }
    ASSERT(found_root_one_token);
    ASSERT(found_root_two_token);
    int found_root_one_vector = 0;
    for (int i = 0; i < snapshot.node_vector_count; i++)
        if (snapshot.node_vectors[i].node_id == root_one_id &&
            snapshot.node_vectors[i].vector[0] == 127)
            found_root_one_vector = 1;
    ASSERT(found_root_one_vector);
    cbm_zova_workspace_snapshot_free(&snapshot);
    ASSERT_NULL(snapshot.root_path);
    ASSERT_NULL(snapshot.nodes);
    ASSERT_EQ(snapshot.node_count, 0);
    ASSERT_EQ(snapshot.workspace_id[0], '\0');
    PASS();
}

TEST(zova_operations_online_backup_excludes_held_wal_generation) {
    char source[OPERATIONS_PATH_MAX], backup[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "backup-live");
    operations_path(backup, sizeof(backup), "backup-snapshot");
    cbm_zova_workspace_generation_result_t a_first = {0}, b_first = {0};
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/operations-backup-a", "backup-a", 1,
                                         &a_first),
              0);
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/operations-backup-b", "backup-b", 1,
                                         &b_first),
              0);

    zova_database *writer = NULL;
    zova_message error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path = source, .out_db = &writer, .out_error_message = &error}),
              ZOVA_OK);
    zova_message_free(&error);
    ASSERT_EQ(zova_database_begin_immediate(&(zova_database_simple_request){.db = writer}),
              ZOVA_OK);
    cbm_zova_workspace_generation_result_t held = {0};
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "backup-a", .label = "Function", .name = "root",
         .qualified_name = "fixture.root_one", .file_path = "root1.c", .properties = "{}"},
        {.id = 2, .project = "backup-a", .label = "Function", .name = "root",
         .qualified_name = "fixture.root_two", .file_path = "root2.c", .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1, .project = "backup-a", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    const int8_t first[] = {127, 0}, second[] = {0, 127};
    const CBMDumpVector node_vectors[] = {
        {.node_id = 1, .project = "backup-a", .vector = (const uint8_t *)first,
         .vector_len = 2},
        {.node_id = 2, .project = "backup-a", .vector = (const uint8_t *)second,
         .vector_len = 2},
    };
    const CBMDumpTokenVec token_vectors[] = {
        {.id = 1, .project = "backup-a", .token = "root-one",
         .vector = (const uint8_t *)first, .vector_len = 2, .idf = 1.0f},
        {.id = 2, .project = "backup-a", .token = "root-two",
         .vector = (const uint8_t *)second, .vector_len = 2, .idf = 1.0f},
    };
    cbm_zova_workspace_generation_input_t held_input = {
        .root_path = "/tmp/operations-backup-a", .project = "backup-a",
        .indexed_at = "2026-07-13T01:00:01Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 2,
        .nodes = nodes, .node_count = 2, .edges = edges, .edge_count = 1,
        .node_vectors = node_vectors, .node_vector_count = 2,
        .token_vectors = token_vectors, .token_vector_count = 2,
    };
    ASSERT_EQ(cbm_zova_user_database_publish_workspace_tx(writer, &held_input, &held), 0);
    ASSERT_EQ(held.generation, 2);
    ASSERT_EQ(operations_repository_inventory(source, "backup-a", 1, 1, 0, 1, 1), 0);

    cbm_zova_operation_report_t report = {0};
    cbm_zova_operation_code_t code = cbm_zova_database_backup(source, backup, &report);
    ASSERT_EQ(zova_database_rollback(&(zova_database_simple_request){.db = writer}), ZOVA_OK);
    ASSERT_EQ(zova_database_close(writer), ZOVA_OK);
    ASSERT_EQ(code, CBM_ZOVA_OPERATION_OK);
    ASSERT_STR_EQ(report.operation, "backup");
    ASSERT_STR_EQ(report.reason, "ok");
    ASSERT_EQ(report.schema_version, CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    ASSERT_TRUE(report.database_bytes > 0);
    ASSERT_EQ(operations_repository_inventory(backup, "backup-a", 1, 1, 0, 1, 1), 0);
    ASSERT_EQ(operations_repository_inventory(backup, "backup-b", 1, 1, 0, 1, 1), 0);

    cbm_unlink(backup);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_backup_refuses_symlink_source_without_creating_destination) {
    char source[OPERATIONS_PATH_MAX], source_alias[OPERATIONS_PATH_MAX];
    char destination[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "backup-symlink-source");
    operations_path(source_alias, sizeof(source_alias), "backup-symlink-alias");
    operations_path(destination, sizeof(destination), "backup-symlink-destination");
    cbm_zova_workspace_generation_result_t result = {0};
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/operations-backup-symlink", "symlink",
                                         2, &result),
              0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(source, before), 0);
    ASSERT_EQ(symlink(source, source_alias), 0);

    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_backup(source_alias, destination, &report),
              CBM_ZOVA_OPERATION_INVALID);
    ASSERT_TRUE(access(destination, F_OK) != 0);
    ASSERT_EQ(operations_file_digest(source, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_repository_inventory(source, "symlink", 1, 2, 1, 2, 2), 0);

    cbm_unlink(source_alias);
    cbm_unlink(destination);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_restore_requires_confirmation_and_replaces_verified_inventory) {
    char source[OPERATIONS_PATH_MAX], backup[OPERATIONS_PATH_MAX], live[OPERATIONS_PATH_MAX];
    char restore_temp[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "restore-source");
    operations_path(backup, sizeof(backup), "restore-backup");
    operations_path(live, sizeof(live), "restore-live");
    size_t live_len = strlen(live);
    ASSERT_TRUE(live_len > 5);
    ASSERT_TRUE(snprintf(restore_temp, sizeof(restore_temp), "%.*s.restore.tmp.zova",
                         (int)(live_len - 5), live) < (int)sizeof(restore_temp));
    ASSERT_STR_EQ(restore_temp + strlen(restore_temp) - 5, ".zova");
    ASSERT_TRUE(access(restore_temp, F_OK) != 0);
    cbm_zova_workspace_generation_result_t source_result = {0}, live_result = {0};
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/operations-restore", "restore", 2,
                                         &source_result),
              0);
    ASSERT_EQ(operations_publish_fixture(live, "/tmp/operations-restore", "restore", 1,
                                         &live_result),
              0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_backup(source, backup, &report), CBM_ZOVA_OPERATION_OK);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(live, before), 0);

    ASSERT_EQ(cbm_zova_database_restore(live, "/tmp/operations-missing-backup", true, &report),
              CBM_ZOVA_OPERATION_INVALID);
    ASSERT_EQ(operations_file_digest(live, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(cbm_zova_database_restore(live, backup, false, &report),
              CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED);
    ASSERT_STR_EQ(report.operation, "restore");
    ASSERT_EQ(operations_file_digest(live, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(cbm_zova_database_restore(live, live, true, &report), CBM_ZOVA_OPERATION_INVALID);
    ASSERT_EQ(operations_file_digest(live, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);

    ASSERT_EQ(cbm_zova_database_restore(live, backup, true, &report), CBM_ZOVA_OPERATION_OK);
    ASSERT_STR_EQ(report.operation, "restore");
    ASSERT_STR_EQ(report.reason, "ok");
    ASSERT_EQ(report.schema_version, CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    ASSERT_TRUE(access(restore_temp, F_OK) != 0);
    ASSERT_EQ(operations_repository_inventory(live, "restore", 1, 2, 1, 2, 2), 0);
    ASSERT_EQ(operations_repository_inventory(backup, "restore", 1, 2, 1, 2, 2), 0);

    cbm_unlink(live);
    cbm_unlink(backup);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_restore_fault_phases_retry_to_verified_live_database) {
    const char *phases[] = {
        "restore_after_temp_creation",
        "restore_after_temp_verification",
        "restore_after_live_to_recovery",
        "restore_after_temp_to_live",
        "restore_before_recovery_cleanup",
    };
    for (size_t phase = 0; phase < sizeof(phases) / sizeof(phases[0]); phase++) {
        char source[OPERATIONS_PATH_MAX], backup[OPERATIONS_PATH_MAX];
        char live[OPERATIONS_PATH_MAX], temporary[OPERATIONS_PATH_MAX];
        char recovery[OPERATIONS_PATH_MAX];
        char label[64];
        snprintf(label, sizeof(label), "restore-fault-source-%zu", phase);
        operations_path(source, sizeof(source), label);
        snprintf(label, sizeof(label), "restore-fault-backup-%zu", phase);
        operations_path(backup, sizeof(backup), label);
        snprintf(label, sizeof(label), "restore-fault-live-%zu", phase);
        operations_path(live, sizeof(live), label);
        size_t live_len = strlen(live);
        ASSERT_TRUE(snprintf(temporary, sizeof(temporary), "%.*s.restore.tmp.zova",
                             (int)(live_len - 5), live) < (int)sizeof(temporary));
        ASSERT_TRUE(snprintf(recovery, sizeof(recovery), "%.*s.restore.recovery.zova",
                             (int)(live_len - 5), live) < (int)sizeof(recovery));

        cbm_zova_workspace_generation_result_t source_result = {0}, live_result = {0};
        ASSERT_EQ(operations_publish_fixture(source, "/tmp/operations-restore-fault", "fault",
                                             2, &source_result),
                  0);
        ASSERT_EQ(operations_publish_fixture(live, "/tmp/operations-restore-fault", "fault", 1,
                                             &live_result),
                  0);
        cbm_zova_operation_report_t report = {0};
        ASSERT_EQ(cbm_zova_database_backup(source, backup, &report), CBM_ZOVA_OPERATION_OK);

        cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", phases[phase], 1);
        ASSERT_EQ(cbm_zova_database_restore(live, backup, true, &report),
                  CBM_ZOVA_OPERATION_VERIFY_FAILED);
        cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");

        int original_remains =
            (access(live, F_OK) == 0 &&
             operations_repository_inventory(live, "fault", 1, 1, 0, 1, 1) == 0) ||
            (access(recovery, F_OK) == 0 &&
             operations_repository_inventory(recovery, "fault", 1, 1, 0, 1, 1) == 0);
        int restored_remains =
            (access(live, F_OK) == 0 &&
             operations_repository_inventory(live, "fault", 1, 2, 1, 2, 2) == 0) ||
            (access(temporary, F_OK) == 0 &&
             operations_repository_inventory(temporary, "fault", 1, 2, 1, 2, 2) == 0);
        ASSERT_TRUE(original_remains || restored_remains);

        ASSERT_EQ(cbm_zova_database_restore(live, backup, true, &report),
                  CBM_ZOVA_OPERATION_OK);
        ASSERT_EQ(operations_repository_inventory(live, "fault", 1, 2, 1, 2, 2), 0);
        ASSERT_TRUE(access(temporary, F_OK) != 0);
        ASSERT_TRUE(access(recovery, F_OK) != 0);

        cbm_unlink(live);
        cbm_unlink(backup);
        cbm_unlink(source);
    }
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    PASS();
}

/* A crashed writer leaves a hot -wal next to the live database. SQLite checks a
 * WAL against its own header, not the main file, so if restore leaves that WAL
 * in place it is replayed on top of the restored generation at the next open. */
TEST(zova_operations_restore_ignores_stale_live_wal) {
    char source[OPERATIONS_PATH_MAX], backup[OPERATIONS_PATH_MAX], live[OPERATIONS_PATH_MAX];
    char wal[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "restore-wal-source");
    operations_path(backup, sizeof(backup), "restore-wal-backup");
    operations_path(live, sizeof(live), "restore-wal-live");
    ASSERT_TRUE(snprintf(wal, sizeof(wal), "%s-wal", live) < (int)sizeof(wal));
    cbm_zova_workspace_generation_result_t source_result = {0}, live_result = {0};
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/operations-restore-wal", "walrestore", 2,
                                         &source_result),
              0);
    ASSERT_EQ(operations_publish_fixture(live, "/tmp/operations-restore-wal", "walrestore", 1,
                                         &live_result),
              0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_backup(source, backup, &report), CBM_ZOVA_OPERATION_OK);

    /* Hot-copy a WAL holding a committed write, close cleanly, put the copy back. */
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open(live, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
                           "PRAGMA journal_mode=WAL;PRAGMA wal_autocheckpoint=0;"
                           "CREATE TABLE stale_generation(x);"
                           "INSERT INTO stale_generation VALUES(1);",
                           NULL, NULL, NULL),
              SQLITE_OK);
    FILE *in = fopen(wal, "rb");
    ASSERT_NOT_NULL(in);
    static unsigned char wal_bytes[1 << 22];
    size_t wal_len = fread(wal_bytes, 1, sizeof(wal_bytes), in);
    fclose(in);
    ASSERT_TRUE(wal_len > 32 && wal_len < sizeof(wal_bytes));
    sqlite3_close(db);
    FILE *out = fopen(wal, "wb");
    ASSERT_NOT_NULL(out);
    ASSERT_EQ(fwrite(wal_bytes, 1, wal_len, out), wal_len);
    fclose(out);

    ASSERT_EQ(cbm_zova_database_restore(live, backup, true, &report), CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(operations_scalar(live, "SELECT count(*) FROM sqlite_master "
                                      "WHERE name='stale_generation'"),
              0);
    ASSERT_EQ(operations_repository_inventory(live, "walrestore", 1, 2, 1, 2, 2), 0);

    cbm_unlink(live);
    cbm_unlink(backup);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_database_archive_is_deterministic_and_round_trips) {
    char source[OPERATIONS_PATH_MAX], live[OPERATIONS_PATH_MAX];
    char first[OPERATIONS_PATH_MAX], second[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "archive-source");
    operations_path(live, sizeof(live), "archive-live");
    operations_archive_path(first, sizeof(first), "archive-first");
    operations_archive_path(second, sizeof(second), "archive-second");
    operations_archive_cleanup(first);
    operations_archive_cleanup(second);
    cbm_zova_workspace_generation_result_t result = {0};
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/archive-a", "archive-a", 2, &result),
              0);
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/archive-b", "archive-b", 1, &result),
              0);
    ASSERT_EQ(operations_publish_fixture(live, "/tmp/archive-old", "archive-old", 1, &result),
              0);

    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_export(source, first, &report), CBM_ZOVA_OPERATION_OK);
    ASSERT_STR_EQ(report.operation, "export_database");
    ASSERT_EQ(report.archive_version, CBM_ZOVA_OPERATIONS_ARCHIVE_VERSION);
    ASSERT_EQ(report.schema_version, CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    ASSERT_EQ(cbm_zova_database_export(source, second, &report), CBM_ZOVA_OPERATION_OK);
    char first_manifest[OPERATIONS_PATH_MAX + 64], second_manifest[OPERATIONS_PATH_MAX + 64];
    char first_data[OPERATIONS_PATH_MAX + 64];
    snprintf(first_manifest, sizeof(first_manifest), "%s/manifest.json", first);
    snprintf(second_manifest, sizeof(second_manifest), "%s/manifest.json", second);
    snprintf(first_data, sizeof(first_data), "%s/data.zova", first);
    uint8_t first_digest[CBM_SHA256_DIGEST_LEN], second_digest[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(first_manifest, first_digest), 0);
    ASSERT_EQ(operations_file_digest(second_manifest, second_digest), 0);
    ASSERT_EQ(memcmp(first_digest, second_digest, sizeof(first_digest)), 0);
    struct stat archived;
    ASSERT_EQ(lstat(first_data, &archived), 0);
    ASSERT_TRUE(S_ISREG(archived.st_mode));

    ASSERT_EQ(cbm_zova_database_import(live, first, true, &report), CBM_ZOVA_OPERATION_OK);
    ASSERT_STR_EQ(report.operation, "import_database");
    ASSERT_EQ(report.archive_version, CBM_ZOVA_OPERATIONS_ARCHIVE_VERSION);
    ASSERT_EQ(operations_repository_inventory(live, "archive-a", 1, 2, 1, 2, 2), 0);
    ASSERT_EQ(operations_repository_inventory(live, "archive-b", 1, 1, 0, 1, 1), 0);

    operations_archive_cleanup(second);
    operations_archive_cleanup(first);
    cbm_unlink(live);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_database_archive_refuses_untrusted_inputs_without_live_writes) {
    char source[OPERATIONS_PATH_MAX], live[OPERATIONS_PATH_MAX], source_alias[OPERATIONS_PATH_MAX];
    char archives[6][OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "archive-refusal-source");
    operations_path(live, sizeof(live), "archive-refusal-live");
    operations_path(source_alias, sizeof(source_alias), "archive-refusal-source-alias");
    cbm_unlink(source_alias);
    cbm_zova_workspace_generation_result_t result = {0};
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/archive-refusal", "refusal", 2,
                                         &result),
              0);
    ASSERT_EQ(operations_publish_fixture(live, "/tmp/archive-live", "live", 1, &result), 0);
    cbm_zova_operation_report_t report = {0};
    for (size_t i = 0; i < 6; i++) {
        char label[64];
        snprintf(label, sizeof(label), "archive-refusal-%zu", i);
        operations_archive_path(archives[i], sizeof(archives[i]), label);
        operations_archive_cleanup(archives[i]);
        ASSERT_EQ(cbm_zova_database_export(source, archives[i], &report),
                  CBM_ZOVA_OPERATION_OK);
    }
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(live, before), 0);

    ASSERT_EQ(cbm_zova_database_export(source, archives[0], &report),
              CBM_ZOVA_OPERATION_INVALID);
    ASSERT_EQ(cbm_zova_database_export("/tmp/archive-missing.zova", archives[0], &report),
              CBM_ZOVA_OPERATION_INVALID);
    ASSERT_EQ(symlink(source, source_alias), 0);
    char alias_export[OPERATIONS_PATH_MAX];
    operations_archive_path(alias_export, sizeof(alias_export), "archive-source-alias-output");
    operations_archive_cleanup(alias_export);
    ASSERT_EQ(cbm_zova_database_export(source_alias, alias_export, &report),
              CBM_ZOVA_OPERATION_INVALID);
    cbm_unlink(source_alias);

    ASSERT_EQ(operations_manifest_replace_char(
                  archives[0], "\"archive_version\":1", strlen("\"archive_version\":"), '2'),
              0);
    ASSERT_EQ(cbm_zova_database_import(live, archives[0], true, &report),
              CBM_ZOVA_OPERATION_INCOMPATIBLE);
    ASSERT_EQ(operations_manifest_replace_char(
                  archives[1], "\"schema_version\":7", strlen("\"schema_version\":"), '8'),
              0);
    ASSERT_EQ(cbm_zova_database_import(live, archives[1], true, &report),
              CBM_ZOVA_OPERATION_INCOMPATIBLE);
    ASSERT_EQ(operations_manifest_replace_char(
                  archives[2], "\"sha256\":\"", strlen("\"sha256\":\""), '0'),
              0);
    ASSERT_EQ(cbm_zova_database_import(live, archives[2], true, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    char truncated[OPERATIONS_PATH_MAX + 64];
    snprintf(truncated, sizeof(truncated), "%s/data.zova", archives[3]);
    ASSERT_EQ(truncate(truncated, 128), 0);
    ASSERT_EQ(cbm_zova_database_import(live, archives[3], true, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    ASSERT_EQ(cbm_zova_database_import(live, archives[4], false, &report),
              CBM_ZOVA_OPERATION_CONFIRMATION_REQUIRED);
    ASSERT_EQ(cbm_zova_database_import(live, "/tmp/archive-missing", true, &report),
              CBM_ZOVA_OPERATION_INVALID);

    char archive_symlink[OPERATIONS_PATH_MAX];
    operations_archive_path(archive_symlink, sizeof(archive_symlink), "archive-directory-alias");
    cbm_unlink(archive_symlink);
    ASSERT_EQ(symlink(archives[4], archive_symlink), 0);
    ASSERT_EQ(cbm_zova_database_import(live, archive_symlink, true, &report),
              CBM_ZOVA_OPERATION_INVALID);
    cbm_unlink(archive_symlink);

    char aliased_data[OPERATIONS_PATH_MAX + 64];
    snprintf(aliased_data, sizeof(aliased_data), "%s/data.zova", archives[5]);
    cbm_unlink(aliased_data);
    ASSERT_EQ(link(live, aliased_data), 0);
    ASSERT_EQ(cbm_zova_database_import(live, archives[5], true, &report),
              CBM_ZOVA_OPERATION_INVALID);
    ASSERT_EQ(operations_file_digest(live, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);

    for (size_t i = 0; i < 6; i++) operations_archive_cleanup(archives[i]);
    operations_archive_cleanup(alias_export);
    cbm_unlink(live);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_database_archive_rejects_unexpected_top_level_members) {
    char source[OPERATIONS_PATH_MAX], live[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "archive-extra-source");
    operations_path(live, sizeof(live), "archive-extra-live");
    operations_archive_path(archive, sizeof(archive), "archive-extra-members");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t result = {0};
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/archive-extra-source", "extra", 2,
                                         &result),
              0);
    ASSERT_EQ(operations_publish_fixture(live, "/tmp/archive-extra-live", "live", 1, &result),
              0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_database_export(source, archive, &report), CBM_ZOVA_OPERATION_OK);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(live, before), 0);

    char extra_file[OPERATIONS_PATH_MAX + 64];
    snprintf(extra_file, sizeof(extra_file), "%s/unexpected.txt", archive);
    FILE *file = fopen(extra_file, "wb");
    ASSERT_NOT_NULL(file);
    ASSERT_EQ(fputs("unexpected\n", file) < 0, 0);
    ASSERT_EQ(fclose(file), 0);
    ASSERT_EQ(cbm_zova_database_import(live, archive, true, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    ASSERT_EQ(operations_file_digest(live, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    cbm_unlink(extra_file);

    char extra_directory[OPERATIONS_PATH_MAX + 64];
    snprintf(extra_directory, sizeof(extra_directory), "%s/unexpected", archive);
    ASSERT_EQ(mkdir(extra_directory, 0700), 0);
    ASSERT_EQ(cbm_zova_database_import(live, archive, true, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    ASSERT_EQ(operations_file_digest(live, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(rmdir(extra_directory), 0);

    operations_archive_cleanup(archive);
    cbm_unlink(live);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_export_is_fresh_rich_and_single_workspace) {
    char source[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    char data[OPERATIONS_PATH_MAX + 64];
    operations_path(source, sizeof(source), "workspace-export-source");
    operations_archive_path(archive, sizeof(archive), "workspace-export-rich");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t rich = {0}, other = {0};
    ASSERT_EQ(operations_publish_rich_fixture(source, &rich), 0);
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/workspace-other", "workspace-other", 1,
                                         &other), 0);
    ASSERT_EQ(rich.metadata_edges, 2);
    ASSERT_EQ(rich.metadata_topology_edges, 1);
    ASSERT_EQ(rich.graph_edges, 1);

    cbm_zova_operation_report_t report = {0};
    cbm_zova_operation_code_t export_code =
        cbm_zova_workspace_export(source, rich.workspace_id, archive, &report);
    ASSERT_EQ(export_code, CBM_ZOVA_OPERATION_OK);
    ASSERT_STR_EQ(report.operation, "export_workspace");
    ASSERT_STR_EQ(report.workspace_id, rich.workspace_id);
    ASSERT_EQ(report.generation, rich.generation);
    snprintf(data, sizeof(data), "%s/data.zova", archive);
    ASSERT_EQ(operations_scalar(data, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    ASSERT_EQ(operations_scalar(data, "SELECT count(*) FROM cbm_workspace_registry"), 1);

    cbm_zova_workspace_snapshot_t snapshot = {0};
    ASSERT_EQ(cbm_zova_repository_export_snapshot(data, rich.workspace_id, &snapshot), 0);
    ASSERT_EQ(snapshot.node_count, 3);
    ASSERT_EQ(snapshot.edge_count, 2);
    ASSERT_EQ(snapshot.integrity.graph_edges, 1);
    ASSERT_EQ(snapshot.file_hash_count, 2);
    ASSERT_TRUE(snapshot.project_summary.present);
    ASSERT_STR_EQ(snapshot.project_summary.summary, "{\"summary\":\"rich!\"}");
    ASSERT_STR_EQ(snapshot.integrity.metadata_sha256, rich.metadata_sha256);
    ASSERT_STR_EQ(snapshot.integrity.fts_sha256, rich.fts_sha256);
    ASSERT_STR_EQ(snapshot.integrity.topology_sha256, rich.topology_sha256);
    ASSERT_STR_EQ(snapshot.integrity.node_vector_sha256, rich.node_vector_sha256);
    ASSERT_STR_EQ(snapshot.integrity.token_vector_sha256, rich.token_vector_sha256);
    int saw_punctuation = 0, saw_raw_i8 = 0;
    for (int i = 0; i < snapshot.node_count; i++)
        if (strcmp(snapshot.nodes[i].qualified_name, "rich.parse::alpha/beta?") == 0)
            saw_punctuation = 1;
    for (int i = 0; i < snapshot.node_vector_count; i++)
        if (snapshot.node_vectors[i].vector_len == 3 &&
            snapshot.node_vectors[i].vector[0] == 127 &&
            snapshot.node_vectors[i].vector[1] == 128)
            saw_raw_i8 = 1;
    ASSERT(saw_punctuation);
    ASSERT(saw_raw_i8);

    char source_local_id[128], archive_local_id[128];
    ASSERT_EQ(operations_stable_id_for_qn(source, "workspace-rich", rich.workspace_id,
                                          "rich.parse::<local>#value", source_local_id,
                                          sizeof(source_local_id)), 0);
    ASSERT_TRUE(strncmp(source_local_id, "n:v2:", 5) == 0);
    ASSERT_EQ(operations_stable_id_for_qn(data, "workspace-rich", rich.workspace_id,
                                          "rich.parse::<local>#value", archive_local_id,
                                          sizeof(archive_local_id)), 0);
    ASSERT_STR_EQ(archive_local_id, source_local_id);

    cbm_zova_repository_t *repo = cbm_zova_repository_open(data, "workspace-rich");
    ASSERT_NOT_NULL(repo);
    cbm_search_output_t fts = {0};
    ASSERT_EQ(cbm_zova_repository_search_fts(repo, rich.workspace_id, "alpha beta", NULL, 10, 0,
                                             &fts), CBM_STORE_OK);
    ASSERT_EQ(fts.count, 1);
    cbm_store_search_free(&fts);
    cbm_zova_repository_close(repo);
    repo = cbm_zova_repository_open(data, "workspace-other");
    ASSERT_NULL(repo);

    cbm_zova_workspace_snapshot_free(&snapshot);
    operations_archive_cleanup(archive);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_manifest_binds_complete_inventory_and_escapes_identity) {
    char source[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "workspace-manifest-source");
    operations_archive_path(archive, sizeof(archive), "workspace-manifest");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(source, &rich), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_export(source, rich.workspace_id, archive, &report),
              CBM_ZOVA_OPERATION_OK);

    char manifest[16384], expected[512];
    ASSERT_EQ(operations_manifest_read(archive, manifest, sizeof(manifest), NULL), 0);
    ASSERT_NOT_NULL(strstr(manifest, "\"archive_kind\":\"workspace\""));
    snprintf(expected, sizeof(expected), "\"workspace_id\":\"%s\"", rich.workspace_id);
    ASSERT_NOT_NULL(strstr(manifest, expected));
    ASSERT_NOT_NULL(strstr(manifest, "\"canonical_root\":\"/tmp/workspace-rich\""));
    ASSERT_NOT_NULL(strstr(manifest, "\"project\":\"workspace-rich\""));
    ASSERT_NOT_NULL(strstr(manifest, "\"model_fingerprint\":\"rich_model_v2\""));
    ASSERT_NOT_NULL(strstr(manifest, "\"vector_dimensions\":3"));
    ASSERT_NOT_NULL(strstr(manifest, "\"generation\":1"));
    ASSERT_NOT_NULL(strstr(manifest, "\"graph_nodes\":3"));
    ASSERT_NOT_NULL(strstr(manifest, "\"graph_edges\":1"));
    ASSERT_NOT_NULL(strstr(manifest, "\"metadata_nodes\":3"));
    ASSERT_NOT_NULL(strstr(manifest, "\"metadata_edges\":2"));
    ASSERT_NOT_NULL(strstr(manifest, "\"metadata_topology_edges\":1"));
    ASSERT_NOT_NULL(strstr(manifest, "\"fts_rows\":3"));
    ASSERT_NOT_NULL(strstr(manifest, "\"node_vector_rows\":3"));
    ASSERT_NOT_NULL(strstr(manifest, "\"token_vector_rows\":2"));
    ASSERT_NOT_NULL(strstr(manifest, "\"node_vectors\":3"));
    ASSERT_NOT_NULL(strstr(manifest, "\"token_vectors\":2"));
    snprintf(expected, sizeof(expected), "\"metadata_sha256\":\"%s\"", rich.metadata_sha256);
    ASSERT_NOT_NULL(strstr(manifest, expected));
    snprintf(expected, sizeof(expected), "\"fts_sha256\":\"%s\"", rich.fts_sha256);
    ASSERT_NOT_NULL(strstr(manifest, expected));
    snprintf(expected, sizeof(expected), "\"topology_sha256\":\"%s\"", rich.topology_sha256);
    ASSERT_NOT_NULL(strstr(manifest, expected));
    snprintf(expected, sizeof(expected), "\"node_vector_sha256\":\"%s\"",
             rich.node_vector_sha256);
    ASSERT_NOT_NULL(strstr(manifest, expected));
    snprintf(expected, sizeof(expected), "\"token_vector_sha256\":\"%s\"",
             rich.token_vector_sha256);
    ASSERT_NOT_NULL(strstr(manifest, expected));

    operations_archive_cleanup(archive);
    cbm_unlink(source);
    operations_path(source, sizeof(source), "workspace-manifest-escape-source");
    operations_archive_path(archive, sizeof(archive), "workspace-manifest-escape");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t escaped = {0};
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/workspace-\"quoted",
                                         "project-\"quoted", 1, &escaped), 0);
    cbm_zova_operation_code_t escaped_export =
        cbm_zova_workspace_export(source, escaped.workspace_id, archive, &report);
    ASSERT_STR_EQ(report.reason, "ok");
    ASSERT_EQ(escaped_export, CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(operations_manifest_read(archive, manifest, sizeof(manifest), NULL), 0);
    ASSERT_NOT_NULL(strstr(manifest,
                           "\"canonical_root\":\"/tmp/workspace-\\\"quoted\""));
    ASSERT_NOT_NULL(strstr(manifest, "\"project\":\"project-\\\"quoted\""));

    operations_archive_cleanup(archive);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_import_refuses_each_manifest_binding_without_target_writes) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "workspace-manifest-tamper-source");
    operations_path(target, sizeof(target), "workspace-manifest-tamper-target");
    operations_archive_path(archive, sizeof(archive), "workspace-manifest-tamper");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t rich = {0}, resident = {0};
    ASSERT_EQ(operations_publish_rich_fixture(source, &rich), 0);
    ASSERT_EQ(operations_publish_fixture(target, "/tmp/workspace-manifest-resident",
                                         "manifest-resident", 1, &resident), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_export(source, rich.workspace_id, archive, &report),
              CBM_ZOVA_OPERATION_OK);

    char original[16384], workspace_binding[256];
    size_t original_length = 0;
    ASSERT_EQ(operations_manifest_read(archive, original, sizeof(original), &original_length), 0);
    snprintf(workspace_binding, sizeof(workspace_binding), "\"workspace_id\":\"%s",
             rich.workspace_id);
    const char *bindings[] = {
        "\"archive_kind\":\"workspace", workspace_binding,
        "\"canonical_root\":\"/tmp/workspace-rich", "\"project\":\"workspace-rich",
        "\"model_fingerprint\":\"rich_model_v2", "\"vector_dimensions\":3",
        "\"generation\":1", "\"graph_nodes\":3", "\"graph_edges\":1",
        "\"metadata_nodes\":3", "\"metadata_edges\":2",
        "\"metadata_topology_edges\":1", "\"fts_rows\":3",
        "\"node_vector_rows\":3", "\"token_vector_rows\":2",
        "\"node_vectors\":3", "\"token_vectors\":2",
    };
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(target, before), 0);
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        ASSERT_EQ(operations_manifest_write(archive, original, original_length), 0);
        ASSERT_EQ(operations_manifest_replace_char(archive, bindings[i], strlen(bindings[i]) - 1,
                                                   '9'), 0);
        ASSERT_EQ(cbm_zova_workspace_import(target, archive, false, &report),
                  CBM_ZOVA_OPERATION_VERIFY_FAILED);
        ASSERT_EQ(operations_file_digest(target, after), 0);
        ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    }

    ASSERT_EQ(operations_manifest_write(archive, original, original_length), 0);
    operations_archive_cleanup(archive);
    cbm_unlink(target);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_import_adds_rich_workspace_without_touching_resident) {
    char source[OPERATIONS_PATH_MAX], empty[OPERATIONS_PATH_MAX], resident[OPERATIONS_PATH_MAX];
    char archive[OPERATIONS_PATH_MAX], archive_data[OPERATIONS_PATH_MAX + 64];
    operations_path(source, sizeof(source), "workspace-import-source");
    operations_path(empty, sizeof(empty), "workspace-import-empty");
    operations_path(resident, sizeof(resident), "workspace-import-resident");
    operations_archive_path(archive, sizeof(archive), "workspace-import-rich");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t rich = {0}, source_b = {0}, resident_b = {0};
    ASSERT_EQ(operations_publish_rich_fixture(source, &rich), 0);
    ASSERT_EQ(operations_publish_fixture(source, "/tmp/workspace-import-source-b", "source-b", 1,
                                         &source_b), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_export(source, rich.workspace_id, archive, &report),
              CBM_ZOVA_OPERATION_OK);
    snprintf(archive_data, sizeof(archive_data), "%s/data.zova", archive);

    ASSERT_EQ(cbm_zova_workspace_import(empty, archive, false, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT(operations_archive_has_two_exact_members(archive));
    ASSERT_STR_EQ(report.operation, "import_workspace");
    ASSERT_STR_EQ(report.workspace_id, rich.workspace_id);
    ASSERT_EQ(report.generation, rich.generation);
    ASSERT_EQ(operations_scalar(empty,
                                "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1"),
              CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    ASSERT_EQ(operations_scalar(empty, "SELECT count(*) FROM cbm_workspace_registry"), 1);
    cbm_zova_workspace_snapshot_t imported_empty = {0};
    ASSERT_EQ(cbm_zova_repository_export_snapshot(empty, rich.workspace_id, &imported_empty), 0);
    ASSERT_EQ(imported_empty.node_count, 3);
    ASSERT_EQ(imported_empty.edge_count, 2);
    ASSERT_EQ(imported_empty.file_hash_count, 2);
    ASSERT_TRUE(imported_empty.project_summary.present);
    ASSERT_STR_EQ(imported_empty.integrity.metadata_sha256, rich.metadata_sha256);
    ASSERT_STR_EQ(imported_empty.integrity.fts_sha256, rich.fts_sha256);
    ASSERT_STR_EQ(imported_empty.integrity.topology_sha256, rich.topology_sha256);
    ASSERT_STR_EQ(imported_empty.integrity.node_vector_sha256, rich.node_vector_sha256);
    ASSERT_STR_EQ(imported_empty.integrity.token_vector_sha256, rich.token_vector_sha256);

    ASSERT_EQ(operations_publish_fixture(resident, "/tmp/workspace-import-resident-b",
                                         "resident-b", 2, &resident_b), 0);
    cbm_zova_workspace_snapshot_t before_b = {0};
    ASSERT_EQ(cbm_zova_repository_export_snapshot(resident, resident_b.workspace_id, &before_b), 0);
    char b_id_before[128], b_id_after[128];
    ASSERT_EQ(operations_stable_id_for_qn(resident, "resident-b", resident_b.workspace_id,
                                          "fixture.root_one", b_id_before,
                                          sizeof(b_id_before)), 0);

    ASSERT_EQ(cbm_zova_workspace_import(resident, archive, false, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT(operations_archive_has_two_exact_members(archive));
    ASSERT_STR_EQ(report.workspace_id, rich.workspace_id);
    cbm_zova_workspace_snapshot_t imported_resident = {0}, after_b = {0};
    ASSERT_EQ(cbm_zova_repository_export_snapshot(resident, rich.workspace_id,
                                                  &imported_resident), 0);
    ASSERT_EQ(cbm_zova_repository_export_snapshot(resident, resident_b.workspace_id, &after_b), 0);
    ASSERT_STR_EQ(imported_resident.integrity.metadata_sha256, rich.metadata_sha256);
    ASSERT_STR_EQ(imported_resident.integrity.fts_sha256, rich.fts_sha256);
    ASSERT_STR_EQ(imported_resident.integrity.topology_sha256, rich.topology_sha256);
    ASSERT_STR_EQ(imported_resident.integrity.node_vector_sha256, rich.node_vector_sha256);
    ASSERT_STR_EQ(imported_resident.integrity.token_vector_sha256, rich.token_vector_sha256);
    ASSERT_STR_EQ(after_b.integrity.metadata_sha256, before_b.integrity.metadata_sha256);
    ASSERT_STR_EQ(after_b.integrity.fts_sha256, before_b.integrity.fts_sha256);
    ASSERT_STR_EQ(after_b.integrity.topology_sha256, before_b.integrity.topology_sha256);
    ASSERT_STR_EQ(after_b.integrity.node_vector_sha256, before_b.integrity.node_vector_sha256);
    ASSERT_STR_EQ(after_b.integrity.token_vector_sha256, before_b.integrity.token_vector_sha256);
    ASSERT_EQ(after_b.node_count, before_b.node_count);
    ASSERT_EQ(after_b.edge_count, before_b.edge_count);
    ASSERT_EQ(after_b.node_vector_count, before_b.node_vector_count);
    ASSERT_EQ(after_b.token_vector_count, before_b.token_vector_count);
    for (int i = 0; i < before_b.node_vector_count; i++) {
        ASSERT_EQ(after_b.node_vectors[i].vector_len, before_b.node_vectors[i].vector_len);
        ASSERT_EQ(memcmp(after_b.node_vectors[i].vector, before_b.node_vectors[i].vector,
                         (size_t)before_b.node_vectors[i].vector_len), 0);
    }
    for (int i = 0; i < before_b.token_vector_count; i++) {
        ASSERT_STR_EQ(after_b.token_vectors[i].token, before_b.token_vectors[i].token);
        ASSERT_EQ(after_b.token_vectors[i].vector_len, before_b.token_vectors[i].vector_len);
        ASSERT_EQ(memcmp(after_b.token_vectors[i].vector, before_b.token_vectors[i].vector,
                         (size_t)before_b.token_vectors[i].vector_len), 0);
    }
    ASSERT_EQ(operations_stable_id_for_qn(resident, "resident-b", resident_b.workspace_id,
                                          "fixture.root_one", b_id_after,
                                          sizeof(b_id_after)), 0);
    ASSERT_STR_EQ(b_id_after, b_id_before);
    ASSERT_EQ(operations_scalar(archive_data, "SELECT count(*) FROM cbm_workspace_registry"), 1);

    cbm_zova_workspace_snapshot_free(&after_b);
    cbm_zova_workspace_snapshot_free(&before_b);
    cbm_zova_workspace_snapshot_free(&imported_resident);
    cbm_zova_workspace_snapshot_free(&imported_empty);
    operations_archive_cleanup(archive);
    cbm_unlink(resident);
    cbm_unlink(empty);
    cbm_unlink(source);
    PASS();
}

/* #12: parse coverage lives in File node properties and the Project node
 * marker, so a workspace export/import carries it unchanged. */
TEST(zova_operations_workspace_import_preserves_parse_coverage) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX];
    char archive[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "coverage-source");
    operations_path(target, sizeof(target), "coverage-target");
    operations_archive_path(archive, sizeof(archive), "coverage-archive");
    operations_archive_cleanup(archive);
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "covered", .label = "Project", .name = "covered",
         .qualified_name = "covered", .file_path = "",
         .properties = "{\"coverage_version\":1}"},
        {.id = 2, .project = "covered", .label = "File", .name = "bad.c",
         .qualified_name = "covered.bad.c", .file_path = "bad.c",
         .properties = "{\"extension\":\".c\",\"coverage\":\"partial\","
                       "\"parse_error_ranges\":\"3-4\"}"},
        {.id = 3, .project = "covered", .label = "File", .name = "ok.c",
         .qualified_name = "covered.ok.c", .file_path = "ok.c",
         .properties = "{\"extension\":\".c\"}"},
    };
    const cbm_zova_file_hash_input_t hashes[] = {
        {.file_path = "bad.c", .content_hash = "bad-hash", .mtime_ns = 1, .size_bytes = 2},
        {.file_path = "ok.c", .content_hash = "ok-hash", .mtime_ns = 3, .size_bytes = 4},
    };
    const cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/covered", .project = "covered",
        .indexed_at = "2026-10-15T00:00:00Z", .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 768, .nodes = nodes, .node_count = 3,
        .file_hashes = hashes, .file_hash_count = 2};
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(source, &input, &published), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_export(source, published.workspace_id, archive, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, false, &report),
              CBM_ZOVA_OPERATION_OK);

    cbm_zova_repository_t *repo = cbm_zova_repository_open(target, "covered");
    ASSERT_NOT_NULL(repo);
    cbm_zova_file_coverage_t *rows = NULL;
    int count = 0;
    bool recorded = false;
    char *root_path = NULL;
    int rc = cbm_zova_repository_file_coverage(repo, cbm_zova_repository_workspace_id(repo),
                                               NULL, &rows, &count, &root_path, NULL,
                                               &recorded);
    cbm_zova_repository_close(repo);
    bool ok = rc == 0 && recorded && count == 2 && root_path &&
              strcmp(root_path, "/tmp/covered") == 0 && strcmp(rows[0].file_path, "bad.c") == 0 &&
              rows[0].properties && strstr(rows[0].properties, "\"parse_error_ranges\":\"3-4\"") &&
              rows[0].has_hash && rows[0].size_bytes == 2 &&
              strcmp(rows[1].file_path, "ok.c") == 0 && rows[1].properties &&
              !strstr(rows[1].properties, "coverage");
    cbm_zova_file_coverage_free(rows, count);
    free(root_path);
    operations_archive_cleanup(archive);
    cbm_unlink(source);
    cbm_unlink(target);
    ASSERT_TRUE(ok);
    PASS();
}

/* Importing a workspace publishes it, so the import refuses (without creating
 * the target) while that workspace's lease or a database replacement is held. */
TEST(zova_operations_workspace_import_refuses_while_index_coordination_is_held) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    char data[OPERATIONS_PATH_MAX + 64], sidecar[OPERATIONS_PATH_MAX + 80];
    operations_path(source, sizeof(source), "workspace-import-coord-source");
    operations_path(target, sizeof(target), "workspace-import-coord-target");
    operations_archive_path(archive, sizeof(archive), "workspace-import-coord");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(source, &rich), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_export(source, rich.workspace_id, archive, &report),
              CBM_ZOVA_OPERATION_OK);
    snprintf(data, sizeof(data), "%s/data.zova", archive);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", data); cbm_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", data); cbm_unlink(sidecar);

    cbm_zova_coord_hold_t hold = {0};
    ASSERT_EQ(cbm_zova_workspace_lease_acquire(target, rich.workspace_id, 0, &hold),
              CBM_ZOVA_COORD_OK);
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, false, &report),
              CBM_ZOVA_OPERATION_BUSY);
    ASSERT_STR_EQ(report.operation, "import_workspace");
    ASSERT_STR_EQ(report.reason, "workspace_busy");
    ASSERT_TRUE(access(target, F_OK) != 0);
    cbm_zova_coord_release(&hold);

    ASSERT_EQ(cbm_zova_prepare_barrier_acquire(target, true, 0, &hold), CBM_ZOVA_COORD_OK);
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, false, &report),
              CBM_ZOVA_OPERATION_BUSY);
    ASSERT_STR_EQ(report.reason, "database_replacement_in_progress");
    ASSERT_TRUE(access(target, F_OK) != 0);
    cbm_zova_coord_release(&hold);

    ASSERT_EQ(cbm_zova_workspace_import(target, archive, false, &report),
              CBM_ZOVA_OPERATION_OK);

    char lock_path[OPERATIONS_PATH_MAX + 128];
    snprintf(lock_path, sizeof(lock_path), "%s.prepare.lock", target);
    cbm_unlink(lock_path);
    snprintf(lock_path, sizeof(lock_path), "%s.prepare.turnstile.lock", target);
    cbm_unlink(lock_path);
    snprintf(lock_path, sizeof(lock_path), "%s.ws-%s.lock", target, rich.workspace_id);
    cbm_unlink(lock_path);
    snprintf(lock_path, sizeof(lock_path), "%s.writer.lock", target);
    cbm_unlink(lock_path);
    operations_archive_cleanup(archive);
    cbm_unlink(target);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_export_and_empty_import_publish_generation_three_once) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    char data[OPERATIONS_PATH_MAX + 64], sql[512];
    operations_path(source, sizeof(source), "workspace-generation-once-source");
    operations_path(target, sizeof(target), "workspace-generation-once-target");
    operations_archive_path(archive, sizeof(archive), "workspace-generation-once");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t rich = {0};
    for (int i = 0; i < 3; i++) ASSERT_EQ(operations_publish_rich_fixture(source, &rich), 0);
    ASSERT_EQ(rich.generation, 3);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_export(source, rich.workspace_id, archive, &report),
              CBM_ZOVA_OPERATION_OK);
    snprintf(data, sizeof(data), "%s/data.zova", archive);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_database_generation_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             rich.workspace_id);
    ASSERT_EQ(operations_scalar(data, sql), 1);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_database_generation_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s') "
             "AND generation=3 AND state='ready'",
             rich.workspace_id);
    ASSERT_EQ(operations_scalar(data, sql), 1);
    char sidecar[OPERATIONS_PATH_MAX + 80];
    snprintf(sidecar, sizeof(sidecar), "%s-wal", data); cbm_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", data); cbm_unlink(sidecar);

    ASSERT_EQ(cbm_zova_workspace_import(target, archive, false, &report),
              CBM_ZOVA_OPERATION_OK);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_database_generation_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             rich.workspace_id);
    ASSERT_EQ(operations_scalar(target, sql), 1);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_database_generation_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s') "
             "AND generation=3 AND state='ready'",
             rich.workspace_id);
    ASSERT_EQ(operations_scalar(target, sql), 1);

    operations_archive_cleanup(archive);
    cbm_unlink(target);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_replace_publishes_only_requested_generation_atomically) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], fault_target[OPERATIONS_PATH_MAX];
    char archive[OPERATIONS_PATH_MAX], sql[512];
    operations_path(source, sizeof(source), "workspace-generation-replace-source");
    operations_path(target, sizeof(target), "workspace-generation-replace-target");
    operations_path(fault_target, sizeof(fault_target), "workspace-generation-fault-target");
    operations_archive_path(archive, sizeof(archive), "workspace-generation-replace");
    cbm_zova_workspace_generation_result_t archive_a = {0}, target_a = {0}, target_b = {0};
    ASSERT_EQ(operations_prepare_rich_archive(source, archive, 3, "{\"summary\":\"rich!\"}",
                                              &archive_a), 0);
    ASSERT_EQ(archive_a.generation, 3);
    ASSERT_EQ(operations_seed_collision_target(target, 1, "{\"summary\":\"rich!\"}",
                                               "/tmp/workspace-generation-resident", &target_a,
                                               &target_b), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, true, &report),
              CBM_ZOVA_OPERATION_OK);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_database_generation_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             archive_a.workspace_id);
    ASSERT_EQ(operations_scalar(target, sql), 2);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_database_generation_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s') "
             "AND generation=2",
             archive_a.workspace_id);
    ASSERT_EQ(operations_scalar(target, sql), 0);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_database_generation_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s') "
             "AND generation=3 AND state='ready'",
             archive_a.workspace_id);
    ASSERT_EQ(operations_scalar(target, sql), 1);

    cbm_zova_workspace_generation_result_t fault_a = {0}, fault_b = {0};
    ASSERT_EQ(operations_seed_collision_target(fault_target, 1, "{\"summary\":\"rich!\"}",
                                               "/tmp/workspace-generation-fault-resident",
                                               &fault_a, &fault_b), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(fault_target, before), 0);
    cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", "user_before_commit", 1);
    ASSERT_EQ(cbm_zova_workspace_import(fault_target, archive, true, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    ASSERT_EQ(operations_file_digest(fault_target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_database_generation_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             archive_a.workspace_id);
    ASSERT_EQ(operations_scalar(fault_target, sql), 1);
    snprintf(sql, sizeof(sql),
             "SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id='%s'",
             archive_a.workspace_id);
    ASSERT_EQ(operations_scalar(fault_target, sql), 1);

    operations_archive_cleanup(archive);
    cbm_unlink(fault_target);
    cbm_unlink(target);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_export_refuses_missing_and_unready_without_source_writes) {
    char source[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "workspace-export-intrinsic-refusal");
    operations_archive_path(archive, sizeof(archive), "workspace-export-intrinsic-refusal");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t rich = {0};
    ASSERT_EQ(operations_publish_rich_fixture(source, &rich), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(source, before), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_export(
                  source, "w1_00000000000000000000000000000000", archive, &report),
              CBM_ZOVA_OPERATION_INVALID);
    ASSERT_EQ(operations_file_digest(source, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_TRUE(access(archive, F_OK) != 0);

    char sql[512];
    snprintf(sql, sizeof(sql),
             "PRAGMA journal_mode=DELETE; UPDATE cbm_database_generation_v1 SET state='building' "
             "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
             "WHERE workspace_id='%s') AND generation=1", rich.workspace_id);
    ASSERT_EQ(operations_sql(source, sql), 0);
    ASSERT_EQ(operations_file_digest(source, before), 0);
    ASSERT_EQ(cbm_zova_workspace_export(source, rich.workspace_id, archive, &report),
              CBM_ZOVA_OPERATION_INVALID);
    ASSERT_EQ(operations_file_digest(source, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_TRUE(access(archive, F_OK) != 0);

    operations_archive_cleanup(archive);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_import_refuses_outer_archive_tampering_without_target_writes) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX];
    char archives[4][OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "workspace-import-outer-refusal-source");
    operations_path(target, sizeof(target), "workspace-import-outer-refusal-target");
    cbm_zova_workspace_generation_result_t rich = {0}, resident = {0};
    ASSERT_EQ(operations_publish_rich_fixture(source, &rich), 0);
    ASSERT_EQ(operations_publish_fixture(target, "/tmp/workspace-import-outer-target",
                                         "outer-target", 1, &resident), 0);
    cbm_zova_operation_report_t report = {0};
    for (int i = 0; i < 4; i++) {
        char name[64];
        snprintf(name, sizeof(name), "workspace-import-outer-refusal-%d", i);
        operations_archive_path(archives[i], sizeof(archives[i]), name);
        operations_archive_cleanup(archives[i]);
        ASSERT_EQ(cbm_zova_workspace_export(source, rich.workspace_id, archives[i], &report),
                  CBM_ZOVA_OPERATION_OK);
    }
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(target, before), 0);
    ASSERT_EQ(operations_manifest_replace_char(
                  archives[0], "\"archive_version\":1", strlen("\"archive_version\":"), '2'), 0);
    ASSERT_EQ(cbm_zova_workspace_import(target, archives[0], false, &report),
              CBM_ZOVA_OPERATION_INCOMPATIBLE);
    ASSERT_EQ(operations_file_digest(target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_manifest_replace_char(
                  archives[1], "\"schema_version\":7", strlen("\"schema_version\":"), '8'), 0);
    ASSERT_EQ(cbm_zova_workspace_import(target, archives[1], false, &report),
              CBM_ZOVA_OPERATION_INCOMPATIBLE);
    ASSERT_EQ(operations_file_digest(target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_manifest_replace_char(
                  archives[2], "\"sha256\":\"", strlen("\"sha256\":\""), '0'), 0);
    ASSERT_EQ(cbm_zova_workspace_import(target, archives[2], false, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    ASSERT_EQ(operations_file_digest(target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    char truncated[OPERATIONS_PATH_MAX + 64];
    snprintf(truncated, sizeof(truncated), "%s/data.zova", archives[3]);
    ASSERT_EQ(truncate(truncated, 128), 0);
    ASSERT_EQ(cbm_zova_workspace_import(target, archives[3], false, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    ASSERT_EQ(operations_file_digest(target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_scalar(target, "SELECT count(*) FROM cbm_workspace_registry"), 1);

    for (int i = 0; i < 4; i++) operations_archive_cleanup(archives[i]);
    cbm_unlink(target);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_import_refuses_workspace_root_identity_mismatch_without_target_writes) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    char data[OPERATIONS_PATH_MAX + 64];
    operations_path(source, sizeof(source), "workspace-import-root-refusal-source");
    operations_path(target, sizeof(target), "workspace-import-root-refusal-target");
    operations_archive_path(archive, sizeof(archive), "workspace-import-root-refusal");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t rich = {0}, resident = {0};
    ASSERT_EQ(operations_publish_rich_fixture(source, &rich), 0);
    ASSERT_EQ(operations_publish_fixture(target, "/tmp/workspace-import-root-target",
                                         "root-target", 1, &resident), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_export(source, rich.workspace_id, archive, &report),
              CBM_ZOVA_OPERATION_OK);
    snprintf(data, sizeof(data), "%s/data.zova", archive);
    ASSERT_EQ(operations_sql(data,
                            "PRAGMA journal_mode=DELETE; UPDATE cbm_workspace_registry SET "
                            "canonical_root='/tmp/workspace-rich-mismatched'"), 0);
    ASSERT_EQ(operations_refresh_archive_database_digest(archive), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(target, before), 0);
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, false, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    ASSERT_EQ(operations_file_digest(target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_scalar(target, "SELECT count(*) FROM cbm_workspace_registry"), 1);

    operations_archive_cleanup(archive);
    cbm_unlink(target);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_import_refuses_cross_workspace_native_names_without_target_writes) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    char data[OPERATIONS_PATH_MAX + 64], sql[4096];
    operations_path(source, sizeof(source), "workspace-import-native-refusal-source");
    operations_path(target, sizeof(target), "workspace-import-native-refusal-target");
    operations_archive_path(archive, sizeof(archive), "workspace-import-native-refusal");
    operations_archive_cleanup(archive);
    cbm_zova_workspace_generation_result_t rich = {0}, resident = {0};
    ASSERT_EQ(operations_publish_rich_fixture(source, &rich), 0);
    ASSERT_EQ(operations_publish_fixture(target, "/tmp/workspace-import-native-target",
                                         "native-target", 1, &resident), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_export(source, rich.workspace_id, archive, &report),
              CBM_ZOVA_OPERATION_OK);
    snprintf(data, sizeof(data), "%s/data.zova", archive);
    snprintf(sql, sizeof(sql),
             "PRAGMA journal_mode=DELETE; PRAGMA foreign_keys=OFF;"
             "UPDATE _zova_graphs SET name=replace(name,'%s','w1_00000000000000000000000000000000');"
             "UPDATE _zova_vector_collections SET name=replace(name,'%s','w1_00000000000000000000000000000000');",
             rich.workspace_id, rich.workspace_id);
    ASSERT_EQ(operations_sql(data, sql), 0);
    ASSERT_EQ(operations_refresh_archive_database_digest(archive), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(target, before), 0);
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, false, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    ASSERT_EQ(operations_file_digest(target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_scalar(target, "SELECT count(*) FROM cbm_workspace_registry"), 1);

    operations_archive_cleanup(archive);
    cbm_unlink(target);
    cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_import_refuses_existing_without_replace) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "workspace-collision-no-replace-source");
    operations_path(target, sizeof(target), "workspace-collision-no-replace-target");
    operations_archive_path(archive, sizeof(archive), "workspace-collision-no-replace");
    cbm_zova_workspace_generation_result_t archive_a = {0}, target_a = {0}, target_b = {0};
    ASSERT_EQ(operations_prepare_rich_archive(source, archive, 1, "{\"summary\":\"rich!\"}",
                                              &archive_a), 0);
    ASSERT_EQ(operations_seed_collision_target(target, 1, "{\"summary\":\"rich!\"}",
                                               "/tmp/workspace-collision-b-1", &target_a,
                                               &target_b), 0);
    operations_workspace_state_t before_b = {0}, after_b = {0};
    ASSERT_EQ(operations_workspace_state_capture(target, target_b.workspace_id, &before_b), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(target, before), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, false, &report),
              CBM_ZOVA_OPERATION_INVALID);
    ASSERT_STR_EQ(report.reason, "workspace_exists");
    ASSERT_EQ(operations_file_digest(target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_workspace_state_capture(target, target_b.workspace_id, &after_b), 0);
    ASSERT(operations_workspace_state_equal(&before_b, &after_b));
    operations_workspace_state_free(&after_b); operations_workspace_state_free(&before_b);
    operations_archive_cleanup(archive); cbm_unlink(target); cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_import_refuses_older_archive_with_replace) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "workspace-collision-older-source");
    operations_path(target, sizeof(target), "workspace-collision-older-target");
    operations_archive_path(archive, sizeof(archive), "workspace-collision-older");
    cbm_zova_workspace_generation_result_t archive_a = {0}, target_a = {0}, target_b = {0};
    ASSERT_EQ(operations_prepare_rich_archive(source, archive, 1, "{\"summary\":\"rich!\"}",
                                              &archive_a), 0);
    ASSERT_EQ(operations_seed_collision_target(target, 2, "{\"summary\":\"rich!\"}",
                                               "/tmp/workspace-collision-b-2", &target_a,
                                               &target_b), 0);
    operations_workspace_state_t before_b = {0}, after_b = {0};
    ASSERT_EQ(operations_workspace_state_capture(target, target_b.workspace_id, &before_b), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(target, before), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, true, &report),
              CBM_ZOVA_OPERATION_INCOMPATIBLE);
    ASSERT_STR_EQ(report.reason, "archive_generation_older");
    ASSERT_EQ(operations_file_digest(target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_workspace_state_capture(target, target_b.workspace_id, &after_b), 0);
    ASSERT(operations_workspace_state_equal(&before_b, &after_b));
    operations_workspace_state_free(&after_b); operations_workspace_state_free(&before_b);
    operations_archive_cleanup(archive); cbm_unlink(target); cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_import_refuses_same_generation_digest_mismatch) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "workspace-collision-digest-source");
    operations_path(target, sizeof(target), "workspace-collision-digest-target");
    operations_archive_path(archive, sizeof(archive), "workspace-collision-digest");
    cbm_zova_workspace_generation_result_t archive_a = {0}, target_a = {0}, target_b = {0};
    ASSERT_EQ(operations_prepare_rich_archive(source, archive, 1, "{\"summary\":\"rich!\"}",
                                              &archive_a), 0);
    ASSERT_EQ(operations_seed_collision_target(target, 1, "{\"summary\":\"different\"}",
                                               "/tmp/workspace-collision-b-3", &target_a,
                                               &target_b), 0);
    ASSERT_NEQ(strcmp(archive_a.metadata_sha256, target_a.metadata_sha256), 0);
    operations_workspace_state_t before_b = {0}, after_b = {0};
    ASSERT_EQ(operations_workspace_state_capture(target, target_b.workspace_id, &before_b), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(target, before), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, true, &report),
              CBM_ZOVA_OPERATION_VERIFY_FAILED);
    ASSERT_STR_EQ(report.reason, "same_generation_digest_mismatch");
    ASSERT_EQ(operations_file_digest(target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_workspace_state_capture(target, target_b.workspace_id, &after_b), 0);
    ASSERT(operations_workspace_state_equal(&before_b, &after_b));
    operations_workspace_state_free(&after_b); operations_workspace_state_free(&before_b);
    operations_archive_cleanup(archive); cbm_unlink(target); cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_import_noops_same_generation_identical) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "workspace-collision-noop-source");
    operations_path(target, sizeof(target), "workspace-collision-noop-target");
    operations_archive_path(archive, sizeof(archive), "workspace-collision-noop");
    cbm_zova_workspace_generation_result_t archive_a = {0}, target_a = {0}, target_b = {0};
    ASSERT_EQ(operations_prepare_rich_archive(source, archive, 1, "{\"summary\":\"rich!\"}",
                                              &archive_a), 0);
    ASSERT_EQ(operations_seed_collision_target(target, 1, "{\"summary\":\"rich!\"}",
                                               "/tmp/workspace-collision-b-4", &target_a,
                                               &target_b), 0);
    operations_workspace_state_t before_b = {0}, after_b = {0};
    ASSERT_EQ(operations_workspace_state_capture(target, target_b.workspace_id, &before_b), 0);
    uint8_t before[CBM_SHA256_DIGEST_LEN], after[CBM_SHA256_DIGEST_LEN];
    ASSERT_EQ(operations_file_digest(target, before), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, true, &report),
              CBM_ZOVA_OPERATION_NOOP);
    ASSERT_STR_EQ(report.reason, "already_current");
    ASSERT_EQ(operations_file_digest(target, after), 0);
    ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
    ASSERT_EQ(operations_workspace_state_capture(target, target_b.workspace_id, &after_b), 0);
    ASSERT(operations_workspace_state_equal(&before_b, &after_b));
    operations_workspace_state_free(&after_b); operations_workspace_state_free(&before_b);
    operations_archive_cleanup(archive); cbm_unlink(target); cbm_unlink(source);
    PASS();
}

TEST(zova_operations_workspace_import_replaces_with_newer_generation_only) {
    char source[OPERATIONS_PATH_MAX], target[OPERATIONS_PATH_MAX], archive[OPERATIONS_PATH_MAX];
    operations_path(source, sizeof(source), "workspace-collision-newer-source");
    operations_path(target, sizeof(target), "workspace-collision-newer-target");
    operations_archive_path(archive, sizeof(archive), "workspace-collision-newer");
    cbm_zova_workspace_generation_result_t archive_a = {0}, target_a = {0}, target_b = {0};
    ASSERT_EQ(operations_prepare_rich_archive(source, archive, 2, "{\"summary\":\"rich!\"}",
                                              &archive_a), 0);
    ASSERT_EQ(operations_seed_collision_target(target, 1, "{\"summary\":\"rich!\"}",
                                               "/tmp/workspace-collision-b-5", &target_a,
                                               &target_b), 0);
    operations_workspace_state_t before_b = {0}, after_b = {0};
    ASSERT_EQ(operations_workspace_state_capture(target, target_b.workspace_id, &before_b), 0);
    cbm_zova_operation_report_t report = {0};
    ASSERT_EQ(cbm_zova_workspace_import(target, archive, true, &report),
              CBM_ZOVA_OPERATION_OK);
    ASSERT_EQ(report.generation, 2);
    cbm_zova_workspace_snapshot_t imported = {0};
    ASSERT_EQ(cbm_zova_repository_export_snapshot(target, archive_a.workspace_id, &imported), 0);
    ASSERT_EQ(imported.generation, archive_a.generation);
    ASSERT_STR_EQ(imported.integrity.metadata_sha256, archive_a.metadata_sha256);
    ASSERT_STR_EQ(imported.integrity.fts_sha256, archive_a.fts_sha256);
    ASSERT_STR_EQ(imported.integrity.topology_sha256, archive_a.topology_sha256);
    ASSERT_STR_EQ(imported.integrity.node_vector_sha256, archive_a.node_vector_sha256);
    ASSERT_STR_EQ(imported.integrity.token_vector_sha256, archive_a.token_vector_sha256);
    ASSERT_EQ(operations_workspace_state_capture(target, target_b.workspace_id, &after_b), 0);
    ASSERT(operations_workspace_state_equal(&before_b, &after_b));
    cbm_zova_workspace_snapshot_free(&imported);
    operations_workspace_state_free(&after_b); operations_workspace_state_free(&before_b);
    operations_archive_cleanup(archive); cbm_unlink(target); cbm_unlink(source);
    PASS();
}
#endif

#if CBM_WITH_ZOVA
TEST(zova_operations_three_queued_publishers_keep_readers_generation_consistent) {
    char path[OPERATIONS_PATH_MAX];
    operations_path(path, sizeof(path), "three-publishers");
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);

    const CBMDumpNode a_nodes[] = {
        {.id = 1, .project = "queue-a", .label = "Function", .name = "root",
         .qualified_name = "queue_a.root_one", .file_path = "a1.c", .properties = "{}"},
        {.id = 2, .project = "queue-a", .label = "Function", .name = "root",
         .qualified_name = "queue_a.root_two", .file_path = "a2.c", .properties = "{}"},
    };
    const CBMDumpNode b_nodes[] = {
        {.id = 1, .project = "queue-b", .label = "Function", .name = "root",
         .qualified_name = "queue_b.root_one", .file_path = "b1.c", .properties = "{}"},
        {.id = 2, .project = "queue-b", .label = "Function", .name = "root",
         .qualified_name = "queue_b.root_two", .file_path = "b2.c", .properties = "{}"},
    };
    const CBMDumpNode c_nodes[] = {
        {.id = 1, .project = "queue-c", .label = "Function", .name = "root",
         .qualified_name = "queue_c.root_one", .file_path = "c1.c", .properties = "{}"},
    };
    const CBMDumpEdge a_edges[] = {
        {.id = 1, .project = "queue-a", .source_id = 1, .target_id = 2,
         .type = "CALLS", .properties = "{}", .url_path = "", .local_name = ""},
    };
    const CBMDumpEdge b_edges[] = {
        {.id = 1, .project = "queue-b", .source_id = 1, .target_id = 2,
         .type = "CALLS", .properties = "{}", .url_path = "", .local_name = ""},
    };
    const int8_t vector_one[] = {127, 0};
    const int8_t vector_two[] = {0, 127};
    const CBMDumpVector a_node_vectors[] = {
        {.node_id = 1, .project = "queue-a", .vector = (const uint8_t *)vector_one,
         .vector_len = 2},
        {.node_id = 2, .project = "queue-a", .vector = (const uint8_t *)vector_two,
         .vector_len = 2},
    };
    const CBMDumpVector b_node_vectors[] = {
        {.node_id = 1, .project = "queue-b", .vector = (const uint8_t *)vector_one,
         .vector_len = 2},
        {.node_id = 2, .project = "queue-b", .vector = (const uint8_t *)vector_two,
         .vector_len = 2},
    };
    const CBMDumpVector c_node_vectors[] = {
        {.node_id = 1, .project = "queue-c", .vector = (const uint8_t *)vector_one,
         .vector_len = 2},
    };
    const CBMDumpTokenVec a_token_vectors[] = {
        {.id = 1, .project = "queue-a", .token = "root-one",
         .vector = (const uint8_t *)vector_one, .vector_len = 2, .idf = 1.0f},
        {.id = 2, .project = "queue-a", .token = "root-two",
         .vector = (const uint8_t *)vector_two, .vector_len = 2, .idf = 1.0f},
    };
    const CBMDumpTokenVec b_token_vectors[] = {
        {.id = 1, .project = "queue-b", .token = "root-one",
         .vector = (const uint8_t *)vector_one, .vector_len = 2, .idf = 1.0f},
        {.id = 2, .project = "queue-b", .token = "root-two",
         .vector = (const uint8_t *)vector_two, .vector_len = 2, .idf = 1.0f},
    };
    const CBMDumpTokenVec c_token_vectors[] = {
        {.id = 1, .project = "queue-c", .token = "root-one",
         .vector = (const uint8_t *)vector_one, .vector_len = 2, .idf = 1.0f},
    };
    cbm_zova_workspace_generation_input_t inputs[] = {
        {.root_path = "/tmp/operations-queue-a", .project = "queue-a",
         .indexed_at = "2026-07-13T00:00:01Z",
         .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 2,
         .nodes = a_nodes, .node_count = 2, .edges = a_edges, .edge_count = 1,
         .node_vectors = a_node_vectors, .node_vector_count = 2,
         .token_vectors = a_token_vectors, .token_vector_count = 2},
        {.root_path = "/tmp/operations-queue-b", .project = "queue-b",
         .indexed_at = "2026-07-13T00:00:02Z",
         .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 2,
         .nodes = b_nodes, .node_count = 2, .edges = b_edges, .edge_count = 1,
         .node_vectors = b_node_vectors, .node_vector_count = 2,
         .token_vectors = b_token_vectors, .token_vector_count = 2},
        {.root_path = "/tmp/operations-queue-c", .project = "queue-c",
         .indexed_at = "2026-07-13T00:00:03Z",
         .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 2,
         .nodes = c_nodes, .node_count = 1,
         .node_vectors = c_node_vectors, .node_vector_count = 1,
         .token_vectors = c_token_vectors, .token_vector_count = 1},
    };
    cbm_zova_workspace_generation_input_t seed = inputs[0];
    seed.nodes = a_nodes;
    seed.node_count = 1;
    seed.edges = NULL;
    seed.edge_count = 0;
    seed.node_vector_count = 1;
    seed.token_vector_count = 1;
    cbm_zova_workspace_generation_result_t seeded_a = {0}, seeded_b = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &seed, &seeded_a), 0);
    seed = inputs[1];
    seed.nodes = b_nodes;
    seed.node_count = 1;
    seed.edges = NULL;
    seed.edge_count = 0;
    seed.node_vector_count = 1;
    seed.token_vector_count = 1;
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &seed, &seeded_b), 0);
    ASSERT_EQ(seeded_a.generation, 1);
    ASSERT_EQ(seeded_b.generation, 1);

    cbm_mcp_server_t *server = cbm_mcp_server_new();
    ASSERT_NOT_NULL(server);
    _Atomic int entry_release[3], publisher_release, publisher_held, entry_count;
    for (int i = 0; i < 3; ++i) atomic_init(&entry_release[i], 0);
    atomic_init(&publisher_release, 0);
    atomic_init(&publisher_held, 0);
    atomic_init(&entry_count, 0);
    char entry_log[4] = {0};
    operations_queue_publisher_t publishers[3] = {
        {.server = server, .path = path, .label = 'A', .input = inputs[0],
         .entry_release = &entry_release[0], .publisher_release = &publisher_release,
         .publisher_held = &publisher_held, .entry_count = &entry_count,
         .entry_log = entry_log},
        {.server = server, .path = path, .label = 'B', .input = inputs[1],
         .entry_release = &entry_release[1], .entry_count = &entry_count,
         .entry_log = entry_log},
        {.server = server, .path = path, .label = 'C', .input = inputs[2],
         .entry_release = &entry_release[2], .entry_count = &entry_count,
         .entry_log = entry_log},
    };
    /* Each iteration owns an independent read-only repository connection. A and B expose
     * their preceding complete generations while A's replacement is uncommitted; C has no
     * committed generation yet. Direct repository reads cannot fall back to a project DB. */
    ASSERT_EQ(operations_run_three_publishers(publishers, entry_release, &publisher_release,
                                              &publisher_held, &entry_count),
              0);

    ASSERT_STR_EQ(entry_log, "ABC");
    ASSERT_EQ(publishers[0].result.generation, 2);
    ASSERT_EQ(publishers[1].result.generation, 2);
    ASSERT_EQ(publishers[2].result.generation, 1);
    ASSERT_EQ(operations_repository_inventory(path, "queue-a", 2, 2, 1, 2, 2), 0);
    ASSERT_EQ(operations_repository_inventory(path, "queue-b", 2, 2, 1, 2, 2), 0);
    ASSERT_EQ(operations_repository_inventory(path, "queue-c", 1, 1, 0, 1, 1), 0);

    cbm_mcp_server_free(server);
    char sidecar[OPERATIONS_PATH_MAX + 16];
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path); cbm_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path); cbm_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s.writer.lock", path); cbm_unlink(sidecar);
    cbm_unlink(path);
    PASS();
}
#endif

SUITE(zova_operations) {
    RUN_TEST(zova_operations_report_contract_and_code_names_are_stable);
#if CBM_WITH_ZOVA && !defined(_WIN32)
    RUN_TEST(zova_operations_database_status_reports_exact_checked_disk_accounting);
    RUN_TEST(zova_operations_database_status_refuses_untrusted_inputs_without_writes);
    RUN_TEST(zova_operations_delete_primitive_removes_health_before_registry);
    RUN_TEST(zova_operations_workspace_delete_requires_exact_confirmation_without_writes);
    RUN_TEST(zova_operations_workspace_delete_is_isolated_reported_and_idempotent);
    RUN_TEST(zova_operations_refuse_while_index_coordination_is_held);
    RUN_TEST(zova_operations_migrate_format_upgrades_format9_database);
    RUN_TEST(zova_operations_migrate_format_upgrades_format12_database);
    RUN_TEST(zova_operations_database_compact_noops_below_policy_threshold);
    RUN_TEST(zova_operations_database_compact_reclaims_verified_database);
    RUN_TEST(zova_operations_database_compact_preserves_two_workspace_public_state);
    RUN_TEST(zova_operations_database_compact_refuses_disk_and_active_writer_without_writes);
    RUN_TEST(zova_operations_database_compact_refuses_checkpoint_blocked_reader_then_retries);
    RUN_TEST(zova_operations_database_compact_fault_phases_retry_to_verified_live_database);
    RUN_TEST(zova_operations_database_compact_keeps_open_committed_reader_valid);
    RUN_TEST(zova_operations_health_classifies_workspace_defect_without_cross_workspace_damage);
    RUN_TEST(zova_operations_health_classifies_shared_schema_and_truncated_file_as_whole_file);
    RUN_TEST(zova_operations_health_classifies_failed_quick_check_as_whole_file);
    RUN_TEST(zova_operations_health_classifies_foreign_key_violation_as_whole_file);
    RUN_TEST(zova_operations_health_marks_workspace_when_native_vector_collection_is_missing);
    RUN_TEST(zova_operations_health_detects_workspace_digest_mismatch);
    RUN_TEST(zova_operations_rebuild_health_clears_only_with_successful_publication);
    RUN_TEST(zova_operations_workspace_recover_runs_full_source_rebuild_and_retries);
    RUN_TEST(zova_operations_whole_file_corruption_recovers_only_through_verified_restore);
    RUN_TEST(zova_operations_recover_does_not_quarantine_a_busy_database);
    RUN_TEST(zova_operations_workspace_recover_quarantines_whole_file_without_replacement);
#endif
    RUN_TEST(zova_operations_empty_bootstrap_creates_exact_v7_health_schema);
    RUN_TEST(zova_operations_pre_v7_init_requires_repack_without_writes);
    RUN_TEST(zova_operations_v7_init_is_idempotent);
    RUN_TEST(zova_operations_malformed_v7_shape_is_rejected_without_writes);
    RUN_TEST(zova_operations_malformed_v4_and_future_v8_fail_closed_without_writes);
    RUN_TEST(zova_operations_interrupted_v7_bootstrap_rolls_back_and_retries_once);
#if CBM_WITH_ZOVA
    RUN_TEST(zova_operations_repository_snapshot_is_owned_and_workspace_filtered);
    RUN_TEST(zova_operations_workspace_export_is_fresh_rich_and_single_workspace);
    RUN_TEST(zova_operations_workspace_manifest_binds_complete_inventory_and_escapes_identity);
    RUN_TEST(zova_operations_workspace_import_refuses_each_manifest_binding_without_target_writes);
    RUN_TEST(zova_operations_workspace_import_adds_rich_workspace_without_touching_resident);
    RUN_TEST(zova_operations_workspace_import_preserves_parse_coverage);
    RUN_TEST(zova_operations_workspace_export_and_empty_import_publish_generation_three_once);
    RUN_TEST(zova_operations_workspace_import_refuses_while_index_coordination_is_held);
    RUN_TEST(zova_operations_workspace_replace_publishes_only_requested_generation_atomically);
    RUN_TEST(zova_operations_workspace_export_refuses_missing_and_unready_without_source_writes);
    RUN_TEST(zova_operations_workspace_import_refuses_outer_archive_tampering_without_target_writes);
    RUN_TEST(zova_operations_workspace_import_refuses_workspace_root_identity_mismatch_without_target_writes);
    RUN_TEST(zova_operations_workspace_import_refuses_cross_workspace_native_names_without_target_writes);
    RUN_TEST(zova_operations_workspace_import_refuses_existing_without_replace);
    RUN_TEST(zova_operations_workspace_import_refuses_older_archive_with_replace);
    RUN_TEST(zova_operations_workspace_import_refuses_same_generation_digest_mismatch);
    RUN_TEST(zova_operations_workspace_import_noops_same_generation_identical);
    RUN_TEST(zova_operations_workspace_import_replaces_with_newer_generation_only);
    RUN_TEST(zova_operations_online_backup_excludes_held_wal_generation);
    RUN_TEST(zova_operations_backup_refuses_symlink_source_without_creating_destination);
    RUN_TEST(zova_operations_restore_requires_confirmation_and_replaces_verified_inventory);
    RUN_TEST(zova_operations_restore_fault_phases_retry_to_verified_live_database);
    RUN_TEST(zova_operations_restore_ignores_stale_live_wal);
    RUN_TEST(zova_operations_database_archive_is_deterministic_and_round_trips);
    RUN_TEST(zova_operations_database_archive_refuses_untrusted_inputs_without_live_writes);
    RUN_TEST(zova_operations_database_archive_rejects_unexpected_top_level_members);
    RUN_TEST(zova_operations_three_queued_publishers_keep_readers_generation_consistent);
#endif
}
