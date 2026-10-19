/*
 * pipeline_incremental.c — Disk-based incremental re-indexing.
 *
 * Operates on the existing SQLite DB directly (not RAM-first graph buffer).
 * Compares file mtime+size against stored hashes to classify changed/unchanged.
 * Deletes changed files' nodes (edges cascade via ON DELETE CASCADE),
 * re-parses only changed files through passes into a temp graph buffer,
 * then merges new nodes/edges into the disk DB. Persists updated hashes.
 *
 * Called from pipeline.c when a DB with stored hashes already exists.
 */
#include "foundation/constants.h"

enum { INCR_RING_BUF = 4, INCR_RING_MASK = 3, INCR_TS_BUF = 24 };
#include "pipeline/pipeline.h"
#include <stdio.h>
#include <time.h>
#include "pipeline/pipeline_internal.h"
#include "pipeline/lsp_resolve.h" /* cbm_pipeline_node_is_callable_target */
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_repository.h"
#include "graph_buffer/graph_buffer.h"
#include "discover/discover.h"
#include "foundation/log.h"
#include "foundation/hash_table.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/sha256_backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdint.h>

/* ── Constants ───────────────────────────────────────────────────── */

#define CBM_MS_PER_SEC 1000.0
#define CBM_NS_PER_MS 1000000.0
#define CBM_NS_PER_SEC 1000000000LL

/* ── Timing helper (same as pipeline.c) ──────────────────────────── */

static double elapsed_ms(struct timespec start) {
    struct timespec now;
    cbm_clock_gettime(CLOCK_MONOTONIC, &now);
    double s = (double)(now.tv_sec - start.tv_sec);
    double ns = (double)(now.tv_nsec - start.tv_nsec);
    return (s * CBM_MS_PER_SEC) + (ns / CBM_NS_PER_MS);
}

typedef struct {
    uint64_t xor_words[4];
    uint64_t sum_words[4];
    uint64_t count;
} cbm_incremental_multiset_digest_t;

typedef struct {
    cbm_incremental_multiset_digest_t input;
    cbm_incremental_multiset_digest_t config;
    cbm_incremental_multiset_digest_t similarity;
    cbm_incremental_multiset_digest_t semantic;
} cbm_incremental_derived_state_t;

static void derived_hash_text(cbm_sha256_backend_ctx *hash, const char *text) {
    const char *value = text ? text : "";
    cbm_sha256_backend_update(hash, value, strlen(value));
    cbm_sha256_backend_update(hash, "\0", 1);
}

static void derived_digest_add(cbm_incremental_multiset_digest_t *set,
                               cbm_sha256_backend_ctx *hash) {
    uint8_t bytes[CBM_SHA256_DIGEST_LEN];
    uint64_t words[4];
    cbm_sha256_backend_final(hash, bytes);
    memcpy(words, bytes, sizeof(words));
    for (int i = 0; i < 4; i++) {
        set->xor_words[i] ^= words[i];
        set->sum_words[i] += words[i];
    }
    set->count++;
}

static bool derived_digest_equal(const cbm_incremental_multiset_digest_t *left,
                                 const cbm_incremental_multiset_digest_t *right) {
    return left->count == right->count &&
           memcmp(left->xor_words, right->xor_words, sizeof(left->xor_words)) == 0 &&
           memcmp(left->sum_words, right->sum_words, sizeof(left->sum_words)) == 0;
}

typedef struct {
    const cbm_gbuf_t *gbuf;
    cbm_incremental_derived_state_t *state;
} cbm_incremental_derived_scan_t;

static void derived_node_visitor(const cbm_gbuf_node_t *node, void *userdata) {
    cbm_incremental_derived_scan_t *scan = userdata;
    cbm_sha256_backend_ctx hash;
    cbm_sha256_backend_init(&hash);
    derived_hash_text(&hash, "node");
    derived_hash_text(&hash, node->label);
    derived_hash_text(&hash, node->name);
    derived_hash_text(&hash, node->qualified_name);
    derived_hash_text(&hash, node->file_path);
    cbm_sha256_backend_update(&hash, &node->start_line, sizeof(node->start_line));
    cbm_sha256_backend_update(&hash, &node->end_line, sizeof(node->end_line));
    derived_hash_text(&hash, node->properties_json);
    derived_digest_add(&scan->state->input, &hash);
}

static void derived_edge_visitor(const cbm_gbuf_edge_t *edge, void *userdata) {
    cbm_incremental_derived_scan_t *scan = userdata;
    cbm_incremental_multiset_digest_t *target = &scan->state->input;
    if (strcmp(edge->type, "CONFIGURES") == 0) target = &scan->state->config;
    else if (strcmp(edge->type, "SIMILAR_TO") == 0) target = &scan->state->similarity;
    else if (strcmp(edge->type, "SEMANTICALLY_RELATED") == 0)
        target = &scan->state->semantic;
    const cbm_gbuf_node_t *source = cbm_gbuf_find_by_id(scan->gbuf, edge->source_id);
    const cbm_gbuf_node_t *destination = cbm_gbuf_find_by_id(scan->gbuf, edge->target_id);
    cbm_sha256_backend_ctx hash;
    cbm_sha256_backend_init(&hash);
    derived_hash_text(&hash, "edge");
    derived_hash_text(&hash, source ? source->qualified_name : NULL);
    derived_hash_text(&hash, edge->type);
    derived_hash_text(&hash, destination ? destination->qualified_name : NULL);
    derived_hash_text(&hash, edge->properties_json);
    derived_digest_add(target, &hash);
}

static cbm_incremental_derived_state_t derived_state(const cbm_gbuf_t *gbuf) {
    cbm_incremental_derived_state_t state = {0};
    cbm_incremental_derived_scan_t scan = {.gbuf = gbuf, .state = &state};
    cbm_gbuf_foreach_node(gbuf, derived_node_visitor, &scan);
    cbm_gbuf_foreach_edge(gbuf, derived_edge_visitor, &scan);
    return state;
}

/* itoa into static buffer — matches pipeline.c helper */
static const char *itoa_buf(int v) {
    static _Thread_local char buf[INCR_RING_BUF][INCR_TS_BUF];
    static _Thread_local int idx = 0;
    idx = (idx + SKIP_ONE) & INCR_RING_MASK;
    snprintf(buf[idx], sizeof(buf[idx]), "%d", v);
    return buf[idx];
}

/* ── Platform-portable mtime_ns ──────────────────────────────────── */

static int64_t stat_mtime_ns(const struct stat *st) {
#ifdef __APPLE__
    return ((int64_t)st->st_mtimespec.tv_sec * CBM_NS_PER_SEC) + (int64_t)st->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (int64_t)st->st_mtime * CBM_NS_PER_SEC;
#else
    return ((int64_t)st->st_mtim.tv_sec * CBM_NS_PER_SEC) + (int64_t)st->st_mtim.tv_nsec;
#endif
}

/* ── File classification ─────────────────────────────────────────── */

/* Classify discovered files against stored hashes using mtime+size.
 * Returns a boolean array: changed[i] = true if files[i] needs re-parsing.
 * Caller must free the returned array. */
