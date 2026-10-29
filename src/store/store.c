/*
 * store.c — SQLite graph store implementation.
 *
 * Implements the opaque cbm_store_t handle with prepared statement caching,
 * schema initialization, and all CRUD operations for nodes, edges, projects,
 * file hashes, search, BFS traversal, and schema introspection.
 */

// for ISO timestamp

#include <stdint.h>
#include "foundation/constants.h"

#include <math.h>
#include <limits.h>

enum {
    ST_COL_1 = 1,
    ST_COL_2 = 2,
    ST_COL_3 = 3,
    ST_COL_4 = 4,
    ST_COL_5 = 5,
    ST_COL_6 = 6,
    ST_COL_7 = 7,
    ST_COL_8 = 8,
    ST_COL_9 = 9,
    ST_FOUND = -1,
    ST_BUF_16 = 16,
    ST_BUF_64 = 64,
    /* file: URI for the immutable read-only fallback. A path is at most
     * CBM_SZ_1K; percent-encoding can triple it, plus the "file://" prefix,
     * the "?immutable=1" suffix and a leading '/'. */
    ST_QUERY_URI_MAX = 3 * 1024 + 64,
    /* Matches the PRAGMA busy_timeout configure_pragmas applies. */
    ST_BUSY_TIMEOUT_MS = 10000,
    ST_GROWTH = 2,
    ST_MAX_DEGREE = 8192,
    ST_HALF_SEC = 500000,
    ST_RETRY_WAIT_US = 1000,
    ST_INIT_CAP_8 = 8,
    ST_INIT_CAP_16 = 16,
    ST_SQL_BUF = 8192,
    ST_MAX_ROW_CHECK = 5,
    ST_QN_MAX_DOTS = 5,
    ST_QN_MIN_DOTS = 3,
    ST_IN_CLAUSE_MARGIN = 4,
    ST_GLOB_MIN_LEN = 3,
    ST_GLOB_SKIP = 2,
    ST_MAX_LANG = 10,
    ST_SEARCH_MAX_BINDS = 32, /* increased: LIKE pre-filter adds binds per pattern */
    ST_LIKE_POOL_MAX = 12,    /* max malloc'd LIKE strings alive during one search */
    ST_LIKE_HINT_MAX = 2,     /* max LIKE hints extracted per regex pattern */
    ST_MAX_PKGS = 64,
    ST_INIT_CAP_4 = 4,
    ST_HEADER_PREFIX = 3,
    ST_MIN_INDEGREE = 3,
    ST_MAX_PATH_DEPTH = 3,
    ST_MAX_ITERATIONS = 10,
    ST_MAX_SECTIONS = 16,
    ST_METHOD_PROP_LEN = 8,
    ST_PATH_PROP_LEN = 6,
    ST_HANDLER_PROP_LEN = 9,
};

#define SLEN(s) (sizeof(s) - 1)
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_repository.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/log.h"
#include "foundation/compat_regex.h"
#include "foundation/str_util.h"

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

#include <sqlite3.h>
#include <yyjson/yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── SQLite bind helpers ───────────────────────────────────────── */

/* Isolate the int-to-ptr cast that SQLITE_TRANSIENT expands to.
   A union type-pun avoids the performance-no-int-to-ptr diagnostic. */
static sqlite3_destructor_type make_transient(void) {
    union {
        uintptr_t i;
        sqlite3_destructor_type fn;
    } u;
    u.i = (uintptr_t)CBM_NOT_FOUND;
    return u.fn;
}
#define BIND_TRANSIENT (make_transient())

static int bind_text(sqlite3_stmt *s, int col, const char *v) {
    return sqlite3_bind_text(s, col, v, CBM_NOT_FOUND, BIND_TRANSIENT);
}

/* ── Internal store structure ───────────────────────────────────── */

/* One cbm edge of a Zova workspace store: Zova node keys, which are also the
 * ids of the nodes view, and an index into the store's edge type names. */
typedef struct {
    int64_t source_key;
    int64_t target_key;
    int type;
} store_zova_edge_t;

struct cbm_store {
    sqlite3 *db;
    const char *db_path; /* heap-allocated, or NULL for :memory: */
    /* Set only by cbm_store_open_backend; db is NULL then. */
    const cbm_store_backend_t *backend;
    void *backend_ctx;
    char errbuf[CBM_SZ_512];

    /* Read-only Zova sidecar session. One store is single-thread-owned. */
    cbm_zova_vector_session_t *zova_vector_session;
    char *zova_vector_session_path;
    struct stat zova_vector_session_stat;
    bool zova_vector_session_stat_valid;

    /* Read-only Zova graph session. Separate from vectors so either sidecar
     * capability can be unavailable without disturbing the other. */
    cbm_zova_graph_session_t *zova_graph_session;
    char *zova_graph_workspace_root;
    char zova_graph_workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    int64_t zova_graph_cached_numeric_id;
    char zova_graph_cached_stable_id[CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    bool zova_graph_cached_stable_id_valid;
    bool zova_workspace_query;
    cbm_zova_repository_t *zova_repository;
    /* Legacy nodes view over the workspace; see store_legacy_scope. */
    char *zova_legacy_project;
    /* The workspace's edges, read once from the graph; see store_zova_edges. */
    store_zova_edge_t *zova_edges;
    int zova_edge_count;
    bool zova_edges_loaded;
    char **zova_edge_types;
    int zova_edge_type_count;
    int zova_native_topology_ops;

    /* Prepared statements (lazily initialized, cached for lifetime) */
    sqlite3_stmt *stmt_upsert_node;
    sqlite3_stmt *stmt_find_node_by_id;
    sqlite3_stmt *stmt_find_node_by_qn;
    sqlite3_stmt *stmt_find_node_by_qn_any; /* QN lookup without project filter */
    sqlite3_stmt *stmt_find_nodes_by_name;
    sqlite3_stmt *stmt_find_nodes_by_name_any; /* name lookup without project filter */
    sqlite3_stmt *stmt_find_nodes;
    sqlite3_stmt *stmt_find_nodes_by_label;
    sqlite3_stmt *stmt_find_nodes_by_file;
    sqlite3_stmt *stmt_count_nodes;
    sqlite3_stmt *stmt_delete_nodes_by_project;
    sqlite3_stmt *stmt_delete_nodes_by_file;
    sqlite3_stmt *stmt_delete_nodes_by_label;

    sqlite3_stmt *stmt_insert_edge;
    sqlite3_stmt *stmt_find_edges_by_source;
    sqlite3_stmt *stmt_find_edges_by_target;
    sqlite3_stmt *stmt_find_edges_by_source_type;
    sqlite3_stmt *stmt_find_edges_by_target_type;
    sqlite3_stmt *stmt_find_edges_by_type;
    sqlite3_stmt *stmt_count_edges;
    sqlite3_stmt *stmt_count_edges_by_type;
    sqlite3_stmt *stmt_delete_edges_by_project;
    sqlite3_stmt *stmt_delete_edges_by_type;

    sqlite3_stmt *stmt_upsert_project;
    sqlite3_stmt *stmt_get_project;
    sqlite3_stmt *stmt_list_projects;
    sqlite3_stmt *stmt_delete_project;

    sqlite3_stmt *stmt_upsert_file_hash;
    sqlite3_stmt *stmt_get_file_hashes;
    sqlite3_stmt *stmt_delete_file_hash;
    sqlite3_stmt *stmt_delete_file_hashes;
};

static void store_close_zova_vector_session(cbm_store_t *s);
static void store_close_zova_graph_session(cbm_store_t *s);
static void store_clear_zova_graph_workspace(cbm_store_t *s);

/* ── Helpers ────────────────────────────────────────────────────── */

static void store_set_error(cbm_store_t *s, const char *msg) {
    snprintf(s->errbuf, sizeof(s->errbuf), "%s", msg);
}

static void store_set_error_sqlite(cbm_store_t *s, const char *prefix) {
    snprintf(s->errbuf, sizeof(s->errbuf), "%s: %s", prefix, sqlite3_errmsg(s->db));
}

static int exec_sql(cbm_store_t *s, const char *sql) {
    if (!s || !s->db) {
        return CBM_STORE_ERR;
    }
    char *err = NULL;
    int rc = sqlite3_exec(s->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        snprintf(s->errbuf, sizeof(s->errbuf), "exec: %s", err ? err : "unknown");
        sqlite3_free(err);
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

/* Safe string: returns "" if NULL. */
static const char *safe_str(const char *s) {
    return s ? s : "";
}

/* Safe properties: returns "{}" if NULL. */
static const char *safe_props(const char *s) {
    return (s && s[0]) ? s : "{}";
}

/* Duplicate a string onto the heap. */
static char *heap_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *d = malloc(len + SKIP_ONE);
    if (d) {
        memcpy(d, s, len + SKIP_ONE);
    }
    return d;
}

/* Prepare a statement (cached). If already prepared, reset+clear. */
static sqlite3_stmt *prepare_cached(cbm_store_t *s, sqlite3_stmt **slot, const char *sql) {
    if (!s || !s->db) {
        return NULL;
    }
    if (*slot) {
        sqlite3_reset(*slot);
        sqlite3_clear_bindings(*slot);
        return *slot;
    }
    int rc = sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, slot, NULL);
    if (rc != SQLITE_OK) {
        store_set_error_sqlite(s, "prepare");
        return NULL;
    }
    return *slot;
}

/* Get ISO-8601 timestamp. */
static void iso_now(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tm;
    cbm_gmtime_r(&t, &tm);
    (void)strftime(buf, sz, "%Y-%m-%dT%H:%M:%SZ",
                   &tm); // cert-err33-c: strftime only fails if buffer is too small — 21-byte ISO
                         // timestamp always fits in caller-provided buffers
}

/* ── Schema ─────────────────────────────────────────────────────── */

static int init_schema(cbm_store_t *s) {
    const char *ddl =
        "CREATE TABLE IF NOT EXISTS projects ("
        "  name TEXT PRIMARY KEY,"
        "  indexed_at TEXT NOT NULL,"
        "  root_path TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS file_hashes ("
        "  project TEXT NOT NULL REFERENCES projects(name) ON DELETE CASCADE,"
        "  rel_path TEXT NOT NULL,"
        "  sha256 TEXT NOT NULL,"
        "  mtime_ns INTEGER NOT NULL DEFAULT 0,"
        "  size INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (project, rel_path)"
        ");"
        "CREATE TABLE IF NOT EXISTS nodes ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project TEXT NOT NULL REFERENCES projects(name) ON DELETE CASCADE,"
        "  label TEXT NOT NULL,"
        "  name TEXT NOT NULL,"
        "  qualified_name TEXT NOT NULL,"
        "  file_path TEXT DEFAULT '',"
        "  start_line INTEGER DEFAULT 0,"
        "  end_line INTEGER DEFAULT 0,"
        "  properties TEXT DEFAULT '{}',"
        "  UNIQUE(project, qualified_name)"
        ");"
        /* local_name_gen (#768): IMPORTS edges carry one imported symbol's
         * local_name each, so uniqueness must discriminate on it — two named
         * imports from the same specifier are distinct edges. Non-IMPORTS
         * edges get '' (NOT NULL: NULLs never conflict in a UNIQUE index,
         * which would break their dedup entirely). Mirrors the graph-buffer
         * dedup key (make_edge_key) — keep both in sync. */
        "CREATE TABLE IF NOT EXISTS edges ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project TEXT NOT NULL REFERENCES projects(name) ON DELETE CASCADE,"
        "  source_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,"
        "  target_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,"
        "  type TEXT NOT NULL,"
        "  properties TEXT DEFAULT '{}',"
        "  url_path_gen TEXT GENERATED ALWAYS AS (json_extract(properties,'$.url_path')),"
        "  local_name_gen TEXT GENERATED ALWAYS AS (CASE WHEN type='IMPORTS'"
        "    THEN coalesce(json_extract(properties,'$.local_name'),'') ELSE '' END),"
        "  UNIQUE(source_id, target_id, type, local_name_gen)"
        ");"
        "CREATE TABLE IF NOT EXISTS project_summaries ("
        "  project TEXT PRIMARY KEY,"
        "  summary TEXT NOT NULL,"
        "  source_hash TEXT NOT NULL,"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");";

    int rc = exec_sql(s, ddl);
    if (rc != CBM_STORE_OK) {
        return rc;
    }

    /* Schema-compat probe (#768): DBs created before the local_name_gen
     * discriminator still enforce UNIQUE(source_id,target_id,type) and lack
     * the column — the widened upsert in cbm_store_insert_edge can neither
     * prepare nor let two named imports coexist against them. SQLite cannot
     * ALTER a table-level UNIQUE constraint in place, so fail the open:
     * callers already treat an unopenable DB as incompatible (a full index
     * deletes + rebuilds it, artifact import refuses and falls back to a
     * reindex). Read-only query opens skip init_schema and keep working. */
    {
        sqlite3_stmt *probe = NULL;
        if (sqlite3_prepare_v2(s->db, "SELECT local_name_gen FROM edges LIMIT 0;", CBM_NOT_FOUND,
                               &probe, NULL) != SQLITE_OK) {
            cbm_log_warn("store.schema", "result", "incompatible", "missing",
                         "edges.local_name_gen");
            return CBM_STORE_ERR;
        }
        sqlite3_finalize(probe);
    }

    /* FTS5 contentless virtual table for BM25 full-text search.
     * Contentless (content='') means FTS5 stores only the inverted index,
     * not a copy of the source text — required for camelCase tokenization
     * because we feed it `cbm_camel_split(name)` at insert time but want
     * queries to match against the split tokens, not the original.
     * Fails silently if FTS5 is not compiled in (SQLITE_ENABLE_FTS5). */
    {
        char *fts_err = NULL;
        int fts_rc = sqlite3_exec(s->db,
                                  "CREATE VIRTUAL TABLE IF NOT EXISTS nodes_fts USING fts5("
                                  "  name, qualified_name, label, file_path,"
                                  "  content='',"
                                  "  tokenize='unicode61 remove_diacritics 2'"
                                  ");",
                                  NULL, NULL, &fts_err);
        if (fts_rc != SQLITE_OK && fts_err) {
            sqlite3_free(fts_err);
        }
    }
    return CBM_STORE_OK;
}

static int create_user_indexes(cbm_store_t *s) {
    const char *sql =
        "CREATE INDEX IF NOT EXISTS idx_nodes_label ON nodes(project, label);"
        "CREATE INDEX IF NOT EXISTS idx_nodes_name ON nodes(project, name);"
        "CREATE INDEX IF NOT EXISTS idx_nodes_file ON nodes(project, file_path);"
        "CREATE INDEX IF NOT EXISTS idx_edges_source ON edges(source_id, type);"
        "CREATE INDEX IF NOT EXISTS idx_edges_target ON edges(target_id, type);"
        "CREATE INDEX IF NOT EXISTS idx_edges_type ON edges(project, type);"
        "CREATE INDEX IF NOT EXISTS idx_edges_target_type ON edges(project, target_id, type);"
        "CREATE INDEX IF NOT EXISTS idx_edges_source_type ON edges(project, source_id, type);"
        "CREATE INDEX IF NOT EXISTS idx_edges_url_path ON edges(project, url_path_gen);";
    /* NOTE: a partial expression index on json_extract(properties,'$.is_entry_point')
     * was tried for arch_entry_points and REVERTED: json_extract in an index WHERE
     * aborts CREATE INDEX (and thus store open) on any row whose properties JSON is
     * malformed — and pre-fix databases contain such rows (see
     * pipeline_def_props_valid_json_when_oversized). Revisit only with a
     * json_valid()-guarded expression once legacy DBs have aged out. */
    return exec_sql(s, sql);
}

int64_t cbm_store_resolve_mmap_size(void) {
    enum { MMAP_DEFAULT = 67108864, BASE_10 = 10 }; /* default 64 MB; decimal radix */
    char buf[ST_BUF_64];
    if (cbm_safe_getenv("CBM_SQLITE_MMAP_SIZE", buf, sizeof(buf), NULL) == NULL) {
        return (int64_t)MMAP_DEFAULT;
    }
    char *end = NULL;
    long long parsed = strtoll(buf, &end, BASE_10);
    if (end == buf || *end != '\0') {
        /* Malformed — fall back to default rather than fail the store open. */
        return (int64_t)MMAP_DEFAULT;
    }
    if (parsed < 0) {
        return 0;
    }
    return (int64_t)parsed;
}

/* Configure connection pragmas.
 *   in_memory  — :memory: DB (synchronous OFF, no journal file).
 *   read_only  — query-only connection opened SQLITE_OPEN_READONLY. Runs
 *                ONLY non-writing pragmas (foreign_keys, temp_store,
 *                busy_timeout, mmap_size) and SKIPS journal_mode=WAL,
 *                wal_checkpoint and synchronous — those WRITE to the DB/WAL
 *                and would (a) mutate the DB on every read query and (b)
 *                fail on a read-only DB file / filesystem.
 * in_memory and read_only are mutually exclusive. */
static int configure_pragmas(cbm_store_t *s, bool in_memory, bool read_only) {
    int rc;
    rc = exec_sql(s, "PRAGMA foreign_keys = ON;");
    if (rc != CBM_STORE_OK) {
        return rc;
    }
    rc = exec_sql(s, "PRAGMA temp_store = MEMORY;");
    if (rc != CBM_STORE_OK) {
        return rc;
    }

    if (in_memory) {
        rc = exec_sql(s, "PRAGMA synchronous = OFF;");
    } else if (read_only) {
        /* Non-writing pragmas only — see the function comment. */
        rc = exec_sql(s, "PRAGMA busy_timeout = 10000;");
        if (rc != CBM_STORE_OK) {
            return rc;
        }
        char mmap_sql[ST_BUF_64];
        snprintf(mmap_sql, sizeof(mmap_sql), "PRAGMA mmap_size = %lld;",
                 (long long)cbm_store_resolve_mmap_size());
        rc = exec_sql(s, mmap_sql);
    } else {
        rc = exec_sql(s, "PRAGMA busy_timeout = 10000;");
        if (rc != CBM_STORE_OK) {
            return rc;
        }
        rc = exec_sql(s, "PRAGMA journal_mode = WAL;");
        if (rc != CBM_STORE_OK) {
            return rc;
        }
        /* Recover stale WAL from previous crash (best-effort).
         * PASSIVE never blocks readers and never ftruncates.
         * May fail with SQLITE_BUSY if another process holds a lock. */
        (void)sqlite3_exec(s->db, "PRAGMA wal_checkpoint(PASSIVE)", NULL, NULL, NULL);
        rc = exec_sql(s, "PRAGMA synchronous = NORMAL;");
        if (rc != CBM_STORE_OK) {
            return rc;
        }
        char mmap_sql[ST_BUF_64];
        snprintf(mmap_sql, sizeof(mmap_sql), "PRAGMA mmap_size = %lld;",
                 (long long)cbm_store_resolve_mmap_size());
        rc = exec_sql(s, mmap_sql);
    }
    return rc;
}

/* ── camelCase splitter for FTS5 indexing ───────────────────────── */

/* Emits the original identifier plus a space-separated split version, so FTS5's
 * whitespace tokenizer produces both `updateCloudClient` (exact match) and the
 * word tokens `update`, `cloud`, `client`.  Rules:
 *   1. insert space before an uppercase letter preceded by a lowercase letter
 *      ("updateCloud" → "update Cloud")
 *   2. insert space before an uppercase letter preceded by another uppercase
 *      but followed by a lowercase letter ("XMLParser" → "XML Parser", not
 *      "X M L Parser")
 * snake_case is already split by FTS5's unicode61 tokenizer on `_`. */
enum {
    CAMEL_SPLIT_BUF = 2048,
    SQLITE_AUTO_LEN = -1, /* sqlite3_result_text sentinel: use strlen(). */
    CAMEL_BUF_GUARD = 2,  /* reserve room for inserted space + NUL terminator. */
};

/* Denominator epsilon guard for double-precision cosine. */
#define CBM_STORE_DENOM_EPS_D 1e-10
#define CBM_STORE_DENOM_EPS_F 1e-10F
#define CBM_STORE_INT8_MAX 127.0F
#define CBM_STORE_UNIT_POS_F 1.0F
#define CBM_STORE_UNIT_POS_D 1.0

/* Module-local copy of SQLite's SQLITE_TRANSIENT sentinel ((void*)-1).
 * We construct it via memcpy from a volatile intptr_t sentinel so the
 * resulting expression isn't syntactically a direct int-to-ptr cast —
 * clang-tidy's performance-no-int-to-ptr sees the memcpy boundary and
 * doesn't flag it per-use-site. */
static sqlite3_destructor_type cbm_sqlite_transient_destructor(void) {
    static const volatile intptr_t raw = -1;
    sqlite3_destructor_type dtor = NULL;
    memcpy(&dtor, (const void *)&raw, sizeof(dtor));
    return dtor;
}
#define CBM_SQLITE_TRANSIENT (cbm_sqlite_transient_destructor())

/* True if we should insert a space BEFORE input[i] to split a camelCase word
 * boundary (lowercase→uppercase or uppercase-run→uppercase-then-lowercase). */
static bool camel_should_split(const char *input, int i) {
    if (i <= 0) {
        return false;
    }
    char curr = input[i];
    char prev = input[i - SKIP_ONE];
    char next = input[i + SKIP_ONE];
    if (curr >= 'A' && curr <= 'Z' && prev >= 'a' && prev <= 'z') {
        return true;
    }
    if (curr >= 'A' && curr <= 'Z' && prev >= 'A' && prev <= 'Z' && next >= 'a' && next <= 'z') {
        return true;
    }
    return false;
}

static void sqlite_camel_split(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *input = (const char *)sqlite3_value_text(argv[0]);
    if (!input || !input[0]) {
        sqlite3_result_text(ctx, input ? input : "", SQLITE_AUTO_LEN, CBM_SQLITE_TRANSIENT);
        return;
    }
    char buf[CAMEL_SPLIT_BUF];
    int len = snprintf(buf, sizeof(buf), "%s ", input);
    if (len < 0 || len >= (int)sizeof(buf)) {
        /* Input too long — fall back to the original string unmodified. */
        sqlite3_result_text(ctx, input, SQLITE_AUTO_LEN, CBM_SQLITE_TRANSIENT);
        return;
    }
    for (int i = 0; input[i] && len < (int)sizeof(buf) - CAMEL_BUF_GUARD; i++) {
        if (camel_should_split(input, i)) {
            buf[len++] = ' ';
        }
        buf[len++] = input[i];
    }
    buf[len] = '\0';
    sqlite3_result_text(ctx, buf, len, CBM_SQLITE_TRANSIENT);
}

/* ── REGEXP function for SQLite ──────────────────────────────────── */

/* Destructor passed to sqlite3_set_auxdata — frees the cached compiled regex. */
static void regex_free_cb(void *p) {
    cbm_regex_t *re = (cbm_regex_t *)p;
    cbm_regfree(re);
    free(re);
}

/* Cache the compiled regex on argument slot 0 for the lifetime of the statement.
 * sqlite3_get_auxdata returns the cached pointer on subsequent rows (same parameter
 * value), so cbm_regcomp is called exactly once per statement instead of once per row. */
static void sqlite_regexp(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *pattern = (const char *)sqlite3_value_text(argv[0]);
    const char *text = (const char *)sqlite3_value_text(argv[SKIP_ONE]);
    if (!pattern || !text) {
        sqlite3_result_int(ctx, 0);
        return;
    }

    cbm_regex_t *re = (cbm_regex_t *)sqlite3_get_auxdata(ctx, 0);
    if (!re) {
        re = malloc(sizeof(cbm_regex_t));
        if (!re) {
            sqlite3_result_error_nomem(ctx);
            return;
        }
        if (cbm_regcomp(re, pattern, CBM_REG_EXTENDED | CBM_REG_NOSUB) != 0) {
            free(re);
            sqlite3_result_error(ctx, "invalid regex", CBM_NOT_FOUND);
            return;
        }
        sqlite3_set_auxdata(ctx, 0, re, regex_free_cb);
    }

    sqlite3_result_int(ctx, cbm_regexec(re, text, 0, NULL, 0) == 0 ? SKIP_ONE : 0);
}

/* Case-insensitive REGEXP variant — same auxdata caching strategy. */
static void sqlite_iregexp(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *pattern = (const char *)sqlite3_value_text(argv[0]);
    const char *text = (const char *)sqlite3_value_text(argv[SKIP_ONE]);
    if (!pattern || !text) {
        sqlite3_result_int(ctx, 0);
        return;
    }

    cbm_regex_t *re = (cbm_regex_t *)sqlite3_get_auxdata(ctx, 0);
    if (!re) {
        re = malloc(sizeof(cbm_regex_t));
        if (!re) {
            sqlite3_result_error_nomem(ctx);
            return;
        }
        if (cbm_regcomp(re, pattern, CBM_REG_EXTENDED | CBM_REG_NOSUB | CBM_REG_ICASE) != 0) {
            free(re);
            sqlite3_result_error(ctx, "invalid regex", CBM_NOT_FOUND);
            return;
        }
        sqlite3_set_auxdata(ctx, 0, re, regex_free_cb);
    }

    sqlite3_result_int(ctx, cbm_regexec(re, text, 0, NULL, 0) == 0 ? SKIP_ONE : 0);
}

/* Cosine similarity between two int8 BLOB vectors.
 * Returns float in [-1, 1].  Used for vector search at query time. */
static void sqlite_cosine_i8(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) != SQLITE_BLOB ||
        sqlite3_value_type(argv[SKIP_ONE]) != SQLITE_BLOB) {
        sqlite3_result_double(ctx, 0.0);
        return;
    }
    int len_a = sqlite3_value_bytes(argv[0]);
    int len_b = sqlite3_value_bytes(argv[SKIP_ONE]);
    if (len_a != len_b || len_a == 0) {
        sqlite3_result_double(ctx, 0.0);
        return;
    }
    const int8_t *a = (const int8_t *)sqlite3_value_blob(argv[0]);
    const int8_t *b = (const int8_t *)sqlite3_value_blob(argv[SKIP_ONE]);
    int32_t dot = 0;
    int32_t mag_a = 0;
    int32_t mag_b = 0;
    for (int i = 0; i < len_a; i++) {
        dot += (int32_t)a[i] * (int32_t)b[i];
        mag_a += (int32_t)a[i] * (int32_t)a[i];
        mag_b += (int32_t)b[i] * (int32_t)b[i];
    }
    double denom = sqrt((double)mag_a) * sqrt((double)mag_b);
    sqlite3_result_double(ctx, denom > CBM_STORE_DENOM_EPS_D ? (double)dot / denom : 0.0);
}

/* ── Lifecycle ──────────────────────────────────────────────────── */

/* SQLite authorizer: deny dangerous operations that could be exploited via
 * SQL injection through the Cypher→SQL translation layer. */
static int store_authorizer(void *user_data, int action, const char *p3, const char *p4,
                            const char *p5, const char *p6) {
    (void)user_data;
    (void)p3;
    (void)p4;
    (void)p5;
    (void)p6;
    switch (action) {
    case SQLITE_ATTACH: /* ATTACH DATABASE — could create/read arbitrary files */
    case SQLITE_DETACH: /* DETACH DATABASE */
        return SQLITE_DENY;
    default:
        return SQLITE_OK;
    }
}

static cbm_store_t *store_open_internal(const char *path, bool in_memory) {
    cbm_store_t *s = calloc(CBM_ALLOC_ONE, sizeof(cbm_store_t));
    if (!s) {
        return NULL;
    }

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (in_memory) {
        flags |= SQLITE_OPEN_MEMORY;
    }

    int rc = sqlite3_open_v2(path, &s->db, flags, NULL);
    if (rc != SQLITE_OK) {
        free(s);
        return NULL;
    }

    if (path && !in_memory) {
        s->db_path = heap_strdup(path);
    }

    /* Security: block ATTACH/DETACH to prevent file creation via SQL injection.
     * The authorizer runs inside SQLite's query planner — no string-level bypass. */
    sqlite3_set_authorizer(s->db, store_authorizer, NULL);

    /* Register REGEXP function (SQLite doesn't have one built-in) */
    sqlite3_create_function(s->db, "regexp", ST_COL_2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                            sqlite_regexp, NULL, NULL);
    /* Case-insensitive variant for search with case_sensitive=false */
    sqlite3_create_function(s->db, "iregexp", ST_COL_2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                            sqlite_iregexp, NULL, NULL);
    /* Int8 cosine similarity for vector search */
    sqlite3_create_function(s->db, "cbm_cosine_i8", ST_COL_2, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                            NULL, sqlite_cosine_i8, NULL, NULL);
    /* camelCase splitter for FTS5 BM25 indexing */
    sqlite3_create_function(s->db, "cbm_camel_split", SKIP_ONE, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                            NULL, sqlite_camel_split, NULL, NULL);

    if (configure_pragmas(s, in_memory, false) != CBM_STORE_OK || init_schema(s) != CBM_STORE_OK ||
        create_user_indexes(s) != CBM_STORE_OK) {
        sqlite3_close(s->db);
        safe_str_free(&s->db_path);
        free(s);
        return NULL;
    }

    return s;
}

cbm_store_t *cbm_store_open_memory(void) {
    return store_open_internal(":memory:", true);
}

static bool store_cleanup_pending(const char *db_path) {
    char marker[2048];
    int n = snprintf(marker, sizeof(marker), "%s.cbm-cleanup-pending", db_path ? db_path : "");
    if (n <= 0 || (size_t)n >= sizeof(marker)) return false;
    struct stat marker_stat;
    return cbm_lstat(marker, &marker_stat) == 0;
}

const char *cbm_store_db_path(const cbm_store_t *s) {
    return s ? s->db_path : NULL;
}

/* Build a SQLite "file:" URI with immutable=1 from a filesystem path.
 * immutable=1 bypasses WAL and locking and reads the main DB file directly —
 * used only as a fallback for read-only filesystems where the wal-index
 * (-shm) cannot be created. URI-special characters are percent-encoded.
 * Windows backslashes are normalized to '/', and a leading '/' is inserted
 * for drive-letter paths so the drive is not parsed as a URI authority.
 * Returns false if the output buffer is too small. */
static bool build_immutable_uri(const char *path, char *out, size_t out_sz) {
    static const char PREFIX[] = "file://";
    static const char SUFFIX[] = "?immutable=1";
    static const char HEX[] = "0123456789ABCDEF";
    size_t prefix_len = sizeof(PREFIX) - 1;
    size_t suffix_len = sizeof(SUFFIX) - 1;
    if (prefix_len + 1 > out_sz) {
        return false;
    }
    memcpy(out, PREFIX, prefix_len);
    size_t pos = prefix_len;

    /* Ensure the path component begins with '/' (POSIX absolute paths already
     * do; Windows "C:\..." gets a leading '/' -> "/C:/..."). */
    if (path[0] != '/') {
        if (pos + 1 >= out_sz) {
            return false;
        }
        out[pos++] = '/';
    }

    for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++) {
        unsigned char c = *p;
        if (c == '\\') {
            c = '/'; /* normalize Windows separators */
        }
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '/' || c == '.' || c == '-' || c == '_' || c == '~' || c == ':';
        if (safe) {
            if (pos + 1 >= out_sz) {
                return false;
            }
            out[pos++] = (char)c;
        } else {
            if (pos + 3 >= out_sz) {
                return false;
            }
            out[pos++] = '%';
            out[pos++] = HEX[(c >> 4) & 0xF];
            out[pos++] = HEX[c & 0xF];
        }
    }

    if (pos + suffix_len + 1 > out_sz) {
        return false;
    }
    memcpy(out + pos, SUFFIX, suffix_len);
    pos += suffix_len;
    out[pos] = '\0';
    return true;
}

cbm_store_t *cbm_store_open_path_query(const char *db_path) {
    if (!db_path || store_cleanup_pending(db_path)) {
        return NULL;
    }

    cbm_store_t *s = calloc(CBM_ALLOC_ONE, sizeof(cbm_store_t));
    if (!s) {
        return NULL;
    }

    /* Query tools open the project DB READ-ONLY: a read query must never
     * mutate the DB (the previous READWRITE open + WAL write-pragmas did),
     * and must work on a read-only DB file / filesystem.
     *
     * Try a plain READONLY open first — on a normal writable filesystem this
     * reads WAL frames correctly via the -shm wal-index. SQLite opens lazily,
     * so a read-only-filesystem failure (cannot create -shm for a WAL-mode
     * DB) surfaces on first access, not at open time; we probe with a trivial
     * read to force it. If the probe fails, retry once with an immutable URI
     * that bypasses WAL and reads the main DB file directly.
     *
     * No SQLITE_OPEN_CREATE on either path — a missing DB must return NULL
     * (no ghost .db for unknown/unindexed projects). */
    int rc = sqlite3_open_v2(db_path, &s->db, SQLITE_OPEN_READONLY, NULL);
    if (rc == SQLITE_OK) {
        /* Wait out a concurrent writer before probing: a busy probe is not a
         * read-only filesystem, and the immutable fallback below ignores the
         * WAL, so it would serve a stale snapshot without recent commits. */
        sqlite3_busy_timeout(s->db, ST_BUSY_TIMEOUT_MS);
        /* Force first DB access so a read-only-FS WAL failure surfaces now. */
        if (sqlite3_exec(s->db, "SELECT 1 FROM sqlite_master LIMIT 1;", NULL, NULL, NULL) !=
            SQLITE_OK) {
            bool busy = (sqlite3_errcode(s->db) & 0xFF) == SQLITE_BUSY;
            sqlite3_close(s->db);
            s->db = NULL;
            if (busy) {
                free(s);
                return NULL;
            }
            rc = SQLITE_CANTOPEN; /* trigger immutable fallback */
        }
    }
    if (rc != SQLITE_OK) {
        sqlite3_close(s->db); /* no-op if already NULL */
        s->db = NULL;
        /* A genuinely missing DB must return NULL without creating anything —
         * only retry with the immutable URI when the file exists but could not
         * be opened (the read-only-filesystem case). This also keeps the
         * common "project not found" path to a single open attempt. */
        if (!cbm_file_exists(db_path)) {
            free(s);
            return NULL;
        }
        char uri[ST_QUERY_URI_MAX];
        if (!build_immutable_uri(db_path, uri, sizeof(uri))) {
            free(s);
            return NULL;
        }
        rc = sqlite3_open_v2(uri, &s->db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL);
        if (rc != SQLITE_OK) {
            /* sqlite3_open_v2 allocates a handle even on failure — must close it. */
            sqlite3_close(s->db);
            free(s);
            return NULL;
        }
    }

    s->db_path = heap_strdup(db_path);

    /* Security: block ATTACH/DETACH to prevent file creation via SQL injection. */
    sqlite3_set_authorizer(s->db, store_authorizer, NULL);

    /* Register REGEXP functions. */
    sqlite3_create_function(s->db, "regexp", ST_COL_2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                            sqlite_regexp, NULL, NULL);
    sqlite3_create_function(s->db, "iregexp", ST_COL_2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                            sqlite_iregexp, NULL, NULL);
    sqlite3_create_function(s->db, "cbm_cosine_i8", ST_COL_2, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                            NULL, sqlite_cosine_i8, NULL, NULL);
    sqlite3_create_function(s->db, "cbm_camel_split", SKIP_ONE, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                            NULL, sqlite_camel_split, NULL, NULL);

    if (configure_pragmas(s, false, true) != CBM_STORE_OK) {
        sqlite3_close(s->db);
        safe_str_free(&s->db_path);
        free(s);
        return NULL;
    }

    return s;
}

static int64_t store_stable_numeric_id(const char *value) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)value; p && *p; ++p)
        hash = (hash ^ *p) * 1099511628211ULL;
    return (int64_t)(hash & INT64_MAX);
}

