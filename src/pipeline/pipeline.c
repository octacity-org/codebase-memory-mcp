/*
 * pipeline.c — Indexing pipeline orchestrator.
 *
 * Coordinates multi-pass indexing:
 *   1. Discover files
 *   2. Build structure (Project/Folder/Package/File nodes)
 *   3. Bulk load sources (read + LZ4 HC compress)
 *   4. Extract definitions (fused: extract + write nodes + build registry)
 *   5. Resolve imports, calls, usages, semantic edges
 *   6. Post-passes: tests, communities, HTTP links, git history
 *   7. Dump graph buffer to SQLite
 */
#include "foundation/constants.h"

enum { CBM_DIR_PERMS = 0755, PL_RING = 4, PL_RING_MASK = 3, PL_SEQ_PASSES = 6 };
#define PL_NSEC_PER_SEC 1000000000LL
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "pipeline/pass_lsp_cross.h"
#include "pipeline/worker_pool.h"
#include "graph_buffer/graph_buffer.h"
#include "git/git_context.h"
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_route.h"
#include "zova/cbm_zova_repository.h"
#include "discover/discover.h"
#include "discover/userconfig.h"
#include "foundation/platform.h"
#include "foundation/compat_fs.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include "foundation/hash_table.h"
#include "foundation/compat.h"
#include "foundation/compat_thread.h"
#include "foundation/profile.h"
#include "foundation/mem.h"
#include "yyjson/yyjson.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>

static inline void *intptr_to_ptr(intptr_t v) {
    void *p;
    memcpy(&p, &v, sizeof(p));
    return p;
}

/* ── Internal state ──────────────────────────────────────────────── */

struct cbm_pipeline {
    char *repo_path;
    char *db_path;
    char *project_name;
    cbm_git_context_t git_ctx;
    char *branch_qn;
    cbm_index_mode_t mode;
    atomic_int cancelled;

    /* Indexing state (set during run) */
    cbm_gbuf_t *gbuf;
    cbm_registry_t *registry;

    /* Directory subtrees skipped during discovery (rel paths). Captured from
     * cbm_discover_ex so the MCP layer can report excluded subtrees (#411).
     * Owned by the pipeline; freed in cbm_pipeline_free. */
    char **excluded_dirs;
    int excluded_count;

    /* Set when discovery could not fully materialize the tree (bounded walk
     * stack full / allocation failure). The MCP layer reports a degraded index
     * instead of a complete-looking partial one (#17, upstream 03dc9c91). */
    bool discover_degraded;

    /* Per-file indexing failures (skipped files) surfaced via MCP/CLI/logfile
     * (Stage 2 / Track B). A skip is the expected handled outcome of a bad or
     * oversized file — the run still succeeds ("indexed"). Owned by the
     * pipeline; freed in cbm_pipeline_free. */
    cbm_file_error_t *file_errors;
    int file_errors_count;
    int file_errors_cap;

    /* User-defined extension overrides (loaded once per run) */
    cbm_userconfig_t *userconfig;

    /* Committed graph size at dump time (-1 = dump did not run). #334 gate axis. */
    int committed_nodes;
    int committed_edges;
    double zova_publish_ms;
    cbm_pipeline_route_t last_route;
    /* The last run rebuilt a generation stamped with an older index format. */
    bool format_migration;
    cbm_pipeline_zova_publish_stats_t zova_publish_stats;

    cbm_project_summary_export_t saved_summary;
    bool saved_summary_valid;
};

/* ── Timing helper ──────────────────────────────────────────────── */

static double elapsed_ms(struct timespec start) {
    struct timespec now;
    cbm_clock_gettime(CLOCK_MONOTONIC, &now);
    return ((double)(now.tv_sec - start.tv_sec) * CBM_MS_PER_SEC) +
           ((double)(now.tv_nsec - start.tv_nsec) / CBM_US_PER_SEC_F);
}

/* Format int to string for logging. Thread-safe via TLS rotating buffers. */
static const char *itoa_buf(int val) {
    static CBM_TLS char bufs[PL_RING][CBM_SZ_32];
    static CBM_TLS int idx = 0;
    int i = idx;
    idx = (idx + SKIP_ONE) & PL_RING_MASK;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", val);
    return bufs[i];
}

/* Log current + peak RSS and the budget at a pipeline phase boundary (memory
 * profiling). The process peak is cumulative, so the boundary where peak_mb
 * first rises is the phase that set it (#49). */
void cbm_pipeline_log_phase_mem(const char *phase) {
    enum { PL_BYTES_PER_MB = 1024 * 1024 };
    cbm_log_info("mem.phase", "phase", phase, "rss_mb",
                 itoa_buf((int)(cbm_mem_rss() / PL_BYTES_PER_MB)), "peak_mb",
                 itoa_buf((int)(cbm_mem_peak_rss() / PL_BYTES_PER_MB)), "budget_mb",
                 itoa_buf((int)(cbm_mem_budget() / PL_BYTES_PER_MB)));
}

/* Post-extraction budget gate: reclaim freed pages, then nap in bounded steps
 * while still over budget (a transient may drain). A full cycle that ends over
 * budget means the resident graph, not an in-flight transient, holds the
 * memory: fail the attempt whole before publication so the previously served
 * generation keeps answering. Returns 0 when under budget (or no budget). */
int cbm_pipeline_budget_gate(const char *phase) {
    enum { PL_BACKPRESSURE_MAX_SPINS = 40, PL_BACKPRESSURE_NAP_NS = 3000000 };
    if (cbm_mem_budget() == 0) {
        return 0;
    }
    if (!cbm_mem_over_budget()) {
        return 0;
    }
    cbm_mem_collect();
    int spins = 0;
    for (; spins < PL_BACKPRESSURE_MAX_SPINS && cbm_mem_over_budget(); spins++) {
        struct timespec nap = {0, PL_BACKPRESSURE_NAP_NS};
        cbm_nanosleep(&nap, NULL);
    }
    if (!cbm_mem_over_budget()) {
        return 0;
    }
    cbm_log_error("mem.budget.exceeded", "rss_mb", itoa_buf((int)(cbm_mem_rss() / (1024 * 1024))),
                  "budget_mb", itoa_buf((int)(cbm_mem_budget() / (1024 * 1024))), "phase",
                  phase ? phase : "post_extract", "action", "fail_whole");
    return CBM_PIPELINE_ABORT_OVER_BUDGET;
}

/* ── Lifecycle ──────────────────────────────────────────────────── */

cbm_pipeline_t *cbm_pipeline_new(const char *repo_path, const char *db_path,
                                 cbm_index_mode_t mode) {
    if (!repo_path) {
        return NULL;
    }

    cbm_pipeline_t *p = calloc(CBM_ALLOC_ONE, sizeof(cbm_pipeline_t));
    if (!p) {
        return NULL;
    }

    p->repo_path = strdup(repo_path);
    p->db_path = db_path ? strdup(db_path) : NULL;
    p->project_name = cbm_project_name_from_path(repo_path);
    (void)cbm_git_context_resolve(repo_path, &p->git_ctx);
    p->branch_qn = cbm_git_context_branch_qn(p->project_name, &p->git_ctx);
    p->mode = mode;
    p->committed_nodes = -1;
    p->committed_edges = -1;
    atomic_init(&p->cancelled, 0);

    return p;
}

bool cbm_pipeline_set_project_name(cbm_pipeline_t *p, const char *name) {
    if (!p || !name || !name[0]) {
        return false;
    }

    char *normalized = cbm_project_name_from_path(name);
    if (!normalized) {
        return false;
    }
    if (!cbm_validate_project_name(normalized)) {
        free(normalized);
        return false;
    }

    free(p->project_name);
    p->project_name = normalized;
    free(p->branch_qn);
    p->branch_qn = cbm_git_context_branch_qn(p->project_name, &p->git_ctx);
    return true;
}

void cbm_pipeline_free(cbm_pipeline_t *p) {
    if (!p) {
        return;
    }
    free(p->repo_path);
    free(p->db_path);
    free(p->project_name);
    cbm_discover_free_excluded(p->excluded_dirs, p->excluded_count);
    p->excluded_dirs = NULL;
    p->excluded_count = 0;
    for (int i = 0; i < p->file_errors_count; i++) {
        free(p->file_errors[i].path);
        free(p->file_errors[i].reason);
        free(p->file_errors[i].phase);
    }
    free(p->file_errors);
    p->file_errors = NULL;
    p->file_errors_count = 0;
    p->file_errors_cap = 0;
    free(p->branch_qn);
    cbm_store_project_summary_export_free(&p->saved_summary);
    p->saved_summary_valid = false;
    cbm_git_context_free(&p->git_ctx);
    /* gbuf, store, registry freed during/after run */
    /* Defensively free userconfig in case run() was never called or panicked */
    if (p->userconfig) {
        cbm_set_user_lang_config(NULL);
        cbm_userconfig_free(p->userconfig);
        p->userconfig = NULL;
    }
    free(p);
}

void cbm_pipeline_cancel(cbm_pipeline_t *p) {
    if (p) {
        atomic_store(&p->cancelled, 1);
    }
}

const char *cbm_pipeline_project_name(const cbm_pipeline_t *p) {
    return p ? p->project_name : NULL;
}

const char *cbm_pipeline_repo_path(const cbm_pipeline_t *p) {
    return p ? p->repo_path : NULL;
}

atomic_int *cbm_pipeline_cancelled_ptr(cbm_pipeline_t *p) {
    return p ? &p->cancelled : NULL;
}

int cbm_pipeline_get_mode(const cbm_pipeline_t *p) {
    return p ? (int)p->mode : 0;
}

