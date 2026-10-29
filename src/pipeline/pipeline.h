/*
 * pipeline.h — Indexing pipeline orchestrator.
 *
 * Orchestrates multi-pass indexing of a repository:
 *   1. Structure: Project/Folder/Package/File nodes
 *   2. Definitions: Extract + write nodes + build registry
 *   3. Imports: Resolve import edges
 *   4. Calls: Call resolution (registry + LSP)
 *   5. Usages: Usage/type_ref edges
 *   6. Semantic: Inherits/decorates/implements
 *   7. Post: Tests, communities, HTTP links, config, git history
 *
 * Depends on: foundation, extraction, lsp, store, graph_buffer, discover
 */
#ifndef CBM_PIPELINE_H
#define CBM_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbm.h" /* CBMLanguage for language-scoped suppression helpers */

/* Forward declarations */
typedef struct cbm_store cbm_store_t;
typedef struct cbm_gbuf cbm_gbuf_t;

/* ── Opaque handle ──────────────────────────────────────────────── */

typedef struct cbm_pipeline cbm_pipeline_t;

typedef enum {
    CBM_PIPELINE_ROUTE_UNKNOWN = 0,
    CBM_PIPELINE_ROUTE_FULL = 1,
    CBM_PIPELINE_ROUTE_INCREMENTAL = 2,
} cbm_pipeline_route_t;

/* ── Index mode ─────────────────────────────────────────────────── */

#ifndef CBM_INDEX_MODE_T_DEFINED
#define CBM_INDEX_MODE_T_DEFINED
typedef enum {
    /* All modes run the LSP type-aware call/usage resolution (per-file +
     * cross-file). The mode only controls file discovery breadth and whether
     * SIMILAR_TO / SEMANTICALLY_RELATED edges are computed. */
    CBM_MODE_FULL = 0,     /* Full: everything including SIMILAR_TO + SEMANTICALLY_RELATED */
    CBM_MODE_MODERATE = 1, /* Moderate: fast discovery + SIMILAR_TO + SEMANTICALLY_RELATED */
    CBM_MODE_FAST = 2,     /* Fast: skip non-essential files, no similarity/semantic edges */
} cbm_index_mode_t;
#endif

/* ── Pipeline lifecycle ─────────────────────────────────────────── */

/* Create a new pipeline. Caller owns the result. */
cbm_pipeline_t *cbm_pipeline_new(const char *repo_path, const char *db_path, cbm_index_mode_t mode);

/* Free a pipeline and all its internal state. NULL-safe. */
void cbm_pipeline_free(cbm_pipeline_t *p);

/* Run the full indexing pipeline. Returns 0 on success, non-zero on error.
 * Discovers files, extracts, resolves, and dumps to SQLite. */
/* Resident memory stayed above the budget after back-pressure and one
 * confirmation cycle: the run stops before publication, no partial graph is
 * written and the previous generation still serves (upstream #1997 #832).
 * Distinct from the cancel sentinel so callers can name the cause. */
#define CBM_PIPELINE_ABORT_OVER_BUDGET (-5)
int cbm_pipeline_run(cbm_pipeline_t *p);

/* Request cancellation of a running pipeline (thread-safe). */
void cbm_pipeline_cancel(cbm_pipeline_t *p);

/* Get the project name derived from repo_path. Returned string is
 * owned by the pipeline. Valid until cbm_pipeline_free(). */
const char *cbm_pipeline_project_name(const cbm_pipeline_t *p);

/* Override the derived project name with a sanitized user-provided label. */
bool cbm_pipeline_set_project_name(cbm_pipeline_t *p, const char *name);

/* Get the index mode (CBM_MODE_FULL, CBM_MODE_MODERATE, CBM_MODE_FAST). */
int cbm_pipeline_get_mode(const cbm_pipeline_t *p);