static bool *classify_files(cbm_file_info_t *files, int file_count, cbm_file_hash_t *stored,
                            int stored_count, int *out_changed, int *out_unchanged) {
    bool *changed = calloc((size_t)file_count, sizeof(bool));
    if (!changed) {
        return NULL;
    }

    int n_changed = 0;
    int n_unchanged = 0;

    /* Build lookup: rel_path -> stored hash */
    CBMHashTable *ht =
        cbm_ht_create(stored_count > 0 ? (size_t)stored_count * PAIR_LEN : CBM_SZ_64);
    for (int i = 0; i < stored_count; i++) {
        cbm_ht_set(ht, stored[i].rel_path, &stored[i]);
    }

    for (int i = 0; i < file_count; i++) {
        cbm_file_hash_t *h = cbm_ht_get(ht, files[i].rel_path);
        if (!h) {
            /* New file */
            changed[i] = true;
            n_changed++;
            continue;
        }

        struct stat st;
        if (stat(files[i].path, &st) != 0) {
            changed[i] = true;
            n_changed++;
            continue;
        }

        if (stat_mtime_ns(&st) != h->mtime_ns || st.st_size != h->size) {
            changed[i] = true;
            n_changed++;
        } else {
            n_unchanged++;
        }
    }

    cbm_ht_free(ht);
    *out_changed = n_changed;
    *out_unchanged = n_unchanged;
    return changed;
}

/* Classify stored files that are absent from current discovery. Returns the
 * count of truly-deleted files (output via out_deleted) and ALSO collects
 * mode-skipped files into out_mode_skipped (caller frees both).
 *
 * A stored file is classified as:
 *   - "deleted"      — `stat()` returns ENOENT or ENOTDIR. Its nodes will
 *                       be purged and its hash row dropped.
 *   - "mode-skipped" — `stat()` succeeds. The file exists on disk but the
 *                       current discovery pass didn't visit it (e.g. excluded
 *                       by FAST_SKIP_DIRS in fast/moderate mode). Its nodes
 *                       must be preserved AND its hash row must be carried
 *                       forward into the new DB so subsequent reindexes can
 *                       still see it as "known" rather than treating it as
 *                       new-or-deleted.
 *
 * Without this distinction, a fast-mode reindex after a full-mode index
 * would silently purge every file under `tools/`, `scripts/`, `bin/`,
 * `build/`, `docs/`, `__tests__/`, etc. — see task
 * claude-connectors/codebase-memory-index-repository-is-destructive-...
 * and the 2026-04-13 Skyline incident (packages/mcp/src/tools/ vanished
 * from a live graph mid-session).
 *
 * Mode-skipped hash preservation is the second half of the additive-merge
 * contract: dump_and_persist re-upserts these hash rows so the next reindex
 * can correctly detect a real on-disk deletion of a mode-skipped file (as
 * opposed to seeing it as "never existed" → noop → orphaned graph nodes).
 *
 * Fail-safe rules (preserve nodes on uncertainty):
 *   - repo_path NULL → log error and preserve everything (return 0
 *     deletions, empty mode_skipped). The caller contract is that
 *     repo_path is required; a NULL means a misconfigured pipeline,
 *     not a deletion signal.
 *   - snprintf truncation (combined path ≥ CBM_SZ_4K) → preserve. We can't
 *     reliably stat a truncated path. Treat as mode-skipped.
 *   - stat() errno != ENOENT/ENOTDIR (EACCES, EIO, ELOOP, transient NFS,
 *     etc.) → preserve. The file may exist; we just can't see it right now.
 *     Treat as mode-skipped.
 *
 * Note: we use stat() (not lstat()) on purpose. A symlink whose target was
 * deleted should be classified as deleted from the indexer's perspective
 * because the indexer follows symlinks during discovery — a stale symlink
 * has no source to parse. */
static int find_deleted_files(const char *repo_path, cbm_file_info_t *files, int file_count,
                              cbm_file_hash_t *stored, int stored_count, char ***out_deleted,
                              cbm_file_hash_t **out_mode_skipped, int *out_mode_skipped_count) {
    *out_deleted = NULL;
    *out_mode_skipped = NULL;
    *out_mode_skipped_count = 0;

    if (!repo_path) {
        /* Misconfigured pipeline. Preserve everything rather than risk
         * silently re-introducing the destructive overwrite this function
         * was rewritten to prevent. */
        cbm_log_error("incremental.err", "msg", "find_deleted_files_null_repo_path");
        return 0;
    }

    CBMHashTable *current = cbm_ht_create((size_t)file_count * PAIR_LEN);
    for (int i = 0; i < file_count; i++) {
        cbm_ht_set(current, files[i].rel_path, &files[i]);
    }

    int del_count = 0;
    int del_cap = CBM_SZ_64;
    char **deleted = malloc((size_t)del_cap * sizeof(char *));
    if (!deleted) {
        cbm_log_error("incremental.err", "msg", "find_deleted_files_oom");
        cbm_ht_free(current);
        return 0;
    }

    int ms_count = 0;
    int ms_cap = CBM_SZ_64;
    cbm_file_hash_t *mode_skipped = malloc((size_t)ms_cap * sizeof(cbm_file_hash_t));
    if (!mode_skipped) {
        cbm_log_error("incremental.err", "msg", "find_deleted_files_oom_ms");
        free(deleted);
        cbm_ht_free(current);
        return 0;
    }

    for (int i = 0; i < stored_count; i++) {
        if (cbm_ht_get(current, stored[i].rel_path)) {
            continue; /* still visited by current pass */
        }
        /* Not in current discovery — check if it's truly deleted or just
         * mode-skipped (excluded by FAST_SKIP_DIRS etc.). */
        bool preserve = false;
        char abs_path[CBM_SZ_4K];
        int n = snprintf(abs_path, sizeof(abs_path), "%s/%s", repo_path, stored[i].rel_path);
        if (n < 0 || n >= (int)sizeof(abs_path)) {
            /* Truncation or encoding error — can't reliably stat. Preserve. */
            cbm_log_warn("incremental.path_truncated", "rel_path", stored[i].rel_path);
            preserve = true;
        } else {
            struct stat st;
            if (stat(abs_path, &st) == 0) {
                /* File exists on disk — mode-skipped, not deleted. */
                preserve = true;
            } else if (errno != ENOENT && errno != ENOTDIR) {
                /* Transient or permission error — fail safe by preserving.
                 * EACCES, EIO, ELOOP, ENAMETOOLONG, etc. */
                cbm_log_warn("incremental.stat_uncertain", "rel_path", stored[i].rel_path, "errno",
                             itoa_buf(errno));
                preserve = true;
            }
        }

        if (preserve) {
            /* Carry forward the existing hash row so subsequent reindexes
             * can correctly classify this file. */
            if (ms_count >= ms_cap) {
                ms_cap *= PAIR_LEN;
                cbm_file_hash_t *tmp = realloc(mode_skipped, (size_t)ms_cap * sizeof(*tmp));
                if (!tmp) {
                    cbm_log_error("incremental.err", "msg", "find_deleted_files_realloc_oom_ms");
                    break;
                }
                mode_skipped = tmp;
            }
            char *rp = strdup(stored[i].rel_path);
            char *sh = stored[i].sha256 ? strdup(stored[i].sha256) : NULL;
            if (!rp || (stored[i].sha256 && !sh)) {
                /* OOM mid-record. Drop this entry rather than persist a
                 * row with a NULL rel_path that would silently fail the
                 * NOT NULL constraint in upsert and reintroduce the
                 * orphaned-node bug. */
                cbm_log_error("incremental.err", "msg", "find_deleted_files_strdup_oom", "rel_path",
                              stored[i].rel_path);
                free(rp);
                free(sh);
                break;
            }
            mode_skipped[ms_count].project = NULL; /* unused by upsert API */
            mode_skipped[ms_count].rel_path = rp;
            mode_skipped[ms_count].sha256 = sh;
            mode_skipped[ms_count].mtime_ns = stored[i].mtime_ns;
            mode_skipped[ms_count].size = stored[i].size;
            ms_count++;
            continue;
        }

        /* File is truly gone — record for purge. */
        if (del_count >= del_cap) {
            del_cap *= PAIR_LEN;
            char **tmp = realloc(deleted, (size_t)del_cap * sizeof(char *));
            if (!tmp) {
                cbm_log_error("incremental.err", "msg", "find_deleted_files_realloc_oom");
                break;
            }
            deleted = tmp;
        }
        deleted[del_count++] = strdup(stored[i].rel_path);
    }

    cbm_ht_free(current);
    *out_deleted = deleted;
    *out_mode_skipped = mode_skipped;
    *out_mode_skipped_count = ms_count;
    return del_count;
}