void cbm_pipeline_get_excluded(const cbm_pipeline_t *p, char ***out, int *count) {
    if (out) {
        *out = p ? p->excluded_dirs : NULL;
    }
    if (count) {
        *count = p ? p->excluded_count : 0;
    }
}

bool cbm_pipeline_discover_degraded(const cbm_pipeline_t *p) {
    return p ? p->discover_degraded : false;
}

/* NULL-safe heap strdup (avoids a strdup dependency + guards NULL inputs). */
static char *fe_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

bool cbm_pipeline_phase_is_partial(const char *phase) {
    return phase && (strcmp(phase, "parse_partial") == 0 || strcmp(phase, "parse_unusable") == 0);
}

void cbm_pipeline_add_file_error(cbm_pipeline_t *p, const char *path, const char *reason,
                                 const char *phase) {
    if (!p) {
        return;
    }
    if (p->file_errors_count >= p->file_errors_cap) {
        int ncap = p->file_errors_cap ? p->file_errors_cap * 2 : 16;
        cbm_file_error_t *grown =
            (cbm_file_error_t *)realloc(p->file_errors, (size_t)ncap * sizeof(*grown));
        if (!grown) {
            /* Never abort indexing just to record a skip — drop this record. */
            return;
        }
        p->file_errors = grown;
        p->file_errors_cap = ncap;
    }
    cbm_file_error_t *e = &p->file_errors[p->file_errors_count];
    e->path = fe_strdup(path);
    e->reason = fe_strdup(reason);
    e->phase = fe_strdup(phase);
    p->file_errors_count++;
}

void cbm_pipeline_get_file_errors(const cbm_pipeline_t *p, cbm_file_error_t **out, int *count) {
    if (out) {
        *out = p ? p->file_errors : NULL;
    }
    if (count) {
        *count = p ? p->file_errors_count : 0;
    }
}

/* Rebuild a File node's properties with its coverage stamp. The extension
 * key mirrors pass_structure / incr_add_changed_file_structure. */
static bool stamp_file_coverage(cbm_pipeline_t *p, cbm_gbuf_t *gbuf, const cbm_file_error_t *e,
                                bool partial) {
    char *file_qn = cbm_pipeline_fqn_compute(p->project_name, e->path, "__file__");
    if (!file_qn) {
        return false;
    }
    const char *slash = strrchr(e->path, '/');
    const char *basename = slash ? slash + SKIP_ONE : e->path;
    const char *ext = strrchr(basename, '.');
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "extension", ext ? ext : "");
    if (partial) {
        yyjson_mut_obj_add_str(doc, root, "coverage", CBM_COVERAGE_PARTIAL);
        yyjson_mut_obj_add_str(doc, root, "parse_error_ranges", e->reason ? e->reason : "");
        if (strcmp(e->phase, "parse_unusable") == 0) {
            yyjson_mut_obj_add_bool(doc, root, "parse_unusable", true);
        }
    } else {
        yyjson_mut_obj_add_str(doc, root, "coverage", CBM_COVERAGE_MISSED);
        yyjson_mut_obj_add_str(doc, root, "coverage_phase", e->phase);
        yyjson_mut_obj_add_str(doc, root, "coverage_reason", e->reason ? e->reason : "");
    }
    char *props = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    bool stamped = props && cbm_gbuf_set_node_props(gbuf, file_qn, props);
    free(props);
    free(file_qn);
    return stamped;
}