/* Get the list of directory subtrees skipped during discovery (#411).
 * *out receives a borrowed array of rel-path strings (owned by the pipeline,
 * valid until cbm_pipeline_free()); *count receives its length. Both are set
 * to NULL/0 when p is NULL or nothing was excluded. Do not free. */
void cbm_pipeline_get_excluded(const cbm_pipeline_t *p, char ***out, int *count);

/* True when discovery could not fully materialize the tree (bounded walk stack
 * full / allocation failure). The index produced from such a run covers only
 * the files that were actually discovered (#17, upstream 03dc9c91); callers
 * should surface a degraded status. false when p is NULL. */
bool cbm_pipeline_discover_degraded(const cbm_pipeline_t *p);

/* Committed node/edge counts captured at dump time (-1 when dump did not run).
 * Nodes are the #334 plausibility-gate axis; edges are informational only. */
void cbm_pipeline_get_committed_counts(const cbm_pipeline_t *p, int *nodes, int *edges);

/* Elapsed time of the shared user-local Zova publication from the last run,
 * or 0 when publication was disabled or did not complete. */
double cbm_pipeline_get_zova_publish_ms(const cbm_pipeline_t *p);

/* Actual route selected by the most recent cbm_pipeline_run(). This is
 * independent from cbm_index_mode_t: both full and incremental runs use the
 * same extraction breadth, while the route describes persistence behavior. */
cbm_pipeline_route_t cbm_pipeline_get_last_route(const cbm_pipeline_t *p);

/* True when the most recent cbm_pipeline_run() found a generation written in
 * an older index format and rebuilt it in full (CBM_INDEX_FORMAT_VERSION_STR). */
bool cbm_pipeline_had_format_migration(const cbm_pipeline_t *p);

typedef struct {
    bool completed;
    bool delta;
    uint64_t full_clear_count;
    uint64_t unchanged_rewrite_count;
    uint64_t nodes_inserted;
    uint64_t nodes_updated;
    uint64_t nodes_deleted;
    uint64_t edges_inserted;
    uint64_t edges_deleted;
    uint64_t node_vectors_upserted;
    uint64_t node_vectors_deleted;
    uint64_t token_vectors_upserted;
    uint64_t token_vectors_deleted;
    double route_ms;
    double normalization_ms;
    double model_nodes_ms;
    double model_edges_ms;
    double model_edge_endpoint_ms;
    double model_edge_sort_ms;
    double model_edge_group_ms;
    double model_edge_payload_ms;
    double model_edge_digest_ms;
    uint64_t model_edge_default_payloads;
    uint64_t model_edge_payload_scratch_edges;
    double model_hashes_ms;
    double model_vectors_ms;
    double model_digests_ms;
    double writer_guard_ms;
    double database_init_ms;
    double database_open_ms;
    double transaction_begin_ms;
    double transaction_body_ms;
    double transaction_commit_ms;
    double database_close_ms;
    double clear_ms;
    double finalize_ms;
    double canonical_files_ms;
    double canonical_nodes_ms;
    double canonical_edges_ms;
    double canonical_hashes_ms;
    double fts_ms;
    double token_metadata_ms;
    double native_graph_ms;
    double native_graph_materialize_ms;
    double native_graph_reset_ms;
    double native_graph_nodes_ms;
    double native_graph_edges_ms;
    double native_graph_validate_ms;
    double native_graph_key_generation_ms;
    double native_graph_cleanup_ms;
    double native_vectors_ms;
    double fresh_validation_ms;
    double fresh_index_ms;
    double fresh_commit_ms;
    double fresh_build_ms;
    double readback_ms;
    bool snapshot_completed;
    double snapshot_base_ms;
    double snapshot_optional_ms;
    double snapshot_open_ms;
    double snapshot_header_ms;
    double snapshot_integrity_ms;
    double snapshot_nodes_sql_ms;
    double snapshot_nodes_native_ms;
    double snapshot_nodes_finalize_ms;
    double snapshot_edges_sql_ms;
    double snapshot_edges_native_ms;
    double snapshot_edges_finalize_ms;
    double snapshot_hashes_summary_ms;
    double snapshot_close_ms;
    double snapshot_graph_buffer_ms;
    uint32_t snapshot_base_phase_mask;
    uint64_t snapshot_node_rows;
    uint64_t snapshot_edge_rows;
    uint64_t snapshot_file_hash_rows;
    uint32_t snapshot_hydrated_components;
    uint64_t snapshot_topology_rows;
    uint64_t snapshot_node_vector_rows;
    uint64_t snapshot_token_vector_rows;
    int64_t snapshot_generation;
} cbm_pipeline_zova_publish_stats_t;

