#ifndef CBM_ZOVA_REPOSITORY_H
#define CBM_ZOVA_REPOSITORY_H

#include "store/store.h"
#include "zova/cbm_zova.h"

typedef struct cbm_zova_repository cbm_zova_repository_t;
typedef struct cbm_zova_catalog cbm_zova_catalog_t;
typedef struct cbm_gbuf cbm_gbuf_t;

typedef struct {
    int64_t workspace_key;
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char *selector;
    char *project;
    char *root_path;
    char *indexed_at;
    char *model_fingerprint;
    char *health_reason;
    int vector_dimensions;
    int64_t generation;
    bool ready;
    bool healthy;
} cbm_zova_catalog_project_t;

typedef struct {
    cbm_zova_catalog_project_t *projects;
    int count;
    cbm_zova_catalog_project_t *excluded_projects;
    int excluded_count;
} cbm_zova_catalog_scope_t;

enum {
    CBM_ZOVA_CATALOG_STALE_SCOPE = -3,
    CBM_ZOVA_CATALOG_INCOMPATIBLE_MODELS = -4,
};

typedef uint32_t cbm_zova_snapshot_components_t;

enum {
    CBM_ZOVA_SNAPSHOT_COMPONENT_NONE = 0,
    CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY = 1u << 0,
    CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS = 1u << 1,
    CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS = 1u << 2,
    CBM_ZOVA_SNAPSHOT_COMPONENT_ALL =
        CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY |
        CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS |
        CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS,
};

typedef enum {
    CBM_ZOVA_SNAPSHOT_OK = 0,
    CBM_ZOVA_SNAPSHOT_ERROR = -1,
    CBM_ZOVA_SNAPSHOT_STALE = -2,
} cbm_zova_snapshot_status_t;

enum {
    CBM_ZOVA_SNAPSHOT_BASE_PHASE_OPEN = 1u << 0,
    CBM_ZOVA_SNAPSHOT_BASE_PHASE_HEADER = 1u << 1,
    CBM_ZOVA_SNAPSHOT_BASE_PHASE_INTEGRITY = 1u << 2,
    CBM_ZOVA_SNAPSHOT_BASE_PHASE_NODES = 1u << 3,
    CBM_ZOVA_SNAPSHOT_BASE_PHASE_EDGES = 1u << 4,
    CBM_ZOVA_SNAPSHOT_BASE_PHASE_HASHES_SUMMARY = 1u << 5,
    CBM_ZOVA_SNAPSHOT_BASE_PHASE_CLOSE = 1u << 6,
    CBM_ZOVA_SNAPSHOT_BASE_PHASE_ALL = (1u << 7) - 1,
};

typedef struct {
    double base_ms;
    double optional_ms;
    double open_ms;
    double header_ms;
    double integrity_ms;
    double nodes_sql_ms;
    double nodes_native_ms;
    double nodes_finalize_ms;
    double edges_sql_ms;
    double edges_native_ms;
    double edges_finalize_ms;
    double hashes_summary_ms;
    double close_ms;
    double graph_buffer_ms;
    uint32_t base_phase_mask;
    uint64_t node_rows;
    uint64_t edge_rows;
    uint64_t edge_scan_pages;
    uint64_t edge_native_rows;
    uint64_t edge_logical_rows;
    uint64_t edge_keyed_read_count;
    uint64_t edge_string_arena_chunks;
    uint64_t edge_string_arena_bytes;
    uint64_t file_hash_rows;
    cbm_zova_snapshot_components_t hydrated_components;
    uint64_t topology_rows;
    uint64_t node_vector_rows;
    uint64_t token_vector_rows;
} cbm_zova_snapshot_metrics_t;

typedef struct {
    char *source_stable_id;
    char *edge_type;
    char *target_stable_id;
} cbm_zova_snapshot_topology_edge_t;

typedef struct cbm_zova_workspace_snapshot {
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char *root_path;
    char *project;
    char *indexed_at;
    char *model_fingerprint;
    int vector_dimensions;
    int64_t generation;
    CBMDumpNode *nodes;
    char **node_stable_ids;
    int64_t *zova_node_keys;
    uint64_t *node_source_ordinals;
    int node_count;
    CBMDumpEdge *edges;
    char **edge_ids;
    int edge_count;
    void *edge_string_arena;
    cbm_zova_snapshot_topology_edge_t *topology_edges;
    int topology_edge_count;
    CBMDumpVector *node_vectors;
    char **node_vector_ids;
    int node_vector_count;
    CBMDumpTokenVec *token_vectors;
    char **token_vector_ids;
    int token_vector_count;
    cbm_zova_file_hash_input_t *file_hashes;
    int file_hash_count;
    cbm_zova_project_summary_input_t project_summary;
    cbm_zova_workspace_generation_result_t integrity;
    cbm_zova_snapshot_components_t hydrated_components;
    cbm_zova_snapshot_metrics_t metrics;
} cbm_zova_workspace_snapshot_t;