/* Free a mode_skipped array allocated by find_deleted_files. */
static void free_mode_skipped(cbm_file_hash_t *ms, int count) {
    if (!ms) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free((void *)ms[i].rel_path);
        free((void *)ms[i].sha256);
    }
    free(ms);
}

/* Add the structural nodes for a newly discovered path before definition
 * extraction. This mirrors the relevant part of the full structure pass: a
 * Folder must exist before a directory-based Module definition arrives, or
 * the Module wins the shared qualified-name slot and the graph diverges. */
static void incr_add_changed_file_structure(cbm_gbuf_t *gbuf, const char *project,
                                            const cbm_file_info_t *file) {
    if (!gbuf || !project || !file || !file->rel_path) {
        return;
    }
    const char *rel_path = file->rel_path;
    const char *slash = strrchr(rel_path, '/');
    const char *basename = slash ? slash + 1 : rel_path;
    const char *extension = strrchr(basename, '.');
    char properties[CBM_SZ_256];
    snprintf(properties, sizeof(properties), "{\"extension\":\"%s\"}",
             extension ? extension : "");
    char *file_qn = cbm_pipeline_fqn_compute(project, rel_path, "__file__");
    if (!file_qn) {
        return;
    }
    cbm_gbuf_upsert_node(gbuf, "File", basename, file_qn, rel_path, 0, 0, properties);

    const cbm_gbuf_node_t **branches = NULL;
    int branch_count = 0;
    const char *branch_qn = project;
    if (cbm_gbuf_find_by_label(gbuf, "Branch", &branches, &branch_count) == 0 &&
        branch_count > 0 && branches[0]->qualified_name) {
        branch_qn = branches[0]->qualified_name;
    }

    char *dir = strdup(rel_path);
    if (!dir) {
        free(file_qn);
        return;
    }
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        dir[0] = '\0';
    }
    char *file_parent_qn = dir[0] ? cbm_pipeline_fqn_folder(project, dir) : NULL;

    for (char *walk = dir; walk[0];) {
        char *folder_qn = cbm_pipeline_fqn_folder(project, walk);
        if (!folder_qn) {
            break;
        }
        const char *folder_name = strrchr(walk, '/');
        folder_name = folder_name ? folder_name + 1 : walk;
        cbm_gbuf_upsert_node(gbuf, "Folder", folder_name, folder_qn, walk, 0, 0, "{}");

        char *parent_dir = strdup(walk);
        if (!parent_dir) {
            free(folder_qn);
            break;
        }
        char *parent_slash = strrchr(parent_dir, '/');
        if (parent_slash) {
            *parent_slash = '\0';
        } else {
            parent_dir[0] = '\0';
        }
        char *parent_qn_heap = parent_dir[0] ? cbm_pipeline_fqn_folder(project, parent_dir) : NULL;
        const char *parent_qn = parent_qn_heap ? parent_qn_heap : branch_qn;
        const cbm_gbuf_node_t *folder = cbm_gbuf_find_by_qn(gbuf, folder_qn);
        const cbm_gbuf_node_t *parent = cbm_gbuf_find_by_qn(gbuf, parent_qn);
        if (folder && parent) {
            cbm_gbuf_insert_edge(gbuf, parent->id, folder->id, "CONTAINS_FOLDER", "{}");
        }
        free(parent_qn_heap);
        free(parent_dir);
        free(folder_qn);

        char *up = strrchr(walk, '/');
        if (up) {
            *up = '\0';
        } else {
            walk[0] = '\0';
        }
    }

    const cbm_gbuf_node_t *file_node = cbm_gbuf_find_by_qn(gbuf, file_qn);
    const char *parent_qn = file_parent_qn ? file_parent_qn : branch_qn;
    const cbm_gbuf_node_t *parent = cbm_gbuf_find_by_qn(gbuf, parent_qn);
    if (file_node && parent) {
        cbm_gbuf_insert_edge(gbuf, parent->id, file_node->id, "CONTAINS_FILE", "{}");
    }
    free(file_parent_qn);
    free(dir);
    free(file_qn);
}

/* ── Inbound cross-file edge preservation (incremental correctness) ──
 *
 * The purge step (cbm_gbuf_delete_by_file) removes a changed file's nodes,
 * and the cascade then drops every edge referencing them — INCLUDING inbound
 * edges whose source lives in an UNCHANGED file (e.g. StudyService.grade ->
 * SM2.review, or a Folder -> File containment edge). Because incremental only
 * re-parses the changed files, the resolution passes never regenerate those
 * inbound edges, so the graph silently loses cross-file CALLS / USAGE /
 * CONTAINS_FILE / INHERITS / ... edges on every edit and diverges from a
 * clean full reindex (which resolves every file).
 *
 * Fix: snapshot inbound cross-file edges into changed or deleted files BEFORE
 * the purge. Re-run CALLS resolution for unchanged callers after re-extraction,
 * then re-link non-CALLS edges by exact qualified-name target. Notes:
 *   - Only edges whose target is in a changed file and whose source is NOT
 *     are snapshotted; edges out of a changed file are regenerated when that
 *     file is re-resolved.
 *   - Edge types recomputed wholesale by post-passes (SIMILAR_TO,
 *     SEMANTICALLY_RELATED) are skipped — re-linking a stale snapshot could
 *     add edges a full reindex would not produce.
 *   - cbm_gbuf_insert_edge dedups, so re-linking an edge the resolver already
 *     recreated is a harmless no-op.
 *   - A deleted target is dropped. A renamed CALLS edge is rebuilt from its
 *     unchanged caller source, preserving the resolver's current confidence
 *     and strategy rather than copying stale edge properties. */

typedef struct {
    char *source_qn;
    char *target_qn;
    char *target_name;
    char *target_label;
    char *type;
    char *props;
} cbm_saved_edge_t;

typedef struct {
    cbm_gbuf_t *gbuf;
    CBMHashTable *changed_paths; /* rel_path -> non-NULL sentinel (membership set) */
    cbm_saved_edge_t *items;
    int count;
    int cap;
} cbm_edge_capture_t;