/* Copy exact counters from the last successful authoritative Zova publish. */
bool cbm_pipeline_get_zova_publish_stats(
    const cbm_pipeline_t *p, cbm_pipeline_zova_publish_stats_t *out_stats);

/* ── Per-file indexing failures (Stage 2 / Track B) ─────────────── */

/* One source file that was skipped during indexing. All strings are owned by
 * the pipeline (copied on record, freed in cbm_pipeline_free). A skip is the
 * expected, handled outcome of a bad/oversized file — indexing continues and
 * the run still reports status "indexed"; these are surfaced (not errors that
 * fail the run) via MCP `skipped[]` / the CLI / a per-run logfile. */
typedef struct {
    char *path;   /* repo-relative path of the skipped file */
    char *reason; /* human-readable cause (e.g. "oversized (712 MB > 512 MB)",
                   * "parse timeout", "read failed"). For the partial phases
                   * this is the 1-based line-range list ("12-40,88-90"). */
    char *phase;  /* "read" | "extract" | "oversized" | "crash" | "hang" |
                   * "parse_partial" | "parse_unusable". Neither is a skip: the
                   * file was indexed but has unrecovered parse-error regions
                   * ("parse_unusable": one range covers most of the file); the
                   * MCP layer reports them apart from skipped[]. "cross_lsp" is a RESERVED
                   * phase string for Track C's crash-attribution signal and is
                   * intentionally NOT emitted today (the cross-LSP passes are
                   * best-effort/void with no genuine per-file failure). */
} cbm_file_error_t;

/* Record a skipped file. path/reason/phase are copied. NULL-safe on p.
 *
 * NOT thread-safe: call it from the sequential extraction pass, or from the
 * parallel merge step (never from inside a parallel worker — workers collect
 * into per-worker lists and merge sequentially). */
/* True for the phases that mean "indexed, but with unrecovered parse-error
 * regions" ("parse_partial", "parse_unusable"), as opposed to a skip. */
bool cbm_pipeline_phase_is_partial(const char *phase);

void cbm_pipeline_add_file_error(cbm_pipeline_t *p, const char *path, const char *reason,
                                 const char *phase);

/* Borrowed accessor for the recorded skips (owned by the pipeline, valid until
 * cbm_pipeline_free()). out and count are set to NULL and 0 when p is NULL or
 * nothing was skipped. Do not free. */
void cbm_pipeline_get_file_errors(const cbm_pipeline_t *p, cbm_file_error_t **out, int *count);

/* Parse coverage stamped on File node properties. A File node without a
 * "coverage" key was parsed with no recorded gap; "partial" carries
 * "parse_error_ranges"; "missed" carries "coverage_phase" and
 * "coverage_reason". The Project node's "coverage_version" marks a generation
 * whose File nodes were stamped. Best-effort: no flag is not a completeness
 * proof. */
#define CBM_COVERAGE_PARTIAL "partial"
#define CBM_COVERAGE_MISSED "missed"
#define CBM_COVERAGE_VERSION_STR "1"