cbm_zova_repository_t *cbm_zova_repository_open(const char *path, const char *project);
void cbm_zova_repository_close(cbm_zova_repository_t *repo);
const char *cbm_zova_repository_workspace_id(const cbm_zova_repository_t *repo);
int cbm_zova_repository_get_project(cbm_zova_repository_t *repo, const char *workspace_id,
                                    cbm_project_t *out);
int cbm_zova_repository_find_node_by_qn(cbm_zova_repository_t *repo, const char *workspace_id,
                                         const char *qualified_name, cbm_node_t *out);
int cbm_zova_repository_find_node_by_stable_id(cbm_zova_repository_t *repo,
                                                const char *workspace_id,
                                                const char *stable_id, cbm_node_t *out);
int cbm_zova_repository_find_nodes_by_stable_ids(cbm_zova_repository_t *repo,
                                                  const char *workspace_id,
                                                  const char *const *stable_ids,
                                                  int count, cbm_node_t **out);
int cbm_zova_repository_find_nodes_by_keys(cbm_zova_repository_t *repo,
                                           const char *workspace_id,
                                           const int64_t *node_keys,
                                           int count, cbm_node_t **out);
int cbm_zova_repository_find_node_by_numeric_id(cbm_zova_repository_t *repo,
                                                 const char *workspace_id,
                                                 int64_t numeric_id, cbm_node_t *out);
int cbm_zova_repository_find_nodes_by_file_overlap(cbm_zova_repository_t *repo,
                                                    const char *workspace_id,
                                                    const char *file_path,
                                                    int start_line, int end_line,
                                                    cbm_node_t **out, int *out_count);
int cbm_zova_repository_stable_id_for_numeric_id(cbm_zova_repository_t *repo,
                                                  const char *workspace_id,
                                                  int64_t numeric_id, char *out,
                                                  size_t out_size);
int cbm_zova_repository_find_edges(cbm_zova_repository_t *repo, const char *workspace_id,
                                   const char *stable_id,
                                   const char *direction, cbm_edge_t **out, int *out_count);
/* Zova node keys of the nodes that call (inbound) or are called by (outbound)
 * stable_id over CALLS, HTTP_CALLS or ASYNC_CALLS. Caller frees *out. */
int cbm_zova_repository_call_neighbor_keys(cbm_zova_repository_t *repo, const char *workspace_id,
                                           const char *stable_id, bool inbound, int64_t **out,
                                           int *out_count);
/* One cbm edge of the workspace graph; endpoints are Zova node keys. The
 * pointers are borrowed for the call. Return nonzero to stop with an error. */
typedef int (*cbm_zova_repository_edge_visitor_t)(int64_t source_key, int64_t target_key,
                                                  const char *type, size_t type_len,
                                                  const uint8_t *properties,
                                                  size_t properties_len, void *context);
/* Visit every cbm edge in graph scan order, or only those of edge_type
 * (NULL for all). */
int cbm_zova_repository_visit_edges(cbm_zova_repository_t *repo, const char *workspace_id,
                                    const char *edge_type,
                                    cbm_zova_repository_edge_visitor_t visitor, void *context);
/* Every cbm edge of edge_type in graph scan order. Caller frees *out. */
int cbm_zova_repository_find_edges_by_type(cbm_zova_repository_t *repo, const char *workspace_id,
                                           const char *edge_type, cbm_edge_t **out,
                                           int *out_count);
/* Identity rows for graph comparison. Strings are owned by the arrays. */
typedef struct {
    int64_t node_key;
    char *qualified_name;
    char *label;
    char *file_path;
} cbm_zova_identity_node_t;

typedef struct {
    int64_t source_key;
    int64_t target_key;
    char *type;
    char *local_name; /* IMPORTS local name, "" for other edge types */
} cbm_zova_identity_edge_t;

/* Every node (ordered by node key) and every cbm edge record of the opened
 * generation. Fails with CBM_STORE_SCAN_LIMIT, returning nothing, when more
 * than max_nodes nodes or max_edges edges exist. Free both arrays with
 * cbm_zova_identities_free. */
int cbm_zova_repository_graph_identities(cbm_zova_repository_t *repo, const char *workspace_id,
                                         uint64_t max_nodes, uint64_t max_edges,
                                         cbm_zova_identity_node_t **out_nodes, int *node_count,
                                         cbm_zova_identity_edge_t **out_edges, int *edge_count);
void cbm_zova_identities_free(cbm_zova_identity_node_t *nodes, int node_count,
                              cbm_zova_identity_edge_t *edges, int edge_count);
