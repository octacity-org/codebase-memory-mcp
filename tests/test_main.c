/*
 * test_main.c — Test runner entry point for pure C rewrite.
 *
 * Includes all test suites and runs them sequentially.
 */
/* Global test counters (declared extern in test_framework.h) */
int tf_pass_count = 0;
int tf_fail_count = 0;
int tf_skip_count = 0;
int tf_tier_excluded_count = 0;

#include "test_framework.h"
#include "foundation/compat.h"    /* cbm_setenv — #845 supervisor kill switch */
#include "foundation/compat_fs.h" /* cbm_fopen — worker response file */
#include "foundation/mem.h"       /* cbm_mem_init — worker budget */
#include "mcp/index_supervisor.h" /* cbm_index_set_worker_role */
#include "pipeline/result_spill.h" /* cbm_result_spill_pin_free_bytes_for_tests */
#include "mcp/mcp.h"              /* cbm_mcp_handle_tool — act as a real worker */
#include "foundation/subprocess.h" /* cbm_resolve_binary_path — deleted-self probe */
#include "test_helpers.h"          /* th_rmtree — isolated cache cleanup */
#include "discover/discover.h"     /* cbm_language_tier — tf_lang_runs */
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h> /* #798 follow-up: socket-isolation re-exec probe */
#else
#include <unistd.h> /* deleted-self probe */
#endif

/* #832 guard support: when the index supervisor spawns THIS binary as
 * `<self> cli --index-worker index_repository <args_json> --response-out <file>`
 * (exactly the argv cbm_index_spawn_worker builds), act as a faithful in-process
 * index worker instead of re-running the test suites. This lets the deterministic
 * gating guard (test_mcp.c) spawn a REAL worker child that indexes the fixture and
 * writes its response back, using only public APIs — no production test seam.
 * Returns an exit code (>=0) when it handled a worker invocation, else -1. */
static int tf_maybe_run_index_worker(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "cli") != 0) {
        return -1;
    }
    bool is_worker = false;
    const char *tool = NULL;
    const char *args_json = "{}";
    const char *response_out = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--index-worker") == 0) {
            is_worker = true;
        } else if (strcmp(argv[i], "--response-out") == 0) {
            if (i + 1 < argc) {
                response_out = argv[++i];
            }
        } else if (argv[i][0] == '{') {
            args_json = argv[i];
        } else if (argv[i][0] != '-' && !tool) {
            tool = argv[i];
        }
    }
    if (!is_worker) {
        return -1;
    }
    if (!tool) {
        tool = "index_repository";
    }

    cbm_mem_init(0.5);
    cbm_index_set_worker_role(true, response_out); /* worker role → index in-process */
    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    if (!srv) {
        return 1;
    }
    char *result = cbm_mcp_handle_tool(srv, tool, args_json);
    if (result) {
        const char *ro = cbm_index_worker_response_out();
        if (ro) {
            FILE *rf = cbm_fopen(ro, "wb");
            if (rf) {
                (void)fputs(result, rf);
                (void)fclose(rf);
            }
        }
    }
    /* Faithful worker exit: mirror run_cli's supervised-worker fast path.
     * The worker-role pipeline deliberately skips its teardown (the OS
     * reclaims everything wholesale on process death), so a normal return
     * through main() lets LeakSanitizer run at exit, report the
     * intentionally-unfreed pipeline, and force exit code 1 — the
     * supervisor then reads a HEALTHY index as worker_failed (the
     * Linux-only IDX832 red: LSan is active in Linux gcc ASan builds,
     * absent on macOS/Windows). _Exit skips atexit/LSan by design,
     * exactly like the production worker in run_cli. */
    /* run_cli exits nonzero for an isError result too (cli_print_mcp_result). */
    int exit_code = result && !strstr(result, "\"isError\":true") ? 0 : 1;
    fflush(NULL);
    _Exit(exit_code);
}