/* Index format, stamped as "index_format" on the Project node. Bump it when a
 * change alters the QN scheme or node identity of an already-written graph:
 * an older (or unstamped) generation then takes the full route once instead
 * of being mixed with new-format nodes by the incremental route.
 * 1 = File QNs keep the file extension (upstream #769/#1108).
 * 2 = C-family node identities (upstream #2543): a preprocessor macro's QN
 *     ends in "#macro"; unscoped C/C++/Objective-C enumerators are
 *     `<scope>.<NAME>`; typedef names, anonymous-enum constants and
 *     macro-prefixed functions are nodes; a bodyless `struct X` is no node.
 * 3 = Zig node identities: declarations inside a named container are
 *     `<container>.<name>` (methods, nested structs/enums/unions, and alias
 *     Variables such as `pub const Db = impl.Database;`). */
#define CBM_INDEX_FORMAT_VERSION_STR "3"

/* Runs share no process-global state, so several may run in one process.
 * Same-workspace exclusion is the workspace lease
 * (zova/cbm_zova_coordination.h), taken at the index entry points. */

/* ── FQN helpers (used by passes and external callers) ──────────── */

/* Compute a qualified name: project.dir.parts.name
 * Strips extension, converts / to ., drops __init__ and index.
 * Caller must free() the returned string. */
char *cbm_pipeline_fqn_compute(const char *project, const char *rel_path, const char *name);

/* Module QN: project.dir.parts (no name). Caller must free(). */
char *cbm_pipeline_fqn_module(const char *project, const char *rel_path);

/* Language-aware module QN. When `module_is_dir` is true (Java/Go package
 * semantics) the module is derived from the CONTAINING DIRECTORY (the filename
 * stem is dropped), so it agrees with the extraction-side def QNs; when false
 * it is exactly cbm_pipeline_fqn_module(). Caller must free(). */
char *cbm_pipeline_fqn_module_dir(const char *project, const char *rel_path, bool module_is_dir);

/* Folder QN: project.dir.parts. Caller must free(). */
char *cbm_pipeline_fqn_folder(const char *project, const char *rel_dir);

/* Resolve an import specifier that uses a relative path (./foo, ../bar, .foo,
 * or an unqualified local name like "foo.h") against the importing file's
 * path.  Returns a malloc'd normalized relative path without extension
 * (e.g. "src/api/helpers") suitable for passing to cbm_pipeline_fqn_module,
 * or NULL if the specifier is not a relative path (bare module names like
 * "lodash", "django", "github.com/foo/bar" return NULL — the caller should
 * treat those as external/unresolvable). Handles ".", "..", and leading
 * dot-only segments used by Python relative imports. */
char *cbm_pipeline_resolve_relative_import(const char *source_rel, const char *module_path);

/* Derive project name from an absolute path.
 * Replaces / and : with -, collapses --, trims leading -.
 * Caller must free() the returned string. */
char *cbm_project_name_from_path(const char *abs_path);
/* Public MCP selector derived from the final path component plus
 * `parent_levels` parents. The returned separator is always '/'. */
char *cbm_project_selector_from_path(const char *abs_path, size_t parent_levels);

/* ── Function Registry ──────────────────────────────────────────── */

typedef struct cbm_registry cbm_registry_t;

typedef struct {
    const char *qualified_name; /* borrowed from registry */
    const char *strategy;       /* resolution strategy name */
    double confidence;          /* 0.0–1.0 */
    int candidate_count;
} cbm_resolution_t;

/* Create/free a function registry. */
cbm_registry_t *cbm_registry_new(void);
void cbm_registry_free(cbm_registry_t *r);

/* Register a function/method/class. All strings are copied. */
void cbm_registry_add(cbm_registry_t *r, const char *name, const char *qualified_name,
                      const char *label);

/* Resolve a callee name using prioritized strategies.
 * import_map: NULL-terminated array of {local_name, resolved_qn} pairs, or NULL.
 * Returns result with qualified_name="" if unresolved. */