int cbm_zova_repository_search(cbm_zova_repository_t *repo, const char *workspace_id,
                               const cbm_search_params_t *params, cbm_search_output_t *out);
/* Full-text search. file_pattern (a path glob) and label narrow the matches;
 * nodes named exactly exact_name (case-insensitive) rank first. Each of the
 * three may be NULL. */
int cbm_zova_repository_search_fts(cbm_zova_repository_t *repo, const char *workspace_id,
                                   const char *query,
                                   const char *file_pattern, const char *label,
                                   const char *exact_name, int limit, int offset,
                                   cbm_search_output_t *out);
int cbm_zova_repository_index_status(cbm_zova_repository_t *repo, const char *workspace_id,
                                      int64_t *generation,
                                      char **indexed_at);
int cbm_zova_repository_counts(cbm_zova_repository_t *repo, const char *workspace_id,
                               int *nodes, int *edges);

/* One indexed file of the active generation: its File node properties (NULL
 * when the file has no File node) and its stored filesystem metadata. Parse
 * coverage lives in those properties; see CBM_COVERAGE_* in pipeline.h. */
typedef struct {
    char *file_path;
    char *properties;
    bool has_hash;
    int64_t mtime_ns;
    int64_t size_bytes;
} cbm_zova_file_coverage_t;

/* Read the workspace's indexed files ordered by path, in one statement so the
 * rows come from a single generation. exact_path limits the read to that file.
 * out_recorded (optional) says whether the generation was stamped with
 * coverage (Project node "coverage_version").
 * out_root_path (optional) receives the workspace root, out_generation
 * (optional) the generation the repository opened. */
int cbm_zova_repository_file_coverage(cbm_zova_repository_t *repo, const char *workspace_id,
                                      const char *exact_path, cbm_zova_file_coverage_t **out,
                                      int *count, char **out_root_path, int64_t *out_generation,
                                      bool *out_recorded);
void cbm_zova_file_coverage_free(cbm_zova_file_coverage_t *rows, int count);
int cbm_zova_repository_project_summary(cbm_zova_repository_t *repo,
                                        const char *workspace_id,
                                        cbm_project_summary_export_t *out);
int cbm_zova_repository_export_snapshot(const char *zova_path, const char *workspace_id,
                                        cbm_zova_workspace_snapshot_t *out);
/* Export the complete committed baseline needed to calculate an exact
 * incremental delta, including the authoritative native vector payloads. */
int cbm_zova_repository_export_incremental_snapshot(
    const char *zova_path, const char *workspace_id, cbm_zova_workspace_snapshot_t *out);
int cbm_zova_repository_export_incremental_snapshot_to_graph(
    const char *zova_path, const char *workspace_id, cbm_zova_workspace_snapshot_t *out,
    cbm_gbuf_t *graph);
int cbm_zova_repository_hydrate_incremental_components(
    const char *zova_path, const char *workspace_id, int64_t expected_generation,
    cbm_zova_snapshot_components_t requested, cbm_zova_workspace_snapshot_t *snapshot);
int cbm_zova_workspace_snapshot_format_edge_id(
    const cbm_zova_workspace_snapshot_t *snapshot, int edge_index,
    char *out, size_t out_size);
void cbm_zova_workspace_snapshot_free(cbm_zova_workspace_snapshot_t *snapshot);

cbm_zova_catalog_t *cbm_zova_catalog_open(const char *path);
void cbm_zova_catalog_close(cbm_zova_catalog_t *catalog);
int cbm_zova_catalog_list(cbm_zova_catalog_t *catalog, cbm_zova_catalog_scope_t *out);
const char *cbm_zova_catalog_error(const cbm_zova_catalog_t *catalog);
int cbm_zova_catalog_resolve(cbm_zova_catalog_t *catalog, const char *const *selectors,
                             int selector_count, bool wildcard,
                             cbm_zova_catalog_scope_t *out);
int cbm_zova_catalog_search_fts(cbm_zova_catalog_t *catalog,
                                const cbm_zova_catalog_scope_t *scope,
                                const char *query, const char *file_pattern,
                                const char *label, const char *exact_name,
                                int limit, int offset, cbm_search_output_t *out);
int cbm_zova_catalog_search(cbm_zova_catalog_t *catalog,
                            const cbm_zova_catalog_scope_t *scope,
                            const cbm_search_params_t *params, cbm_search_output_t *out);
int cbm_zova_catalog_search_semantic(
    cbm_zova_catalog_t *catalog, const char *path, const cbm_zova_catalog_scope_t *scope,
    const char **keywords, int keyword_count, int limit, int offset,
    cbm_vector_result_t **out, int *out_count, int *out_total);
void cbm_zova_catalog_scope_free(cbm_zova_catalog_scope_t *scope);

#endif