/* #798 follow-up: socket-isolation probe. The parent test
 * (popen_isolates_listening_socket, test_security.c) spawns THIS binary through
 * cbm_popen — the same cmd.exe-grandchild path git takes — passing the numeric
 * value of an inheritable listening-socket handle. If cbm_popen correctly
 * isolates handles, that socket is NOT present in this child and getsockopt
 * fails; a regression to raw _popen leaks it (bInheritHandles=TRUE propagates it
 * transitively through cmd.exe) and getsockopt succeeds. We report via exit code
 * so the verdict survives `cmd.exe /c` (proven by popen_isolated_propagates_exit_code).
 * Returns an exit code (>=0) when it handled a probe invocation, else -1. */
static int tf_maybe_run_socket_probe(int argc, char **argv) {
#ifdef _WIN32
    if (argc < 3 || strcmp(argv[1], "__cbm_sockprobe") != 0) {
        return -1;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 0; /* no winsock in child ⇒ cannot observe a socket ⇒ not leaked */
    }
    unsigned long long hv = strtoull(argv[2], NULL, 10);
    SOCKET s = (SOCKET)(uintptr_t)hv;
    int type = 0;
    int len = (int)sizeof(type);
    int rc = getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &len);
    /* rc==0 ⇒ the handle is a live socket in THIS child ⇒ it was inherited. */
    return rc == 0 ? 42 : 0;
#else
    (void)argc;
    (void)argv;
    return -1;
#endif
}

/* #1204 (upstream 11bbf0d8, 5b96b067): spawned by
 * subprocess_deleted_self_spawn_path_follows_platform_ruling. Signals ready,
 * waits while the parent replaces or removes this image, then reports what
 * cbm_resolve_binary_path hands back. */
static int tf_maybe_run_deleted_self_probe(int argc, char **argv) {
#if defined(__linux__) || defined(__APPLE__)
    if (argc != 4 || strcmp(argv[1], "__cbm_deleted_self_probe") != 0) {
        return -1;
    }
    int ready_fd = atoi(argv[2]);
    int continue_fd = atoi(argv[3]);
    if (write(ready_fd, "R", 1) != 1) {
        return 41;
    }
    char go = '\0';
    if (read(continue_fd, &go, 1) != 1) {
        return 42;
    }
    char resolved[1024];
    bool ok = cbm_resolve_binary_path(NULL, resolved, sizeof(resolved));
#if defined(__linux__)
    /* Prove the image really is deleted, or the checks below pass vacuously. */
    char link_target[1024];
    ssize_t n = readlink("/proc/self/exe", link_target, sizeof(link_target) - 1);
    if (n <= 0) {
        return 45;
    }
    link_target[n] = '\0';
    if (strstr(link_target, " (deleted)") == NULL) {
        return 46;
    }
    if (!ok) {
        return 43;
    }
    return strcmp(resolved, "/proc/self/exe") == 0 && access(resolved, X_OK) == 0 ? 0 : 44;
#else
    /* macOS has no magic link: success is the resolver refusing. */
    return ok ? 44 : 0;
#endif
#else
    (void)argc;
    (void)argv;
    return -1;
#endif
}

/* Highest language tier this run covers: CBM_TEST_TIERS = 1 (default), 2, 3
 * or "all". */
static int tf_max_tier(void) {
    static int max_tier = 0;
    if (max_tier == 0) {
        const char *env = getenv("CBM_TEST_TIERS");
        max_tier = 1;
        if (env && strcmp(env, "all") == 0) {
            max_tier = 3;
        } else if (env && env[0] >= '1' && env[0] <= '3' && env[1] == '\0') {
            max_tier = env[0] - '0';
        } else if (env && env[0]) {
            fprintf(stderr, "CBM_TEST_TIERS=%s not understood (use 1, 2, 3 or all); running tier 1\n",
                    env);
        }
    }
    return max_tier;
}

int tf_lang_runs(int lang) {
    int tier = cbm_language_tier((CBMLanguage)lang);
    return tier >= 1 && tier <= tf_max_tier();
}

static int g_suite_argc = 0;
static char **g_suite_argv = NULL;