/* Edge types that must NOT be re-linked from the pre-purge snapshot, because a
 * full reindex (re)computes them via a pass whose result can differ from the
 * snapshot — restoring a stale copy could leave wrong properties or even an
 * edge a full reindex would not produce:
 *   - SIMILAR_TO / SEMANTICALLY_RELATED / FILE_CHANGES_WITH / DATA_FLOWS:
 *     rebuilt wholesale by the incremental post-passes (similarity /
 *     semantic_edges / githistory / route matching) over a drifting corpus.
 * Structural containment edges are regenerated from changed paths before
 * extraction, so snapshots omit them rather than retaining a pre-rename
 * parent relationship. */
static bool incr_edge_type_is_recomputed(const char *type) {
    return type && (strcmp(type, "SIMILAR_TO") == 0 || strcmp(type, "SEMANTICALLY_RELATED") == 0 ||
                    strcmp(type, "FILE_CHANGES_WITH") == 0 || strcmp(type, "DATA_FLOWS") == 0 ||
                    strcmp(type, "CONTAINS_FILE") == 0 || strcmp(type, "CONTAINS_FOLDER") == 0);
}

/* cbm_gbuf_foreach_edge visitor: snapshot inbound cross-file edges into
 * changed files so they survive the purge and can be re-linked afterward. */
static void incr_capture_inbound_edge(const cbm_gbuf_edge_t *edge, void *userdata) {
    cbm_edge_capture_t *cap = (cbm_edge_capture_t *)userdata;
    if (incr_edge_type_is_recomputed(edge->type)) {
        return;
    }
    const cbm_gbuf_node_t *src = cbm_gbuf_find_by_id(cap->gbuf, edge->source_id);
    const cbm_gbuf_node_t *tgt = cbm_gbuf_find_by_id(cap->gbuf, edge->target_id);
    if (!src || !tgt || !src->qualified_name || !tgt->qualified_name || !src->file_path ||
        !tgt->file_path) {
        return;
    }
    /* Keep only edges that the purge would orphan permanently: target is in a
     * changed file (its node is deleted + re-created), source is NOT (its file
     * is never re-parsed, so the resolver won't regenerate the edge). */
    if (!cbm_ht_get(cap->changed_paths, tgt->file_path) ||
        cbm_ht_get(cap->changed_paths, src->file_path)) {
        return;
    }
    if (cap->count >= cap->cap) {
        int ncap = (cap->cap > 0) ? cap->cap * PAIR_LEN : CBM_SZ_64;
        cbm_saved_edge_t *tmp = realloc(cap->items, (size_t)ncap * sizeof(*tmp));
        if (!tmp) {
            cbm_log_warn("incremental.edge_snapshot_oom", "captured", itoa_buf(cap->count));
            return; /* best-effort: stop capturing, keep what we have */
        }
        cap->items = tmp;
        cap->cap = ncap;
    }
    cbm_saved_edge_t *s = &cap->items[cap->count];
    s->source_qn = strdup(src->qualified_name);
    s->target_qn = strdup(tgt->qualified_name);
    s->target_name = strdup(tgt->name ? tgt->name : "");
    s->target_label = strdup(tgt->label ? tgt->label : "");
    s->type = strdup(edge->type);
    s->props = strdup(edge->properties_json ? edge->properties_json : "{}");
    if (!s->source_qn || !s->target_qn || !s->target_name || !s->target_label || !s->type ||
        !s->props) {
        free(s->source_qn);
        free(s->target_qn);
        free(s->target_name);
        free(s->target_label);
        free(s->type);
        free(s->props);
        return;
    }
    cap->count++;
}

static const cbm_gbuf_node_t *incr_find_unique_renamed_target(const cbm_gbuf_t *gbuf,
                                                               const cbm_saved_edge_t *saved) {
    if (!saved->target_name || !saved->target_name[0] || !saved->target_label ||
        !saved->target_label[0]) {
        return NULL;
    }
    const cbm_gbuf_node_t **nodes = NULL;
    int count = 0;
    if (cbm_gbuf_find_by_name(gbuf, saved->target_name, &nodes, &count) != 0) {
        return NULL;
    }
    const cbm_gbuf_node_t *candidate = NULL;
    for (int i = 0; i < count; i++) {
        const cbm_gbuf_node_t *node = nodes[i];
        if (!node->qualified_name || !node->label || strcmp(node->label, saved->target_label) != 0) {
            continue;
        }
        if (candidate) {
            return NULL; /* ambiguous: do not guess a rename target */
        }
        candidate = node;
    }
    return candidate;
}

