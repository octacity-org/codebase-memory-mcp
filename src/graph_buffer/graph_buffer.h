/*
 * graph_buffer.h — In-memory graph buffer for pipeline indexing.
 *
 * Holds all nodes and edges in RAM during indexing, then dumps to SQLite.
 * Provides O(1) node lookup by qualified name and edge dedup by key.
 *
 * Depends on: foundation (hash_table, dyn_array), store (data structs)
 */
#ifndef CBM_GRAPH_BUFFER_H
#define CBM_GRAPH_BUFFER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "zova/cbm_zova.h"

/* ── Opaque handle ──────────────────────────────────────────────── */

typedef struct cbm_gbuf cbm_gbuf_t;

/* Forward declare store for dump path */
typedef struct cbm_store cbm_store_t;
typedef struct cbm_zova_workspace_snapshot cbm_zova_workspace_snapshot_t;

/* ── Node / Edge structs (owned by the buffer) ───────────────────── */

typedef struct {
    int64_t id;           /* temp ID (sequential from 1) */
    char *label;          /* heap-owned */
    char *name;           /* heap-owned */
    char *qualified_name; /* heap-owned */
    char *file_path;      /* heap-owned */
    int start_line;
    int end_line;
    char *properties_json; /* heap-owned JSON string, "{}" default */
} cbm_gbuf_node_t;

typedef struct {
    int64_t id;            /* temp ID */
    int64_t source_id;     /* temp node ID */
    int64_t target_id;     /* temp node ID */
    char *type;            /* heap-owned */
    char *properties_json; /* heap-owned JSON string, "{}" default */
} cbm_gbuf_edge_t;

/* ── Lifecycle ──────────────────────────────────────────────────── */

/* Create a new graph buffer for a project. */
cbm_gbuf_t *cbm_gbuf_new(const char *project, const char *root_path);

/* Create a graph buffer with a shared atomic ID source.
 * IDs are allocated via atomic_fetch_add on *id_source.
 * Used for parallel extraction where multiple gbufs need unique IDs.
 * If id_source is NULL, behaves like cbm_gbuf_new(). */
cbm_gbuf_t *cbm_gbuf_new_shared_ids(const char *project, const char *root_path,
                                    _Atomic int64_t *id_source);

/* Pre-size the primary node/edge storage for a known snapshot. Never shrinks.
 * Returns 0 on success, -1 for invalid input or allocation failure. */
int cbm_gbuf_reserve(cbm_gbuf_t *gb, int node_count, int edge_count);

/* Free the graph buffer and all owned data. NULL-safe. */
void cbm_gbuf_free(cbm_gbuf_t *gb);

/* Merge all nodes and edges from src into dst.
 * Nodes are merged by QN: on collision, src wins (updates dst node fields).
 * New nodes are inserted with their original IDs (from shared ID source).
 * Edges are remapped for any QN-colliding nodes, then inserted with dedup.
 * After merge, src can be safely freed (all data is copied).
 * Returns 0 on success, -1 on error. */
int cbm_gbuf_merge(cbm_gbuf_t *dst, cbm_gbuf_t *src);

/* ── Node operations ─────────────────────────────────────────────── */

/* Upsert a node by qualified name. Returns the temp ID.
 * All string fields are copied (buffer owns the copies).
 * Returns 0 on error. */
int64_t cbm_gbuf_upsert_node(cbm_gbuf_t *gb, const char *label, const char *name,
                             const char *qualified_name, const char *file_path, int start_line,
                             int end_line, const char *properties_json);

/* Replace an existing node's properties_json by qualified name (copied).
 * Bypasses the upsert survivor rule; used to stamp parse coverage on File
 * nodes. Returns true when the node was found and updated. */
bool cbm_gbuf_set_node_props(cbm_gbuf_t *gb, const char *qualified_name,
                             const char *properties_json);

/* Find a node by qualified name. Returns NULL if not found. */
const cbm_gbuf_node_t *cbm_gbuf_find_by_qn(const cbm_gbuf_t *gb, const char *qn);