cbm_store_t *cbm_store_open_zova_workspace_query(const char *db_path,
                                                 const char *workspace_id) {
    if (!db_path || cbm_zova_workspace_id_validate(workspace_id) != 0) return NULL;
    cbm_store_t *s = cbm_store_open_path_query(db_path);
    if (!s) return NULL;
    /* A user-local database can hold several workspaces, and graph hydration
     * precedes vector sampling on the same handle. A bounded 256 MiB ceiling
     * keeps both working sets resident on large repositories without changing
     * compatibility project-store behavior. SQLite allocates pages lazily. */
    if (sqlite3_exec(s->db, "PRAGMA cache_size=-262144", NULL, NULL, NULL) != SQLITE_OK) {
        cbm_store_close(s);
        return NULL;
    }
    sqlite3_stmt *project_stmt = NULL;
    char project[256] = {0};
    if (sqlite3_prepare_v2(
            s->db,
            "SELECT p.project FROM cbm_projects_v1 p JOIN cbm_workspace_registry r "
            "USING(workspace_key) WHERE r.workspace_id=?1",
            -1, &project_stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(project_stmt, 1, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(project_stmt) != SQLITE_ROW) {
        if (project_stmt) sqlite3_finalize(project_stmt);
        cbm_store_close(s);
        return NULL;
    }
    const char *project_value = (const char *)sqlite3_column_text(project_stmt, 0);
    if (project_value) snprintf(project, sizeof(project), "%s", project_value);
    sqlite3_finalize(project_stmt);
    if (!project[0]) {
        cbm_store_close(s);
        return NULL;
    }
    s->zova_repository = cbm_zova_repository_open(db_path, project);
    if (!s->zova_repository ||
        strcmp(cbm_zova_repository_workspace_id(s->zova_repository), workspace_id) != 0) {
        cbm_store_close(s);
        return NULL;
    }
    s->zova_graph_session = cbm_zova_graph_session_open(db_path);
    if (!s->zova_graph_session) {
        cbm_store_close(s);
        return NULL;
    }
    snprintf(s->zova_graph_workspace_id, sizeof(s->zova_graph_workspace_id), "%s", workspace_id);
    s->zova_workspace_query = true;
    return s;
}

int cbm_store_zova_native_topology_ops(const cbm_store_t *s) {
    return s ? s->zova_native_topology_ops : 0;
}

/* ── Integrity check ───────────────────────────────────────────── */

static void finalize_stmt(sqlite3_stmt **s) {
    if (*s) {
        sqlite3_finalize(*s);
        *s = NULL;
    }
}

cbm_store_t *cbm_store_open_backend(const cbm_store_backend_t *backend, void *ctx) {
    if (!backend) {
        return NULL;
    }
    cbm_store_t *s = calloc(1, sizeof(*s));
    if (s) {
        s->backend = backend;
        s->backend_ctx = ctx;
    }
    return s;
}

void cbm_store_close(cbm_store_t *s) {
    if (!s) {
        return;
    }
    if (s->backend) {
        free(s);
        return;
    }

    store_close_zova_vector_session(s);
    store_close_zova_graph_session(s);
    store_clear_zova_graph_workspace(s);
    cbm_zova_repository_close(s->zova_repository);
    s->zova_repository = NULL;
    free(s->zova_legacy_project);
    s->zova_legacy_project = NULL;
    free(s->zova_edges);
    for (int i = 0; i < s->zova_edge_type_count; i++) {
        free(s->zova_edge_types[i]);
    }
    free(s->zova_edge_types);

    /* Checkpoint WAL before close to prevent orphan WAL accumulation.
     * Best-effort — silently skips if concurrent reader holds a lock. */
    if (s->db && s->db_path) {
        (void)sqlite3_wal_checkpoint_v2(s->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    }

    /* Finalize all cached statements */
    finalize_stmt(&s->stmt_upsert_node);
    finalize_stmt(&s->stmt_find_node_by_id);
    finalize_stmt(&s->stmt_find_node_by_qn);
    finalize_stmt(&s->stmt_find_node_by_qn_any);
    finalize_stmt(&s->stmt_find_nodes_by_name);
    finalize_stmt(&s->stmt_find_nodes_by_name_any);
    finalize_stmt(&s->stmt_find_nodes);
    finalize_stmt(&s->stmt_find_nodes_by_label);
    finalize_stmt(&s->stmt_find_nodes_by_file);
    finalize_stmt(&s->stmt_count_nodes);
    finalize_stmt(&s->stmt_delete_nodes_by_project);
    finalize_stmt(&s->stmt_delete_nodes_by_file);
    finalize_stmt(&s->stmt_delete_nodes_by_label);

    finalize_stmt(&s->stmt_insert_edge);
    finalize_stmt(&s->stmt_find_edges_by_source);
    finalize_stmt(&s->stmt_find_edges_by_target);
    finalize_stmt(&s->stmt_find_edges_by_source_type);
    finalize_stmt(&s->stmt_find_edges_by_target_type);
    finalize_stmt(&s->stmt_find_edges_by_type);
    finalize_stmt(&s->stmt_count_edges);
    finalize_stmt(&s->stmt_count_edges_by_type);
    finalize_stmt(&s->stmt_delete_edges_by_project);
    finalize_stmt(&s->stmt_delete_edges_by_type);

    finalize_stmt(&s->stmt_upsert_project);
    finalize_stmt(&s->stmt_get_project);
    finalize_stmt(&s->stmt_list_projects);
    finalize_stmt(&s->stmt_delete_project);

    finalize_stmt(&s->stmt_upsert_file_hash);
    finalize_stmt(&s->stmt_get_file_hashes);
    finalize_stmt(&s->stmt_delete_file_hash);
    finalize_stmt(&s->stmt_delete_file_hashes);

    /* Use sqlite3_close_v2 — auto-deallocates when last statement finalizes.
     * Prevents ASan false-positive leaks from sqlite3 internal state. */
    sqlite3_close_v2(s->db);
    safe_str_free(&s->db_path);
    free(s);
}

sqlite3 *cbm_store_get_db(cbm_store_t *s) {
    return s ? s->db : NULL;
}

int cbm_store_exec(cbm_store_t *s, const char *sql) {
    return exec_sql(s, sql);
}

const char *cbm_store_error(cbm_store_t *s) {
    return s ? s->errbuf : "null store";
}

/* ── Transaction ────────────────────────────────────────────────── */

int cbm_store_begin(cbm_store_t *s) {
    return exec_sql(s, "BEGIN IMMEDIATE;");
}

int cbm_store_commit(cbm_store_t *s) {
    return exec_sql(s, "COMMIT;");
}

int cbm_store_rollback(cbm_store_t *s) {
    return exec_sql(s, "ROLLBACK;");
}

/* ── Bulk write ─────────────────────────────────────────────────── */

int cbm_store_begin_bulk(cbm_store_t *s) {
    /* Stay in WAL mode throughout. Switching to MEMORY journal mode would
     * make the database unrecoverable if the process crashes mid-write,
     * because the in-memory rollback journal is lost on crash.
     * WAL mode is crash-safe: uncommitted WAL entries are simply discarded
     * on the next open. Performance is preserved via synchronous=OFF and a
     * larger cache, which are safe with WAL. */
    int rc = exec_sql(s, "PRAGMA synchronous = OFF;");
    if (rc != CBM_STORE_OK) {
        return rc;
    }
    return exec_sql(s, "PRAGMA cache_size = -65536;"); /* CBM_SZ_64 MB */
}

int cbm_store_end_bulk(cbm_store_t *s) {
    int rc = exec_sql(s, "PRAGMA synchronous = NORMAL;");
    if (rc != CBM_STORE_OK) {
        return rc;
    }
    return exec_sql(s, "PRAGMA cache_size = -2000;"); /* default ~2 MB */
}

int cbm_store_drop_indexes(cbm_store_t *s) {
    return exec_sql(s, "DROP INDEX IF EXISTS idx_nodes_label;"
                       "DROP INDEX IF EXISTS idx_nodes_name;"
                       "DROP INDEX IF EXISTS idx_nodes_file;"
                       "DROP INDEX IF EXISTS idx_edges_source;"
                       "DROP INDEX IF EXISTS idx_edges_target;"
                       "DROP INDEX IF EXISTS idx_edges_type;"
                       "DROP INDEX IF EXISTS idx_edges_target_type;"
                       "DROP INDEX IF EXISTS idx_edges_source_type;");
}

int cbm_store_create_indexes(cbm_store_t *s) {
    return create_user_indexes(s);
}

/* ── Checkpoint ─────────────────────────────────────────────────── */

/* ── Dump ───────────────────────────────────────────────────────── */

/* ── Project CRUD ───────────────────────────────────────────────── */

int cbm_store_upsert_project(cbm_store_t *s, const char *name, const char *root_path) {
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_upsert_project,
                       "INSERT INTO projects (name, indexed_at, root_path) VALUES (?1, ?2, ?3) "
                       "ON CONFLICT(name) DO UPDATE SET indexed_at=?2, root_path=?3;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    char ts[CBM_SZ_64];
    iso_now(ts, sizeof(ts));

    bind_text(stmt, SKIP_ONE, name);
    bind_text(stmt, ST_COL_2, ts);
    bind_text(stmt, ST_COL_3, root_path);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        store_set_error_sqlite(s, "upsert_project");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

int cbm_store_get_project(cbm_store_t *s, const char *name, cbm_project_t *out) {
    if (s->zova_workspace_query && s->zova_repository)
        return cbm_zova_repository_get_project(
            s->zova_repository, s->zova_graph_workspace_id, out);
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_get_project,
                       "SELECT name, indexed_at, root_path FROM projects WHERE name = ?1;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, name);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->name = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
        out->indexed_at = heap_strdup((const char *)sqlite3_column_text(stmt, SKIP_ONE));
        out->root_path = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_2));
        return CBM_STORE_OK;
    }
    return CBM_STORE_NOT_FOUND;
}

int cbm_store_list_projects(cbm_store_t *s, cbm_project_t **out, int *count) {
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_list_projects,
                       "SELECT name, indexed_at, root_path FROM projects ORDER BY name;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    /* Collect into dynamic array */
    int cap = ST_INIT_CAP_8;
    int n = 0;
    cbm_project_t *arr = malloc(cap * sizeof(cbm_project_t));

    int scan_rc1;
    while ((scan_rc1 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            arr = safe_realloc(arr, cap * sizeof(cbm_project_t));
        }
        arr[n].name = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
        arr[n].indexed_at = heap_strdup((const char *)sqlite3_column_text(stmt, SKIP_ONE));
        arr[n].root_path = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_2));
        n++;
    }
    if (scan_rc1 != SQLITE_DONE) { /* SCANCHK:1:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        cbm_store_free_projects(arr, n);
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }

    *out = arr;
    *count = n;
    return CBM_STORE_OK;
}

int cbm_store_delete_project(cbm_store_t *s, const char *name) {
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_delete_project, "DELETE FROM projects WHERE name = ?1;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, name);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        store_set_error_sqlite(s, "delete_project");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

/* ── Node CRUD ──────────────────────────────────────────────────── */

int64_t cbm_store_upsert_node(cbm_store_t *s, const cbm_node_t *n) {
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_upsert_node,
                       "INSERT INTO nodes (project, label, name, qualified_name, file_path, "
                       "start_line, end_line, properties) "
                       "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
                       "ON CONFLICT(project, qualified_name) DO UPDATE SET "
                       "label=?2, name=?3, file_path=?5, start_line=?6, end_line=?7, properties=?8 "
                       "RETURNING id;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, safe_str(n->project));
    bind_text(stmt, ST_COL_2, safe_str(n->label));
    bind_text(stmt, ST_COL_3, safe_str(n->name));
    bind_text(stmt, ST_COL_4, safe_str(n->qualified_name));
    bind_text(stmt, ST_COL_5, safe_str(n->file_path));
    sqlite3_bind_int(stmt, ST_COL_6, n->start_line);
    sqlite3_bind_int(stmt, ST_COL_7, n->end_line);
    bind_text(stmt, ST_COL_8, safe_props(n->properties_json));

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        sqlite3_reset(stmt); /* unblock COMMIT — RETURNING leaves stmt active */
        return id;
    }
    sqlite3_reset(stmt);
    store_set_error_sqlite(s, "upsert_node");
    return CBM_STORE_ERR;
}

/* Scan a node from current row of stmt. Heap-allocates strings. */
static void scan_node(sqlite3_stmt *stmt, cbm_node_t *n) {
    n->id = sqlite3_column_int64(stmt, 0);
    n->project = heap_strdup((const char *)sqlite3_column_text(stmt, SKIP_ONE));
    n->label = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_2));
    n->name = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_3));
    n->qualified_name = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_4));
    n->file_path = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_5));
    n->start_line = sqlite3_column_int(stmt, CBM_SZ_6);
    n->end_line = sqlite3_column_int(stmt, CBM_SZ_7);
    n->properties_json = heap_strdup((const char *)sqlite3_column_text(stmt, ST_COL_8));
}

int cbm_store_find_node_by_id(cbm_store_t *s, int64_t id, cbm_node_t *out) {
    if (s && s->backend) return s->backend->find_node_by_id(s->backend_ctx, id, out);
    if (s && s->zova_workspace_query && s->zova_repository)
        return cbm_zova_repository_find_node_by_numeric_id(
            s->zova_repository, s->zova_graph_workspace_id, id, out);
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_find_node_by_id,
                       "SELECT id, project, label, name, qualified_name, file_path, "
                       "start_line, end_line, properties FROM nodes WHERE id = ?1;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    sqlite3_bind_int64(stmt, SKIP_ONE, id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        scan_node(stmt, out);
        return CBM_STORE_OK;
    }
    return CBM_STORE_NOT_FOUND;
}

int cbm_store_find_node_by_qn(cbm_store_t *s, const char *project, const char *qn,
                              cbm_node_t *out) {
    if (!s || !s->db) {
        return CBM_STORE_ERR;
    }
    if (s->zova_workspace_query && s->zova_repository)
        return cbm_zova_repository_find_node_by_qn(
            s->zova_repository, s->zova_graph_workspace_id, qn, out);
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_find_node_by_qn,
                       "SELECT id, project, label, name, qualified_name, file_path, "
                       "start_line, end_line, properties FROM nodes "
                       "WHERE project = ?1 AND qualified_name = ?2;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    bind_text(stmt, ST_COL_2, qn);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        scan_node(stmt, out);
        return CBM_STORE_OK;
    }
    return CBM_STORE_NOT_FOUND;
}

int cbm_store_find_node_by_qn_any(cbm_store_t *s, const char *qn, cbm_node_t *out) {
    if (!s || !s->db) {
        return CBM_STORE_ERR;
    }
    if (s->zova_workspace_query && s->zova_repository)
        return cbm_zova_repository_find_node_by_qn(
            s->zova_repository, s->zova_graph_workspace_id, qn, out);
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_find_node_by_qn_any,
                       "SELECT id, project, label, name, qualified_name, file_path, "
                       "start_line, end_line, properties FROM nodes "
                       "WHERE qualified_name = ?1 LIMIT 1;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, qn);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        scan_node(stmt, out);
        return CBM_STORE_OK;
    }
    return CBM_STORE_NOT_FOUND;
}