cbm_resolution_t cbm_registry_resolve(const cbm_registry_t *r, const char *callee_name,
                                      const char *module_qn, const char **import_map_keys,
                                      const char **import_map_vals, int import_map_count);

/* Per-file memoization cache for is_import_reachable. Thread-local —
 * each resolve worker owns its own cache. Call _begin at the start
 * of resolve_file_calls (or any per-file resolve loop) and _end at
 * the end. The cache MUST be invalidated between files because
 * is_import_reachable's truth depends on the file's import_vals. */
void cbm_registry_reach_cache_begin(int estimated_capacity);
void cbm_registry_reach_cache_end(void);

/* Per-file import-map prefix → module-QN hash. Turns the linear
 * strcmp scan inside resolve_import_map into O(1). Keys/values are
 * BORROWED — caller must keep the import_map arrays alive for the
 * cache lifetime. Invalidate between files via _end. */
void cbm_registry_import_map_cache_begin(const char **keys, const char **vals, int count);
void cbm_registry_import_map_cache_end(void);

/* Per-file full-result cache for cbm_registry_resolve. The same
 * callee_name appears in many call sites within a file; module_qn
 * is constant per file so each name resolves identically. First
 * lookup does the full strategy chain; repeats are O(1) hash hits.
 * This eliminates ~75% of the resolve-chain work on K8s where the
 * same names ("Get", "Add", "New", etc) appear hundreds of times. */
void cbm_registry_resolve_cache_begin(int estimated_capacity);
void cbm_registry_resolve_cache_end(void);

/* Check if a qualified name exists in the registry. */
bool cbm_registry_exists(const cbm_registry_t *r, const char *qn);

/* True if `name` is one of the curated Perl core builtins (perlfunc). Used by
 * the call-resolution passes to suppress generic-resolver CALLS edges from Perl
 * builtin invocations (push/shift/keys/...) to project subs that merely share
 * the name. Perl-scoped: callers gate on the file language. */
bool cbm_perl_is_builtin(const char *name);

/* Decide whether a resolved Perl call edge is generic-resolver noise to drop
 * (#476): true only for Perl, only for a builtin/method call, and only when the
 * match used a weak short-name strategy — high-confidence same_module/import_map
 * matches are kept. Pure; unit-tested in test_registry.c. */
bool cbm_perl_suppress_generic_match(bool is_perl, bool is_method, const char *callee_name,
                                     const char *strategy);

/* Decide whether a resolved member-call edge is weak-strategy noise to drop
 * (#592/#606/#1276): true only when the CALLER's per-language gate says the
 * guard applies (`enabled`), only for a member call with an unresolved receiver
 * (is_method), and only when the match used a weak short-name strategy
 * (suffix_match / unique_name / field_type_hint / fuzzy).
 * Explicit drop-list keeps every lsp_* / import / same-module / qualified match.
 * The language set lives at the call sites (pass_calls.c / pass_parallel.c) and
 * must be identical in both, or the sequential and parallel resolvers diverge.
 * Pure; unit-tested in test_registry.c. */
bool cbm_suppress_weak_member_match(bool enabled, bool is_method, const char *strategy);

/* Zig-only addition to the guard above: also drop qualified_suffix for a Zig
 * member call. The Zig LSP types every mapped or relative @import alias, so a
 * registry qualified_suffix match there carries no import evidence (an unmapped
 * `old.hello()` must not bind libs/old/src/old.zig). Pure; unit-tested in
 * test_registry.c. */
bool cbm_suppress_zig_qualified_member(bool is_zig, bool is_method, const char *strategy);

/* Bare-call counterpart of the guard above. True when a resolved BARE call edge
 * binds a callee that is shadowed by an enclosing parameter, and the match came
 * from a weak short-name strategy — so the edge is fabricated by construction
 * (`def f(run): run()` must not bind an unrelated `SatoriLive.run`). Shares the
 * member guard's drop-list, so lsp_* / import / same-module matches are kept.
 * Deliberately keyed on the SCOPE FACT, not on the callee's spelling. The
 * language set lives at the call sites (pass_calls.c / pass_parallel.c) and must
 * be identical in both, or the sequential and parallel resolvers diverge.
 * Pure; unit-tested in test_registry.c. */