static int incr_cmp_id(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

typedef struct {
    const int64_t *ids;
    int count;
} incr_id_set_t;

static bool incr_keep_unless_source_in_set(const cbm_gbuf_t *gb, const cbm_gbuf_edge_t *edge,
                                           void *userdata) {
    (void)gb;
    const incr_id_set_t *set = userdata;
    return !bsearch(&edge->source_id, set->ids, (size_t)set->count, sizeof(*set->ids),
                    incr_cmp_id);
}

/* A path rename changes the callee's qualified name. Re-run the CALLS pass
 * for only unchanged source files that previously called a purged target, so
 * the normal resolver recalculates strategy/confidence against the new
 * registry instead of preserving stale edge properties. */
static void incr_rebuild_snapshot_callers(cbm_pipeline_ctx_t *ctx, const cbm_edge_capture_t *cap,
                                          const cbm_file_info_t *files, int file_count) {
    if (!ctx || !cap || !files || file_count <= 0) {
        return;
    }
    cbm_file_info_t *callers = calloc((size_t)cap->count, sizeof(*callers));
    int64_t *source_ids = malloc((size_t)cap->count * sizeof(*source_ids));
    if (!callers || !source_ids) {
        free(callers);
        free(source_ids);
        return;
    }
    int caller_count = 0;
    int source_count = 0;
    for (int i = 0; i < cap->count; i++) {
        const cbm_saved_edge_t *saved = &cap->items[i];
        /* Re-resolve unchanged sources that previously emitted either CALLS
         * or resolver-owned CONFIGURES. CONFIGURES may be the only derived
         * edge from a source (for example an environment accessor), so
         * restricting this list to CALLS leaves those edges missing after a
         * changed-file purge. */
        if (!saved->type || (strcmp(saved->type, "CALLS") != 0 &&
                             strcmp(saved->type, "CONFIGURES") != 0)) {
            continue;
        }
        const cbm_gbuf_node_t *source = cbm_gbuf_find_by_qn(ctx->gbuf, saved->source_qn);
        if (!source || !source->file_path) {
            continue;
        }
        source_ids[source_count++] = source->id;
        for (int f = 0; f < file_count; f++) {
            if (strcmp(files[f].rel_path, source->file_path) != 0) {
                continue;
            }
            bool already_added = false;
            for (int existing = 0; existing < caller_count; existing++) {
                if (strcmp(callers[existing].rel_path, files[f].rel_path) == 0) {
                    already_added = true;
                    break;
                }
            }
            if (!already_added) {
                callers[caller_count++] = files[f];
            }
            break;
        }
    }
    /* Drop these sources' CONFIGURES edges in one pass over the edge list:
     * deleting per captured edge rescanned every edge and rebuilt the edge
     * indexes each time, which made a one-file reindex slower than a full one. */
    if (source_count > 0) {
        qsort(source_ids, (size_t)source_count, sizeof(*source_ids), incr_cmp_id);
        incr_id_set_t set = {source_ids, source_count};
        cbm_gbuf_delete_edges_by_type_unless(ctx->gbuf, "CONFIGURES",
                                             incr_keep_unless_source_in_set, &set);
    }
    if (caller_count > 0) {
        cbm_pipeline_pass_calls(ctx, callers, caller_count);
    }
    free(source_ids);
    free(callers);
}

/* Re-link snapshotted inbound edges to the freshly re-created target nodes.
 * Returns the number of edges re-linked. */
static int incr_restore_inbound_edges(cbm_gbuf_t *gbuf, cbm_edge_capture_t *cap) {
    int restored = 0;
    for (int i = 0; i < cap->count; i++) {
        cbm_saved_edge_t *s = &cap->items[i];
        const cbm_gbuf_node_t *src = cbm_gbuf_find_by_qn(gbuf, s->source_qn);
        const cbm_gbuf_node_t *tgt = cbm_gbuf_find_by_qn(gbuf, s->target_qn);
        if (!tgt && s->type && strcmp(s->type, "CALLS") == 0) {
            continue; /* rebuilt from source so resolver owns its properties */
        }
        /* Incremental runs skip cross-file LSP, so an unchanged source cannot
         * re-prove its CALL_REFERENCE. Keep the snapshot edge only while the
         * same QN is still a callable; never guess a rename. */
        if (s->type && strcmp(s->type, "CALL_REFERENCE") == 0 &&
            !cbm_pipeline_node_is_callable_target(tgt)) {
            continue;
        }
        if (!tgt) {
            tgt = incr_find_unique_renamed_target(gbuf, s);
        }
        if (src && tgt) {
            cbm_gbuf_insert_edge(gbuf, src->id, tgt->id, s->type, s->props);
            restored++;
        }
    }
    return restored;
}

static void incr_free_edge_capture(cbm_edge_capture_t *cap) {
    for (int i = 0; i < cap->count; i++) {
        free(cap->items[i].source_qn);
        free(cap->items[i].target_qn);
        free(cap->items[i].target_name);
        free(cap->items[i].target_label);
        free(cap->items[i].type);
        free(cap->items[i].props);
    }
    free(cap->items);
    cap->items = NULL;
    cap->count = 0;
    cap->cap = 0;
}

/* ── Registry seed visitor ────────────────────────────────────────── */

/* Labels the full-index definition pass seeds into the registry
 * (pass_definitions.c — KEEP IN SYNC). Incremental re-resolution must see the
 * SAME symbol set, or it diverges from a clean full reindex: seeding extra
 * container nodes (File / Module / Folder / ...) lets a type usage like `Word`
 * resolve to the same-named Module node instead of the Class node. Only
 * callable / declared symbols belong in the registry. */
static bool incr_label_is_registry_symbol(const char *label) {
    /* Mirror pass_definitions.c / pass_parallel.c registry seeding EXACTLY:
     * callables + every type-like container (Class/Struct/Interface/Enum/Type/
     * Trait) + Variable/Field. Struct included so an incremental re-resolve seeds
     * the same struct type nodes a full reindex would. */
    return label && (strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0 ||
                     cbm_label_is_type_like(label) || strcmp(label, "Variable") == 0 ||
                     strcmp(label, "Field") == 0);
}

/* Callback for cbm_gbuf_foreach_node: seed the registry with the existing
 * project's definition symbols so the resolver can match cross-file symbols
 * during incremental. Mirrors the full-index registry contents exactly so an
 * incremental re-resolve picks the same nodes a full reindex would. */
static void registry_visitor(const cbm_gbuf_node_t *node, void *userdata) {
    cbm_registry_t *r = (cbm_registry_t *)userdata;
    if (!incr_label_is_registry_symbol(node->label)) {
        return;
    }
    cbm_registry_add(r, node->name, node->qualified_name, node->label);
}

/* Run parallel or sequential extract+resolve for changed files. */
static int run_extract_resolve(cbm_pipeline_ctx_t *ctx, cbm_file_info_t *changed_files, int ci) {
    struct timespec t;

    /* Per-file LSP always runs (every mode). Cross-file LSP stays disabled in
     * incremental regardless (cbm_parallel_resolve is called with NULL
     * cross_registries below). */

#define MIN_FILES_FOR_PARALLEL_INCR 50
    int worker_count = cbm_default_worker_count(true);
    bool use_parallel = (worker_count > SKIP_ONE && ci > MIN_FILES_FOR_PARALLEL_INCR);

    if (use_parallel) {
        cbm_log_info("incremental.mode", "mode", "parallel", "workers", itoa_buf(worker_count),
                     "changed", itoa_buf(ci));

        _Atomic int64_t shared_ids;
        atomic_init(&shared_ids, cbm_gbuf_next_id(ctx->gbuf));

        CBMFileResult **cache = (CBMFileResult **)calloc(ci, sizeof(CBMFileResult *));
        if (cache) {
            cbm_clock_gettime(CLOCK_MONOTONIC, &t);
            int rc = cbm_parallel_extract(ctx, changed_files, ci, cache, &shared_ids, worker_count);
            if (rc == CBM_PIPELINE_ABORT_OVER_BUDGET) {
                /* Over the memory budget: stop before publication so the
                 * served generation stays as it was. */
                for (int j = 0; j < ci; j++) {
                    if (cache[j]) {
                        cbm_free_result(cache[j]);
                    }
                }
                free(cache);
                return rc;
            }
            cbm_gbuf_set_next_id(ctx->gbuf, atomic_load(&shared_ids));
            cbm_log_info("pass.timing", "pass", "incr_extract", "elapsed_ms",
                         itoa_buf((int)elapsed_ms(t)));

            cbm_clock_gettime(CLOCK_MONOTONIC, &t);
            cbm_build_registry_from_cache(ctx, changed_files, ci, cache);
            cbm_log_info("pass.timing", "pass", "incr_registry", "elapsed_ms",
                         itoa_buf((int)elapsed_ms(t)));

            /* Incremental skips cross-file LSP precondition build — it
             * would need all_defs from the full project, not just the
             * changed slice. Per-file LSP (run inside cbm_extract_file)
             * still fires; cross-file resolution is deferred to the
             * next full re-index. Pass NULL/0/NULL to make the fused
             * step in resolve_worker a no-op. */
            cbm_clock_gettime(CLOCK_MONOTONIC, &t);
            cbm_parallel_resolve(ctx, changed_files, ci, cache, &shared_ids, worker_count, NULL, 0,
                                 NULL, NULL /* module_def_index */,
                                 NULL /* cross_registries — incremental skips Tier 2 prebuild */);
            cbm_gbuf_set_next_id(ctx->gbuf, atomic_load(&shared_ids));
            cbm_log_info("pass.timing", "pass", "incr_resolve", "elapsed_ms",
                         itoa_buf((int)elapsed_ms(t)));

            cbm_pipeline_extract_infra_routes(ctx->gbuf, changed_files, cache, NULL, ci);
            cbm_pipeline_process_infra_bindings(ctx->gbuf, changed_files, cache, NULL, ci);
            for (int j = 0; j < ci; j++) {
                if (cache[j]) {
                    cbm_free_result(cache[j]);
                }
            }
            free(cache);
        }
    } else {
        cbm_log_info("incremental.mode", "mode", "sequential", "changed", itoa_buf(ci));
        /* Keep extraction results, as the full sequential path does, so the
         * infra Route passes can read them. */
        CBMFileResult **cache = ci > 0 ? (CBMFileResult **)calloc(ci, sizeof(CBMFileResult *))
                                       : NULL;
        ctx->result_cache = cache;
        cbm_pipeline_pass_definitions(ctx, changed_files, ci);
        cbm_pipeline_pass_calls(ctx, changed_files, ci);
        cbm_pipeline_pass_usages(ctx, changed_files, ci);
        cbm_pipeline_pass_semantic(ctx, changed_files, ci);
        ctx->result_cache = NULL;
        if (cache) {
            cbm_pipeline_extract_infra_routes(ctx->gbuf, changed_files, cache, NULL, ci);
            cbm_pipeline_process_infra_bindings(ctx->gbuf, changed_files, cache, NULL, ci);
            for (int j = 0; j < ci; j++) {
                if (cache[j]) {
                    cbm_free_result(cache[j]);
                }
            }
            free(cache);
        }
    }
    return 0;
}

/* Infra Route nodes are keyed by URL across config files: a URL becomes a
 * Route only when no config file lists it under a denied key, and the node
 * belongs to one of the files that list it. A change to one config file can
 * therefore change what another contributes, so when any config file changed
 * or was deleted, every config file is re-extracted. Returns files added. */
static int incr_expand_infra_changes(const cbm_file_info_t *files, int file_count,
                                     bool *is_changed, char *const *deleted, int deleted_count) {
    bool infra_touched = false;
    for (int i = 0; i < file_count && !infra_touched; i++) {
        infra_touched = is_changed[i] && cbm_pipeline_is_infra_file(files[i].rel_path);
    }
    for (int i = 0; i < deleted_count && !infra_touched; i++) {
        infra_touched = cbm_pipeline_is_infra_file(deleted[i]);
    }
    if (!infra_touched) {
        return 0;
    }
    int added = 0;
    for (int i = 0; i < file_count; i++) {
        if (!is_changed[i] && cbm_pipeline_is_infra_file(files[i].rel_path)) {
            is_changed[i] = true;
            added++;
        }
    }
    return added;
}

/* HANDLES edges into infra Routes come only from route matching. */
static bool incr_handles_edge_is_not_infra_match(const cbm_gbuf_t *gbuf,
                                                 const cbm_gbuf_edge_t *edge, void *userdata) {
    (void)userdata;
    static const char infra_prefix[] = "__route__infra__";
    const cbm_gbuf_node_t *target = cbm_gbuf_find_by_id(gbuf, edge->target_id);
    return !target || !target->qualified_name ||
           strncmp(target->qualified_name, infra_prefix, sizeof(infra_prefix) - 1) != 0;
}

/* Run post-extraction passes (tests, decorator tags, configlink). */
static void run_postpasses(cbm_pipeline_ctx_t *ctx, cbm_file_info_t *changed_files, int ci,
                           const char *project,
                           const cbm_incremental_derived_state_t *before) {
    struct timespec t;

    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_pipeline_pass_tests(ctx, changed_files, ci);
    cbm_log_info("pass.timing", "pass", "incr_tests", "elapsed_ms", itoa_buf((int)elapsed_ms(t)));

    /* File nodes for changed paths are deleted and recreated above, so their
     * git-history properties disappear unless the full-repository derived
     * pass is reapplied. Rebuild the derived edges as well to match a fresh
     * full index instead of accumulating stale FILE_CHANGES_WITH rows. */
    cbm_gbuf_delete_edges_by_type(ctx->gbuf, "FILE_CHANGES_WITH");
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    int history_rc = cbm_pipeline_pass_githistory(ctx);
    cbm_log_info("pass.timing", "pass", "incr_githistory", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)), "rc", itoa_buf(history_rc));

    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_pipeline_pass_decorator_tags(ctx->gbuf, project);
    cbm_log_info("pass.timing", "pass", "incr_decorator_tags", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    /* Route matching is global: an infra Route in an unchanged config file can
     * match a handler in a changed file, and DATA_FLOWS follow those matches.
     * Remove the previous matches and data flows, then rerun the pass a full
     * index runs, before the derived digest reads the graph. */
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_gbuf_delete_edges_by_type_unless(ctx->gbuf, "HANDLES",
                                         incr_handles_edge_is_not_infra_match, NULL);
    cbm_gbuf_delete_edges_by_type(ctx->gbuf, "DATA_FLOWS");
    cbm_pipeline_create_route_nodes(ctx->gbuf);
    cbm_log_info("pass.timing", "pass", "incr_route_match", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    /* Restore the final derived node properties before comparing pass inputs.
     * The semantic and similarity passes ignore these complexity fields, but
     * reproducing them makes the conservative whole-graph input digest exact. */
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_pipeline_pass_complexity(ctx);
    cbm_log_info("pass.timing", "pass", "incr_complexity", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    /* Importance reads CALLS/USAGE (re-extraction plus relinked inbound
     * edges) and TESTS (pass_tests, first in this function). It must run
     * before the digest below, which hashes properties_json. Nodes from
     * unchanged files already carry the key; the write-back overwrites it. */
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_pipeline_pass_importance(ctx);
    cbm_log_info("pass.timing", "pass", "incr_importance", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_incremental_derived_state_t after = derived_state(ctx->gbuf);
    bool input_unchanged = before && derived_digest_equal(&before->input, &after.input);
    cbm_log_info("incremental.derived_digest", "unchanged",
                 input_unchanged ? "true" : "false", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    bool config_unchanged = input_unchanged &&
        derived_digest_equal(&before->config, &after.config);
    if (!config_unchanged) {
        cbm_clock_gettime(CLOCK_MONOTONIC, &t);
        cbm_pipeline_pass_configlink(ctx);
        cbm_log_info("pass.timing", "pass", "incr_configlink", "elapsed_ms",
                     itoa_buf((int)elapsed_ms(t)));
    } else {
        cbm_log_info("incremental.derived_gate", "component", "configlink", "decision", "reuse");
    }

    /* SIMILAR_TO + SEMANTICALLY_RELATED edges only in moderate/full modes */
    if (ctx->mode <= CBM_MODE_MODERATE) {
        /* These are global derived relations. The old graph was loaded before
         * this incremental update, so rerunning either pass without first
         * removing its prior output leaves relationships that a clean full
         * index would no longer select after the corpus changed. */
        bool similarity_unchanged = input_unchanged &&
            derived_digest_equal(&before->similarity, &after.similarity);
        if (!similarity_unchanged) {
            cbm_gbuf_delete_edges_by_type(ctx->gbuf, "SIMILAR_TO");
            cbm_clock_gettime(CLOCK_MONOTONIC, &t);
            cbm_pipeline_pass_similarity(ctx);
            cbm_log_info("pass.timing", "pass", "incr_similarity", "elapsed_ms",
                         itoa_buf((int)elapsed_ms(t)));
        } else {
            cbm_log_info("incremental.derived_gate", "component", "similarity", "decision", "reuse");
        }

        bool semantic_unchanged = input_unchanged &&
            derived_digest_equal(&before->semantic, &after.semantic) &&
            cbm_gbuf_reuse_zova_semantic_vectors(ctx->gbuf) == 0;
        if (!semantic_unchanged) {
            cbm_gbuf_delete_edges_by_type(ctx->gbuf, "SEMANTICALLY_RELATED");
            cbm_gbuf_clear_semantic_vectors(ctx->gbuf);
            cbm_clock_gettime(CLOCK_MONOTONIC, &t);
            cbm_pipeline_pass_semantic_edges(ctx);
            cbm_log_info("pass.timing", "pass", "incr_semantic_edges", "elapsed_ms",
                         itoa_buf((int)elapsed_ms(t)));
        } else {
            cbm_log_info("incremental.derived_gate", "component", "semantic", "decision", "reuse");
        }
    }
}
/* Publish the merged graph and hashes to cbm.zova as a delta generation.
 * Mode-skipped hash rows are preserved so subsequent reindexes can correctly
 * distinguish "never indexed" from "indexed but not visited this pass". */
static int dump_and_persist(cbm_pipeline_t *pipeline, cbm_gbuf_t *gbuf, cbm_file_info_t *files,
                            int file_count, const cbm_file_hash_t *mode_skipped,
                            int mode_skipped_count, cbm_zova_workspace_snapshot_t *before) {
    /* #49: same post-extraction gates as the full path. Aborting here leaves
     * the served generation untouched (publish is atomic). */
    cbm_pipeline_log_phase_mem("incr_pre_dump");
    int gate_rc = cbm_pipeline_budget_gate("incr_pre_dump");
    if (gate_rc != 0) {
        return gate_rc;
    }
    if (cbm_gbuf_prepare_zova_dump_delta(gbuf) != 0) {
        cbm_log_error("incremental.err", "phase", "zova_prepare");
        return CBM_NOT_FOUND;
    }
    cbm_pipeline_log_phase_mem("incr_zova_prepare");
    gate_rc = cbm_pipeline_budget_gate("incr_zova_publish");
    if (gate_rc != 0) {
        return gate_rc;
    }
    if (cbm_pipeline_publish_zova_user_database_delta(pipeline, gbuf, files, file_count,
                                                      mode_skipped, mode_skipped_count,
                                                      before) != 0) {
        cbm_log_error("incremental.err", "phase", "zova_single_file");
        return CBM_NOT_FOUND;
    }
    cbm_pipeline_log_phase_mem("incr_zova_publish");
    return 0;
}

/* ── Incremental pipeline entry point ────────────────────────────── */

static int copy_zova_snapshot_hashes(const char *project,
                                     const cbm_zova_workspace_snapshot_t *snapshot,
                                     cbm_file_hash_t **out_hashes, int *out_count) {
    *out_hashes = NULL;
    *out_count = 0;
    if (snapshot->file_hash_count <= 0) {
        return 0;
    }
    cbm_file_hash_t *hashes = calloc((size_t)snapshot->file_hash_count, sizeof(*hashes));
    if (!hashes) {
        return CBM_NOT_FOUND;
    }
    for (int i = 0; i < snapshot->file_hash_count; i++) {
        const cbm_zova_file_hash_input_t *source = &snapshot->file_hashes[i];
        hashes[i].project = strdup(project);
        hashes[i].rel_path = strdup(source->file_path);
        hashes[i].sha256 = strdup(source->content_hash);
        hashes[i].mtime_ns = source->mtime_ns;
        hashes[i].size = source->size_bytes;
        if (!hashes[i].project || !hashes[i].rel_path || !hashes[i].sha256) {
            cbm_store_free_file_hashes(hashes, snapshot->file_hash_count);
            return CBM_NOT_FOUND;
        }
    }
    *out_hashes = hashes;
    *out_count = snapshot->file_hash_count;
    return 0;
}

static int run_incremental(cbm_pipeline_t *p, cbm_file_info_t *files, int file_count,
                           cbm_zova_workspace_snapshot_t *snapshot, cbm_gbuf_t *existing) {
    struct timespec t0;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t0);

    const char *project = cbm_pipeline_project_name(p);

    cbm_file_hash_t *stored = NULL;
    int stored_count = 0;
    if (copy_zova_snapshot_hashes(project, snapshot, &stored, &stored_count) != 0) {
        cbm_log_error("incremental.err", "msg", "load_zova_hashes_failed");
        cbm_gbuf_free(existing);
        return CBM_NOT_FOUND;
    }

    /* Classify files */
    int n_changed = 0;
    int n_unchanged = 0;
    bool *is_changed =
        classify_files(files, file_count, stored, stored_count, &n_changed, &n_unchanged);

    /* Classify stored files absent from current discovery: truly-deleted
     * (purge) vs mode-skipped (preserve nodes AND hash rows). */
    char **deleted = NULL;
    cbm_file_hash_t *mode_skipped = NULL;
    int mode_skipped_count = 0;
    int deleted_count =
        find_deleted_files(cbm_pipeline_repo_path(p), files, file_count, stored, stored_count,
                           &deleted, &mode_skipped, &mode_skipped_count);

    if (is_changed) {
        int infra_added =
            incr_expand_infra_changes(files, file_count, is_changed, deleted, deleted_count);
        n_changed += infra_added;
        n_unchanged -= infra_added;
    }

    cbm_log_info("incremental.classify", "changed", itoa_buf(n_changed), "unchanged",
                 itoa_buf(n_unchanged), "deleted", itoa_buf(deleted_count), "mode_skipped",
                 itoa_buf(mode_skipped_count));

    /* Fast path: nothing changed → skip. The committed generation is left
     * untouched, which means existing hash rows (including for any
     * mode-skipped files that were already preserved by an earlier run)
     * remain intact. */
    if (n_changed == 0 && deleted_count == 0) {
        cbm_log_info("incremental.noop", "reason", "no_changes");
        free(is_changed);
        free(deleted);
        free_mode_skipped(mode_skipped, mode_skipped_count);
        cbm_store_free_file_hashes(stored, stored_count);
        cbm_gbuf_free(existing);
        return 0;
    }

    cbm_store_free_file_hashes(stored, stored_count);

    /* Build list of changed files */
    cbm_file_info_t *changed_files =
        (n_changed > 0) ? malloc((size_t)n_changed * sizeof(cbm_file_info_t)) : NULL;
    int ci = 0;
    for (int i = 0; i < file_count; i++) {
        if (is_changed[i]) {
            changed_files[ci++] = files[i];
        }
    }
    free(is_changed);

    cbm_log_info("incremental.reparse", "files", itoa_buf(ci));

    /* Step 1: the existing graph was exported from the committed generation */
    cbm_log_info("incremental.load_zova", "nodes", itoa_buf(cbm_gbuf_node_count(existing)),
                 "edges", itoa_buf(cbm_gbuf_edge_count(existing)));

    struct timespec t;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_incremental_derived_state_t derived_before = derived_state(existing);
    cbm_log_info("incremental.derived_digest", "phase", "baseline", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    /* Snapshot inbound cross-file edges into changed files BEFORE purging, so
     * the cascade delete doesn't permanently drop edges whose source lives in
     * an unchanged (never-re-parsed) file. Re-linked after re-resolution. */
    cbm_edge_capture_t edge_cap = {0};
    edge_cap.gbuf = existing;
    {
        CBMHashTable *changed_paths = cbm_ht_create(ci > 0 ? (size_t)ci * PAIR_LEN : CBM_SZ_64);
        for (int i = 0; i < ci; i++) {
            cbm_ht_set(changed_paths, changed_files[i].rel_path, &changed_files[i]);
        }
        for (int i = 0; i < deleted_count; i++) {
            cbm_ht_set(changed_paths, deleted[i], deleted[i]);
        }
        edge_cap.changed_paths = changed_paths;
        cbm_clock_gettime(CLOCK_MONOTONIC, &t);
        cbm_gbuf_foreach_edge(existing, incr_capture_inbound_edge, &edge_cap);
        edge_cap.changed_paths = NULL;
        cbm_ht_free(changed_paths); /* keys borrowed from changed_files; not freed here */
    }
    cbm_log_info("incremental.edge_snapshot", "captured", itoa_buf(edge_cap.count), "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    /* Step 2: Purge stale nodes */
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    for (int i = 0; i < ci; i++) {
        cbm_gbuf_delete_by_file(existing, changed_files[i].rel_path);
    }
    for (int i = 0; i < deleted_count; i++) {
        cbm_gbuf_delete_by_file(existing, deleted[i]);
    }

    int affected_count = ci + deleted_count;
    const char **affected_paths = NULL;
    if (affected_count > 0) {
        affected_paths = calloc((size_t)affected_count, sizeof(*affected_paths));
        if (affected_paths) {
            for (int i = 0; i < ci; i++) {
                affected_paths[i] = changed_files[i].rel_path;
            }
            for (int i = 0; i < deleted_count; i++) {
                affected_paths[ci + i] = deleted[i];
            }
        }
    }
    if (affected_paths) {
        cbm_gbuf_delete_empty_folders_under(existing, affected_paths, affected_count);
        free(affected_paths);
    } else if (affected_count > 0) {
        cbm_log_warn("incremental.folder_prune_oom", "affected_paths", itoa_buf(affected_count));
    }
    for (int i = 0; i < deleted_count; i++) {
        free(deleted[i]);
    }
    free(deleted);
    cbm_log_info("incremental.purge", "elapsed_ms", itoa_buf((int)elapsed_ms(t)));

    /* Step 3-5: Registry + extract + resolve */
    cbm_registry_t *registry = cbm_registry_new();
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_gbuf_foreach_node(existing, registry_visitor, registry);
    cbm_log_info("incremental.registry_seed", "symbols", itoa_buf(cbm_registry_size(registry)),
                 "elapsed_ms", itoa_buf((int)elapsed_ms(t)));

    /* Discovery exclusions (gitignore + skip dirs) captured by the run that
     * routed here. Borrowed from the pipeline so the auxiliary repo walks
     * (pkgmap via merge_pkg_entries, path aliases) skip excluded subtrees on
     * incremental runs too — same borrow as the full path (#792/#804). */
    char **excluded_dirs = NULL;
    int excluded_count = 0;
    cbm_pipeline_get_excluded(p, &excluded_dirs, &excluded_count);

    cbm_path_alias_collection_t *path_aliases =
        cbm_load_path_aliases_excluded(cbm_pipeline_repo_path(p), excluded_dirs, excluded_count);

    cbm_pipeline_ctx_t ctx = {
        .project_name = project,
        .repo_path = cbm_pipeline_repo_path(p),
        .gbuf = existing,
        .registry = registry,
        .cancelled = cbm_pipeline_cancelled_ptr(p),
        .pipeline = p, /* so passes can record per-file skips (Track B) */
        .mode = cbm_pipeline_get_mode(p),
        .path_aliases = path_aliases,
        .excluded_dirs = excluded_dirs,
        .excluded_count = excluded_count,
    };

    for (int i = 0; i < ci; i++) {
        incr_add_changed_file_structure(existing, project, &changed_files[i]);
    }

    int extract_rc = run_extract_resolve(&ctx, changed_files, ci);
    /* #49: extract + resolve work sets are done; gate before the derived
     * passes and publication so an over-budget run stops here. */
    if (extract_rc == 0) {
        extract_rc = cbm_pipeline_budget_gate("incr_resolve");
    }
    if (extract_rc != 0) {
        incr_free_edge_capture(&edge_cap);
        free(changed_files);
        cbm_registry_free(registry);
        cbm_path_alias_collection_free(path_aliases);
        cbm_pkgmap_free(ctx.pkgmap);
        free_mode_skipped(mode_skipped, mode_skipped_count);
        cbm_gbuf_free(existing);
        return extract_rc;
    }
    /* Changed files got fresh File props above, so a stale coverage stamp
     * cannot survive; unchanged files keep theirs from the generation. */
    cbm_pipeline_stamp_coverage(p, existing);
    cbm_pipeline_pass_k8s(&ctx, changed_files, ci);

    incr_rebuild_snapshot_callers(&ctx, &edge_cap, files, file_count);

    /* Restore inbound cross-file edges before any derived pass reads the
     * mixed graph. Semantic vectors and complexity both consume CALLS
     * topology, so restoring only after those passes made their output differ
     * from a clean full index even though the final SQLite edges matched. */
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    int relinked = incr_restore_inbound_edges(existing, &edge_cap);
    cbm_log_info("incremental.edge_relink", "relinked", itoa_buf(relinked), "captured",
                 itoa_buf(edge_cap.count), "elapsed_ms", itoa_buf((int)elapsed_ms(t)));
    incr_free_edge_capture(&edge_cap);

    run_postpasses(&ctx, changed_files, ci, project, &derived_before);

    free(changed_files);
    cbm_registry_free(registry);
    cbm_path_alias_collection_free(path_aliases);
    cbm_pkgmap_free(ctx.pkgmap); /* built by the parallel extract, if it ran */
    ctx.pkgmap = NULL;

    /* Step 7: Publish (preserves mode-skipped hash rows so the next reindex
     * can correctly classify those files instead of seeing them as
     * never-existed). */
    /* Record committed counts before dump_and_persist (whose dump frees the
     * gbuf node index, zeroing the count) so the #334 plausibility gate also
     * covers incremental reindexes, not just full ones. */
    cbm_pipeline_set_committed_counts(p, cbm_gbuf_node_count(existing),
                                      cbm_gbuf_edge_count(existing));
    int persist_rc = dump_and_persist(p, existing, files, file_count, mode_skipped,
                                      mode_skipped_count, snapshot);
    free_mode_skipped(mode_skipped, mode_skipped_count);
    cbm_gbuf_free(existing);
    if (persist_rc != 0) {
        return persist_rc;
    }

    cbm_log_info("incremental.done", "elapsed_ms", itoa_buf((int)elapsed_ms(t0)));
    return 0;
}

int cbm_pipeline_run_incremental_zova(cbm_pipeline_t *p, const char *zova_path,
                                      cbm_file_info_t *files, int file_count,
                                      cbm_zova_workspace_snapshot_t *snapshot,
                                      cbm_gbuf_t *preloaded_graph) {
    if (!snapshot || !preloaded_graph) {
        return CBM_NOT_FOUND;
    }
    return run_incremental(p, files, file_count, snapshot, preloaded_graph);
}