int cbm_store_find_nodes_by_name_any(cbm_store_t *s, const char *name, cbm_node_t **out,
                                     int *count) {
    if (!s || !s->db) {
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_find_nodes_by_name_any,
                       "SELECT id, project, label, name, qualified_name, file_path, "
                       "start_line, end_line, properties FROM nodes "
                       "WHERE name = ?1;");
    if (!stmt) {
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, name);

    int cap = ST_INIT_CAP_16;
    int n = 0;
    cbm_node_t *arr = malloc(cap * sizeof(cbm_node_t));
    int scan_rc2;
    while ((scan_rc2 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            arr = safe_realloc(arr, cap * sizeof(cbm_node_t));
        }
        scan_node(stmt, &arr[n]);
        n++;
    }
    if (scan_rc2 != SQLITE_DONE) { /* SCANCHK:2:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        cbm_store_free_nodes(arr, n);
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }
    *out = arr;
    *count = n;
    return CBM_STORE_OK;
}

int cbm_store_find_node_ids_by_qns(cbm_store_t *s, const char *project, const char **qns,
                                   int qn_count, int64_t *out_ids) {
    if (!s || !project || !qns || !out_ids || qn_count <= 0) {
        return 0;
    }

    /* Zero out results */
    memset(out_ids, 0, (size_t)qn_count * sizeof(int64_t));

    int found = 0;
    cbm_node_t node = {0};
    for (int i = 0; i < qn_count; i++) {
        if (!qns[i]) {
            continue;
        }
        int rc = cbm_store_find_node_by_qn(s, project, qns[i], &node);
        if (rc == CBM_STORE_OK) {
            out_ids[i] = node.id;
            found++;
            cbm_node_free_fields(&node);
            memset(&node, 0, sizeof(node));
        }
    }
    return found;
}

/* Generic: find multiple nodes by a single-column filter. */
static int find_nodes_generic(cbm_store_t *s, sqlite3_stmt **slot, const char *sql,
                              const char *project, const char *val, cbm_node_t **out, int *count) {
    if (!s || !s->db) {
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *stmt = prepare_cached(s, slot, sql);
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    if (val) {
        bind_text(stmt, ST_COL_2, val);
    }

    int cap = ST_INIT_CAP_16;
    int n = 0;
    cbm_node_t *arr = malloc(cap * sizeof(cbm_node_t));

    int scan_rc3;
    while ((scan_rc3 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            arr = safe_realloc(arr, cap * sizeof(cbm_node_t));
        }
        scan_node(stmt, &arr[n]);
        n++;
    }
    if (scan_rc3 != SQLITE_DONE) { /* SCANCHK:3:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        cbm_store_free_nodes(arr, n);
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }

    *out = arr;
    *count = n;
    return CBM_STORE_OK;
}

static int node_public_order_compare(const void *left_ptr, const void *right_ptr) {
    const cbm_node_t *left = left_ptr;
    const cbm_node_t *right = right_ptr;
    int cmp = strcmp(left->qualified_name ? left->qualified_name : "",
                     right->qualified_name ? right->qualified_name : "");
    if (cmp == 0)
        cmp = strcmp(left->file_path ? left->file_path : "",
                     right->file_path ? right->file_path : "");
    if (cmp == 0)
        cmp = (left->start_line > right->start_line) -
              (left->start_line < right->start_line);
    if (cmp == 0)
        cmp = (left->end_line > right->end_line) -
              (left->end_line < right->end_line);
    if (cmp == 0) cmp = (left->id > right->id) - (left->id < right->id);
    return cmp;
}

int cbm_store_find_nodes_by_name(cbm_store_t *s, const char *project, const char *name,
                                 cbm_node_t **out, int *count) {
    if (s && (s->backend || (s->zova_workspace_query && s->zova_repository))) {
        cbm_search_output_t search = {0};
        cbm_search_params_t params = {
            .project = project, .name_pattern = name, .min_degree = -1, .max_degree = -1,
            .limit = INT_MAX};
        int rc = s->backend
                     ? s->backend->search(s->backend_ctx, &params, &search)
                     : cbm_zova_repository_search(
                           s->zova_repository, s->zova_graph_workspace_id, &params, &search);
        if (rc != CBM_STORE_OK) {
            *out = NULL;
            *count = 0;
            return rc;
        }
        cbm_node_t *nodes = search.count ? calloc((size_t)search.count, sizeof(*nodes)) : NULL;
        if (search.count && !nodes) {
            cbm_store_search_free(&search);
            return CBM_STORE_ERR;
        }
        int matched = 0;
        for (int i = 0; i < search.count; i++) {
            if (!search.results[i].node.name || strcmp(search.results[i].node.name, name) != 0)
                continue;
            nodes[matched++] = search.results[i].node;
            memset(&search.results[i].node, 0, sizeof(search.results[i].node));
        }
        *out = nodes;
        *count = matched;
        cbm_store_search_free(&search);
        return CBM_STORE_OK;
    }
    return find_nodes_generic(s, &s->stmt_find_nodes_by_name,
                              "SELECT id, project, label, name, qualified_name, file_path, "
                              "start_line, end_line, properties FROM nodes "
                              "WHERE project = ?1 AND name = ?2;",
                              project, name, out, count);
}

/* label NULL matches every label. */
int cbm_store_find_nodes_by_label(cbm_store_t *s, const char *project, const char *label,
                                  cbm_node_t **out, int *count) {
    if (s && (s->backend || (s->zova_workspace_query && s->zova_repository))) {
        cbm_search_output_t search = {0};
        cbm_search_params_t params = {
            .project = project, .label = label, .min_degree = -1, .max_degree = -1,
            .limit = INT_MAX};
        int rc = s->backend
                     ? s->backend->search(s->backend_ctx, &params, &search)
                     : cbm_zova_repository_search(s->zova_repository,
                                                  s->zova_graph_workspace_id, &params, &search);
        if (rc != CBM_STORE_OK) {
            *out = NULL;
            *count = 0;
            return rc;
        }
        cbm_node_t *nodes = search.count ? calloc((size_t)search.count, sizeof(*nodes)) : NULL;
        if (search.count && !nodes) {
            cbm_store_search_free(&search);
            return CBM_STORE_ERR;
        }
        for (int i = 0; i < search.count; i++) {
            nodes[i] = search.results[i].node;
            memset(&search.results[i].node, 0, sizeof(search.results[i].node));
        }
        if (search.count > 1) {
            qsort(nodes, (size_t)search.count, sizeof(*nodes), node_public_order_compare);
        }
        *out = nodes;
        *count = search.count;
        cbm_store_search_free(&search);
        return CBM_STORE_OK;
    }
    if (!label) {
        return find_nodes_generic(s, &s->stmt_find_nodes,
                                  "SELECT id, project, label, name, qualified_name, file_path, "
                                  "start_line, end_line, properties FROM nodes "
                                  "WHERE project = ?1 "
                                  "ORDER BY qualified_name,file_path,start_line,end_line,id;",
                                  project, NULL, out, count);
    }
    return find_nodes_generic(s, &s->stmt_find_nodes_by_label,
                              "SELECT id, project, label, name, qualified_name, file_path, "
                              "start_line, end_line, properties FROM nodes "
                              "WHERE project = ?1 AND label = ?2 "
                              "ORDER BY qualified_name,file_path,start_line,end_line,id;",
                              project, label, out, count);
}

int cbm_store_find_nodes(cbm_store_t *s, const char *project, cbm_node_t **out, int *count) {
    return cbm_store_find_nodes_by_label(s, project, NULL, out, count);
}

int cbm_store_find_nodes_by_file(cbm_store_t *s, const char *project, const char *file_path,
                                 cbm_node_t **out, int *count) {
    if (s && s->zova_workspace_query && s->zova_repository) {
        cbm_search_output_t search = {0};
        cbm_search_params_t params = {
            .project = project, .file_pattern = file_path,
            .min_degree = -1, .max_degree = -1, .limit = INT_MAX};
        int rc = cbm_zova_repository_search(
            s->zova_repository, s->zova_graph_workspace_id, &params, &search);
        if (rc != CBM_STORE_OK) {
            *out = NULL;
            *count = 0;
            return rc;
        }
        cbm_node_t *nodes = search.count ? calloc((size_t)search.count, sizeof(*nodes)) : NULL;
        if (search.count && !nodes) {
            cbm_store_search_free(&search);
            return CBM_STORE_ERR;
        }
        int matched = 0;
        for (int i = 0; i < search.count; i++) {
            if (!search.results[i].node.file_path ||
                strcmp(search.results[i].node.file_path, file_path) != 0)
                continue;
            nodes[matched++] = search.results[i].node;
            memset(&search.results[i].node, 0, sizeof(search.results[i].node));
        }
        *out = nodes;
        *count = matched;
        cbm_store_search_free(&search);
        return CBM_STORE_OK;
    }
    return find_nodes_generic(s, &s->stmt_find_nodes_by_file,
                              "SELECT id, project, label, name, qualified_name, file_path, "
                              "start_line, end_line, properties FROM nodes "
                              "WHERE project = ?1 AND file_path = ?2;",
                              project, file_path, out, count);
}

static bool file_outline_is_container(const char *label) {
    static const char *const containers[] = {"Module", "Package", "File", "Folder", "Project"};
    for (size_t i = 0; i < sizeof(containers) / sizeof(containers[0]); i++) {
        if (strcmp(label, containers[i]) == 0) {
            return true;
        }
    }
    return false;
}

static int file_outline_str_cmp(const char *a, const char *b) {
    return strcmp(a ? a : "", b ? b : "");
}

static int file_outline_node_cmp(const void *pa, const void *pb) {
    const cbm_node_t *a = *(const cbm_node_t *const *)pa;
    const cbm_node_t *b = *(const cbm_node_t *const *)pb;
    if (a->start_line != b->start_line) {
        return a->start_line < b->start_line ? -1 : 1;
    }
    if (a->end_line != b->end_line) {
        return a->end_line < b->end_line ? -1 : 1;
    }
    int c = file_outline_str_cmp(a->label, b->label);
    if (c == 0) {
        c = file_outline_str_cmp(a->name, b->name);
    }
    if (c == 0) {
        c = file_outline_str_cmp(a->qualified_name, b->qualified_name);
    }
    if (c == 0 && a->id != b->id) {
        c = a->id < b->id ? -1 : 1;
    }
    return c;
}

int cbm_store_get_file_outline(cbm_store_t *s, const char *project, const char *file_path,
                               const char *const *labels, int label_count, int limit, int offset,
                               cbm_file_outline_row_t **out, int *count, int *total) {
    if (!out || !count || !total) {
        return CBM_STORE_ERR;
    }
    *out = NULL;
    *count = 0;
    *total = 0;
    if (!s || !project || !file_path || !file_path[0] || limit < 1 ||
        limit > CBM_STORE_FILE_OUTLINE_MAX_LIMIT || offset < 0 || label_count < 0 ||
        label_count > CBM_STORE_FILE_OUTLINE_MAX_LABELS || (label_count > 0 && !labels)) {
        return CBM_STORE_ERR;
    }
    for (int i = 0; i < label_count; i++) {
        if (!labels[i] || !labels[i][0]) {
            return CBM_STORE_ERR;
        }
    }

    cbm_node_t *nodes = NULL;
    int node_count = 0;
    int rc = cbm_store_find_nodes_by_file(s, project, file_path, &nodes, &node_count);
    if (rc != CBM_STORE_OK) {
        return rc;
    }
    const cbm_node_t **kept = node_count ? calloc((size_t)node_count, sizeof(*kept)) : NULL;
    if (node_count && !kept) {
        cbm_store_free_nodes(nodes, node_count);
        return CBM_STORE_ERR;
    }
    int kept_count = 0;
    for (int i = 0; i < node_count; i++) {
        const char *label = nodes[i].label ? nodes[i].label : "";
        if (file_outline_is_container(label)) {
            continue;
        }
        bool wanted = label_count == 0;
        for (int j = 0; j < label_count && !wanted; j++) {
            wanted = strcmp(label, labels[j]) == 0;
        }
        if (wanted) {
            kept[kept_count++] = &nodes[i];
        }
    }
    if (kept_count > 1) {
        qsort(kept, (size_t)kept_count, sizeof(*kept), file_outline_node_cmp);
    }

    int first = offset < kept_count ? offset : kept_count;
    int page = kept_count - first < limit ? kept_count - first : limit;
    size_t text_bytes = 0;
    for (int i = first; i < first + page; i++) {
        text_bytes += strlen(kept[i]->name ? kept[i]->name : "") +
                      strlen(kept[i]->label ? kept[i]->label : "") +
                      strlen(kept[i]->qualified_name ? kept[i]->qualified_name : "");
    }
    if (text_bytes > CBM_STORE_FILE_OUTLINE_MAX_TEXT_BYTES) {
        free(kept);
        cbm_store_free_nodes(nodes, node_count);
        return CBM_STORE_SCAN_LIMIT;
    }

    cbm_file_outline_row_t *rows = page ? calloc((size_t)page, sizeof(*rows)) : NULL;
    rc = page && !rows ? CBM_STORE_ERR : CBM_STORE_OK;
    for (int i = 0; rc == CBM_STORE_OK && i < page; i++) {
        const cbm_node_t *n = kept[first + i];
        rows[i].name = heap_strdup(n->name ? n->name : "");
        rows[i].label = heap_strdup(n->label ? n->label : "");
        rows[i].qualified_name = heap_strdup(n->qualified_name ? n->qualified_name : "");
        rows[i].start_line = n->start_line;
        rows[i].end_line = n->end_line;
        if (!rows[i].name || !rows[i].label || !rows[i].qualified_name) {
            cbm_store_free_file_outline(rows, i + 1);
            rows = NULL;
            rc = CBM_STORE_ERR;
        }
    }
    free(kept);
    cbm_store_free_nodes(nodes, node_count);
    if (rc != CBM_STORE_OK) {
        return rc;
    }
    *out = rows;
    *count = page;
    *total = kept_count;
    return CBM_STORE_OK;
}

void cbm_store_free_file_outline(cbm_file_outline_row_t *rows, int count) {
    if (!rows) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free((void *)rows[i].name);
        free((void *)rows[i].label);
        free((void *)rows[i].qualified_name);
    }
    free(rows);
}

/* ── Graph comparison ──────────────────────────────────────────── */

typedef struct {
    const char *key_qn; /* qualified_name without the leading "<project>." */
    const cbm_zova_identity_node_t *row;
} graph_cmp_node_t;

typedef struct {
    const graph_cmp_node_t *source;
    const graph_cmp_node_t *target;
    const cbm_zova_identity_edge_t *row;
} graph_cmp_edge_t;

typedef struct {
    cbm_zova_identity_node_t *rows;
    int row_count;
    cbm_zova_identity_edge_t *edge_rows;
    int edge_row_count;
    graph_cmp_node_t *nodes; /* identity order */
    graph_cmp_node_t *by_key; /* node-key order, for edge endpoints */
    graph_cmp_edge_t *edges;  /* identity order */
} graph_cmp_side_t;

static const char *graph_cmp_strip_project(const char *qn, const char *project) {
    size_t n = strlen(project);
    if (strncmp(qn, project, n) == 0) {
        if (qn[n] == '.') {
            return qn + n + 1;
        }
        if (qn[n] == '\0') {
            return qn + n;
        }
    }
    return qn;
}

static int graph_cmp_node_identity(const graph_cmp_node_t *a, const graph_cmp_node_t *b) {
    int cmp = strcmp(a->key_qn, b->key_qn);
    if (cmp == 0) {
        cmp = strcmp(a->row->label, b->row->label);
    }
    if (cmp == 0) {
        cmp = strcmp(a->row->file_path, b->row->file_path);
    }
    return cmp;
}

static int graph_cmp_node_qsort(const void *a, const void *b) {
    return graph_cmp_node_identity(a, b);
}

static int graph_cmp_edge_identity(const graph_cmp_edge_t *a, const graph_cmp_edge_t *b) {
    int cmp = graph_cmp_node_identity(a->source, b->source);
    if (cmp == 0) {
        cmp = graph_cmp_node_identity(a->target, b->target);
    }
    if (cmp == 0) {
        cmp = strcmp(a->row->type, b->row->type);
    }
    if (cmp == 0) {
        cmp = strcmp(a->row->local_name, b->row->local_name);
    }
    return cmp;
}

static int graph_cmp_edge_qsort(const void *a, const void *b) {
    return graph_cmp_edge_identity(a, b);
}

static int graph_cmp_key_bsearch(const void *key, const void *elem) {
    int64_t k = *(const int64_t *)key;
    int64_t e = ((const graph_cmp_node_t *)elem)->row->node_key;
    return (k > e) - (k < e);
}

static void graph_cmp_side_free(graph_cmp_side_t *side) {
    cbm_zova_identities_free(side->rows, side->row_count, side->edge_rows, side->edge_row_count);
    free(side->nodes);
    free(side->by_key);
    free(side->edges);
    memset(side, 0, sizeof(*side));
}

/* Load one side and order its nodes and edges by identity. */
static int graph_cmp_side_load(cbm_store_t *s, const char *project, uint64_t max_nodes,
                               uint64_t max_edges, graph_cmp_side_t *side,
                               cbm_graph_compare_project_t *meta) {
    if (!s || !s->zova_workspace_query || !s->zova_repository || !project) {
        return CBM_STORE_ERR;
    }
    char *indexed_at = NULL;
    if (cbm_zova_repository_index_status(s->zova_repository, s->zova_graph_workspace_id,
                                         &meta->generation, &indexed_at) != CBM_STORE_OK) {
        free(indexed_at);
        return CBM_STORE_ERR;
    }
    snprintf(meta->indexed_at, sizeof(meta->indexed_at), "%s", indexed_at ? indexed_at : "");
    free(indexed_at);
    int rc = cbm_zova_repository_graph_identities(
        s->zova_repository, s->zova_graph_workspace_id, max_nodes, max_edges, &side->rows,
        &side->row_count, &side->edge_rows, &side->edge_row_count);
    if (rc != CBM_STORE_OK) {
        return rc;
    }
    meta->node_count = side->row_count;
    meta->edge_count = side->edge_row_count;
    size_t nn = (size_t)side->row_count;
    size_t ne = (size_t)side->edge_row_count;
    side->nodes = nn ? malloc(nn * sizeof(*side->nodes)) : NULL;
    side->by_key = nn ? malloc(nn * sizeof(*side->by_key)) : NULL;
    side->edges = ne ? malloc(ne * sizeof(*side->edges)) : NULL;
    if ((nn && (!side->nodes || !side->by_key)) || (ne && !side->edges)) {
        return CBM_STORE_ERR;
    }
    for (size_t i = 0; i < nn; i++) {
        for (char *p = side->rows[i].file_path; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            }
        }
        side->by_key[i] = (graph_cmp_node_t){
            graph_cmp_strip_project(side->rows[i].qualified_name, project), &side->rows[i]};
    }
    /* Rows arrive in node-key order, so by_key is already sorted. */
    memcpy(side->nodes, side->by_key, nn * sizeof(*side->nodes));
    qsort(side->nodes, nn, sizeof(*side->nodes), graph_cmp_node_qsort);
    for (size_t i = 0; i < ne; i++) {
        const cbm_zova_identity_edge_t *row = &side->edge_rows[i];
        const graph_cmp_node_t *source =
            bsearch(&row->source_key, side->by_key, nn, sizeof(*side->by_key),
                    graph_cmp_key_bsearch);
        const graph_cmp_node_t *target =
            bsearch(&row->target_key, side->by_key, nn, sizeof(*side->by_key),
                    graph_cmp_key_bsearch);
        if (!source || !target) {
            return CBM_STORE_ERR; /* an edge endpoint outside the generation */
        }
        side->edges[i] = (graph_cmp_edge_t){source, target, row};
    }
    qsort(side->edges, ne, sizeof(*side->edges), graph_cmp_edge_qsort);
    return CBM_STORE_OK;
}

static cbm_graph_node_identity_t graph_cmp_node_out(const graph_cmp_node_t *node) {
    return (cbm_graph_node_identity_t){node->row->qualified_name, node->row->label,
                                       node->row->file_path};
}

static bool graph_cmp_emit_node(cbm_graph_compare_node_fn fn, void *context, bool added,
                                const graph_cmp_node_t *node, uint64_t *total) {
    (*total)++;
    cbm_graph_node_identity_t id = graph_cmp_node_out(node);
    return !fn || fn(context, added, &id);
}

static bool graph_cmp_emit_edge(cbm_graph_compare_edge_fn fn, void *context, bool added,
                                const graph_cmp_edge_t *edge, uint64_t *total) {
    (*total)++;
    cbm_graph_edge_identity_t id = {graph_cmp_node_out(edge->source),
                                    graph_cmp_node_out(edge->target), edge->row->type,
                                    edge->row->local_name};
    return !fn || fn(context, added, &id);
}

int cbm_store_compare_graphs(cbm_store_t *base_store, const char *base_project,
                             cbm_store_t *target_store, const char *target_project,
                             uint64_t scan_limit, cbm_graph_compare_node_fn on_node,
                             cbm_graph_compare_edge_fn on_edge, void *context,
                             cbm_graph_compare_result_t *out) {
    if (!out) {
        return CBM_STORE_ERR;
    }
    memset(out, 0, sizeof(*out));
    graph_cmp_side_t base = {0};
    graph_cmp_side_t target = {0};
    int rc = graph_cmp_side_load(base_store, base_project, scan_limit, scan_limit, &base,
                                 &out->base);
    if (rc == CBM_STORE_OK) {
        rc = graph_cmp_side_load(target_store, target_project,
                                 scan_limit - (uint64_t)base.row_count,
                                 scan_limit - (uint64_t)base.edge_row_count, &target,
                                 &out->target);
    }
    bool ok = rc == CBM_STORE_OK;
    int i = 0;
    int j = 0;
    while (ok && (i < base.row_count || j < target.row_count)) {
        int cmp = i >= base.row_count     ? 1
                  : j >= target.row_count ? -1
                                          : graph_cmp_node_identity(&base.nodes[i],
                                                                    &target.nodes[j]);
        if (cmp < 0) {
            ok = graph_cmp_emit_node(on_node, context, false, &base.nodes[i++],
                                     &out->nodes_removed_total);
        } else if (cmp > 0) {
            ok = graph_cmp_emit_node(on_node, context, true, &target.nodes[j++],
                                     &out->nodes_added_total);
        } else {
            i++;
            j++;
        }
    }
    i = 0;
    j = 0;
    while (ok && (i < base.edge_row_count || j < target.edge_row_count)) {
        int cmp = i >= base.edge_row_count     ? 1
                  : j >= target.edge_row_count ? -1
                                               : graph_cmp_edge_identity(&base.edges[i],
                                                                         &target.edges[j]);
        if (cmp < 0) {
            ok = graph_cmp_emit_edge(on_edge, context, false, &base.edges[i++],
                                     &out->edges_removed_total);
        } else if (cmp > 0) {
            ok = graph_cmp_emit_edge(on_edge, context, true, &target.edges[j++],
                                     &out->edges_added_total);
        } else {
            i++;
            j++;
        }
    }
    if (rc == CBM_STORE_OK && !ok) {
        rc = CBM_STORE_ERR;
    }
    graph_cmp_side_free(&base);
    graph_cmp_side_free(&target);
    return rc;
}

/* A Zova workspace store has no legacy nodes and edges tables. The schema,
 * count and architecture queries below read nodes through a temp view over the
 * workspace's rows, whose ids are Zova node keys; edges come from the graph
 * through store_zova_edges. The store holds one workspace, so *project is
 * replaced by the name the rows carry. Does nothing for other stores. */
static int store_legacy_scope(cbm_store_t *s, const char **project) {
    if (!s || !s->db || !s->zova_workspace_query || !s->zova_repository) return CBM_STORE_OK;
    if (!s->zova_legacy_project) {
        cbm_project_t row = {0};
        if (cbm_zova_repository_get_project(s->zova_repository, s->zova_graph_workspace_id,
                                            &row) != CBM_STORE_OK) {
            return CBM_STORE_ERR;
        }
        char *sql = sqlite3_mprintf(
            "CREATE TEMP VIEW nodes AS SELECT n.zova_node_key AS id,%Q AS project,n.label,"
            "n.name,n.qualified_name,f.file_path,n.start_line,n.end_line,n.properties "
            "FROM main.cbm_nodes_v1 n JOIN main.cbm_files_v1 f ON f.file_key=n.file_key "
            "WHERE n.workspace_key=(SELECT workspace_key FROM main.cbm_workspace_registry "
            "WHERE workspace_id=%Q)",
            row.name, s->zova_graph_workspace_id);
        int rc = sql ? sqlite3_exec(s->db, sql, NULL, NULL, NULL) : SQLITE_NOMEM;
        sqlite3_free(sql);
        if (rc == SQLITE_OK) s->zova_legacy_project = heap_strdup(row.name);
        cbm_project_free_fields(&row);
        if (!s->zova_legacy_project) {
            store_set_error_sqlite(s, "zova legacy nodes");
            return CBM_STORE_ERR;
        }
    }
    *project = s->zova_legacy_project;
    return CBM_STORE_OK;
}

static bool store_is_zova_workspace(const cbm_store_t *s) {
    return s && s->zova_workspace_query && s->zova_repository;
}

typedef struct {
    cbm_store_t *store;
    int capacity;
} store_zova_edge_load_t;

static int store_zova_edge_type_find(const cbm_store_t *s, const char *type, size_t type_len) {
    for (int i = 0; i < s->zova_edge_type_count; i++) {
        if (strlen(s->zova_edge_types[i]) == type_len &&
            memcmp(s->zova_edge_types[i], type, type_len) == 0) {
            return i;
        }
    }
    return CBM_NOT_FOUND;
}

static int store_zova_edge_load(int64_t source_key, int64_t target_key, const char *type,
                                size_t type_len, const uint8_t *properties, size_t properties_len,
                                void *context_ptr) {
    (void)properties;
    (void)properties_len;
    store_zova_edge_load_t *context = context_ptr;
    cbm_store_t *s = context->store;
    int t = store_zova_edge_type_find(s, type, type_len);
    if (t < 0) {
        char **types = realloc(s->zova_edge_types,
                               (size_t)(s->zova_edge_type_count + SKIP_ONE) * sizeof(*types));
        char *name = types ? malloc(type_len + SKIP_ONE) : NULL;
        if (types) {
            s->zova_edge_types = types;
        }
        if (!name) {
            return CBM_NOT_FOUND;
        }
        memcpy(name, type, type_len);
        name[type_len] = '\0';
        t = s->zova_edge_type_count;
        s->zova_edge_types[s->zova_edge_type_count++] = name;
    }
    if (s->zova_edge_count == context->capacity) {
        int capacity = context->capacity ? context->capacity * ST_GROWTH : CBM_SZ_1K;
        store_zova_edge_t *edges = realloc(s->zova_edges, (size_t)capacity * sizeof(*edges));
        if (!edges) {
            return CBM_NOT_FOUND;
        }
        s->zova_edges = edges;
        context->capacity = capacity;
    }
    s->zova_edges[s->zova_edge_count++] =
        (store_zova_edge_t){.source_key = source_key, .target_key = target_key, .type = t};
    return 0;
}

/* The workspace's cbm edges, read once per store from the Zova graph in scan
 * order. Schema, count and architecture reads aggregate this list instead of
 * querying an edges table. */
static int store_zova_edges(cbm_store_t *s) {
    if (s->zova_edges_loaded) {
        return CBM_STORE_OK;
    }
    store_zova_edge_load_t context = {.store = s};
    if (cbm_zova_repository_visit_edges(s->zova_repository, s->zova_graph_workspace_id, NULL,
                                        store_zova_edge_load, &context) != CBM_STORE_OK) {
        free(s->zova_edges);
        s->zova_edges = NULL;
        s->zova_edge_count = 0;
        store_set_error(s, "zova edges");
        return CBM_STORE_ERR;
    }
    s->zova_edges_loaded = true;
    return CBM_STORE_OK;
}

static int store_id_compare(const void *left_ptr, const void *right_ptr) {
    int64_t left = *(const int64_t *)left_ptr;
    int64_t right = *(const int64_t *)right_ptr;
    return (left > right) - (left < right);
}

static bool store_id_in(const int64_t *ids, int count, int64_t id) {
    return count > 0 && bsearch(&id, ids, (size_t)count, sizeof(*ids), store_id_compare);
}

int cbm_store_count_nodes(cbm_store_t *s, const char *project) {
    if (!s || !s->db) {
        return 0;
    }
    if (store_legacy_scope(s, &project) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_count_nodes, "SELECT COUNT(*) FROM nodes WHERE project = ?1;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return sqlite3_column_int(stmt, 0);
    }
    return 0;
}

int cbm_store_delete_nodes_by_project(cbm_store_t *s, const char *project) {
    sqlite3_stmt *stmt = prepare_cached(s, &s->stmt_delete_nodes_by_project,
                                        "DELETE FROM nodes WHERE project = ?1;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        store_set_error_sqlite(s, "delete_nodes_by_project");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

int cbm_store_delete_nodes_by_file(cbm_store_t *s, const char *project, const char *file_path) {
    sqlite3_stmt *stmt = prepare_cached(s, &s->stmt_delete_nodes_by_file,
                                        "DELETE FROM nodes WHERE project = ?1 AND file_path = ?2;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    bind_text(stmt, ST_COL_2, file_path);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        store_set_error_sqlite(s, "delete_nodes_by_file");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

int cbm_store_delete_nodes_by_label(cbm_store_t *s, const char *project, const char *label) {
    sqlite3_stmt *stmt = prepare_cached(s, &s->stmt_delete_nodes_by_label,
                                        "DELETE FROM nodes WHERE project = ?1 AND label = ?2;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    bind_text(stmt, ST_COL_2, label);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        store_set_error_sqlite(s, "delete_nodes_by_label");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

/* ── Node batch ─────────────────────────────────────────────────── */

int cbm_store_upsert_node_batch(cbm_store_t *s, const cbm_node_t *nodes, int count,
                                int64_t *out_ids) {
    if (count == 0) {
        return CBM_STORE_OK;
    }

    exec_sql(s, "BEGIN IMMEDIATE;");
    for (int i = 0; i < count; i++) {
        int64_t id = cbm_store_upsert_node(s, &nodes[i]);
        if (id == CBM_STORE_ERR) {
            exec_sql(s, "ROLLBACK;");
            return CBM_STORE_ERR;
        }
        if (out_ids) {
            out_ids[i] = id;
        }
    }
    exec_sql(s, "COMMIT;");
    return CBM_STORE_OK;
}

/* ── Edge CRUD ──────────────────────────────────────────────────── */

int64_t cbm_store_insert_edge(cbm_store_t *s, const cbm_edge_t *e) {
    /* Conflict target includes local_name_gen (#768) so IMPORTS edges with
     * different local_name coexist while re-inserting the same import still
     * upserts. Must match the table's UNIQUE constraint in init_schema. */
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_insert_edge,
                       "INSERT INTO edges (project, source_id, target_id, type, properties) "
                       "VALUES (?1, ?2, ?3, ?4, ?5) "
                       "ON CONFLICT(source_id, target_id, type, local_name_gen) DO UPDATE SET "
                       "properties = json_patch(properties, ?5) "
                       "RETURNING id;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, safe_str(e->project));
    sqlite3_bind_int64(stmt, PAIR_LEN, e->source_id);
    sqlite3_bind_int64(stmt, CBM_SZ_3, e->target_id);
    bind_text(stmt, ST_COL_4, safe_str(e->type));
    bind_text(stmt, ST_COL_5, safe_props(e->properties_json));

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        sqlite3_reset(stmt); /* unblock COMMIT — RETURNING leaves stmt active */
        return id;
    }
    sqlite3_reset(stmt);
    store_set_error_sqlite(s, "insert_edge");
    return CBM_STORE_ERR;
}

/* Scan an edge from current row of stmt. */
static void scan_edge(sqlite3_stmt *stmt, cbm_edge_t *e) {
    e->id = sqlite3_column_int64(stmt, 0);
    e->project = heap_strdup((const char *)sqlite3_column_text(stmt, SKIP_ONE));
    e->source_id = sqlite3_column_int64(stmt, CBM_SZ_2);
    e->target_id = sqlite3_column_int64(stmt, CBM_SZ_3);
    e->type = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_4));
    e->properties_json = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_5));
}

/* Generic: find multiple edges by a filter. */
static int find_edges_generic(cbm_store_t *s, sqlite3_stmt **slot, const char *sql,
                              void (*bind_fn)(sqlite3_stmt *, const void *), const void *bind_data,
                              cbm_edge_t **out, int *count) {
    if (!s || !s->db) {
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *stmt = prepare_cached(s, slot, sql);
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_fn(stmt, bind_data);

    int cap = ST_INIT_CAP_16;
    int n = 0;
    cbm_edge_t *arr = malloc(cap * sizeof(cbm_edge_t));

    int scan_rc4;
    while ((scan_rc4 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            arr = safe_realloc(arr, cap * sizeof(cbm_edge_t));
        }
        scan_edge(stmt, &arr[n]);
        n++;
    }
    if (scan_rc4 != SQLITE_DONE) { /* SCANCHK:4:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        cbm_store_free_edges(arr, n);
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }

    *out = arr;
    *count = n;
    return CBM_STORE_OK;
}

/* Bind helpers for edge queries */
typedef struct {
    int64_t id;
} bind_id_t;
typedef struct {
    int64_t id;
    const char *type;
} bind_id_type_t;
typedef struct {
    const char *project;
    const char *type;
} bind_proj_type_t;

static void bind_source_id(sqlite3_stmt *stmt, const void *data) {
    const bind_id_t *b = data;
    sqlite3_bind_int64(stmt, SKIP_ONE, b->id);
}

static void bind_id_and_type(sqlite3_stmt *stmt, const void *data) {
    const bind_id_type_t *b = data;
    sqlite3_bind_int64(stmt, SKIP_ONE, b->id);
    bind_text(stmt, ST_COL_2, b->type);
}

static void bind_proj_and_type(sqlite3_stmt *stmt, const void *data) {
    const bind_proj_type_t *b = data;
    bind_text(stmt, SKIP_ONE, b->project);
    bind_text(stmt, ST_COL_2, b->type);
}

static int store_zova_workspace_edges(cbm_store_t *s, int64_t node_numeric_id,
                                      const char *direction, const char *type,
                                      cbm_edge_t **out, int *count) {
    if (!s || !s->zova_workspace_query || !s->zova_repository || !out || !count)
        return CBM_STORE_NOT_FOUND;
    *out = NULL; *count = 0;
    char stable[CBM_ZOVA_WORKSPACE_ID_MAX+96]={0};
    int rc=cbm_zova_repository_stable_id_for_numeric_id(
        s->zova_repository,s->zova_graph_workspace_id,node_numeric_id,stable,sizeof(stable));
    if(rc!=CBM_STORE_OK)return rc;
    cbm_edge_t *edges=NULL;int edge_count=0;
    rc=cbm_zova_repository_find_edges(s->zova_repository,s->zova_graph_workspace_id,
                                      stable,direction,&edges,&edge_count);
    if(rc!=CBM_STORE_OK)return rc;
    if(type){
        int kept=0;
        for(int i=0;i<edge_count;i++){
            if(edges[i].type&&strcmp(edges[i].type,type)==0){
                if(kept!=i){edges[kept]=edges[i];memset(&edges[i],0,sizeof(edges[i]));}
                kept++;
            }else{
                safe_str_free(&edges[i].project);
                safe_str_free(&edges[i].type);
                safe_str_free(&edges[i].properties_json);
            }
        }
        edge_count=kept;
    }
    *out=edges;*count=edge_count;s->zova_native_topology_ops++;return CBM_STORE_OK;
}

int cbm_store_find_edges_by_source(cbm_store_t *s, int64_t source_id, cbm_edge_t **out,
                                   int *count) {
    if (s && s->backend)
        return s->backend->find_edges(s->backend_ctx, source_id, "outbound", NULL, out, count);
    if (s && s->zova_workspace_query)
        return store_zova_workspace_edges(s,source_id,"outbound",NULL,out,count);
    bind_id_t b = {source_id};
    return find_edges_generic(s, &s->stmt_find_edges_by_source,
                              "SELECT id, project, source_id, target_id, type, properties "
                              "FROM edges WHERE source_id = ?1;",
                              bind_source_id, &b, out, count);
}

int cbm_store_find_edges_by_target(cbm_store_t *s, int64_t target_id, cbm_edge_t **out,
                                   int *count) {
    if (s && s->backend)
        return s->backend->find_edges(s->backend_ctx, target_id, "inbound", NULL, out, count);
    if (s && s->zova_workspace_query)
        return store_zova_workspace_edges(s,target_id,"inbound",NULL,out,count);
    bind_id_t b = {target_id};
    return find_edges_generic(s, &s->stmt_find_edges_by_target,
                              "SELECT id, project, source_id, target_id, type, properties "
                              "FROM edges WHERE target_id = ?1;",
                              bind_source_id, &b, out, count);
}

int cbm_store_find_edges_by_source_type(cbm_store_t *s, int64_t source_id, const char *type,
                                        cbm_edge_t **out, int *count) {
    if (s && s->backend)
        return s->backend->find_edges(s->backend_ctx, source_id, "outbound", type, out, count);
    if (s && s->zova_workspace_query)
        return store_zova_workspace_edges(s,source_id,"outbound",type,out,count);
    bind_id_type_t b = {source_id, type};
    return find_edges_generic(s, &s->stmt_find_edges_by_source_type,
                              "SELECT id, project, source_id, target_id, type, properties "
                              "FROM edges WHERE source_id = ?1 AND type = ?2;",
                              bind_id_and_type, &b, out, count);
}

int cbm_store_find_edges_by_target_type(cbm_store_t *s, int64_t target_id, const char *type,
                                        cbm_edge_t **out, int *count) {
    if (s && s->backend)
        return s->backend->find_edges(s->backend_ctx, target_id, "inbound", type, out, count);
    if (s && s->zova_workspace_query)
        return store_zova_workspace_edges(s,target_id,"inbound",type,out,count);
    bind_id_type_t b = {target_id, type};
    return find_edges_generic(s, &s->stmt_find_edges_by_target_type,
                              "SELECT id, project, source_id, target_id, type, properties "
                              "FROM edges WHERE target_id = ?1 AND type = ?2;",
                              bind_id_and_type, &b, out, count);
}

int cbm_store_find_edges_by_type(cbm_store_t *s, const char *project, const char *type,
                                 cbm_edge_t **out, int *count) {
    if (store_is_zova_workspace(s)) {
        return cbm_zova_repository_find_edges_by_type(s->zova_repository,
                                                      s->zova_graph_workspace_id, type, out, count);
    }
    bind_proj_type_t b = {project, type};
    return find_edges_generic(s, &s->stmt_find_edges_by_type,
                              "SELECT id, project, source_id, target_id, type, properties "
                              "FROM edges WHERE project = ?1 AND type = ?2;",
                              bind_proj_and_type, &b, out, count);
}

int cbm_store_count_edges(cbm_store_t *s, const char *project) {
    if (!s || !s->db) {
        return 0;
    }
    if (s->zova_workspace_query && s->zova_repository) {
        int nodes = 0;
        int edges = 0;
        return cbm_zova_repository_counts(s->zova_repository, s->zova_graph_workspace_id, &nodes,
                                          &edges) == CBM_STORE_OK
                   ? edges
                   : CBM_STORE_ERR;
    }
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_count_edges, "SELECT COUNT(*) FROM edges WHERE project = ?1;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return sqlite3_column_int(stmt, 0);
    }
    return 0;
}

int cbm_store_count_edges_by_type(cbm_store_t *s, const char *project, const char *type) {
    if (store_is_zova_workspace(s)) {
        if (store_zova_edges(s) != CBM_STORE_OK) {
            return CBM_STORE_ERR;
        }
        int t = type ? store_zova_edge_type_find(s, type, strlen(type)) : CBM_NOT_FOUND;
        int n = 0;
        for (int i = 0; t >= 0 && i < s->zova_edge_count; i++) {
            n += s->zova_edges[i].type == t;
        }
        return n;
    }
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_count_edges_by_type,
                       "SELECT COUNT(*) FROM edges WHERE project = ?1 AND type = ?2;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    bind_text(stmt, ST_COL_2, type);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return sqlite3_column_int(stmt, 0);
    }
    return 0;
}

int cbm_store_delete_edges_by_project(cbm_store_t *s, const char *project) {
    sqlite3_stmt *stmt = prepare_cached(s, &s->stmt_delete_edges_by_project,
                                        "DELETE FROM edges WHERE project = ?1;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        store_set_error_sqlite(s, "delete_edges_by_project");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

int cbm_store_delete_edges_by_type(cbm_store_t *s, const char *project, const char *type) {
    sqlite3_stmt *stmt = prepare_cached(s, &s->stmt_delete_edges_by_type,
                                        "DELETE FROM edges WHERE project = ?1 AND type = ?2;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    bind_text(stmt, ST_COL_2, type);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        store_set_error_sqlite(s, "delete_edges_by_type");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

/* ── Edge batch ─────────────────────────────────────────────────── */

int cbm_store_insert_edge_batch(cbm_store_t *s, const cbm_edge_t *edges, int count) {
    if (count == 0) {
        return CBM_STORE_OK;
    }

    exec_sql(s, "BEGIN IMMEDIATE;");
    for (int i = 0; i < count; i++) {
        int64_t id = cbm_store_insert_edge(s, &edges[i]);
        if (id == CBM_STORE_ERR) {
            exec_sql(s, "ROLLBACK;");
            return CBM_STORE_ERR;
        }
    }
    exec_sql(s, "COMMIT;");
    return CBM_STORE_OK;
}

/* ── File hash CRUD ─────────────────────────────────────────────── */

int cbm_store_upsert_file_hash(cbm_store_t *s, const char *project, const char *rel_path,
                               const char *sha256, int64_t mtime_ns, int64_t size) {
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_upsert_file_hash,
                       "INSERT INTO file_hashes (project, rel_path, sha256, mtime_ns, size) "
                       "VALUES (?1, ?2, ?3, ?4, ?5) "
                       "ON CONFLICT(project, rel_path) DO UPDATE SET "
                       "sha256=?3, mtime_ns=?4, size=?5;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    bind_text(stmt, ST_COL_2, rel_path);
    bind_text(stmt, ST_COL_3, sha256);
    sqlite3_bind_int64(stmt, CBM_SZ_4, mtime_ns);
    sqlite3_bind_int64(stmt, CBM_SZ_5, size);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        store_set_error_sqlite(s, "upsert_file_hash");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

int cbm_store_get_file_hashes(cbm_store_t *s, const char *project, cbm_file_hash_t **out,
                              int *count) {
    sqlite3_stmt *stmt = prepare_cached(s, &s->stmt_get_file_hashes,
                                        "SELECT project, rel_path, sha256, mtime_ns, size "
                                        "FROM file_hashes WHERE project = ?1;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);

    int cap = ST_INIT_CAP_16;
    int n = 0;
    cbm_file_hash_t *arr = malloc(cap * sizeof(cbm_file_hash_t));

    int scan_rc5;
    while ((scan_rc5 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            arr = safe_realloc(arr, cap * sizeof(cbm_file_hash_t));
        }
        arr[n].project = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
        arr[n].rel_path = heap_strdup((const char *)sqlite3_column_text(stmt, SKIP_ONE));
        arr[n].sha256 = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_2));
        arr[n].mtime_ns = sqlite3_column_int64(stmt, CBM_SZ_3);
        arr[n].size = sqlite3_column_int64(stmt, CBM_SZ_4);
        n++;
    }
    if (scan_rc5 != SQLITE_DONE) { /* SCANCHK:5:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        cbm_store_free_file_hashes(arr, n);
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }

    *out = arr;
    *count = n;
    return CBM_STORE_OK;
}

int cbm_store_delete_file_hash(cbm_store_t *s, const char *project, const char *rel_path) {
    sqlite3_stmt *stmt =
        prepare_cached(s, &s->stmt_delete_file_hash,
                       "DELETE FROM file_hashes WHERE project = ?1 AND rel_path = ?2;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    bind_text(stmt, ST_COL_2, rel_path);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        store_set_error_sqlite(s, "delete_file_hash");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

int cbm_store_delete_file_hashes(cbm_store_t *s, const char *project) {
    sqlite3_stmt *stmt = prepare_cached(s, &s->stmt_delete_file_hashes,
                                        "DELETE FROM file_hashes WHERE project = ?1;");
    if (!stmt) {
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        store_set_error_sqlite(s, "delete_file_hashes");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

/* ── FindNodesByFileOverlap ─────────────────────────────────────── */

int cbm_store_find_nodes_by_file_overlap(cbm_store_t *s, const char *project, const char *file_path,
                                         int start_line, int end_line, cbm_node_t **out,
                                         int *count) {
    if (s && s->zova_workspace_query && s->zova_repository)
        return cbm_zova_repository_find_nodes_by_file_overlap(
            s->zova_repository, s->zova_graph_workspace_id, file_path,
            start_line, end_line, out, count);
    *out = NULL;
    *count = 0;
    const char *sql = "SELECT id, project, label, name, qualified_name, file_path, "
                      "start_line, end_line, properties FROM nodes "
                      "WHERE project = ?1 AND file_path = ?2 "
                      "AND label NOT IN ('Module', 'Package', 'File', 'Folder') "
                      "AND start_line <= ?4 AND end_line >= ?3 "
                      "ORDER BY start_line";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL);
    if (rc != SQLITE_OK) {
        store_set_error_sqlite(s, "overlap prepare");
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    bind_text(stmt, ST_COL_2, file_path);
    sqlite3_bind_int(stmt, ST_COL_3, start_line);
    sqlite3_bind_int(stmt, ST_COL_4, end_line);

    int cap = ST_INIT_CAP_8;
    int n = 0;
    cbm_node_t *nodes = malloc(cap * sizeof(cbm_node_t));
    int scan_rc7;
    while ((scan_rc7 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            nodes = safe_realloc(nodes, cap * sizeof(cbm_node_t));
        }
        memset(&nodes[n], 0, sizeof(cbm_node_t));
        scan_node(stmt, &nodes[n]);
        n++;
    }
    if (scan_rc7 != SQLITE_DONE) { /* SCANCHK:7:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        cbm_store_free_nodes(nodes, n);
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    *out = nodes;
    *count = n;
    return CBM_STORE_OK;
}

/* ── FindNodesByQNSuffix ───────────────────────────────────────── */

int cbm_store_find_nodes_by_qn_suffix(cbm_store_t *s, const char *project, const char *suffix,
                                      cbm_node_t **out, int *count) {
    *out = NULL;
    *count = 0;
    if (!s || !s->db) {
        return CBM_STORE_ERR;
    }
    if (store_legacy_scope(s, &project) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    /* Match QNs ending with ".suffix" or exactly equal to suffix */
    char like_pattern[CBM_SZ_512];
    snprintf(like_pattern, sizeof(like_pattern), "%%.%s", suffix);

    const char *sql_with_project =
        "SELECT id, project, label, name, qualified_name, file_path, "
        "start_line, end_line, properties FROM nodes "
        "WHERE project = ?1 AND (qualified_name LIKE ?2 OR qualified_name = ?3)";
    const char *sql_any = "SELECT id, project, label, name, qualified_name, file_path, "
                          "start_line, end_line, properties FROM nodes "
                          "WHERE (qualified_name LIKE ?1 OR qualified_name = ?2)";

    sqlite3_stmt *stmt = NULL;
    int rc =
        sqlite3_prepare_v2(s->db, project ? sql_with_project : sql_any, CBM_NOT_FOUND, &stmt, NULL);
    if (rc != SQLITE_OK) {
        store_set_error_sqlite(s, "qn_suffix prepare");
        return CBM_STORE_ERR;
    }

    if (project) {
        bind_text(stmt, SKIP_ONE, project);
        bind_text(stmt, ST_COL_2, like_pattern);
        bind_text(stmt, ST_COL_3, suffix);
    } else {
        bind_text(stmt, SKIP_ONE, like_pattern);
        bind_text(stmt, ST_COL_2, suffix);
    }

    int cap = ST_INIT_CAP_8;
    int n = 0;
    cbm_node_t *nodes = malloc(cap * sizeof(cbm_node_t));
    int scan_rc8;
    while ((scan_rc8 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            nodes = safe_realloc(nodes, cap * sizeof(cbm_node_t));
        }
        memset(&nodes[n], 0, sizeof(cbm_node_t));
        scan_node(stmt, &nodes[n]);
        n++;
    }
    if (scan_rc8 != SQLITE_DONE) { /* SCANCHK:8:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        cbm_store_free_nodes(nodes, n);
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    if (s->zova_workspace_query && s->zova_repository) {
        /* The view's ids are Zova node keys; callers expect repository ids. */
        for (int i = 0; i < n; i++) {
            cbm_node_t resolved = {0};
            if (cbm_zova_repository_find_node_by_qn(s->zova_repository,
                                                    s->zova_graph_workspace_id,
                                                    nodes[i].qualified_name,
                                                    &resolved) != CBM_STORE_OK) {
                cbm_store_free_nodes(nodes, n);
                return CBM_STORE_ERR;
            }
            cbm_node_free_fields(&nodes[i]);
            nodes[i] = resolved;
        }
    }
    *out = nodes;
    *count = n;
    return CBM_STORE_OK;
}

/* ── NodeDegree ────────────────────────────────────────────────── */

void cbm_store_node_degree(cbm_store_t *s, int64_t node_id, int *in_deg, int *out_deg) {
    if (!s) {
        if (in_deg)
            *in_deg = 0;
        if (out_deg)
            *out_deg = 0;
        return;
    }
    *in_deg = 0;
    *out_deg = 0;

    if (s->backend) {
        if (s->backend->node_degree(s->backend_ctx, node_id, in_deg, out_deg) != CBM_STORE_OK) {
            *in_deg = 0;
            *out_deg = 0;
        }
        return;
    }
    if (s->zova_workspace_query) {
        char stable[CBM_ZOVA_WORKSPACE_ID_MAX + 96] = {0};
        char graph[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
        if (!s->zova_graph_session || !s->zova_repository ||
            cbm_zova_repository_stable_id_for_numeric_id(
                s->zova_repository, s->zova_graph_workspace_id, node_id,
                stable, sizeof(stable)) != CBM_STORE_OK) {
            return;
        }
        if (!stable[0] ||
            cbm_zova_workspace_graph_name(s->zova_graph_workspace_id, graph,
                                          sizeof(graph)) != 0) {
            return;
        }
        const char *types[] = {"CALLS"};
        if (cbm_zova_graph_session_degree(s->zova_graph_session,
                                          s->zova_graph_workspace_id, graph, stable,
                                          "inbound", types, 1, in_deg) != 0 ||
            cbm_zova_graph_session_degree(s->zova_graph_session,
                                          s->zova_graph_workspace_id, graph, stable,
                                          "outbound", types, 1, out_deg) != 0) {
            *in_deg = 0;
            *out_deg = 0;
            return;
        }
        s->zova_native_topology_ops += 2;
        return;
    }

    const char *in_sql = "SELECT COUNT(*) FROM edges WHERE target_id = ?1 AND type = 'CALLS'";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, in_sql, CBM_NOT_FOUND, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, SKIP_ONE, node_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            *in_deg = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    const char *out_sql = "SELECT COUNT(*) FROM edges WHERE source_id = ?1 AND type = 'CALLS'";
    if (sqlite3_prepare_v2(s->db, out_sql, CBM_NOT_FOUND, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, SKIP_ONE, node_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            *out_deg = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
}

/* ── List distinct file paths ────────────────────────────────── */

int cbm_store_list_files(cbm_store_t *s, const char *project, char ***out, int *count) {
    *out = NULL;
    *count = 0;
    if (!s || !s->db || !project) {
        return CBM_STORE_ERR;
    }
    if (store_legacy_scope(s, &project) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }

    /* Upstream cc9c2f4c: a node path can name a graph-only identity (a Folder,
     * <python-builtins>, another synthetic source). Indexed stores have one
     * canonical File node per scannable path, so prefer that exact set; stores
     * without File nodes keep the node-path fallback minus Folder directories. */
    const char *sql = "SELECT DISTINCT file_path FROM nodes "
                      "WHERE project = ?1 AND label != 'Folder' "
                      "AND file_path IS NOT NULL AND file_path != '' "
                      "AND (label = 'File' OR NOT EXISTS ("
                      "SELECT 1 FROM nodes WHERE project = ?1 AND label = 'File'))";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, SKIP_ONE, project, CBM_NOT_FOUND, SQLITE_STATIC);

    int cap = CBM_SZ_64;
    int n = 0;
    char **files = malloc(cap * sizeof(char *));
    int scan_rc9;
    while ((scan_rc9 = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *fp = (const char *)sqlite3_column_text(stmt, 0);
        if (!fp) {
            continue;
        }
        if (n >= cap) {
            cap *= ST_GROWTH;
            files = safe_realloc(files, cap * sizeof(char *));
        }
        files[n++] = heap_strdup(fp);
    }
    if (scan_rc9 != SQLITE_DONE) { /* SCANCHK:9:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        for (int fi = 0; fi < n; fi++) {
            free(files[fi]);
        }
        free(files);
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    *out = files;
    *count = n;
    return CBM_STORE_OK;
}

/* ── Node neighbor names ──────────────────────────────────────── */

static int query_neighbor_names(sqlite3 *db, const char *sql, int64_t node_id, int limit,
                                char ***out, int *out_count) {
    *out = NULL;
    *out_count = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        return CBM_NOT_FOUND;
    }
    sqlite3_bind_int64(stmt, SKIP_ONE, node_id);
    sqlite3_bind_int(stmt, ST_COL_2, limit);

    int cap = ST_INIT_CAP_8;
    char **names = malloc((size_t)cap * sizeof(char *));
    int count = 0;
    int scan_rc10;
    while ((scan_rc10 = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        if (!name) {
            continue;
        }
        if (count >= cap) {
            cap *= ST_GROWTH;
            names = safe_realloc(names, (size_t)cap * sizeof(char *));
        }
        names[count++] = strdup(name);
    }
    if (scan_rc10 != SQLITE_DONE) { /* SCANCHK:10:stmt */
        /* No store handle here; neighbor names are include_connected
         * enrichment — fail the call, primary surfaces report loudly. */
        sqlite3_finalize(stmt);
        for (int ni = 0; ni < count; ni++) {
            free(names[ni]);
        }
        free(names);
        *out = NULL;
        *out_count = 0;
        return CBM_NOT_FOUND;
    }
    sqlite3_finalize(stmt);
    *out = names;
    *out_count = count;
    return 0;
}

static int store_name_compare(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

/* Distinct, sorted names of the call neighbors of a Zova node, from the graph's
 * own adjacency index and the workspace nodes view. */
static int store_zova_neighbor_names(cbm_store_t *s, const char *stable_id, bool inbound,
                                     int limit, char ***out, int *out_count) {
    int64_t *keys = NULL;
    int key_count = 0;
    if (cbm_zova_repository_call_neighbor_keys(s->zova_repository, s->zova_graph_workspace_id,
                                               stable_id, inbound, &keys,
                                               &key_count) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *stmt = NULL;
    if (key_count == 0 || sqlite3_prepare_v2(s->db, "SELECT name FROM nodes WHERE id = ?1",
                                             CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        free(keys);
        return key_count == 0 ? CBM_STORE_OK : CBM_STORE_ERR;
    }
    char **names = malloc((size_t)key_count * sizeof(char *));
    int count = 0;
    for (int i = 0; names && i < key_count; i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, SKIP_ONE, keys[i]);
        const char *name =
            sqlite3_step(stmt) == SQLITE_ROW ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
        if (name) {
            names[count++] = heap_strdup(name);
        }
    }
    sqlite3_finalize(stmt);
    free(keys);
    if (!names) {
        return CBM_STORE_ERR;
    }
    qsort(names, (size_t)count, sizeof(char *), store_name_compare);
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (kept < limit && (kept == 0 || strcmp(names[kept - 1], names[i]) != 0)) {
            names[kept++] = names[i];
        } else {
            free(names[i]);
        }
    }
    *out = names;
    *out_count = kept;
    return CBM_STORE_OK;
}

int cbm_store_node_neighbor_names(cbm_store_t *s, int64_t node_id, int limit, char ***out_callers,
                                  int *caller_count, char ***out_callees, int *callee_count) {
    if (!s) {
        return CBM_NOT_FOUND;
    }
    *out_callers = NULL;
    *caller_count = 0;
    *out_callees = NULL;
    *callee_count = 0;

    if (s->zova_workspace_query && s->zova_repository) {
        const char *project = NULL;
        char stable[CBM_ZOVA_WORKSPACE_ID_MAX + 96] = {0};
        if (store_legacy_scope(s, &project) != CBM_STORE_OK ||
            cbm_zova_repository_stable_id_for_numeric_id(s->zova_repository,
                                                         s->zova_graph_workspace_id, node_id,
                                                         stable, sizeof(stable)) != CBM_STORE_OK) {
            return CBM_STORE_ERR;
        }
        if (store_zova_neighbor_names(s, stable, true, limit, out_callers, caller_count) !=
                CBM_STORE_OK ||
            store_zova_neighbor_names(s, stable, false, limit, out_callees, callee_count) !=
                CBM_STORE_OK) {
            return CBM_STORE_ERR;
        }
        return 0;
    }

    query_neighbor_names(
        s->db,
        "SELECT DISTINCT n.name FROM edges e JOIN nodes n ON e.source_id = n.id "
        "WHERE e.target_id = ?1 AND e.type IN ('CALLS','HTTP_CALLS','ASYNC_CALLS') "
        "ORDER BY n.name LIMIT ?2",
        node_id, limit, out_callers, caller_count);

    query_neighbor_names(
        s->db,
        "SELECT DISTINCT n.name FROM edges e JOIN nodes n ON e.target_id = n.id "
        "WHERE e.source_id = ?1 AND e.type IN ('CALLS','HTTP_CALLS','ASYNC_CALLS') "
        "ORDER BY n.name LIMIT ?2",
        node_id, limit, out_callees, callee_count);

    return 0;
}

static int count_degrees_direction(cbm_store_t *s, const int64_t *node_ids, int id_count,
                                   const char *in_clause, bool has_type, const char *edge_type,
                                   bool inbound, int *out_counts) {
    char sql[ST_SQL_BUF];
    const char *id_col = inbound ? "target_id" : "source_id";
    if (has_type) {
        snprintf(sql, sizeof(sql),
                 "SELECT %s, COUNT(*) FROM edges "
                 "WHERE %s IN (%s) AND type = ? GROUP BY %s",
                 id_col, id_col, in_clause, id_col);
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT %s, COUNT(*) FROM edges "
                 "WHERE %s IN (%s) GROUP BY %s",
                 id_col, id_col, in_clause, id_col);
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }

    for (int i = 0; i < id_count; i++) {
        sqlite3_bind_int64(stmt, i + SKIP_ONE, node_ids[i]);
    }
    if (has_type) {
        bind_text(stmt, id_count + SKIP_ONE, edge_type);
    }

    int scan_rc11;
    while ((scan_rc11 = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t nid = sqlite3_column_int64(stmt, 0);
        int cnt = sqlite3_column_int(stmt, SKIP_ONE);
        for (int i = 0; i < id_count; i++) {
            if (node_ids[i] == nid) {
                out_counts[i] = cnt;
                break;
            }
        }
    }
    if (scan_rc11 != SQLITE_DONE) { /* SCANCHK:11:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    return CBM_STORE_OK;
}

int cbm_store_batch_count_degrees(cbm_store_t *s, const int64_t *node_ids, int id_count,
                                  const char *edge_type, int *out_in, int *out_out) {
    if (!s || !node_ids || id_count <= 0 || !out_in || !out_out) {
        return CBM_STORE_ERR;
    }

    memset(out_in, 0, (size_t)id_count * sizeof(int));
    memset(out_out, 0, (size_t)id_count * sizeof(int));

    if (s->zova_workspace_query) {
        for (int i = 0; i < id_count; ++i) {
            if (!edge_type || strcmp(edge_type, "CALLS") == 0) {
                cbm_store_node_degree(s, node_ids[i], &out_in[i], &out_out[i]);
                continue;
            }
            cbm_edge_t *edges = NULL;
            int count = 0;
            if (store_zova_workspace_edges(s, node_ids[i], "inbound", edge_type,
                                           &edges, &count) != CBM_STORE_OK) {
                return CBM_STORE_ERR;
            }
            out_in[i] = count;
            cbm_store_free_edges(edges, count);
            edges = NULL;
            count = 0;
            if (store_zova_workspace_edges(s, node_ids[i], "outbound", edge_type,
                                           &edges, &count) != CBM_STORE_OK) {
                return CBM_STORE_ERR;
            }
            out_out[i] = count;
            cbm_store_free_edges(edges, count);
        }
        return CBM_STORE_OK;
    }

    /* Build IN clause: (?,?,?) */
    char in_clause[CBM_SZ_4K];
    int pos = 0;
    for (int i = 0; i < id_count && pos < (int)sizeof(in_clause) - ST_IN_CLAUSE_MARGIN; i++) {
        if (i > 0) {
            in_clause[pos++] = ',';
        }
        in_clause[pos++] = '?';
    }
    in_clause[pos] = '\0';

    bool has_type = edge_type && edge_type[0] != '\0';

    int rc = count_degrees_direction(s, node_ids, id_count, in_clause, has_type, edge_type, true,
                                     out_in);
    if (rc != CBM_STORE_OK) {
        return rc;
    }

    return count_degrees_direction(s, node_ids, id_count, in_clause, has_type, edge_type, false,
                                   out_out);
}

/* ── UpsertFileHashBatch ───────────────────────────────────────── */

int cbm_store_upsert_file_hash_batch(cbm_store_t *s, const cbm_file_hash_t *hashes, int count) {
    if (count == 0) {
        return CBM_STORE_OK;
    }

    int rc = cbm_store_begin(s);
    if (rc != CBM_STORE_OK) {
        return rc;
    }

    for (int i = 0; i < count; i++) {
        rc = cbm_store_upsert_file_hash(s, hashes[i].project, hashes[i].rel_path, hashes[i].sha256,
                                        hashes[i].mtime_ns, hashes[i].size);
        if (rc != CBM_STORE_OK) {
            cbm_store_rollback(s);
            return rc;
        }
    }

    return cbm_store_commit(s);
}

/* ── FindEdgesByURLPath ────────────────────────────────────────── */

int cbm_store_find_edges_by_url_path(cbm_store_t *s, const char *project, const char *keyword,
                                     cbm_edge_t **out, int *count) {
    *out = NULL;
    *count = 0;

    /* Search properties JSON for url_path containing keyword */
    char like_pattern[CBM_SZ_512];
    snprintf(like_pattern, sizeof(like_pattern), "%%\"url_path\":\"%%%s%%\"%%", keyword);

    const char *sql = "SELECT id, project, source_id, target_id, type, properties FROM edges "
                      "WHERE project = ?1 AND properties LIKE ?2";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL);
    if (rc != SQLITE_OK) {
        store_set_error_sqlite(s, "url_path prepare");
        return CBM_STORE_ERR;
    }

    bind_text(stmt, SKIP_ONE, project);
    bind_text(stmt, ST_COL_2, like_pattern);

    int cap = ST_INIT_CAP_8;
    int n = 0;
    cbm_edge_t *edges = malloc(cap * sizeof(cbm_edge_t));
    int scan_rc12;
    while ((scan_rc12 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            edges = safe_realloc(edges, cap * sizeof(cbm_edge_t));
        }
        memset(&edges[n], 0, sizeof(cbm_edge_t));
        scan_edge(stmt, &edges[n]);
        n++;
    }
    if (scan_rc12 != SQLITE_DONE) { /* SCANCHK:12:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        cbm_store_free_edges(edges, n);
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    *out = edges;
    *count = n;
    return CBM_STORE_OK;
}

/* ── RestoreFrom ───────────────────────────────────────────────── */

int cbm_store_restore_from(cbm_store_t *dst, cbm_store_t *src) {
    if (!dst || !src) {
        return CBM_STORE_ERR;
    }
    sqlite3_backup *bk = sqlite3_backup_init(dst->db, "main", src->db, "main");
    if (!bk) {
        store_set_error_sqlite(dst, "backup init");
        return CBM_STORE_ERR;
    }
    int rc = sqlite3_backup_step(bk, CBM_NOT_FOUND); /* copy all pages */
    sqlite3_backup_finish(bk);

    if (rc != SQLITE_DONE) {
        store_set_error(dst, "backup step failed");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

/* ── Search ─────────────────────────────────────────────────────── */

/* Convert a glob pattern to SQL LIKE pattern. */
char *cbm_glob_to_like(const char *pattern) {
    if (!pattern) {
        return NULL;
    }
    size_t len = strlen(pattern);
    char *out = malloc((len * ST_GROWTH) + SKIP_ONE);
    size_t j = 0;

    for (size_t i = 0; i < len; i++) {
        if (pattern[i] == '*' && i + SKIP_ONE < len && pattern[i + SKIP_ONE] == '*') {
            /* Remove leading / from output if present (handles glob dir-star) */
            if (j > 0 && out[j - SKIP_ONE] == '/') {
                j--;
            }
            out[j++] = '%';
            i++; /* skip second * */
            if (i + SKIP_ONE < len && pattern[i + SKIP_ONE] == '/') {
                i++; /* skip trailing / */
            }
        } else if (pattern[i] == '*') {
            out[j++] = '%';
        } else if (pattern[i] == '?') {
            out[j++] = '_';
        } else {
            out[j++] = pattern[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* ── extractLikeHints ─────────────────────────────────────────── */

int cbm_extract_like_hints(const char *pattern, char **out, int max_out) {
    if (!pattern || !out || max_out <= 0) {
        return 0;
    }

    /* Bail on alternation — can't convert OR regex to AND LIKE */
    for (const char *p = pattern; *p; p++) {
        if (*p == '|') {
            return 0;
        }
    }

    int count = 0;
    char buf[CBM_SZ_256];
    int blen = 0;

    int i = 0;
    while (pattern[i]) {
        char ch = pattern[i];
        switch (ch) {
        case '\\':
            /* Escaped char — the next char is literal */
            if (pattern[i + SKIP_ONE]) {
                if (blen < (int)sizeof(buf) - SKIP_ONE) {
                    buf[blen++] = pattern[i + SKIP_ONE];
                }
                i += ST_GLOB_SKIP;
            } else {
                i++;
            }
            break;
        case '.':
        case '*':
        case '+':
        case '?':
        case '^':
        case '$':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
            /* Meta character — flush current literal segment */
            if (blen >= ST_GLOB_MIN_LEN && count < max_out) {
                buf[blen] = '\0';
                out[count++] = strdup(buf);
            }
            blen = 0;
            i++;
            break;
        default:
            if (blen < (int)sizeof(buf) - SKIP_ONE) {
                buf[blen++] = ch;
            }
            i++;
            break;
        }
    }
    /* Flush trailing segment */
    if (blen >= ST_GLOB_MIN_LEN && count < max_out) {
        buf[blen] = '\0';
        out[count++] = strdup(buf);
    }
    return count;
}

/* ── ensureCaseInsensitive / stripCaseFlag ────────────────────── */

const char *cbm_ensure_case_insensitive(const char *pattern) {
    static char buf[CBM_SZ_2K];
    if (!pattern) {
        buf[0] = '\0';
        return buf;
    }
    /* Already has (?i) prefix? Return as-is. */
    if (strncmp(pattern, "(?i)", SLEN("(?i)")) == 0) {
        snprintf(buf, sizeof(buf), "%s", pattern);
    } else {
        snprintf(buf, sizeof(buf), "(?i)%s", pattern);
    }
    return buf;
}

const char *cbm_strip_case_flag(const char *pattern) {
    static char buf[CBM_SZ_2K];
    if (!pattern) {
        buf[0] = '\0';
        return buf;
    }
    if (strncmp(pattern, "(?i)", SLEN("(?i)")) == 0) {
        snprintf(buf, sizeof(buf), "%s", pattern + 4);
    } else {
        snprintf(buf, sizeof(buf), "%s", pattern);
    }
    return buf;
}

/* Bind value tracker for dynamic query building */
typedef struct {
    const char *text;
} search_bind_t;

/* Pool of malloc'd strings that must outlive statement execution.
 * Freed in one shot after both statements are finalized. */
typedef struct {
    char *ptrs[ST_LIKE_POOL_MAX];
    int count;
} search_like_pool_t;

static void like_pool_add(search_like_pool_t *pool, char *ptr) {
    if (ptr && pool->count < ST_LIKE_POOL_MAX) {
        pool->ptrs[pool->count++] = ptr;
    } else {
        free(ptr); /* pool full — don't leak */
    }
}

static void like_pool_free(search_like_pool_t *pool) {
    for (int i = 0; i < pool->count; i++) {
        free(pool->ptrs[i]);
    }
    pool->count = 0;
}

/* Wrap a literal string in % for use as a LIKE pattern (%literal%). */
static char *make_like_hint(const char *literal) {
    size_t len = strlen(literal);
    char *buf = malloc(len + 3); /* % + literal + % + NUL */
    if (buf) {
        buf[0] = '%';
        memcpy(buf + SKIP_ONE, literal, len);
        buf[len + SKIP_ONE] = '%';
        buf[len + 2] = '\0';
    }
    return buf;
}

static void search_apply_degree_filter(char *sql, size_t sql_sz, const cbm_search_params_t *p) {
    bool has_degree_filter = (p->min_degree >= 0 || p->max_degree >= 0);
    if (!has_degree_filter) {
        return;
    }
    char inner_sql[CBM_SZ_4K];
    snprintf(inner_sql, sizeof(inner_sql), "%s", sql);
    if (p->min_degree >= 0 && p->max_degree >= 0) {
        snprintf(sql, sql_sz,
                 "SELECT * FROM (%s) WHERE (in_deg + out_deg) >= %d AND (in_deg + out_deg) <= %d",
                 inner_sql, p->min_degree, p->max_degree);
    } else if (p->min_degree >= 0) {
        snprintf(sql, sql_sz, "SELECT * FROM (%s) WHERE (in_deg + out_deg) >= %d", inner_sql,
                 p->min_degree);
    } else {
        snprintf(sql, sql_sz, "SELECT * FROM (%s) WHERE (in_deg + out_deg) <= %d", inner_sql,
                 p->max_degree);
    }
}

/* Append a WHERE clause fragment, joining with AND if not the first. */
static int where_append(char *where, int where_sz, int wlen, int *nparams, const char *cond) {
    if (*nparams > 0) {
        wlen += snprintf(where + wlen, where_sz - wlen, " AND ");
    }
    wlen += snprintf(where + wlen, where_sz - wlen, "%s", cond);
    (*nparams)++;
    return wlen;
}

/* Bind a text parameter and increment the bind index. */
static void where_bind_text(search_bind_t *binds, int *bind_idx, const char *val) {
    binds[*bind_idx].text = val;
    (*bind_idx)++;
}

/* Build exclude-labels NOT IN clause with bind placeholders. */
static void search_build_exclude_labels(const char **labels, search_bind_t *binds, int *bind_idx,
                                        char *clause, int clause_sz) {
    int elen = snprintf(clause, clause_sz, "n.label NOT IN (");
    for (int i = 0; labels[i]; i++) {
        if (i > 0) {
            elen += snprintf(clause + elen, clause_sz - elen, ",");
            if (elen >= clause_sz) {
                elen = clause_sz - SKIP_ONE;
            }
        }
        elen += snprintf(clause + elen, clause_sz - elen, "?%d", *bind_idx + SKIP_ONE);
        if (elen >= clause_sz) {
            elen = clause_sz - SKIP_ONE;
        }
        where_bind_text(binds, bind_idx, labels[i]);
    }
    snprintf(clause + elen, clause_sz - elen, ")");
}

/* Append a regex WHERE clause for a column (case-sensitive or insensitive). */
static void where_add_regex(char *where, int where_sz, int *wlen, int *nparams,
                            search_bind_t *binds, int *bind_idx, const char *column,
                            const char *pattern, bool case_sensitive) {
    char buf[CBM_SZ_128];
    if (case_sensitive) {
        snprintf(buf, sizeof(buf), "%s REGEXP ?%d", column, *bind_idx + SKIP_ONE);
    } else {
        snprintf(buf, sizeof(buf), "iregexp(?%d, %s)", *bind_idx + SKIP_ONE, column);
    }
    *wlen = where_append(where, where_sz, *wlen, nparams, buf);
    where_bind_text(binds, bind_idx, pattern);
}

/* Prepend LIKE pre-filter conditions for literal segments of a regex pattern.
 * The idx_nodes_name index satisfies LIKE '%literal%', cutting the rows that
 * reach the (more expensive) iregexp call to only those containing the literal.
 * cbm_extract_like_hints bails on alternation, so no false negatives. */
static void where_add_like_hints(const char *column, const char *pattern, char *where, int where_sz,
                                 int *wlen, int *nparams, search_bind_t *binds, int *bind_idx,
                                 search_like_pool_t *pool) {
    char *hints[ST_LIKE_HINT_MAX];
    int nhints = cbm_extract_like_hints(pattern, hints, ST_LIKE_HINT_MAX);
    char bind_buf[CBM_SZ_64];
    for (int i = 0; i < nhints; i++) {
        char *lp = make_like_hint(hints[i]);
        free(hints[i]);
        if (!lp)
            continue;
        int pool_was_full = (pool->count >= ST_LIKE_POOL_MAX);
        like_pool_add(pool, lp);
        if (pool_was_full)
            continue; /* lp was freed — skip bind */
        snprintf(bind_buf, sizeof(bind_buf), "%s LIKE ?%d", column, *bind_idx + SKIP_ONE);
        *wlen = where_append(where, where_sz, *wlen, nparams, bind_buf);
        where_bind_text(binds, bind_idx, lp);
    }
}

/* Build basic WHERE clauses: project, label, name, file, qn patterns. */
static int search_where_basic(const cbm_search_params_t *params, char *where, int where_sz,
                              int *wlen, int *nparams, search_bind_t *binds, int *bind_idx,
                              search_like_pool_t *pool) {
    char bind_buf[CBM_SZ_64];

    if (params->project) {
        snprintf(bind_buf, sizeof(bind_buf), "n.project = ?%d", *bind_idx + SKIP_ONE);
        *wlen = where_append(where, where_sz, *wlen, nparams, bind_buf);
        where_bind_text(binds, bind_idx, params->project);
    }
    /* Ignore an empty-string label: it is non-NULL but should behave like an
     * omitted label (no filter), matching the BM25 query path. Without the
     * params->label[0] guard, name_pattern/qn_pattern searches that pass
     * label="" append `n.label = ''`, which matches no node and silently
     * returns zero results (issue #481). */
    if (params->label && params->label[0]) {
        snprintf(bind_buf, sizeof(bind_buf), "n.label = ?%d", *bind_idx + SKIP_ONE);
        *wlen = where_append(where, where_sz, *wlen, nparams, bind_buf);
        where_bind_text(binds, bind_idx, params->label);
    }
    if (params->name_pattern) {
        where_add_like_hints("n.name", params->name_pattern, where, where_sz, wlen, nparams, binds,
                             bind_idx, pool);
        where_add_regex(where, where_sz, wlen, nparams, binds, bind_idx, "n.name",
                        params->name_pattern, params->case_sensitive);
    }
    if (params->qn_pattern) {
        where_add_like_hints("n.qualified_name", params->qn_pattern, where, where_sz, wlen, nparams,
                             binds, bind_idx, pool);
        where_add_regex(where, where_sz, wlen, nparams, binds, bind_idx, "n.qualified_name",
                        params->qn_pattern, params->case_sensitive);
    }
    if (params->file_pattern) {
        char *lp = cbm_glob_to_like(params->file_pattern);
        /* A file_pattern with no glob wildcards is treated as a path-substring
         * match (issue #200): file_pattern="offer-server" should match
         * "src/offer-server/x.js", not only a path equal to "offer-server".
         * Patterns that DO contain * / ? keep their explicit glob semantics. */
        if (lp && !strchr(params->file_pattern, '*') && !strchr(params->file_pattern, '?')) {
            size_t lplen = strlen(lp);
            char *contains = malloc(lplen + 3);
            if (contains) {
                contains[0] = '%';
                memcpy(contains + 1, lp, lplen);
                contains[lplen + 1] = '%';
                contains[lplen + 2] = '\0';
                free(lp);
                lp = contains;
            }
        }
        int pool_was_full = (pool->count >= ST_LIKE_POOL_MAX);
        like_pool_add(pool, lp);
        if (!pool_was_full && lp) {
            snprintf(bind_buf, sizeof(bind_buf), "n.file_path LIKE ?%d", *bind_idx + SKIP_ONE);
            *wlen = where_append(where, where_sz, *wlen, nparams, bind_buf);
            where_bind_text(binds, bind_idx, lp);
        }
    }
    return *nparams;
}

/* Build advanced WHERE clauses: relationship, entry points, exclude labels. */
static void search_where_advanced(const cbm_search_params_t *params, char *where, int where_sz,
                                  int *wlen, int *nparams, search_bind_t *binds, int *bind_idx) {
    if (params->relationship) {
        char rel_clause[CBM_SZ_256];
        snprintf(rel_clause, sizeof(rel_clause),
                 "EXISTS(SELECT 1 FROM edges e WHERE "
                 "(e.source_id = n.id OR e.target_id = n.id) AND e.type = ?%d)",
                 *bind_idx + SKIP_ONE);
        *wlen = where_append(where, where_sz, *wlen, nparams, rel_clause);
        where_bind_text(binds, bind_idx, params->relationship);
    }
    if (params->exclude_entry_points) {
        /* Exclude nodes with no inbound CALLS but at least one outbound CALLS.
         * Dead code (degree=0) is NOT excluded — only true entry points. */
        *wlen = where_append(where, where_sz, *wlen, nparams,
                             "NOT (NOT EXISTS(SELECT 1 FROM edges e WHERE e.target_id = n.id "
                             "AND e.type = 'CALLS') "
                             "AND EXISTS(SELECT 1 FROM edges e2 WHERE e2.source_id = n.id "
                             "AND e2.type = 'CALLS'))");
    }
    if (params->exclude_labels) {
        char excl_clause[CBM_SZ_512];
        search_build_exclude_labels(params->exclude_labels, binds, bind_idx, excl_clause,
                                    (int)sizeof(excl_clause));
        (void)where_append(where, where_sz, *wlen, nparams, excl_clause);
    }
}

static int search_build_where(const cbm_search_params_t *params, char *where, int where_sz,
                              search_bind_t *binds, int *bind_idx, search_like_pool_t *pool) {
    int wlen = 0;
    int nparams = 0;

    search_where_basic(params, where, where_sz, &wlen, &nparams, binds, bind_idx, pool);
    search_where_advanced(params, where, where_sz, &wlen, &nparams, binds, bind_idx);

    return nparams;
}

int cbm_store_search(cbm_store_t *s, const cbm_search_params_t *params, cbm_search_output_t *out) {
    memset(out, 0, sizeof(*out));
    if (s && s->backend) {
        return s->backend->search(s->backend_ctx, params, out);
    }
    if (!s || !s->db) {
        return CBM_STORE_ERR;
    }
    if (s->zova_workspace_query && s->zova_repository) {
        int rc = cbm_zova_repository_search(
            s->zova_repository, s->zova_graph_workspace_id, params, out);
        if (rc == CBM_STORE_OK) s->zova_native_topology_ops += 8;
        return rc;
    }

    char sql[CBM_SZ_4K];
    char count_sql[CBM_SZ_4K];
    int bind_idx = 0;

    const char *select_cols = "SELECT n.id, n.project, n.label, n.name, n.qualified_name, "
                              "n.file_path, n.start_line, n.end_line, n.properties, "
                              "(SELECT COUNT(*) FROM edges e WHERE e.target_id = n.id AND "
                              "e.type IN ('CALLS', 'USAGE', 'CALL_REFERENCE', 'INHERITS', 'IMPLEMENTS')) "
                              "AS in_deg, "
                              "(SELECT COUNT(*) FROM edges e WHERE e.source_id = n.id AND "
                              "e.type IN ('CALLS', 'USAGE', 'CALL_REFERENCE', 'INHERITS', 'IMPLEMENTS')) "
                              "AS out_deg ";

    char where[CBM_SZ_2K] = "";
    search_bind_t binds[ST_SEARCH_MAX_BINDS];
    search_like_pool_t like_pool = {0};

    int nparams =
        search_build_where(params, where, (int)sizeof(where), binds, &bind_idx, &like_pool);

    /* Build full SQL */
    if (nparams > 0) {
        snprintf(sql, sizeof(sql), "%s FROM nodes n WHERE %s", select_cols, where);
    } else {
        snprintf(sql, sizeof(sql), "%s FROM nodes n", select_cols);
    }

    /* Degree filters */
    bool has_degree_filter = (params->min_degree >= 0 || params->max_degree >= 0);
    search_apply_degree_filter(sql, sizeof(sql), params);

    /* Count query — stripped of per-row edge subqueries for the common (no-degree-filter)
     * case, since we only need the row count, not in_deg/out_deg.  The degree-filter
     * case must wrap the full query because the filter references those columns. */
    if (has_degree_filter) {
        snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM (%s)", sql);
    } else if (nparams > 0) {
        snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM nodes n WHERE %s", where);
    } else {
        snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM nodes n");
    }

    /* Add ORDER BY + LIMIT */
    int limit = params->limit > 0 ? params->limit : CBM_DEFAULT_SEARCH_LIMIT;
    int offset = params->offset;
    const char *name_col = has_degree_filter ? "name" : "n.name";
    char order_limit[CBM_SZ_128];
    snprintf(order_limit, sizeof(order_limit),
             has_degree_filter
                 ? " ORDER BY %s, qualified_name, id LIMIT %d OFFSET %d"
                 : " ORDER BY %s, n.qualified_name, n.id LIMIT %d OFFSET %d",
             name_col, limit, offset);
    strncat(sql, order_limit, sizeof(sql) - strlen(sql) - 1);

    /* Execute count query */
    sqlite3_stmt *cnt_stmt = NULL;
    int rc = sqlite3_prepare_v2(s->db, count_sql, CBM_NOT_FOUND, &cnt_stmt, NULL);
    if (rc == SQLITE_OK) {
        for (int i = 0; i < bind_idx; i++) {
            bind_text(cnt_stmt, i + SKIP_ONE, binds[i].text);
        }
        if (sqlite3_step(cnt_stmt) == SQLITE_ROW) {
            out->total = sqlite3_column_int(cnt_stmt, 0);
        }
        sqlite3_finalize(cnt_stmt);
    }

    /* Execute main query */
    sqlite3_stmt *main_stmt = NULL;
    rc = sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &main_stmt, NULL);
    if (rc != SQLITE_OK) {
        store_set_error_sqlite(s, "search prepare");
        like_pool_free(&like_pool);
        return CBM_STORE_ERR;
    }

    for (int i = 0; i < bind_idx; i++) {
        bind_text(main_stmt, i + SKIP_ONE, binds[i].text);
    }

    int cap = ST_INIT_CAP_16;
    int n = 0;
    cbm_search_result_t *results = malloc(cap * sizeof(cbm_search_result_t));

    int scan_rc13;
    while ((scan_rc13 = sqlite3_step(main_stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            results = safe_realloc(results, cap * sizeof(cbm_search_result_t));
        }
        memset(&results[n], 0, sizeof(cbm_search_result_t));
        scan_node(main_stmt, &results[n].node);
        results[n].in_degree = sqlite3_column_int(main_stmt, ST_COL_9);
        results[n].out_degree = sqlite3_column_int(main_stmt, CBM_DECIMAL_BASE);
        n++;
    }
    if (scan_rc13 != SQLITE_DONE) { /* SCANCHK:13:main_stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(main_stmt);
        like_pool_free(&like_pool);
        out->results = results;
        out->count = n;
        return CBM_STORE_ERR;
    }

    sqlite3_finalize(main_stmt);
    like_pool_free(&like_pool);

    out->results = results;
    out->count = n;
    return CBM_STORE_OK;
}

void cbm_store_search_free(cbm_search_output_t *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < out->count; i++) {
        cbm_search_result_t *r = &out->results[i];
        safe_str_free(&r->node.project);
        safe_str_free(&r->node.label);
        safe_str_free(&r->node.name);
        safe_str_free(&r->node.qualified_name);
        safe_str_free(&r->node.file_path);
        safe_str_free(&r->node.properties_json);
        for (int j = 0; j < r->connected_count; j++) {
            safe_str_free(&r->connected_names[j]);
        }
        free(r->connected_names);
    }
    free(out->results);
    memset(out, 0, sizeof(*out));
}

/* ── BFS Traversal ──────────────────────────────────────────────── */

static int bfs_collect_edges(cbm_store_t *s, int64_t start_id, const cbm_node_hop_t *visited,
                             int visited_count, const char *types_clause, const char **edge_types,
                             int edge_type_count, cbm_edge_info_t **out_edges,
                             int *out_edge_count) {
    /* Build ID set: root + all visited */
    char id_set[CBM_SZ_4K];
    int ilen = snprintf(id_set, sizeof(id_set), "%lld", (long long)start_id);
    if (ilen >= (int)sizeof(id_set)) {
        ilen = (int)sizeof(id_set) - SKIP_ONE;
    }
    for (int i = 0; i < visited_count; i++) {
        ilen += snprintf(id_set + ilen, sizeof(id_set) - (size_t)ilen, ",%lld",
                         (long long)visited[i].node.id);
        if (ilen >= (int)sizeof(id_set)) {
            ilen = (int)sizeof(id_set) - SKIP_ONE;
        }
    }

    char edge_sql[ST_SQL_BUF];
    snprintf(edge_sql, sizeof(edge_sql),
             "SELECT n1.name, n2.name, e.type, e.source_id, e.target_id, e.properties "
             "FROM edges e "
             "JOIN nodes n1 ON n1.id = e.source_id "
             "JOIN nodes n2 ON n2.id = e.target_id "
             "WHERE e.source_id IN (%s) AND e.target_id IN (%s) "
             "AND e.type IN (%s)",
             id_set, id_set, types_clause);

    sqlite3_stmt *estmt = NULL;
    int rc = sqlite3_prepare_v2(s->db, edge_sql, CBM_NOT_FOUND, &estmt, NULL);
    if (rc != SQLITE_OK) {
        *out_edges = NULL;
        *out_edge_count = 0;
        return CBM_STORE_OK;
    }

    if (edge_type_count > 0) {
        for (int i = 0; i < edge_type_count; i++) {
            bind_text(estmt, i + SKIP_ONE, edge_types[i]);
        }
    } else {
        bind_text(estmt, SKIP_ONE, "CALLS");
    }

    int ecap = ST_INIT_CAP_8;
    int en = 0;
    cbm_edge_info_t *edges = malloc(ecap * sizeof(cbm_edge_info_t));

    int scan_rc14;
    while ((scan_rc14 = sqlite3_step(estmt)) == SQLITE_ROW) {
        if (en >= ecap) {
            ecap *= ST_GROWTH;
            edges = safe_realloc(edges, ecap * sizeof(cbm_edge_info_t));
        }
        edges[en].from_name = heap_strdup((const char *)sqlite3_column_text(estmt, 0));
        edges[en].to_name = heap_strdup((const char *)sqlite3_column_text(estmt, SKIP_ONE));
        edges[en].type = heap_strdup((const char *)sqlite3_column_text(estmt, CBM_SZ_2));
        edges[en].confidence = (double)SKIP_ONE;
        edges[en].source_id = sqlite3_column_int64(estmt, ST_COL_3);
        edges[en].target_id = sqlite3_column_int64(estmt, ST_COL_4);
        edges[en].properties_json = heap_strdup((const char *)sqlite3_column_text(estmt, CBM_SZ_5));
        en++;
    }
    if (scan_rc14 != SQLITE_DONE) { /* SCANCHK:14:estmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(estmt);
        *out_edges = edges;
        *out_edge_count = en;
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(estmt);

    *out_edges = edges;
    *out_edge_count = en;
    return CBM_STORE_OK;
}

/* Build parameterized placeholder list "?1,?2,?3" for N edge types. */
static void bfs_build_types_clause(int edge_type_count, char *buf, int buf_sz) {
    if (edge_type_count <= 0) {
        snprintf(buf, buf_sz, "?1");
        return;
    }
    int tlen = 0;
    for (int i = 0; i < edge_type_count; i++) {
        if (i > 0) {
            tlen += snprintf(buf + tlen, buf_sz - tlen, ",");
            if (tlen >= buf_sz) {
                tlen = buf_sz - SKIP_ONE;
            }
        }
        tlen += snprintf(buf + tlen, buf_sz - tlen, "?%d", i + SKIP_ONE);
        if (tlen >= buf_sz) {
            tlen = buf_sz - SKIP_ONE;
        }
    }
}

static int store_find_cached_zova_workspace_node(cbm_store_t *s, int64_t node_numeric_id,
                                                 cbm_node_t *out) {
    if (!s || !out || !s->zova_workspace_query ||
        !s->zova_graph_cached_stable_id_valid ||
        s->zova_graph_cached_numeric_id != node_numeric_id) {
        return CBM_STORE_NOT_FOUND;
    }
    return cbm_zova_repository_find_node_by_numeric_id(
        s->zova_repository, s->zova_graph_workspace_id, node_numeric_id, out);
}

static int store_zova_workspace_bfs(cbm_store_t *s, int64_t start_id, const char *direction,
                                    const char **edge_types, int edge_type_count, int max_depth,
                                    int max_results, cbm_traverse_result_t *out) {
    if (!s->zova_workspace_query || !s->zova_graph_session ||
        (edge_type_count > 1) ||
        (edge_type_count == 1 && strcmp(edge_types[0], "CALLS") != 0) ||
        (direction && strcmp(direction, "outbound") != 0 && strcmp(direction, "inbound") != 0))
        return CBM_STORE_NOT_FOUND;
    char stable_copy[CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    stable_copy[0] = '\0';
    if (s->zova_graph_cached_stable_id_valid &&
        s->zova_graph_cached_numeric_id == start_id) {
        snprintf(stable_copy, sizeof(stable_copy), "%s",
                 s->zova_graph_cached_stable_id);
    } else {
        int stable_rc = cbm_zova_repository_stable_id_for_numeric_id(
            s->zova_repository, s->zova_graph_workspace_id, start_id,
            stable_copy, sizeof(stable_copy));
        if (stable_rc != CBM_STORE_OK) return stable_rc;
        if (stable_copy[0]) {
            s->zova_graph_cached_numeric_id = start_id;
            snprintf(s->zova_graph_cached_stable_id,
                     sizeof(s->zova_graph_cached_stable_id), "%s", stable_copy);
            s->zova_graph_cached_stable_id_valid = true;
        }
    }
    if (!stable_copy[0]) return CBM_STORE_NOT_FOUND;
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    if (cbm_zova_workspace_graph_name(s->zova_graph_workspace_id, graph_name,
                                      sizeof(graph_name)) != 0) return CBM_STORE_ERR;
    cbm_zova_graph_visit_t *visits = NULL;
    int count = 0;
    cbm_zova_graph_metrics_t metrics = {0};
    if (cbm_zova_graph_session_walk_calls(
            s->zova_graph_session, s->zova_graph_workspace_id, graph_name, stable_copy,
            direction ? direction : "outbound", max_depth, max_results, &visits, &count,
            &metrics) != 0) return CBM_STORE_ERR;
    cbm_node_hop_t *items = count ? calloc((size_t)count, sizeof(*items)) : NULL;
    if (count && !items) { cbm_zova_graph_visits_free(visits, count); return CBM_STORE_ERR; }
    for (int i = 0; i < count; ++i) {
        items[i].node.id = store_stable_numeric_id(visits[i].node_id);
        items[i].node.project = heap_strdup(out->root.project ? out->root.project : "");
        items[i].node.label = visits[i].label; visits[i].label = NULL;
        items[i].node.name = visits[i].name; visits[i].name = NULL;
        items[i].node.qualified_name = visits[i].qualified_name; visits[i].qualified_name = NULL;
        items[i].node.file_path = visits[i].file_path; visits[i].file_path = NULL;
        items[i].node.start_line = visits[i].start_line;
        items[i].node.end_line = visits[i].end_line;
        items[i].node.properties_json = visits[i].properties; visits[i].properties = NULL;
        items[i].hop = visits[i].hop;
    }
    cbm_zova_graph_visits_free(visits, count);
    out->visited = items;
    out->visited_count = count;
    s->zova_native_topology_ops++;
    return CBM_STORE_OK;
}

/* The native workspace walk follows CALLS only. Every other edge-type set
 * (CALL_REFERENCE, HTTP_CALLS, mixed sets) walks level by level over the
 * workspace edge lookup with the SQL route's hop ordering and limit, and keeps
 * the traversed edges so callers can read their properties. */
static bool bfs_edge_type_selected(const char *type, const char **edge_types,
                                   int edge_type_count) {
    if (!type) return false;
    if (edge_type_count <= 0) return strcmp(type, "CALLS") == 0;
    for (int i = 0; i < edge_type_count; i++)
        if (edge_types[i] && strcmp(type, edge_types[i]) == 0) return true;
    return false;
}

static int bfs_hop_index(const cbm_node_hop_t *hops, int count, int64_t id) {
    for (int i = 0; i < count; i++)
        if (hops[i].node.id == id) return i;
    return CBM_NOT_FOUND;
}

static int bfs_hop_order(const void *a, const void *b) {
    const cbm_node_t *x = &((const cbm_node_hop_t *)a)->node;
    const cbm_node_t *y = &((const cbm_node_hop_t *)b)->node;
    int c = strcmp(x->file_path ? x->file_path : "", y->file_path ? y->file_path : "");
    if (!c) c = strcmp(x->qualified_name ? x->qualified_name : "",
                       y->qualified_name ? y->qualified_name : "");
    if (!c) c = strcmp(x->label ? x->label : "", y->label ? y->label : "");
    if (c) return c;
    if (x->start_line != y->start_line) return x->start_line < y->start_line ? -1 : 1;
    if (x->end_line != y->end_line) return x->end_line < y->end_line ? -1 : 1;
    return x->id < y->id ? -1 : (x->id > y->id);
}

static int store_zova_workspace_bfs_typed(cbm_store_t *s, int64_t start_id,
                                          const char *direction, const char **edge_types,
                                          int edge_type_count, int max_depth, int max_results,
                                          cbm_traverse_result_t *out) {
    bool inbound = direction && strcmp(direction, "inbound") == 0;
    cbm_node_hop_t *hops = NULL;
    cbm_edge_t *seen = NULL;
    int hop_count = 0, hop_cap = 0, seen_count = 0, seen_cap = 0;
    int level_start = 0, level_end = 0; /* hops[level_start, level_end) is the frontier */
    int rc = CBM_STORE_OK;
    for (int depth = 0; depth < max_depth && rc == CBM_STORE_OK; depth++) {
        int frontier = depth == 0 ? SKIP_ONE : level_end - level_start;
        int next_start = hop_count;
        for (int f = 0; f < frontier && rc == CBM_STORE_OK; f++) {
            int64_t from = depth == 0 ? start_id : hops[level_start + f].node.id;
            cbm_edge_t *edges = NULL;
            int edge_count = 0;
            rc = store_zova_workspace_edges(s, from, inbound ? "inbound" : "outbound", NULL,
                                            &edges, &edge_count);
            for (int e = 0; e < edge_count && rc == CBM_STORE_OK; e++) {
                if (!bfs_edge_type_selected(edges[e].type, edge_types, edge_type_count))
                    continue;
                int64_t next = inbound ? edges[e].source_id : edges[e].target_id;
                if (seen_count == seen_cap) {
                    seen_cap = seen_cap ? seen_cap * ST_GROWTH : ST_INIT_CAP_16;
                    seen = safe_realloc(seen, (size_t)seen_cap * sizeof(*seen));
                }
                seen[seen_count++] = edges[e];
                memset(&edges[e], 0, sizeof(edges[e]));
                if (next == start_id || bfs_hop_index(hops, hop_count, next) >= 0) continue;
                if (hop_count == hop_cap) {
                    hop_cap = hop_cap ? hop_cap * ST_GROWTH : ST_INIT_CAP_16;
                    hops = safe_realloc(hops, (size_t)hop_cap * sizeof(*hops));
                }
                memset(&hops[hop_count], 0, sizeof(hops[hop_count]));
                rc = cbm_store_find_node_by_id(s, next, &hops[hop_count].node);
                if (rc == CBM_STORE_OK) hops[hop_count++].hop = depth + SKIP_ONE;
                else if (rc == CBM_STORE_NOT_FOUND) rc = CBM_STORE_OK;
            }
            cbm_store_free_edges(edges, edge_count);
        }
        if (hop_count - next_start > 1) /* hops stays NULL until the first hop */
            qsort(hops + next_start, (size_t)(hop_count - next_start), sizeof(*hops),
                  bfs_hop_order);
        while (hop_count > max_results) cbm_node_free_fields(&hops[--hop_count].node);
        level_start = next_start;
        level_end = hop_count;
        if (level_start == level_end) break;
    }
    /* Keep the edges whose endpoints both survived, as the SQL route does. */
    cbm_edge_info_t *infos = seen_count ? calloc((size_t)seen_count, sizeof(*infos)) : NULL;
    int info_count = 0;
    for (int e = 0; e < seen_count && infos; e++) {
        int si = bfs_hop_index(hops, hop_count, seen[e].source_id);
        int ti = bfs_hop_index(hops, hop_count, seen[e].target_id);
        if ((si < 0 && seen[e].source_id != start_id) ||
            (ti < 0 && seen[e].target_id != start_id)) continue;
        cbm_edge_info_t *info = &infos[info_count++];
        info->from_name = heap_strdup(si < 0 ? out->root.name : hops[si].node.name);
        info->to_name = heap_strdup(ti < 0 ? out->root.name : hops[ti].node.name);
        info->type = seen[e].type;
        seen[e].type = NULL;
        info->confidence = (double)SKIP_ONE;
        info->source_id = seen[e].source_id;
        info->target_id = seen[e].target_id;
        info->properties_json = seen[e].properties_json;
        seen[e].properties_json = NULL;
    }
    cbm_store_free_edges(seen, seen_count);
    out->visited = hops;
    out->visited_count = hop_count;
    out->edges = infos;
    out->edge_count = info_count;
    return rc;
}

static int store_bfs(cbm_store_t *s, int64_t start_id, const char *direction,
                     const char **edge_types, int edge_type_count, int max_depth,
                     int max_results, bool need_edges, cbm_traverse_result_t *out) {
    memset(out, 0, sizeof(*out));
    if (s && s->backend) {
        return s->backend->bfs(s->backend_ctx, start_id, direction, edge_types, edge_type_count,
                               max_depth, max_results, out);
    }

    cbm_node_t root = {0};
    int rc = store_find_cached_zova_workspace_node(s, start_id, &root);
    if (rc == CBM_STORE_NOT_FOUND) {
        rc = cbm_store_find_node_by_id(s, start_id, &root);
    }
    if (rc != CBM_STORE_OK) {
        return rc;
    }
    out->root = root;

    /* The native walk returns nodes only; a caller that reads edge
     * properties takes the typed walk instead. */
    rc = need_edges ? CBM_STORE_NOT_FOUND
                    : store_zova_workspace_bfs(s, start_id, direction, edge_types,
                                               edge_type_count, max_depth, max_results, out);
    if (rc == CBM_STORE_OK) return rc;
    if (s->zova_workspace_query && rc == CBM_STORE_NOT_FOUND)
        return store_zova_workspace_bfs_typed(s, start_id, direction, edge_types,
                                              edge_type_count, max_depth, max_results, out);
    if (s->zova_workspace_query) return rc;

    char types_clause[CBM_SZ_512];
    bfs_build_types_clause(edge_type_count, types_clause, (int)sizeof(types_clause));

    /* Build recursive CTE for BFS */
    char sql[CBM_SZ_4K];
    const char *join_cond;
    const char *next_id;
    bool is_inbound = (direction != NULL) && (strcmp(direction, "inbound") == 0);

    if (is_inbound) {
        join_cond = "e.target_id = bfs.node_id";
        next_id = "e.source_id";
    } else {
        join_cond = "e.source_id = bfs.node_id";
        next_id = "e.target_id";
    }

    snprintf(sql, sizeof(sql),
             "WITH RECURSIVE bfs(node_id, hop) AS ("
             "  SELECT %lld, 0"
             "  UNION"
             "  SELECT %s, bfs.hop + 1"
             "  FROM bfs"
             "  JOIN edges e ON %s"
             "  WHERE e.type IN (%s) AND bfs.hop < %d"
             "), shortest AS ("
             "  SELECT node_id, MIN(hop) AS hop FROM bfs "
             "  WHERE hop > 0 AND node_id != %lld GROUP BY node_id"
             ")"
             "SELECT DISTINCT n.id, n.project, n.label, n.name, n.qualified_name, "
             "n.file_path, n.start_line, n.end_line, n.properties, shortest.hop "
             "FROM shortest "
             "JOIN nodes n ON n.id = shortest.node_id "
             "ORDER BY shortest.hop, n.file_path, n.qualified_name, n.label, n.start_line, "
             "n.end_line, n.id "
             "LIMIT %d;",
             (long long)start_id, next_id, join_cond, types_clause, max_depth,
             (long long)start_id, max_results);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL);
    if (rc != SQLITE_OK) {
        store_set_error_sqlite(s, "bfs prepare");
        return CBM_STORE_ERR;
    }

    /* Bind edge type parameters */
    if (edge_type_count > 0) {
        for (int i = 0; i < edge_type_count; i++) {
            bind_text(stmt, i + SKIP_ONE, edge_types[i]);
        }
    } else {
        bind_text(stmt, SKIP_ONE, "CALLS");
    }

    int cap = ST_INIT_CAP_16;
    int n = 0;
    cbm_node_hop_t *visited = malloc(cap * sizeof(cbm_node_hop_t));

    int scan_rc15;
    while ((scan_rc15 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            visited = safe_realloc(visited, cap * sizeof(cbm_node_hop_t));
        }
        scan_node(stmt, &visited[n].node);
        visited[n].hop = sqlite3_column_int(stmt, ST_COL_9);
        n++;
    }
    if (scan_rc15 != SQLITE_DONE) { /* SCANCHK:15:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        out->visited = visited;
        out->visited_count = n;
        return CBM_STORE_ERR;
    }

    sqlite3_finalize(stmt);

    out->visited = visited;
    out->visited_count = n;

    /* Collect edges between visited nodes (including root) */
    if (n > 0) {
        bfs_collect_edges(s, start_id, out->visited, n, types_clause, edge_types, edge_type_count,
                          &out->edges, &out->edge_count);
    } else {
        out->edges = NULL;
        out->edge_count = 0;
    }

    return CBM_STORE_OK;
}

int cbm_store_bfs(cbm_store_t *s, int64_t start_id, const char *direction, const char **edge_types,
                  int edge_type_count, int max_depth, int max_results, cbm_traverse_result_t *out) {
    return store_bfs(s, start_id, direction, edge_types, edge_type_count, max_depth, max_results,
                     false, out);
}

int cbm_store_bfs_with_edges(cbm_store_t *s, int64_t start_id, const char *direction,
                             const char **edge_types, int edge_type_count, int max_depth,
                             int max_results, cbm_traverse_result_t *out) {
    return store_bfs(s, start_id, direction, edge_types, edge_type_count, max_depth, max_results,
                     true, out);
}

/* ── Relationship-trail walk for Cypher variable-length paths ──────
 *
 * cbm_store_bfs reports each node once, at its shortest distance. A Cypher
 * pattern such as [:CALLS*2..2] asks for paths of length 2, which may reach a
 * node whose shortest distance is 1, and must not reuse a relationship (a
 * self-loop cannot pad a path). So this walk reports every (node, hop) pair
 * that some edge-distinct path reaches.
 *
 * Path enumeration is exponential, so the walk has a row budget: every node
 * entered on a path counts one row, the start included. It goes deepest first
 * and, among siblings, lower node id first, so a wide first hop cannot use up
 * the budget before a deeper match is seen (upstream 0c5ffa50, 0d97729c,
 * cd22487c). It reads edges through cbm_store_find_edges_by_{source,target},
 * so the SQLite, Zova workspace and query-backend stores share one semantics. */
enum { ST_TRAIL_ROW_MULTIPLIER = 8, ST_TRAIL_MAX_ROWS = 4096 };

static int trail_row_limit(int max_results, int max_depth) {
    long base = (long)(max_results > 0 ? max_results : ST_INIT_CAP_16) * ST_TRAIL_ROW_MULTIPLIER +
                SKIP_ONE;
    long rows = base * (max_depth > 0 ? max_depth : ST_INIT_CAP_16);
    return rows > ST_TRAIL_MAX_ROWS ? ST_TRAIL_MAX_ROWS : (int)rows;
}

typedef struct {
    int64_t node;
    int64_t *next; /* selected neighbours, lower id first */
    int count;
} trail_adj_t;

typedef struct {
    int adj;    /* index into the adjacency cache */
    int cursor; /* next neighbour to try; the edge taken to reach the child is cursor - 1 */
} trail_frame_t;

static int trail_next_order(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return x < y ? -1 : (x > y);
}

/* Index of node's adjacency in the cache, loading it on first use. */
static int trail_adjacency(cbm_store_t *s, trail_adj_t **cache, int *count, int *cap,
                           int64_t node, bool inbound, const char **edge_types,
                           int edge_type_count, int *rc) {
    for (int i = 0; i < *count; i++)
        if ((*cache)[i].node == node) return i;
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    *rc = inbound ? cbm_store_find_edges_by_target(s, node, &edges, &edge_count)
                  : cbm_store_find_edges_by_source(s, node, &edges, &edge_count);
    if (*rc == CBM_STORE_NOT_FOUND) *rc = CBM_STORE_OK;
    if (*rc != CBM_STORE_OK) return CBM_NOT_FOUND;
    trail_adj_t adj = {.node = node};
    adj.next = edge_count ? malloc((size_t)edge_count * sizeof(*adj.next)) : NULL;
    for (int e = 0; e < edge_count && adj.next; e++)
        if (bfs_edge_type_selected(edges[e].type, edge_types, edge_type_count))
            adj.next[adj.count++] = inbound ? edges[e].source_id : edges[e].target_id;
    cbm_store_free_edges(edges, edge_count);
    /* glibc declares qsort's base nonnull: an empty adjacency has next == NULL. */
    if (adj.count > 1)
        qsort(adj.next, (size_t)adj.count, sizeof(*adj.next), trail_next_order);
    if (*count == *cap) {
        *cap = *cap ? *cap * ST_GROWTH : ST_INIT_CAP_16;
        *cache = safe_realloc(*cache, (size_t)*cap * sizeof(**cache));
    }
    (*cache)[*count] = adj;
    return (*count)++;
}

/* True when the edge (adjacency a, neighbour slot k) is already on the path. */
static bool trail_uses_edge(const trail_frame_t *stack, int depth, int a, int k) {
    for (int i = 0; i < depth; i++)
        if (stack[i].adj == a && stack[i].cursor - SKIP_ONE == k) return true;
    return false;
}

static int trail_hop_order(const void *a, const void *b) {
    const cbm_node_hop_t *x = a;
    const cbm_node_hop_t *y = b;
    if (x->hop != y->hop) return x->hop < y->hop ? -1 : 1;
    return bfs_hop_order(a, b);
}

int cbm_store_bfs_trail(cbm_store_t *s, int64_t start_id, const char *direction,
                        const char **edge_types, int edge_type_count, int max_depth,
                        int max_results, cbm_traverse_result_t *out) {
    memset(out, 0, sizeof(*out));
    int rc = cbm_store_find_node_by_id(s, start_id, &out->root);
    if (rc != CBM_STORE_OK) return rc;
    if (max_results < 0) max_results = 0;
    bool inbound = direction && strcmp(direction, "inbound") == 0;
    int row_limit = trail_row_limit(max_results, max_depth);

    trail_adj_t *cache = NULL;
    int cache_count = 0, cache_cap = 0;
    trail_frame_t *stack = malloc(((size_t)(max_depth > 0 ? max_depth : 0) + SKIP_ONE) *
                                  sizeof(*stack));
    cbm_node_hop_t *hops = NULL; /* distinct (node, hop) pairs; node holds only the id here */
    int hop_count = 0, hop_cap = 0;
    int rows = SKIP_ONE; /* the start node */
    int depth = 0;
    stack[0].adj = trail_adjacency(s, &cache, &cache_count, &cache_cap, start_id, inbound,
                                   edge_types, edge_type_count, &rc);
    stack[0].cursor = 0;
    while (rc == CBM_STORE_OK && depth >= 0 && stack[0].adj >= 0) {
        trail_frame_t *f = &stack[depth];
        const trail_adj_t *adj = &cache[f->adj];
        if (depth >= max_depth || f->cursor >= adj->count) {
            depth--;
            continue;
        }
        int k = f->cursor++;
        if (trail_uses_edge(stack, depth, f->adj, k)) continue;
        if (rows >= row_limit) {
            out->truncated = true;
            break;
        }
        rows++;
        int64_t next = adj->next[k];
        bool seen = false;
        for (int i = 0; i < hop_count && !seen; i++)
            seen = hops[i].node.id == next && hops[i].hop == depth + SKIP_ONE;
        if (!seen) {
            if (hop_count == hop_cap) {
                hop_cap = hop_cap ? hop_cap * ST_GROWTH : ST_INIT_CAP_16;
                hops = safe_realloc(hops, (size_t)hop_cap * sizeof(*hops));
            }
            memset(&hops[hop_count], 0, sizeof(hops[hop_count]));
            hops[hop_count].node.id = next;
            hops[hop_count++].hop = depth + SKIP_ONE;
        }
        if (depth + SKIP_ONE < max_depth) {
            int child = trail_adjacency(s, &cache, &cache_count, &cache_cap, next, inbound,
                                        edge_types, edge_type_count, &rc);
            if (child >= 0) {
                depth++;
                stack[depth].adj = child;
                stack[depth].cursor = 0;
            }
        }
    }
    for (int i = 0; i < cache_count; i++) free(cache[i].next);
    free(cache);
    free(stack);
    if (out->truncated) {
        char limit_buf[ST_BUF_16];
        snprintf(limit_buf, sizeof(limit_buf), "%d", row_limit);
        cbm_log_warn("cypher.trail_truncated", "rows", limit_buf, "result", "partial");
    }

    /* Materialize each pair; a node reached at several hops is read once. */
    int kept = 0;
    for (int i = 0; i < hop_count && rc == CBM_STORE_OK; i++) {
        int64_t id = hops[i].node.id;
        int prior = CBM_NOT_FOUND;
        for (int j = 0; j < kept && prior < 0; j++)
            if (hops[j].node.id == id) prior = j;
        cbm_node_t node = {0};
        int nrc = prior >= 0 ? CBM_STORE_OK : cbm_store_find_node_by_id(s, id, &node);
        if (nrc == CBM_STORE_NOT_FOUND) continue;
        if (nrc != CBM_STORE_OK) {
            rc = nrc;
            break;
        }
        if (prior >= 0) {
            const cbm_node_t *p = &hops[prior].node;
            node = *p;
            node.project = heap_strdup(p->project);
            node.label = heap_strdup(p->label);
            node.name = heap_strdup(p->name);
            node.qualified_name = heap_strdup(p->qualified_name);
            node.file_path = heap_strdup(p->file_path);
            node.properties_json = heap_strdup(p->properties_json);
        }
        int hop = hops[i].hop;
        hops[kept].node = node;
        hops[kept++].hop = hop;
    }
    hop_count = kept;
    if (hop_count > 0)
        qsort(hops, (size_t)hop_count, sizeof(*hops), trail_hop_order);
    if (hop_count > max_results) {
        while (hop_count > max_results) cbm_node_free_fields(&hops[--hop_count].node);
        out->truncated = true;
    }
    out->visited = hops;
    out->visited_count = hop_count;
    return rc;
}

void cbm_store_traverse_free(cbm_traverse_result_t *out) {
    if (!out) {
        return;
    }
    /* Free root */
    safe_str_free(&out->root.project);
    safe_str_free(&out->root.label);
    safe_str_free(&out->root.name);
    safe_str_free(&out->root.qualified_name);
    safe_str_free(&out->root.file_path);
    safe_str_free(&out->root.properties_json);

    /* Free visited */
    for (int i = 0; i < out->visited_count; i++) {
        cbm_node_hop_t *h = &out->visited[i];
        safe_str_free(&h->node.project);
        safe_str_free(&h->node.label);
        safe_str_free(&h->node.name);
        safe_str_free(&h->node.qualified_name);
        safe_str_free(&h->node.file_path);
        safe_str_free(&h->node.properties_json);
    }
    free(out->visited);

    /* Free edges */
    for (int i = 0; i < out->edge_count; i++) {
        safe_str_free(&out->edges[i].from_name);
        safe_str_free(&out->edges[i].to_name);
        safe_str_free(&out->edges[i].type);
        safe_str_free(&out->edges[i].properties_json);
    }
    free(out->edges);

    memset(out, 0, sizeof(*out));
}

/* ── Impact analysis ────────────────────────────────────────────── */

cbm_risk_level_t cbm_hop_to_risk(int hop) {
    switch (hop) {
    case SKIP_ONE:
        return CBM_RISK_CRITICAL;
    case ST_COL_2:
        return CBM_RISK_HIGH;
    case ST_COL_3:
        return CBM_RISK_MEDIUM;
    default:
        return CBM_RISK_LOW;
    }
}

const char *cbm_risk_label(cbm_risk_level_t level) {
    switch (level) {
    case CBM_RISK_CRITICAL:
        return "CRITICAL";
    case CBM_RISK_HIGH:
        return "HIGH";
    case CBM_RISK_MEDIUM:
        return "MEDIUM";
    case CBM_RISK_LOW:
    default:
        return "LOW";
    }
}

cbm_impact_summary_t cbm_build_impact_summary(const cbm_node_hop_t *hops, int hop_count,
                                              const cbm_edge_info_t *edges, int edge_count) {
    cbm_impact_summary_t s = {0};
    for (int i = 0; i < hop_count; i++) {
        switch (cbm_hop_to_risk(hops[i].hop)) {
        case CBM_RISK_CRITICAL:
            s.critical++;
            break;
        case CBM_RISK_HIGH:
            s.high++;
            break;
        case CBM_RISK_MEDIUM:
            s.medium++;
            break;
        case CBM_RISK_LOW:
            s.low++;
            break;
        }
        s.total++;
    }
    for (int i = 0; i < edge_count; i++) {
        if (edges[i].type && (strcmp(edges[i].type, "HTTP_CALLS") == 0 ||
                              strcmp(edges[i].type, "ASYNC_CALLS") == 0)) {
            s.has_cross_service = true;
            break;
        }
    }
    return s;
}

int cbm_deduplicate_hops(const cbm_node_hop_t *hops, int hop_count, cbm_node_hop_t **out,
                         int *out_count) {
    *out = NULL;
    *out_count = 0;
    if (hop_count == 0) {
        return CBM_STORE_OK;
    }

    /* Simple O(n²) dedup — keep minimum hop per node ID */
    cbm_node_hop_t *result = malloc(hop_count * sizeof(cbm_node_hop_t));
    int n = 0;

    for (int i = 0; i < hop_count; i++) {
        int found = ST_FOUND;
        for (int j = 0; j < n; j++) {
            if (result[j].node.id == hops[i].node.id) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            if (hops[i].hop < result[found].hop) {
                result[found].hop = hops[i].hop;
            }
        } else {
            result[n] = hops[i];
            n++;
        }
    }

    *out = safe_realloc(result, n * sizeof(cbm_node_hop_t));
    *out_count = n;
    return CBM_STORE_OK;
}

/* ── Schema ─────────────────────────────────────────────────────── */

enum { SCHEMA_MAX_JSON_KEYS = 50 };

/* Discover distinct JSON property keys for a table/column via json_each().
 * Prepends base_cols, then appends up to SCHEMA_MAX_JSON_KEYS from the query.
 * Caller must free the returned array and each string in it. */
static void schema_discover_props(sqlite3 *db, const char *sql, const char *project,
                                  const char *filter, const char **base_cols, int base_col_count,
                                  char ***out_props, int *out_count) {
    int pcap = base_col_count + SCHEMA_MAX_JSON_KEYS;
    char **props = malloc(pcap * sizeof(char *));
    int pn = 0;

    for (int b = 0; b < base_col_count; b++) {
        props[pn++] = heap_strdup(base_cols[b]);
    }

    sqlite3_stmt *pstmt = NULL;
    if (sqlite3_prepare_v2(db, sql, CBM_NOT_FOUND, &pstmt, NULL) == SQLITE_OK) {
        bind_text(pstmt, SKIP_ONE, project);
        bind_text(pstmt, PAIR_LEN, filter);
        while (sqlite3_step(pstmt) == SQLITE_ROW && pn < pcap) {
            props[pn++] = heap_strdup((const char *)sqlite3_column_text(pstmt, 0));
        }
        sqlite3_finalize(pstmt);
    }

    *out_props = props;
    *out_count = pn;
}

/* Path scoping for architecture / schema (shared). */
static bool arch_path_is_set(const char *path) {
    if (!path) {
        return false;
    }
    while (*path == ' ' || *path == '\t' || *path == '\n' || *path == '\r') {
        path++;
    }
    return path[0] != '\0';
}

static bool arch_path_prepare(const char *path, char *norm_out, size_t norm_sz, char *like_out,
                              size_t like_sz) {
    if (!arch_path_is_set(path)) {
        return false;
    }
    while (*path == ' ' || *path == '\t' || *path == '\n' || *path == '\r') {
        path++;
    }
    if (path[0] == '\0') {
        return false;
    }
    if (strncmp(path, "./", 2) == 0) {
        path += 2;
    }
    while (*path == '/') {
        path++;
    }
    if (path[0] == '\0') {
        return false;
    }
    strncpy(norm_out, path, norm_sz - 1);
    norm_out[norm_sz - 1] = '\0';
    size_t len = strlen(norm_out);
    while (len > 0 &&
           (norm_out[len - 1] == ' ' || norm_out[len - 1] == '\t' || norm_out[len - 1] == '/')) {
        norm_out[--len] = '\0';
    }
    /* Collapse duplicate slashes */
    size_t w = 0;
    for (size_t r = 0; norm_out[r] != '\0'; r++) {
        if (norm_out[r] == '/' && w > 0 && norm_out[w - 1] == '/') {
            continue;
        }
        norm_out[w++] = norm_out[r];
    }
    norm_out[w] = '\0';
    if (norm_out[0] == '\0') {
        return false;
    }
    snprintf(like_out, like_sz, "%s/%%", norm_out);
    return true;
}

static const char *arch_path_scope_sql(void) {
    return " AND (file_path = ? OR file_path LIKE ?)";
}

static void arch_bind_path_scope(sqlite3_stmt *stmt, int exact_idx, int like_idx, const char *norm,
                                 const char *like_pat) {
    bind_text(stmt, exact_idx, norm);
    bind_text(stmt, like_idx, like_pat);
}

bool cbm_store_arch_path_scoped(const char *path) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512 + 4];
    return arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
}

bool cbm_store_normalize_arch_path(const char *path, char *norm_out, size_t norm_sz) {
    char like[CBM_SZ_512 + 4];
    return arch_path_prepare(path, norm_out, norm_sz, like, sizeof(like));
}

/* Ids of the nodes view rows under a prepared path scope, sorted ascending. */
static int store_scope_node_ids(cbm_store_t *s, const char *project, const char *norm,
                                const char *like, int64_t **out, int *out_count) {
    *out = NULL;
    *out_count = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db,
                           "SELECT id FROM nodes WHERE project = ?1 "
                           "AND (file_path = ?2 OR file_path LIKE ?3) ORDER BY id;",
                           CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "scope_node_ids");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, ST_COL_1, project);
    arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    int cap = 0;
    int n = 0;
    int64_t *ids = NULL;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? cap * ST_GROWTH : CBM_SZ_256;
            int64_t *grown = realloc(ids, (size_t)cap * sizeof(*ids));
            if (!grown) {
                rc = SQLITE_NOMEM;
                break;
            }
            ids = grown;
        }
        ids[n++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        free(ids);
        store_set_error(s, "scope_node_ids");
        return CBM_STORE_ERR;
    }
    *out = ids;
    *out_count = n;
    return CBM_STORE_OK;
}

static int store_type_count_compare(const void *left_ptr, const void *right_ptr) {
    const cbm_type_count_t *left = left_ptr;
    const cbm_type_count_t *right = right_ptr;
    if (left->count != right->count) {
        return left->count > right->count ? CBM_NOT_FOUND : SKIP_ONE;
    }
    return strcmp(left->type, right->type);
}

/* Edge type counts of a Zova workspace store, most frequent first. With
 * scoped, only edges whose two ends are in the sorted scope ids count. */
static int store_zova_edge_type_counts(cbm_store_t *s, bool scoped, const int64_t *scope,
                                       int scope_count, cbm_type_count_t **out,
                                       int *out_count) {
    *out = NULL;
    *out_count = 0;
    if (store_zova_edges(s) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    int types = s->zova_edge_type_count;
    int *counts = calloc((size_t)types + SKIP_ONE, sizeof(*counts));
    cbm_type_count_t *arr = calloc((size_t)types + SKIP_ONE, sizeof(*arr));
    if (!counts || !arr) {
        free(counts);
        free(arr);
        return CBM_STORE_ERR;
    }
    for (int i = 0; i < s->zova_edge_count; i++) {
        const store_zova_edge_t *e = &s->zova_edges[i];
        if (!scoped || (store_id_in(scope, scope_count, e->source_key) &&
                        store_id_in(scope, scope_count, e->target_key))) {
            counts[e->type]++;
        }
    }
    int n = 0;
    for (int t = 0; t < types; t++) {
        if (counts[t] == 0) {
            continue;
        }
        arr[n].type = heap_strdup(s->zova_edge_types[t]);
        arr[n].count = counts[t];
        if (!arr[n].type) {
            for (int i = 0; i < n; i++) {
                safe_str_free(&arr[i].type);
            }
            free(counts);
            free(arr);
            return CBM_STORE_ERR;
        }
        n++;
    }
    free(counts);
    qsort(arr, (size_t)n, sizeof(*arr), store_type_count_compare);
    *out = arr;
    *out_count = n;
    return CBM_STORE_OK;
}

typedef struct {
    cbm_schema_info_t *schema;
    char ***keys;
    int *key_counts;
    int *key_caps;
    bool failed;
} store_edge_keys_t;

static int store_edge_keys_visit(int64_t source_key, int64_t target_key, const char *type,
                                 size_t type_len, const uint8_t *properties,
                                 size_t properties_len, void *context_ptr) {
    (void)source_key;
    (void)target_key;
    store_edge_keys_t *context = context_ptr;
    if (properties_len == 0 || (properties_len == PAIR_LEN && memcmp(properties, "{}", 2) == 0)) {
        return 0;
    }
    int t = CBM_NOT_FOUND;
    for (int i = 0; i < context->schema->edge_type_count; i++) {
        const char *name = context->schema->edge_types[i].type;
        if (strlen(name) == type_len && memcmp(name, type, type_len) == 0) {
            t = i;
            break;
        }
    }
    if (t < 0) {
        return 0;
    }
    yyjson_doc *doc = yyjson_read((const char *)properties, properties_len, 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_obj_iter iter;
    yyjson_val *key;
    if (yyjson_is_obj(root) && yyjson_obj_iter_init(root, &iter)) {
        while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
            const char *k = yyjson_get_str(key);
            bool seen = false;
            for (int i = 0; k && !seen && i < context->key_counts[t]; i++) {
                seen = strcmp(context->keys[t][i], k) == 0;
            }
            if (!k || seen) {
                continue;
            }
            if (context->key_counts[t] == context->key_caps[t]) {
                int cap = context->key_caps[t] ? context->key_caps[t] * ST_GROWTH : ST_INIT_CAP_8;
                char **grown = realloc(context->keys[t], (size_t)cap * sizeof(*grown));
                if (!grown) {
                    context->failed = true;
                    break;
                }
                context->keys[t] = grown;
                context->key_caps[t] = cap;
            }
            char *copy = heap_strdup(k);
            if (!copy) {
                context->failed = true;
                break;
            }
            context->keys[t][context->key_counts[t]++] = copy;
        }
    }
    yyjson_doc_free(doc);
    return context->failed ? CBM_NOT_FOUND : 0;
}

static int store_name_ptr_compare(const void *left_ptr, const void *right_ptr) {
    return strcmp(*(char *const *)left_ptr, *(char *const *)right_ptr);
}

/* Property keys of each edge type in schema, as get_schema_impl lists them:
 * the base columns, then up to SCHEMA_MAX_JSON_KEYS distinct JSON keys in
 * key order, read from the edge payloads in the Zova graph. */
static int store_zova_edge_property_keys(cbm_store_t *s, cbm_schema_info_t *schema,
                                         const char **base_cols, int base_col_count) {
    int types = schema->edge_type_count;
    store_edge_keys_t context = {
        .schema = schema,
        .keys = calloc((size_t)types + SKIP_ONE, sizeof(char **)),
        .key_counts = calloc((size_t)types + SKIP_ONE, sizeof(int)),
        .key_caps = calloc((size_t)types + SKIP_ONE, sizeof(int)),
    };
    int rc = context.keys && context.key_counts && context.key_caps &&
                     cbm_zova_repository_visit_edges(s->zova_repository,
                                                     s->zova_graph_workspace_id, NULL,
                                                     store_edge_keys_visit,
                                                     &context) == CBM_STORE_OK
                 ? CBM_STORE_OK
                 : CBM_STORE_ERR;
    for (int t = 0; t < types; t++) {
        int found = context.key_counts ? context.key_counts[t] : 0;
        char **keys = context.keys ? context.keys[t] : NULL;
        if (rc == CBM_STORE_OK) {
            if (found > 1) {
                qsort(keys, (size_t)found, sizeof(*keys), store_name_ptr_compare);
            }
            int take = found < SCHEMA_MAX_JSON_KEYS ? found : SCHEMA_MAX_JSON_KEYS;
            char **props = malloc((size_t)(base_col_count + take + SKIP_ONE) * sizeof(*props));
            if (props) {
                int pn = 0;
                for (int b = 0; b < base_col_count; b++) {
                    props[pn++] = heap_strdup(base_cols[b]);
                }
                for (int k = 0; k < take; k++) {
                    props[pn++] = keys[k];
                    keys[k] = NULL;
                }
                schema->edge_types[t].properties = props;
                schema->edge_types[t].property_count = pn;
            } else {
                rc = CBM_STORE_ERR;
            }
        }
        for (int k = 0; k < found; k++) {
            free(keys[k]);
        }
        free(keys);
    }
    free(context.keys);
    free(context.key_counts);
    free(context.key_caps);
    if (rc != CBM_STORE_OK) {
        store_set_error(s, "zova edge property keys");
    }
    return rc;
}

int cbm_store_count_nodes_scoped(cbm_store_t *s, const char *project, const char *path) {
    if (!s || !s->db || !project) {
        return 0;
    }
    if (store_legacy_scope(s, &project) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    if (!arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like))) {
        return cbm_store_count_nodes(s, project);
    }
    const char *sql = "SELECT COUNT(*) FROM nodes WHERE project = ?1 "
                      "AND (file_path = ?2 OR file_path LIKE ?3);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK || !stmt) {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
        return CBM_STORE_ERR;
    }
    bind_text(stmt, ST_COL_1, project);
    arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        n = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return n;
}

int cbm_store_count_edges_scoped(cbm_store_t *s, const char *project, const char *path) {
    if (!s || !s->db || !project) {
        return 0;
    }
    if (store_legacy_scope(s, &project) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    if (!arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like))) {
        return cbm_store_count_edges(s, project);
    }
    if (store_is_zova_workspace(s)) {
        int64_t *scope = NULL;
        int scope_count = 0;
        if (store_scope_node_ids(s, project, norm, like, &scope, &scope_count) != CBM_STORE_OK ||
            store_zova_edges(s) != CBM_STORE_OK) {
            free(scope);
            return CBM_STORE_ERR;
        }
        int n = 0;
        for (int i = 0; i < s->zova_edge_count; i++) {
            n += store_id_in(scope, scope_count, s->zova_edges[i].source_key) &&
                 store_id_in(scope, scope_count, s->zova_edges[i].target_key);
        }
        free(scope);
        return n;
    }
    const char *sql =
        "SELECT COUNT(*) FROM edges e WHERE e.project = ?1 "
        "AND EXISTS (SELECT 1 FROM nodes ns WHERE ns.id = e.source_id AND ns.project = ?1 "
        "AND (ns.file_path = ?2 OR ns.file_path LIKE ?3)) "
        "AND EXISTS (SELECT 1 FROM nodes nt WHERE nt.id = e.target_id AND nt.project = ?1 "
        "AND (nt.file_path = ?2 OR nt.file_path LIKE ?3));";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK || !stmt) {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
        return CBM_STORE_ERR;
    }
    bind_text(stmt, ST_COL_1, project);
    arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        n = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return n;
}

/* with_props=false skips the per-label/per-type JSON property-key discovery:
 * those json_each() scans walk EVERY row of each label/type (minutes-scale on
 * multi-million-node graphs) and get_architecture only needs the counts. */
static int get_schema_impl(cbm_store_t *s, const char *project, cbm_schema_info_t *out,
                           bool with_props) {
    memset(out, 0, sizeof(*out));
    if (!s || !s->db) {
        return CBM_NOT_FOUND;
    }
    if (store_legacy_scope(s, &project) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }

    /* Node labels */
    {
        const char *sql = "SELECT label, COUNT(*) FROM nodes WHERE project = ?1 GROUP BY label "
                          "ORDER BY COUNT(*) DESC;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK || !stmt) {
            if (stmt) {
                sqlite3_finalize(stmt);
            }
            return CBM_NOT_FOUND;
        }
        bind_text(stmt, SKIP_ONE, project);

        int cap = ST_INIT_CAP_8;
        int n = 0;
        cbm_label_count_t *arr = malloc(cap * sizeof(cbm_label_count_t));
        if (!arr) {
            sqlite3_finalize(stmt);
            return CBM_NOT_FOUND;
        }
        int scan_rc16;
        while ((scan_rc16 = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (n >= cap) {
                int new_cap = cap * ST_GROWTH;
                void *tmp = realloc(arr, new_cap * sizeof(cbm_label_count_t));
                if (!tmp) {
                    for (int i = 0; i < n; i++) {
                        safe_str_free(&arr[i].label);
                    }
                    free(arr);
                    sqlite3_finalize(stmt);
                    return CBM_NOT_FOUND;
                }
                arr = tmp;
                cap = new_cap;
            }
            arr[n].label = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
            arr[n].count = sqlite3_column_int(stmt, SKIP_ONE);
            arr[n].properties = NULL;
            arr[n].property_count = 0;
            n++;
        }
        if (scan_rc16 != SQLITE_DONE) { /* SCANCHK:16:stmt */
            store_set_error_sqlite(s, "row scan aborted");
            sqlite3_finalize(stmt);
            out->node_labels = arr;
            out->node_label_count = n;
            return CBM_STORE_ERR;
        }
        sqlite3_finalize(stmt);
        out->node_labels = arr;
        out->node_label_count = n;
    }

    /* Node label property keys: base columns + distinct JSON property keys per label */
    if (with_props) {
        static const char *node_base_cols[] = {"name", "qualified_name", "file_path", "start_line",
                                               "end_line"};
        const char *prop_sql = "SELECT DISTINCT je.key "
                               "FROM nodes, json_each(nodes.properties) AS je "
                               "WHERE nodes.project = ?1 AND nodes.label = ?2 "
                               "  AND nodes.properties != '{}' "
                               "ORDER BY je.key "
                               "LIMIT 50;";

        for (int i = 0; i < out->node_label_count; i++) {
            schema_discover_props(
                s->db, prop_sql, project, out->node_labels[i].label, node_base_cols,
                (int)(sizeof(node_base_cols) / sizeof(node_base_cols[0])),
                &out->node_labels[i].properties, &out->node_labels[i].property_count);
        }
    }

    /* Edge types */
    if (store_is_zova_workspace(s)) {
        if (store_zova_edge_type_counts(s, false, NULL, 0, &out->edge_types,
                                        &out->edge_type_count) != CBM_STORE_OK) {
            cbm_store_schema_free(out);
            return CBM_STORE_ERR;
        }
    } else {
        const char *sql = "SELECT type, COUNT(*) FROM edges WHERE project = ?1 GROUP BY type ORDER "
                          "BY COUNT(*) DESC;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK || !stmt) {
            if (stmt) {
                sqlite3_finalize(stmt);
            }
            cbm_store_schema_free(out);
            return CBM_NOT_FOUND;
        }
        bind_text(stmt, SKIP_ONE, project);

        int cap = ST_INIT_CAP_8;
        int n = 0;
        cbm_type_count_t *arr = malloc(cap * sizeof(cbm_type_count_t));
        if (!arr) {
            sqlite3_finalize(stmt);
            cbm_store_schema_free(out);
            return CBM_NOT_FOUND;
        }
        int scan_rc17;
        while ((scan_rc17 = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (n >= cap) {
                int new_cap = cap * ST_GROWTH;
                void *tmp = realloc(arr, new_cap * sizeof(cbm_type_count_t));
                if (!tmp) {
                    for (int i = 0; i < n; i++) {
                        safe_str_free(&arr[i].type);
                    }
                    free(arr);
                    sqlite3_finalize(stmt);
                    cbm_store_schema_free(out);
                    return CBM_NOT_FOUND;
                }
                arr = tmp;
                cap = new_cap;
            }
            arr[n].type = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
            arr[n].count = sqlite3_column_int(stmt, SKIP_ONE);
            arr[n].properties = NULL;
            arr[n].property_count = 0;
            n++;
        }
        if (scan_rc17 != SQLITE_DONE) { /* SCANCHK:17:stmt */
            store_set_error_sqlite(s, "row scan aborted");
            sqlite3_finalize(stmt);
            out->edge_types = arr;
            out->edge_type_count = n;
            return CBM_STORE_ERR;
        }
        sqlite3_finalize(stmt);
        out->edge_types = arr;
        out->edge_type_count = n;
    }

    /* Edge type property keys: base columns + distinct JSON property keys per type */
    if (with_props && store_is_zova_workspace(s)) {
        static const char *edge_base_cols[] = {"source_id", "target_id"};
        if (store_zova_edge_property_keys(s, out, edge_base_cols,
                                          (int)(sizeof(edge_base_cols) /
                                                sizeof(edge_base_cols[0]))) != CBM_STORE_OK) {
            cbm_store_schema_free(out);
            return CBM_STORE_ERR;
        }
    } else if (with_props) {
        static const char *edge_base_cols[] = {"source_id", "target_id"};
        const char *prop_sql = "SELECT DISTINCT je.key "
                               "FROM edges, json_each(edges.properties) AS je "
                               "WHERE edges.project = ?1 AND edges.type = ?2 "
                               "  AND edges.properties != '{}' "
                               "ORDER BY je.key "
                               "LIMIT 50;";

        for (int i = 0; i < out->edge_type_count; i++) {
            schema_discover_props(s->db, prop_sql, project, out->edge_types[i].type, edge_base_cols,
                                  (int)(sizeof(edge_base_cols) / sizeof(edge_base_cols[0])),
                                  &out->edge_types[i].properties,
                                  &out->edge_types[i].property_count);
        }
    }

    return CBM_STORE_OK;
}

int cbm_store_get_schema(cbm_store_t *s, const char *project, cbm_schema_info_t *out) {
    return get_schema_impl(s, project, out, true);
}

int cbm_store_get_schema_counts(cbm_store_t *s, const char *project, cbm_schema_info_t *out) {
    return get_schema_impl(s, project, out, false);
}

int cbm_store_get_schema_counts_scoped(cbm_store_t *s, const char *project, const char *path,
                                       cbm_schema_info_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s || !s->db) {
        return CBM_NOT_FOUND;
    }
    if (store_legacy_scope(s, &project) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    if (!scoped) {
        return get_schema_impl(s, project, out, false);
    }

    char sqlbuf[ST_SQL_BUF];
    {
        const char *base = "SELECT label, COUNT(*) FROM nodes WHERE project = ?1";
        snprintf(sqlbuf, sizeof(sqlbuf), "%s%s GROUP BY label ORDER BY COUNT(*) DESC;", base,
                 arch_path_scope_sql());
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(s->db, sqlbuf, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK || !stmt) {
            if (stmt) {
                sqlite3_finalize(stmt);
            }
            return CBM_NOT_FOUND;
        }
        bind_text(stmt, SKIP_ONE, project);
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);

        int cap = ST_INIT_CAP_8;
        int n = 0;
        cbm_label_count_t *arr = malloc(cap * sizeof(cbm_label_count_t));
        if (!arr) {
            sqlite3_finalize(stmt);
            return CBM_NOT_FOUND;
        }
        int scan_rc18;
        while ((scan_rc18 = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (n >= cap) {
                int new_cap = cap * ST_GROWTH;
                void *tmp = realloc(arr, new_cap * sizeof(cbm_label_count_t));
                if (!tmp) {
                    for (int i = 0; i < n; i++) {
                        safe_str_free(&arr[i].label);
                    }
                    free(arr);
                    sqlite3_finalize(stmt);
                    return CBM_NOT_FOUND;
                }
                arr = tmp;
                cap = new_cap;
            }
            arr[n].label = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
            arr[n].count = sqlite3_column_int(stmt, SKIP_ONE);
            arr[n].properties = NULL;
            arr[n].property_count = 0;
            n++;
        }
        if (scan_rc18 != SQLITE_DONE) { /* SCANCHK:18:stmt */
            store_set_error_sqlite(s, "row scan aborted");
            sqlite3_finalize(stmt);
            out->node_labels = arr;
            out->node_label_count = n;
            return CBM_STORE_ERR;
        }
        sqlite3_finalize(stmt);
        out->node_labels = arr;
        out->node_label_count = n;
    }

    if (store_is_zova_workspace(s)) {
        int64_t *scope = NULL;
        int scope_count = 0;
        int rc = store_scope_node_ids(s, project, norm, like, &scope, &scope_count);
        if (rc == CBM_STORE_OK) {
            rc = store_zova_edge_type_counts(s, true, scope, scope_count, &out->edge_types,
                                             &out->edge_type_count);
        }
        free(scope);
        if (rc != CBM_STORE_OK) {
            cbm_store_schema_free(out);
            return CBM_STORE_ERR;
        }
    } else {
        const char *esql =
            "SELECT e.type, COUNT(*) FROM edges e WHERE e.project = ?1 "
            "AND EXISTS (SELECT 1 FROM nodes ns WHERE ns.id = e.source_id AND ns.project = ?1 "
            "AND (ns.file_path = ?2 OR ns.file_path LIKE ?3)) "
            "AND EXISTS (SELECT 1 FROM nodes nt WHERE nt.id = e.target_id AND nt.project = ?1 "
            "AND (nt.file_path = ?2 OR nt.file_path LIKE ?3)) "
            "GROUP BY e.type ORDER BY COUNT(*) DESC;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(s->db, esql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK || !stmt) {
            if (stmt) {
                sqlite3_finalize(stmt);
            }
            cbm_store_schema_free(out);
            return CBM_NOT_FOUND;
        }
        bind_text(stmt, SKIP_ONE, project);
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);

        int cap = ST_INIT_CAP_8;
        int n = 0;
        cbm_type_count_t *arr = malloc(cap * sizeof(cbm_type_count_t));
        if (!arr) {
            sqlite3_finalize(stmt);
            cbm_store_schema_free(out);
            return CBM_NOT_FOUND;
        }
        int scan_rc19;
        while ((scan_rc19 = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (n >= cap) {
                int new_cap = cap * ST_GROWTH;
                void *tmp = realloc(arr, new_cap * sizeof(cbm_type_count_t));
                if (!tmp) {
                    for (int i = 0; i < n; i++) {
                        safe_str_free(&arr[i].type);
                    }
                    free(arr);
                    sqlite3_finalize(stmt);
                    cbm_store_schema_free(out);
                    return CBM_NOT_FOUND;
                }
                arr = tmp;
                cap = new_cap;
            }
            arr[n].type = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
            arr[n].count = sqlite3_column_int(stmt, SKIP_ONE);
            arr[n].properties = NULL;
            arr[n].property_count = 0;
            n++;
        }
        if (scan_rc19 != SQLITE_DONE) { /* SCANCHK:19:stmt */
            store_set_error_sqlite(s, "row scan aborted");
            sqlite3_finalize(stmt);
            out->edge_types = arr;
            out->edge_type_count = n;
            return CBM_STORE_ERR;
        }
        sqlite3_finalize(stmt);
        out->edge_types = arr;
        out->edge_type_count = n;
    }

    return CBM_STORE_OK;
}

void cbm_store_schema_free(cbm_schema_info_t *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < out->node_label_count; i++) {
        safe_str_free(&out->node_labels[i].label);
        for (int j = 0; j < out->node_labels[i].property_count; j++) {
            free(out->node_labels[i].properties[j]);
        }
        free(out->node_labels[i].properties);
    }
    free(out->node_labels);

    for (int i = 0; i < out->edge_type_count; i++) {
        safe_str_free(&out->edge_types[i].type);
        for (int j = 0; j < out->edge_types[i].property_count; j++) {
            free(out->edge_types[i].properties[j]);
        }
        free(out->edge_types[i].properties);
    }
    free(out->edge_types);

    for (int i = 0; i < out->rel_pattern_count; i++) {
        safe_str_free(&out->rel_patterns[i]);
    }
    free(out->rel_patterns);

    for (int i = 0; i < out->sample_func_count; i++) {
        safe_str_free(&out->sample_func_names[i]);
    }
    free(out->sample_func_names);

    for (int i = 0; i < out->sample_class_count; i++) {
        safe_str_free(&out->sample_class_names[i]);
    }
    free(out->sample_class_names);

    for (int i = 0; i < out->sample_qn_count; i++) {
        safe_str_free(&out->sample_qns[i]);
    }
    free(out->sample_qns);

    memset(out, 0, sizeof(*out));
}

/* ── Architecture helpers ───────────────────────────────────────── */

/* Extract sub-package from QN: project.dir1.dir2.sym → dir1 (4+ parts → [2], else [1]) */
const char *cbm_qn_to_package(const char *qn) {
    if (!qn || !qn[0]) {
        return "";
    }
    static CBM_TLS char buf[CBM_SZ_256];
    /* Find dots and extract segment */
    const char *dots[ST_QN_MAX_DOTS] = {NULL};
    int ndots = 0;
    for (const char *p = qn; *p && ndots < ST_QN_MAX_DOTS; p++) {
        if (*p == '.') {
            dots[ndots++] = p;
        }
    }
    /* 4+ segments: return segment[2] */
    if (ndots >= ST_QN_MIN_DOTS) {
        const char *start = dots[SKIP_ONE] + SKIP_ONE;
        int len = (int)(dots[ST_COL_2] - start);
        if (len > 0 && len < (int)sizeof(buf)) {
            memcpy(buf, start, len);
            buf[len] = '\0';
            return buf;
        }
    }
    /* 2+ segments: return segment[1] */
    if (ndots >= SKIP_ONE) {
        const char *start = dots[0] + SKIP_ONE;
        const char *end = (ndots >= ST_COL_2) ? dots[SKIP_ONE] : qn + strlen(qn);
        int len = (int)(end - start);
        if (len > 0 && len < (int)sizeof(buf)) {
            memcpy(buf, start, len);
            buf[len] = '\0';
            return buf;
        }
    }
    return "";
}

/* Extract top-level package from QN: project.dir1.rest → dir1 (segment[1]) */
const char *cbm_qn_to_top_package(const char *qn) {
    if (!qn || !qn[0]) {
        return "";
    }
    static CBM_TLS char buf[CBM_SZ_256];
    const char *first_dot = strchr(qn, '.');
    if (!first_dot) {
        return "";
    }
    const char *start = first_dot + SKIP_ONE;
    const char *second_dot = strchr(start, '.');
    const char *end = second_dot ? second_dot : qn + strlen(qn);
    int len = (int)(end - start);
    if (len > 0 && len < (int)sizeof(buf)) {
        memcpy(buf, start, len);
        buf[len] = '\0';
        return buf;
    }
    return "";
}

bool cbm_is_test_file_path(const char *fp) {
    if (!fp || fp[0] == '\0') {
        return false;
    }
    return strstr(fp, "test") != NULL;
}

/* File extension → language name mapping (table-driven) */
typedef struct {
    const char *ext;
    const char *lang;
} ext_lang_entry_t;

static const ext_lang_entry_t ext_lang_table[] = {
    {".py", "Python"},     {".go", "Go"},          {".js", "JavaScript"}, {".jsx", "JavaScript"},
    {".ts", "TypeScript"}, {".tsx", "TypeScript"}, {".rs", "Rust"},       {".java", "Java"},
    {".cpp", "C++"},       {".cc", "C++"},         {".cxx", "C++"},       {".c", "C"},
    {".h", "C"},           {".cs", "C#"},          {".php", "PHP"},       {".lua", "Lua"},
    {".scala", "Scala"},   {".kt", "Kotlin"},      {".rb", "Ruby"},       {".sh", "Bash"},
    {".bash", "Bash"},     {".zig", "Zig"},        {".ex", "Elixir"},     {".exs", "Elixir"},
    {".hs", "Haskell"},    {".ml", "OCaml"},       {".mli", "OCaml"},     {".html", "HTML"},
    {".css", "CSS"},       {".yaml", "YAML"},      {".yml", "YAML"},      {".toml", "TOML"},
    {".hcl", "HCL"},       {".tf", "HCL"},         {".sql", "SQL"},       {".erl", "Erlang"},
    {".swift", "Swift"},   {".dart", "Dart"},      {".groovy", "Groovy"}, {".pl", "Perl"},
    {".r", "R"},           {".scss", "SCSS"},      {".vue", "Vue"},       {".svelte", "Svelte"},
    {NULL, NULL},
};

static const char *ext_to_lang(const char *ext) {
    if (!ext) {
        return NULL;
    }
    for (const ext_lang_entry_t *e = ext_lang_table; e->ext; e++) {
        if (strcmp(ext, e->ext) == 0) {
            return e->lang;
        }
    }
    return NULL;
}

/* Get lowercase file extension from path */
static const char *file_ext(const char *path) {
    if (!path) {
        return NULL;
    }
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return NULL;
    }
    static CBM_TLS char buf[CBM_SZ_16];
    int len = (int)strlen(dot);
    if (len >= (int)sizeof(buf)) {
        return NULL;
    }
    for (int i = 0; i < len; i++) {
        buf[i] = (char)((dot[i] >= 'A' && dot[i] <= 'Z') ? dot[i] + CBM_SZ_32 : dot[i]);
    }
    buf[len] = '\0';
    return buf;
}

/* ── Architecture aspect implementations ───────────────────────── */

static int arch_languages(cbm_store_t *s, const char *project, const char *path,
                          cbm_architecture_info_t *out) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    char sqlbuf[ST_SQL_BUF];
    const char *base = "SELECT file_path FROM nodes WHERE project=?1 AND label='File'";
    if (scoped) {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s%s", base, arch_path_scope_sql());
    } else {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s", base);
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sqlbuf, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "arch_languages");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);
    if (scoped) {
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    }

    /* Count per language using a simple parallel array */
    const char *lang_names[CBM_SZ_64];
    int lang_counts[CBM_SZ_64];
    int nlang = 0;

    int scan_rc20;
    while ((scan_rc20 = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *fp = (const char *)sqlite3_column_text(stmt, 0);
        const char *ext = file_ext(fp);
        const char *lang = ext_to_lang(ext);
        if (!lang) {
            continue;
        }
        int found = ST_FOUND;
        for (int i = 0; i < nlang; i++) {
            if (strcmp(lang_names[i], lang) == 0) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            lang_counts[found]++;
        } else if (nlang < CBM_SZ_64) {
            lang_names[nlang] = lang;
            lang_counts[nlang] = SKIP_ONE;
            nlang++;
        }
    }
    if (scan_rc20 != SQLITE_DONE) { /* SCANCHK:20:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);

    /* Sort by count descending (simple insertion sort) */
    for (int i = SKIP_ONE; i < nlang; i++) {
        int j = i;
        while (j > 0 && lang_counts[j] > lang_counts[j - SKIP_ONE]) {
            int tc = lang_counts[j];
            lang_counts[j] = lang_counts[j - SKIP_ONE];
            lang_counts[j - SKIP_ONE] = tc;
            const char *tn = lang_names[j];
            lang_names[j] = lang_names[j - SKIP_ONE];
            lang_names[j - SKIP_ONE] = tn;
            j--;
        }
    }
    if (nlang > CBM_DECIMAL_BASE) {
        nlang = ST_MAX_LANG;
    }

    out->languages = (nlang > 0) ? calloc(nlang, sizeof(cbm_language_count_t)) : NULL;
    out->language_count = nlang;
    for (int i = 0; i < nlang; i++) {
        out->languages[i].language = heap_strdup(lang_names[i]);
        out->languages[i].file_count = lang_counts[i];
    }
    return CBM_STORE_OK;
}

static int arch_entry_points(cbm_store_t *s, const char *project, const char *path,
                             cbm_architecture_info_t *out) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    char sqlbuf[ST_SQL_BUF];
    const char *base = "SELECT name, qualified_name, file_path FROM nodes "
                       "WHERE project=?1 AND json_extract(properties, '$.is_entry_point') = 1 "
                       "AND (json_extract(properties, '$.is_test') IS NULL OR "
                       "json_extract(properties, '$.is_test') != 1) "
                       "AND file_path NOT LIKE '%test%'";
    if (scoped) {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s%s LIMIT 20", base, arch_path_scope_sql());
    } else {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s LIMIT 20", base);
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sqlbuf, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "arch_entry_points");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);
    if (scoped) {
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    }

    int cap = ST_INIT_CAP_8;
    int n = 0;
    cbm_entry_point_t *arr = calloc(cap, sizeof(cbm_entry_point_t));
    int scan_rc21;
    while ((scan_rc21 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            arr = safe_realloc(arr, cap * sizeof(cbm_entry_point_t));
        }
        arr[n].name = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
        arr[n].qualified_name = heap_strdup((const char *)sqlite3_column_text(stmt, SKIP_ONE));
        arr[n].file = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_2));
        n++;
    }
    if (scan_rc21 != SQLITE_DONE) { /* SCANCHK:21:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        out->entry_points = arr;
        out->entry_point_count = n;
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    out->entry_points = arr;
    out->entry_point_count = n;
    return CBM_STORE_OK;
}

/* Extract a JSON string value from a simple JSON object by key name. */
static char *extract_json_string_prop(const char *json, const char *key, int key_len) {
    if (!json) {
        return NULL;
    }
    const char *m = strstr(json, key);
    if (!m) {
        return NULL;
    }
    m = strchr(m + key_len, '"');
    if (!m) {
        return NULL;
    }
    m++;
    const char *end = strchr(m, '"');
    if (!end || end - m >= CBM_SZ_256) {
        return NULL;
    }
    char vbuf[CBM_SZ_256];
    memcpy(vbuf, m, end - m);
    vbuf[end - m] = '\0';
    return heap_strdup(vbuf);
}

static int arch_routes(cbm_store_t *s, const char *project, const char *path,
                       cbm_architecture_info_t *out) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    char sqlbuf[ST_SQL_BUF];
    const char *base = "SELECT name, properties, COALESCE(file_path, '') FROM nodes "
                       "WHERE project=?1 AND label='Route' "
                       "AND (json_extract(properties, '$.is_test') IS NULL OR "
                       "json_extract(properties, '$.is_test') != 1)";
    if (scoped) {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s%s LIMIT 20", base, arch_path_scope_sql());
    } else {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s LIMIT 20", base);
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sqlbuf, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "arch_routes");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);
    if (scoped) {
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    }

    int cap = ST_INIT_CAP_8;
    int n = 0;
    cbm_route_info_t *arr = calloc(cap, sizeof(cbm_route_info_t));
    int scan_rc22;
    while ((scan_rc22 = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *props = (const char *)sqlite3_column_text(stmt, SKIP_ONE);
        const char *fp = (const char *)sqlite3_column_text(stmt, CBM_SZ_2);
        if (cbm_is_test_file_path(fp)) {
            continue;
        }
        if (n >= cap) {
            cap *= ST_GROWTH;
            arr = safe_realloc(arr, cap * sizeof(cbm_route_info_t));
        }

        arr[n].method = heap_strdup("");
        arr[n].path = heap_strdup(name);
        arr[n].handler = heap_strdup("");

        char *val;
        val = extract_json_string_prop(props, "\"method\"", ST_METHOD_PROP_LEN);
        if (val) {
            safe_str_free(&arr[n].method);
            arr[n].method = val;
        }
        val = extract_json_string_prop(props, "\"path\"", ST_PATH_PROP_LEN);
        if (val) {
            safe_str_free(&arr[n].path);
            arr[n].path = val;
        }
        val = extract_json_string_prop(props, "\"handler\"", ST_HANDLER_PROP_LEN);
        if (val) {
            safe_str_free(&arr[n].handler);
            arr[n].handler = val;
        }
        n++;
    }
    if (scan_rc22 != SQLITE_DONE) { /* SCANCHK:22:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        out->routes = arr;
        out->route_count = n;
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    out->routes = arr;
    out->route_count = n;
    return CBM_STORE_OK;
}

typedef struct {
    int64_t source_id;
    int64_t target_id;
} store_id_pair_t;

/* The project's CALLS edges as (source, target) node ids: from the Zova edge
 * list for a workspace store, otherwise from the edges table. */
static int store_calls_pairs(cbm_store_t *s, const char *project, store_id_pair_t **out,
                             int *out_count) {
    *out = NULL;
    *out_count = 0;
    int cap = 0;
    int n = 0;
    store_id_pair_t *pairs = NULL;
    if (store_is_zova_workspace(s)) {
        if (store_zova_edges(s) != CBM_STORE_OK) {
            return CBM_STORE_ERR;
        }
        int t = store_zova_edge_type_find(s, "CALLS", strlen("CALLS"));
        for (int i = 0; t >= 0 && i < s->zova_edge_count; i++) {
            cap += s->zova_edges[i].type == t;
        }
        pairs = cap ? malloc((size_t)cap * sizeof(*pairs)) : NULL;
        if (cap && !pairs) {
            return CBM_STORE_ERR;
        }
        for (int i = 0; t >= 0 && i < s->zova_edge_count; i++) {
            if (s->zova_edges[i].type == t) {
                pairs[n++] = (store_id_pair_t){s->zova_edges[i].source_key,
                                               s->zova_edges[i].target_key};
            }
        }
        *out = pairs;
        *out_count = n;
        return CBM_STORE_OK;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db,
                           "SELECT source_id, target_id FROM edges WHERE project=?1 "
                           "AND type='CALLS'",
                           CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "calls_pairs");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? cap * ST_GROWTH : CBM_SZ_256;
            store_id_pair_t *grown = realloc(pairs, (size_t)cap * sizeof(*pairs));
            if (!grown) {
                rc = SQLITE_NOMEM;
                break;
            }
            pairs = grown;
        }
        pairs[n++] = (store_id_pair_t){sqlite3_column_int64(stmt, 0),
                                       sqlite3_column_int64(stmt, SKIP_ONE)};
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        free(pairs);
        store_set_error(s, "calls_pairs");
        return CBM_STORE_ERR;
    }
    *out = pairs;
    *out_count = n;
    return CBM_STORE_OK;
}

/* Hotspot candidates: non-test functions and methods. */
#define ARCH_HOTSPOT_FILTER                                                                       \
    "n.project=?1 AND n.label IN ('Function', 'Method') "                                         \
    "AND (json_extract(n.properties, '$.is_test') IS NULL OR "                                    \
    "json_extract(n.properties, '$.is_test') != 1) "                                              \
    "AND n.file_path NOT LIKE '%test%'"

/* Hotspots of a Zova workspace store: fan-in counted from the CALLS edges of
 * the edge list, ties by node id, top 10. */
static int arch_hotspots_zova(cbm_store_t *s, const char *project, const char *norm,
                              const char *like, cbm_architecture_info_t *out) {
    store_id_pair_t *calls = NULL;
    int call_count = 0;
    if (store_calls_pairs(s, project, &calls, &call_count) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    int64_t *targets = malloc(((size_t)call_count + SKIP_ONE) * sizeof(*targets));
    if (!targets) {
        free(calls);
        return CBM_STORE_ERR;
    }
    for (int i = 0; i < call_count; i++) {
        targets[i] = calls[i].target_id;
    }
    free(calls);
    qsort(targets, (size_t)call_count, sizeof(*targets), store_id_compare);

    char sqlbuf[ST_SQL_BUF];
    snprintf(sqlbuf, sizeof(sqlbuf),
             "SELECT n.id, n.name, n.qualified_name FROM nodes n WHERE %s%s ORDER BY n.id",
             ARCH_HOTSPOT_FILTER, norm ? " AND (n.file_path = ?2 OR n.file_path LIKE ?3)" : "");
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sqlbuf, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        free(targets);
        store_set_error_sqlite(s, "arch_hotspots");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);
    if (norm) {
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    }
    enum { HOTSPOT_TOP = 10 };
    cbm_hotspot_t *arr = calloc(HOTSPOT_TOP, sizeof(*arr));
    int n = 0;
    int t = 0;
    int rc = SQLITE_DONE;
    while (arr && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        while (t < call_count && targets[t] < id) {
            t++;
        }
        int fan_in = 0;
        while (t < call_count && targets[t] == id) {
            fan_in++;
            t++;
        }
        /* Rows come in id order, so an equal fan-in never displaces. */
        if (fan_in == 0 || (n == HOTSPOT_TOP && fan_in <= arr[n - SKIP_ONE].fan_in)) {
            continue;
        }
        int at = n < HOTSPOT_TOP ? n++ : n - SKIP_ONE;
        safe_str_free(&arr[at].name);
        safe_str_free(&arr[at].qualified_name);
        while (at > 0 && arr[at - SKIP_ONE].fan_in < fan_in) {
            arr[at] = arr[at - SKIP_ONE];
            at--;
        }
        arr[at] = (cbm_hotspot_t){
            .name = heap_strdup((const char *)sqlite3_column_text(stmt, SKIP_ONE)),
            .qualified_name = heap_strdup((const char *)sqlite3_column_text(stmt, CBM_SZ_2)),
            .fan_in = fan_in,
        };
    }
    sqlite3_finalize(stmt);
    free(targets);
    if (!arr || rc != SQLITE_DONE) {
        for (int i = 0; arr && i < n; i++) {
            safe_str_free(&arr[i].name);
            safe_str_free(&arr[i].qualified_name);
        }
        free(arr);
        store_set_error(s, "arch_hotspots");
        return CBM_STORE_ERR;
    }
    out->hotspots = arr;
    out->hotspot_count = n;
    return CBM_STORE_OK;
}

static int arch_hotspots(cbm_store_t *s, const char *project, const char *path,
                         cbm_architecture_info_t *out) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    if (store_is_zova_workspace(s)) {
        return arch_hotspots_zova(s, project, scoped ? norm : NULL, like, out);
    }
    char sqlbuf[ST_SQL_BUF];
    const char *base = "SELECT n.name, n.qualified_name, COUNT(*) as fan_in "
                       "FROM nodes n JOIN edges e ON e.target_id = n.id AND e.type = 'CALLS' "
                       "WHERE " ARCH_HOTSPOT_FILTER;
    if (scoped) {
        snprintf(sqlbuf, sizeof(sqlbuf),
                 "%s AND (n.file_path = ?2 OR n.file_path LIKE ?3) "
                 "GROUP BY n.id ORDER BY fan_in DESC LIMIT 10",
                 base);
    } else {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s GROUP BY n.id ORDER BY fan_in DESC LIMIT 10", base);
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sqlbuf, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "arch_hotspots");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);
    if (scoped) {
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    }

    int cap = ST_INIT_CAP_8;
    int n = 0;
    cbm_hotspot_t *arr = calloc(cap, sizeof(cbm_hotspot_t));
    int scan_rc23;
    while ((scan_rc23 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            arr = safe_realloc(arr, cap * sizeof(cbm_hotspot_t));
        }
        arr[n].name = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
        arr[n].qualified_name = heap_strdup((const char *)sqlite3_column_text(stmt, SKIP_ONE));
        arr[n].fan_in = sqlite3_column_int(stmt, CBM_SZ_2);
        n++;
    }
    if (scan_rc23 != SQLITE_DONE) { /* SCANCHK:23:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        out->hotspots = arr;
        out->hotspot_count = n;
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    out->hotspots = arr;
    out->hotspot_count = n;
    return CBM_STORE_OK;
}

/* Look up package name for a node ID in the parallel arrays. nids must be
 * sorted ascending (the node query orders by id) — a linear scan here is
 * O(E×N) across the edge loop and spun for >10 minutes on Linux-kernel-sized
 * graphs (~1.4M defs × ~1.4M CALLS edges). */
static const char *lookup_pkg(const int64_t *nids, char **npkgs, int nn, int64_t id) {
    int lo = 0;
    int hi = nn - SKIP_ONE;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / PAIR_LEN;
        if (nids[mid] == id) {
            return npkgs[mid];
        }
        if (nids[mid] < id) {
            lo = mid + SKIP_ONE;
        } else {
            hi = mid - SKIP_ONE;
        }
    }
    return NULL;
}

/* Accumulate a cross-package boundary into parallel arrays. */
static void accum_boundary(const char *src_pkg, const char *tgt_pkg, char **bfroms, char **btos,
                           int *bcounts, int *bn, int bcap) {
    int found = ST_FOUND;
    for (int i = 0; i < *bn; i++) {
        if (strcmp(bfroms[i], src_pkg) == 0 && strcmp(btos[i], tgt_pkg) == 0) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        bcounts[found]++;
    } else if (*bn < bcap) {
        bfroms[*bn] = heap_strdup(src_pkg);
        btos[*bn] = heap_strdup(tgt_pkg);
        bcounts[*bn] = SKIP_ONE;
        (*bn)++;
    }
}

static int arch_boundaries(cbm_store_t *s, const char *project, const char *path,
                           cbm_cross_pkg_boundary_t **out_arr, int *out_count) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    char nsqlbuf[ST_SQL_BUF];
    const char *nbase =
        "SELECT id, qualified_name, file_path FROM nodes WHERE project=?1 AND label IN "
        "('Function','Method','Class')";
    if (scoped) {
        snprintf(nsqlbuf, sizeof(nsqlbuf), "%s%s ORDER BY id", nbase, arch_path_scope_sql());
    } else {
        snprintf(nsqlbuf, sizeof(nsqlbuf), "%s ORDER BY id", nbase);
    }
    sqlite3_stmt *nstmt = NULL;
    if (sqlite3_prepare_v2(s->db, nsqlbuf, CBM_NOT_FOUND, &nstmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "arch_boundaries_nodes");
        return CBM_STORE_ERR;
    }
    bind_text(nstmt, SKIP_ONE, project);
    if (scoped) {
        arch_bind_path_scope(nstmt, ST_COL_2, ST_COL_3, norm, like);
    }

    int ncap = CBM_SZ_256;
    int nn = 0;
    int64_t *nids = malloc(ncap * sizeof(int64_t));
    char **npkgs = malloc(ncap * sizeof(char *));

    int scan_rc24;
    while ((scan_rc24 = sqlite3_step(nstmt)) == SQLITE_ROW) {
        if (nn >= ncap) {
            ncap *= ST_GROWTH;
            nids = safe_realloc(nids, ncap * sizeof(int64_t));
            npkgs = safe_realloc(npkgs, ncap * sizeof(char *));
        }
        int64_t nid = sqlite3_column_int64(nstmt, 0);
        nids[nn] = nid;
        const char *qn = (const char *)sqlite3_column_text(nstmt, SKIP_ONE);
        npkgs[nn] = heap_strdup(cbm_qn_to_package(qn));
        nn++;
    }
    if (scan_rc24 != SQLITE_DONE) { /* SCANCHK:24:nstmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(nstmt);
        for (int bi = 0; bi < nn; bi++) {
            free(npkgs[bi]);
        }
        free(nids);
        free(npkgs);
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(nstmt);

    /* Scan edges, count cross-package calls */
    store_id_pair_t *calls = NULL;
    int call_count = 0;
    if (store_calls_pairs(s, project, &calls, &call_count) != CBM_STORE_OK) {
        for (int i = 0; i < nn; i++) {
            free(npkgs[i]);
        }
        free(nids);
        free(npkgs);
        return CBM_STORE_ERR;
    }

    int bcap = CBM_SZ_32;
    int bn = 0;
    char **bfroms = malloc(bcap * sizeof(char *));
    char **btos = malloc(bcap * sizeof(char *));
    int *bcounts = malloc(bcap * sizeof(int));

    for (int e = 0; e < call_count; e++) {
        int64_t src_id = calls[e].source_id;
        int64_t tgt_id = calls[e].target_id;
        const char *src_pkg = lookup_pkg(nids, npkgs, nn, src_id);
        const char *tgt_pkg = lookup_pkg(nids, npkgs, nn, tgt_id);
        if (!src_pkg || !tgt_pkg || !src_pkg[0] || !tgt_pkg[0] || strcmp(src_pkg, tgt_pkg) == 0) {
            continue;
        }
        accum_boundary(src_pkg, tgt_pkg, bfroms, btos, bcounts, &bn, bcap);
    }
    free(calls);
    for (int i = 0; i < nn; i++) {
        free(npkgs[i]);
    }
    free(nids);
    free(npkgs);

    /* Sort by count descending */
    for (int i = SKIP_ONE; i < bn; i++) {
        int j = i;
        while (j > 0 && bcounts[j] > bcounts[j - SKIP_ONE]) {
            int tc = bcounts[j];
            bcounts[j] = bcounts[j - SKIP_ONE];
            bcounts[j - SKIP_ONE] = tc;
            char *tf = bfroms[j];
            bfroms[j] = bfroms[j - SKIP_ONE];
            bfroms[j - SKIP_ONE] = tf;
            char *tt = btos[j];
            btos[j] = btos[j - SKIP_ONE];
            btos[j - SKIP_ONE] = tt;
            j--;
        }
    }
    if (bn > CBM_DECIMAL_BASE) {
        for (int i = ST_MAX_ITERATIONS; i < bn; i++) {
            free(bfroms[i]);
            free(btos[i]);
        }
        bn = ST_MAX_ITERATIONS;
    }

    cbm_cross_pkg_boundary_t *result =
        (bn > 0) ? calloc(bn, sizeof(cbm_cross_pkg_boundary_t)) : NULL;
    for (int i = 0; i < bn; i++) {
        result[i].from = bfroms[i];
        result[i].to = btos[i];
        result[i].call_count = bcounts[i];
    }
    free(bfroms);
    free(btos);
    free(bcounts);
    *out_arr = result;
    *out_count = bn;
    return CBM_STORE_OK;
}

#define MAX_PREVIEW_NAMES 15

/* Fallback: derive packages from QN segments when no Package nodes exist. */
static int arch_packages_from_qn(cbm_store_t *s, const char *project, const char *path,
                                 cbm_package_summary_t **out_arr, int *out_count) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    char qsqlbuf[ST_SQL_BUF];
    const char *qbase = "SELECT qualified_name FROM nodes WHERE project=?1 AND label IN "
                        "('Function','Method','Class')";
    if (scoped) {
        snprintf(qsqlbuf, sizeof(qsqlbuf), "%s%s", qbase, arch_path_scope_sql());
    } else {
        snprintf(qsqlbuf, sizeof(qsqlbuf), "%s", qbase);
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, qsqlbuf, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "arch_packages_qn");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);
    if (scoped) {
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    }

    char *pnames[CBM_SZ_64];
    int pcounts[CBM_SZ_64];
    int np = 0;
    int scan_rc26;
    while ((scan_rc26 = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(stmt, 0);
        const char *pkg = cbm_qn_to_package(qn);
        if (!pkg[0]) {
            continue;
        }
        int found = ST_FOUND;
        for (int i = 0; i < np; i++) {
            if (strcmp(pnames[i], pkg) == 0) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            pcounts[found]++;
        } else if (np < CBM_SZ_64) {
            pnames[np] = heap_strdup(pkg);
            pcounts[np] = SKIP_ONE;
            np++;
        }
    }
    if (scan_rc26 != SQLITE_DONE) { /* SCANCHK:26:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        for (int pi = 0; pi < np; pi++) {
            free(pnames[pi]);
        }
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);

    /* Sort by count desc */
    for (int i = SKIP_ONE; i < np; i++) {
        int j = i;
        while (j > 0 && pcounts[j] > pcounts[j - SKIP_ONE]) {
            int tc = pcounts[j];
            pcounts[j] = pcounts[j - SKIP_ONE];
            pcounts[j - SKIP_ONE] = tc;
            char *tn = pnames[j];
            pnames[j] = pnames[j - SKIP_ONE];
            pnames[j - SKIP_ONE] = tn;
            j--;
        }
    }
    if (np > MAX_PREVIEW_NAMES) {
        for (int i = MAX_PREVIEW_NAMES; i < np; i++) {
            free(pnames[i]);
        }
        np = MAX_PREVIEW_NAMES;
    }

    cbm_package_summary_t *arr = (np > 0) ? calloc(np, sizeof(cbm_package_summary_t)) : NULL;
    for (int i = 0; i < np; i++) {
        arr[i].name = pnames[i];
        arr[i].node_count = pcounts[i];
    }
    *out_arr = arr;
    *out_count = np;
    return CBM_STORE_OK;
}

static int arch_packages(cbm_store_t *s, const char *project, const char *path,
                         cbm_architecture_info_t *out) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    char sqlbuf[ST_SQL_BUF];
    const char *base = "SELECT n.name, COUNT(*) as cnt FROM nodes n "
                       "WHERE n.project=?1 AND n.label='Package'";
    if (scoped) {
        snprintf(sqlbuf, sizeof(sqlbuf),
                 "%s AND (n.file_path = ?2 OR n.file_path LIKE ?3) "
                 "GROUP BY n.name ORDER BY cnt DESC LIMIT 15",
                 base);
    } else {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s GROUP BY n.name ORDER BY cnt DESC LIMIT 15", base);
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sqlbuf, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "arch_packages");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);
    if (scoped) {
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    }

    int cap = ST_INIT_CAP_16;
    int n = 0;
    cbm_package_summary_t *arr = calloc(cap, sizeof(cbm_package_summary_t));
    int scan_rc27;
    while ((scan_rc27 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            arr = safe_realloc(arr, cap * sizeof(cbm_package_summary_t));
        }
        arr[n].name = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
        arr[n].node_count = sqlite3_column_int(stmt, SKIP_ONE);
        n++;
    }
    if (scan_rc27 != SQLITE_DONE) { /* SCANCHK:27:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        out->packages = arr;
        out->package_count = n;
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);

    /* Fallback: group by QN segment if no Package nodes */
    if (n == 0) {
        free(arr);
        int rc = arch_packages_from_qn(s, project, path, &arr, &n);
        if (rc != CBM_STORE_OK) {
            return rc;
        }
    }

    out->packages = arr;
    out->package_count = n;
    return CBM_STORE_OK;
}

static void classify_layer(const char *pkg, int in, int out_deg, bool has_routes,
                           bool has_entry_points, const char **layer, const char **reason) {
    static CBM_TLS char reason_buf[CBM_SZ_128];
    if (has_entry_points && out_deg > 0 && in == 0) {
        *layer = "entry";
        *reason = "has entry points, only outbound calls";
        return;
    }
    if (has_routes) {
        *layer = "api";
        *reason = "has HTTP route definitions";
        return;
    }
    if (in > out_deg && in > ST_MIN_INDEGREE) {
        snprintf(reason_buf, sizeof(reason_buf), "high fan-in (%d in, %d out)", in, out_deg);
        *layer = "core";
        *reason = reason_buf;
        return;
    }
    if (out_deg == 0 && in > 0) {
        *layer = "leaf";
        *reason = "only inbound calls, no outbound";
        return;
    }
    if (in == 0 && out_deg > 0) {
        *layer = "entry";
        *reason = "only outbound calls";
        return;
    }
    snprintf(reason_buf, sizeof(reason_buf), "fan-in=%d, fan-out=%d", in, out_deg);
    *layer = "internal";
    *reason = reason_buf;
    (void)pkg;
}

/* Find or insert a package name, returning its index. Returns -1 if full. */
static int find_or_add_pkg(char **all_pkgs, int *npkgs, int max_pkgs, const char *pkg) {
    for (int j = 0; j < *npkgs; j++) {
        if (strcmp(all_pkgs[j], pkg) == 0) {
            return j;
        }
    }
    if (*npkgs < max_pkgs) {
        int idx = *npkgs;
        all_pkgs[idx] = heap_strdup(pkg);
        (*npkgs)++;
        return idx;
    }
    return CBM_NOT_FOUND;
}

/* Check if a package name appears in an array. */
static bool pkg_in_list(const char *pkg, char **list, int count) {
    for (int j = 0; j < count; j++) {
        if (strcmp(pkg, list[j]) == 0) {
            return true;
        }
    }
    return false;
}

/* Collect package names from nodes matching a SQL query (must use ?1 = project). */
static int collect_pkg_names(cbm_store_t *s, const char *sql, const char *project, const char *path,
                             char **pkgs, int max_pkgs) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    char sqlbuf[ST_SQL_BUF];
    if (scoped) {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s%s", sql, arch_path_scope_sql());
    } else {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s", sql);
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sqlbuf, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK || !stmt) {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
        return CBM_NOT_FOUND;
    }
    bind_text(stmt, SKIP_ONE, project);
    if (scoped) {
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    }
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_pkgs) {
        const char *qn = (const char *)sqlite3_column_text(stmt, 0);
        pkgs[count++] = heap_strdup(cbm_qn_to_package(qn));
    }
    sqlite3_finalize(stmt);
    return count;
}

static int arch_layers(cbm_store_t *s, const char *project, const char *path,
                       cbm_architecture_info_t *out) {
    /* Get boundaries for fan analysis */
    cbm_cross_pkg_boundary_t *boundaries = NULL;
    int bcount = 0;
    int rc = arch_boundaries(s, project, path, &boundaries, &bcount);
    if (rc != CBM_STORE_OK) {
        return rc;
    }

    /* Collect route and entry point packages */
    char *route_pkgs[CBM_SZ_32];
    int nrpkgs =
        collect_pkg_names(s, "SELECT qualified_name FROM nodes WHERE project=?1 AND label='Route'",
                          project, path, route_pkgs, CBM_SZ_32);

    char *entry_pkgs[CBM_SZ_32];
    int nepkgs = collect_pkg_names(s,
                                   "SELECT qualified_name FROM nodes WHERE project=?1 AND "
                                   "json_extract(properties, '$.is_entry_point') = 1",
                                   project, path, entry_pkgs, CBM_SZ_32);

    /* Compute fan-in/out per package */
    char *all_pkgs[CBM_SZ_64];
    int fan_in[CBM_SZ_64];
    int fan_out[CBM_SZ_64];
    int npkgs = 0;
    memset(fan_in, 0, sizeof(fan_in));
    memset(fan_out, 0, sizeof(fan_out));

    for (int i = 0; i < bcount; i++) {
        int fi = find_or_add_pkg(all_pkgs, &npkgs, ST_MAX_PKGS, boundaries[i].from);
        if (fi >= 0) {
            fan_out[fi] += boundaries[i].call_count;
        }
        int ti = find_or_add_pkg(all_pkgs, &npkgs, ST_MAX_PKGS, boundaries[i].to);
        if (ti >= 0) {
            fan_in[ti] += boundaries[i].call_count;
        }
    }

    /* Also include route/entry packages */
    for (int i = 0; i < nrpkgs; i++) {
        find_or_add_pkg(all_pkgs, &npkgs, ST_MAX_PKGS, route_pkgs[i]);
    }
    for (int i = 0; i < nepkgs; i++) {
        find_or_add_pkg(all_pkgs, &npkgs, ST_MAX_PKGS, entry_pkgs[i]);
    }

    /* Classify each package */
    out->layers = (npkgs > 0) ? calloc(npkgs, sizeof(cbm_package_layer_t)) : NULL;
    out->layer_count = npkgs;
    for (int i = 0; i < npkgs; i++) {
        bool has_route = pkg_in_list(all_pkgs[i], route_pkgs, nrpkgs);
        bool has_entry = pkg_in_list(all_pkgs[i], entry_pkgs, nepkgs);
        const char *layer;
        const char *reason;
        classify_layer(all_pkgs[i], fan_in[i], fan_out[i], has_route, has_entry, &layer, &reason);
        out->layers[i].name = all_pkgs[i]; /* transfer ownership */
        out->layers[i].layer = heap_strdup(layer);
        out->layers[i].reason = heap_strdup(reason);
    }

    /* Sort layers by name */
    for (int i = SKIP_ONE; i < npkgs; i++) {
        int j = i;
        while (j > 0 && strcmp(out->layers[j].name, out->layers[j - SKIP_ONE].name) < 0) {
            cbm_package_layer_t tmp = out->layers[j];
            out->layers[j] = out->layers[j - SKIP_ONE];
            out->layers[j - SKIP_ONE] = tmp;
            j--;
        }
    }

    /* Cleanup */
    for (int i = 0; i < bcount; i++) {
        safe_str_free(&boundaries[i].from);
        safe_str_free(&boundaries[i].to);
    }
    free(boundaries);
    for (int i = 0; i < nrpkgs; i++) {
        free(route_pkgs[i]);
    }
    for (int i = 0; i < nepkgs; i++) {
        free(entry_pkgs[i]);
    }

    return CBM_STORE_OK;
}

/* Add a child to a dir entry if not already present. */
static void dir_add_child(char ***children, int *child_count, int *child_cap, const char *child) {
    for (int k = 0; k < *child_count; k++) {
        if (strcmp((*children)[k], child) == 0) {
            return;
        }
    }
    if (*child_count >= *child_cap) {
        *child_cap = *child_cap ? *child_cap * PAIR_LEN : ST_INIT_CAP_4;
        *children = realloc(*children, *child_cap * sizeof(char *));
    }
    (*children)[(*child_count)++] = heap_strdup(child);
}

/* Find or create a directory entry by path. Returns its index, or -1 if full. */
static int dir_find_or_create(char **dir_paths, int *dir_child_counts, char ***dir_children,
                              int *dir_children_caps, int *dn, int dcap, const char *dir) {
    for (int i = 0; i < *dn; i++) {
        if (strcmp(dir_paths[i], dir) == 0) {
            return i;
        }
    }
    if (*dn < dcap) {
        int idx = *dn;
        dir_paths[idx] = heap_strdup(dir);
        dir_child_counts[idx] = 0;
        dir_children[idx] = NULL;
        dir_children_caps[idx] = 0;
        (*dn)++;
        return idx;
    }
    return CBM_NOT_FOUND;
}

/* Create a file tree entry by checking if a path is a file and counting dir children. */
static cbm_file_tree_entry_t make_tree_entry(const char *path, char **files, int fn,
                                             char **dir_paths, const int *dir_child_counts,
                                             int dn) {
    cbm_file_tree_entry_t e = {0};
    e.path = heap_strdup(path);
    bool is_file = false;
    for (int f = 0; f < fn; f++) {
        if (strcmp(files[f], path) == 0) {
            is_file = true;
            break;
        }
    }
    e.type = heap_strdup(is_file ? "file" : "dir");
    for (int d = 0; d < dn; d++) {
        if (strcmp(dir_paths[d], path) == 0) {
            e.children = dir_child_counts[d];
            break;
        }
    }
    return e;
}

/* Split a path by '/' into parts. Returns number of parts. */
static int split_path_parts(const char *fp, char *buf, int buf_sz, char **parts, int max_parts) {
    strncpy(buf, fp, buf_sz - SKIP_ONE);
    buf[buf_sz - SKIP_ONE] = '\0';
    int nparts = 0;
    char *p = buf;
    parts[nparts++] = p;
    while (*p && nparts < max_parts) {
        if (*p == '/') {
            *p = '\0';
            parts[nparts++] = p + SKIP_ONE;
        }
        p++;
    }
    return nparts;
}

/* Register dir hierarchy for one file path. */
static void arch_register_file_dirs(const char *fp, char **dir_paths, int *dir_child_counts,
                                    char ***dir_children, int *dir_children_caps, int *dn,
                                    int dcap) {
    char tmp[CBM_SZ_512];
    char *parts[ST_SEARCH_MAX_BINDS];
    int nparts = split_path_parts(fp, tmp, (int)sizeof(tmp), parts, ST_SEARCH_MAX_BINDS);

    int ri = dir_find_or_create(dir_paths, dir_child_counts, dir_children, dir_children_caps, dn,
                                dcap, "");
    if (ri >= 0 && nparts > 0) {
        dir_add_child(&dir_children[ri], &dir_child_counts[ri], &dir_children_caps[ri], parts[0]);
    }

    for (int depth = 0; depth < nparts - SKIP_ONE && depth < ST_MAX_PATH_DEPTH; depth++) {
        char dir[CBM_SZ_512] = "";
        for (int k = 0; k <= depth; k++) {
            if (k > 0) {
                strcat(dir, "/");
            }
            strcat(dir, parts[k]);
        }
        const char *child = (depth + SKIP_ONE < nparts) ? parts[depth + SKIP_ONE] : NULL;
        if (!child) {
            continue;
        }
        int di = dir_find_or_create(dir_paths, dir_child_counts, dir_children, dir_children_caps,
                                    dn, dcap, dir);
        if (di >= 0) {
            dir_add_child(&dir_children[di], &dir_child_counts[di], &dir_children_caps[di], child);
        }
    }
}

/* Count the number of '/' in a string. */
static int count_slashes(const char *s) {
    int n = 0;
    for (; *s; s++) {
        if (*s == '/') {
            n++;
        }
    }
    return n;
}

/* Push a tree entry, growing the array if needed. */
static void push_tree_entry(cbm_file_tree_entry_t **entries, int *en, int *ecap,
                            cbm_file_tree_entry_t e) {
    if (*en >= *ecap) {
        *ecap *= ST_GROWTH;
        *entries = safe_realloc(*entries, *ecap * sizeof(cbm_file_tree_entry_t));
    }
    (*entries)[(*en)++] = e;
}

/* Collect tree entries from dir arrays. */
static void arch_collect_entries(char **dir_paths, int *dir_child_counts, char ***dir_children,
                                 int dn, char **files, int fn, cbm_file_tree_entry_t **entries_out,
                                 int *en_out) {
    int ecap = CBM_SZ_64;
    int en = 0;
    cbm_file_tree_entry_t *entries = calloc(ecap, sizeof(cbm_file_tree_entry_t));

    /* Root children */
    for (int i = 0; i < dn; i++) {
        if (strcmp(dir_paths[i], "") != 0) {
            continue;
        }
        for (int k = 0; k < dir_child_counts[i]; k++) {
            push_tree_entry(
                &entries, &en, &ecap,
                make_tree_entry(dir_children[i][k], files, fn, dir_paths, dir_child_counts, dn));
        }
    }

    /* Non-root dir children (depth < ST_COL_3) */
    for (int i = 0; i < dn; i++) {
        if (strcmp(dir_paths[i], "") == 0 || count_slashes(dir_paths[i]) >= ST_MAX_PATH_DEPTH) {
            continue;
        }
        for (int k = 0; k < dir_child_counts[i]; k++) {
            char path[CBM_SZ_512];
            snprintf(path, sizeof(path), "%s/%s", dir_paths[i], dir_children[i][k]);
            push_tree_entry(&entries, &en, &ecap,
                            make_tree_entry(path, files, fn, dir_paths, dir_child_counts, dn));
        }
    }

    /* Sort by path */
    for (int i = SKIP_ONE; i < en; i++) {
        int j = i;
        while (j > 0 && strcmp(entries[j].path, entries[j - SKIP_ONE].path) < 0) {
            cbm_file_tree_entry_t tmp = entries[j];
            entries[j] = entries[j - SKIP_ONE];
            entries[j - SKIP_ONE] = tmp;
            j--;
        }
    }

    *entries_out = entries;
    *en_out = en;
}

/* Free dir arrays. */
static void arch_free_dirs(char **dir_paths, int *dir_child_counts, char ***dir_children,
                           int *dir_children_caps, int dn, char **files, int fn) {
    for (int i = 0; i < dn; i++) {
        free(dir_paths[i]);
        for (int k = 0; k < dir_child_counts[i]; k++) {
            free(dir_children[i][k]);
        }
        free(dir_children[i]);
    }
    free(dir_paths);
    free(dir_child_counts);
    free(dir_children);
    free(dir_children_caps);
    for (int i = 0; i < fn; i++) {
        free(files[i]);
    }
    free(files);
}

static int arch_file_tree(cbm_store_t *s, const char *project, const char *path,
                          cbm_architecture_info_t *out) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    char sqlbuf[ST_SQL_BUF];
    const char *base = "SELECT file_path FROM nodes WHERE project=?1 AND label='File'";
    if (scoped) {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s%s", base, arch_path_scope_sql());
    } else {
        snprintf(sqlbuf, sizeof(sqlbuf), "%s", base);
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sqlbuf, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "arch_file_tree");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);
    if (scoped) {
        arch_bind_path_scope(stmt, ST_COL_2, ST_COL_3, norm, like);
    }

    int fcap = CBM_SZ_32;
    int fn = 0;
    char **files = malloc(fcap * sizeof(char *));

    int dcap = CBM_SZ_64;
    int dn = 0;
    char **dir_paths = calloc(dcap, sizeof(char *));
    int *dir_child_counts = calloc(dcap, sizeof(int));
    char ***dir_children = calloc(dcap, sizeof(char **));
    int *dir_children_caps = calloc(dcap, sizeof(int));

    int scan_rc28;
    while ((scan_rc28 = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *fp = (const char *)sqlite3_column_text(stmt, 0);
        if (!fp) {
            continue;
        }
        if (fn >= fcap) {
            fcap *= ST_GROWTH;
            files = safe_realloc(files, fcap * sizeof(char *));
        }
        files[fn++] = heap_strdup(fp);
        arch_register_file_dirs(fp, dir_paths, dir_child_counts, dir_children, dir_children_caps,
                                &dn, dcap);
    }
    if (scan_rc28 != SQLITE_DONE) { /* SCANCHK:28:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        arch_free_dirs(dir_paths, dir_child_counts, dir_children, dir_children_caps, dn, files, fn);
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);

    arch_collect_entries(dir_paths, dir_child_counts, dir_children, dn, files, fn, &out->file_tree,
                         &out->file_tree_count);

    arch_free_dirs(dir_paths, dir_child_counts, dir_children, dir_children_caps, dn, files, fn);
    return CBM_STORE_OK;
}

/* ── Louvain community detection ───────────────────────────────── */

/* Build deduplicated, normalized edge weight arrays from raw edges.
 * Returns total number of unique edges in *out_wn. */
/* Find the index of a node ID in the nodes array, or -1. */
static int louvain_node_index(const int64_t *nodes, int n, int64_t id) {
    for (int i = 0; i < n; i++) {
        if (nodes[i] == id) {
            return i;
        }
    }
    return CBM_NOT_FOUND;
}

static void louvain_build_weights(const int64_t *nodes, int n, const cbm_louvain_edge_t *edges,
                                  int edge_count, int **out_wsi, int **out_wdi, double **out_ww,
                                  int *out_wn) {
    int wcap = edge_count > 0 ? edge_count : SKIP_ONE;
    int wn = 0;
    int *wsi = malloc(wcap * sizeof(int));
    int *wdi = malloc(wcap * sizeof(int));
    double *ww = malloc(wcap * sizeof(double));

    for (int e = 0; e < edge_count; e++) {
        int si = louvain_node_index(nodes, n, edges[e].src);
        int di = louvain_node_index(nodes, n, edges[e].dst);
        if (si < 0 || di < 0 || si == di) {
            continue;
        }
        if (si > di) {
            int tmp = si;
            si = di;
            di = tmp;
        }
        int found = ST_FOUND;
        for (int i = 0; i < wn; i++) {
            if (wsi[i] == si && wdi[i] == di) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            ww[found] += (double)SKIP_ONE;
        } else {
            if (wn >= wcap) {
                wcap *= ST_GROWTH;
                wsi = safe_realloc(wsi, wcap * sizeof(int));
                wdi = safe_realloc(wdi, wcap * sizeof(int));
                ww = safe_realloc(ww, wcap * sizeof(double));
            }
            wsi[wn] = si;
            wdi[wn] = di;
            ww[wn] = (double)SKIP_ONE;
            wn++;
        }
    }
    *out_wsi = wsi;
    *out_wdi = wdi;
    *out_ww = ww;
    *out_wn = wn;
}

enum { LEIDEN_MAX_LEVELS = 64, LEIDEN_MOVE_PASS_CAP = 100 };

/* Weighted undirected graph in CSR form. Each undirected edge is stored as
 * two directed entries; k[i] is the weighted degree of node i. Self-loops are
 * never materialised — an aggregate node's intra-community weight is folded
 * into k[i] (which is preserved across levels), while nbr/w hold only
 * inter-community edges. The modularity gain therefore uses k[] for the
 * null-model term and nbr/w for connectivity, so intra weight affects the
 * degree but is never mistaken for an edge to another community. */
typedef struct {
    int n;
    int *off;  /* CSR offsets, length n + 1 */
    int *nbr;  /* neighbour indices, length off[n] */
    double *w; /* edge weights aligned with nbr */
    double *k; /* weighted degree per node */
} cbm_lg_t;

static void lg_free(cbm_lg_t *g) {
    free(g->off);
    free(g->nbr);
    free(g->w);
    free(g->k);
    g->off = NULL;
    g->nbr = NULL;
    g->w = NULL;
    g->k = NULL;
}

/* Build a CSR graph from a deduplicated undirected edge list. */
static int lg_build(int n, const int *wsi, const int *wdi, const double *ww, int wn,
                    cbm_lg_t *out) {
    int *off = calloc((size_t)n + 1, sizeof(int));
    double *k = calloc((size_t)n, sizeof(double));
    int *fill = malloc((size_t)n * sizeof(int));
    if (!off || !k || !fill) {
        free(off);
        free(k);
        free(fill);
        return CBM_NOT_FOUND;
    }
    for (int e = 0; e < wn; e++) {
        off[wsi[e] + 1]++;
        off[wdi[e] + 1]++;
    }
    for (int i = 0; i < n; i++) {
        off[i + 1] += off[i];
    }
    int total = off[n];
    int *nbr = malloc((size_t)(total > 0 ? total : 1) * sizeof(int));
    double *w = malloc((size_t)(total > 0 ? total : 1) * sizeof(double));
    if (!nbr || !w) {
        free(off);
        free(k);
        free(fill);
        free(nbr);
        free(w);
        return CBM_NOT_FOUND;
    }
    memcpy(fill, off, (size_t)n * sizeof(int));
    for (int e = 0; e < wn; e++) {
        int a = wsi[e];
        int b = wdi[e];
        double we = ww[e];
        nbr[fill[a]] = b;
        w[fill[a]] = we;
        fill[a]++;
        nbr[fill[b]] = a;
        w[fill[b]] = we;
        fill[b]++;
        k[a] += we;
        k[b] += we;
    }
    free(fill);
    out->n = n;
    out->off = off;
    out->nbr = nbr;
    out->w = w;
    out->k = k;
    return CBM_STORE_OK;
}

/* Local-moving phase: greedily move each node to the neighbouring community
 * with the highest modularity gain, using a work queue seeded with every node
 * and re-queueing only the neighbours of a node that actually moved. Mutates
 * comm[] in place. */
static void leiden_move(const cbm_lg_t *g, int *comm, double gamma, double twom) {
    int n = g->n;
    double *stot = calloc((size_t)n, sizeof(double));
    double *acc = calloc((size_t)n, sizeof(double));
    int *queue = malloc((size_t)n * sizeof(int));
    int *dirty = malloc((size_t)n * sizeof(int));
    bool *inq = calloc((size_t)n, sizeof(bool));
    if (!stot || !acc || !queue || !dirty || !inq) {
        free(stot);
        free(acc);
        free(queue);
        free(dirty);
        free(inq);
        return;
    }
    for (int i = 0; i < n; i++) {
        stot[comm[i]] += g->k[i];
        queue[i] = i;
        inq[i] = true;
    }
    int qhead = 0;
    int qcount = n;
    long cap = (long)n * LEIDEN_MOVE_PASS_CAP + LEIDEN_MAX_LEVELS;
    while (qcount > 0 && cap-- > 0) {
        int v = queue[qhead];
        qhead = (qhead + 1) % n;
        qcount--;
        inq[v] = false;
        int cv = comm[v];
        int ndirty = 0;
        for (int e = g->off[v]; e < g->off[v + 1]; e++) {
            int u = g->nbr[e];
            if (u == v) {
                continue;
            }
            int cu = comm[u];
            if (acc[cu] == 0.0) {
                dirty[ndirty++] = cu;
            }
            acc[cu] += g->w[e];
        }
        stot[cv] -= g->k[v];
        double kv = g->k[v];
        int best_c = cv;
        double best_gain = acc[cv] - gamma * kv * stot[cv] / twom;
        for (int d = 0; d < ndirty; d++) {
            int c = dirty[d];
            double gain = acc[c] - gamma * kv * stot[c] / twom;
            if (gain > best_gain) {
                best_gain = gain;
                best_c = c;
            }
        }
        stot[best_c] += kv;
        comm[v] = best_c;
        if (best_c != cv) {
            for (int e = g->off[v]; e < g->off[v + 1]; e++) {
                int u = g->nbr[e];
                if (comm[u] != best_c && !inq[u] && qcount < n) {
                    queue[(qhead + qcount) % n] = u;
                    qcount++;
                    inq[u] = true;
                }
            }
        }
        for (int d = 0; d < ndirty; d++) {
            acc[dirty[d]] = 0.0;
        }
    }
    free(stot);
    free(acc);
    free(queue);
    free(dirty);
    free(inq);
}

/* Compact community labels in comm[] to the dense range [0, returned count). */
static int leiden_relabel(int *comm, int n) {
    int *map = malloc((size_t)n * sizeof(int));
    if (!map) {
        return n;
    }
    for (int i = 0; i < n; i++) {
        map[i] = CBM_NOT_FOUND;
    }
    int next = 0;
    for (int i = 0; i < n; i++) {
        int c = comm[i];
        if (map[c] == CBM_NOT_FOUND) {
            map[c] = next++;
        }
        comm[i] = map[c];
    }
    free(map);
    return next;
}

/* Refinement phase: within each move-phase community, merge singleton nodes
 * into the best connected sub-community (positive modularity gain, edge must
 * exist). This re-derives communities bottom-up so each one is guaranteed
 * internally connected — the defect single-level Louvain suffers from, which
 * fragments the graph into hundreds of tiny noisy clusters. Writes
 * sub-community labels into refined[] and returns their count. */
static int leiden_refine(const cbm_lg_t *g, const int *comm, double gamma, double twom,
                         int *refined) {
    int n = g->n;
    double *stot = calloc((size_t)n, sizeof(double));
    double *acc = calloc((size_t)n, sizeof(double));
    int *rsize = malloc((size_t)n * sizeof(int));
    int *dirty = malloc((size_t)n * sizeof(int));
    if (!stot || !acc || !rsize || !dirty) {
        free(stot);
        free(acc);
        free(rsize);
        free(dirty);
        for (int i = 0; i < n; i++) {
            refined[i] = i;
        }
        return leiden_relabel(refined, n);
    }
    for (int i = 0; i < n; i++) {
        refined[i] = i;
        stot[i] = g->k[i];
        rsize[i] = 1;
    }
    for (int v = 0; v < n; v++) {
        if (rsize[refined[v]] != 1) {
            continue; /* only singletons merge, per the refinement rule */
        }
        int cv = comm[v];
        int ndirty = 0;
        for (int e = g->off[v]; e < g->off[v + 1]; e++) {
            int u = g->nbr[e];
            if (u == v || comm[u] != cv) {
                continue; /* stay within the move-phase community */
            }
            int ru = refined[u];
            if (acc[ru] == 0.0) {
                dirty[ndirty++] = ru;
            }
            acc[ru] += g->w[e];
        }
        int rv = refined[v];
        double kv = g->k[v];
        stot[rv] -= kv;
        int best_r = rv;
        double best_gain = 0.0;
        for (int d = 0; d < ndirty; d++) {
            int r = dirty[d];
            if (r == rv) {
                continue;
            }
            double gain = acc[r] - gamma * kv * stot[r] / twom;
            if (gain > best_gain) {
                best_gain = gain;
                best_r = r;
            }
        }
        if (best_r != rv) {
            refined[v] = best_r;
            stot[best_r] += kv;
            rsize[best_r]++;
            rsize[rv]--;
        } else {
            stot[rv] += kv;
        }
        for (int d = 0; d < ndirty; d++) {
            acc[dirty[d]] = 0.0;
        }
    }
    free(stot);
    free(acc);
    free(rsize);
    free(dirty);
    return leiden_relabel(refined, n);
}

/* Aggregation phase: collapse each refined sub-community into a single node.
 * Builds the coarser graph in *out and records, for each aggregate node, the
 * move-phase community it belonged to in seed[] so the next level starts from
 * the coarse structure rather than from singletons. */
static int leiden_aggregate(const cbm_lg_t *g, const int *refined, int r_count, const int *comm,
                            cbm_lg_t *out, int *seed) {
    int n = g->n;
    double *k2 = calloc((size_t)r_count, sizeof(double));
    int *gcount = calloc((size_t)r_count, sizeof(int));
    int *gstart = malloc(((size_t)r_count + 1) * sizeof(int));
    int *members = malloc((size_t)n * sizeof(int));
    int *fill = malloc((size_t)r_count * sizeof(int));
    double *acc = calloc((size_t)r_count, sizeof(double));
    int *dirty = malloc((size_t)r_count * sizeof(int));
    int *off2 = malloc(((size_t)r_count + 1) * sizeof(int));
    if (!k2 || !gcount || !gstart || !members || !fill || !acc || !dirty || !off2) {
        free(k2);
        free(gcount);
        free(gstart);
        free(members);
        free(fill);
        free(acc);
        free(dirty);
        free(off2);
        return CBM_NOT_FOUND;
    }
    for (int r = 0; r < r_count; r++) {
        seed[r] = CBM_NOT_FOUND;
    }
    for (int i = 0; i < n; i++) {
        int r = refined[i];
        k2[r] += g->k[i];
        gcount[r]++;
        if (seed[r] == CBM_NOT_FOUND) {
            seed[r] = comm[i];
        }
    }
    gstart[0] = 0;
    for (int r = 0; r < r_count; r++) {
        gstart[r + 1] = gstart[r] + gcount[r];
        fill[r] = gstart[r];
    }
    for (int i = 0; i < n; i++) {
        members[fill[refined[i]]++] = i;
    }
    /* Pass 1: count distinct inter-community neighbours per aggregate node. */
    off2[0] = 0;
    for (int r = 0; r < r_count; r++) {
        int nd = 0;
        for (int m = gstart[r]; m < gstart[r + 1]; m++) {
            int i = members[m];
            for (int e = g->off[i]; e < g->off[i + 1]; e++) {
                int rb = refined[g->nbr[e]];
                if (rb == r || acc[rb] != 0.0) {
                    continue;
                }
                acc[rb] = 1.0;
                dirty[nd++] = rb;
            }
        }
        off2[r + 1] = off2[r] + nd;
        for (int d = 0; d < nd; d++) {
            acc[dirty[d]] = 0.0;
        }
    }
    int total = off2[r_count];
    int *nbr2 = malloc((size_t)(total > 0 ? total : 1) * sizeof(int));
    double *w2 = malloc((size_t)(total > 0 ? total : 1) * sizeof(double));
    if (!nbr2 || !w2) {
        free(k2);
        free(gcount);
        free(gstart);
        free(members);
        free(fill);
        free(acc);
        free(dirty);
        free(off2);
        free(nbr2);
        free(w2);
        return CBM_NOT_FOUND;
    }
    /* Pass 2: accumulate inter-community edge weights. */
    for (int r = 0; r < r_count; r++) {
        int nd = 0;
        for (int m = gstart[r]; m < gstart[r + 1]; m++) {
            int i = members[m];
            for (int e = g->off[i]; e < g->off[i + 1]; e++) {
                int rb = refined[g->nbr[e]];
                if (rb == r) {
                    continue;
                }
                if (acc[rb] == 0.0) {
                    dirty[nd++] = rb;
                }
                acc[rb] += g->w[e];
            }
        }
        int base = off2[r];
        for (int d = 0; d < nd; d++) {
            nbr2[base + d] = dirty[d];
            w2[base + d] = acc[dirty[d]];
            acc[dirty[d]] = 0.0;
        }
    }
    free(gcount);
    free(gstart);
    free(members);
    free(fill);
    free(acc);
    free(dirty);
    out->n = r_count;
    out->off = off2;
    out->nbr = nbr2;
    out->w = w2;
    out->k = k2;
    return CBM_STORE_OK;
}

int cbm_leiden(const int64_t *nodes, int node_count, const cbm_louvain_edge_t *edges,
               int edge_count, double resolution, cbm_louvain_result_t **out, int *out_count) {
    if (node_count <= 0) {
        *out = NULL;
        *out_count = 0;
        return CBM_STORE_OK;
    }
    int n = node_count;
    double gamma = resolution > 0.0 ? resolution : 1.0;

    cbm_louvain_result_t *result = malloc((size_t)n * sizeof(*result));
    if (!result) {
        return CBM_NOT_FOUND;
    }
    for (int i = 0; i < n; i++) {
        result[i].node_id = nodes[i];
        result[i].community = i;
    }

    /* Build deduplicated undirected edge weights, then a CSR graph. */
    int *wsi;
    int *wdi;
    double *ww;
    int wn;
    louvain_build_weights(nodes, n, edges, edge_count, &wsi, &wdi, &ww, &wn);
    cbm_lg_t g;
    int built = (wn > 0) ? lg_build(n, wsi, wdi, ww, wn, &g) : CBM_NOT_FOUND;
    free(wsi);
    free(wdi);
    free(ww);
    if (built != CBM_STORE_OK) {
        /* No edges (or allocation failure): every node is its own community. */
        *out = result;
        *out_count = n;
        return CBM_STORE_OK;
    }

    double twom = 0.0;
    for (int i = 0; i < n; i++) {
        twom += g.k[i];
    }

    int *orig = malloc((size_t)n * sizeof(int)); /* original node -> current graph node */
    int *comm = malloc((size_t)n * sizeof(int));
    if (twom <= 0.0 || !orig || !comm) {
        free(orig);
        free(comm);
        lg_free(&g);
        *out = result;
        *out_count = n;
        return CBM_STORE_OK;
    }
    for (int i = 0; i < n; i++) {
        orig[i] = i;
        comm[i] = i;
    }

    for (int level = 0; level < LEIDEN_MAX_LEVELS; level++) {
        leiden_move(&g, comm, gamma, twom);
        int c_count = leiden_relabel(comm, g.n);
        if (c_count >= g.n) {
            break; /* every node already isolated — nothing to coarsen */
        }
        int *refined = malloc((size_t)g.n * sizeof(int));
        if (!refined) {
            break;
        }
        int r_count = leiden_refine(&g, comm, gamma, twom, refined);
        if (r_count >= g.n) {
            free(refined);
            break; /* refinement cannot reduce the graph further */
        }
        for (int i = 0; i < n; i++) {
            orig[i] = refined[orig[i]];
        }
        cbm_lg_t g2;
        int *seed = malloc((size_t)r_count * sizeof(int));
        if (!seed || leiden_aggregate(&g, refined, r_count, comm, &g2, seed) != CBM_STORE_OK) {
            free(seed);
            free(refined);
            break;
        }
        free(refined);
        lg_free(&g);
        g = g2;
        free(comm);
        comm = seed;
    }

    for (int i = 0; i < n; i++) {
        result[i].community = comm[orig[i]];
    }
    free(comm);
    free(orig);
    lg_free(&g);
    *out = result;
    *out_count = n;
    return CBM_STORE_OK;
}

int cbm_louvain(const int64_t *nodes, int node_count, const cbm_louvain_edge_t *edges,
                int edge_count, cbm_louvain_result_t **out, int *out_count) {
    return cbm_leiden(nodes, node_count, edges, edge_count, 1.0, out, out_count);
}

/* ── Architecture: community clusters via Leiden ───────────────── */

enum {
    CBM_CLUSTER_TOP_N = 12,       /* report at most this many clusters */
    CBM_CLUSTER_MAX_TOPNODES = 5, /* representative node names per cluster */
    CBM_CLUSTER_MAX_PKGS = 5,     /* packages listed per cluster */
    CBM_CLUSTER_MIN_MEMBERS = 2,  /* skip singletons */
    CBM_CLUSTER_NODE_CAP = 8000   /* bound the work for very large graphs */
};

static int cluster_id_cmp(const void *key, const void *el) {
    int64_t k = *(const int64_t *)key;
    int64_t e = *(const int64_t *)el;
    return (k > e) - (k < e);
}

/* Index of node id within the id-sorted ids[], or CBM_NOT_FOUND. */
static int cluster_id_index(const int64_t *ids, int n, int64_t id) {
    const int64_t *hit = bsearch(&id, ids, (size_t)n, sizeof(int64_t), cluster_id_cmp);
    return hit ? (int)(hit - ids) : CBM_NOT_FOUND;
}

/* Append `pkg` to a distinct package list (with a per-package count). */
static void cluster_add_pkg(const char **pkgs, int *counts, int *count, int cap, const char *pkg) {
    if (!pkg || !pkg[0]) {
        return;
    }
    for (int i = 0; i < *count; i++) {
        if (strcmp(pkgs[i], pkg) == 0) {
            counts[i]++;
            return;
        }
    }
    if (*count < cap) {
        pkgs[*count] = heap_strdup(pkg);
        counts[*count] = 1;
        (*count)++;
    }
}

/* Build the cluster_info for one community c into *ci. */
static void cluster_build_one(cbm_cluster_info_t *ci, int c, int n, const int *comm,
                              const int *degree, const char **names, const char **qns, int members,
                              double cohesion) {
    memset(ci, 0, sizeof(*ci));
    ci->id = c;
    ci->members = members;
    ci->cohesion = cohesion;

    /* Top nodes by degree. */
    int top_idx[CBM_CLUSTER_MAX_TOPNODES];
    int top_deg[CBM_CLUSTER_MAX_TOPNODES];
    int tn = 0;
    for (int i = 0; i < n; i++) {
        if (comm[i] != c) {
            continue;
        }
        int d = degree[i];
        int pos = tn;
        while (pos > 0 && top_deg[pos - 1] < d) {
            pos--;
        }
        if (pos < CBM_CLUSTER_MAX_TOPNODES) {
            int last = (tn < CBM_CLUSTER_MAX_TOPNODES) ? tn : CBM_CLUSTER_MAX_TOPNODES - 1;
            for (int k = last; k > pos; k--) {
                top_idx[k] = top_idx[k - 1];
                top_deg[k] = top_deg[k - 1];
            }
            top_idx[pos] = i;
            top_deg[pos] = d;
            if (tn < CBM_CLUSTER_MAX_TOPNODES) {
                tn++;
            }
        }
    }
    if (tn > 0) {
        ci->top_nodes = malloc((size_t)tn * sizeof(char *));
        for (int i = 0; i < tn; i++) {
            ci->top_nodes[i] = heap_strdup(names[top_idx[i]]);
        }
        ci->top_node_count = tn;
    }

    /* Distinct packages (+ dominant one as the label). */
    const char *pkgs[CBM_CLUSTER_MAX_PKGS];
    int pkg_counts[CBM_CLUSTER_MAX_PKGS];
    int pc = 0;
    for (int i = 0; i < n; i++) {
        if (comm[i] == c) {
            cluster_add_pkg(pkgs, pkg_counts, &pc, CBM_CLUSTER_MAX_PKGS,
                            cbm_qn_to_top_package(qns[i]));
        }
    }
    if (pc > 0) {
        ci->packages = malloc((size_t)pc * sizeof(char *));
        int best = 0;
        for (int i = 0; i < pc; i++) {
            ci->packages[i] = heap_strdup(pkgs[i]);
            if (pkg_counts[i] > pkg_counts[best]) {
                best = i;
            }
        }
        ci->package_count = pc;
        ci->label = heap_strdup(pkgs[best]);
        for (int i = 0; i < pc; i++) {
            safe_str_free(&pkgs[i]);
        }
    }
    if (!ci->label) {
        ci->label = heap_strdup(ci->top_node_count > 0 ? ci->top_nodes[0] : "cluster");
    }

    ci->edge_types = malloc(sizeof(char *));
    ci->edge_types[0] = heap_strdup("CALLS");
    ci->edge_type_count = 1;
}

/* Comparator for sorting community indices by descending member count. */
typedef struct {
    int comm;
    int members;
} cluster_rank_t;
static int cluster_rank_cmp(const void *a, const void *b) {
    const cluster_rank_t *ca = a;
    const cluster_rank_t *cb = b;
    return cb->members - ca->members;
}

static int arch_clusters(cbm_store_t *s, const char *project, const char *path,
                         cbm_architecture_info_t *out) {
    char norm[CBM_SZ_512];
    char like[CBM_SZ_512];
    bool scoped = arch_path_prepare(path, norm, sizeof(norm), like, sizeof(like));
    char nsqlbuf[ST_SQL_BUF];
    const char *nbase = "SELECT id, name, qualified_name, file_path FROM nodes "
                        "WHERE project=?1 AND label IN ('Function','Method','Class')";
    if (scoped) {
        snprintf(nsqlbuf, sizeof(nsqlbuf), "%s%s ORDER BY id LIMIT ?4", nbase,
                 arch_path_scope_sql());
    } else {
        snprintf(nsqlbuf, sizeof(nsqlbuf), "%s ORDER BY id LIMIT ?2", nbase);
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, nsqlbuf, CBM_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        return CBM_STORE_OK; /* clusters are best-effort */
    }
    bind_text(st, SKIP_ONE, project);
    if (scoped) {
        arch_bind_path_scope(st, ST_COL_2, ST_COL_3, norm, like);
        sqlite3_bind_int(st, ST_COL_4, CBM_CLUSTER_NODE_CAP);
    } else {
        sqlite3_bind_int(st, CBM_SZ_2, CBM_CLUSTER_NODE_CAP);
    }
    int cap = ST_INIT_CAP_8;
    int n = 0;
    int64_t *ids = malloc((size_t)cap * sizeof(int64_t));
    const char **names = malloc((size_t)cap * sizeof(char *));
    const char **qns = malloc((size_t)cap * sizeof(char *));
    int scan_rc29;
    while ((scan_rc29 = sqlite3_step(st)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            ids = safe_realloc(ids, (size_t)cap * sizeof(int64_t));
            names = safe_realloc(names, (size_t)cap * sizeof(char *));
            qns = safe_realloc(qns, (size_t)cap * sizeof(char *));
        }
        ids[n] = sqlite3_column_int64(st, 0);
        names[n] = heap_strdup((const char *)sqlite3_column_text(st, SKIP_ONE));
        qns[n] = heap_strdup((const char *)sqlite3_column_text(st, CBM_SZ_2));
        n++;
    }
    if (scan_rc29 != SQLITE_DONE) { /* SCANCHK:29:st */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(st);
        for (int ci = 0; ci < n; ci++) {
            safe_str_free(&names[ci]);
            safe_str_free(&qns[ci]);
        }
        free(ids);
        free((void *)names);
        free((void *)qns);
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(st);
    if (n < CBM_CLUSTER_MIN_MEMBERS) {
        for (int i = 0; i < n; i++) {
            safe_str_free(&names[i]);
            safe_str_free(&qns[i]);
        }
        free(ids);
        free(names);
        free(qns);
        return CBM_STORE_OK;
    }

    /* 2. Load CALLS edges with both endpoints in the node set (store indices). */
    cbm_louvain_edge_t *edges = malloc((size_t)n * sizeof(cbm_louvain_edge_t));
    int *esrc = malloc((size_t)n * sizeof(int));
    int *edst = malloc((size_t)n * sizeof(int));
    int *degree = calloc((size_t)n, sizeof(int));
    int ne = 0;
    store_id_pair_t *calls = NULL;
    int call_count = 0;
    if (store_calls_pairs(s, project, &calls, &call_count) == CBM_STORE_OK) {
        int ecap = n;
        for (int c = 0; c < call_count; c++) {
            int si = cluster_id_index(ids, n, calls[c].source_id);
            int ti = cluster_id_index(ids, n, calls[c].target_id);
            if (si < 0 || ti < 0 || si == ti) {
                continue;
            }
            if (ne >= ecap) {
                ecap *= ST_GROWTH;
                edges = safe_realloc(edges, (size_t)ecap * sizeof(cbm_louvain_edge_t));
                esrc = safe_realloc(esrc, (size_t)ecap * sizeof(int));
                edst = safe_realloc(edst, (size_t)ecap * sizeof(int));
            }
            edges[ne].src = ids[si];
            edges[ne].dst = ids[ti];
            esrc[ne] = si;
            edst[ne] = ti;
            degree[si]++;
            degree[ti]++;
            ne++;
        }
        free(calls);
    }

    /* 3. Community detection. */
    cbm_louvain_result_t *res = NULL;
    int rn = 0;
    int *comm = NULL;
    int C = 0;
    if (cbm_leiden(ids, n, edges, ne, 1.0, &res, &rn) == CBM_STORE_OK && res && rn == n) {
        comm = malloc((size_t)n * sizeof(int));
        for (int i = 0; i < n; i++) {
            comm[i] = res[i].community;
            if (comm[i] + 1 > C) {
                C = comm[i] + 1;
            }
        }
    }
    free(res);

    if (comm && C > 0) {
        /* 4. Members + cohesion (internal vs boundary edges) per community. */
        int *members = calloc((size_t)C, sizeof(int));
        int *internal = calloc((size_t)C, sizeof(int));
        int *boundary = calloc((size_t)C, sizeof(int));
        for (int i = 0; i < n; i++) {
            members[comm[i]]++;
        }
        for (int e = 0; e < ne; e++) {
            int cs = comm[esrc[e]];
            int cd = comm[edst[e]];
            if (cs == cd) {
                internal[cs]++;
            } else {
                boundary[cs]++;
                boundary[cd]++;
            }
        }

        /* 5. Rank communities by size, take the top N non-singletons. */
        cluster_rank_t *rank = malloc((size_t)C * sizeof(cluster_rank_t));
        for (int c = 0; c < C; c++) {
            rank[c] = (cluster_rank_t){c, members[c]};
        }
        if (C > 1) {
            qsort(rank, (size_t)C, sizeof(cluster_rank_t), cluster_rank_cmp);
        }

        cbm_cluster_info_t *clusters =
            malloc((size_t)CBM_CLUSTER_TOP_N * sizeof(cbm_cluster_info_t));
        int cc = 0;
        for (int r = 0; r < C && cc < CBM_CLUSTER_TOP_N; r++) {
            int c = rank[r].comm;
            if (members[c] < CBM_CLUSTER_MIN_MEMBERS) {
                break; /* sorted desc — the rest are singletons too */
            }
            double denom = internal[c] + boundary[c];
            double cohesion = denom > 0 ? (double)internal[c] / denom : 0.0;
            cluster_build_one(&clusters[cc], c, n, comm, degree, names, qns, members[c], cohesion);
            cc++;
        }
        out->clusters = clusters;
        out->cluster_count = cc;

        free(members);
        free(internal);
        free(boundary);
        free(rank);
    }

    free(comm);
    for (int i = 0; i < n; i++) {
        safe_str_free(&names[i]);
        safe_str_free(&qns[i]);
    }
    free(ids);
    free(names);
    free(qns);
    free(edges);
    free(esrc);
    free(edst);
    free(degree);
    return CBM_STORE_OK;
}

/* ── GetArchitecture dispatch ──────────────────────────────────── */

/* "overview" = compact architecture summary: every aspect EXCEPT the large
 * per-file listing (file_tree), which alone dominates the payload on real
 * repos and can push the MCP response past the output cap. Declared in
 * store.h and shared with aspect_wanted in src/mcp/mcp.c so the store-side
 * DB gate and the MCP-side serialization gate cannot drift. */
bool cbm_store_arch_aspect_in_overview(const char *name) {
    return strcmp(name, "file_tree") != 0;
}

static bool want_aspect(const char **aspects, int aspect_count, const char *name) {
    if (!aspects || aspect_count == 0) {
        return true;
    }
    for (int i = 0; i < aspect_count; i++) {
        if (strcmp(aspects[i], "all") == 0) {
            return true;
        }
        if (strcmp(aspects[i], "overview") == 0 && cbm_store_arch_aspect_in_overview(name)) {
            return true;
        }
        if (strcmp(aspects[i], name) == 0) {
            return true;
        }
    }
    return false;
}

int cbm_store_get_architecture(cbm_store_t *s, const char *project, const char *path,
                               const char **aspects, int aspect_count,
                               cbm_architecture_info_t *out) {
    memset(out, 0, sizeof(*out));
    int rc = store_legacy_scope(s, &project);
    if (rc != CBM_STORE_OK) {
        return rc;
    }

    if (want_aspect(aspects, aspect_count, "languages")) {
        rc = arch_languages(s, project, path, out);
        if (rc != CBM_STORE_OK) {
            return rc;
        }
    }
    if (want_aspect(aspects, aspect_count, "packages")) {
        rc = arch_packages(s, project, path, out);
        if (rc != CBM_STORE_OK) {
            return rc;
        }
    }
    if (want_aspect(aspects, aspect_count, "entry_points")) {
        rc = arch_entry_points(s, project, path, out);
        if (rc != CBM_STORE_OK) {
            return rc;
        }
    }
    if (want_aspect(aspects, aspect_count, "routes")) {
        rc = arch_routes(s, project, path, out);
        if (rc != CBM_STORE_OK) {
            return rc;
        }
    }
    if (want_aspect(aspects, aspect_count, "hotspots")) {
        rc = arch_hotspots(s, project, path, out);
        if (rc != CBM_STORE_OK) {
            return rc;
        }
    }
    if (want_aspect(aspects, aspect_count, "boundaries")) {
        cbm_cross_pkg_boundary_t *barr = NULL;
        int bcount = 0;
        rc = arch_boundaries(s, project, path, &barr, &bcount);
        if (rc != CBM_STORE_OK) {
            return rc;
        }
        out->boundaries = barr;
        out->boundary_count = bcount;
    }
    if (want_aspect(aspects, aspect_count, "layers")) {
        rc = arch_layers(s, project, path, out);
        if (rc != CBM_STORE_OK) {
            return rc;
        }
    }
    if (want_aspect(aspects, aspect_count, "file_tree")) {
        rc = arch_file_tree(s, project, path, out);
        if (rc != CBM_STORE_OK) {
            return rc;
        }
    }
    if (want_aspect(aspects, aspect_count, "clusters")) {
        rc = arch_clusters(s, project, path, out);
        if (rc != CBM_STORE_OK) {
            return rc;
        }
    }

    return CBM_STORE_OK;
}

void cbm_store_architecture_free(cbm_architecture_info_t *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < out->language_count; i++) {
        safe_str_free(&out->languages[i].language);
    }
    free(out->languages);
    for (int i = 0; i < out->package_count; i++) {
        safe_str_free(&out->packages[i].name);
    }
    free(out->packages);
    for (int i = 0; i < out->entry_point_count; i++) {
        safe_str_free(&out->entry_points[i].name);
        safe_str_free(&out->entry_points[i].qualified_name);
        safe_str_free(&out->entry_points[i].file);
    }
    free(out->entry_points);
    for (int i = 0; i < out->route_count; i++) {
        safe_str_free(&out->routes[i].method);
        safe_str_free(&out->routes[i].path);
        safe_str_free(&out->routes[i].handler);
    }
    free(out->routes);
    for (int i = 0; i < out->hotspot_count; i++) {
        safe_str_free(&out->hotspots[i].name);
        safe_str_free(&out->hotspots[i].qualified_name);
    }
    free(out->hotspots);
    for (int i = 0; i < out->boundary_count; i++) {
        safe_str_free(&out->boundaries[i].from);
        safe_str_free(&out->boundaries[i].to);
    }
    free(out->boundaries);
    for (int i = 0; i < out->service_count; i++) {
        safe_str_free(&out->services[i].from);
        safe_str_free(&out->services[i].to);
        safe_str_free(&out->services[i].type);
    }
    free(out->services);
    for (int i = 0; i < out->layer_count; i++) {
        safe_str_free(&out->layers[i].name);
        safe_str_free(&out->layers[i].layer);
        safe_str_free(&out->layers[i].reason);
    }
    free(out->layers);
    for (int i = 0; i < out->cluster_count; i++) {
        safe_str_free(&out->clusters[i].label);
        for (int j = 0; j < out->clusters[i].top_node_count; j++) {
            safe_str_free(&out->clusters[i].top_nodes[j]);
        }
        free(out->clusters[i].top_nodes);
        for (int j = 0; j < out->clusters[i].package_count; j++) {
            safe_str_free(&out->clusters[i].packages[j]);
        }
        free(out->clusters[i].packages);
        for (int j = 0; j < out->clusters[i].edge_type_count; j++) {
            safe_str_free(&out->clusters[i].edge_types[j]);
        }
        free(out->clusters[i].edge_types);
    }
    free(out->clusters);
    for (int i = 0; i < out->file_tree_count; i++) {
        safe_str_free(&out->file_tree[i].path);
        safe_str_free(&out->file_tree[i].type);
    }
    free(out->file_tree);
    memset(out, 0, sizeof(*out));
}

/* ── Project summaries ─────────────────────────────────────────── */

int cbm_store_project_summary_export(cbm_store_t *s, const char *project,
                                      cbm_project_summary_export_t *out) {
    if (!s || !project || !out) return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT summary, source_hash, created_at, updated_at "
                      "FROM project_summaries WHERE project=?1";
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "project_summary_export_prepare");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return CBM_STORE_NOT_FOUND;
    }
    if (step != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        store_set_error_sqlite(s, "project_summary_export_step");
        return CBM_STORE_ERR;
    }
    out->summary = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
    out->source_hash = heap_strdup((const char *)sqlite3_column_text(stmt, 1));
    out->created_at = heap_strdup((const char *)sqlite3_column_text(stmt, 2));
    out->updated_at = heap_strdup((const char *)sqlite3_column_text(stmt, 3));
    sqlite3_finalize(stmt);
    if (!out->summary || !out->source_hash || !out->created_at || !out->updated_at) {
        cbm_store_project_summary_export_free(out);
        store_set_error(s, "project summary export allocation failed");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

void cbm_store_project_summary_export_free(cbm_project_summary_export_t *row) {
    if (!row) return;
    free(row->summary);
    free(row->source_hash);
    free(row->created_at);
    free(row->updated_at);
    memset(row, 0, sizeof(*row));
}

/* ── Architecture doc discovery ────────────────────────────────── */

int cbm_store_find_architecture_docs(cbm_store_t *s, const char *project, char ***out, int *count) {
    const char *sql = "SELECT file_path FROM nodes WHERE project=?1 AND label='File' "
                      "AND (file_path LIKE '%ARCHITECTURE.md' OR file_path LIKE '%ADR.md' "
                      "OR file_path LIKE '%DECISIONS.md' OR file_path LIKE 'docs/adr/%' "
                      "OR file_path LIKE 'doc/adr/%' OR file_path LIKE 'adr/%') "
                      "ORDER BY file_path LIMIT 20";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "find_arch_docs");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, SKIP_ONE, project);

    int cap = ST_INIT_CAP_8;
    int n = 0;
    char **arr = malloc(cap * sizeof(char *));
    int scan_rc31;
    while ((scan_rc31 = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= ST_GROWTH;
            arr = safe_realloc(arr, cap * sizeof(char *));
        }
        arr[n++] = heap_strdup((const char *)sqlite3_column_text(stmt, 0));
    }
    if (scan_rc31 != SQLITE_DONE) { /* SCANCHK:31:stmt */
        store_set_error_sqlite(s, "row scan aborted");
        sqlite3_finalize(stmt);
        for (int di = 0; di < n; di++) {
            free(arr[di]);
        }
        free(arr);
        *out = NULL;
        *count = 0;
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    *out = arr;
    *count = n;
    return CBM_STORE_OK;
}

/* ── Memory management ──────────────────────────────────────────── */

void cbm_node_free_fields(cbm_node_t *n) {
    safe_str_free(&n->project);
    safe_str_free(&n->label);
    safe_str_free(&n->name);
    safe_str_free(&n->qualified_name);
    safe_str_free(&n->file_path);
    safe_str_free(&n->properties_json);
}

void cbm_store_free_nodes(cbm_node_t *nodes, int count) {
    if (!nodes) {
        return;
    }
    for (int i = 0; i < count; i++) {
        cbm_node_free_fields(&nodes[i]);
    }
    free(nodes);
}

void cbm_store_free_edges(cbm_edge_t *edges, int count) {
    if (!edges) {
        return;
    }
    for (int i = 0; i < count; i++) {
        safe_str_free(&edges[i].project);
        safe_str_free(&edges[i].type);
        safe_str_free(&edges[i].properties_json);
    }
    free(edges);
}

void cbm_project_free_fields(cbm_project_t *p) {
    safe_str_free(&p->name);
    safe_str_free(&p->indexed_at);
    safe_str_free(&p->root_path);
}

void cbm_store_free_projects(cbm_project_t *projects, int count) {
    if (!projects) {
        return;
    }
    for (int i = 0; i < count; i++) {
        cbm_project_free_fields(&projects[i]);
    }
    free(projects);
}

void cbm_store_free_file_hashes(cbm_file_hash_t *hashes, int count) {
    if (!hashes) {
        return;
    }
    for (int i = 0; i < count; i++) {
        safe_str_free(&hashes[i].project);
        safe_str_free(&hashes[i].rel_path);
        safe_str_free(&hashes[i].sha256);
    }
    free(hashes);
}

/* ── Vector search ────────────────────────��──────────────────────── */

int cbm_store_count_vectors(cbm_store_t *s, const char *project) {
    if (!s || !project) {
        return 0;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT count(*) FROM node_vectors WHERE project = ?1";
    if (sqlite3_prepare_v2(s->db, sql, SQLITE_AUTO_LEN, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, SKIP_ONE, project, SQLITE_AUTO_LEN, SQLITE_STATIC);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

void cbm_store_free_vector_results(cbm_vector_result_t *results, int count) {
    if (!results) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(results[i].project);
        free(results[i].name);
        free(results[i].qualified_name);
        free(results[i].file_path);
        free(results[i].label);
    }
    free(results);
}

/* Per-keyword scoring: score each keyword independently against each node
 * vector, then combine using min(cosine_k) across keywords.  This ensures
 * ALL keywords must be relevant, not just the average. */
enum {
    VS_VEC_DIM = 768,
    VS_SPARSE_NNZE = 8,
    VS_RI_SEED = 0x52494E44,
    VS_MAX_KW = 32,
    VS_STR_BUF = 16,
};

static cbm_zova_vector_session_t *vs_get_zova_vector_session(cbm_store_t *s,
                                                              const char *zova_path);

static int vs_workspace_vector_state(cbm_store_t *s, const char *project,
                                     char *model, size_t model_size,
                                     int *out_dimensions, int64_t *out_generation) {
    if (!s || !s->zova_workspace_query || !project || !model || model_size == 0 ||
        !out_dimensions || !out_generation) {
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            s->db,
            "SELECT s.model_fingerprint,s.vector_dimensions,s.generation "
            "FROM cbm_workspace_index_state_v1 s INNER JOIN cbm_projects_v1 p "
            "ON p.workspace_key=s.workspace_key JOIN cbm_workspace_registry r "
            "ON r.workspace_key=s.workspace_key WHERE r.workspace_id=?1 AND p.project=?2 "
            "AND s.generation>0",
            -1, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    bind_text(stmt, 1, s->zova_graph_workspace_id);
    bind_text(stmt, 2, project);
    int rc = CBM_STORE_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *stored_model = (const char *)sqlite3_column_text(stmt, 0);
        int dimensions = sqlite3_column_int(stmt, 1);
        int64_t generation = sqlite3_column_int64(stmt, 2);
        if (stored_model && stored_model[0] && strlen(stored_model) < model_size &&
            dimensions > 0 && generation > 0) {
            memcpy(model, stored_model, strlen(stored_model) + 1);
            *out_dimensions = dimensions;
            *out_generation = generation;
            rc = CBM_STORE_OK;
        }
    }
    sqlite3_finalize(stmt);
    return rc;
}

static int vs_load_workspace_native_token_vector(cbm_store_t *s, const char *project,
                                                  const char *token, float *out,
                                                  bool *out_found,
                                                  bool *out_generation_mismatch) {
    char model[256];
    int dimensions = 0;
    int64_t generation = 0;
    *out_found = false;
    if (out_generation_mismatch) *out_generation_mismatch = false;
    int state_rc = vs_workspace_vector_state(s, project, model, sizeof(model),
                                             &dimensions, &generation);
    if (state_rc != CBM_STORE_OK || dimensions != VS_VEC_DIM) return CBM_STORE_ERR;
    cbm_zova_vector_session_t *session = vs_get_zova_vector_session(s, s->db_path);
    int64_t session_generation = 0;
    if (!session || cbm_zova_vector_session_generation(session, &session_generation) != 0 ||
        session_generation != generation) {
        if (out_generation_mismatch) *out_generation_mismatch = true;
        return CBM_STORE_ERR;
    }
    int8_t values[VS_VEC_DIM];
    bool found = false;
    if (cbm_zova_vector_session_get_workspace_token_i8(
            session, s->zova_graph_workspace_id, model, dimensions, token,
            values, sizeof(values), &found) != 0) {
        return CBM_STORE_ERR;
    }
    if (found) {
        for (int d = 0; d < VS_VEC_DIM; d++) out[d] = (float)values[d] / CBM_STORE_INT8_MAX;
    }
    *out_found = found;
    return CBM_STORE_OK;
}

/* Try to look up an enriched int8 vector for `token` in the token_vectors
 * table.  On success, writes the de-quantized float representation to
 * `out` and returns true. */
static bool vs_load_enriched_vector(cbm_store_t *s, const char *project, const char *token,
                                    float *out) {
    sqlite3_stmt *tv_stmt = NULL;
    const char *tv_sql = "SELECT vector, idf FROM token_vectors"
                         " WHERE project = ?1 AND token = ?2 LIMIT 1";
    if (sqlite3_prepare_v2(s->db, tv_sql, SQLITE_AUTO_LEN, &tv_stmt, NULL) != SQLITE_OK) {
        return false;
    }
    bool found = false;
    sqlite3_bind_text(tv_stmt, SKIP_ONE, project, SQLITE_AUTO_LEN, SQLITE_STATIC);
    sqlite3_bind_text(tv_stmt, ST_COL_2, token, SQLITE_AUTO_LEN, SQLITE_STATIC);
    if (sqlite3_step(tv_stmt) == SQLITE_ROW) {
        const int8_t *vec = (const int8_t *)sqlite3_value_blob(sqlite3_column_value(tv_stmt, 0));
        int vec_len = sqlite3_column_bytes(tv_stmt, 0);
        if (vec && vec_len == VS_VEC_DIM) {
            for (int d = 0; d < VS_VEC_DIM; d++) {
                out[d] = (float)vec[d] / CBM_STORE_INT8_MAX;
            }
            found = true;
        }
    }
    sqlite3_finalize(tv_stmt);
    return found;
}

/* Sparse-random-index fallback for tokens not present in the enriched table. */
static void vs_fill_sparse_random(const char *token, float *out) {
    uint64_t seed = XXH3_64bits(token, strlen(token));
    for (int i = 0; i < VS_SPARSE_NNZE; i++) {
        uint64_t h = XXH3_64bits_withSeed(&i, sizeof(i), seed + VS_RI_SEED);
        int pos = (int)(h % VS_VEC_DIM);
        float sign = (h & SKIP_ONE) ? CBM_STORE_UNIT_POS_F : -CBM_STORE_UNIT_POS_F;
        out[pos] += sign;
    }
}

/* Clamp one float into the int8 representable range. */
static int8_t vs_clamp_int8(float v) {
    if (v > CBM_STORE_INT8_MAX) {
        return (int8_t)CBM_STORE_INT8_MAX;
    }
    if (v < -CBM_STORE_INT8_MAX) {
        return (int8_t)-CBM_STORE_INT8_MAX;
    }
    return (int8_t)v;
}

/* Normalize + int8 quantize a float vector.  Returns false if the vector is
 * effectively zero (magnitude below epsilon), in which case `dst` is left in
 * an indeterminate state and the caller should skip this keyword. */
static bool vs_normalize_and_quantize(const float *src, int8_t *dst) {
    float mag = 0.0F;
    for (int d = 0; d < VS_VEC_DIM; d++) {
        mag += src[d] * src[d];
    }
    mag = sqrtf(mag);
    if (mag < CBM_STORE_DENOM_EPS_F) {
        return false;
    }
    float inv = CBM_STORE_UNIT_POS_F / mag;
    for (int d = 0; d < VS_VEC_DIM; d++) {
        dst[d] = vs_clamp_int8(src[d] * inv * CBM_STORE_INT8_MAX);
    }
    return true;
}

/* Build int8 query vectors for each keyword.  Returns the number of
 * successfully built vectors (may be less than keyword_count when some
 * keywords produce zero-magnitude vectors). */
static int vs_build_keyword_vectors(cbm_store_t *s, const char *project, const char **keywords,
                                    int keyword_count, int8_t (*kw_vecs)[VS_VEC_DIM],
                                    int *out_actual_kw,
                                    cbm_vector_search_metrics_t *metrics) {
    int actual_kw = 0;
    for (int k = 0; k < keyword_count && actual_kw < VS_MAX_KW; k++) {
        if (!keywords[k] || !keywords[k][0]) {
            continue;
        }
        float kw_f[VS_VEC_DIM];
        memset(kw_f, 0, sizeof(kw_f));
        bool found = false;
        if (s->zova_workspace_query) {
            bool generation_mismatch = false;
            if (vs_load_workspace_native_token_vector(
                    s, project, keywords[k], kw_f, &found,
                    &generation_mismatch) != CBM_STORE_OK) {
                if (metrics && generation_mismatch) metrics->generation_mismatch = true;
                return CBM_STORE_ERR;
            }
        } else {
            found = vs_load_enriched_vector(s, project, keywords[k], kw_f);
        }
        if (!found) {
            vs_fill_sparse_random(keywords[k], kw_f);
        }
        if (vs_normalize_and_quantize(kw_f, kw_vecs[actual_kw])) {
            actual_kw++;
        }
    }
    *out_actual_kw = actual_kw;
    return CBM_STORE_OK;
}

/* Compute the per-keyword min cosine score between a node's int8 vector and
 * each of the query vectors.  Returns 0.0 if the node vector is unavailable
 * or mis-sized. */
static double vs_min_cosine_score(const int8_t *node_vec, int node_vec_len,
                                  const int8_t (*kw_vecs)[VS_VEC_DIM], int actual_kw) {
    if (!node_vec || node_vec_len != VS_VEC_DIM) {
        return 0.0;
    }
    double min_score = CBM_STORE_UNIT_POS_D;
    for (int k = 0; k < actual_kw; k++) {
        int32_t dot = 0;
        int32_t ma = 0;
        int32_t mb = 0;
        for (int d = 0; d < VS_VEC_DIM; d++) {
            dot += (int32_t)kw_vecs[k][d] * (int32_t)node_vec[d];
            ma += (int32_t)kw_vecs[k][d] * (int32_t)kw_vecs[k][d];
            mb += (int32_t)node_vec[d] * (int32_t)node_vec[d];
        }
        double denom = sqrt((double)ma) * sqrt((double)mb);
        double cos_k = denom > CBM_STORE_DENOM_EPS_D ? (double)dot / denom : 0.0;
        if (cos_k < min_score) {
            min_score = cos_k;
        }
    }
    return min_score;
}

/* Append one candidate row read from the scan statement into the result
 * vector.  Grows the results array geometrically on demand.  Returns the
 * (possibly grown) results pointer, or NULL on allocation failure. */
static cbm_vector_result_t *vs_append_result(cbm_vector_result_t *results, int *count, int *cap,
                                             sqlite3_stmt *stmt,
                                             const int8_t (*kw_vecs)[VS_VEC_DIM], int actual_kw) {
    if (*count >= *cap) {
        int nc = *cap < CBM_SZ_16 ? CBM_SZ_16 : *cap * ST_COL_2;
        cbm_vector_result_t *grown = realloc(results, (size_t)nc * sizeof(cbm_vector_result_t));
        if (!grown) {
            return NULL;
        }
        results = grown;
        *cap = nc;
    }
    int idx = (*count)++;
    memset(&results[idx], 0, sizeof(results[idx]));
    results[idx].node_id = sqlite3_column_int64(stmt, 0);
    const char *name = (const char *)sqlite3_column_text(stmt, SKIP_ONE);
    const char *qn = (const char *)sqlite3_column_text(stmt, ST_COL_2);
    const char *fp = (const char *)sqlite3_column_text(stmt, ST_COL_3);
    const char *label = (const char *)sqlite3_column_text(stmt, ST_COL_4);
    results[idx].name = name ? strdup(name) : strdup("");
    results[idx].qualified_name = qn ? strdup(qn) : strdup("");
    results[idx].file_path = fp ? strdup(fp) : strdup("");
    results[idx].label = label ? strdup(label) : strdup("");
    const int8_t *node_vec = (const int8_t *)sqlite3_column_blob(stmt, ST_COL_6);
    int node_vec_len = sqlite3_column_bytes(stmt, ST_COL_6);
    results[idx].score = vs_min_cosine_score(node_vec, node_vec_len, kw_vecs, actual_kw);
    return results;
}

static void vs_sort_and_trim(cbm_vector_result_t *results, int *count, int limit) {
    if (!results || !count) {
        return;
    }
    for (int i = 0; i < *count - SKIP_ONE; i++) {
        for (int j = i + SKIP_ONE; j < *count; j++) {
            bool higher_score = results[j].score > results[i].score;
            bool stable_tie = results[j].score == results[i].score &&
                              strcmp(results[j].qualified_name ? results[j].qualified_name : "",
                                     results[i].qualified_name ? results[i].qualified_name : "") < 0;
            if (higher_score || stable_tie) {
                cbm_vector_result_t tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    int final_limit = limit > 0 ? limit : CBM_SZ_16;
    if (*count > final_limit) {
        for (int i = final_limit; i < *count; i++) {
            free(results[i].name);
            free(results[i].qualified_name);
            free(results[i].file_path);
            free(results[i].label);
        }
        *count = final_limit;
    }
}

static double vs_elapsed_ms(const struct timespec *start, const struct timespec *end) {
    return ((double)(end->tv_sec - start->tv_sec) * 1000.0) +
           ((double)(end->tv_nsec - start->tv_nsec) / 1000000.0);
}

static void vs_record_elapsed(const struct timespec *start, double *out_ms) {
    struct timespec end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &end);
    *out_ms = vs_elapsed_ms(start, &end);
}

static void store_close_zova_vector_session(cbm_store_t *s) {
    if (!s) {
        return;
    }
    cbm_zova_vector_session_close(s->zova_vector_session);
    s->zova_vector_session = NULL;
    free(s->zova_vector_session_path);
    s->zova_vector_session_path = NULL;
    s->zova_vector_session_stat_valid = false;
}

static bool vs_zova_session_matches(const cbm_store_t *s, const char *zova_path,
                                    const struct stat *st) {
    return s && s->zova_vector_session && s->zova_vector_session_path &&
           s->zova_vector_session_stat_valid && strcmp(s->zova_vector_session_path, zova_path) == 0 &&
           s->zova_vector_session_stat.st_dev == st->st_dev &&
           s->zova_vector_session_stat.st_ino == st->st_ino &&
           s->zova_vector_session_stat.st_size == st->st_size &&
           s->zova_vector_session_stat.st_mtime == st->st_mtime;
}

static cbm_zova_vector_session_t *vs_get_zova_vector_session(cbm_store_t *s,
                                                              const char *zova_path) {
    struct stat st;
    if (!s || !zova_path || stat(zova_path, &st) != 0) {
        return NULL;
    }
    if (vs_zova_session_matches(s, zova_path, &st)) {
        return s->zova_vector_session;
    }
    store_close_zova_vector_session(s);
    cbm_zova_vector_session_t *session = cbm_zova_vector_session_open(zova_path);
    char *path_copy = session ? heap_strdup(zova_path) : NULL;
    if (!session || !path_copy) {
        cbm_zova_vector_session_close(session);
        return NULL;
    }
    s->zova_vector_session = session;
    s->zova_vector_session_path = path_copy;
    s->zova_vector_session_stat = st;
    s->zova_vector_session_stat_valid = true;
    return session;
}

static void store_close_zova_graph_session(cbm_store_t *s) {
    if (!s) {
        return;
    }
    cbm_zova_graph_session_close(s->zova_graph_session);
    s->zova_graph_session = NULL;
}

static void store_clear_zova_graph_workspace(cbm_store_t *s) {
    if (!s) {
        return;
    }
    free(s->zova_graph_workspace_root);
    s->zova_graph_workspace_root = NULL;
    s->zova_graph_workspace_id[0] = '\0';
}

static int vs_try_zova_workspace_native_vector_search(
    cbm_store_t *s, const char *project, const int8_t (*kw_vecs)[VS_VEC_DIM], int actual_kw,
    int limit, cbm_vector_result_t **out, int *out_count,
    cbm_vector_search_metrics_t *metrics) {
    if (!s || !s->zova_workspace_query) return CBM_STORE_NOT_FOUND;

    char model[256];
    int vector_dimensions = 0;
    int64_t generation = 0;
    if (vs_workspace_vector_state(s, project, model, sizeof(model),
                                  &vector_dimensions, &generation) != CBM_STORE_OK ||
        vector_dimensions != VS_VEC_DIM) {
        return CBM_STORE_ERR;
    }
    cbm_zova_vector_session_t *session = vs_get_zova_vector_session(s, s->db_path);
    int64_t session_generation = 0;
    if (!session || cbm_zova_vector_session_generation(session, &session_generation) != 0 ||
        session_generation != generation) {
        if (metrics) metrics->generation_mismatch = true;
        return CBM_STORE_ERR;
    }
    char collection[CBM_ZOVA_WORKSPACE_ID_MAX + 384];
    if (cbm_zova_workspace_node_vector_collection_name(
            s->zova_graph_workspace_id, model, vector_dimensions,
            collection, sizeof(collection)) != 0) {
        return CBM_STORE_ERR;
    }

    int requested_limit = limit > 0 ? limit : CBM_SZ_16;
    int fetch_limit = requested_limit * ST_COL_5;
    struct timespec started;
    cbm_clock_gettime(CLOCK_MONOTONIC, &started);
    cbm_zova_vector_hit_t *hits = NULL;
    int hit_count = 0;
    if (cbm_zova_vector_session_search_collection_i8(
            session, collection, &kw_vecs[0][0], actual_kw, vector_dimensions,
            fetch_limit, fetch_limit, &hits, &hit_count) != 0) {
        return CBM_STORE_ERR;
    }
    if (metrics) {
        metrics->used_zova = true;
        metrics->zova_native_multi_query = actual_kw > 1;
        metrics->zova_fetch_limit = fetch_limit;
        vs_record_elapsed(&started, &metrics->zova_prefetch_ms);
    }
    if (hit_count == 0) {
        cbm_zova_vector_hits_free(hits, hit_count);
        return CBM_STORE_OK;
    }

    if (!s->zova_repository) {
        cbm_zova_vector_hits_free(hits, hit_count);
        return CBM_STORE_ERR;
    }
    cbm_vector_result_t *results = calloc((size_t)hit_count, sizeof(*results));
    int64_t *node_keys = calloc((size_t)hit_count, sizeof(*node_keys));
    cbm_node_t *nodes = NULL;
    if (!results || !node_keys) {
        free(results);
        free(node_keys);
        cbm_zova_vector_hits_free(hits, hit_count);
        return CBM_STORE_ERR;
    }
    for (int i = 0; i < hit_count; i++) {
        char *end = NULL;
        long long key = hits[i].vector_id ? strtoll(hits[i].vector_id, &end, 10) : 0;
        if (!hits[i].vector_id || end == hits[i].vector_id || *end != '\0' || key <= 0) {
            free(node_keys);
            free(results);
            cbm_zova_vector_hits_free(hits, hit_count);
            return CBM_STORE_ERR;
        }
        node_keys[i] = (int64_t)key;
    }
    if (cbm_zova_repository_find_nodes_by_keys(
            s->zova_repository, s->zova_graph_workspace_id, node_keys,
            hit_count, &nodes) != CBM_STORE_OK) {
        free(node_keys);
        free(results);
        cbm_zova_vector_hits_free(hits, hit_count);
        return CBM_STORE_ERR;
    }
    free(node_keys);
    int count = 0;
    int rc = CBM_STORE_OK;
    for (int i = 0; i < hit_count; i++) {
        cbm_node_t *node = &nodes[i];
        if (!node->label || (strcmp(node->label, "Function") != 0 &&
                             strcmp(node->label, "Method") != 0 &&
                             strcmp(node->label, "Class") != 0)) {
            cbm_node_free_fields(node);
            continue;
        }
        cbm_vector_result_t *item = &results[count++];
        item->node_id = node->id;
        item->name = (char *)node->name; node->name = NULL;
        item->qualified_name = (char *)node->qualified_name; node->qualified_name = NULL;
        item->file_path = (char *)node->file_path; node->file_path = NULL;
        item->label = (char *)node->label; node->label = NULL;
        item->score = hits[i].score;
        cbm_node_free_fields(node);
        if (!item->name || !item->qualified_name || !item->file_path || !item->label) {
            rc = CBM_STORE_ERR;
            break;
        }
    }
    cbm_store_free_nodes(nodes, hit_count);
    cbm_zova_vector_hits_free(hits, hit_count);
    if (rc != CBM_STORE_OK) {
        cbm_store_free_vector_results(results, count);
        return CBM_STORE_ERR;
    }
    vs_sort_and_trim(results, &count, limit);
    *out = results;
    *out_count = count;
    return CBM_STORE_OK;
}

static int vs_try_zova_vector_search(cbm_store_t *s, const char *project,
                                     const int8_t (*kw_vecs)[VS_VEC_DIM], int actual_kw,
                                     int limit, cbm_vector_result_t **out, int *out_count,
                                     cbm_vector_search_metrics_t *metrics) {
    if (!cbm_zova_build_enabled() || !s || !s->db_path || !s->zova_workspace_query) {
        return CBM_STORE_NOT_FOUND;
    }
    return vs_try_zova_workspace_native_vector_search(s, project, kw_vecs, actual_kw, limit, out,
                                                      out_count, metrics);
}

int cbm_store_vector_search_ex(cbm_store_t *s, const char *project, const char **keywords,
                               int keyword_count, int limit, cbm_vector_result_t **out,
                               int *out_count, cbm_vector_search_metrics_t *metrics) {
    if (metrics) {
        memset(metrics, 0, sizeof(*metrics));
    }
    *out = NULL;
    *out_count = 0;
    if (!s || !project || !keywords || keyword_count <= 0) {
        return CBM_STORE_ERR;
    }

    struct timespec stage_start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &stage_start);
    int8_t kw_vecs[VS_MAX_KW][VS_VEC_DIM];
    int actual_kw = 0;
    if (vs_build_keyword_vectors(s, project, keywords, keyword_count, kw_vecs,
                                 &actual_kw, metrics) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    if (metrics) {
        vs_record_elapsed(&stage_start, &metrics->query_vector_build_ms);
    }
    if (actual_kw == 0) {
        return CBM_STORE_OK;
    }

    int zova_rc =
        vs_try_zova_vector_search(s, project, kw_vecs, actual_kw, limit, out, out_count, metrics);
    if (zova_rc == CBM_STORE_OK) {
        return CBM_STORE_OK;
    }
    if (zova_rc == CBM_STORE_ERR) {
        return CBM_STORE_ERR;
    }
    if (s->zova_workspace_query) {
        return CBM_STORE_ERR;
    }

    /* Scan all node vectors, compute per-keyword cosine, take min.
     * We use the FIRST keyword as the SQL sort (for top-K pre-filter),
     * then re-score with min across all keywords in the append helper. */
    const char *sql = NULL;
    if (s->zova_workspace_query) {
        sql = "SELECT n.id,n.name,n.qualified_name,n.file_path,n.label,"
              "cbm_cosine_i8(v.vector,?1) AS score,v.vector FROM node_vectors v "
              "INNER JOIN nodes n ON n.id=v.node_id "
              "WHERE n.label IN ('Function','Method','Class') ORDER BY score DESC LIMIT ?2";
    } else {
        sql = "SELECT n.id,n.name,n.qualified_name,n.file_path,n.label,"
              "cbm_cosine_i8(v.vector,?1) AS score,v.vector FROM node_vectors v "
              "INNER JOIN nodes n ON n.id=v.node_id WHERE v.project=?2 "
              "AND n.label IN ('Function','Method','Class') ORDER BY score DESC LIMIT ?3";
    }

    sqlite3_stmt *stmt = NULL;
    int prep_rc = sqlite3_prepare_v2(s->db, sql, SQLITE_AUTO_LEN, &stmt, NULL);
    if (prep_rc != SQLITE_OK) {
        (void)fprintf(stderr, "vector_search: %s\n", sqlite3_errmsg(s->db));
        return CBM_STORE_ERR;
    }

    /* Use first keyword for SQL pre-filter, fetch more candidates for re-ranking */
    int fetch_limit = (limit > 0 ? limit : CBM_SZ_16) * ST_COL_5;
    sqlite3_bind_blob(stmt, SKIP_ONE, kw_vecs[0], VS_VEC_DIM, SQLITE_STATIC);
    if (s->zova_workspace_query) {
        sqlite3_bind_int(stmt, ST_COL_2, fetch_limit);
    } else {
        sqlite3_bind_text(stmt, ST_COL_2, project, SQLITE_AUTO_LEN, SQLITE_STATIC);
        sqlite3_bind_int(stmt, ST_COL_3, fetch_limit);
    }

    {
        char kw_buf[VS_STR_BUF];
        char fl_buf[VS_STR_BUF];
        snprintf(kw_buf, sizeof(kw_buf), "%d", actual_kw);
        snprintf(fl_buf, sizeof(fl_buf), "%d", fetch_limit);
        cbm_log_info("vector_search.exec", "kw_count", kw_buf, "fetch_limit", fl_buf, "project",
                     project);
    }

    cbm_vector_result_t *results = NULL;
    int count = 0;
    int cap = 0;
    int step_rc = 0;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cbm_vector_result_t *grown =
            vs_append_result(results, &count, &cap, stmt, kw_vecs, actual_kw);
        if (!grown) {
            break;
        }
        results = grown;
    }

    if (step_rc != SQLITE_DONE) {
        char rc_buf[VS_STR_BUF];
        snprintf(rc_buf, sizeof(rc_buf), "%d", step_rc);
        cbm_log_warn("vector_search.step_error", "rc", rc_buf, "msg", sqlite3_errmsg(s->db));
    }
    {
        char cnt_buf[VS_STR_BUF];
        snprintf(cnt_buf, sizeof(cnt_buf), "%d", count);
        cbm_log_info("vector_search.done", "candidates", cnt_buf);
    }
    sqlite3_finalize(stmt);

    vs_sort_and_trim(results, &count, limit);

    *out = results;
    *out_count = count;
    return CBM_STORE_OK;
}

int cbm_store_vector_search(cbm_store_t *s, const char *project, const char **keywords,
                            int keyword_count, int limit, cbm_vector_result_t **out,
                            int *out_count) {
    return cbm_store_vector_search_ex(s, project, keywords, keyword_count, limit, out, out_count,
                                      NULL);
}

static int multi_vector_result_compare(const void *left, const void *right) {
    const cbm_vector_result_t *a = left;
    const cbm_vector_result_t *b = right;
    if (a->score > b->score) return -1;
    if (a->score < b->score) return 1;
    int by_project = strcmp(a->project, b->project);
    if (by_project != 0) return by_project;
    int by_qn = strcmp(a->qualified_name, b->qualified_name);
    if (by_qn != 0) return by_qn;
    return (a->node_id > b->node_id) - (a->node_id < b->node_id);
}

int cbm_store_zova_multi_vector_search(
    const char *db_path, const cbm_zova_vector_workspace_t *workspaces, int workspace_count,
    const char **keywords, int keyword_count, int limit, int offset,
    cbm_vector_result_t **out, int *out_count) {
    if (!out || !out_count) return CBM_STORE_ERR;
    *out = NULL;
    *out_count = 0;
    if (!db_path || !workspaces || workspace_count <= 0 || !keywords || keyword_count <= 0 ||
        limit <= 0 || offset < 0 || offset > INT_MAX - limit) {
        return CBM_STORE_ERR;
    }
    int needed = offset + limit;
    cbm_store_t *store = cbm_store_open_path_query(db_path);
    cbm_zova_vector_session_t *session = cbm_zova_vector_session_open(db_path);
    if (!store || !session) {
        cbm_store_close(store);
        cbm_zova_vector_session_close(session);
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *hydrate = NULL;
    if (sqlite3_prepare_v2(
            store->db,
            "SELECT n.label,n.name,n.qualified_name,f.file_path FROM cbm_nodes_v1 n "
            "JOIN cbm_files_v1 f ON f.file_key=n.file_key WHERE n.workspace_key=?1 "
            "AND n.zova_node_key=?2",
            -1, &hydrate, NULL) != SQLITE_OK) {
        cbm_store_close(store);
        cbm_zova_vector_session_close(session);
        return CBM_STORE_ERR;
    }

    cbm_vector_result_t *combined = NULL;
    int combined_count = 0;
    int combined_capacity = 0;
    int rc = CBM_STORE_OK;
    for (int w = 0; w < workspace_count && rc == CBM_STORE_OK; w++) {
        const cbm_zova_vector_workspace_t *workspace = &workspaces[w];
        if (!workspace->workspace_id || !workspace->selector || !workspace->model_fingerprint ||
            workspace->vector_dimensions != VS_VEC_DIM) {
            rc = CBM_STORE_ERR;
            break;
        }
        int8_t query_vectors[VS_MAX_KW][VS_VEC_DIM];
        int query_count = 0;
        for (int k = 0; k < keyword_count && query_count < VS_MAX_KW; k++) {
            if (!keywords[k] || !keywords[k][0]) continue;
            int8_t token_vector[VS_VEC_DIM];
            bool found = false;
            float values[VS_VEC_DIM] = {0};
            if (cbm_zova_vector_session_get_workspace_token_i8(
                    session, workspace->workspace_id, workspace->model_fingerprint,
                    workspace->vector_dimensions, keywords[k], token_vector,
                    sizeof(token_vector), &found) != 0) {
                rc = CBM_STORE_ERR;
                break;
            }
            if (found) {
                for (int d = 0; d < VS_VEC_DIM; d++)
                    values[d] = (float)token_vector[d] / CBM_STORE_INT8_MAX;
            } else {
                vs_fill_sparse_random(keywords[k], values);
            }
            if (vs_normalize_and_quantize(values, query_vectors[query_count])) query_count++;
        }
        if (rc != CBM_STORE_OK || query_count == 0) continue;

        char collection[CBM_ZOVA_WORKSPACE_ID_MAX + 384];
        if (cbm_zova_workspace_node_vector_collection_name(
                workspace->workspace_id, workspace->model_fingerprint,
                workspace->vector_dimensions, collection, sizeof(collection)) != 0) {
            rc = CBM_STORE_ERR;
            break;
        }
        int prefilter = query_count > 1 && needed <= INT_MAX / ST_COL_5
                            ? needed * ST_COL_5
                            : needed;
        cbm_zova_vector_hit_t *hits = NULL;
        int hit_count = 0;
        if (cbm_zova_vector_session_search_collection_i8(
                session, collection, &query_vectors[0][0], query_count, VS_VEC_DIM,
                prefilter, needed, &hits, &hit_count) != 0) {
            rc = CBM_STORE_ERR;
            break;
        }
        for (int i = 0; i < hit_count && rc == CBM_STORE_OK; i++) {
            char *end = NULL;
            long long key = hits[i].vector_id ? strtoll(hits[i].vector_id, &end, 10) : 0;
            if (!hits[i].vector_id || end == hits[i].vector_id || *end != '\0' || key <= 0 ||
                sqlite3_reset(hydrate) != SQLITE_OK ||
                sqlite3_clear_bindings(hydrate) != SQLITE_OK ||
                sqlite3_bind_int64(hydrate, 1, workspace->workspace_key) != SQLITE_OK ||
                sqlite3_bind_int64(hydrate, 2, (int64_t)key) != SQLITE_OK) {
                rc = CBM_STORE_ERR;
                break;
            }
            if (sqlite3_step(hydrate) != SQLITE_ROW) {
                rc = CBM_STORE_ERR;
                break;
            }
            const char *label = (const char *)sqlite3_column_text(hydrate, 0);
            if (!label || (strcmp(label, "Function") != 0 && strcmp(label, "Method") != 0 &&
                           strcmp(label, "Class") != 0)) {
                continue;
            }
            if (combined_count == combined_capacity) {
                if (combined_capacity > INT_MAX / 2) {
                    rc = CBM_STORE_ERR;
                    break;
                }
                int next = combined_capacity ? combined_capacity * 2 : CBM_SZ_16;
                cbm_vector_result_t *grown =
                    realloc(combined, (size_t)next * sizeof(*combined));
                if (!grown) {
                    rc = CBM_STORE_ERR;
                    break;
                }
                combined = grown;
                combined_capacity = next;
            }
            cbm_vector_result_t *result = &combined[combined_count];
            memset(result, 0, sizeof(*result));
            result->node_id = (int64_t)key;
            result->project = strdup(workspace->selector);
            result->label = strdup(label);
            const char *name = (const char *)sqlite3_column_text(hydrate, 1);
            const char *qn = (const char *)sqlite3_column_text(hydrate, 2);
            const char *file = (const char *)sqlite3_column_text(hydrate, 3);
            result->name = strdup(name ? name : "");
            result->qualified_name = strdup(qn ? qn : "");
            result->file_path = strdup(file ? file : "");
            result->score = hits[i].score;
            if (!result->project || !result->label || !result->name ||
                !result->qualified_name || !result->file_path) {
                free(result->project);
                free(result->label);
                free(result->name);
                free(result->qualified_name);
                free(result->file_path);
                memset(result, 0, sizeof(*result));
                rc = CBM_STORE_ERR;
                break;
            }
            combined_count++;
        }
        cbm_zova_vector_hits_free(hits, hit_count);
    }
    sqlite3_finalize(hydrate);
    cbm_store_close(store);
    cbm_zova_vector_session_close(session);
    if (rc != CBM_STORE_OK) {
        cbm_store_free_vector_results(combined, combined_count);
        return rc;
    }
    if (combined_count > 1) {
        qsort(combined, (size_t)combined_count, sizeof(*combined), multi_vector_result_compare);
    }
    int start = offset < combined_count ? offset : combined_count;
    int end = start + limit < combined_count ? start + limit : combined_count;
    int page_count = end - start;
    cbm_vector_result_t *page = page_count ? calloc((size_t)page_count, sizeof(*page)) : NULL;
    if (page_count && !page) {
        cbm_store_free_vector_results(combined, combined_count);
        return CBM_STORE_ERR;
    }
    for (int i = 0; i < page_count; i++) {
        page[i] = combined[start + i];
        memset(&combined[start + i], 0, sizeof(*combined));
    }
    cbm_store_free_vector_results(combined, combined_count);
    *out = page;
    *out_count = page_count;
    return CBM_STORE_OK;
}