static bool suite_requested(const char *name) {
    if (g_suite_argc <= 1) {
        return true;
    }
    for (int i = 1; i < g_suite_argc; i++) {
        if (strcmp(g_suite_argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

#define RUN_SELECTED_SUITE(name)      \
    do {                              \
        if (suite_requested(#name)) { \
            RUN_SUITE(name);          \
        }                             \
    } while (0)

#define RUN_EXPLICIT_SUITE(name)                         \
    do {                                                  \
        if (g_suite_argc > 1 && suite_requested(#name)) { \
            RUN_SUITE(name);                             \
        }                                                 \
    } while (0)

/* Forward declarations of suite functions */
extern void suite_arena(void);
extern void suite_hash_table(void);
extern void suite_dyn_array(void);
extern void suite_str_intern(void);
extern void suite_log(void);
extern void suite_str_util(void);
extern void suite_platform(void);
extern void suite_subprocess(void);
extern void suite_extraction(void);
extern void suite_parse_coverage(void);
extern void suite_extraction_inheritance(void);
extern void suite_extraction_imports(void);
extern void suite_grammar_regression(void);
extern void suite_grammar_labels(void);
extern void suite_grammar_imports(void);
extern void suite_store_nodes(void);
extern void suite_store_edges(void);
extern void suite_store_search(void);
extern void suite_cypher(void);
extern void suite_mcp(void);
extern void suite_language(void);
extern void suite_userconfig(void);
extern void suite_gitignore(void);
extern void suite_git_context(void);
extern void suite_discover(void);
extern void suite_graph_buffer(void);
extern void suite_registry(void);
extern void suite_pipeline(void);
extern void suite_spawns(void);
extern void suite_importance(void);
extern void suite_index_resilience(void);
extern void suite_call_reference(void);
extern void suite_fqn(void);
extern void suite_route_canon(void);
extern void suite_path_alias(void);
extern void suite_watcher(void);
extern void suite_go_lsp(void);
extern void suite_c_lsp(void);
extern void suite_php_lsp(void);
extern void suite_cs_lsp(void);
extern void suite_cs_lsp_bench(void);
extern void suite_scope(void);
extern void suite_type_rep(void);
extern void suite_py_lsp(void);
extern void suite_py_lsp_bench(void);
extern void suite_py_lsp_stress(void);
extern void suite_py_lsp_scale(void);
extern void suite_ts_lsp(void);
extern void suite_java_lsp(void);
extern void suite_java_lsp_coverage(void);
extern void suite_kotlin_lsp(void);
extern void suite_rust_lsp(void);
extern void suite_zig_lsp(void);
extern void suite_store_arch(void);
extern void suite_configlink(void);
extern void suite_infrascan(void);
extern void suite_cli(void);
extern void suite_system_info(void);
extern void suite_worker_pool(void);
extern void suite_parallel(void);
extern void suite_mem(void);
extern void suite_security(void);
extern void suite_yaml(void);
extern void suite_integration(void);
extern void suite_lang_contract(void);
extern void suite_edge_imports(void);
extern void suite_edge_structural(void);
extern void suite_lsp_resolution_probe(void);
extern void suite_node_creation_probe(void);
extern void suite_edge_types_probe(void);
extern void suite_convergence_probe(void);
extern void suite_matrix_known_classes(void);
extern void suite_matrix_new_constructs(void);
extern void suite_grammar_probe_a(void);
extern void suite_grammar_probe_b(void);
extern void suite_grammar_probe_c(void);
extern void suite_grammar_probe_d(void);
extern void suite_grammar_probe_e(void);
extern void suite_grammar_probe_f(void);
extern void suite_grammar_probe_g(void);
extern void suite_incremental(void);
extern void suite_zova_incremental_native(void);
extern void suite_semantic(void);
extern void suite_ast_profile(void);
extern void suite_slab_alloc(void);
extern void suite_simhash(void);
extern void suite_stack_overflow(void);
extern void suite_dump_verify(void);
extern void suite_zova(void);
extern void suite_zova_normalization_benchmark(void);
extern void suite_zova_operations(void);
extern void suite_zova_c_sql_functions(void);
extern void suite_zova_bridge(void);
extern void suite_zova_plugin(void);

/* Free the main thread's thread-local node-type bitset cache before exit so
 * LeakSanitizer (Linux x64) doesn't report it. Worker threads free their own
 * caches at thread teardown (pass_parallel.c). */
extern void cbm_kind_in_set_free_cache(void);

/* Isolated HOME and cache for a direct run. scripts/zova-run-tests.sh sets
 * HOME and CBM_CACHE_DIR to a temp dir, but the raw runner and the
 * `zig build test*` steps inherit the user's own, and suites that reset the
 * catalog (th_remove_zova_user_database) or upsert fixture projects would then
 * act on the live ~/.cache/codebase-memory-mcp. Mirror the script's layout
 * when CBM_CACHE_DIR is unset. Returns the temp root to remove, else "". */
static char g_isolated_root[1024];

static const char *tf_isolate_user_cache(void) {
    if (getenv("CBM_CACHE_DIR")) {
        return "";
    }
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) {
        base = cbm_tmpdir();
    }
    snprintf(g_isolated_root, sizeof(g_isolated_root), "%s/cbm-test-env-XXXXXX", base);
    if (!cbm_mkdtemp(g_isolated_root)) {
        fprintf(stderr, "test runner: cannot create an isolated cache under %s\n", base);
        exit(2);
    }
    char home[1100];
    char cache[1200];
    snprintf(home, sizeof(home), "%s/home", g_isolated_root);
    snprintf(cache, sizeof(cache), "%s/.cache/codebase-memory-mcp", home);
    if (!cbm_mkdir_p(cache, 0755)) {
        fprintf(stderr, "test runner: cannot create %s\n", cache);
        exit(2);
    }
    cbm_setenv("HOME", home, 1);
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    return g_isolated_root;
}

int main(int argc, char **argv) {
    /* #56: never let a caller's GIT_DIR/GIT_INDEX_FILE/... redirect fixture
     * git commands at the caller's real repository. */
    th_clear_git_repo_env();
    /* #138: spilling refuses to start below 10 GB of free disk. Tests that
     * spill must not depend on the machine's disk, so report plenty; the floor
     * has its own test, which pins a low value. Set before the worker branch
     * below so a re-executed worker sees it too. */
    (void)cbm_result_spill_pin_free_bytes_for_tests(UINT64_C(1) << 40);
    /* #798 follow-up: if spawned as the socket-isolation probe, report whether an
     * inheritable socket handle crossed into this child and exit before any suite. */
    int probe_rc = tf_maybe_run_socket_probe(argc, argv);
    if (probe_rc >= 0) {
        return probe_rc;
    }

    /* #832: if spawned as a supervised index worker, do the real work and exit
     * before any suite runs (see tf_maybe_run_index_worker). */
    int worker_rc = tf_maybe_run_index_worker(argc, argv);
    if (worker_rc >= 0) {
        return worker_rc;
    }
    int deleted_self_rc = tf_maybe_run_deleted_self_probe(argc, argv);
    if (deleted_self_rc >= 0) {
        return deleted_self_rc;
    }

    /* #845 belt-and-suspenders: this binary EMBEDS cbm_mcp_handle_tool. The
     * supervisor gate already ignores unmarked hosts, but pin the kill switch
     * too so even a future supervisor-marked test host can never resolve THIS
     * binary as `<self> cli --index-worker …` and recursively re-run suites.
     * A test that exercises the supervisor must explicitly re-enable it. */
    cbm_setenv("CBM_INDEX_SUPERVISOR", "0", 1);
    const char *isolated_root = tf_isolate_user_cache();
    g_suite_argc = argc;
    g_suite_argv = argv;
    printf("\n  codebase-memory-mcp  C test suite\n");

#if defined(CBM_TEST_RUNNER_ZOVA)
    RUN_SELECTED_SUITE(zova_operations);
    RUN_SELECTED_SUITE(zova);
    RUN_EXPLICIT_SUITE(zova_normalization_benchmark);
    RUN_SELECTED_SUITE(zova_c_sql_functions);
    RUN_SELECTED_SUITE(zova_bridge);
    RUN_SELECTED_SUITE(zova_plugin);
    RUN_SELECTED_SUITE(zova_incremental_native);
#elif defined(CBM_TEST_RUNNER_MCP)
    RUN_SELECTED_SUITE(mcp);
#elif defined(CBM_TEST_RUNNER_PIPELINE)
    RUN_SELECTED_SUITE(graph_buffer);
    RUN_SELECTED_SUITE(registry);
    RUN_SELECTED_SUITE(pipeline);
    RUN_SELECTED_SUITE(spawns);
    RUN_SELECTED_SUITE(importance);
    RUN_SELECTED_SUITE(index_resilience);
    RUN_SELECTED_SUITE(call_reference);
    RUN_SELECTED_SUITE(fqn);
    RUN_SELECTED_SUITE(route_canon);
    RUN_SELECTED_SUITE(path_alias);
    RUN_SELECTED_SUITE(configlink);
    RUN_SELECTED_SUITE(infrascan);
    RUN_SELECTED_SUITE(worker_pool);
    RUN_SELECTED_SUITE(parallel);
    RUN_SELECTED_SUITE(incremental);
#else
    /* Foundation */
    RUN_SELECTED_SUITE(arena);
    RUN_SELECTED_SUITE(hash_table);
    RUN_SELECTED_SUITE(dyn_array);
    RUN_SELECTED_SUITE(str_intern);
    RUN_SELECTED_SUITE(log);
    RUN_SELECTED_SUITE(str_util);
    RUN_SELECTED_SUITE(platform);
    RUN_SELECTED_SUITE(subprocess);
    RUN_SELECTED_SUITE(dump_verify);

    /* Existing C code regression tests */
    RUN_SELECTED_SUITE(extraction);
    RUN_SELECTED_SUITE(parse_coverage);
    RUN_SELECTED_SUITE(extraction_inheritance);
    RUN_SELECTED_SUITE(extraction_imports);
    RUN_SELECTED_SUITE(grammar_regression);
    RUN_SELECTED_SUITE(grammar_labels);
    RUN_SELECTED_SUITE(grammar_imports);

    /* Store (M5) */
    RUN_SELECTED_SUITE(store_nodes);
    RUN_SELECTED_SUITE(store_edges);
    RUN_SELECTED_SUITE(store_search);

    /* Cypher (M6) */
    RUN_SELECTED_SUITE(cypher);

    RUN_SELECTED_SUITE(zova_operations);

    /* MCP Server (M9) */
    RUN_SELECTED_SUITE(mcp);

    /* Discover (M2) */
    RUN_SELECTED_SUITE(language);
    RUN_SELECTED_SUITE(userconfig);
    RUN_SELECTED_SUITE(gitignore);
    RUN_SELECTED_SUITE(git_context);
    RUN_SELECTED_SUITE(discover);

    /* Graph Buffer (M7) */
    RUN_SELECTED_SUITE(graph_buffer);

    /* Pipeline (M8) */
    RUN_SELECTED_SUITE(registry);
    RUN_SELECTED_SUITE(pipeline);
    RUN_SELECTED_SUITE(spawns);
    RUN_SELECTED_SUITE(importance);
    RUN_SELECTED_SUITE(index_resilience);
    RUN_SELECTED_SUITE(call_reference);
    RUN_SELECTED_SUITE(fqn);
    RUN_SELECTED_SUITE(route_canon);
    RUN_SELECTED_SUITE(path_alias);

    /* Watcher (M10) */
    RUN_SELECTED_SUITE(watcher);

    /* LSP resolvers */
    RUN_SELECTED_SUITE(scope);
    RUN_SELECTED_SUITE(type_rep);
    RUN_SELECTED_SUITE(go_lsp);
    RUN_SELECTED_SUITE(c_lsp);
    RUN_SELECTED_SUITE(php_lsp);
    RUN_SELECTED_SUITE(cs_lsp);
    RUN_SELECTED_SUITE(cs_lsp_bench);
    RUN_SELECTED_SUITE(py_lsp);
    RUN_SELECTED_SUITE(kotlin_lsp);
    RUN_SELECTED_SUITE(rust_lsp);
    RUN_SELECTED_SUITE(zig_lsp);
    RUN_SELECTED_SUITE(py_lsp_bench);
    RUN_SELECTED_SUITE(py_lsp_stress);
    RUN_SELECTED_SUITE(py_lsp_scale);
    RUN_SELECTED_SUITE(ts_lsp);
    RUN_SELECTED_SUITE(java_lsp);
    RUN_SELECTED_SUITE(java_lsp_coverage);

    /* Architecture + ADR + Louvain */
    RUN_SELECTED_SUITE(store_arch);

    /* HTTP link */

    /* Traces helpers */

    /* Config link */
    RUN_SELECTED_SUITE(configlink);

    /* Infrastructure scanning */
    RUN_SELECTED_SUITE(infrascan);

    /* CLI (install, update, config) */
    RUN_SELECTED_SUITE(cli);

    /* System info + worker pool (parallelism) */
    RUN_SELECTED_SUITE(system_info);
    RUN_SELECTED_SUITE(worker_pool);

    /* Parallel pipeline */
    RUN_SELECTED_SUITE(parallel);

    /* mem + arena + slab integration */
    RUN_SELECTED_SUITE(slab_alloc);
    RUN_SELECTED_SUITE(mem);

    /* Security defenses */
    RUN_SELECTED_SUITE(security);

    /* YAML parser */
    RUN_SELECTED_SUITE(yaml);

    /* SimHash / SIMILAR_TO */
    RUN_SELECTED_SUITE(semantic);
    RUN_SELECTED_SUITE(ast_profile);
    RUN_SELECTED_SUITE(simhash);

    /* Stack overflow regression (GitHub #199) */
    RUN_SELECTED_SUITE(stack_overflow);

    /* Optional Zova migration adapter */
    RUN_SELECTED_SUITE(zova);
    RUN_EXPLICIT_SUITE(zova_normalization_benchmark);
    RUN_SELECTED_SUITE(zova_c_sql_functions);
    RUN_SELECTED_SUITE(zova_bridge);
    RUN_SELECTED_SUITE(zova_plugin);

    /* Integration (end-to-end) */
    RUN_SELECTED_SUITE(integration);

    /* Per-language graph contracts (node/edge types, attribution, no-crash) */
    RUN_SELECTED_SUITE(lang_contract);
    RUN_SELECTED_SUITE(edge_imports);
    RUN_SELECTED_SUITE(edge_structural);
    RUN_SELECTED_SUITE(lsp_resolution_probe);
    RUN_SELECTED_SUITE(node_creation_probe);
    RUN_SELECTED_SUITE(edge_types_probe);
    RUN_SELECTED_SUITE(convergence_probe);
    RUN_SELECTED_SUITE(matrix_known_classes);
    RUN_SELECTED_SUITE(matrix_new_constructs);
    RUN_SELECTED_SUITE(grammar_probe_a);
    RUN_SELECTED_SUITE(grammar_probe_b);
    RUN_SELECTED_SUITE(grammar_probe_c);
    RUN_SELECTED_SUITE(grammar_probe_d);
    RUN_SELECTED_SUITE(grammar_probe_e);
    RUN_SELECTED_SUITE(grammar_probe_f);
    RUN_SELECTED_SUITE(grammar_probe_g);

    RUN_SELECTED_SUITE(incremental);
    RUN_SELECTED_SUITE(zova_incremental_native);
#endif

    /* Release process-lifetime caches so LeakSanitizer reports no leaks. */
    cbm_kind_in_set_free_cache();
    sqlite3_shutdown();
    if (isolated_root[0]) {
        th_rmtree(isolated_root);
    }
    TEST_SUMMARY();
}