void cbm_pipeline_stamp_coverage(cbm_pipeline_t *p, cbm_gbuf_t *gbuf) {
    if (!p || !gbuf) {
        return;
    }
    int partial = 0;
    int missed = 0;
    /* Partial first, so a file recorded under both keeps the stronger
     * "missed" stamp. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < p->file_errors_count; i++) {
            const cbm_file_error_t *e = &p->file_errors[i];
            if (!e->path || !e->phase) {
                continue;
            }
            bool is_partial = cbm_pipeline_phase_is_partial(e->phase);
            if (is_partial != (pass == 0)) {
                continue;
            }
            if (stamp_file_coverage(p, gbuf, e, is_partial)) {
                if (is_partial) {
                    partial++;
                } else {
                    missed++;
                }
            }
        }
    }
    if (partial > 0 || missed > 0) {
        cbm_log_info("index.coverage", "partial", itoa_buf(partial), "missed", itoa_buf(missed));
    }
}

void cbm_pipeline_get_committed_counts(const cbm_pipeline_t *p, int *nodes, int *edges) {
    if (nodes) {
        *nodes = p ? p->committed_nodes : -1;
    }
    if (edges) {
        *edges = p ? p->committed_edges : -1;
    }
}

double cbm_pipeline_get_zova_publish_ms(const cbm_pipeline_t *p) {
    return p ? p->zova_publish_ms : 0.0;
}

cbm_pipeline_route_t cbm_pipeline_get_last_route(const cbm_pipeline_t *p) {
    return p ? p->last_route : CBM_PIPELINE_ROUTE_UNKNOWN;
}

bool cbm_pipeline_had_format_migration(const cbm_pipeline_t *p) {
    return p && p->format_migration;
}

bool cbm_pipeline_get_zova_publish_stats(
    const cbm_pipeline_t *p, cbm_pipeline_zova_publish_stats_t *out_stats) {
    if (!p || !out_stats || !p->zova_publish_stats.completed) return false;
    *out_stats = p->zova_publish_stats;
    return true;
}

void cbm_pipeline_set_committed_counts(cbm_pipeline_t *p, int nodes, int edges) {
    if (p) {
        p->committed_nodes = nodes;
        p->committed_edges = edges;
    }
}

static int check_cancel(const cbm_pipeline_t *p) {
    return atomic_load(&p->cancelled) ? CBM_NOT_FOUND : 0;
}

/* ── Hash table cleanup callback ─────────────────────────────────── */

static void free_seen_dir_key(const char *key, void *val, void *ud) {
    (void)val;
    (void)ud;
    free((void *)key);
}

/* ── Pass 1: Structure ──────────────────────────────────────────── */

/* Create Project, Folder/Package, and File nodes in the graph buffer. */
/* Walk directory chain upward, creating Folder nodes and CONTAINS_FOLDER edges. */
static void create_folder_chain(cbm_pipeline_t *p, const char *dir, CBMHashTable *seen_dirs) {
    char *walk = strdup(dir);
    while (walk[0] != '\0' && !cbm_ht_get(seen_dirs, walk)) {
        cbm_ht_set(seen_dirs, strdup(walk), intptr_to_ptr(SKIP_ONE));
        char *folder_qn = cbm_pipeline_fqn_folder(p->project_name, walk);
        const char *dir_base = strrchr(walk, '/');
        dir_base = dir_base ? dir_base + SKIP_ONE : walk;
        cbm_gbuf_upsert_node(p->gbuf, "Folder", dir_base, folder_qn, walk, 0, 0, "{}");

        char *pdir = strdup(walk);
        char *ps = strrchr(pdir, '/');
        if (ps) {
            *ps = '\0';
        } else {
            free(pdir);
            pdir = strdup("");
        }
        const char *pqn;
        char *pqn_heap = NULL;
        if (pdir[0] == '\0') {
            pqn = p->branch_qn ? p->branch_qn : p->project_name;
        } else {
            pqn_heap = cbm_pipeline_fqn_folder(p->project_name, pdir);
            pqn = pqn_heap;
        }
        const cbm_gbuf_node_t *fn = cbm_gbuf_find_by_qn(p->gbuf, folder_qn);
        const cbm_gbuf_node_t *pn = cbm_gbuf_find_by_qn(p->gbuf, pqn);
        if (fn && pn) {
            cbm_gbuf_insert_edge(p->gbuf, pn->id, fn->id, "CONTAINS_FOLDER", "{}");
        }
        free(folder_qn);
        free(pqn_heap);
        char *up = strrchr(walk, '/');
        if (up) {
            *up = '\0';
        } else {
            walk[0] = '\0';
        }
        free(pdir);
    }
    free(walk);
}

static int pass_structure(cbm_pipeline_t *p, const cbm_file_info_t *files, int file_count) {
    cbm_log_info("pass.start", "pass", "structure", "files", itoa_buf(file_count));

    /* Project node */
    /* coverage_version marks a generation whose File nodes carry coverage
     * stamps; incremental runs keep the node, so older generations stay
     * unmarked until the next full index. index_format is checked by
     * try_incremental before an existing generation is reused. */
    cbm_gbuf_upsert_node(p->gbuf, "Project", p->project_name, p->project_name, NULL, 0, 0,
                         "{\"coverage_version\":" CBM_COVERAGE_VERSION_STR
                         ",\"index_format\":" CBM_INDEX_FORMAT_VERSION_STR "}");
    const char *branch_qn = p->branch_qn ? p->branch_qn : p->project_name;
    const char *branch_name = p->git_ctx.branch ? p->git_ctx.branch : "working-tree";
    char branch_props[CBM_SZ_2K];
    const char *branch_props_json = "{}";
    if (cbm_git_context_props_json(&p->git_ctx, branch_props, sizeof(branch_props)) > 0) {
        branch_props_json = branch_props;
    }
    if (p->branch_qn) {
        int64_t branch_id = cbm_gbuf_upsert_node(p->gbuf, "Branch", branch_name, branch_qn, NULL, 0,
                                                 0, branch_props_json);
        const cbm_gbuf_node_t *project_node = cbm_gbuf_find_by_qn(p->gbuf, p->project_name);
        if (project_node && branch_id > 0) {
            cbm_gbuf_insert_edge(p->gbuf, project_node->id, branch_id, "HAS_BRANCH",
                                 branch_props_json);
        }
    }

    /* Collect unique directories and create Folder/Package nodes */
    CBMHashTable *seen_dirs = cbm_ht_create(CBM_SZ_256);

    for (int i = 0; i < file_count; i++) {
        const char *rel = files[i].rel_path;
        if (!rel) {
            continue;
        }

        /* Create File node */
        char *file_qn = cbm_pipeline_fqn_compute(p->project_name, rel, "__file__");
        /* Extract basename */
        const char *slash = strrchr(rel, '/');
        const char *basename = slash ? slash + SKIP_ONE : rel;

        char props[CBM_SZ_256];
        const char *ext = strrchr(basename, '.');
        snprintf(props, sizeof(props), "{\"extension\":\"%s\"}", ext ? ext : "");

        const char *qualified_name = file_qn;
        const char *file_path = rel;
        cbm_gbuf_upsert_node(p->gbuf, "File", basename, qualified_name, file_path, 0, 0, props);

        /* CONTAINS_FILE edge: parent dir -> file */
        char *dir = strdup(rel);
        char *last_slash = strrchr(dir, '/');
        if (last_slash) {
            {
                *last_slash = '\0';
            }
        } else {
            free(dir);
            dir = strdup("");
        }

        const char *parent_qn;
        char *parent_qn_heap = NULL;
        if (dir[0] == '\0') {
            parent_qn = branch_qn;
        } else {
            parent_qn_heap = cbm_pipeline_fqn_folder(p->project_name, dir);
            parent_qn = parent_qn_heap;
        }

        /* Walk up directory chain, creating Folder nodes */
        create_folder_chain(p, dir, seen_dirs);

        /* Now create the CONTAINS_FILE edge */
        const cbm_gbuf_node_t *fnode = cbm_gbuf_find_by_qn(p->gbuf, file_qn);
        const cbm_gbuf_node_t *pnode = cbm_gbuf_find_by_qn(p->gbuf, parent_qn);
        if (fnode && pnode) {
            cbm_gbuf_insert_edge(p->gbuf, pnode->id, fnode->id, "CONTAINS_FILE", "{}");
        }

        free(file_qn);
        free(dir);
        free(parent_qn_heap);
    }

    /* Free seen_dirs keys */
    cbm_ht_foreach(seen_dirs, free_seen_dir_key, NULL);
    cbm_ht_free(seen_dirs);

    cbm_log_info("pass.done", "pass", "structure", "nodes", itoa_buf(cbm_gbuf_node_count(p->gbuf)),
                 "edges", itoa_buf(cbm_gbuf_edge_count(p->gbuf)));
    return 0;
}

/* ── Pass 2: Definitions ─────────────────────────────────────────── */

/* Implemented in pass_definitions.c via cbm_pipeline_pass_definitions() */

/* ── Githistory compute thread (for fused post-pass parallelism) ─── */

typedef struct {
    const char *repo_path;
    cbm_githistory_result_t *result;
} gh_compute_arg_t;

static void *gh_compute_thread_fn(void *arg) {
    gh_compute_arg_t *a = arg;
    cbm_pipeline_githistory_compute(a->repo_path, a->result);
    return NULL;
}

/* Extract Route nodes from URL strings found in config files (YAML, HCL, TOML).
 * These are infrastructure-defined endpoints (Cloud Scheduler, Terraform). */
/* Process infra bindings: topic→URL pairs from IaC configs.
 * Creates Route nodes for endpoints and HANDLES edges linking
 * topic Routes to endpoint Routes (bridging the gap). */
/* Process one infra binding: create Route node + INFRA_MAPS edge. */
static int process_one_infra_binding(cbm_gbuf_t *gbuf, const CBMInfraBinding *ib,
                                     const char *rel_path) {
    char url_route_qn[CBM_ROUTE_QN_SIZE];
    snprintf(url_route_qn, sizeof(url_route_qn), "__route__infra__%s", ib->target_url);
    int64_t url_route_id = cbm_gbuf_upsert_node(gbuf, "Route", ib->target_url, url_route_qn,
                                                rel_path, 0, 0, "{\"source\":\"infra\"}");
    char topic_route_qn[CBM_ROUTE_QN_SIZE];
    snprintf(topic_route_qn, sizeof(topic_route_qn), "__route__%s__%s",
             ib->broker ? ib->broker : "async", ib->source_name);
    const cbm_gbuf_node_t *topic_route = cbm_gbuf_find_by_qn(gbuf, topic_route_qn);
    int64_t topic_route_id;
    if (topic_route) {
        topic_route_id = topic_route->id;
    } else {
        /* The config file IS the declaration that the topic/queue/schedule exists;
         * upsert its Route node so the binding maps even when no code-side dispatch
         * call created the node first (e.g. a standalone scheduler/subscription
         * manifest). */
        topic_route_id = cbm_gbuf_upsert_node(gbuf, "Route", ib->source_name, topic_route_qn,
                                              rel_path, 0, 0, ib->broker ? ib->broker : "async");
        if (topic_route_id <= 0) {
            return 0;
        }
    }
    char props[CBM_SZ_512];
    snprintf(props, sizeof(props), "{\"broker\":\"%s\",\"topic\":\"%s\",\"endpoint\":\"%s\"}",
             ib->broker ? ib->broker : "async", ib->source_name, ib->target_url);
    cbm_gbuf_insert_edge(gbuf, topic_route_id, url_route_id, "INFRA_MAPS", props);
    return SKIP_ONE;
}

void cbm_pipeline_process_infra_bindings(cbm_gbuf_t *gbuf, const cbm_file_info_t *files,
                                         CBMFileResult **result_cache, cbm_result_spill_t *spill,
                                         int file_count) {
    int bindings = 0;
    for (int i = 0; i < file_count; i++) {
        if (!result_cache[i] && cbm_result_spill_infra_binding_count(spill, i) == 0) {
            continue;
        }
        CBMFileResult *result = cbm_result_cache_acquire(result_cache, spill, i);
        if (!result) {
            continue;
        }
        for (int bi = 0; bi < result->infra_bindings.count; bi++) {
            const CBMInfraBinding *ib = &result->infra_bindings.items[bi];
            if (ib->source_name && ib->target_url) {
                bindings += process_one_infra_binding(gbuf, ib, files[i].rel_path);
            }
        }
        cbm_result_cache_release(result_cache, i, result);
    }
    if (bindings > 0) {
        char buf[CBM_SZ_16];
        snprintf(buf, sizeof(buf), "%d", bindings);
        cbm_log_info("pass.infra_bindings", "linked", buf);
    }
}

bool cbm_pipeline_is_infra_file(const char *fp) {
    return fp != NULL &&
           (strstr(fp, ".yaml") != NULL || strstr(fp, ".yml") != NULL ||
            strstr(fp, ".tf") != NULL || strstr(fp, ".hcl") != NULL || strstr(fp, ".toml") != NULL);
}

/* True when a YAML key path denotes an UPSTREAM dependency, CONFIG value, or
 * HEALTHCHECK target rather than an endpoint this service exposes. Such URLs
 * (auth JWKS, downstream service base URLs, package-registry URLs, healthcheck
 * curl targets) are NOT routes the service serves and must not mint Route nodes
 * (#521). Exposed-endpoint keys (push_endpoint, post_url, callback, webhook)
 * are intentionally absent here so they still produce infra Route nodes. */
static bool is_upstream_config_key(const char *key_path) {
    if (!key_path) {
        /* No key context (e.g. flat string) — keep prior behaviour and mint. */
        return false;
    }
    static const char *const deny[] = {"jwks",     "registry",     "registries", "healthcheck",
                                       "upstream", "_service_url", "auth",       NULL};
    for (int i = 0; deny[i]; i++) {
        if (strstr(key_path, deny[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* Try to create an infra Route node from one string_ref. */
static void try_upsert_infra_route(cbm_gbuf_t *gbuf, const CBMStringRef *sr, const char *fp) {
    if (sr->kind != CBM_STRREF_URL || !sr->value || !strstr(sr->value, "://")) {
        return;
    }
    /* Skip upstream/config/healthcheck URLs — they are not exposed routes (#521). */
    if (is_upstream_config_key(sr->key_path)) {
        return;
    }
    char route_qn[CBM_ROUTE_QN_SIZE];
    snprintf(route_qn, sizeof(route_qn), "__route__infra__%s", sr->value);
    char route_props[CBM_SZ_512];
    if (sr->key_path) {
        snprintf(route_props, sizeof(route_props), "{\"source\":\"infra\",\"key_path\":\"%s\"}",
                 sr->key_path);
    } else {
        snprintf(route_props, sizeof(route_props), "{\"source\":\"infra\"}");
    }
    cbm_gbuf_upsert_node(gbuf, "Route", sr->value, route_qn, fp, 0, 0, route_props);
}

/* A URL string_ref that does NOT denote a route the service serves: a value
 * containing whitespace is a command/sentence with an embedded URL (e.g. a
 * Docker healthcheck `curl --fail http://... || exit 1`); a NULL key_path is a
 * context-less/duplicate ref; an upstream/config/healthcheck key is an external
 * dependency, not an exposed route. (#521) */
/* True when one dotted key segment names an endpoint the service is called at:
 * Pub/Sub push endpoints, Cloud Scheduler/Tasks `uri`, webhooks, callbacks,
 * post URLs. Case, '_' and '-' are ignored (pushEndpoint == push_endpoint). */
static bool is_endpoint_key_segment(const char *segment, size_t len) {
    char norm[CBM_SZ_64];
    size_t n = 0;
    for (size_t i = 0; i < len && n < sizeof(norm) - SKIP_ONE; i++) {
        char c = segment[i];
        if (c == '_' || c == '-') {
            continue;
        }
        norm[n++] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    norm[n] = '\0';
    static const char *const contains[] = {"endpoint", "webhook", "callback", "posturl", NULL};
    for (int i = 0; contains[i]; i++) {
        if (strstr(norm, contains[i])) {
            return true;
        }
    }
    return strcmp(norm, "uri") == 0;
}

/* Allow-list, not deny-list: generic keys such as `url`, `link` or `avatarUrl`
 * hold documentation and data URLs (sponsor lists, profiles, images), so only
 * endpoint-named keys mint infra Routes. Upstream/config keys stay denied. (#521) */
bool cbm_pipeline_infra_route_key_allowed(const char *key_path) {
    if (!key_path || is_upstream_config_key(key_path)) {
        return false;
    }
    const char *segment = key_path;
    for (;;) {
        const char *dot = strchr(segment, '.');
        size_t len = dot ? (size_t)(dot - segment) : strlen(segment);
        if (is_endpoint_key_segment(segment, len)) {
            return true;
        }
        if (!dot) {
            return false;
        }
        segment = dot + SKIP_ONE;
    }
}

static bool route_sr_denied(const CBMStringRef *sr) {
    if (!sr->value || strchr(sr->value, ' ')) {
        return true;
    }
    return !cbm_pipeline_infra_route_key_allowed(sr->key_path);
}

void cbm_pipeline_extract_infra_routes(cbm_gbuf_t *gbuf, const cbm_file_info_t *files,
                                       CBMFileResult **result_cache, cbm_result_spill_t *spill,
                                       int file_count) {
    /* DENY-WINS-BY-VALUE: the same URL is often extracted as several string_refs
     * at different key_path granularities (full path, leaf key, flat). The Route
     * node is keyed by VALUE, so it would be minted if ANY granularity passed the
     * per-ref guard — e.g. a denied full path `registries.terraform-registry.url`
     * is defeated by a sibling leaf `url`. So pass 1 collects every URL value
     * denied under ANY of its refs; pass 2 mints only values never denied. (#521) */
    CBMHashTable *denied = cbm_ht_create(16);
    /* `denied` borrows its keys; values from a loaded (spilled) result are
     * copied here because the result is freed before pass 2 reads them. */
    CBMArena denied_keys;
    cbm_arena_init(&denied_keys);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < file_count; i++) {
            if (!cbm_pipeline_is_infra_file(files[i].rel_path) ||
                (!result_cache[i] && cbm_result_spill_string_ref_count(spill, i) == 0)) {
                continue;
            }
            CBMFileResult *result = cbm_result_cache_acquire(result_cache, spill, i);
            if (!result) {
                continue;
            }
            const bool loaded = result != result_cache[i];
            for (int si = 0; si < result->string_refs.count; si++) {
                const CBMStringRef *sr = &result->string_refs.items[si];
                if (sr->kind != CBM_STRREF_URL || !sr->value || !strstr(sr->value, "://")) {
                    continue;
                }
                if (pass == 0) {
                    if (denied && route_sr_denied(sr) && !cbm_ht_has(denied, sr->value)) {
                        const char *key =
                            loaded ? cbm_arena_strdup(&denied_keys, sr->value) : sr->value;
                        if (key) {
                            cbm_ht_set(denied, key, (void *)1);
                        }
                    }
                } else if (!denied || !cbm_ht_has(denied, sr->value)) {
                    try_upsert_infra_route(gbuf, sr, files[i].rel_path);
                }
            }
            cbm_result_cache_release(result_cache, i, result);
        }
    }
    cbm_ht_free(denied);
    cbm_arena_destroy(&denied_keys);
}

/* Run decorator_tags, configlink, and route matching passes. */
typedef void (*predump_pass_fn)(cbm_pipeline_ctx_t *);
static void predump_deco(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_decorator_tags(ctx->gbuf, ctx->project_name);
}
static void predump_route(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_create_route_nodes(ctx->gbuf);
}
static void predump_sim(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_similarity(ctx);
}
static void predump_sem(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_semantic_edges(ctx);
}
static void predump_cfg(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_configlink(ctx);
}
static void predump_complexity(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_complexity(ctx);
}
static void predump_importance(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_importance(ctx);
}

static int run_predump_passes(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx) {
    static const struct {
        predump_pass_fn fn;
        const char *name;
        bool moderate_only; /* true = skip in fast mode */
    } passes[] = {
        {predump_deco, "decorator_tags", false}, {predump_cfg, "configlink", false},
        {predump_route, "route_match", false},   {predump_sim, "similarity", true},
        {predump_sem, "semantic_edges", true},   {predump_complexity, "complexity", false},
        /* Importance runs LAST: it reads CALLS/USAGE (extraction) and TESTS
         * (pass_tests, run before this loop). */
        {predump_importance, "importance", false},
    };
    /* Derived from the table: a hand-written count that lags an appended
     * entry silently skips the last pass. */
    enum { PREDUMP_PASS_COUNT = (int)(sizeof(passes) / sizeof(passes[0])) };
    struct timespec t;
    for (int i = 0; i < PREDUMP_PASS_COUNT && !check_cancel(p); i++) {
        /* "moderate_only" passes (similarity/semantic edges) run in FULL,
         * MODERATE and ADVANCED — they are skipped only in FAST. Compare
         * explicitly against FAST rather than `> MODERATE` so ADVANCED
         * (numerically 3) is not mistaken for a lighter mode than FULL. */
        if (passes[i].moderate_only && p->mode == CBM_MODE_FAST) {
            continue;
        }
        /* #49: each derived pass allocates whole-graph scratch. Check before
         * it starts so an over-budget run stops instead of stacking another
         * pass's working set on top. */
        int gate_rc = cbm_pipeline_budget_gate(passes[i].name);
        if (gate_rc != 0) {
            return gate_rc;
        }
        cbm_clock_gettime(CLOCK_MONOTONIC, &t);
        passes[i].fn(ctx);
        cbm_log_info("pass.timing", "pass", passes[i].name, "elapsed_ms",
                     itoa_buf((int)elapsed_ms(t)));
        cbm_pipeline_log_phase_mem(passes[i].name);
    }
    return 0;
}

/* Adapter that lets cbm_pipeline_pass_lsp_cross slot into the seq_passes
 * dispatch table. The cross-file LSP needs the per-file CBMFileResult cache
 * to read defs/imports without re-extracting; in the sequential path that
 * cache is ctx->result_cache (set up by run_sequential_pipeline before
 * launching the dispatch loop). When the cache is unavailable (e.g. if the
 * pipeline opted out of caching), the pass becomes a no-op since there are
 * no extracted results to feed cross-file resolution. */
static int seq_pass_lsp_cross_dispatch(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                                       int file_count) {
    if (!ctx || !ctx->result_cache)
        return 0;
    /* Cross-file LSP runs in every mode. */
    return cbm_pipeline_pass_lsp_cross(ctx, files, file_count, ctx->result_cache);
}

/* Run the sequential pipeline path: definitions, k8s, lsp_cross, calls, usages, semantic. */
static int run_sequential_pipeline(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                                   const cbm_file_info_t *files, int file_count,
                                   struct timespec *t) {
    cbm_log_info("pipeline.mode", "mode", "sequential", "files", itoa_buf(file_count));

    /* Build package map from manifest files (sequential: read manifests directly).
     * Use the repo-walking variant so manifests filtered out by the main
     * discoverer (package.json, composer.json) still feed pkgmap and let
     * workspace imports like `@my/pkg` resolve to their target Module. */
    ctx->pkgmap = cbm_pkgmap_build_from_repo(ctx->repo_path, files, file_count, ctx->project_name,
                                             ctx->excluded_dirs, ctx->excluded_count);

    CBMFileResult **seq_cache = (CBMFileResult **)calloc(file_count, sizeof(CBMFileResult *));
    if (seq_cache) {
        ctx->result_cache = seq_cache;
    }
    typedef int (*seq_pass_fn)(cbm_pipeline_ctx_t *, const cbm_file_info_t *, int);
    static const struct {
        seq_pass_fn fn;
        const char *name;
        bool ignore_err;
    } seq_passes[] = {
        {cbm_pipeline_pass_definitions, "definitions", false},
        {cbm_pipeline_pass_k8s, "k8s", true},
        {seq_pass_lsp_cross_dispatch, "lsp_cross", true},
        {cbm_pipeline_pass_calls, "calls", false},
        {cbm_pipeline_pass_usages, "usages", false},
        {cbm_pipeline_pass_semantic, "semantic", false},
    };
    int rc = 0;
    for (int si = 0; si < PL_SEQ_PASSES && rc == 0; si++) {
        /* #49: definitions is the extract step; every later pass re-reads the
         * whole cache/graph. Gate each one so an over-budget run stops before
         * stacking another pass's working set. */
        if (si > 0) {
            rc = cbm_pipeline_budget_gate(seq_passes[si].name);
            if (rc != 0) {
                break;
            }
        }
        cbm_clock_gettime(CLOCK_MONOTONIC, t);
        int pr = seq_passes[si].fn(ctx, files, file_count);
        if (pr != 0 && !seq_passes[si].ignore_err) {
            rc = pr;
        }
        cbm_log_info("pass.timing", "pass", seq_passes[si].name, "elapsed_ms",
                     itoa_buf((int)elapsed_ms(*t)));
        cbm_pipeline_log_phase_mem(seq_passes[si].name);
        if (check_cancel(p)) {
            rc = CBM_NOT_FOUND;
        }
    }
    /* Consume infra bindings (YAML/HCL topic/queue/scheduler → endpoint) so
     * INFRA_MAPS edges also form on the sequential path, not just the parallel
     * one. process_one_infra_binding self-creates the topic Route node when no
     * code-side dispatch created it (e.g. a standalone scheduler manifest). */
    if (seq_cache && rc == 0) {
        cbm_pipeline_extract_infra_routes(p->gbuf, files, seq_cache, NULL, file_count);
        cbm_pipeline_process_infra_bindings(p->gbuf, files, seq_cache, NULL, file_count);
    }
    if (seq_cache) {
        for (int i = 0; i < file_count; i++) {
            if (seq_cache[i]) {
                cbm_free_result(seq_cache[i]);
            }
        }
        free(seq_cache);
        ctx->result_cache = NULL;
    }
    /* Release the lsp_cross pass's shared registries only now: resolved_calls
     * borrowed registry-owned strings that the calls pass read above. */
    if (ctx->seq_cross_arena_live) {
        cbm_arena_destroy(&ctx->seq_cross_arena);
        ctx->seq_cross_arena_live = false;
    }
    return rc;
}

/* Run the parallel pipeline path: extract, registry, resolve, infra, k8s. */
static int run_parallel_passes(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                               const cbm_file_info_t *files, int file_count, int worker_count,
                               struct timespec *t) {
    cbm_log_info("pipeline.mode", "mode", "parallel", "workers", itoa_buf(worker_count), "files",
                 itoa_buf(file_count));
    _Atomic int64_t shared_ids;
    atomic_init(&shared_ids, cbm_gbuf_next_id(p->gbuf));
    CBMFileResult **cache = (CBMFileResult **)calloc(file_count, sizeof(CBMFileResult *));
    if (!cache) {
        cbm_log_error("pipeline.err", "phase", "cache_alloc");
        return CBM_NOT_FOUND;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    int rc = cbm_parallel_extract(ctx, files, file_count, cache, &shared_ids, worker_count);
    cbm_log_info("pass.timing", "pass", "parallel_extract", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(*t)));
    if (rc != 0 || check_cancel(p)) {
        free(cache);
        return rc != 0 ? rc : CBM_NOT_FOUND;
    }
    cbm_gbuf_set_next_id(p->gbuf, atomic_load(&shared_ids));
    /* extract -> registry handoff: return the extract phase's freed-but-retained
     * allocator pages to the OS before registry_build allocates. On a 2x Linux
     * index the extract peak holds ~13 GB of reclaimable pages (peak_mb 20.7 vs
     * live rss_mb 7); not returning them pushed the process over the system
     * memory-pressure threshold and got it SIGKILLed at registry entry. */
    cbm_mem_collect();
    cbm_log_info("mem.collect", "phase", "post_extract", "rss_mb",
                 itoa_buf((int)(cbm_mem_rss() / (1024 * 1024))));
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    rc = cbm_build_registry_from_cache(ctx, files, file_count, cache);
    cbm_log_info("pass.timing", "pass", "registry_build", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(*t)));
    cbm_pipeline_log_phase_mem("registry_build");
    /* #49: registry_build is the first post-extract whole-graph allocation. */
    if (rc == 0 && !check_cancel(p)) {
        rc = cbm_pipeline_budget_gate("registry_build");
    }
    if (rc != 0 || check_cancel(p)) {
        for (int i = 0; i < file_count; i++) {
            if (cache[i]) {
                cbm_free_result(cache[i]);
            }
        }
        free(cache);
        return rc != 0 ? rc : CBM_NOT_FOUND;
    }
    /* Cross-file LSP precondition: build a project-wide CBMLSPDef[]
     * once. The fused resolve_worker invokes cbm_pxc_run_one(_ts) per
     * file using these defs + the file's IMPORTS map, so cross-file
     * type-resolved CALLS land in result->resolved_calls before the
     * CALLS-edge emission. This replaces the old sequential
     * cbm_pipeline_pass_lsp_cross pass which re-read every source from
     * disk and re-parsed every tree on a single thread (~520s on
     * kubernetes). Soft-failure: NULL all_defs / NULL def_modules just
     * mean cross-file LSP no-ops; per-file LSP already ran during
     * extract. */
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    /* Cross-file LSP (type-aware call/usage resolution across files) — the
     * most expensive phase. CBM_DISABLE_LSP_CROSS=1 opts out (it can SIGSEGV
     * on large TS projects — see #340/#344); with cross-LSP off, all_defs
     * stays NULL and the fused resolver simply no-ops cross-file resolution
     * (per-file LSP already ran during extract). */
    char cbm_lsp_cross_env[CBM_SZ_16];
    const bool run_cross_lsp = cbm_safe_getenv("CBM_DISABLE_LSP_CROSS", cbm_lsp_cross_env,
                                               sizeof(cbm_lsp_cross_env), NULL) == NULL;
    if (!run_cross_lsp) {
        cbm_log_info("lsp_cross.skipped", "reason", "CBM_DISABLE_LSP_CROSS env set");
    }
    /* Owns the per-language cross registries below and the def strings of
     * spilled results, so it outlives all_defs. */
    CBMArena cross_lsp_arena;
    cbm_arena_init(&cross_lsp_arena);
    char **def_modules = NULL;
    int def_count = 0;
    CBMLSPDef *all_defs = NULL;
    if (run_cross_lsp) {
        def_modules = (char **)calloc((size_t)file_count, sizeof(char *));
        all_defs = def_modules ? cbm_pxc_collect_all_defs(ctx, cache, files, file_count,
                                                          ctx->project_name, def_modules,
                                                          &cross_lsp_arena, &def_count)
                               : NULL;
    }
    /* Build inverted index: module_qn → defs. The fused resolve_worker
     * uses this to filter the global all_defs[] down to just the defs
     * each file actually needs (own_module + imported modules) — the
     * gopls "package summary" pattern. Drops per-file registry build
     * cost from O(all_defs) to O(relevant_defs), typically 50-100×
     * smaller per file. */
    CBMModuleDefIndex *module_def_index =
        all_defs ? cbm_pxc_build_module_def_index(all_defs, def_count) : NULL;
    /* Tier 2 full: pre-build per-language cross-LSP registries.
     * Built ONCE here; shared READ-ONLY across all files of that language
     * during resolve. Per-file work is then: parse + AST walk + O(1) lookups
     * — no registry build, no Phase 1b mutations. Languages added so far:
     * Go, Python. Others (C/C++, TS/JS, PHP, C#) fall back to per-file. */
    CBMCrossLspRegistries cross_registries = {0};
    if (all_defs) {
        cross_registries.go = cbm_go_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.python =
            cbm_py_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.c = cbm_c_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.cs = cbm_cs_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.ts = cbm_ts_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.zig_args = cbm_pxc_collect_zig_args(
            ctx, cache, files, file_count, def_modules, module_def_index, all_defs, def_count,
            &cross_lsp_arena, &cross_registries.zig_arg_count);
        /* Rust: NOT built here. The shared all_defs registry is built LAZILY on the
         * first NULL-filter rust file (the amplifier files) inside cbm_parallel_resolve
         * — repos whose rust files all filter to subsets never pay the build/RSS. */
    }
    cbm_log_info("pass.timing", "pass", "lsp_cross_prepare", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(*t)));
    cbm_pipeline_log_phase_mem("lsp_cross_prepare");
    /* #49: the cross-file registries are a whole-graph working set allocated
     * after extract. Stop before resolve if they pushed RSS over the budget. */
    if (cbm_pipeline_budget_gate("lsp_cross_prepare") != 0) {
        rc = CBM_PIPELINE_ABORT_OVER_BUDGET;
    }
    if (rc == 0) {
        cbm_clock_gettime(CLOCK_MONOTONIC, t);
        rc = cbm_parallel_resolve(ctx, files, file_count, cache, &shared_ids, worker_count, all_defs,
                                  def_count, def_modules, module_def_index, &cross_registries);
        cbm_log_info("pass.timing", "pass", "parallel_resolve", "elapsed_ms",
                     itoa_buf((int)elapsed_ms(*t)));
        cbm_pipeline_log_phase_mem("parallel_resolve");
    }
    cbm_pxc_free_module_def_index(module_def_index);
    free((void *)cross_registries.zig_args);
    cbm_arena_destroy(&cross_lsp_arena); /* releases all per-lang registries */
    free(all_defs);
    if (def_modules) {
        for (int i = 0; i < file_count; i++) {
            free(def_modules[i]);
        }
        free(def_modules);
    }
    cbm_gbuf_set_next_id(p->gbuf, atomic_load(&shared_ids));
    /* #49: resolve's per-worker edge buffers are merged into the main gbuf
     * here; if that merge pushed RSS over budget, stop before the derived
     * passes and publication. */
    if (rc == 0 && cbm_pipeline_budget_gate("parallel_resolve") != 0) {
        rc = CBM_PIPELINE_ABORT_OVER_BUDGET;
    }
    if (rc == 0) {
        cbm_pipeline_extract_infra_routes(p->gbuf, files, cache, ctx->spill, file_count);
        cbm_pipeline_process_infra_bindings(p->gbuf, files, cache, ctx->spill, file_count);
    }
    for (int i = 0; i < file_count; i++) {
        if (cache[i]) {
            cbm_free_result(cache[i]);
        }
    }
    free(cache);
    if (rc != 0) {
        return rc;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    cbm_pipeline_pass_k8s(ctx, files, file_count);
    cbm_log_info("pass.timing", "pass", "k8s", "elapsed_ms", itoa_buf((int)elapsed_ms(*t)));
    return check_cancel(p) ? CBM_NOT_FOUND : 0;
}

/* run_parallel_passes with a spill store (F1) the extract can park results
 * in when it runs over its memory budget. The store's file lives under the
 * cache dir and is deleted when the run ends. */
static int run_parallel_pipeline(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                                 const cbm_file_info_t *files, int file_count, int worker_count,
                                 struct timespec *t) {
    const char *cache_dir = cbm_resolve_cache_dir();
    char spill_dir[CBM_SZ_1K];
    if (cache_dir && snprintf(spill_dir, sizeof(spill_dir), "%s/spill", cache_dir) <
                         (int)sizeof(spill_dir)) {
        ctx->spill = cbm_result_spill_open(spill_dir, file_count);
    }
    int rc = run_parallel_passes(p, ctx, files, file_count, worker_count, t);
    if (rc == 0 && cbm_result_spill_load_failures(ctx->spill) > 0) {
        /* A pass ran without some parked results: never publish that graph. */
        cbm_log_error("mem.spill.failed", "load_failures",
                      itoa_buf((int)cbm_result_spill_load_failures(ctx->spill)));
        rc = CBM_NOT_FOUND;
    }
    if (ctx->spill) {
        int64_t parked = 0;
        int64_t bytes = 0;
        int64_t loads = 0;
        cbm_result_spill_stats(ctx->spill, &parked, &bytes, &loads);
        if (parked > 0) {
            cbm_log_info("mem.spill.done", "parked", itoa_buf((int)parked), "mb",
                         itoa_buf((int)(bytes / (1024 * 1024))), "loads", itoa_buf((int)loads));
        }
        cbm_result_spill_close(ctx->spill);
        ctx->spill = NULL;
    }
    return rc;
}

enum { PIPELINE_INCREMENTAL_FATAL = -2 };

/* True when the stored generation's Project node carries the current
 * index_format. Unstamped generations predate the stamp. */
static bool stored_index_format_current(const cbm_gbuf_t *stored, const char *project) {
    static const char key[] = "\"index_format\":" CBM_INDEX_FORMAT_VERSION_STR;
    const cbm_gbuf_node_t *node = cbm_gbuf_find_by_qn(stored, project);
    const char *hit = node && node->properties_json ? strstr(node->properties_json, key) : NULL;
    return hit && !isdigit((unsigned char)hit[sizeof(key) - 1]);
}

/* Try the incremental pipeline against the project's committed cbm.zova
 * workspace. Returns >= 0 if incremental was used (the return code), or -1 to
 * proceed with full. CBM_PIPELINE_ABORT_OVER_BUDGET passes through unchanged. */
static int try_incremental(cbm_pipeline_t *p, cbm_file_info_t *files, int file_count) {
    char zova_path[CBM_SZ_1K];
    if (cbm_zova_user_database_path(zova_path, sizeof(zova_path)) == 0) {
        cbm_zova_repository_t *repo = cbm_zova_repository_open(zova_path, p->project_name);
        if (repo) {
            cbm_project_summary_export_t summary = {0};
            const char *workspace_id = cbm_zova_repository_workspace_id(repo);
            if (cbm_zova_repository_project_summary(repo, workspace_id, &summary) ==
                CBM_STORE_OK) {
                cbm_store_project_summary_export_free(&p->saved_summary);
                p->saved_summary = summary;
                p->saved_summary_valid = true;
            }
            cbm_zova_workspace_snapshot_t snapshot = {0};
            cbm_gbuf_t *preloaded = cbm_gbuf_new(p->project_name, p->repo_path);
            if (preloaded && cbm_zova_repository_export_incremental_snapshot_to_graph(
                    zova_path, workspace_id, &snapshot, preloaded) == 0) {
                int hash_count = snapshot.file_hash_count;
                if (hash_count > 0 && !stored_index_format_current(preloaded, p->project_name)) {
                    /* Reusing it would mix old-format QNs with new ones. */
                    cbm_log_info("pipeline.route", "path", "format_change_reindex",
                                 "required_format", CBM_INDEX_FORMAT_VERSION_STR,
                                 "authority", "zova");
                    p->format_migration = true;
                } else if (hash_count > 0 &&
                           file_count <= hash_count + (hash_count / PAIR_LEN)) {
                    cbm_log_info("pipeline.route", "path", "incremental", "stored_hashes",
                                 itoa_buf(hash_count), "authority", "zova");
                    /* The exported snapshot owns all of its data. Release the
                     * read transaction before publishing the next generation,
                     * otherwise it pins the old WAL and prevents checkpointing. */
                    cbm_zova_repository_close(repo);
                    p->last_route = CBM_PIPELINE_ROUTE_INCREMENTAL;
                    int rc = cbm_pipeline_run_incremental_zova(p, zova_path, files, file_count,
                                                               &snapshot, preloaded);
                    preloaded = NULL;
                    cbm_zova_workspace_snapshot_free(&snapshot);
                    if (rc == CBM_PIPELINE_ABORT_OVER_BUDGET) {
                        return rc;
                    }
                    return rc == 0 ? 0 : PIPELINE_INCREMENTAL_FATAL;
                }
                if (hash_count > 0 && !p->format_migration) {
                    cbm_log_info("pipeline.route", "path", "mode_change_reindex",
                                 "stored_hashes", itoa_buf(hash_count), "discovered",
                                 itoa_buf(file_count), "authority", "zova");
                }
                cbm_zova_workspace_snapshot_free(&snapshot);
            }
            cbm_gbuf_free(preloaded);
            cbm_zova_repository_close(repo);
        }
    }
    return CBM_NOT_FOUND;
}

/* Get platform-specific mtime in nanoseconds. */
static int64_t stat_mtime_ns(const struct stat *fst) {
#ifdef __APPLE__
    return ((int64_t)fst->st_mtimespec.tv_sec * PL_NSEC_PER_SEC) +
           (int64_t)fst->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (int64_t)fst->st_mtime * 1000000000LL;
#else
    return ((int64_t)fst->st_mtim.tv_sec * PL_NSEC_PER_SEC) + (int64_t)fst->st_mtim.tv_nsec;
#endif
}

static int pipeline_publish_zova_user_database(
    cbm_pipeline_t *p, cbm_gbuf_t *gbuf, const cbm_file_info_t *files, int file_count,
    const cbm_file_hash_t *mode_skipped, int mode_skipped_count,
    cbm_zova_workspace_snapshot_t *before) {
    if (!p || !gbuf || file_count < 0 || mode_skipped_count < 0 ||
        (file_count > 0 && !files) || (mode_skipped_count > 0 && !mode_skipped)) {
        return CBM_NOT_FOUND;
    }
    p->zova_publish_ms = 0.0;
    memset(&p->zova_publish_stats, 0, sizeof(p->zova_publish_stats));
    struct timespec route_started = {0};
    cbm_clock_gettime(CLOCK_MONOTONIC, &route_started);
    int capacity = file_count + mode_skipped_count;
    cbm_zova_file_hash_input_t *hashes =
        capacity > 0 ? calloc((size_t)capacity, sizeof(*hashes)) : NULL;
    if (capacity > 0 && !hashes) return CBM_NOT_FOUND;
    int count = 0;
    for (int i = 0; i < file_count; i++) {
        struct stat fst;
        if (stat(files[i].path, &fst) != 0) continue;
        hashes[count++] = (cbm_zova_file_hash_input_t){
            .file_path = files[i].rel_path,
            .content_hash = "",
            .mtime_ns = stat_mtime_ns(&fst),
            .size_bytes = fst.st_size,
        };
    }
    for (int i = 0; i < mode_skipped_count; i++) {
        hashes[count++] = (cbm_zova_file_hash_input_t){
            .file_path = mode_skipped[i].rel_path,
            .content_hash = mode_skipped[i].sha256 ? mode_skipped[i].sha256 : "",
            .mtime_ns = mode_skipped[i].mtime_ns,
            .size_bytes = mode_skipped[i].size,
        };
    }
    cbm_zova_project_summary_input_t summary = {0};
    if (p->saved_summary_valid) {
        summary.present = true;
        summary.summary = p->saved_summary.summary;
        summary.source_hash = p->saved_summary.source_hash;
        summary.created_at = p->saved_summary.created_at;
        summary.updated_at = p->saved_summary.updated_at;
    }
    cbm_zova_workspace_generation_result_t result = {0};
    int rc = before
        ? cbm_gbuf_publish_zova_user_database_delta(gbuf, before, hashes, count, &summary, &result)
        : cbm_gbuf_publish_zova_user_database(gbuf, hashes, count, &summary, &result);
    free(hashes);
    if (rc != 0) {
        cbm_log_error("zova.single_file_publish", "project", p->project_name,
                      "reason", "publication_failed");
        return rc;
    }
    p->zova_publish_ms = result.publish_ms;
    p->zova_publish_stats = (cbm_pipeline_zova_publish_stats_t){
        .completed = true,
        .delta = result.publication_mode == CBM_ZOVA_PUBLICATION_MODE_DELTA,
        .full_clear_count = result.full_clear_count,
        .unchanged_rewrite_count = result.unchanged_rewrite_count,
        .nodes_inserted = result.nodes_inserted,
        .nodes_updated = result.nodes_updated,
        .nodes_deleted = result.nodes_deleted,
        .edges_inserted = result.edges_inserted,
        .edges_deleted = result.edges_deleted,
        .node_vectors_upserted = result.node_vectors_upserted,
        .node_vectors_deleted = result.node_vectors_deleted,
        .token_vectors_upserted = result.token_vectors_upserted,
        .token_vectors_deleted = result.token_vectors_deleted,
        .route_ms = elapsed_ms(route_started),
        .normalization_ms = result.normalization_ms,
        .model_nodes_ms = result.model_nodes_ms,
        .model_edges_ms = result.model_edges_ms,
        .model_edge_endpoint_ms = result.model_edge_endpoint_ms,
        .model_edge_sort_ms = result.model_edge_sort_ms,
        .model_edge_group_ms = result.model_edge_group_ms,
        .model_edge_payload_ms = result.model_edge_payload_ms,
        .model_edge_digest_ms = result.model_edge_digest_ms,
        .model_edge_default_payloads = result.model_edge_default_payloads,
        .model_edge_payload_scratch_edges =
            result.model_edge_payload_scratch_edges,
        .model_hashes_ms = result.model_hashes_ms,
        .model_vectors_ms = result.model_vectors_ms,
        .model_digests_ms = result.model_digests_ms,
        .writer_guard_ms = result.writer_guard_ms,
        .database_init_ms = result.database_init_ms,
        .database_open_ms = result.database_open_ms,
        .transaction_begin_ms = result.transaction_begin_ms,
        .transaction_body_ms = result.transaction_body_ms,
        .transaction_commit_ms = result.transaction_commit_ms,
        .database_close_ms = result.database_close_ms,
        .clear_ms = result.clear_ms,
        .finalize_ms = result.finalize_ms,
        .canonical_files_ms = result.canonical_files_ms,
        .canonical_nodes_ms = result.canonical_nodes_ms,
        .canonical_edges_ms = result.canonical_edges_ms,
        .canonical_hashes_ms = result.canonical_hashes_ms,
        .fts_ms = result.fts_ms,
        .token_metadata_ms = result.token_metadata_ms,
        .native_graph_ms = result.native_graph_ms,
        .native_graph_materialize_ms = result.native_graph_materialize_ms,
        .native_graph_reset_ms = result.native_graph_reset_ms,
        .native_graph_nodes_ms = result.native_graph_nodes_ms,
        .native_graph_edges_ms = result.native_graph_edges_ms,
        .native_graph_validate_ms = result.native_graph_validate_ms,
        .native_graph_key_generation_ms = result.native_graph_key_generation_ms,
        .native_graph_cleanup_ms = result.native_graph_cleanup_ms,
        .native_vectors_ms = result.native_vectors_ms,
        .fresh_validation_ms = result.fresh_validation_ms,
        .fresh_index_ms = result.fresh_index_ms,
        .fresh_commit_ms = result.fresh_commit_ms,
        .fresh_build_ms = result.fresh_build_ms,
        .readback_ms = result.readback_ms,
        .snapshot_completed = before != NULL,
        .snapshot_base_ms = before ? before->metrics.base_ms : 0.0,
        .snapshot_optional_ms = before ? before->metrics.optional_ms : 0.0,
        .snapshot_open_ms = before ? before->metrics.open_ms : 0.0,
        .snapshot_header_ms = before ? before->metrics.header_ms : 0.0,
        .snapshot_integrity_ms = before ? before->metrics.integrity_ms : 0.0,
        .snapshot_nodes_sql_ms = before ? before->metrics.nodes_sql_ms : 0.0,
        .snapshot_nodes_native_ms = before ? before->metrics.nodes_native_ms : 0.0,
        .snapshot_nodes_finalize_ms = before ? before->metrics.nodes_finalize_ms : 0.0,
        .snapshot_edges_sql_ms = before ? before->metrics.edges_sql_ms : 0.0,
        .snapshot_edges_native_ms = before ? before->metrics.edges_native_ms : 0.0,
        .snapshot_edges_finalize_ms = before ? before->metrics.edges_finalize_ms : 0.0,
        .snapshot_hashes_summary_ms = before ? before->metrics.hashes_summary_ms : 0.0,
        .snapshot_close_ms = before ? before->metrics.close_ms : 0.0,
        .snapshot_graph_buffer_ms = before ? before->metrics.graph_buffer_ms : 0.0,
        .snapshot_base_phase_mask = before ? before->metrics.base_phase_mask : 0,
        .snapshot_node_rows = before ? before->metrics.node_rows : 0,
        .snapshot_edge_rows = before ? before->metrics.edge_rows : 0,
        .snapshot_file_hash_rows = before ? before->metrics.file_hash_rows : 0,
        .snapshot_hydrated_components = before ? before->hydrated_components : 0,
        .snapshot_topology_rows = before ? before->metrics.topology_rows : 0,
        .snapshot_node_vector_rows = before ? before->metrics.node_vector_rows : 0,
        .snapshot_token_vector_rows = before ? before->metrics.token_vector_rows : 0,
        .snapshot_generation = before ? before->generation : 0,
    };
    char generation_text[32], publish_ms_text[32], inserted_text[32], updated_text[32], deleted_text[32];
    snprintf(generation_text,sizeof(generation_text),"%lld",(long long)result.generation);
    snprintf(publish_ms_text,sizeof(publish_ms_text),"%.3f",result.publish_ms);
    snprintf(inserted_text,sizeof(inserted_text),"%llu",(unsigned long long)result.inserted_count);
    snprintf(updated_text,sizeof(updated_text),"%llu",(unsigned long long)result.updated_count);
    snprintf(deleted_text,sizeof(deleted_text),"%llu",(unsigned long long)result.deleted_count);
    cbm_log_info("zova.single_file_publish", "project", p->project_name,
                 "generation", generation_text,
                 "publish_ms", publish_ms_text,
                 "publication_mode",
                 result.publication_mode == CBM_ZOVA_PUBLICATION_MODE_DELTA ? "delta" : "full",
                 "inserted", inserted_text,
                 "updated", updated_text,
                 "deleted", deleted_text);
    return 0;
}

int cbm_pipeline_publish_zova_user_database(
    cbm_pipeline_t *p, cbm_gbuf_t *gbuf, const cbm_file_info_t *files, int file_count,
    const cbm_file_hash_t *mode_skipped, int mode_skipped_count) {
    return pipeline_publish_zova_user_database(p,gbuf,files,file_count,mode_skipped,
                                               mode_skipped_count,NULL);
}

int cbm_pipeline_publish_zova_user_database_delta(
    cbm_pipeline_t *p, cbm_gbuf_t *gbuf, const cbm_file_info_t *files, int file_count,
    const cbm_file_hash_t *mode_skipped, int mode_skipped_count,
    cbm_zova_workspace_snapshot_t *before) {
    if(!before)return CBM_NOT_FOUND;
    return pipeline_publish_zova_user_database(p,gbuf,files,file_count,mode_skipped,
                                               mode_skipped_count,before);
}

/* Publish the graph and file hashes to the shared cbm.zova. */
static int dump_and_persist_hashes(cbm_pipeline_t *p, const cbm_file_info_t *files, int file_count,
                                   struct timespec *t) {
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    /* #49: dump + publish materialize the whole graph again (normalization,
     * native topology, FTS, vectors). Check before starting; nothing has been
     * published yet, so an abort leaves the previous generation serving. */
    cbm_pipeline_log_phase_mem("pre_dump");
    int gate_rc = cbm_pipeline_budget_gate("pre_dump");
    if (gate_rc != 0) {
        return gate_rc;
    }
    char zova_dir[CBM_SZ_1K];
    if (cbm_zova_user_database_path(zova_dir, sizeof(zova_dir)) == 0) {
        char *last_slash = strrchr(zova_dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            cbm_mkdir_p(zova_dir, CBM_DIR_PERMS);
        }
    }
    /* Capture committed counts before preparing the dump, which releases the
     * graph buffer indexes (graph_buffer.c), after which cbm_gbuf_node_count()
     * returns 0 and the #334 plausibility gate would never fire. */
    p->committed_nodes = cbm_gbuf_node_count(p->gbuf);
    p->committed_edges = cbm_gbuf_edge_count(p->gbuf);
    int prepare_rc = cbm_gbuf_prepare_zova_dump(p->gbuf);
    if (prepare_rc != 0) {
        cbm_log_error("pipeline.err", "phase", "zova_prepare");
        return prepare_rc;
    }
    cbm_pipeline_log_phase_mem("zova_prepare");
    /* Preparing the dump releases the gbuf indexes; if RSS is still over
     * budget now, publishing would only raise it further. Abort before the
     * atomic publication starts. */
    gate_rc = cbm_pipeline_budget_gate("zova_publish");
    if (gate_rc != 0) {
        return gate_rc;
    }
    int publish_rc = cbm_pipeline_publish_zova_user_database(p, p->gbuf, files, file_count, NULL, 0);
    if (publish_rc != 0) {
        cbm_log_error("pipeline.err", "phase", "zova_single_file");
        return publish_rc;
    }
    cbm_pipeline_log_phase_mem("zova_publish");
    return 0;
}

/* Run githistory pass. */
static int run_githistory(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx) {
    struct timespec t_gh;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t_gh);

    cbm_githistory_result_t gh_result = {0};
    cbm_thread_t gh_thread;
    bool gh_threaded = false;
    gh_compute_arg_t gh_arg = {.repo_path = ctx->repo_path, .result = &gh_result};

    if (p->mode != CBM_MODE_FAST) {
        if (cbm_default_worker_count(true) > SKIP_ONE) {
            if (cbm_thread_create(&gh_thread, 0, gh_compute_thread_fn, &gh_arg) == 0) {
                gh_threaded = true;
            }
        }
        if (!gh_threaded) {
            cbm_pipeline_githistory_compute(ctx->repo_path, &gh_result);
            cbm_log_info("pass.timing", "pass", "githistory_compute", "elapsed_ms",
                         itoa_buf((int)elapsed_ms(t_gh)));
        }
    } else {
        cbm_log_info("pass.skip", "pass", "githistory", "reason", "fast_mode");
    }

    if (gh_threaded) {
        cbm_thread_join(&gh_thread);
        cbm_log_info("pass.timing", "pass", "githistory_compute", "elapsed_ms",
                     itoa_buf((int)elapsed_ms(t_gh)));
    }

    int gh_edges = 0;
    if (gh_result.count > 0 || gh_result.file_temporal_count > 0) {
        gh_edges = cbm_pipeline_githistory_apply(ctx, &gh_result);
    }
    cbm_log_info("pass.done", "pass", "githistory", "commits", itoa_buf(gh_result.commit_count),
                 "edges", itoa_buf(gh_edges));
    free(gh_result.couplings);
    free(gh_result.file_temporal);
    return 0;
}

/* ── Pipeline run ────────────────────────────────────────────────── */

/* Run tests + git history. Returns 0 on success. */
static int run_tests_and_history(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                                 const cbm_file_info_t *files, int file_count) {
    struct timespec t;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    CBM_PROF_START(t_tests);
    int rc = cbm_pipeline_pass_tests(ctx, files, file_count);
    CBM_PROF_END_N("pipeline", "pass_tests", t_tests, file_count);
    cbm_log_info("pass.timing", "pass", "tests", "elapsed_ms", itoa_buf((int)elapsed_ms(t)));
    if (rc == 0 && !check_cancel(p)) {
        CBM_PROF_START(t_gh);
        rc = run_githistory(p, ctx);
        CBM_PROF_END("pipeline", "pass_githistory", t_gh);
    }
    if (check_cancel(p)) {
        return CBM_NOT_FOUND;
    }
    return rc;
}

/* Run tests, git history, predump passes, and dump+persist. */
static int run_post_extraction(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                               const cbm_file_info_t *files, int file_count) {
    /* #49: extract already enforces the budget. Everything from here on works
     * on the whole graph, so gate the entry too. */
    cbm_pipeline_log_phase_mem("post_extract");
    int rc = cbm_pipeline_budget_gate("post_extract");
    if (rc != 0) {
        return rc;
    }
    rc = run_tests_and_history(p, ctx, files, file_count);
    if (rc != 0) {
        return rc;
    }
    cbm_pipeline_log_phase_mem("tests_and_history");

    CBM_PROF_START(t_predump);
    rc = run_predump_passes(p, ctx);
    CBM_PROF_END("pipeline", "3_predump_passes_total", t_predump);
    if (rc != 0) {
        return rc;
    }

    if (!check_cancel(p)) {
        struct timespec t;
        CBM_PROF_START(t_dump);
        rc = dump_and_persist_hashes(p, files, file_count, &t);
        CBM_PROF_END("pipeline", "4_dump_and_persist", t_dump);
    }
    return rc;
}

#define MIN_FILES_FOR_PARALLEL 50

/* Run structure + extraction passes (parallel or sequential). */
static int run_extraction_phase(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                                const cbm_file_info_t *files, int file_count) {
    struct timespec t;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    CBM_PROF_START(t_struct);
    pass_structure(p, files, file_count);
    CBM_PROF_END_N("pipeline", "pass_structure", t_struct, file_count);
    cbm_log_info("pass.timing", "pass", "structure", "elapsed_ms", itoa_buf((int)elapsed_ms(t)));
    if (check_cancel(p)) {
        return CBM_NOT_FOUND;
    }

    int worker_count = cbm_default_worker_count(true);
    CBM_PROF_START(t_extract_total);
    int rc = (worker_count > SKIP_ONE && file_count > MIN_FILES_FOR_PARALLEL)
                 ? run_parallel_pipeline(p, ctx, files, file_count, worker_count, &t)
                 : run_sequential_pipeline(p, ctx, files, file_count, &t);
    CBM_PROF_END_N("pipeline", "2_extraction_total", t_extract_total, file_count);
    if (check_cancel(p)) {
        return CBM_NOT_FOUND;
    }
    if (rc == 0) {
        cbm_pipeline_stamp_coverage(p, p->gbuf);
    }
    return rc;
}

int cbm_pipeline_run(cbm_pipeline_t *p) {
    if (!p) {
        return CBM_NOT_FOUND;
    }
    p->last_route = CBM_PIPELINE_ROUTE_UNKNOWN;
    p->format_migration = false;

    CBM_PROF_START(t_pipeline_total);
    struct timespec t0;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t0);
    cbm_path_alias_collection_t *path_aliases = NULL;
    cbm_pipeline_ctx_t ctx = {0}; /* zeroed first: cleanup reads ctx.pkgmap */

    /* C/C++ #define Macro nodes (#375) dominate extraction on macro-dense repos
     * (≈49% of nodes on the Linux kernel), so gate them to full mode — moderate
     * and fast skip them entirely. Set before any extraction dispatch. */
    cbm_set_macro_extraction(p->mode == CBM_MODE_FULL);

    /* Load user-defined extension overrides (fail-open: NULL on error) */
    CBM_PROF_START(t_userconfig);
    p->userconfig = cbm_userconfig_load(p->repo_path);
    cbm_set_user_lang_config(p->userconfig);
    CBM_PROF_END("pipeline", "0_userconfig_load", t_userconfig);

    /* Phase 1: Discover files */
    CBM_PROF_START(t_discover);
    cbm_discover_opts_t opts = {
        .mode = p->mode,
        .ignore_file = NULL,
        .max_file_size = 0,
    };
    cbm_file_info_t *files = NULL;
    int file_count = 0;
    /* Capture skipped subtrees on the pipeline so the MCP layer can report
     * which directories were excluded (#411). Replace any prior list (e.g. a
     * re-run on the same pipeline) to avoid leaking the previous one. */
    cbm_discover_free_excluded(p->excluded_dirs, p->excluded_count);
    p->excluded_dirs = NULL;
    p->excluded_count = 0;
    bool discover_degraded = false;
    int rc = cbm_discover_ex(p->repo_path, &opts, &files, &file_count, &p->excluded_dirs,
                             &p->excluded_count, &discover_degraded);
    p->discover_degraded = discover_degraded;
    if (discover_degraded) {
        cbm_log_warn("pipeline.discover", "degraded", "true");
    }
    if (rc != 0) {
        cbm_log_error("pipeline.err", "phase", "discover", "rc", itoa_buf(rc));
    }
    CBM_PROF_END_N("pipeline", "1_discover", t_discover, file_count);
    cbm_log_info("pipeline.discover", "files", itoa_buf(file_count), "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t0)));
    if (rc != 0 || check_cancel(p)) {
        rc = CBM_NOT_FOUND;
        goto cleanup;
    }

    /* Check for existing DB → try incremental or delete for reindex */
    rc = try_incremental(p, files, file_count);
    if (rc == PIPELINE_INCREMENTAL_FATAL) {
        cbm_discover_free(files, file_count);
        return CBM_NOT_FOUND;
    }
    if (rc == CBM_PIPELINE_ABORT_OVER_BUDGET) {
        /* A full run would hit the same budget; the served generation stays. */
        cbm_discover_free(files, file_count);
        return rc;
    }
    if (rc >= 0) {
        cbm_discover_free(files, file_count);
        return rc;
    }
    cbm_log_info("pipeline.route", "path", "full");
    p->last_route = CBM_PIPELINE_ROUTE_FULL;

    /* Phase 2: Create graph buffer and registry */
    p->gbuf = cbm_gbuf_new(p->project_name, p->repo_path);
    p->registry = cbm_registry_new();

    /* Phase 2b: Load build-tool path aliases (tsconfig/jsconfig today). NULL
     * when no usable configs are found — non-TS projects pay nothing. */
    path_aliases =
        cbm_load_path_aliases_excluded(p->repo_path, p->excluded_dirs, p->excluded_count);

    /* Build shared context for pass functions */
    ctx = (cbm_pipeline_ctx_t){
        .project_name = p->project_name,
        .repo_path = p->repo_path,
        .gbuf = p->gbuf,
        .registry = p->registry,
        .cancelled = &p->cancelled,
        .pipeline = p, /* so passes can record per-file skips (Track B) */
        .mode = (int)p->mode,
        .path_aliases = path_aliases,
        .excluded_dirs = p->excluded_dirs,
        .excluded_count = p->excluded_count,
    };

    rc = run_extraction_phase(p, &ctx, files, file_count);
    if (rc != 0) {
        goto cleanup;
    }

    rc = run_post_extraction(p, &ctx, files, file_count);
    if (rc != 0) {
        goto cleanup;
    }

    cbm_pipeline_log_phase_mem("pipeline_done");
    cbm_log_info("pipeline.done", "nodes", itoa_buf(cbm_gbuf_node_count(p->gbuf)), "edges",
                 itoa_buf(cbm_gbuf_edge_count(p->gbuf)), "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t0)));
    CBM_PROF_END("pipeline", "TOTAL", t_pipeline_total);

cleanup:
    cbm_pkgmap_free(ctx.pkgmap);
    ctx.pkgmap = NULL;
    cbm_discover_free(files, file_count);
    cbm_gbuf_free(p->gbuf);
    p->gbuf = NULL;
    cbm_registry_free(p->registry);
    p->registry = NULL;
    cbm_path_alias_collection_free(path_aliases);
    /* Clear and free user extension config */
    cbm_set_user_lang_config(NULL);
    cbm_userconfig_free(p->userconfig);
    p->userconfig = NULL;
    return rc;
}