/* Find a node by temp ID. Returns NULL if not found. */
const cbm_gbuf_node_t *cbm_gbuf_find_by_id(const cbm_gbuf_t *gb, int64_t id);

/* Find nodes by label. Sets *out and *count. Caller does NOT free.
 * Returns 0 on success, -1 on error. */
int cbm_gbuf_find_by_label(const cbm_gbuf_t *gb, const char *label, const cbm_gbuf_node_t ***out,
                           int *count);

/* Find nodes by name (exact). Sets *out and *count. Caller does NOT free. */
int cbm_gbuf_find_by_name(const cbm_gbuf_t *gb, const char *name, const cbm_gbuf_node_t ***out,
                          int *count);

/* Count total nodes in buffer. */
int cbm_gbuf_node_count(const cbm_gbuf_t *gb);

/* Get the next ID that would be assigned. Used to initialize shared atomic counters. */
int64_t cbm_gbuf_next_id(const cbm_gbuf_t *gb);

/* Set the next ID counter exactly. Can lower it; to sync after merging worker
 * gbufs use cbm_gbuf_raise_next_id instead. */
void cbm_gbuf_set_next_id(cbm_gbuf_t *gb, int64_t next_id);

/* Raise the next ID counter to at least next_id. Never lowers it, so no id
 * already handed out comes back. Used to sync after merging worker gbufs. */
void cbm_gbuf_raise_next_id(cbm_gbuf_t *gb, int64_t next_id);

/* Number of edges whose id an earlier edge already carries (0 = ids unique,
 * CBM_NOT_FOUND on allocation failure). */
int cbm_gbuf_duplicate_edge_ids(const cbm_gbuf_t *gb);

/* Delete all nodes with a label. Cascade-deletes referencing edges. */
int cbm_gbuf_delete_by_label(cbm_gbuf_t *gb, const char *label);

/* Delete all nodes for a given file path. Cascade-deletes referencing edges.
 * Used by incremental indexing to remove stale nodes before re-extraction. */
int cbm_gbuf_delete_by_file(cbm_gbuf_t *gb, const char *file_path);
int cbm_gbuf_delete_empty_folders(cbm_gbuf_t *gb);
int cbm_gbuf_delete_empty_folders_under(cbm_gbuf_t *gb, const char *const *affected_paths,
                                        int affected_count);

/* Iterate all live nodes (not deleted from QN index). */
typedef void (*cbm_gbuf_node_visitor_fn)(const cbm_gbuf_node_t *node, void *userdata);
void cbm_gbuf_foreach_node(const cbm_gbuf_t *gb, cbm_gbuf_node_visitor_fn fn, void *userdata);

/* Iterate all edges. */
typedef void (*cbm_gbuf_edge_visitor_fn)(const cbm_gbuf_edge_t *edge, void *userdata);
void cbm_gbuf_foreach_edge(const cbm_gbuf_t *gb, cbm_gbuf_edge_visitor_fn fn, void *userdata);

/* ── Edge operations ─────────────────────────────────────────────── */

/* Insert an edge. Deduplicates by (source_id, target_id, type).
 * On duplicate, merges properties (later wins). Returns edge temp ID.
 * Returns 0 on error. */
int64_t cbm_gbuf_insert_edge(cbm_gbuf_t *gb, int64_t source_id, int64_t target_id, const char *type,
                             const char *properties_json);

/* Find edges from source_id with given type.
 * Sets *out and *count. Caller does NOT free. */
int cbm_gbuf_find_edges_by_source_type(const cbm_gbuf_t *gb, int64_t source_id, const char *type,
                                       const cbm_gbuf_edge_t ***out, int *count);

/* Find edges to target_id with given type. */
int cbm_gbuf_find_edges_by_target_type(const cbm_gbuf_t *gb, int64_t target_id, const char *type,
                                       const cbm_gbuf_edge_t ***out, int *count);

/* Find all edges of a given type. */
int cbm_gbuf_find_edges_by_type(const cbm_gbuf_t *gb, const char *type,
                                const cbm_gbuf_edge_t ***out, int *count);