bool cbm_suppress_weak_local_binding_call(bool enabled, bool callee_is_locally_bound,
                                          const char *strategy);

/* True for the weak short-name strategies the two guards above drop
 * (suffix_match / unique_name / field_type_hint / fuzzy): a match by name
 * alone, with no import, receiver or module evidence. Pure; unit-tested in
 * test_registry.c. */
bool cbm_registry_strategy_is_weak(const char *strategy);

/* Decide whether to drop a suffix_match CALLS edge when the caller language
 * and the target file's language disagree (upstream 416ba994/#725): two
 * same-named symbols in different languages would otherwise collapse onto one
 * winner (e.g. a Python Store.commit call attached to a JS commit function).
 * unique_name (candidates == 1) is #1572 and is left alone; same_module /
 * import_map / lsp_* are kept. JS/TS/TSX are one family. Pure; unit-tested
 * in test_registry.c. */
bool cbm_suppress_cross_language_suffix_match(CBMLanguage caller_lang, const char *target_file_path,
                                              const char *strategy);

/* #1928: USAGE/WRITES/READS analog of the CALLS guard above. Reference edges
 * resolved by the short-name registry carry no import-closure evidence, so a
 * cross-language binding is a bare-name collision for EVERY strategy — drop
 * it whenever the caller's language and the target file's language disagree
 * (JS/TS family members and the C/C++ header family excepted). Pure;
 * unit-tested in test_registry.c. */
bool cbm_suppress_cross_language_ref(CBMLanguage caller_lang, const char *target_file_path);

/* #1942: a bare (dot-less) Go reference can never denote a struct field —
 * field access is always a selector expression, and selector references
 * resolve on the LSP path. Drops a READS/WRITES/USAGE bind whose target is a
 * Field when the reference text carries no '.'. Go only: other OO languages
 * legitimately reference their own members bare inside method bodies. Pure;
 * unit-tested in test_registry.c. */
bool cbm_go_suppress_bare_field_ref(bool is_go, bool is_member_access, const char *target_label);

/* Get the label of a qualified name, or NULL if not found. */
const char *cbm_registry_label_of(const cbm_registry_t *r, const char *qn);

/* Find all QNs with a given simple name. Sets *out and *count.
 * Caller does NOT free the array (owned by registry). */
int cbm_registry_find_by_name(const cbm_registry_t *r, const char *name, const char ***out,
                              int *count);

/* Return total number of entries. */
int cbm_registry_size(const cbm_registry_t *r);

/* Find all qualified names ending with ".suffix".
 * Sets *out to heap-allocated array of borrowed string pointers.
 * Caller must free(*out) but NOT the individual strings.
 * Returns count of matches. */
int cbm_registry_find_ending_with(const cbm_registry_t *r, const char *suffix, const char ***out);

/* Check if candidate QN's module prefix is reachable via any import value. */
bool cbm_registry_is_import_reachable(const char *candidate_qn, const char **import_vals,
                                      int import_count);

/* Fuzzy resolve: match callee by bare function name (last segment after dots).
 * Returns result with ok=true if found, ok=false if not.
 * Lower confidence than Resolve (0.40 single, 0.30 multiple). */
typedef struct {
    cbm_resolution_t result;
    bool ok;
} cbm_fuzzy_result_t;

cbm_fuzzy_result_t cbm_registry_fuzzy_resolve(const cbm_registry_t *r, const char *callee_name,
                                              const char *module_qn, const char **import_map_keys,
                                              const char **import_map_vals, int import_map_count);

const char *cbm_confidence_band(double score);

#endif /* CBM_PIPELINE_H */