/* Count total edges. */
int cbm_gbuf_edge_count(const cbm_gbuf_t *gb);

/* Count edges of a given type. */
int cbm_gbuf_edge_count_by_type(const cbm_gbuf_t *gb, const char *type);

/* Delete all edges of a type. */
int cbm_gbuf_delete_edges_by_type(cbm_gbuf_t *gb, const char *type);
int cbm_gbuf_delete_edges_by_source_type(cbm_gbuf_t *gb, int64_t source_id, const char *type);

/* Delete every edge of a type for which `keep` returns false. One pass. */
typedef bool (*cbm_gbuf_edge_keep_fn)(const cbm_gbuf_t *gb, const cbm_gbuf_edge_t *edge,
                                      void *userdata);
int cbm_gbuf_delete_edges_by_type_unless(cbm_gbuf_t *gb, const char *type,
                                         cbm_gbuf_edge_keep_fn keep, void *userdata);

/* ── Vector storage (for semantic embeddings) ───────────────────── */

/* Store an int8-quantized vector for a node. The vector data is copied.
 * Called by pass_semantic_edges after computing RI vectors.
 * Vectors are carried through to the cbm.zova publish during the dump phase. */
int cbm_gbuf_store_vector(cbm_gbuf_t *gb, int64_t node_id, const uint8_t *vector, int vector_len);

/* Store an enriched token vector for query-time lookup.
 * Called by pass_semantic_edges after corpus finalization.
 * Token string and vector data are copied. */
int cbm_gbuf_store_token_vector(cbm_gbuf_t *gb, const char *token, const uint8_t *vector,
                                int vector_len, float idf);

/* Discard semantic vector payloads loaded from a prior incremental graph
 * before the global semantic pass recomputes the complete corpus. */
void cbm_gbuf_clear_semantic_vectors(cbm_gbuf_t *gb);

/* Mark both semantic vector collections unchanged for the next authoritative
 * delta publication. Valid only when the graph buffer contains no vectors. */
int cbm_gbuf_reuse_zova_semantic_vectors(cbm_gbuf_t *gb);

/* ── Dump for cbm.zova ───────────────────────────────────────────── */

/* Finalize stable node/edge/vector identities: assigns sequential final IDs
 * and remaps edge references. Returns 0 on success. */
int cbm_gbuf_prepare_zova_dump(cbm_gbuf_t *gb);
/* Incremental publication retains the delta publication model and therefore
 * only finalizes dense dump arrays. */
int cbm_gbuf_prepare_zova_dump_delta(cbm_gbuf_t *gb);

/* Opt-in single-file publication using the finalized graph/vector arrays. The
 * caller owns the optional file-hash and summary inputs and the result is
 * filled by cbm_zova_user_database_publish_workspace(). */
int cbm_gbuf_publish_zova_user_database(
    cbm_gbuf_t *gb, const cbm_zova_file_hash_input_t *file_hashes,
    int file_hash_count, const cbm_zova_project_summary_input_t *project_summary,
    cbm_zova_workspace_generation_result_t *out_result);
int cbm_gbuf_publish_zova_user_database_delta(
    const cbm_gbuf_t *gb, cbm_zova_workspace_snapshot_t *before,
    const cbm_zova_file_hash_input_t *file_hashes, int file_hash_count,
    const cbm_zova_project_summary_input_t *project_summary,
    cbm_zova_workspace_generation_result_t *out_result);

/* Flush the buffer to an existing store via the store API.
 * Deletes existing project data first. Returns 0 on success. */
int cbm_gbuf_flush_to_store(cbm_gbuf_t *gb, cbm_store_t *store);

/* Merge the buffer into an existing store WITHOUT deleting existing data.
 * Upserts nodes, inserts edges. Used for incremental indexing.
 * Returns 0 on success. */
int cbm_gbuf_merge_into_store(cbm_gbuf_t *gb, cbm_store_t *store);

#endif /* CBM_GRAPH_BUFFER_H */
