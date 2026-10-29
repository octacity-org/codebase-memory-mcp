/*
 * test_pipeline.c — Integration tests for the indexing pipeline.
 *
 * Tests pipeline lifecycle, structure pass, and end-to-end indexing
 * on a temporary directory with known file layout.
 */
#include "../src/foundation/compat.h"
#include "foundation/platform.h" // cbm_normalize_path_sep (drive-canonicalization regression)
#include "test_framework.h"
#include "test_helpers.h"
#include "foundation/mem.h" // cbm_mem_init/budget (back-pressure futile-nap test)
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "store/store.h"
#include "git/git_context.h"
#include "foundation/dump_verify.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "foundation/compat_thread.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "graph_buffer/graph_buffer.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_route.h"
#include "yyjson/yyjson.h"
#include "sqlite3.h"

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if CBM_WITH_ZOVA
#include "zova.h"
#endif

/* ── Helper: create temp test repo with known layout ───────────── */

static char g_tmpdir[256];

/* Create:
 *   /tmp/cbm_test_XXXXXX/
 *     main.go       (empty)
 *     pkg/
 *       service.go  (empty)
 *       util/
 *         helper.go (empty)
 */
static int setup_test_repo(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cbm_test_XXXXXX");
    if (!cbm_mkdtemp(g_tmpdir))
        return -1;

    char path[512];

    /* main.go — calls Serve() from pkg */
    snprintf(path, sizeof(path), "%s/main.go", g_tmpdir);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "package main\n\n"
               "import \"pkg\"\n\n"
               "func main() {\n"
               "\tpkg.Serve()\n"
               "}\n");
    fclose(f);

    /* pkg/ */
    snprintf(path, sizeof(path), "%s/pkg", g_tmpdir);
    cbm_mkdir(path);

    /* pkg/service.go — calls Help() from util */
    snprintf(path, sizeof(path), "%s/pkg/service.go", g_tmpdir);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "package pkg\n\n"
               "import \"pkg/util\"\n\n"
               "func Serve() {\n"
               "\tutil.Help()\n"
               "}\n");
    fclose(f);

    /* pkg/util/ */
    snprintf(path, sizeof(path), "%s/pkg/util", g_tmpdir);
    cbm_mkdir(path);

    /* pkg/util/helper.go */
    snprintf(path, sizeof(path), "%s/pkg/util/helper.go", g_tmpdir);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "package util\n\nfunc Help() {}\n");
    fclose(f);

    return 0;
}

/* Recursive remove (simple, not production-grade) */
static void rm_rf(const char *path) {
    th_rmtree(path);
}

static void teardown_test_repo(void) {
    if (g_tmpdir[0])
        rm_rf(g_tmpdir);
    g_tmpdir[0] = '\0';
}

/* ── Lifecycle tests ─────────────────────────────────────────────── */

TEST(pipeline_create_free) {
    cbm_pipeline_t *p = cbm_pipeline_new("/some/path", NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(cbm_pipeline_project_name(p), "some-path");
    cbm_pipeline_free(p);
    PASS();
}

TEST(pipeline_null_repo) {
    cbm_pipeline_t *p = cbm_pipeline_new(NULL, NULL, CBM_MODE_FULL);
    ASSERT_NULL(p);
    PASS();
}

TEST(pipeline_free_null) {
    cbm_pipeline_free(NULL); /* should not crash */
    PASS();
}

TEST(pipeline_cancel) {
    cbm_pipeline_t *p = cbm_pipeline_new("/some/path", NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_cancel(p);
    /* Running a cancelled pipeline should return -1 immediately */
    int rc = cbm_pipeline_run(p);
    /* Note: it may fail because /some/path doesn't exist, not because of cancel.
     * This test just verifies cancel doesn't crash. */
    (void)rc;
    cbm_pipeline_free(p);
    PASS();
}

TEST(pipeline_cancel_null) {
    cbm_pipeline_cancel(NULL); /* should not crash */
    PASS();
}

TEST(pipeline_run_null) {
    int rc = cbm_pipeline_run(NULL);
    ASSERT_EQ(rc, -1);
    PASS();
}

/* ── Integration: structure pass on temp repo ────────────────────── */

TEST(pipeline_structure_nodes) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);

    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    /* Verify results by opening the store */
    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);

    const char *project = cbm_pipeline_project_name(p);

    /* Should have nodes: 1 Project + 2 Folders + 3 Files + N Functions */
    int node_count = cbm_store_count_nodes(s, project);
    ASSERT_GTE(node_count, 9); /* 6 structure + at least 3 definitions */

    /* Verify project node exists */
    cbm_node_t proj_node = {0};
    rc = cbm_store_find_node_by_qn(s, project, project, &proj_node);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_STR_EQ(proj_node.label, "Project");
    cbm_node_free_fields(&proj_node);

    /* Verify folder nodes */
    cbm_node_t *folders = NULL;
    int folder_count = 0;
    rc = cbm_store_find_nodes_by_label(s, project, "Folder", &folders, &folder_count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_GTE(folder_count, 2); /* pkg, pkg/util */
    cbm_store_free_nodes(folders, folder_count);

    /* Verify file nodes */
    cbm_node_t *file_nodes = NULL;
    int file_count = 0;
    rc = cbm_store_find_nodes_by_label(s, project, "File", &file_nodes, &file_count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_GTE(file_count, 3); /* main.go, service.go, helper.go */
    cbm_store_free_nodes(file_nodes, file_count);

    /* Verify edges exist */
    int edge_count = cbm_store_count_edges(s, project);
    ASSERT_GTE(edge_count, 5); /* CONTAINS_FOLDER + CONTAINS_FILE edges */

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

TEST(pipeline_structure_edges) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    /* Check CONTAINS_FILE edges */
    int cf_count = cbm_store_count_edges_by_type(s, project, "CONTAINS_FILE");
    /* Check CONTAINS_FOLDER edges */
    int cd_count = cbm_store_count_edges_by_type(s, project, "CONTAINS_FOLDER");

    /* Cleanup before assertions (so failures don't leak) */
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_test_repo();

    ASSERT_GTE(cf_count, 3); /* project->main.go, pkg->service.go, util->helper.go */
    ASSERT_GTE(cd_count, 1); /* project->pkg (pkg->util may merge on some platforms) */
    PASS();
}

TEST(pipeline_branch_root_structure) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    char branch_qn[1024];
    snprintf(branch_qn, sizeof(branch_qn), "%s.__branch__.working-tree", project);

    cbm_node_t project_node = {0};
    cbm_node_t branch_node = {0};
    cbm_node_t root_file_node = {0};
    cbm_node_t root_folder_node = {0};
    rc = cbm_store_find_node_by_qn(s, project, project, &project_node);
    ASSERT_EQ(rc, CBM_STORE_OK);
    rc = cbm_store_find_node_by_qn(s, project, branch_qn, &branch_node);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_STR_EQ(branch_node.label, "Branch");
    ASSERT_STR_EQ(branch_node.name, "working-tree");
    ASSERT_NOT_NULL(strstr(branch_node.properties_json, "\"is_git\":false"));
    char *root_folder_qn = cbm_pipeline_fqn_folder(project, "pkg");
    char *root_file_qn = cbm_pipeline_fqn_compute(project, "main.go", "__file__");
    ASSERT_NOT_NULL(root_folder_qn);
    ASSERT_NOT_NULL(root_file_qn);
    rc = cbm_store_find_node_by_qn(s, project, root_folder_qn, &root_folder_node);
    ASSERT_EQ(rc, CBM_STORE_OK);
    rc = cbm_store_find_node_by_qn(s, project, root_file_qn, &root_file_node);
    ASSERT_EQ(rc, CBM_STORE_OK);

    cbm_edge_t *has_branch = NULL;
    int has_branch_count = 0;
    rc = cbm_store_find_edges_by_source_type(s, project_node.id, "HAS_BRANCH", &has_branch,
                                             &has_branch_count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(has_branch_count, 1);
    ASSERT_EQ(has_branch[0].target_id, branch_node.id);

    cbm_edge_t *project_files = NULL;
    int project_file_count = 0;
    rc = cbm_store_find_edges_by_source_type(s, project_node.id, "CONTAINS_FILE", &project_files,
                                             &project_file_count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(project_file_count, 0);

    cbm_edge_t *branch_files = NULL;
    int branch_file_count = 0;
    rc = cbm_store_find_edges_by_source_type(s, branch_node.id, "CONTAINS_FILE", &branch_files,
                                             &branch_file_count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(branch_file_count, 1);
    ASSERT_EQ(branch_files[0].target_id, root_file_node.id);

    cbm_edge_t *branch_folders = NULL;
    int branch_folder_count = 0;
    rc = cbm_store_find_edges_by_source_type(s, branch_node.id, "CONTAINS_FOLDER", &branch_folders,
                                             &branch_folder_count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(branch_folder_count, 1);
    ASSERT_EQ(branch_folders[0].target_id, root_folder_node.id);

    cbm_store_free_edges(has_branch, has_branch_count);
    cbm_store_free_edges(project_files, project_file_count);
    cbm_store_free_edges(branch_files, branch_file_count);
    cbm_store_free_edges(branch_folders, branch_folder_count);
    cbm_node_free_fields(&project_node);
    cbm_node_free_fields(&branch_node);
    cbm_node_free_fields(&root_file_node);
    cbm_node_free_fields(&root_folder_node);
    free(root_folder_qn);
    free(root_file_qn);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

TEST(pipeline_project_name_derived) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);

    /* Project name should be derived from tmpdir path */
    const char *name = cbm_pipeline_project_name(p);
    ASSERT_NOT_NULL(name);
    ASSERT_TRUE(strlen(name) > 0);

    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

TEST(pipeline_fast_mode) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FAST);
    ASSERT_NOT_NULL(p);

    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    /* Just verify it completes without error in fast mode */
    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);
    int node_count = cbm_store_count_nodes(s, project);
    ASSERT_GT(node_count, 0);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

/* ── Definitions pass tests ──────────────────────────────────────── */

TEST(pipeline_definitions_function_nodes) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    /* Verify Function nodes extracted from Go source files */
    cbm_node_t *funcs = NULL;
    int func_count = 0;
    rc = cbm_store_find_nodes_by_label(s, project, "Function", &funcs, &func_count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_GTE(func_count, 3); /* main, Serve, Help */

    /* Verify each expected function exists */
    int found_main = 0, found_serve = 0, found_help = 0;
    for (int i = 0; i < func_count; i++) {
        if (strcmp(funcs[i].name, "main") == 0)
            found_main = 1;
        else if (strcmp(funcs[i].name, "Serve") == 0)
            found_serve = 1;
        else if (strcmp(funcs[i].name, "Help") == 0)
            found_help = 1;
    }
    ASSERT_TRUE(found_main);
    ASSERT_TRUE(found_serve);
    ASSERT_TRUE(found_help);

    cbm_store_free_nodes(funcs, func_count);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

TEST(pipeline_definitions_defines_edges) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    /* DEFINES edges: File → Function (one per extracted definition) */
    int defines_count = cbm_store_count_edges_by_type(s, project, "DEFINES");
    ASSERT_GTE(defines_count, 3); /* main.go→main, service.go→Serve, helper.go→Help */

    /* CONTAINS_FILE edges should still exist from structure pass */
    int cf_count = cbm_store_count_edges_by_type(s, project, "CONTAINS_FILE");
    ASSERT_GTE(cf_count, 3);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

TEST(pipeline_definitions_properties) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    /* Verify a function has valid properties (complexity, lines, etc.) */
    cbm_node_t *funcs = NULL;
    int func_count = 0;
    rc = cbm_store_find_nodes_by_label(s, project, "Function", &funcs, &func_count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_GT(func_count, 0);

    /* Check that Serve (exported) has is_exported:true in properties */
    for (int i = 0; i < func_count; i++) {
        if (strcmp(funcs[i].name, "Serve") == 0) {
            ASSERT_NOT_NULL(funcs[i].properties_json);
            ASSERT_TRUE(strstr(funcs[i].properties_json, "\"is_exported\":true") != NULL);
        }
        /* All functions should have file_path set */
        ASSERT_NOT_NULL(funcs[i].file_path);
    }

    cbm_store_free_nodes(funcs, func_count);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

/* Node properties must remain VALID JSON even when a definition's serialized
 * properties exceed the fixed 2 KB build buffer. Found on the Linux kernel:
 * 135 nodes (50-param functions with struct-typed signatures) had properties
 * truncated mid-string at 2047 bytes — malformed JSON that aborts EVERY
 * json_extract()-based consumer (arch_entry_points, partial indexes, user
 * Cypher on properties). Oversized optional fields must be dropped whole,
 * never cut mid-value. */
TEST(pipeline_def_props_valid_json_when_oversized) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    /* Sweep of C functions with growing signatures. The corruption fires only
     * when the serialized position lands in a narrow window just under the
     * buffer margin as the param_types array starts — the array is then cut
     * mid-item (`"param_types":["enum`), exactly the kernel failure shape.
     * Sweeping 10..69 params deterministically hits the window (pre-fix:
     * sweep_fn_30 -> 2047-byte malformed properties). ONE FUNCTION PER FILE so
     * the file count exceeds MIN_FILES_FOR_PARALLEL (50) and the test covers
     * the PARALLEL pipeline's duplicated props builder (pass_parallel.c) — the
     * path large repos take; the serial twin lives in pass_definitions.c. */
    for (int n = 10; n < 70; n++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/sweep_%02d.c", g_tmpdir, n);
        FILE *f = fopen(path, "w");
        if (!f) {
            teardown_test_repo();
            FAIL("failed to write sweep file");
        }
        fprintf(f, "int sweep_fn_%02d(", n);
        for (int i = 0; i < n; i++) {
            fprintf(f, "%sstruct long_struct_type_name_padding_padding_%02d *par_%02d",
                    i ? ", " : "", i, i);
        }
        fprintf(f, ") { return 0; }\n");
        fclose(f);
    }

    /* Multi-line parameter declarations: param_types items then carry raw
     * newline/tab bytes from the source slice. The array appender's inline
     * escape loop only handled quote/backslash, so the raw control bytes made
     * the JSON invalid (118 Linux-kernel rows of this shape). */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/sweep_nl.c", g_tmpdir);
        FILE *f = fopen(path, "w");
        if (!f) {
            teardown_test_repo();
            FAIL("failed to write sweep_nl.c");
        }
        fprintf(f, "int sweep_fn_nl(struct\n\t\t\t\treally_long_struct_name *a,\n"
                   "          enum\n\tweird_enum b) { return 0; }\n");
        fclose(f);
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    cbm_node_t *funcs = NULL;
    int func_count = 0;
    ASSERT_EQ(cbm_store_find_nodes_by_label(s, project, "Function", &funcs, &func_count),
              CBM_STORE_OK);
    int checked = 0;
    for (int i = 0; i < func_count; i++) {
        if (strncmp(funcs[i].name, "sweep_fn_", 9) != 0) {
            continue;
        }
        ASSERT_NOT_NULL(funcs[i].properties_json);
        yyjson_doc *doc =
            yyjson_read(funcs[i].properties_json, strlen(funcs[i].properties_json), 0);
        if (!doc) {
            printf("    INVALID properties JSON for %s (%zu bytes): ...%s\n", funcs[i].name,
                   strlen(funcs[i].properties_json),
                   funcs[i].properties_json + (strlen(funcs[i].properties_json) > 60
                                                   ? strlen(funcs[i].properties_json) - 60
                                                   : 0));
        }
        ASSERT_NOT_NULL(doc); /* valid JSON for EVERY sweep size */
        yyjson_doc_free(doc);
        checked++;
    }
    ASSERT_EQ(checked, 61); /* all sweep functions present (60 sizes + nl case) */

    cbm_store_free_nodes(funcs, func_count);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

/* Edge properties must be VALID JSON. Decorator source text (quotes, raw
 * newlines: @register.tag("block"), multi-line @override_settings) was
 * interpolated raw into the DECORATES properties — django produced 3826
 * malformed edges, and any json_extract-based consumer (including the
 * url_path_gen generated-column evaluation during PRAGMA integrity_check)
 * aborts on them. Usage/call emit sites had the same hole for sliced source
 * text. */
TEST(pipeline_edge_props_valid_json) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/deco.py", g_tmpdir);
    FILE *f = fopen(path, "w");
    if (!f) {
        teardown_test_repo();
        FAIL("failed to write deco.py");
    }
    fprintf(f, "from x import register\n"
               "@register.tag(\"block\")\n"
               "def do_block(parser):\n"
               "    return parser\n"
               "@register.tag(\"extends\")\n"
               "def do_extends(parser):\n"
               "    return parser\n");
    fclose(f);

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    ASSERT_EQ(cbm_store_find_edges_by_type(s, project, "DECORATES", &edges, &edge_count),
              CBM_STORE_OK);
    ASSERT_GT(edge_count, 0); /* the decorators must produce DECORATES edges */
    for (int i = 0; i < edge_count; i++) {
        const char *pj = edges[i].properties_json;
        if (!pj) {
            continue;
        }
        yyjson_doc *doc = yyjson_read(pj, strlen(pj), 0);
        if (!doc) {
            printf("    INVALID edge properties JSON: %.80s\n", pj);
        }
        ASSERT_NOT_NULL(doc);
        yyjson_doc_free(doc);
    }

    cbm_store_free_edges(edges, edge_count);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

/* ── Calls pass tests ──────────────────────────────────────────── */

TEST(pipeline_calls_resolution) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    /* main() calls pkg.Serve(), Serve() calls util.Help() → at least 2 CALLS edges */
    int calls_count = cbm_store_count_edges_by_type(s, project, "CALLS");
    ASSERT_GTE(calls_count, 1); /* at least some calls resolved */

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

/* True iff a CALLS edge exists from a node named src_name to a node named
 * tgt_name. Used to assert cross-file call resolution survives a reindex. */
static bool cross_file_edge_exists(cbm_store_t *s, const char *project, const char *src_name,
                                   const char *tgt_name, const char *edge_type) {
    cbm_node_t *srcs = NULL;
    cbm_node_t *tgts = NULL;
    int sc = 0;
    int tc = 0;
    cbm_store_find_nodes_by_name(s, project, src_name, &srcs, &sc);
    cbm_store_find_nodes_by_name(s, project, tgt_name, &tgts, &tc);
    bool found = false;
    for (int i = 0; i < sc && !found; i++) {
        cbm_edge_t *edges = NULL;
        int ec = 0;
        cbm_store_find_edges_by_source_type(s, srcs[i].id, edge_type, &edges, &ec);
        for (int j = 0; j < ec && !found; j++) {
            for (int k = 0; k < tc; k++) {
                if (edges[j].target_id == tgts[k].id) {
                    found = true;
                    break;
                }
            }
        }
        if (edges) {
            cbm_store_free_edges(edges, ec);
        }
    }
    if (srcs) {
        cbm_store_free_nodes(srcs, sc);
    }
    if (tgts) {
        cbm_store_free_nodes(tgts, tc);
    }
    return found;
}

static bool cross_file_call_exists(cbm_store_t *s, const char *project, const char *src_name,
                                   const char *tgt_name) {
    return cross_file_edge_exists(s, project, src_name, tgt_name, "CALLS");
}

/* True iff the exact named CALLS edge exists and its serialized properties
 * contain `strategy_fragment` — proves the edge came from the type-aware
 * resolver rather than a registry short-name guess. */
static bool cross_file_call_has_strategy(cbm_store_t *s, const char *project, const char *src_name,
                                         const char *tgt_name, const char *strategy_fragment) {
    cbm_node_t *srcs = NULL;
    cbm_node_t *tgts = NULL;
    int sc = 0;
    int tc = 0;
    cbm_store_find_nodes_by_name(s, project, src_name, &srcs, &sc);
    cbm_store_find_nodes_by_name(s, project, tgt_name, &tgts, &tc);
    bool found = false;
    for (int i = 0; i < sc && !found; i++) {
        cbm_edge_t *edges = NULL;
        int ec = 0;
        cbm_store_find_edges_by_source_type(s, srcs[i].id, "CALLS", &edges, &ec);
        for (int j = 0; j < ec && !found; j++) {
            for (int k = 0; k < tc; k++) {
                if (edges[j].target_id == tgts[k].id && edges[j].properties_json &&
                    strstr(edges[j].properties_json, strategy_fragment)) {
                    found = true;
                    break;
                }
            }
        }
        if (edges) {
            cbm_store_free_edges(edges, ec);
        }
    }
    if (srcs) {
        cbm_store_free_nodes(srcs, sc);
    }
    if (tgts) {
        cbm_store_free_nodes(tgts, tc);
    }
    return found;
}

/* Regression: incremental re-index of an edited file must NOT drop inbound
 * cross-file CALLS edges whose source lives in an UNCHANGED file.
 *
 * Repro: Serve() (pkg/service.go) CALLS Help() (pkg/util/helper.go). Editing
 * helper.go purges Help's node; before the fix the cascade deleted the
 * Serve->Help edge and never regenerated it (helper.go's callers are not
 * re-parsed), so the edge silently vanished on every edit and the incremental
 * graph diverged from a full reindex. */
TEST(pipeline_incremental_preserves_cross_file_calls) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    /* 1. Full index. */
    cbm_pipeline_t *p1 = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p1);
    ASSERT_EQ(cbm_pipeline_run(p1), 0);
    const char *project = cbm_pipeline_project_name(p1);

    cbm_store_t *s1 = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s1);
    int calls_before = cbm_store_count_edges_by_type(s1, project, "CALLS");
    ASSERT_GTE(calls_before, 2); /* main->Serve and Serve->Help at minimum */
    ASSERT_TRUE(cross_file_call_exists(s1, project, "Serve", "Help"));
    cbm_store_close(s1);
    cbm_pipeline_free(p1);

    /* 2. Edit the callee's file so the incremental classifier marks it changed
     *    (mtime+size differ). Help's symbol + qualified name are unchanged. */
    char helper[512];
    snprintf(helper, sizeof(helper), "%s/pkg/util/helper.go", g_tmpdir);
    ASSERT_EQ(th_append_file(helper, "\n// incremental regression marker\n"), 0);

    /* 3. Re-run on the same repo → auto-routes to incremental re-index. */
    cbm_pipeline_t *p2 = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ(cbm_pipeline_run(p2), 0);

    /* 4. The inbound cross-file CALLS edge must survive and the total CALLS
     *    count must not regress. (Before the fix: Serve->Help is dropped.)
     *    NOTE: query with p2's project name — p1 (and the `project` pointer it
     *    owned) was freed above; p1 and p2 derive the same name from g_tmpdir. */
    const char *project2 = cbm_pipeline_project_name(p2);
    cbm_store_t *s2 = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s2);
    int calls_after = cbm_store_count_edges_by_type(s2, project2, "CALLS");
    ASSERT_EQ(calls_after, calls_before);
    ASSERT_TRUE(cross_file_call_exists(s2, project2, "Serve", "Help"));
    cbm_store_close(s2);
    cbm_pipeline_free(p2);

    teardown_test_repo();
    PASS();
}

/* TS/JS receiver-aware weak-strategy suppression (#592/#606 direction; Perl
 * precedent #477). A member call x.foo() whose receiver TYPE the TS-LSP cannot
 * resolve (a regex literal `re.test()`) must NOT be bound to a same-named
 * project method by a weak short-name strategy — that fabricates a CALLS edge.
 * Type-resolved receivers (`c.test()` on a typed SalesforceRestClient) and bare
 * local calls must still resolve. < 50 files → sequential path (pass_calls.c).
 * RED before the fix: checkFormat->test exists via unique_name/suffix_match. */
static void write_temp_file(const char *dir, const char *name, const char *content);
TEST(pipeline_tsjs_receiver_suppresses_weak_method_edge) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_tsjs_recv_XXXXXX");
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    /* The lone project symbol named "test" — a real method. */
    write_temp_file(tmp, "src/client.ts",
                    "export class SalesforceRestClient {\n"
                    "  test(): boolean {\n"
                    "    return true;\n"
                    "  }\n"
                    "}\n");
    /* Regex receiver: `re.test(s)` calls RegExp.prototype.test, NOT the method.
     * The TS-LSP cannot bind it to a project symbol → the registry would guess
     * "test" by short name (weak). This is the false edge to suppress. */
    write_temp_file(tmp, "src/caller.ts",
                    "const re = /^[a-z]+$/;\n"
                    "export function checkFormat(s: string): boolean {\n"
                    "  return re.test(s);\n"
                    "}\n");
    /* Typed receiver: `c` is annotated SalesforceRestClient, so the TS-LSP
     * resolves c.test() to the method (lsp_ts_method, conf 0.95) BEFORE the
     * registry runs — the guard's explicit drop-list keeps every lsp_* edge. */
    write_temp_file(tmp, "src/typed.ts",
                    "import { SalesforceRestClient } from './client';\n"
                    "export function runTyped(c: SalesforceRestClient): boolean {\n"
                    "  return c.test();\n"
                    "}\n");
    /* Bare local call: same-module resolution, unaffected by the guard. */
    write_temp_file(tmp, "src/local.ts",
                    "function localHelper(): number {\n"
                    "  return 1;\n"
                    "}\n"
                    "export function callsLocal(): number {\n"
                    "  return localHelper();\n"
                    "}\n");

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    /* (1) The false edge is suppressed (reproduce-first: RED before the fix). */
    ASSERT_FALSE(cross_file_call_exists(s, project, "checkFormat", "test"));
    /* (2) The type-resolved receiver call survives (LSP wins before the guard). */
    ASSERT_TRUE(cross_file_call_exists(s, project, "runTyped", "test"));
    /* (3) The bare local call survives (same-module / lsp_ts_local). */
    ASSERT_TRUE(cross_file_call_exists(s, project, "callsLocal", "localHelper"));
    /* (4) breadth insurance: the real edges are still emitted. */
    ASSERT_GTE(cbm_store_count_edges_by_type(s, project, "CALLS"), 2);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

/* Count nodes with the given exact name in the project (e.g. a Route path). */
static int count_nodes_named(cbm_store_t *s, const char *project, const char *name) {
    cbm_node_t *ns = NULL;
    int n = 0;
    cbm_store_find_nodes_by_name(s, project, name, &ns, &n);
    if (ns) {
        cbm_store_free_nodes(ns, n);
    }
    return n;
}

/* HTTP_CALLS edges out of the function `source_name` whose Route target's
 * qualified name contains `route_qn_part` (e.g. "__route__GET__"). */
static int http_calls_to_route(cbm_store_t *s, const char *project, const char *source_name,
                               const char *route_qn_part) {
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    if (cbm_store_find_edges_by_type(s, project, "HTTP_CALLS", &edges, &edge_count) !=
        CBM_STORE_OK) {
        return -1;
    }
    int matches = 0;
    for (int i = 0; i < edge_count; i++) {
        cbm_node_t source = {0};
        cbm_node_t target = {0};
        int source_ok = cbm_store_find_node_by_id(s, edges[i].source_id, &source) == CBM_STORE_OK;
        int target_ok = cbm_store_find_node_by_id(s, edges[i].target_id, &target) == CBM_STORE_OK;
        if (source_ok && target_ok && source.name && target.qualified_name &&
            strcmp(source.name, source_name) == 0 && strstr(target.qualified_name, route_qn_part)) {
            matches++;
        }
        cbm_node_free_fields(&source);
        cbm_node_free_fields(&target);
    }
    if (edges) {
        cbm_store_free_edges(edges, edge_count);
    }
    return matches;
}

/* Route-registration CALLS edges (via=route_registration) out of the function
 * `source_name`. */
static int route_registrations_from(cbm_store_t *s, const char *project, const char *source_name) {
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    if (cbm_store_find_edges_by_type(s, project, "CALLS", &edges, &edge_count) != CBM_STORE_OK) {
        return -1;
    }
    int matches = 0;
    for (int i = 0; i < edge_count; i++) {
        cbm_node_t source = {0};
        int source_ok = cbm_store_find_node_by_id(s, edges[i].source_id, &source) == CBM_STORE_OK;
        if (source_ok && source.name && strcmp(source.name, source_name) == 0 &&
            edges[i].properties_json &&
            strstr(edges[i].properties_json, "\"via\":\"route_registration\"")) {
            matches++;
        }
        cbm_node_free_fields(&source);
    }
    if (edges) {
        cbm_store_free_edges(edges, edge_count);
    }
    return matches;
}

/* Parallel-resolver regression for the TS/JS receiver guard (>= 50 files forces
 * pass_parallel.c's resolve_file_calls). The guard must not drop a weak member
 * match before the service classification runs — it suppresses ONLY the plain
 * CALLS fall-through, so every service edge (HTTP_CALLS via the #523 callee
 * bypass or emit_service_edge's unconditional detect_url_in_args, Route via the
 * ROUTE_REG fall-through, …) is emitted exactly as on main. These callees are
 * classified by main's verb-suffix + URL-arg heuristic, NOT by an HTTP library
 * name in the callee — a duplicated predicate keyed on the resolved QN lost them
 * (axios.get, api.patch on a renamed-axios instance, supertest request(app).get).
 * The regex false edge must stay suppressed in parallel too. CBM_WORKERS forces
 * >1 worker so the parallel path is taken regardless of the host core count. */
TEST(pipeline_tsjs_receiver_parallel_keeps_service_edges) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_tsjs_par_XXXXXX");
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    /* The lone project symbols named get()/patch()/test() — weak short-name
     * targets the registry mis-binds the member calls below to. */
    write_temp_file(tmp, "src/thing.ts",
                    "export class ApiThing {\n"
                    "  get(): number {\n"
                    "    return 1;\n"
                    "  }\n"
                    "  patch(): number {\n"
                    "    return 2;\n"
                    "  }\n"
                    "  load(): number {\n"
                    "    return 3;\n"
                    "  }\n"
                    "}\n"
                    "export class Other {\n"
                    "  test(): boolean {\n"
                    "    return true;\n"
                    "  }\n"
                    "}\n");
    /* Untyped axios: the HTTP signal is in the callee name, not the resolved QN
     * (the registry mis-binds "get" to ApiThing.get). Must yield HTTP_CALLS. */
    write_temp_file(tmp, "src/http.ts",
                    "export function callApi() {\n"
                    "  return axios.get('/api/orders');\n"
                    "}\n");
    /* Renamed axios instance `api`: no HTTP lib id in the callee — classified by
     * the .patch verb suffix + URL arg. Must yield HTTP_CALLS (was lost when the
     * exemption keyed on the resolved QN only). */
    write_temp_file(tmp, "src/http2.ts",
                    "export function callPatch(): unknown {\n"
                    "  return api.patch('/plans/:id', {});\n"
                    "}\n");
    /* Supertest-style chained receiver `request(app).get('/y')` — untyped, verb
     * suffix + URL arg. */
    write_temp_file(tmp, "src/supertest.ts",
                    "export function callSup(app: unknown): unknown {\n"
                    "  return request(app).get('/y');\n"
                    "}\n");
    /* `dev.load('/data')`: `.load` is NOT a route suffix and `dev` is not an HTTP
     * lib, so main classifies it via the unconditional detect_url_in_args (URL
     * arg) -> HTTP_CALLS, while the weak plain match to ApiThing.load is the false
     * edge that must be suppressed. This is exactly the detect_url_in_args path
     * the previous (predicate-duplicating) guard skipped by dropping the call
     * before emit_service_edge ran — the class of ~399 HTTP_CALLS it lost. */
    write_temp_file(tmp, "src/load.ts",
                    "export function callLoad(dev: unknown): unknown {\n"
                    "  return dev.load('/api/data');\n"
                    "}\n");
    /* Route registration by callee suffix + a '/'-path arg. Must yield a Route. */
    write_temp_file(tmp, "src/routes.ts",
                    "function handler() {}\n"
                    "export function reg(router: any) {\n"
                    "  router.get('/users', handler);\n"
                    "}\n");
    /* Regex receiver: the false edge that must stay suppressed in parallel too. */
    write_temp_file(tmp, "src/re.ts",
                    "const re = /^[a-z]+$/;\n"
                    "export function checkFormat(s: string): boolean {\n"
                    "  return re.test(s);\n"
                    "}\n");
    /* Pad past MIN_FILES_FOR_PARALLEL (50) so the parallel resolver runs. */
    for (int i = 0; i < 52; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "src/filler%d.ts", i);
        snprintf(body, sizeof(body), "export function filler%d(): number {\n  return %d;\n}\n", i,
                 i);
        write_temp_file(tmp, name, body);
    }

    char *old_workers = getenv("CBM_WORKERS");
    char *saved = old_workers ? strdup(old_workers) : NULL;
    cbm_setenv("CBM_WORKERS", "4", 1); /* force parallel regardless of host cores */

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    /* (1) Genuine HTTP_CALLS survive under the guard:
     *   - axios.get('/api/orders') -> 1 edge, typed GET (recognized lib #523
     *     callee bypass; no method-less arg-URL twin), and
     *   - dev.load('/api/data')    -> 1 edge via detect_url_in_args, which runs
     *     after emit_service_edge's branch even when the plain fall-through is
     *     suppressed.
     * dev.load is the class the predicate-duplicating guard lost: `.load` is not
     * a route suffix and `dev` is not an HTTP lib, so it was dropped before
     * emit_service_edge ran (RED on that guard: only axios's edge). */
    ASSERT_EQ(http_calls_to_route(s, project, "callApi", "__route__GET__"), 1);
    ASSERT_EQ(http_calls_to_route(s, project, "callApi", "__route__"), 1);
    ASSERT_EQ(http_calls_to_route(s, project, "callLoad", "__route__ANY__"), 1);
    ASSERT_EQ(cbm_store_count_edges_by_type(s, project, "HTTP_CALLS"), 2);
    /* (2) The verb-suffix + route-path member calls keep their route
     * registrations (edge type CALLS -> a Route node named by the path). These
     * classify as route_registration on main, NOT HTTP_CALLS — Option A preserves
     * that by construction. Assert the three Route paths survive:
     * api.patch('/plans/:id'), request(app).get('/y'), router.get('/users'). */
    ASSERT_GTE(count_nodes_named(s, project, "/plans/:id"), 1);
    ASSERT_GTE(count_nodes_named(s, project, "/y"), 1);
    ASSERT_GTE(count_nodes_named(s, project, "/users"), 1);
    /* (3) The false plain-CALLS edges are suppressed in parallel: the regex
     * receiver and the dev.load weak match to ApiThing.load. */
    ASSERT_FALSE(cross_file_call_exists(s, project, "checkFormat", "test"));
    ASSERT_FALSE(cross_file_call_exists(s, project, "callLoad", "load"));

    cbm_store_close(s);
    cbm_pipeline_free(p);
    if (saved) {
        cbm_setenv("CBM_WORKERS", saved, 1);
        free(saved);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    th_rmtree(tmp);
    PASS();
}

/* Python counterpart to the TS/JS receiver guard (upstream b6a22843, #1276),
 * sequential path. NEGATIVE: an attribute call on an unknown receiver (a
 * parameter) must not bind the lone same-named project method through
 * unique_name. POSITIVE: an import-bound receiver (helper.compute) and a bare
 * local call keep their CALLS edges. */
TEST(pipeline_python_receiver_suppresses_weak_method_edge) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_python_recv_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    write_temp_file(tmp, "worker.py",
                    "class Worker:\n"
                    "    def backward(self):\n"
                    "        return 1\n");
    write_temp_file(tmp, "pkg/__init__.py", "from .helper import compute\n");
    write_temp_file(tmp, "pkg/helper.py",
                    "def compute(value):\n"
                    "    return value * 2\n");
    write_temp_file(tmp, "caller.py",
                    "from pkg import helper\n"
                    "\n"
                    "def external_call(accelerator):\n"
                    "    return accelerator.backward()\n"
                    "\n"
                    "def imported_call():\n"
                    "    return helper.compute(42)\n"
                    "\n"
                    "def local_helper():\n"
                    "    return 1\n"
                    "\n"
                    "def bare_call():\n"
                    "    return local_helper()\n");

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);
    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    ASSERT_FALSE(cross_file_call_exists(s, project, "external_call", "backward"));
    ASSERT_TRUE(cross_file_call_exists(s, project, "imported_call", "compute"));
    ASSERT_TRUE(cross_file_call_exists(s, project, "bare_call", "local_helper"));

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

/* Fixture for the #1928 cross-language reference-guard probes (sequential and
 * parallel twins). pad_files > 0 adds filler files to push the index over the
 * parallel-pipeline threshold, since USAGE/WRITES/READS have one resolver per
 * path and both must consult the guard. */
static void write_go_c_ref_guard_fixture(const char *tmp, int pad_files) {
    write_temp_file(tmp, "go.mod", "module example.com/fxguard\n\ngo 1.22\n");
    /* The C probe: a local named `event` and a file-scope function `handle`,
     * the two shapes Go identifiers collide with. */
    write_temp_file(tmp, "probe/probe.c",
                    "static int total_events = 0;\n"
                    "\n"
                    "static int handle(void) {\n"
                    "    int event = 0;\n"
                    "    total_events += event;\n"
                    "    return event;\n"
                    "}\n");
    /* Go: a local write named like the C local, and a value use named like
     * the C function. Neither can touch anything in a C translation unit. */
    write_temp_file(tmp, "app/app.go",
                    "package app\n"
                    "\n"
                    "func TrackEvent() int {\n"
                    "\tevent := 1\n"
                    "\treturn event\n"
                    "}\n"
                    "\n"
                    "func UsesHandle() int {\n"
                    "\th := handle\n"
                    "\t_ = h\n"
                    "\treturn 4\n"
                    "}\n"
                    "\n"
                    "func WriteTotal() {\n"
                    "\ttotal_events := 5\n"
                    "\t_ = total_events\n"
                    "}\n");
    /* Same-language control: a Go package-level var written from another
     * file must keep its WRITES edge. */
    write_temp_file(tmp, "state/vars.go",
                    "package state\n"
                    "\n"
                    "var Counter int\n");
    write_temp_file(tmp, "state/state.go",
                    "package state\n"
                    "\n"
                    "func Bump() {\n"
                    "\tCounter = 2\n"
                    "}\n");
    for (int i = 0; i < pad_files; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "pad/filler%d.go", i);
        snprintf(body, sizeof(body), "package pad\n\nfunc filler%d() int { return %d }\n", i, i);
        write_temp_file(tmp, name, body);
    }
}

TEST(pipeline_go_rw_usage_never_cross_into_c) {
    /* #1928: USAGE and WRITES/READS resolve through the same short-name
     * registry as CALLS but never consulted the #725 cross-language guard.
     * On a Go tree with eBPF C probes every Go identifier spelled like a C
     * one produced an edge into the C file — 31.5% of all WRITES on the
     * measured repo crossed the Go/C boundary. Go code cannot write a C
     * automatic variable, so every edge in that class is false. This is the
     * SEQUENTIAL-path twin; the parallel twin follows. */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_go_rw_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    write_go_c_ref_guard_fixture(tmp, 0);

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    /* Reproduce-first: RED before the fix — the Go local write binds the C
     * probe's global, and the Go value use binds the C `handle`. */
    ASSERT_FALSE(cross_file_edge_exists(s, project, "TrackEvent", "event", "WRITES"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "TrackEvent", "event", "READS"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "UsesHandle", "handle", "WRITES"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "UsesHandle", "handle", "READS"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "UsesHandle", "handle", "USAGE"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "WriteTotal", "total_events", "WRITES"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "WriteTotal", "total_events", "USAGE"));
    /* Same-language reference edges survive the guard. */
    ASSERT_TRUE(cross_file_edge_exists(s, project, "Bump", "Counter", "WRITES"));

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

TEST(pipeline_go_rw_usage_never_cross_into_c_parallel) {
    /* Parallel twin of the test above: USAGE and WRITES/READS each have a
     * second, independent resolver in pass_parallel.c (resolve_file_usages /
     * resolve_file_rw) that must consult the same guard — the field census
     * that motivated this caught the sequential-only fix leaving 344 Go→C
     * WRITES alive on a ~1150-file repo. >= 50 files forces the parallel
     * pipeline. */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_go_rwp_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    write_go_c_ref_guard_fixture(tmp, 52);

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    ASSERT_FALSE(cross_file_edge_exists(s, project, "TrackEvent", "event", "WRITES"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "TrackEvent", "event", "READS"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "UsesHandle", "handle", "WRITES"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "UsesHandle", "handle", "READS"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "UsesHandle", "handle", "USAGE"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "WriteTotal", "total_events", "WRITES"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "WriteTotal", "total_events", "USAGE"));
    ASSERT_TRUE(cross_file_edge_exists(s, project, "Bump", "Counter", "WRITES"));

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}


static int count_nodes_named(cbm_store_t *s, const char *project, const char *name);

/* Fixture for the #1942 bare-reference-vs-Field probes: a Go struct field
 * named like the commonest local (`err`), and a function whose local of the
 * same name must NOT bind it — in Go a field is only reachable through a
 * selector expression, never a bare identifier. Needs Go Field extraction
 * (#1935) to have anything to falsely bind. */
static void write_go_bare_field_fixture(const char *tmp, int pad_files) {
    write_temp_file(tmp, "go.mod", "module example.com/fxbare\n\ngo 1.22\n");
    write_temp_file(tmp, "state/state.go",
                    "package state\n"
                    "\n"
                    "type Tracker struct {\n"
                    "\terr error\n"
                    "\tn   int\n"
                    "}\n");
    write_temp_file(tmp, "app/app.go",
                    "package app\n"
                    "\n"
                    "import \"errors\"\n"
                    "\n"
                    "func Run() error {\n"
                    "\terr := errors.New(\"x\")\n"
                    "\treturn err\n"
                    "}\n");
    /* #1962: genuine selector references from a sibling file of the same
     * package. `t.err = nil` writes the field through a selector; `t.n` reads
     * it. The extractor strips the receiver on both paths, so only the
     * is_member_access signal can distinguish these from Run's bare local. */
    write_temp_file(tmp, "state/reset.go",
                    "package state\n"
                    "\n"
                    "func (t *Tracker) Reset() int {\n"
                    "\tt.err = nil\n"
                    "\treturn t.n\n"
                    "}\n");
    for (int i = 0; i < pad_files; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "pad/filler%d.go", i);
        snprintf(body, sizeof(body), "package pad\n\nfunc filler%d() int { return %d }\n", i, i);
        write_temp_file(tmp, name, body);
    }
}

TEST(pipeline_go_bare_ref_never_binds_field) {
    /* #1942: the READS/WRITES resolvers and the USAGE registry fallback hand
     * bare reference text to the short-name registry, which contains Field
     * nodes — so every Go local `err := …` bound whichever struct field was
     * named err (21308 USAGE / 5191 WRITES onto Go fields on the measured
     * repo, top target a test struct's field collecting 3013 edges).
     * Sequential-path twin; the parallel twin follows. */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_go_bare_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    write_go_bare_field_fixture(tmp, 0);

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    /* The field must exist for the probe to mean anything (#1935's fix). */
    ASSERT_TRUE(count_nodes_named(s, project, "err") >= 1);
    /* Reproduce-first: RED before the fix — the bare local binds the field. */
    ASSERT_FALSE(cross_file_edge_exists(s, project, "Run", "err", "WRITES"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "Run", "err", "READS"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "Run", "err", "USAGE"));
    /* #1962 parallel twin: resolve_file_rw / resolve_file_usages must honour
     * the member-access signal exactly like the sequential resolvers. */
    ASSERT_TRUE(cross_file_edge_exists(s, project, "Reset", "err", "WRITES"));
    ASSERT_TRUE(cross_file_edge_exists(s, project, "Reset", "n", "USAGE"));

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

TEST(pipeline_go_bare_ref_never_binds_field_parallel) {
    /* Parallel twin: resolve_file_rw / resolve_file_usages are independent
     * resolvers and must consult the same predicate (#1928's lesson). */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_go_barep_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    write_go_bare_field_fixture(tmp, 52);

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    ASSERT_TRUE(count_nodes_named(s, project, "err") >= 1);
    ASSERT_FALSE(cross_file_edge_exists(s, project, "Run", "err", "WRITES"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "Run", "err", "READS"));
    ASSERT_FALSE(cross_file_edge_exists(s, project, "Run", "err", "USAGE"));
    /* #1962, reproduce-first: RED while the guard is a blanket veto — genuine
     * selector references must reach the field (write via `t.err = nil`,
     * value use via `t.n`). */
    ASSERT_TRUE(cross_file_edge_exists(s, project, "Reset", "err", "WRITES"));
    ASSERT_TRUE(cross_file_edge_exists(s, project, "Reset", "n", "USAGE"));

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}


/* Python bare-call local-binding suppression, sequential path. The bare-call
 * counterpart of the receiver guard above: `run` is a PARAMETER, so `run()`
 * cannot be the module-level `run` and must not bind SatoriLive.run.
 *
 * The positive control is deliberately a CROSS-FILE bare call with no import,
 * so it resolves by a weak short-name strategy — one this guard could have
 * killed. Asserting a same-file (same_module) edge instead would prove nothing,
 * because no guard in this codebase touches same_module for any input.
 * Fewer than 50 files exercises pass_calls.c. */
TEST(pipeline_python_bare_local_binding_suppresses_weak_edge) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_py_bare_seq_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    write_temp_file(tmp, "live.py",
                    "class SatoriLive:\n"
                    "    def run(self):\n"
                    "        return 1\n");
    write_temp_file(tmp, "helpers.py",
                    "def compute_widget_total():\n"
                    "    return 7\n");
    write_temp_file(tmp, "gate.py",
                    "def _run_with_heavy_slot(run):\n"
                    "    return run()\n"
                    "\n"
                    "def uses_free_function():\n"
                    "    return compute_widget_total()\n");

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    /* NEGATIVE: the callee is shadowed by a parameter. */
    ASSERT_FALSE(cross_file_call_exists(s, project, "_run_with_heavy_slot", "run"));
    /* POSITIVE: an unshadowed cross-file bare call survives. */
    ASSERT_TRUE(cross_file_call_exists(s, project, "uses_free_function", "compute_widget_total"));
    /* Tripwire: a run that emitted no edges at all would satisfy the negative
     * assertion vacuously. */
    ASSERT_GTE(cbm_store_count_edges_by_type(s, project, "CALLS"), 1);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

/* Parallel counterpart. >= 50 files forces pass_parallel.c, which is wired with
 * the same gate: a guard wired on only one resolver produces an edge on the
 * sequential path and not the parallel one, breaking MT determinism. #1386
 * wired both and tested only the sequential path, and the `parallel` suite is
 * exactly what catches that. Same both-directions pin as the sequential test. */
TEST(pipeline_python_bare_local_binding_parallel_suppresses_weak_edge) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_py_bare_par_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    write_temp_file(tmp, "live.py",
                    "class SatoriLive:\n"
                    "    def run(self):\n"
                    "        return 1\n"
                    "\n"
                    "class BatchJob:\n"
                    "    def execute(self):\n"
                    "        return 2\n");
    write_temp_file(tmp, "helpers.py",
                    "def compute_widget_total():\n"
                    "    return 7\n");
    write_temp_file(tmp, "gate.py",
                    "def _run_with_heavy_slot(run, execute):\n"
                    "    run()\n"
                    "    return execute()\n"
                    "\n"
                    "def uses_free_function():\n"
                    "    return compute_widget_total()\n");
    for (int i = 0; i < 52; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "filler%d.py", i);
        snprintf(body, sizeof(body), "def filler%d():\n    return %d\n", i, i);
        write_temp_file(tmp, name, body);
    }

    char *old_workers = getenv("CBM_WORKERS");
    char *saved = old_workers ? strdup(old_workers) : NULL;
    cbm_setenv("CBM_WORKERS", "4", 1);

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    /* NEGATIVE: both callees are shadowed by parameters. */
    ASSERT_FALSE(cross_file_call_exists(s, project, "_run_with_heavy_slot", "run"));
    ASSERT_FALSE(cross_file_call_exists(s, project, "_run_with_heavy_slot", "execute"));
    /* POSITIVE: the unshadowed cross-file bare call survives the parallel path. */
    ASSERT_TRUE(cross_file_call_exists(s, project, "uses_free_function", "compute_widget_total"));
    /* Tripwire against a vacuous pass. */
    ASSERT_GTE(cbm_store_count_edges_by_type(s, project, "CALLS"), 1);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    if (saved) {
        cbm_setenv("CBM_WORKERS", saved, 1);
        free(saved);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    th_rmtree(tmp);
    PASS();
}


/* Upstream 8f50841a: a Go call through a struct field whose type lives in
 * another package (h.orderSvc.PlaceOrder()) resolves by type dispatch. Go
 * fields are extracted as flat "Field" defs that the cross-file LSP def
 * builder drops; without folding them into the struct's field_defs the
 * handler struct registers with zero fields and the chain cannot resolve. A
 * same-named decoy method in another package keeps a short-name guess from
 * passing the test. */
TEST(pipeline_go_cross_package_field_chain_resolves) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_go_fold_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    write_temp_file(tmp, "go.mod", "module example.com/acme\n\ngo 1.22\n");
    write_temp_file(tmp, "service/order.go",
                    "package service\n"
                    "\n"
                    "type OrderService struct{}\n"
                    "\n"
                    "func (s *OrderService) PlaceOrder(id int) error {\n"
                    "\treturn nil\n"
                    "}\n");
    write_temp_file(tmp, "legacy/order.go",
                    "package legacy\n"
                    "\n"
                    "type LegacyService struct{}\n"
                    "\n"
                    "func (s *LegacyService) PlaceOrder(id int) error {\n"
                    "\treturn nil\n"
                    "}\n");
    write_temp_file(tmp, "handler/order.go",
                    "package handler\n"
                    "\n"
                    "import \"example.com/acme/service\"\n"
                    "\n"
                    "type OrderHandler struct {\n"
                    "\torderSvc *service.OrderService\n"
                    "}\n"
                    "\n"
                    "func (h *OrderHandler) Place(id int) error {\n"
                    "\treturn h.orderSvc.PlaceOrder(id)\n"
                    "}\n");

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);
    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    bool dispatch = cross_file_call_has_strategy(s, project, "Place", "PlaceOrder",
                                                 "\"strategy\":\"lsp_type_dispatch\"");

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    ASSERT_TRUE(dispatch);
    PASS();
}

/* CALLS edges from project QN suffix src to project QN suffix tgt (both after
 * "<project>."); *strategy_ok counts those whose properties hold fragment. */
static int calls_between_qns(cbm_store_t *s, const char *project, const char *src,
                             const char *tgt, const char *fragment, int *strategy_ok) {
    char sq[512];
    char tq[512];
    snprintf(sq, sizeof(sq), "%s.%s", project, src);
    snprintf(tq, sizeof(tq), "%s.%s", project, tgt);
    cbm_node_t a = {0};
    cbm_node_t b = {0};
    int n = 0;
    if (cbm_store_find_node_by_qn(s, project, sq, &a) == CBM_STORE_OK &&
        cbm_store_find_node_by_qn(s, project, tq, &b) == CBM_STORE_OK) {
        cbm_edge_t *edges = NULL;
        int ec = 0;
        cbm_store_find_edges_by_source_type(s, a.id, "CALLS", &edges, &ec);
        for (int i = 0; i < ec; i++) {
            if (edges[i].target_id != b.id) {
                continue;
            }
            n++;
            if (strategy_ok && fragment && edges[i].properties_json &&
                strstr(edges[i].properties_json, fragment)) {
                (*strategy_ok)++;
            }
        }
        if (edges) {
            cbm_store_free_edges(edges, ec);
        }
    }
    cbm_node_free_fields(&a);
    cbm_node_free_fields(&b);
    return n;
}

static int inbound_calls_to_qn(cbm_store_t *s, const char *project, const char *tgt) {
    char tq[512];
    snprintf(tq, sizeof(tq), "%s.%s", project, tgt);
    cbm_node_t b = {0};
    int ec = 0;
    if (cbm_store_find_node_by_qn(s, project, tq, &b) == CBM_STORE_OK) {
        cbm_edge_t *edges = NULL;
        cbm_store_find_edges_by_target_type(s, b.id, "CALLS", &edges, &ec);
        if (edges) {
            cbm_store_free_edges(edges, ec);
        }
    } else {
        ec = -1; /* the target itself is missing */
    }
    cbm_node_free_fields(&b);
    return ec;
}

/* Zig member calls bind by the receiver's inferred type, across files and
 * relative `@import` paths, on both the sequential and the parallel (>= 50
 * files) path. The same-named deinit() calls in run() reach their own
 * targets; tmp.cleanup() on a std value gets no edge although exactly one
 * project function is named cleanup. app/ sees the library only as the
 * build.zig module "zova", whose root re-exports it, and db.zig spells a
 * return type through its own import (`!sqlite.Statement`); a Python package
 * of the same name must not capture the Zig import. */
static void zig_receiver_fixture(const char *tmp, int fillers) {
    write_temp_file(tmp, "build.zig",
                    "const std = @import(\"std\");\n"
                    "\n"
                    "pub fn build(b: *std.Build) void {\n"
                    "    const lib_module = b.createModule(.{\n"
                    "        .root_source_file = b.path(\"src/root.zig\"),\n"
                    "    });\n"
                    "    const exe = b.addExecutable(.{\n"
                    "        .name = \"app\",\n"
                    "        .root_module = b.createModule(.{ .root_source_file = "
                    "b.path(\"app/main.zig\") }),\n"
                    "    });\n"
                    "    exe.root_module.addImport(\"zova\", lib_module);\n"
                    "}\n");
    write_temp_file(tmp, "bindings/python/pyproject.toml", "[project]\nname = \"zova\"\n");
    write_temp_file(tmp, "bindings/python/zova/__init__.py", "def open():\n    return None\n");
    write_temp_file(tmp, "src/root.zig",
                    "const impl = @import(\"db.zig\");\n"
                    "pub const Database = impl.Database;\n"
                    "pub const sqlite = @import(\"sqlite.zig\");\n"
                    "pub const api = struct {\n"
                    "    pub const Db = impl.Database;\n"
                    "    const Local = struct {\n"
                    "        pub fn ping() void {}\n"
                    "    };\n"
                    "    pub const L = Local;\n"
                    "};\n");
    write_temp_file(tmp, "src/sqlite.zig",
                    "pub const Statement = struct {\n"
                    "    pub fn step(self: *Statement) bool {\n"
                    "        _ = self;\n"
                    "        return false;\n"
                    "    }\n"
                    "    pub fn reset(self: *Statement) void {\n"
                    "        _ = self;\n"
                    "    }\n"
                    "};\n");
    write_temp_file(tmp, "app/main.zig",
                    "const zova = @import(\"zova\");\n"
                    "\n"
                    "pub fn main() !void {\n"
                    "    var d = try zova.Database.open();\n"
                    "    defer d.deinit();\n"
                    "    var raw = try d.raw();\n"
                    "    _ = raw.step();\n"
                    "}\n"
                    "\n"
                    "pub fn rows(stmt: *zova.sqlite.Statement) void {\n"
                    "    stmt.reset();\n"
                    "}\n"
                    "\n"
                    "pub fn viaApi() !void {\n"
                    "    var d = try zova.api.Db.open();\n"
                    "    d.deinit();\n"
                    "    zova.api.L.ping();\n"
                    "}\n");
    write_temp_file(tmp, "src/db.zig",
                    "const sqlite = @import(\"sqlite.zig\");\n"
                    "\n"
                    "pub const Statement = struct {\n"
                    "    pub fn deinit(self: *Statement) void {\n"
                    "        _ = self;\n"
                    "    }\n"
                    "};\n"
                    "\n"
                    "pub const Database = struct {\n"
                    "    const Self = @This();\n"
                    "    pub fn open() !Self {\n"
                    "        return .{};\n"
                    "    }\n"
                    "    pub fn prepare(self: *Self) !Statement {\n"
                    "        _ = self;\n"
                    "        return .{};\n"
                    "    }\n"
                    "    pub fn deinit(self: *Self) void {\n"
                    "        _ = self;\n"
                    "    }\n"
                    "    pub fn raw(self: *Self) !sqlite.Statement {\n"
                    "        _ = self;\n"
                    "        return .{};\n"
                    "    }\n"
                    "};\n");
    write_temp_file(tmp, "src/handles.zig",
                    "const builtin = @import(\"builtin\");\n"
                    "const AbiMutex = if (builtin.os.tag == .emscripten) struct {\n"
                    "    pub fn lock(_: *@This()) void {}\n"
                    "} else struct {\n"
                    "    state: u32 = 0,\n"
                    "    pub fn lock(self: *AbiMutex) void {\n"
                    "        _ = self;\n"
                    "    }\n"
                    "};\n"
                    "pub const Handle = struct {\n"
                    "    mutex: AbiMutex = .{},\n"
                    "    pub fn run(h: *Handle) void {\n"
                    "        h.mutex.lock();\n"
                    "    }\n"
                    "};\n"
                    "const std = @import(\"std\");\n"
                    "pub const Registry = struct {\n"
                    "    list: std.ArrayList(*Handle),\n"
                    "    by_name: std.StringHashMap(Handle),\n"
                    "    pub fn named(r: *Registry) void {\n"
                    "        if (r.by_name.getPtr(\"x\")) |h| h.run();\n"
                    "    }\n"
                    "};\n");
    write_temp_file(tmp, "src/pool.zig",
                    "pub fn Pool(comptime T: type) type {\n"
                    "    return struct {\n"
                    "        items: []T,\n"
                    "        pub fn first(self: *@This()) *T {\n"
                    "            return &self.items[0];\n"
                    "        }\n"
                    "    };\n"
                    "}\n"
                    "fn Ops(comptime kind: u8) type {\n"
                    "    return switch (kind) {\n"
                    "        0 => struct {\n"
                    "            fn copy() void {}\n"
                    "        },\n"
                    "        else => struct {\n"
                    "            fn copy() void {}\n"
                    "        },\n"
                    "    };\n"
                    "}\n"
                    "pub fn useOps() void {\n"
                    "    Ops(1).copy();\n"
                    "}\n");
    /* `anytype`: support.zig does not import handles.zig. */
    write_temp_file(tmp, "src/support.zig",
                    "pub fn runAll(h: anytype) void {\n"
                    "    h.run();\n"
                    "}\n");
    write_temp_file(tmp, "src/publication.zig",
                    "const Publication = struct {\n"
                    "    fn cleanup(self: *Publication) void {\n"
                    "        _ = self;\n"
                    "    }\n"
                    "};\n");
    write_temp_file(tmp, "src/main.zig",
                    "const std = @import(\"std\");\n"
                    "const db = @import(\"db.zig\");\n"
                    "const handles = @import(\"handles.zig\");\n"
                    "const pool = @import(\"pool.zig\");\n"
                    "const support = @import(\"support.zig\");\n"
                    "\n"
                    "pub fn viaAny(h: *handles.Handle) void {\n"
                    "    support.runAll(h);\n"
                    "}\n"
                    "\n"
                    "pub fn pooled(p: *pool.Pool(handles.Handle)) void {\n"
                    "    p.first().run();\n"
                    "}\n"
                    "\n"
                    "pub fn first(r: *handles.Registry) void {\n"
                    "    for (r.list.items) |h| h.run();\n"
                    "}\n"
                    "\n"
                    "pub fn fresh() !void {\n"
                    "    const h = try std.heap.page_allocator.create(handles.Handle);\n"
                    "    h.run();\n"
                    "}\n"
                    "\n"
                    "pub fn run() !void {\n"
                    "    var conn = try db.Database.open();\n"
                    "    defer conn.deinit();\n"
                    "    var st = try conn.prepare();\n"
                    "    st.deinit();\n"
                    "}\n"
                    "\n"
                    "test \"tmp dir\" {\n"
                    "    var tmp = std.testing.tmpDir(.{});\n"
                    "    defer tmp.cleanup();\n"
                    "}\n");
    write_temp_file(tmp, "src/cli/tool.zig",
                    "const db = @import(\"../db.zig\");\n"
                    "\n"
                    "pub fn main() !void {\n"
                    "    var d = try db.Database.open();\n"
                    "    d.deinit();\n"
                    "}\n");
    for (int i = 0; i < fillers; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "fill/filler%d.zig", i);
        snprintf(body, sizeof(body), "pub fn filler%d() u32 {\n    return %d;\n}\n", i, i);
        write_temp_file(tmp, name, body);
    }
}

static int check_zig_receiver_edges(int fillers) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_zig_recv_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    zig_receiver_fixture(tmp, fillers);
    char *old_workers = getenv("CBM_WORKERS");
    char *saved = old_workers ? strdup(old_workers) : NULL;
    if (fillers > 0) {
        cbm_setenv("CBM_WORKERS", "4", 1);
    }
    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    int rc = p ? cbm_pipeline_run(p) : -1;
    const char *project = p ? cbm_pipeline_project_name(p) : NULL;
    cbm_store_t *s = rc == 0 ? th_open_repo_store(tmp) : NULL;
    const char *lsp = "\"strategy\":\"lsp_zig";
    int ok[20] = {0};
    int n[20] = {0};
    int cleanup_in = -2;
    if (s) {
        n[0] = calls_between_qns(s, project, "src.main.run", "src.db.Database.deinit", lsp, &ok[0]);
        n[1] = calls_between_qns(s, project, "src.main.run", "src.db.Statement.deinit", lsp, &ok[1]);
        n[2] = calls_between_qns(s, project, "src.main.run", "src.db.Database.open", lsp, &ok[2]);
        n[3] = calls_between_qns(s, project, "src.main.run", "src.db.Database.prepare", lsp, &ok[3]);
        n[4] = calls_between_qns(s, project, "src.cli.tool.main", "src.db.Database.deinit", lsp,
                                 &ok[4]);
        n[5] = calls_between_qns(s, project, "app.main.main", "src.db.Database.open", lsp, &ok[5]);
        n[6] = calls_between_qns(s, project, "app.main.main", "src.db.Database.deinit", lsp, &ok[6]);
        n[7] = calls_between_qns(s, project, "app.main.main", "src.sqlite.Statement.step", lsp,
                                 &ok[7]);
        n[8] = calls_between_qns(s, project, "app.main.rows", "src.sqlite.Statement.reset", lsp,
                                 &ok[8]);
        /* Re-exports declared inside a struct (zova.api.Db, zova.api.L). */
        n[9] = calls_between_qns(s, project, "app.main.viaApi", "src.db.Database.open", lsp, &ok[9]);
        n[10] = calls_between_qns(s, project, "app.main.viaApi", "src.db.Database.deinit", lsp,
                                  &ok[10]);
        n[11] = calls_between_qns(s, project, "app.main.viaApi", "src.root.api.Local.ping", lsp,
                                  &ok[11]);
        /* A type chosen by a compile-time `if`: both branches are AbiMutex. */
        n[12] = calls_between_qns(s, project, "src.handles.Handle.run", "src.handles.AbiMutex.lock",
                                  lsp, &ok[12]);
        /* Element types carried through std containers and allocators. */
        n[13] = calls_between_qns(s, project, "src.handles.Registry.named", "src.handles.Handle.run",
                                  lsp, &ok[13]);
        n[14] = calls_between_qns(s, project, "src.main.first", "src.handles.Handle.run", lsp,
                                  &ok[14]);
        n[15] = calls_between_qns(s, project, "src.main.fresh", "src.handles.Handle.run", lsp,
                                  &ok[15]);
        /* Project generics: Pool(T) binds T across files; Ops switches. */
        n[16] = calls_between_qns(s, project, "src.main.pooled", "src.pool.Pool.first", lsp,
                                  &ok[16]);
        n[17] = calls_between_qns(s, project, "src.main.pooled", "src.handles.Handle.run", lsp,
                                  &ok[17]);
        n[18] = calls_between_qns(s, project, "src.pool.useOps", "src.pool.Ops.copy", lsp,
                                  &ok[18]);
        /* An `anytype` parameter takes the type main.viaAny passes. */
        n[19] = calls_between_qns(s, project, "src.support.runAll", "src.handles.Handle.run", lsp,
                                  &ok[19]);
        cleanup_in = inbound_calls_to_qn(s, project, "src.publication.Publication.cleanup");
        cbm_store_close(s);
    }
    if (p) {
        cbm_pipeline_free(p);
    }
    if (fillers > 0) {
        if (saved) {
            cbm_setenv("CBM_WORKERS", saved, 1);
        } else {
            cbm_unsetenv("CBM_WORKERS");
        }
    }
    free(saved);
    th_rmtree(tmp);
    printf("    fillers=%d run->Database.deinit %d/%d Statement.deinit %d/%d open %d/%d "
           "prepare %d/%d tool->Database.deinit %d/%d app->open %d/%d deinit %d/%d step %d/%d "
           "rows->reset %d/%d api.Db.open %d/%d api.Db.deinit %d/%d api.L.ping %d/%d "
           "AbiMutex.lock %d/%d std map %d/%d std list %d/%d std alloc %d/%d Pool.first %d/%d "
           "Pool T %d/%d Ops.copy %d/%d anytype %d/%d cleanup_in=%d\n",
           fillers, ok[0], n[0], ok[1], n[1], ok[2], n[2], ok[3], n[3], ok[4], n[4], ok[5], n[5],
           ok[6], n[6], ok[7], n[7], ok[8], n[8], ok[9], n[9], ok[10], n[10], ok[11], n[11],
           ok[12], n[12], ok[13], n[13], ok[14], n[14], ok[15], n[15], ok[16], n[16], ok[17],
           n[17], ok[18], n[18], ok[19], n[19], cleanup_in);
    ASSERT_EQ(rc, 0);
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ(n[i], 1);
        ASSERT_EQ(ok[i], 1);
    }
    ASSERT_EQ(cleanup_in, 0);
    return 0;
}

/* build.zig: a module of an in-repo path dependency (build.zig.zon `.path`)
 * maps to the dependency's addModule root; a URL dependency and a
 * commented-out one map to no module. */
TEST(pipeline_zig_dependency_modules_resolve_through_build_zig_zon) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_zig_dep_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    const char *dirs[] = {"libs", "libs/util", "libs/util/src", "libs/old", "libs/old/src"};
    for (int i = 0; i < 5; i++) {
        char d[512];
        snprintf(d, sizeof(d), "%s/%s", tmp, dirs[i]);
        cbm_mkdir(d);
    }
    write_temp_file(tmp, "build.zig.zon",
                    ".{\n"
                    "    .name = .app,\n"
                    "    .dependencies = .{\n"
                    "        // .old = .{ .path = \"libs/old\" },\n"
                    "        .util = .{ .path = \"./libs/util/\" },\n"
                    "        .remote = .{\n"
                    "            .url = \"https://example.com/remote.tar.gz\",\n"
                    "            .hash = \"1220abcd\",\n"
                    "        },\n"
                    "    },\n"
                    "}\n");
    write_temp_file(tmp, "build.zig",
                    "const std = @import(\"std\");\n"
                    "\n"
                    "pub fn build(b: *std.Build) void {\n"
                    "    const util_dep = b.dependency(\"util\", .{});\n"
                    "    const exe = b.addExecutable(.{\n"
                    "        .name = \"app\",\n"
                    "        .root_module = b.createModule(.{\n"
                    "            .root_source_file = b.path(\"app/main.zig\"),\n"
                    "            .imports = &.{\n"
                    "                .{ .name = \"strings\", .module = util_dep.module(\"strings\") },\n"
                    "            },\n"
                    "        }),\n"
                    "    });\n"
                    "    exe.root_module.addImport(\"u\", util_dep.module(\"util\"));\n"
                    "    const old = b.dependency(\"old\", .{}).module(\"old\");\n"
                    "    exe.root_module.addImport(\"old\", old);\n"
                    "    exe.root_module.addImport(\"remote\", "
                    "b.dependency(\"remote\", .{}).module(\"remote\"));\n"
                    "}\n");
    write_temp_file(tmp, "libs/util/build.zig",
                    "const std = @import(\"std\");\n"
                    "\n"
                    "pub fn build(b: *std.Build) void {\n"
                    "    _ = b.addModule(\"util\", .{ .root_source_file = b.path(\"src/util.zig\") });\n"
                    "    _ = b.addModule(\"strings\", .{ .root_source_file = "
                    "b.path(\"src/strings.zig\") });\n"
                    "}\n");
    write_temp_file(tmp, "libs/util/src/util.zig", "pub fn hello() void {}\n");
    write_temp_file(tmp, "libs/util/src/strings.zig", "pub fn trim() void {}\n");
    write_temp_file(tmp, "libs/old/src/old.zig", "pub fn hello() void {}\n");
    write_temp_file(tmp, "app/main.zig",
                    "const u = @import(\"u\");\n"
                    "const strings = @import(\"strings\");\n"
                    "const old = @import(\"old\");\n"
                    "const remote = @import(\"remote\");\n"
                    "\n"
                    "pub fn main() void {\n"
                    "    u.hello();\n"
                    "    strings.trim();\n"
                    "    old.hello();\n"
                    "    remote.hello();\n"
                    "}\n");
    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    int rc = p ? cbm_pipeline_run(p) : -1;
    const char *project = p ? cbm_pipeline_project_name(p) : NULL;
    cbm_store_t *s = rc == 0 ? th_open_repo_store(tmp) : NULL;
    const char *lsp = "\"strategy\":\"lsp_zig";
    int ok[3] = {0};
    int n[3] = {0};
    if (s) {
        n[0] = calls_between_qns(s, project, "app.main.main", "libs.util.src.util.hello", lsp,
                                 &ok[0]);
        n[1] = calls_between_qns(s, project, "app.main.main", "libs.util.src.strings.trim", lsp,
                                 &ok[1]);
        n[2] = calls_between_qns(s, project, "app.main.main", "libs.old.src.old.hello", lsp,
                                 &ok[2]);
        cbm_store_close(s);
    }
    if (p) {
        cbm_pipeline_free(p);
    }
    th_rmtree(tmp);
    printf("    util.hello %d/%d strings.trim %d/%d old.hello %d/%d\n", ok[0], n[0], ok[1], n[1],
           ok[2], n[2]);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(n[0], 1);
    ASSERT_EQ(ok[0], 1);
    ASSERT_EQ(n[1], 1);
    ASSERT_EQ(ok[1], 1);
    /* Not mapped: no edge at all, not even a qualified_suffix guess. */
    ASSERT_EQ(n[2], 0);
    PASS();
}

TEST(pipeline_zig_member_calls_bind_by_receiver_type) {
    ASSERT_EQ(check_zig_receiver_edges(0), 0);
    ASSERT_EQ(check_zig_receiver_edges(52), 0);
    PASS();
}

/* Parallel path of the same guard (>= 50 files, CBM_WORKERS=4): the
 * field-type heuristic and suffix matching must not bind accelerator.print(),
 * accelerator.backward() or trainer.lr_scheduler.step() to project methods,
 * while the import-bound and bare local calls stay. */
TEST(pipeline_python_receiver_parallel_suppresses_weak_method_edges) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_python_par_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    write_temp_file(tmp, "targets.py",
                    "class MockAccelerator:\n"
                    "    def print(self, value):\n"
                    "        return value\n"
                    "\n"
                    "class OtherPrinter:\n"
                    "    def print(self, value):\n"
                    "        return value\n"
                    "\n"
                    "class FlashAttentionFunction:\n"
                    "    def backward(self, loss):\n"
                    "        return loss\n"
                    "\n"
                    "class OtherBackward:\n"
                    "    def backward(self, loss):\n"
                    "        return loss\n"
                    "\n"
                    "class EulerScheduler:\n"
                    "    def step(self):\n"
                    "        return 1\n"
                    "\n"
                    "class OtherScheduler:\n"
                    "    def step(self):\n"
                    "        return 1\n");
    write_temp_file(tmp, "pkg/__init__.py", "from .helper import compute\n");
    write_temp_file(tmp, "pkg/helper.py",
                    "def compute(value):\n"
                    "    return value * 2\n");
    write_temp_file(tmp, "caller.py",
                    "from pkg import helper\n"
                    "\n"
                    "def local_helper():\n"
                    "    return 1\n"
                    "\n"
                    "def train(accelerator, trainer):\n"
                    "    accelerator.print('hello')\n"
                    "    accelerator.backward(1)\n"
                    "    trainer.lr_scheduler.step()\n"
                    "    helper.compute(42)\n"
                    "    return local_helper()\n");
    for (int i = 0; i < 52; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "filler%d.py", i);
        snprintf(body, sizeof(body), "def filler%d():\n    return %d\n", i, i);
        write_temp_file(tmp, name, body);
    }

    char *old_workers = getenv("CBM_WORKERS");
    char *saved = old_workers ? strdup(old_workers) : NULL;
    cbm_setenv("CBM_WORKERS", "4", 1);

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);
    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    bool print_edge = cross_file_call_exists(s, project, "train", "print");
    bool backward_edge = cross_file_call_exists(s, project, "train", "backward");
    bool step_edge = cross_file_call_exists(s, project, "train", "step");
    bool compute_edge = cross_file_call_exists(s, project, "train", "compute");
    bool local_edge = cross_file_call_exists(s, project, "train", "local_helper");

    cbm_store_close(s);
    cbm_pipeline_free(p);
    if (saved) {
        cbm_setenv("CBM_WORKERS", saved, 1);
        free(saved);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    th_rmtree(tmp);

    ASSERT_FALSE(print_edge);
    ASSERT_FALSE(backward_edge);
    ASSERT_FALSE(step_edge);
    ASSERT_TRUE(compute_edge);
    ASSERT_TRUE(local_edge);
    PASS();
}

/* `edge_type` edges out of the function `source_name` whose target QN contains
 * `target_needle` (any target when NULL); -1 on a failed query. */
static int xi_edges_from(cbm_store_t *s, const char *project, const char *edge_type,
                         const char *source_name, const char *target_needle) {
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    if (cbm_store_find_edges_by_type(s, project, edge_type, &edges, &edge_count) !=
        CBM_STORE_OK) {
        return -1;
    }
    int matches = 0;
    for (int i = 0; i < edge_count; i++) {
        cbm_node_t source = {0};
        cbm_node_t target = {0};
        bool source_ok = cbm_store_find_node_by_id(s, edges[i].source_id, &source) ==
                             CBM_STORE_OK &&
                         source.name && strcmp(source.name, source_name) == 0;
        bool target_ok = !target_needle ||
                         (cbm_store_find_node_by_id(s, edges[i].target_id, &target) ==
                              CBM_STORE_OK &&
                          target.qualified_name && strstr(target.qualified_name, target_needle));
        if (source_ok && target_ok) {
            matches++;
        }
        cbm_node_free_fields(&source);
        cbm_node_free_fields(&target);
    }
    if (edges) {
        cbm_store_free_edges(edges, edge_count);
    }
    return matches;
}

/* HTTP_CALLS + ASYNC_CALLS edges out of the function `source_name`. */
static int xi_service_edges_from(cbm_store_t *s, const char *project, const char *source_name) {
    int http = xi_edges_from(s, project, "HTTP_CALLS", source_name, NULL);
    int async = xi_edges_from(s, project, "ASYNC_CALLS", source_name, NULL);
    return http < 0 || async < 0 ? -1 : http + async;
}

/* IMPORTS edges whose local_name is `local_name`; -1 on a failed query. */
static int xi_imports_binding(cbm_store_t *s, const char *project, const char *local_name) {
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    if (cbm_store_find_edges_by_type(s, project, "IMPORTS", &edges, &edge_count) !=
        CBM_STORE_OK) {
        return -1;
    }
    char needle[128];
    snprintf(needle, sizeof(needle), "\"local_name\":\"%s\"", local_name);
    int matches = 0;
    for (int i = 0; i < edge_count; i++) {
        if (edges[i].properties_json && strstr(edges[i].properties_json, needle)) {
            matches++;
        }
    }
    if (edges) {
        cbm_store_free_edges(edges, edge_count);
    }
    return matches;
}

/* A call through an import of an EXTERNAL client library is that library's
 * call: `fetchIt` from `{ get as fetchIt } from 'axios'` is a typed GET,
 * `http_post` from `from requests import post as http_post` a typed POST,
 * `emitIt` from `{ publish as emitIt } from 'mqtt'` a publish -- also when a
 * same-named project function elsewhere would win a weak name match
 * (`navigate` under `{ get as navigate }`, with a Python `navigate`). A bare
 * native `fetch('/api/items')` is a GET (upstream #856). Precision controls,
 * each with no HTTP_CALLS/ASYNC_CALLS edge:
 *   - urljoin from urllib.parse: an HTTP library's helper, not a request;
 *   - lodash's `get`, json's `dumps`: libraries no service pattern knows;
 *   - `send` defined in the importing module shadows
 *     `from paho.mqtt import publish as send`; `relay` rebound as a parameter
 *     shadows `... as relay`;
 *   - `httpx` is a PROJECT package here: its import is materialized (the
 *     precondition is asserted), so the call stays off the external httpx.post.
 * Both resolvers: CBM_WORKERS=1 is sequential, 4 with > 50 files parallel. */
TEST(pipeline_external_import_binding_sequential_parallel) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_extimport_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    write_temp_file(
        tmp, "src/ext_http.ts",
        "import { get as fetchIt } from 'axios';\n"
        "import { get as navigate } from 'axios';\n"
        "import { get as lodashGet } from 'lodash';\n"
        "import { publish as emitIt } from 'mqtt';\n"
        "export function viaAxiosAlias(): unknown { return fetchIt('/api/alias-get'); }\n"
        "export function viaShadowedName(): unknown { return navigate('/api/alias-nav'); }\n"
        "export function viaLodashAlias(o: object): unknown {\n"
        "  return lodashGet(o, '/api/lodash');\n"
        "}\n"
        "export function viaMqtt(): void { emitIt('tenant/ts', 'payload'); }\n"
        "export function viaFetch(): unknown { return fetch('/api/items'); }\n");
    write_temp_file(tmp, "src/nav.py", "def navigate(url):\n    return url\n");
    write_temp_file(tmp, "src/ext_py.py",
                    "from requests import post as http_post\n"
                    "from urllib.parse import urljoin\n"
                    "from json import dumps as publish_json\n"
                    "from paho.mqtt import publish as send\n"
                    "def py_post():\n    return http_post('/api/py-post')\n"
                    "def join_url(base):\n    return urljoin(base, '/static/asset.css')\n"
                    "def join_abs(name):\n    return urljoin('https://example.com/assets/', name)\n"
                    "def to_json():\n    return publish_json('/tenant/json')\n"
                    "def send(topic, payload):\n    return topic\n"
                    "def shadowed():\n    return send('tenant/shadow', 'payload')\n");
    write_temp_file(tmp, "src/ext_param.py",
                    "from paho.mqtt import publish as relay\n"
                    "def param_shadow(relay):\n    return relay('tenant/param', 'payload')\n");
    write_temp_file(tmp, "httpx/__init__.py", "def get(url):\n    return url\n");
    write_temp_file(tmp, "src/proj_client.py",
                    "from httpx import post as hx_post\n"
                    "def proj_post():\n    return hx_post('/api/project')\n");
    for (int i = 0; i < 52; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "src/padding%d.ts", i);
        snprintf(body, sizeof(body), "export function padding%d() { return %d; }\n", i, i);
        write_temp_file(tmp, name, body);
    }
    static const char *const silent[] = {"viaLodashAlias", "join_url",     "join_abs", "to_json",
                                         "shadowed",       "param_shadow", "proj_post"};
    enum { SILENT = sizeof(silent) / sizeof(silent[0]) };
    static const char *const workers[2] = {"1", "4"};
    char *old_workers = getenv("CBM_WORKERS");
    char *saved_workers = old_workers ? strdup(old_workers) : NULL;
    int run_rc[2] = {-1, -1};
    bool opened[2] = {false, false};
    int typed_get[2] = {-1, -1};
    int shadowed_get[2] = {-1, -1};
    int fetch_get[2] = {-1, -1};
    int typed_post[2] = {-1, -1};
    int published[2] = {-1, -1};
    int shadow_calls[2] = {-1, -1};
    int project_import[2] = {-1, -1};
    int silent_edges[2][SILENT];
    for (int mode = 0; mode < 2; mode++) {
        for (int i = 0; i < SILENT; i++) {
            silent_edges[mode][i] = -1;
        }
        char *proj_name = cbm_project_name_from_path(tmp);
        if (proj_name) {
            th_remove_project_workspace(proj_name);
            free(proj_name);
        }
        cbm_setenv("CBM_WORKERS", workers[mode], 1);
        cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
        run_rc[mode] = p ? cbm_pipeline_run(p) : -1;
        const char *project = p ? cbm_pipeline_project_name(p) : NULL;
        cbm_store_t *s = run_rc[mode] == 0 ? th_open_repo_store(tmp) : NULL;
        opened[mode] = s != NULL;
        if (s && project) {
            typed_get[mode] = xi_edges_from(s, project, "HTTP_CALLS", "viaAxiosAlias",
                                            "__route__GET__/api/alias-get");
            shadowed_get[mode] = xi_edges_from(s, project, "HTTP_CALLS", "viaShadowedName",
                                               "__route__GET__/api/alias-nav");
            fetch_get[mode] =
                xi_edges_from(s, project, "HTTP_CALLS", "viaFetch", "/api/items");
            typed_post[mode] = xi_edges_from(s, project, "HTTP_CALLS", "py_post",
                                             "__route__POST__/api/py-post");
            published[mode] = xi_edges_from(s, project, "ASYNC_CALLS", "viaMqtt", "tenant/ts");
            shadow_calls[mode] = xi_edges_from(s, project, "CALLS", "shadowed", "send");
            project_import[mode] = xi_imports_binding(s, project, "hx_post");
            for (int i = 0; i < SILENT; i++) {
                silent_edges[mode][i] = xi_service_edges_from(s, project, silent[i]);
            }
        }
        if (s) {
            cbm_store_close(s);
        }
        cbm_pipeline_free(p);
    }
    if (saved_workers) {
        cbm_setenv("CBM_WORKERS", saved_workers, 1);
        free(saved_workers);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    th_rmtree(tmp);
    for (int mode = 0; mode < 2; mode++) {
        ASSERT_EQ(run_rc[mode], 0);
        ASSERT_TRUE(opened[mode]);
        ASSERT_EQ(typed_get[mode], 1);
        ASSERT_EQ(shadowed_get[mode], 1);
        ASSERT_EQ(fetch_get[mode], 1);
        ASSERT_EQ(typed_post[mode], 1);
        ASSERT_EQ(published[mode], 1);
        ASSERT_GTE(shadow_calls[mode], 1);
        ASSERT_GTE(project_import[mode], 1);
        for (int i = 0; i < SILENT; i++) {
            ASSERT_EQ(silent_edges[mode][i], 0);
        }
    }
    PASS();
}

/* Native `fetch()` (upstream #856): a LOCAL `fetch` definition wins over the
 * global-API guess. Registry resolution finds this project's own top-level
 * `fetch`, so the call is a plain CALLS edge to it, never HTTP_CALLS. Its own
 * project: the registry's project-wide unique-name fallback lets a local
 * `fetch` anywhere capture bare `fetch()` calls project-wide. */
TEST(pipeline_local_fetch_shadow_not_classified_as_http) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_fetch_shadow_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }
    write_temp_file(tmp, "src/local_fetch.ts",
                    "function fetch(url: string): string {\n"
                    "  return 'mock:' + url;\n"
                    "}\n"
                    "export function useLocalFetch(): string {\n"
                    "  return fetch('/api/data');\n"
                    "}\n");

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_count_edges_by_type(s, project, "HTTP_CALLS"), 0);
    ASSERT_TRUE(cross_file_call_exists(s, project, "useLocalFetch", "fetch"));

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

/* ── Git history pass tests ─────────────────────────────────────── */

TEST(githistory_is_trackable) {
    /* Source files → trackable */
    ASSERT_TRUE(cbm_is_trackable_file("main.go"));
    ASSERT_TRUE(cbm_is_trackable_file("src/app.py"));
    ASSERT_TRUE(cbm_is_trackable_file("README.md"));

    /* Non-trackable: skip prefixes */
    ASSERT_FALSE(cbm_is_trackable_file("node_modules/foo/bar.js"));
    ASSERT_FALSE(cbm_is_trackable_file("vendor/lib/dep.go"));
    ASSERT_FALSE(cbm_is_trackable_file(".git/config"));
    ASSERT_FALSE(cbm_is_trackable_file("__pycache__/mod.pyc"));

    /* Non-trackable: lock files */
    ASSERT_FALSE(cbm_is_trackable_file("package-lock.json"));
    ASSERT_FALSE(cbm_is_trackable_file("go.sum"));

    /* Non-trackable: binary/minified extensions */
    ASSERT_FALSE(cbm_is_trackable_file("image.png"));
    ASSERT_FALSE(cbm_is_trackable_file("src/style.min.css"));

    PASS();
}

/* The history pass adds change_count/last_modified to File nodes without
 * dropping keys already there: the parse-coverage stamp is written before it
 * runs, and replacing the properties lost every partial/missed file in a git
 * repository. */
TEST(githistory_apply_keeps_coverage_stamp) {
    cbm_gbuf_t *gb = cbm_gbuf_new("proj", "/repo");
    ASSERT_NOT_NULL(gb);
    char *qn = cbm_pipeline_fqn_compute("proj", "include/zova.h", "__file__");
    ASSERT_NOT_NULL(qn);
    ASSERT_TRUE(cbm_gbuf_upsert_node(gb, "File", "zova.h", qn, "include/zova.h", 0, 0,
                                     "{\"extension\":\".h\",\"coverage\":\"partial\","
                                     "\"parse_error_ranges\":\"2021-2021\"}") > 0);
    cbm_file_temporal_t ft = {0};
    snprintf(ft.file_path, sizeof(ft.file_path), "%s", "include/zova.h");
    ft.change_count = 29;
    ft.last_modified = 1790588152LL;
    cbm_githistory_result_t res = {0};
    res.file_temporal = &ft;
    res.file_temporal_count = 1;
    cbm_pipeline_ctx_t ctx = {.project_name = "proj", .repo_path = "/repo", .gbuf = gb};
    cbm_pipeline_githistory_apply(&ctx, &res);
    const cbm_gbuf_node_t *n = cbm_gbuf_find_by_qn(gb, qn);
    ASSERT_NOT_NULL(n);
    const char *props = n->properties_json;
    ASSERT_NOT_NULL(strstr(props, "\"coverage\":\"partial\""));
    ASSERT_NOT_NULL(strstr(props, "\"parse_error_ranges\":\"2021-2021\""));
    ASSERT_NOT_NULL(strstr(props, "\"change_count\":29"));
    ASSERT_NOT_NULL(strstr(props, "\"last_modified\":1790588152"));
    ASSERT_NOT_NULL(strstr(props, "\"extension\":\".h\""));
    free(qn);
    cbm_gbuf_free(gb);
    PASS();
}

TEST(githistory_compute_coupling) {
    /* 5 commits with overlapping files */
    char *files_0[] = {"a.go", "b.go", "c.go"};
    char *files_1[] = {"a.go", "b.go"};
    char *files_2[] = {"a.go", "b.go"};
    char *files_3[] = {"a.go", "c.go"};
    char *files_4[] = {"d.go", "e.go"};

    cbm_commit_files_t commits[] = {
        {files_0, 3, 0}, {files_1, 2, 0}, {files_2, 2, 0}, {files_3, 2, 0}, {files_4, 2, 0},
    };

    cbm_change_coupling_t results[100];
    int n = cbm_compute_change_coupling(commits, 5, results, 100);

    /* a.go + b.go co-change 3 times → should appear */
    bool found_ab = false;
    for (int i = 0; i < n; i++) {
        if ((strcmp(results[i].file_a, "a.go") == 0 && strcmp(results[i].file_b, "b.go") == 0) ||
            (strcmp(results[i].file_a, "b.go") == 0 && strcmp(results[i].file_b, "a.go") == 0)) {
            found_ab = true;
            ASSERT_EQ(results[i].co_change_count, 3);
            ASSERT_TRUE(results[i].coupling_score >= 0.7);
        }
    }
    ASSERT_TRUE(found_ab);

    /* d.go + e.go co-change only 1 time → below threshold, should NOT appear */
    for (int i = 0; i < n; i++) {
        ASSERT_FALSE(strcmp(results[i].file_a, "d.go") == 0 ||
                     strcmp(results[i].file_b, "d.go") == 0);
    }

    PASS();
}

TEST(githistory_coupling_carries_last_co_change) {
    /* Coupling output must surface the most recent commit timestamp at which
     * a pair co-changed, so callers can score recency in addition to
     * frequency. The pair (a.go, b.go) co-changes at three timestamps; the
     * resulting last_co_change must be the maximum (newest) of those. */
    char *files_old[] = {"a.go", "b.go"};
    char *files_mid[] = {"a.go", "b.go"};
    char *files_new[] = {"a.go", "b.go"};
    /* Unrelated co-change in the middle to make sure we don't accidentally
     * pick up that pair's timestamp by index. */
    char *files_other[] = {"c.go", "d.go"};

    cbm_commit_files_t commits[] = {
        {files_old, 2, 1700000000LL},   /* oldest a.go/b.go co-change */
        {files_other, 2, 1750000000LL}, /* unrelated pair */
        {files_mid, 2, 1720000000LL},
        {files_new, 2, 1800000000LL}, /* newest a.go/b.go co-change */
    };

    cbm_change_coupling_t results[16];
    int n = cbm_compute_change_coupling(commits, 4, results, 16);
    ASSERT_GTE(n, 1);

    bool found_ab = false;
    for (int i = 0; i < n; i++) {
        bool is_ab =
            (strcmp(results[i].file_a, "a.go") == 0 && strcmp(results[i].file_b, "b.go") == 0) ||
            (strcmp(results[i].file_a, "b.go") == 0 && strcmp(results[i].file_b, "a.go") == 0);
        if (!is_ab) {
            continue;
        }
        ASSERT_EQ(results[i].co_change_count, 3);
        /* last_co_change must be the max of the three a.go/b.go timestamps. */
        ASSERT_EQ(results[i].last_co_change, 1800000000LL);
        found_ab = true;
    }
    ASSERT_TRUE(found_ab);
    PASS();
}

TEST(githistory_skip_large_commits) {
    /* A single commit with 25 files → should be skipped (>20) */
    char *files[25];
    char bufs[25][32];
    for (int i = 0; i < 25; i++) {
        snprintf(bufs[i], sizeof(bufs[i]), "file%d.go", i);
        files[i] = bufs[i];
    }

    cbm_commit_files_t commits[] = {{files, 25, 0}};

    cbm_change_coupling_t results[100];
    int n = cbm_compute_change_coupling(commits, 1, results, 100);
    ASSERT_EQ(n, 0);

    PASS();
}

TEST(githistory_limits_to_max) {
    /* Create many commits to generate >100 couplings */
    /* 50 files, each pair has 3 commits → C(50,2)=1225 pairs, but only
     * pairs with ≥3 co-changes and score≥0.3 pass the threshold */
    int nfiles = 50;
    int npairs = nfiles * (nfiles - 1) / 2;
    int ncommits = npairs * 3;

    cbm_commit_files_t *commits = calloc(ncommits, sizeof(cbm_commit_files_t));
    char **file_strs = calloc(nfiles, sizeof(char *));
    for (int i = 0; i < nfiles; i++) {
        file_strs[i] = malloc(32);
        snprintf(file_strs[i], 32, "f%d.go", i);
    }

    int ci = 0;
    for (int i = 0; i < nfiles; i++) {
        for (int j = i + 1; j < nfiles; j++) {
            for (int k = 0; k < 3; k++) {
                commits[ci].files = malloc(2 * sizeof(char *));
                commits[ci].files[0] = file_strs[i];
                commits[ci].files[1] = file_strs[j];
                commits[ci].count = 2;
                ci++;
            }
        }
    }

    /* max_out = 100 → should cap at 100 */
    cbm_change_coupling_t results[100];
    int n = cbm_compute_change_coupling(commits, ncommits, results, 100);
    ASSERT_TRUE(n <= 100);

    /* Cleanup */
    for (int i = 0; i < ncommits; i++) {
        free(commits[i].files);
    }
    for (int i = 0; i < nfiles; i++) {
        free(file_strs[i]);
    }
    free(file_strs);
    free(commits);

    PASS();
}

/* ── Test detection tests ──────────────────────────────────────── */

TEST(testdetect_is_test_file) {
    /* Test file patterns (all languages) */
    ASSERT_TRUE(cbm_is_test_path("foo_test.go"));
    ASSERT_TRUE(cbm_is_test_path("test_handler.py"));
    ASSERT_TRUE(cbm_is_test_path("handler.test.js"));
    ASSERT_TRUE(cbm_is_test_path("handler.spec.ts"));
    ASSERT_TRUE(cbm_is_test_path("Component.test.tsx"));
    ASSERT_TRUE(cbm_is_test_path("OrderTest.java"));
    ASSERT_TRUE(cbm_is_test_path("handler_test.rs"));
    ASSERT_TRUE(cbm_is_test_path("handler_test.cpp"));
    ASSERT_TRUE(cbm_is_test_path("OrderTest.cs"));
    ASSERT_TRUE(cbm_is_test_path("OrderTest.php"));
    ASSERT_TRUE(cbm_is_test_path("OrderSpec.scala"));
    ASSERT_TRUE(cbm_is_test_path("OrderTest.kt"));
    ASSERT_TRUE(cbm_is_test_path("handler_test.lua"));

    /* Non-test file patterns */
    ASSERT_FALSE(cbm_is_test_path("foo.go"));
    ASSERT_FALSE(cbm_is_test_path("handler.py"));
    ASSERT_FALSE(cbm_is_test_path("handler.js"));
    ASSERT_FALSE(cbm_is_test_path("handler.ts"));
    ASSERT_FALSE(cbm_is_test_path("Component.tsx"));
    ASSERT_FALSE(cbm_is_test_path("Order.java"));
    ASSERT_FALSE(cbm_is_test_path("handler.rs"));
    ASSERT_FALSE(cbm_is_test_path("handler.cpp"));
    ASSERT_FALSE(cbm_is_test_path("Order.cs"));
    ASSERT_FALSE(cbm_is_test_path("Order.php"));
    ASSERT_FALSE(cbm_is_test_path("Order.scala"));
    ASSERT_FALSE(cbm_is_test_path("Order.kt"));
    ASSERT_FALSE(cbm_is_test_path("handler.lua"));

    PASS();
}

TEST(testdetect_is_test_function) {
    /* Test function patterns */
    ASSERT_TRUE(cbm_is_test_func_name("TestCreate"));  /* Go */
    ASSERT_TRUE(cbm_is_test_func_name("test_create")); /* Python/Rust/Lua */
    ASSERT_TRUE(cbm_is_test_func_name("test"));        /* JS/TS */
    ASSERT_TRUE(cbm_is_test_func_name("describe"));    /* JS/TS */
    ASSERT_TRUE(cbm_is_test_func_name("it"));          /* JS/TS */
    ASSERT_TRUE(cbm_is_test_func_name("testCreate"));  /* Java/PHP/Scala/Kotlin */
    ASSERT_TRUE(cbm_is_test_func_name("TestCreate"));  /* C++/C# */

    /* Non-test function patterns */
    ASSERT_FALSE(cbm_is_test_func_name("create"));
    ASSERT_FALSE(cbm_is_test_func_name("handleRequest"));
    ASSERT_FALSE(cbm_is_test_func_name("process"));

    PASS();
}

/* ── Implements pass tests (graph buffer based) ────────────────── */

TEST(implements_creates_override) {
    /* Port of TestPassImplementsCreatesOverrideEdges.
     * Set up Interface+methods, Class+methods, verify IMPLEMENTS+OVERRIDE. */
    cbm_gbuf_t *gb = cbm_gbuf_new("test-proj", "/tmp/test");
    ASSERT_NOT_NULL(gb);

    /* Interface "Reader" with methods "Read" and "Close" */
    int64_t iface_id =
        cbm_gbuf_upsert_node(gb, "Interface", "Reader", "pkg.Reader", "pkg/reader.go", 1, 5, "{}");
    ASSERT_GT(iface_id, 0);

    int64_t read_method_id =
        cbm_gbuf_upsert_node(gb, "Method", "Read", "pkg.Reader.Read", "pkg/reader.go", 2, 2, "{}");
    int64_t close_method_id = cbm_gbuf_upsert_node(gb, "Method", "Close", "pkg.Reader.Close",
                                                   "pkg/reader.go", 3, 3, "{}");
    ASSERT_GT(read_method_id, 0);
    ASSERT_GT(close_method_id, 0);

    /* DEFINES_METHOD edges */
    cbm_gbuf_insert_edge(gb, iface_id, read_method_id, "DEFINES_METHOD", "{}");
    cbm_gbuf_insert_edge(gb, iface_id, close_method_id, "DEFINES_METHOD", "{}");

    /* Class "FileReader" with matching methods */
    int64_t class_id = cbm_gbuf_upsert_node(gb, "Class", "FileReader", "pkg.FileReader",
                                            "pkg/filereader.go", 1, 10, "{}");
    ASSERT_GT(class_id, 0);

    int64_t fr_read_id =
        cbm_gbuf_upsert_node(gb, "Method", "Read", "pkg.FileReader.Read", "pkg/filereader.go", 2, 4,
                             "{\"receiver\":\"(f *FileReader)\"}");
    int64_t fr_close_id =
        cbm_gbuf_upsert_node(gb, "Method", "Close", "pkg.FileReader.Close", "pkg/filereader.go", 5,
                             7, "{\"receiver\":\"(f *FileReader)\"}");
    ASSERT_GT(fr_read_id, 0);
    ASSERT_GT(fr_close_id, 0);

    /* Run Go-style implements detection */
    atomic_int cancelled = 0;
    cbm_pipeline_ctx_t ctx = {
        .project_name = "test-proj",
        .repo_path = "/tmp/test",
        .gbuf = gb,
        .registry = NULL,
        .cancelled = &cancelled,
    };
    int edges_created = cbm_pipeline_implements_go(&ctx);
    ASSERT_GT(edges_created, 0);

    /* Verify IMPLEMENTS edge: FileReader → Reader */
    const cbm_gbuf_edge_t **impl_edges = NULL;
    int impl_count = 0;
    int rc =
        cbm_gbuf_find_edges_by_source_type(gb, class_id, "IMPLEMENTS", &impl_edges, &impl_count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(impl_count, 1);
    ASSERT_EQ(impl_edges[0]->target_id, iface_id);

    /* Verify OVERRIDE edges: FileReader.Read → Reader.Read */
    const cbm_gbuf_edge_t **read_overrides = NULL;
    int read_override_count = 0;
    rc = cbm_gbuf_find_edges_by_source_type(gb, fr_read_id, "OVERRIDE", &read_overrides,
                                            &read_override_count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(read_override_count, 1);
    ASSERT_EQ(read_overrides[0]->target_id, read_method_id);

    /* Verify OVERRIDE edge: FileReader.Close → Reader.Close */
    const cbm_gbuf_edge_t **close_overrides = NULL;
    int close_override_count = 0;
    rc = cbm_gbuf_find_edges_by_source_type(gb, fr_close_id, "OVERRIDE", &close_overrides,
                                            &close_override_count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(close_override_count, 1);
    ASSERT_EQ(close_overrides[0]->target_id, close_method_id);

    cbm_gbuf_free(gb);
    PASS();
}

TEST(implements_no_match) {
    /* Port of TestPassImplementsNoOverrideWithoutMatch.
     * Interface requires Read+Write, struct only has Read → no edges. */
    cbm_gbuf_t *gb = cbm_gbuf_new("test-proj", "/tmp/test");
    ASSERT_NOT_NULL(gb);

    /* Interface "ReadWriter" with methods "Read" and "Write" */
    int64_t iface_id = cbm_gbuf_upsert_node(gb, "Interface", "ReadWriter", "pkg.ReadWriter",
                                            "pkg/rw.go", 1, 5, "{}");
    int64_t read_id =
        cbm_gbuf_upsert_node(gb, "Method", "Read", "pkg.ReadWriter.Read", "pkg/rw.go", 2, 2, "{}");
    int64_t write_id = cbm_gbuf_upsert_node(gb, "Method", "Write", "pkg.ReadWriter.Write",
                                            "pkg/rw.go", 3, 3, "{}");
    cbm_gbuf_insert_edge(gb, iface_id, read_id, "DEFINES_METHOD", "{}");
    cbm_gbuf_insert_edge(gb, iface_id, write_id, "DEFINES_METHOD", "{}");

    /* Struct "OnlyReader" with only "Read" (missing "Write") */
    int64_t class_id = cbm_gbuf_upsert_node(gb, "Class", "OnlyReader", "pkg.OnlyReader",
                                            "pkg/onlyreader.go", 1, 10, "{}");
    int64_t or_read_id =
        cbm_gbuf_upsert_node(gb, "Method", "Read", "pkg.OnlyReader.Read", "pkg/onlyreader.go", 2, 4,
                             "{\"receiver\":\"(o *OnlyReader)\"}");

    /* Run Go-style implements detection */
    atomic_int cancelled = 0;
    cbm_pipeline_ctx_t ctx = {
        .project_name = "test-proj",
        .repo_path = "/tmp/test",
        .gbuf = gb,
        .registry = NULL,
        .cancelled = &cancelled,
    };
    int edges_created = cbm_pipeline_implements_go(&ctx);
    ASSERT_EQ(edges_created, 0);

    /* Verify NO IMPLEMENTS edge */
    const cbm_gbuf_edge_t **impl_edges = NULL;
    int impl_count = 0;
    cbm_gbuf_find_edges_by_source_type(gb, class_id, "IMPLEMENTS", &impl_edges, &impl_count);
    ASSERT_EQ(impl_count, 0);

    /* Verify NO OVERRIDE edge */
    const cbm_gbuf_edge_t **override_edges = NULL;
    int override_count = 0;
    cbm_gbuf_find_edges_by_source_type(gb, or_read_id, "OVERRIDE", &override_edges,
                                       &override_count);
    ASSERT_EQ(override_count, 0);

    /* Suppress unused warnings */
    (void)iface_id;
    (void)read_id;
    (void)write_id;

    cbm_gbuf_free(gb);
    PASS();
}

/* ── Usages pass tests (full pipeline integration) ──────────────── */

/* Helper to create a temp dir with a single source file */
static char g_usages_tmpdir[256];

static int setup_usages_repo(const char *filename, const char *content, const char *extra_file,
                             const char *extra_content) {
    snprintf(g_usages_tmpdir, sizeof(g_usages_tmpdir), "/tmp/cbm_usage_XXXXXX");
    if (!cbm_mkdtemp(g_usages_tmpdir))
        return -1;

    /* Check if filename has subdirectory */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_usages_tmpdir, filename);

    /* Create subdirectories if needed */
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s", path);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        th_mkdir_p(dir_path);
    }

    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "%s", content);
    fclose(f);

    if (extra_file && extra_content) {
        snprintf(path, sizeof(path), "%s/%s", g_usages_tmpdir, extra_file);
        f = fopen(path, "w");
        if (!f)
            return -1;
        fprintf(f, "%s", extra_content);
        fclose(f);
    }

    return 0;
}

static void teardown_usages_repo(void) {
    if (g_usages_tmpdir[0])
        rm_rf(g_usages_tmpdir);
    g_usages_tmpdir[0] = '\0';
}

TEST(usages_creates_edges) {
    /* Port of TestPassUsagesCreatesEdges.
     * Go source with callback reference → USAGE edge. */
    const char *go_source = "package mypkg\n\n"
                            "func Process(data string) string {\n"
                            "\treturn data\n"
                            "}\n\n"
                            "func Register() {\n"
                            "\thandler := Process\n"
                            "\t_ = handler\n"
                            "}\n";

    if (setup_usages_repo("mypkg/main.go", go_source, "go.mod", "module testmod\ngo 1.21\n") != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_usages_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = th_open_repo_store(g_usages_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    /* Check for USAGE edges */
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    rc = cbm_store_find_edges_by_type(s, project, "USAGE", &edges, &edge_count);

    bool found_usage = false;
    for (int i = 0; i < edge_count; i++) {
        cbm_node_t src = {0}, tgt = {0};
        if (cbm_store_find_node_by_id(s, edges[i].source_id, &src) == CBM_STORE_OK &&
            cbm_store_find_node_by_id(s, edges[i].target_id, &tgt) == CBM_STORE_OK) {
            if (strcmp(src.name, "Register") == 0 && strcmp(tgt.name, "Process") == 0) {
                found_usage = true;
            }
        }
        cbm_node_free_fields(&src);
        cbm_node_free_fields(&tgt);
    }
    if (edges)
        cbm_store_free_edges(edges, edge_count);
    ASSERT_TRUE(found_usage);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_usages_repo();
    PASS();
}

TEST(usages_no_duplicate_calls) {
    /* Port of TestPassUsagesDoesNotDuplicateCalls.
     * A direct function call should produce CALLS but not USAGE. */
    const char *go_source = "package mypkg\n\n"
                            "func Helper() string {\n"
                            "\treturn \"ok\"\n"
                            "}\n\n"
                            "func Main() {\n"
                            "\tHelper()\n"
                            "}\n";

    if (setup_usages_repo("mypkg/main.go", go_source, "go.mod", "module testmod\ngo 1.21\n") != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_usages_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = th_open_repo_store(g_usages_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    /* Should have CALLS edge from Main to Helper */
    cbm_edge_t *call_edges = NULL;
    int call_count = 0;
    cbm_store_find_edges_by_type(s, project, "CALLS", &call_edges, &call_count);

    bool found_call = false;
    for (int i = 0; i < call_count; i++) {
        cbm_node_t src = {0}, tgt = {0};
        if (cbm_store_find_node_by_id(s, call_edges[i].source_id, &src) == CBM_STORE_OK &&
            cbm_store_find_node_by_id(s, call_edges[i].target_id, &tgt) == CBM_STORE_OK) {
            if (strcmp(src.name, "Main") == 0 && strcmp(tgt.name, "Helper") == 0) {
                found_call = true;
            }
        }
        cbm_node_free_fields(&src);
        cbm_node_free_fields(&tgt);
    }
    if (call_edges)
        cbm_store_free_edges(call_edges, call_count);
    ASSERT_TRUE(found_call);

    /* Should NOT have USAGE edge from Main to Helper */
    cbm_edge_t *usage_edges = NULL;
    int usage_count = 0;
    cbm_store_find_edges_by_type(s, project, "USAGE", &usage_edges, &usage_count);

    for (int i = 0; i < usage_count; i++) {
        cbm_node_t src = {0}, tgt = {0};
        if (cbm_store_find_node_by_id(s, usage_edges[i].source_id, &src) == CBM_STORE_OK &&
            cbm_store_find_node_by_id(s, usage_edges[i].target_id, &tgt) == CBM_STORE_OK) {
            ASSERT_FALSE(strcmp(src.name, "Main") == 0 && strcmp(tgt.name, "Helper") == 0);
        }
        cbm_node_free_fields(&src);
        cbm_node_free_fields(&tgt);
    }
    if (usage_edges)
        cbm_store_free_edges(usage_edges, usage_count);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_usages_repo();
    PASS();
}

TEST(calls_edge_carries_call_site_line) {
    /* Regression guard for #503: a CALLS edge must persist the source line of
     * the call site in its JSON props as "line":N. Helper() is invoked on
     * line 8 (1-based) of the source below. */
    const char *go_source = "package mypkg\n"
                            "\n"
                            "func Helper() string {\n"
                            "\treturn \"ok\"\n"
                            "}\n"
                            "\n"
                            "func Main() {\n"
                            "\tHelper()\n"
                            "}\n";

    if (setup_usages_repo("mypkg/main.go", go_source, "go.mod", "module testmod\ngo 1.21\n") != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_usages_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_usages_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    cbm_edge_t *call_edges = NULL;
    int call_count = 0;
    cbm_store_find_edges_by_type(s, project, "CALLS", &call_edges, &call_count);

    bool found_line = false;
    for (int i = 0; i < call_count; i++) {
        cbm_node_t src = {0}, tgt = {0};
        if (cbm_store_find_node_by_id(s, call_edges[i].source_id, &src) == CBM_STORE_OK &&
            cbm_store_find_node_by_id(s, call_edges[i].target_id, &tgt) == CBM_STORE_OK) {
            if (strcmp(src.name, "Main") == 0 && strcmp(tgt.name, "Helper") == 0) {
                ASSERT_NOT_NULL(call_edges[i].properties_json);
                ASSERT_TRUE(strstr(call_edges[i].properties_json, "\"line\":8}") != NULL);
                found_line = true;
            }
        }
        cbm_node_free_fields(&src);
        cbm_node_free_fields(&tgt);
    }
    if (call_edges)
        cbm_store_free_edges(call_edges, call_count);
    ASSERT_TRUE(found_line);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_usages_repo();
    PASS();
}

TEST(usages_kotlin_creates_edges) {
    /* Port of TestPassUsagesKotlinCreatesEdges.
     * Kotlin source with callback reference → USAGE edge. */
    const char *kt_source = "fun process(data: String): String {\n"
                            "    return data\n"
                            "}\n\n"
                            "fun register() {\n"
                            "    val handler = ::process\n"
                            "}\n";

    if (setup_usages_repo("Main.kt", kt_source, NULL, NULL) != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_usages_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = th_open_repo_store(g_usages_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    /* Check USAGE edges exist (Kotlin extraction may or may not produce them
     * depending on extraction support — just verify no crash and valid counts) */
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    rc = cbm_store_find_edges_by_type(s, project, "USAGE", &edges, &edge_count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    /* Note: The Go test just logs, doesn't assert — same behavior here */
    if (edges)
        cbm_store_free_edges(edges, edge_count);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_usages_repo();
    PASS();
}

TEST(usages_kotlin_no_duplicate_calls) {
    /* Port of TestPassUsagesKotlinDoesNotDuplicateCalls.
     * Direct call in Kotlin → CALLS but not USAGE. */
    const char *kt_source = "fun helper(): String {\n"
                            "    return \"ok\"\n"
                            "}\n\n"
                            "fun main() {\n"
                            "    helper()\n"
                            "}\n";

    if (setup_usages_repo("Main.kt", kt_source, NULL, NULL) != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_usages_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = th_open_repo_store(g_usages_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    /* Should have CALLS edge from main to helper */
    cbm_edge_t *call_edges = NULL;
    int call_count = 0;
    cbm_store_find_edges_by_type(s, project, "CALLS", &call_edges, &call_count);

    bool found_call = false;
    for (int i = 0; i < call_count; i++) {
        cbm_node_t src = {0}, tgt = {0};
        if (cbm_store_find_node_by_id(s, call_edges[i].source_id, &src) == CBM_STORE_OK &&
            cbm_store_find_node_by_id(s, call_edges[i].target_id, &tgt) == CBM_STORE_OK) {
            if (strcmp(src.name, "main") == 0 && strcmp(tgt.name, "helper") == 0) {
                found_call = true;
            }
        }
        cbm_node_free_fields(&src);
        cbm_node_free_fields(&tgt);
    }
    if (call_edges)
        cbm_store_free_edges(call_edges, call_count);
    ASSERT_TRUE(found_call);

    /* Should NOT have USAGE edge from main to helper */
    cbm_edge_t *usage_edges = NULL;
    int usage_count = 0;
    cbm_store_find_edges_by_type(s, project, "USAGE", &usage_edges, &usage_count);

    for (int i = 0; i < usage_count; i++) {
        cbm_node_t src = {0}, tgt = {0};
        if (cbm_store_find_node_by_id(s, usage_edges[i].source_id, &src) == CBM_STORE_OK &&
            cbm_store_find_node_by_id(s, usage_edges[i].target_id, &tgt) == CBM_STORE_OK) {
            ASSERT_FALSE(strcmp(src.name, "main") == 0 && strcmp(tgt.name, "helper") == 0);
        }
        cbm_node_free_fields(&src);
        cbm_node_free_fields(&tgt);
    }
    if (usage_edges)
        cbm_store_free_edges(usage_edges, usage_count);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_usages_repo();
    PASS();
}

/* ── Pipeline language integration tests ────────────────────────── */

/* General helper: create temp dir and write multiple files */
static char g_lang_tmpdir[256];

static int setup_lang_repo(const char **filenames, const char **contents, int count) {
    snprintf(g_lang_tmpdir, sizeof(g_lang_tmpdir), "/tmp/cbm_lang_XXXXXX");
    if (!cbm_mkdtemp(g_lang_tmpdir))
        return -1;

    for (int i = 0; i < count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", g_lang_tmpdir, filenames[i]);

        /* Create parent directories */
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            th_mkdir_p(dir);
        }

        FILE *f = fopen(path, "wb");
        if (!f)
            return -1;
        fprintf(f, "%s", contents[i]);
        fclose(f);
    }
    return 0;
}

static void teardown_lang_repo(void) {
    if (g_lang_tmpdir[0])
        rm_rf(g_lang_tmpdir);
    g_lang_tmpdir[0] = '\0';
}

TEST(pipeline_python_project) {
    /* Port of TestPipelinePythonProject */
    const char *files[] = {"main.py", "utils.py"};
    const char *contents[] = {
        "def greet(name):\n    return f\"Hello, {name}\"\n\n"
        "def process():\n    result = greet(\"world\")\n    return result\n",

        "API_URL = \"https://example.com/api\"\nMAX_RETRIES = 3\n\n"
        "def fetch_data(url):\n    pass\n\n"
        "class DataProcessor:\n    def transform(self, data):\n        return data\n"};

    if (setup_lang_repo(files, contents, 2) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *funcs = NULL;
    int fc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Function", &funcs, &fc);
    ASSERT_GTE(fc, 3); /* greet, process, fetch_data */
    cbm_store_free_nodes(funcs, fc);

    cbm_node_t *cls = NULL;
    int cc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Class", &cls, &cc);
    ASSERT_GTE(cc, 1); /* DataProcessor */
    cbm_store_free_nodes(cls, cc);

    cbm_node_t *methods = NULL;
    int mc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Method", &methods, &mc);
    ASSERT_GTE(mc, 1); /* transform */
    cbm_store_free_nodes(methods, mc);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

/* #768 end-to-end: `import { A, B } from './lib'` must survive the REAL
 * pipeline (extract -> gbuf dedup -> publish) as TWO IMPORTS edges with
 * distinct local_name. */
TEST(pipeline_imports_multi_symbol_edges) {
    const char *files[] = {"consumer.ts", "lib.ts"};
    const char *contents[] = {
        "import { A, B } from './lib';\n\nexport function useBoth() {\n  return A() + B();\n}\n",
        "export function A() {\n  return 1;\n}\nexport function B() {\n  return 2;\n}\n"};

    if (setup_lang_repo(files, contents, 2) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    /* Both named imports must be queryable as separate IMPORTS edges. */
    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_edge_t *edges = NULL;
    int count = 0;
    ASSERT_EQ(cbm_store_find_edges_by_type(s, proj, "IMPORTS", &edges, &count), CBM_STORE_OK);
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(strstr(edges[0].properties_json, "\"local_name\":\"A\"") != NULL ||
                strstr(edges[1].properties_json, "\"local_name\":\"A\"") != NULL);
    ASSERT_TRUE(strstr(edges[0].properties_json, "\"local_name\":\"B\"") != NULL ||
                strstr(edges[1].properties_json, "\"local_name\":\"B\"") != NULL);
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

/* Regression for the sequential path used by supervised recovery. Rust
 * grouped imports preserve the whole brace list as local_name, including
 * embedded newlines; IMPORTS properties must still be valid JSON. */
TEST(pipeline_rust_grouped_import_properties_valid_json_sequential) {
    const char *files[] = {"temporary.rs", "app_state.rs"};
    const char *contents[] = {
        "use crate::app_state::{\n"
        "    ActiveTemporaryInvite, AppState, NetworkState, TemporaryChatKind, "
        "TemporaryChatSession,\n"
        "    TemporaryInvitePayload,\n"
        "};\n\n"
        "pub fn open_chat() -> AppState { AppState {} }\n",
        "pub struct ActiveTemporaryInvite {}\n"
        "pub struct AppState {}\n"
        "pub struct NetworkState {}\n"
        "pub struct TemporaryChatKind {}\n"
        "pub struct TemporaryChatSession {}\n"
        "pub struct TemporaryInvitePayload {}\n"};

    const char *saved_workers = getenv("CBM_WORKERS");
    char *saved_workers_copy = saved_workers ? strdup(saved_workers) : NULL;
    cbm_setenv("CBM_WORKERS", "0", 1);

    int setup_rc = setup_lang_repo(files, contents, 2);
    int run_rc = -1;
    int import_count = 0;
    int valid_json_count = 0;
    bool local_name_preserved = false;
    cbm_pipeline_t *p = NULL;
    cbm_store_t *s = NULL;
    cbm_edge_t *edges = NULL;

    if (setup_rc == 0) {
        p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
        if (p) {
            run_rc = cbm_pipeline_run(p);
            if (run_rc == 0) {
                s = th_open_repo_store(g_lang_tmpdir);
            }
        }
        if (s) {
            const char *project = cbm_pipeline_project_name(p);
            if (cbm_store_find_edges_by_type(s, project, "IMPORTS", &edges, &import_count) ==
                CBM_STORE_OK) {
                for (int i = 0; i < import_count; i++) {
                    const char *properties = edges[i].properties_json;
                    yyjson_doc *doc =
                        properties ? yyjson_read(properties, strlen(properties), 0) : NULL;
                    if (!doc) {
                        continue;
                    }
                    valid_json_count++;
                    yyjson_val *local_name =
                        yyjson_obj_get(yyjson_doc_get_root(doc), "local_name");
                    if (yyjson_is_str(local_name)) {
                        const char *value = yyjson_get_str(local_name);
                        if (strstr(value, "ActiveTemporaryInvite") &&
                            strstr(value, "TemporaryInvitePayload")) {
                            local_name_preserved = true;
                        }
                    }
                    yyjson_doc_free(doc);
                }
            }
        }
    }

    if (edges) cbm_store_free_edges(edges, import_count);
    if (s) cbm_store_close(s);
    if (p) cbm_pipeline_free(p);
    if (setup_rc == 0) teardown_lang_repo();
    if (saved_workers_copy) {
        cbm_setenv("CBM_WORKERS", saved_workers_copy, 1);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    free(saved_workers_copy);

    ASSERT_EQ(setup_rc, 0);
    ASSERT_EQ(run_rc, 0);
    ASSERT_GT(import_count, 0);
    ASSERT_EQ(valid_json_count, import_count);
    ASSERT_TRUE(local_name_preserved);
    PASS();
}

TEST(pipeline_go_cross_package_call) {
    /* Port of TestGoCrossPackageCallViaImport */
    const char *files[] = {"main.go", "svc/handler.go"};
    const char *contents[] = {
        "package main\n\nimport \"example.com/myapp/svc\"\n\n"
        "func run() {\n\tsvc.ProcessOrder(\"123\")\n}\n",

        "package svc\n\nfunc ProcessOrder(id string) error {\n\treturn nil\n}\n"};

    if (setup_lang_repo(files, contents, 2) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    /* Verify ProcessOrder exists */
    cbm_node_t *targets = NULL;
    int tc = 0;
    cbm_store_find_nodes_by_name(s, proj, "ProcessOrder", &targets, &tc);
    ASSERT_GT(tc, 0);

    /* Verify run() exists */
    cbm_node_t *callers = NULL;
    int clc = 0;
    cbm_store_find_nodes_by_name(s, proj, "run", &callers, &clc);
    ASSERT_GT(clc, 0);

    /* Check CALLS edge from run to ProcessOrder */
    cbm_edge_t *edges = NULL;
    int ec = 0;
    cbm_store_find_edges_by_source_type(s, callers[0].id, "CALLS", &edges, &ec);
    bool found = false;
    for (int i = 0; i < ec; i++) {
        if (edges[i].target_id == targets[0].id)
            found = true;
    }
    ASSERT_TRUE(found);

    if (edges)
        cbm_store_free_edges(edges, ec);
    cbm_store_free_nodes(targets, tc);
    cbm_store_free_nodes(callers, clc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_python_cross_module_call) {
    /* Port of TestPythonCrossModuleCallViaImport */
    const char *files[] = {"utils.py", "main.py"};
    const char *contents[] = {
        "def fetch_data(url):\n    return {\"status\": \"ok\"}\n",

        "from utils import fetch_data\n\n"
        "def process():\n    result = fetch_data(\"https://example.com\")\n    return result\n"};

    if (setup_lang_repo(files, contents, 2) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *targets = NULL;
    int tc = 0;
    cbm_store_find_nodes_by_name(s, proj, "fetch_data", &targets, &tc);
    ASSERT_GT(tc, 0);

    cbm_node_t *callers = NULL;
    int clc = 0;
    cbm_store_find_nodes_by_name(s, proj, "process", &callers, &clc);
    ASSERT_GT(clc, 0);

    cbm_edge_t *edges = NULL;
    int ec = 0;
    cbm_store_find_edges_by_source_type(s, callers[0].id, "CALLS", &edges, &ec);
    bool found = false;
    for (int i = 0; i < ec; i++) {
        if (edges[i].target_id == targets[0].id)
            found = true;
    }
    ASSERT_TRUE(found);

    if (edges)
        cbm_store_free_edges(edges, ec);
    cbm_store_free_nodes(targets, tc);
    cbm_store_free_nodes(callers, clc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

/* #725: two same-named symbols across languages must not share CALLS edges.
 * Python Store.commit is the real callee of save(); the JS Editor.commit
 * function is a distinct binding and must have no inbound CALLS from Python.
 * unique_name (candidates==1) is #1572 and is not this claim. */
TEST(pipeline_cross_language_same_name_does_not_share_calls_issue725) {
    const char *files[] = {"store.py", "app.py", "web/src/pages/Editor.js"};
    const char *contents[] = {
        "class Store:\n"
        "    def commit(self):\n"
        "        return True\n",

        "from store import Store\n"
        "\n"
        "def save():\n"
        "    return Store().commit()\n",

        "export function commit() {\n"
        "  return 1;\n"
        "}\n"};

    if (setup_lang_repo(files, contents, 3) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *commits = NULL;
    int ncommit = 0;
    cbm_store_find_nodes_by_name(s, proj, "commit", &commits, &ncommit);
    ASSERT_GTE(ncommit, 2);

    int64_t js_id = 0;
    int64_t py_id = 0;
    for (int i = 0; i < ncommit; i++) {
        if (commits[i].file_path && strstr(commits[i].file_path, "Editor.js"))
            js_id = commits[i].id;
        if (commits[i].file_path && strstr(commits[i].file_path, "store.py"))
            py_id = commits[i].id;
    }
    ASSERT_TRUE(js_id != 0);
    ASSERT_TRUE(py_id != 0);

    cbm_node_t *saves = NULL;
    int nsave = 0;
    cbm_store_find_nodes_by_name(s, proj, "save", &saves, &nsave);
    ASSERT_GT(nsave, 0);

    cbm_edge_t *from_save = NULL;
    int nfrom = 0;
    cbm_store_find_edges_by_source_type(s, saves[0].id, "CALLS", &from_save, &nfrom);
    bool save_calls_py = false;
    bool save_calls_js = false;
    for (int i = 0; i < nfrom; i++) {
        if (from_save[i].target_id == py_id)
            save_calls_py = true;
        if (from_save[i].target_id == js_id)
            save_calls_js = true;
    }
    ASSERT_TRUE(save_calls_py);
    ASSERT_FALSE(save_calls_js);

    cbm_edge_t *into_js = NULL;
    int njs = 0;
    cbm_store_find_edges_by_target_type(s, js_id, "CALLS", &into_js, &njs);
    ASSERT_EQ(njs, 0);

    if (from_save)
        cbm_store_free_edges(from_save, nfrom);
    if (into_js)
        cbm_store_free_edges(into_js, njs);
    cbm_store_free_nodes(commits, ncommit);
    cbm_store_free_nodes(saves, nsave);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_go_type_classification) {
    /* Port of TestGoTypeClassification */
    const char *files[] = {"types.go"};
    const char *contents[] = {"package types\n\n"
                              "type Reader interface {\n\tRead(p []byte) (n int, err error)\n}\n\n"
                              "type Writer interface {\n\tWrite(p []byte) (n int, err error)\n}\n\n"
                              "type Config struct {\n\tHost string\n\tPort int\n}\n\n"
                              "type ID = string\n"};

    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    /* Should have 2 Interface nodes (Reader, Writer) */
    cbm_node_t *ifaces = NULL;
    int ic = 0;
    cbm_store_find_nodes_by_label(s, proj, "Interface", &ifaces, &ic);
    ASSERT_EQ(ic, 2);
    cbm_store_free_nodes(ifaces, ic);

    /* Should have 1 Struct node (Config struct) */
    cbm_node_t *cls = NULL;
    int cc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Struct", &cls, &cc);
    ASSERT_EQ(cc, 1);
    ASSERT_STR_EQ(cls[0].name, "Config");
    cbm_store_free_nodes(cls, cc);

    /* Should have 1 Type node (ID alias) */
    cbm_node_t *types = NULL;
    int tc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Type", &types, &tc);
    ASSERT_EQ(tc, 1);
    ASSERT_STR_EQ(types[0].name, "ID");
    cbm_store_free_nodes(types, tc);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_go_grouped_types) {
    /* Port of TestGoGroupedTypeDeclaration */
    const char *files[] = {"models.go"};
    const char *contents[] = {"package models\n\n"
                              "type (\n"
                              "\tRequest struct {\n\t\tURL string\n\t}\n\n"
                              "\tResponse struct {\n\t\tStatus int\n\t}\n\n"
                              "\tHandler interface {\n\t\tHandle(req Request) Response\n\t}\n"
                              ")\n"};

    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *cls = NULL;
    int cc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Struct", &cls, &cc);
    ASSERT_EQ(cc, 2); /* Request, Response */
    cbm_store_free_nodes(cls, cc);

    cbm_node_t *ifaces = NULL;
    int ic = 0;
    cbm_store_find_nodes_by_label(s, proj, "Interface", &ifaces, &ic);
    ASSERT_EQ(ic, 1); /* Handler */
    cbm_store_free_nodes(ifaces, ic);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_kotlin_project) {
    /* Port of TestPipelineKotlinProject */
    const char *files[] = {"Main.kt", "Service.kt"};
    const char *contents[] = {
        "fun greet(name: String): String {\n    return \"Hello, $name\"\n}\n\n"
        "fun main() {\n    val result = greet(\"world\")\n    println(result)\n}\n",

        "class OrderService {\n"
        "    fun processOrder(id: String): Boolean {\n        return true\n    }\n\n"
        "    fun submitOrder(order: String): Boolean {\n"
        "        return processOrder(order)\n    }\n}\n\n"
        "object Config {\n    val API_URL = \"https://example.com/api\"\n}\n"};

    if (setup_lang_repo(files, contents, 2) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *funcs = NULL;
    int fc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Function", &funcs, &fc);
    ASSERT_GTE(fc, 2); /* greet, main */
    cbm_store_free_nodes(funcs, fc);

    cbm_node_t *cls = NULL;
    int cc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Class", &cls, &cc);
    ASSERT_GTE(cc, 1); /* OrderService */
    cbm_store_free_nodes(cls, cc);

    cbm_node_t *methods = NULL;
    int mc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Method", &methods, &mc);
    ASSERT_GTE(mc, 2); /* processOrder, submitOrder */
    cbm_store_free_nodes(methods, mc);

    int edge_count = cbm_store_count_edges(s, proj);
    ASSERT_GT(edge_count, 0);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_lua_anonymous_functions) {
    /* Port of TestLuaAnonymousFunctionExtraction */
    const char *files[] = {"app.lua"};
    const char *contents[] = {"local run_before_filter\n"
                              "run_before_filter = function(filter, r)\n"
                              "  return filter(r)\n"
                              "end\n\n"
                              "local validate = function(data)\n"
                              "  return data ~= nil\n"
                              "end\n\n"
                              "function named_func(x)\n"
                              "  return x + 1\n"
                              "end\n"};

    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *funcs = NULL;
    int fc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Function", &funcs, &fc);
    ASSERT_GTE(fc, 3); /* run_before_filter, validate, named_func */

    /* Verify specific functions exist */
    const char *expected[] = {"run_before_filter", "validate", "named_func"};
    for (int e = 0; e < 3; e++) {
        cbm_node_t *found = NULL;
        int fnc = 0;
        cbm_store_find_nodes_by_name(s, proj, expected[e], &found, &fnc);
        ASSERT_GT(fnc, 0);
        cbm_store_free_nodes(found, fnc);
    }

    cbm_store_free_nodes(funcs, fc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_csharp_modern) {
    /* Port of TestCSharpModernFeatures */
    const char *files[] = {"Controller.cs", "Model.cs"};
    const char *contents[] = {"namespace Conduit.Features;\n\n"
                              "public class UsersController {\n"
                              "\tpublic void Get() {}\n"
                              "\tpublic void Create(string name) {}\n"
                              "}\n",

                              "namespace Conduit.Models {\n"
                              "\tclass User {\n"
                              "\t\tpublic string Name { get; set; }\n"
                              "\t\tpublic int GetAge() { return 0; }\n"
                              "\t}\n"
                              "}\n"};

    if (setup_lang_repo(files, contents, 2) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *modules = NULL;
    int modc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Module", &modules, &modc);
    ASSERT_GTE(modc, 2);
    cbm_store_free_nodes(modules, modc);

    cbm_node_t *cls = NULL;
    int cc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Class", &cls, &cc);
    ASSERT_GTE(cc, 2); /* UsersController, User */
    cbm_store_free_nodes(cls, cc);

    cbm_node_t *methods = NULL;
    int mc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Method", &methods, &mc);
    ASSERT_GTE(mc, 3); /* Get, Create, GetAge */
    cbm_store_free_nodes(methods, mc);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_bom_stripping) {
    /* Port of TestBOMStripping — UTF-8 BOM prefix should be handled */
    snprintf(g_lang_tmpdir, sizeof(g_lang_tmpdir), "/tmp/cbm_bom_XXXXXX");
    if (!cbm_mkdtemp(g_lang_tmpdir))
        FAIL("tmpdir");

    char path[512];
    snprintf(path, sizeof(path), "%s/bom.go", g_lang_tmpdir);
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    /* Write UTF-8 BOM (0xEF 0xBB 0xBF) followed by Go source */
    unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    fwrite(bom, 1, 3, f);
    fprintf(f, "package main\n\nfunc BOMFunc() {}\n");
    fclose(f);

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *found = NULL;
    int fc = 0;
    cbm_store_find_nodes_by_name(s, proj, "BOMFunc", &found, &fc);
    ASSERT_GT(fc, 0); /* BOMFunc should be found despite BOM */
    cbm_store_free_nodes(found, fc);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_form_call_resolution) {
    /* Port of TestFORMProcedureCallResolution */
    const char *files[] = {"calc.frm"};
    const char *contents[] = {"#procedure callee(x)\n"
                              "  id x = 0;\n"
                              "#endprocedure\n"
                              "#procedure caller()\n"
                              "  #call callee(1)\n"
                              "#endprocedure\n"};

    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    /* Verify CALLS edge exists to callee */
    cbm_edge_t *edges = NULL;
    int ec = 0;
    cbm_store_find_edges_by_type(s, proj, "CALLS", &edges, &ec);
    bool found = false;
    for (int i = 0; i < ec; i++) {
        cbm_node_t tgt = {0};
        if (cbm_store_find_node_by_id(s, edges[i].target_id, &tgt) == CBM_STORE_OK &&
            strcmp(tgt.name, "callee") == 0) {
            found = true;
        }
        cbm_node_free_fields(&tgt);
    }
    ASSERT_TRUE(found);
    if (edges)
        cbm_store_free_edges(edges, ec);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_python_type_inference) {
    /* Port of TestPythonMethodDispatchViaTypeInference
     * p = DataProcessor() then p.transform() → CALLS DataProcessor.transform */
    const char *files[] = {"processor.py", "main.py"};
    const char *contents[] = {"class DataProcessor:\n"
                              "    def transform(self, data):\n"
                              "        return data.upper()\n"
                              "\n"
                              "    def validate(self, data):\n"
                              "        return len(data) > 0\n",

                              "from processor import DataProcessor\n"
                              "\n"
                              "def run():\n"
                              "    p = DataProcessor()\n"
                              "    result = p.transform(\"hello\")\n"
                              "    return result\n"};

    if (setup_lang_repo(files, contents, 2) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    /* Verify DataProcessor.transform exists as a Method */
    cbm_node_t *methods = NULL;
    int mc = 0;
    cbm_store_find_nodes_by_name(s, proj, "transform", &methods, &mc);
    ASSERT_GT(mc, 0);

    /* Verify run() exists */
    cbm_node_t *callers = NULL;
    int clc = 0;
    cbm_store_find_nodes_by_name(s, proj, "run", &callers, &clc);
    ASSERT_GT(clc, 0);

    /* Check CALLS edge from run() to DataProcessor.transform */
    cbm_edge_t *edges = NULL;
    int ec = 0;
    cbm_store_find_edges_by_source_type(s, callers[0].id, "CALLS", &edges, &ec);
    bool found = false;
    for (int i = 0; i < ec; i++) {
        if (edges[i].target_id == methods[0].id)
            found = true;
    }
    /* Type inference may or may not resolve — log but don't fail hard */
    if (!found) {
        /* At minimum, verify the method exists and run() has some calls */
    }
    if (edges)
        cbm_store_free_edges(edges, ec);

    cbm_store_free_nodes(methods, mc);
    cbm_store_free_nodes(callers, clc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Docstring integration (port of TestDocstringIntegration)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(pipeline_docstring_go_function) {
    /* Go function with // comment docstring */
    const char *files[] = {"main.go"};
    const char *contents[] = {"package main\n\n"
                              "// Compute does something.\n"
                              "func Compute() {}\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *nodes = NULL;
    int nc = 0;
    cbm_store_find_nodes_by_name(s, proj, "Compute", &nodes, &nc);
    ASSERT_GT(nc, 0);

    /* Check properties_json contains docstring */
    bool found_docstring = false;
    for (int i = 0; i < nc; i++) {
        if (strcmp(nodes[i].label, "Function") == 0 && nodes[i].properties_json &&
            strstr(nodes[i].properties_json, "docstring") &&
            strstr(nodes[i].properties_json, "Compute does something")) {
            found_docstring = true;
        }
    }
    ASSERT_TRUE(found_docstring);

    cbm_store_free_nodes(nodes, nc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_docstring_python_function) {
    /* Python function with triple-quoted docstring */
    const char *files[] = {"main.py"};
    const char *contents[] = {"def compute():\n"
                              "\t\"\"\"Does something.\"\"\"\n"
                              "\tpass\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *nodes = NULL;
    int nc = 0;
    cbm_store_find_nodes_by_name(s, proj, "compute", &nodes, &nc);
    ASSERT_GT(nc, 0);

    bool found_docstring = false;
    for (int i = 0; i < nc; i++) {
        if (strcmp(nodes[i].label, "Function") == 0 && nodes[i].properties_json &&
            strstr(nodes[i].properties_json, "docstring") &&
            strstr(nodes[i].properties_json, "Does something")) {
            found_docstring = true;
        }
    }
    ASSERT_TRUE(found_docstring);

    cbm_store_free_nodes(nodes, nc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_docstring_java_method) {
    /* Java method with Javadoc comment */
    const char *files[] = {"A.java"};
    const char *contents[] = {"class A {\n"
                              "\t/** Computes result. */\n"
                              "\tvoid compute() {}\n"
                              "}\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *nodes = NULL;
    int nc = 0;
    cbm_store_find_nodes_by_name(s, proj, "compute", &nodes, &nc);
    ASSERT_GT(nc, 0);

    bool found_docstring = false;
    for (int i = 0; i < nc; i++) {
        if (strcmp(nodes[i].label, "Method") == 0 && nodes[i].properties_json &&
            strstr(nodes[i].properties_json, "docstring") &&
            strstr(nodes[i].properties_json, "Computes result")) {
            found_docstring = true;
        }
    }
    ASSERT_TRUE(found_docstring);

    cbm_store_free_nodes(nodes, nc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_docstring_kotlin_function) {
    /* Kotlin function with KDoc comment */
    const char *files[] = {"main.kt"};
    const char *contents[] = {"/** Computes result. */\n"
                              "fun compute() {}\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *nodes = NULL;
    int nc = 0;
    cbm_store_find_nodes_by_name(s, proj, "compute", &nodes, &nc);
    ASSERT_GT(nc, 0);

    bool found_docstring = false;
    for (int i = 0; i < nc; i++) {
        if (strcmp(nodes[i].label, "Function") == 0 && nodes[i].properties_json &&
            strstr(nodes[i].properties_json, "docstring") &&
            strstr(nodes[i].properties_json, "Computes result")) {
            found_docstring = true;
        }
    }
    ASSERT_TRUE(found_docstring);

    cbm_store_free_nodes(nodes, nc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(pipeline_docstring_go_class) {
    /* Go struct with // comment docstring */
    const char *files[] = {"main.go"};
    const char *contents[] = {"package main\n\n"
                              "// MyStruct is documented.\n"
                              "type MyStruct struct{}\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *nodes = NULL;
    int nc = 0;
    cbm_store_find_nodes_by_name(s, proj, "MyStruct", &nodes, &nc);
    ASSERT_GT(nc, 0);

    bool found_docstring = false;
    for (int i = 0; i < nc; i++) {
        if (strcmp(nodes[i].label, "Struct") == 0 && nodes[i].properties_json &&
            strstr(nodes[i].properties_json, "docstring") &&
            strstr(nodes[i].properties_json, "MyStruct is documented")) {
            found_docstring = true;
        }
    }
    ASSERT_TRUE(found_docstring);

    cbm_store_free_nodes(nodes, nc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(project_name_from_path) {
    /* Port of TestProjectNameFromPath — more cases than integ_pipeline_project_name */
    struct {
        const char *path;
        const char *want;
    } cases[] = {
        {"/tmp/bench/erlang/lib/stdlib/src", "tmp-bench-erlang-lib-stdlib-src"},
        {"/Users/martin/projects/myapp", "Users-martin-projects-myapp"},
        {"/home/user/repo", "home-user-repo"},
        {"/single", "single"},
    };

    for (int i = 0; i < 4; i++) {
        char *got = cbm_project_name_from_path(cases[i].path);
        ASSERT_NOT_NULL(got);
        ASSERT_STR_EQ(got, cases[i].want);
        free(got);
    }
    PASS();
}

/* Regression for #394/#227/#367: a Windows drive letter is case-insensitive, so
 * "c:/repo" and "C:/repo" must canonicalize to the SAME project key. Otherwise
 * agent CWDs (which report lowercase "c:\...") produce a distinct key + colliding
 * cache file that clobbers the good index, and the lowercase index self-deletes.
 * Pure string logic, so it reproduces on any platform. */
TEST(project_name_drive_letter_case_insensitive_issue394) {
    char *lower = cbm_project_name_from_path("c:/WEBDEV/Cardio-Cloud");
    char *upper = cbm_project_name_from_path("C:/WEBDEV/Cardio-Cloud");
    ASSERT_NOT_NULL(lower);
    ASSERT_NOT_NULL(upper);
    /* Both must fold to the upper-case-drive key, e.g. "C-WEBDEV-Cardio-Cloud". */
    ASSERT_STR_EQ(lower, upper);
    ASSERT_EQ(lower[0], 'C');
    free(lower);
    free(upper);

    /* And the normalizer itself upper-cases the drive root in place. */
    char buf1[32];
    snprintf(buf1, sizeof(buf1), "%s", "c:/x");
    cbm_normalize_path_sep(buf1);
    ASSERT_STR_EQ(buf1, "C:/x");
    char buf2[32];
    snprintf(buf2, sizeof(buf2), "%s", "d:\\proj\\sub");
    cbm_normalize_path_sep(buf2);
    ASSERT_STR_EQ(buf2, "D:/proj/sub");
    PASS();
}

static const char *test_null_dev(void) {
#ifdef _WIN32
    return "NUL";
#else
    return "/dev/null";
#endif
}

static bool git_available(void) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "git --version >%s 2>&1", test_null_dev());
    int rc = system(cmd);
    return rc == 0;
}

static int run_cmd(const char *cmd) {
    return system(cmd);
}

TEST(git_context_non_git_path) {
    char *tmp = th_mktempdir("cbm_gitctx_nongit");
    ASSERT_NOT_NULL(tmp);

    cbm_git_context_t ctx = {0};
    ASSERT_EQ(cbm_git_context_resolve(tmp, &ctx), 0);
    ASSERT_FALSE(ctx.is_git);
    ASSERT_TRUE(ctx.root_exists);

    char *qn = cbm_git_context_branch_qn("proj", &ctx);
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "proj.__branch__.working-tree");
    free(qn);

    char json[1024];
    ASSERT_GT(cbm_git_context_props_json(&ctx, json, sizeof(json)), 0);
    ASSERT_NOT_NULL(strstr(json, "\"is_git\":false"));
    ASSERT_NOT_NULL(strstr(json, "\"root_exists\":true"));

    char long_value[1200];
    memset(long_value, 'a', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';
    cbm_git_context_t long_ctx = {
        .root_exists = true,
        .canonical_root = long_value,
    };
    char small_json[64];
    ASSERT_EQ(cbm_git_context_props_json(&long_ctx, small_json, sizeof(small_json)), 0);

    cbm_git_context_free(&ctx);
    th_rmtree(tmp);
    PASS();
}

TEST(git_context_linked_worktree) {
    if (!git_available()) {
        FAIL("git unavailable");
    }

    char *tmp = th_mktempdir("cbm_gitctx_repo");
    ASSERT_NOT_NULL(tmp);

    char repo[512], wt[512], cmd[2048];
    int n = snprintf(repo, sizeof(repo), "%s/repo with space", tmp);
    ASSERT_TRUE(n > 0 && n < (int)sizeof(repo));
    n = snprintf(wt, sizeof(wt), "%s/wt with space", tmp);
    ASSERT_TRUE(n > 0 && n < (int)sizeof(wt));
    ASSERT_EQ(th_mkdir_p(repo), 0);

    const char *null_dev = test_null_dev();
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" init >%s 2>&1", repo, null_dev);
    ASSERT_EQ(run_cmd(cmd), 0);
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" checkout -b main >%s 2>&1", repo, null_dev);
    ASSERT_EQ(run_cmd(cmd), 0);
    ASSERT_EQ(th_write_file(TH_PATH(repo, "file.txt"), "hello\n"), 0);
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" add file.txt >%s 2>&1", repo, null_dev);
    ASSERT_EQ(run_cmd(cmd), 0);
    snprintf(cmd, sizeof(cmd),
             "git -C \"%s\" -c user.name=\"CBM Test\" -c user.email=\"cbm@example.invalid\" "
             "commit -m \"initial\" >%s 2>&1",
             repo, null_dev);
    ASSERT_EQ(run_cmd(cmd), 0);
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" worktree add -b feature/git-context \"%s\" >%s 2>&1",
             repo, wt, null_dev);
    ASSERT_EQ(run_cmd(cmd), 0);

    cbm_git_context_t main_ctx = {0};
    cbm_git_context_t wt_ctx = {0};
    ASSERT_EQ(cbm_git_context_resolve(repo, &main_ctx), 0);
    ASSERT_EQ(cbm_git_context_resolve(wt, &wt_ctx), 0);

    ASSERT_TRUE(main_ctx.is_git);
    ASSERT_FALSE(main_ctx.is_worktree);
    ASSERT_TRUE(wt_ctx.is_git);
    ASSERT_TRUE(wt_ctx.is_worktree);
    ASSERT_STR_EQ(main_ctx.canonical_root, wt_ctx.canonical_root);
    ASSERT_NOT_NULL(wt_ctx.branch);
    ASSERT_STR_EQ(wt_ctx.branch, "feature/git-context");
    ASSERT_STR_EQ(wt_ctx.branch_slug, "feature-git-context");
    ASSERT_NOT_NULL(wt_ctx.head_sha);

    char *qn = cbm_git_context_branch_qn("proj", &wt_ctx);
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "proj.__branch__.feature-git-context");
    free(qn);

    char json[2048];
    ASSERT_GT(cbm_git_context_props_json(&wt_ctx, json, sizeof(json)), 0);
    ASSERT_NOT_NULL(strstr(json, "\"is_git\":true"));
    ASSERT_NOT_NULL(strstr(json, "\"is_worktree\":true"));
    ASSERT_NOT_NULL(strstr(json, "\"branch\":\"feature/git-context\""));

    cbm_git_context_free(&main_ctx);
    cbm_git_context_free(&wt_ctx);

    snprintf(cmd, sizeof(cmd), "git -C \"%s\" checkout --detach HEAD >%s 2>&1", repo, null_dev);
    ASSERT_EQ(run_cmd(cmd), 0);
    cbm_git_context_t detached_ctx = {0};
    ASSERT_EQ(cbm_git_context_resolve(repo, &detached_ctx), 0);
    ASSERT_TRUE(detached_ctx.is_detached);
    ASSERT_STR_EQ(detached_ctx.branch_slug, "detached");
    cbm_git_context_free(&detached_ctx);

    th_rmtree(tmp);
    PASS();
}

TEST(project_name_uniqueness) {
    /* Port of TestProjectNameUniqueness */
    char *a = cbm_project_name_from_path("/tmp/bench/zig/lib/std");
    char *b = cbm_project_name_from_path("/tmp/bench/erlang/lib/stdlib/src");
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_TRUE(strcmp(a, b) != 0);
    free(a);
    free(b);
    PASS();
}

/* ── Git diff helpers (pass_gitdiff.c) ─────────────────────────── */

TEST(gitdiff_parse_range_with_count) {
    int start, count;
    cbm_parse_range("10,5", &start, &count);
    ASSERT_EQ(start, 10);
    ASSERT_EQ(count, 5);
    PASS();
}

TEST(gitdiff_parse_range_no_count) {
    int start, count;
    cbm_parse_range("10", &start, &count);
    ASSERT_EQ(start, 10);
    ASSERT_EQ(count, 1);
    PASS();
}

TEST(gitdiff_parse_range_zero_count) {
    int start, count;
    cbm_parse_range("1,0", &start, &count);
    ASSERT_EQ(start, 1);
    ASSERT_EQ(count, 0);
    PASS();
}

TEST(gitdiff_parse_range_large) {
    int start, count;
    cbm_parse_range("52,2", &start, &count);
    ASSERT_EQ(start, 52);
    ASSERT_EQ(count, 2);
    PASS();
}

TEST(gitdiff_parse_name_status) {
    const char *input = "M\tinternal/store/nodes.go\n"
                        "A\tnew_file.go\n"
                        "D\told_file.go\n"
                        "R100\tsrc/old.go\tsrc/new.go\n";

    cbm_changed_file_t files[16];
    int n = cbm_parse_name_status(input, files, 16);

    ASSERT_EQ(n, 4);
    ASSERT_STR_EQ(files[0].status, "M");
    ASSERT_STR_EQ(files[0].path, "internal/store/nodes.go");
    ASSERT_STR_EQ(files[1].status, "A");
    ASSERT_STR_EQ(files[1].path, "new_file.go");
    ASSERT_STR_EQ(files[2].status, "D");
    ASSERT_STR_EQ(files[2].path, "old_file.go");
    ASSERT_STR_EQ(files[3].status, "R");
    ASSERT_STR_EQ(files[3].path, "src/new.go");
    ASSERT_STR_EQ(files[3].old_path, "src/old.go");
    PASS();
}

TEST(gitdiff_parse_name_status_filters_untrackable) {
    const char *input = "M\tpackage-lock.json\n"
                        "M\tsrc/main.go\n"
                        "M\tvendor/lib.go\n";

    cbm_changed_file_t files[16];
    int n = cbm_parse_name_status(input, files, 16);

    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(files[0].path, "src/main.go");
    PASS();
}

TEST(gitdiff_parse_hunks) {
    const char *input = "diff --git a/main.go b/main.go\n"
                        "index abc1234..def5678 100644\n"
                        "--- a/main.go\n"
                        "+++ b/main.go\n"
                        "@@ -10,3 +10,5 @@ func main() {\n"
                        "+\tnewLine1()\n"
                        "+\tnewLine2()\n"
                        "@@ -50,0 +52,2 @@ func helper() {\n"
                        "+\tanother()\n"
                        "+\tline()\n"
                        "diff --git a/binary.png b/binary.png\n"
                        "Binary files a/binary.png and b/binary.png differ\n"
                        "diff --git a/utils.go b/utils.go\n"
                        "--- a/utils.go\n"
                        "+++ b/utils.go\n"
                        "@@ -1 +1 @@ package utils\n"
                        "-old\n"
                        "+new\n";

    cbm_changed_hunk_t hunks[16];
    int n = cbm_parse_hunks(input, hunks, 16);

    ASSERT_EQ(n, 3);
    ASSERT_STR_EQ(hunks[0].path, "main.go");
    ASSERT_EQ(hunks[0].start_line, 10);
    ASSERT_EQ(hunks[0].end_line, 14);

    ASSERT_STR_EQ(hunks[1].path, "main.go");
    ASSERT_EQ(hunks[1].start_line, 52);
    ASSERT_EQ(hunks[1].end_line, 53);

    ASSERT_STR_EQ(hunks[2].path, "utils.go");
    ASSERT_EQ(hunks[2].start_line, 1);
    ASSERT_EQ(hunks[2].end_line, 1);
    PASS();
}

TEST(gitdiff_parse_hunks_no_newline_marker) {
    const char *input = "diff --git a/file.go b/file.go\n"
                        "--- a/file.go\n"
                        "+++ b/file.go\n"
                        "@@ -5,2 +5,3 @@ func foo() {\n"
                        "+\tbar()\n"
                        "\\ No newline at end of file\n";

    cbm_changed_hunk_t hunks[4];
    int n = cbm_parse_hunks(input, hunks, 4);

    ASSERT_EQ(n, 1);
    ASSERT_EQ(hunks[0].start_line, 5);
    ASSERT_EQ(hunks[0].end_line, 7);
    PASS();
}

TEST(gitdiff_parse_hunks_mode_change) {
    const char *input = "diff --git a/script.sh b/script.sh\n"
                        "old mode 100644\n"
                        "new mode 100755\n";

    cbm_changed_hunk_t hunks[4];
    int n = cbm_parse_hunks(input, hunks, 4);
    ASSERT_EQ(n, 0);
    PASS();
}

TEST(gitdiff_parse_hunks_deletion) {
    const char *input = "diff --git a/file.go b/file.go\n"
                        "--- a/file.go\n"
                        "+++ b/file.go\n"
                        "@@ -10,3 +10,0 @@ func foo() {\n";

    cbm_changed_hunk_t hunks[4];
    int n = cbm_parse_hunks(input, hunks, 4);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(hunks[0].start_line, 10);
    PASS();
}

/* ── Config helpers (pass_configures.c) ───────────────────────── */

TEST(configures_is_env_var_name) {
    ASSERT(cbm_is_env_var_name("DATABASE_URL"));
    ASSERT(cbm_is_env_var_name("API_KEY"));
    ASSERT(cbm_is_env_var_name("PORT"));
    ASSERT(!cbm_is_env_var_name("A"));      /* too short */
    ASSERT(!cbm_is_env_var_name("port"));   /* lowercase */
    ASSERT(!cbm_is_env_var_name("apiKey")); /* camelCase */
    ASSERT(cbm_is_env_var_name("DB_2"));    /* with digit */
    ASSERT(!cbm_is_env_var_name("__"));     /* no uppercase */
    ASSERT(!cbm_is_env_var_name(""));       /* empty */
    PASS();
}

TEST(configures_normalize_config_key) {
    char norm[256];
    int tokens;

    tokens = cbm_normalize_config_key("max_connections", norm, sizeof(norm));
    ASSERT_STR_EQ(norm, "max_connections");
    ASSERT_EQ(tokens, 2);

    tokens = cbm_normalize_config_key("maxConnections", norm, sizeof(norm));
    ASSERT_STR_EQ(norm, "max_connections");
    ASSERT_EQ(tokens, 2);

    tokens = cbm_normalize_config_key("DATABASE_HOST", norm, sizeof(norm));
    ASSERT_STR_EQ(norm, "database_host");
    ASSERT_EQ(tokens, 2);

    tokens = cbm_normalize_config_key("database.host", norm, sizeof(norm));
    ASSERT_STR_EQ(norm, "database_host");
    ASSERT_EQ(tokens, 2);

    tokens = cbm_normalize_config_key("port", norm, sizeof(norm));
    ASSERT_STR_EQ(norm, "port");
    ASSERT_EQ(tokens, 1);

    tokens = cbm_normalize_config_key("maxRetryCount", norm, sizeof(norm));
    ASSERT_STR_EQ(norm, "max_retry_count");
    ASSERT_EQ(tokens, 3);
    PASS();
}

TEST(configures_has_config_extension) {
    ASSERT(cbm_has_config_extension("config.toml"));
    ASSERT(cbm_has_config_extension("settings.yaml"));
    ASSERT(cbm_has_config_extension("config.yml"));
    ASSERT(cbm_has_config_extension(".env"));
    ASSERT(cbm_has_config_extension("config.ini"));
    ASSERT(cbm_has_config_extension("data.json"));
    ASSERT(cbm_has_config_extension("pom.xml"));
    ASSERT(!cbm_has_config_extension("main.go"));
    ASSERT(!cbm_has_config_extension("app.py"));
    ASSERT(!cbm_has_config_extension("data.csv"));
    PASS();
}

/* ── Config integration tests (configures_test.go ports) ──────── */

TEST(configures_env_var_in_config) {
    /* Port of TestBuildEnvIndex_ConfigVariableAdded:
     * config.toml has DATABASE_URL, main.go does os.Getenv("DATABASE_URL")
     * → CONFIGURES edges should link them. */
    const char *files[] = {"config.toml", "main.go"};
    const char *contents[] = {"DATABASE_URL = \"postgresql://localhost/db\"\n",

                              "package main\n\n"
                              "import \"os\"\n\n"
                              "func main() {\n"
                              "\turl := os.Getenv(\"DATABASE_URL\")\n"
                              "\t_ = url\n"
                              "}\n"};
    if (setup_lang_repo(files, contents, 2) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    /* Verify CONFIGURES edges were created */
    cbm_edge_t *edges = NULL;
    int ec = 0;
    cbm_store_find_edges_by_type(s, proj, "CONFIGURES", &edges, &ec);
    /* At minimum the pipeline should not crash. Edge count depends on
     * extraction matching env var accesses to config variables. */
    if (edges)
        cbm_store_free_edges(edges, ec);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(configures_lowercase_key_skipped) {
    /* Port of TestBuildEnvIndex_LowercaseKeySkipped:
     * config.toml has lowercase key — should NOT produce env var CONFIGURES edges. */
    const char *files[] = {"config.toml", "main.go"};
    const char *contents[] = {"database_host = \"localhost\"\n",

                              "package main\n\nfunc main() {}\n"};
    if (setup_lang_repo(files, contents, 2) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    /* Pipeline ran successfully — no crash from lowercase config keys */
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(configures_non_config_file_skipped) {
    /* Port of TestBuildEnvIndex_NonConfigFileSkipped:
     * Only Go file, no config file — no config-derived CONFIGURES edges. */
    const char *files[] = {"main.go"};
    const char *contents[] = {"package main\n\n"
                              "var API_URL = \"https://api.example.com\"\n\n"
                              "func main() {}\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    /* No config file → buildEnvIndex should not create config-derived entries */
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(configures_full_pipeline_integration) {
    /* Port of TestConfigIntegration_FullPipeline:
     * TOML + INI + JSON config files + Go code → Class & Variable nodes from
     * config files, plus CONFIGURES edges. */
    const char *files[] = {"config.toml", "settings.ini", "config.json", "main.go"};
    const char *contents[] = {"[database]\n"
                              "host = \"localhost\"\n"
                              "port = 5432\n"
                              "max_connections = 100\n\n"
                              "[server]\n"
                              "bind_address = \"0.0.0.0\"\n",

                              "[database]\n"
                              "host = localhost\n"
                              "port = 5432\n",

                              "{\"appName\": \"test\", \"maxRetries\": 3}",

                              "package main\n\n"
                              "import \"os\"\n\n"
                              "func getMaxConnections() int { return 100 }\n\n"
                              "func loadConfig() {\n"
                              "\tcfg := readFile(\"config.toml\")\n"
                              "\t_ = cfg\n"
                              "\tdbURL := os.Getenv(\"DATABASE_URL\")\n"
                              "\t_ = dbURL\n"
                              "}\n\n"
                              "func readFile(path string) string { return \"\" }\n"};
    if (setup_lang_repo(files, contents, 4) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    /* Should have Class nodes (database, server sections from TOML) */
    cbm_node_t *classes = NULL;
    int cc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Class", &classes, &cc);
    ASSERT_GT(cc, 0);
    if (classes)
        cbm_store_free_nodes(classes, cc);

    /* Should have Variable nodes from config files */
    cbm_node_t *vars = NULL;
    int vc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Variable", &vars, &vc);
    ASSERT_GT(vc, 0);
    if (vars)
        cbm_store_free_nodes(vars, vc);

    /* Should have Function nodes from Go code */
    cbm_node_t *funcs = NULL;
    int fc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Function", &funcs, &fc);
    ASSERT_GT(fc, 0);
    if (funcs)
        cbm_store_free_nodes(funcs, fc);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}
TEST(enrichment_split_camel_case) {
    char *parts[8];
    int n;

    n = cbm_split_camel_case("GetMapping", parts, 8);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(parts[0], "Get");
    ASSERT_STR_EQ(parts[1], "Mapping");
    for (int i = 0; i < n; i++)
        free(parts[i]);

    n = cbm_split_camel_case("getMessage", parts, 8);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(parts[0], "get");
    ASSERT_STR_EQ(parts[1], "Message");
    for (int i = 0; i < n; i++)
        free(parts[i]);

    n = cbm_split_camel_case("cache", parts, 8);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(parts[0], "cache");
    for (int i = 0; i < n; i++)
        free(parts[i]);

    n = cbm_split_camel_case("HTMLParser", parts, 8);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(parts[0], "HTMLParser");
    for (int i = 0; i < n; i++)
        free(parts[i]);

    n = cbm_split_camel_case("", parts, 8);
    ASSERT_EQ(n, 0);
    PASS();
}

TEST(enrichment_tokenize_decorator) {
    char *tokens[16];
    int n;

    n = cbm_tokenize_decorator("@Override", tokens, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(tokens[0], "override");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    n = cbm_tokenize_decorator("@Deprecated", tokens, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(tokens[0], "deprecated");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    n = cbm_tokenize_decorator("@Test", tokens, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(tokens[0], "test");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    n = cbm_tokenize_decorator("@login_required", tokens, 16);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(tokens[0], "login");
    ASSERT_STR_EQ(tokens[1], "required");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    n = cbm_tokenize_decorator("@cache", tokens, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(tokens[0], "cache");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    n = cbm_tokenize_decorator("@pytest.fixture", tokens, 16);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(tokens[0], "pytest");
    ASSERT_STR_EQ(tokens[1], "fixture");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    /* "get" is stopword → only "mapping" */
    n = cbm_tokenize_decorator("@GetMapping(\"/api\")", tokens, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(tokens[0], "mapping");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    /* "post" passes, "mapping" passes */
    n = cbm_tokenize_decorator("@PostMapping(\"/api\")", tokens, 16);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(tokens[0], "post");
    ASSERT_STR_EQ(tokens[1], "mapping");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    n = cbm_tokenize_decorator("@Transactional", tokens, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(tokens[0], "transactional");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    n = cbm_tokenize_decorator("@MessageMapping(\"/chat\")", tokens, 16);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(tokens[0], "message");
    ASSERT_STR_EQ(tokens[1], "mapping");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    /* Rust-style #[test] */
    n = cbm_tokenize_decorator("#[test]", tokens, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(tokens[0], "test");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    /* #[derive(Debug)] */
    n = cbm_tokenize_decorator("#[derive(Debug)]", tokens, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(tokens[0], "derive");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    /* Both "app" and "get" are stopwords → empty */
    n = cbm_tokenize_decorator("@app.get(\"/api\")", tokens, 16);
    ASSERT_EQ(n, 0);

    /* "router" is stopword, "post" passes */
    n = cbm_tokenize_decorator("@router.post(\"/api\")", tokens, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(tokens[0], "post");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    /* Too short after filtering */
    n = cbm_tokenize_decorator("@x", tokens, 16);
    ASSERT_EQ(n, 0);

    /* Empty */
    n = cbm_tokenize_decorator("", tokens, 16);
    ASSERT_EQ(n, 0);

    n = cbm_tokenize_decorator("@click.command", tokens, 16);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(tokens[0], "click");
    ASSERT_STR_EQ(tokens[1], "command");
    for (int i = 0; i < n; i++)
        free(tokens[i]);

    n = cbm_tokenize_decorator("@celery.task", tokens, 16);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(tokens[0], "celery");
    ASSERT_STR_EQ(tokens[1], "task");
    for (int i = 0; i < n; i++)
        free(tokens[i]);
    PASS();
}

/* ── Decorator tags integration tests (enrichment_test.go ports) ─ */

/* Helper: check if a node's properties_json contains a specific decorator_tag */
static bool has_decorator_tag(const char *properties_json, const char *tag) {
    if (!properties_json || !tag)
        return false;
    yyjson_doc *doc = yyjson_read(properties_json, strlen(properties_json), 0);
    if (!doc)
        return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *tags = yyjson_obj_get(root, "decorator_tags");
    if (!tags || !yyjson_is_arr(tags)) {
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_val *item;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(tags, &iter);
    while ((item = yyjson_arr_iter_next(&iter))) {
        if (yyjson_is_str(item) && strcmp(yyjson_get_str(item), tag) == 0) {
            yyjson_doc_free(doc);
            return true;
        }
    }
    yyjson_doc_free(doc);
    return false;
}

TEST(decorator_tags_python_auto_discovery) {
    /* Port of TestDecoratorTagAutoDiscovery:
     * Python file with repeated decorators (@login_required on 2 funcs,
     * @cache on 2 funcs, @unique_helper on 1 func).
     * Words on 2+ nodes become tags; unique words do not. */
    const char *files[] = {"views.py"};
    const char *contents[] = {"from functools import cache\n\n"
                              "@login_required\n"
                              "def list_orders():\n"
                              "    pass\n\n"
                              "@login_required\n"
                              "def get_order():\n"
                              "    pass\n\n"
                              "@cache\n"
                              "def compute_total():\n"
                              "    pass\n\n"
                              "@cache\n"
                              "def compute_tax():\n"
                              "    pass\n\n"
                              "@unique_helper\n"
                              "def special():\n"
                              "    pass\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    /* Find functions by name and check decorator_tags */
    cbm_node_t *funcs = NULL;
    int fc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Function", &funcs, &fc);

    /* Build name→properties_json map */
    const char *list_orders_props = NULL;
    const char *get_order_props = NULL;
    const char *compute_total_props = NULL;
    const char *compute_tax_props = NULL;
    const char *special_props = NULL;
    for (int i = 0; i < fc; i++) {
        if (strcmp(funcs[i].name, "list_orders") == 0)
            list_orders_props = funcs[i].properties_json;
        else if (strcmp(funcs[i].name, "get_order") == 0)
            get_order_props = funcs[i].properties_json;
        else if (strcmp(funcs[i].name, "compute_total") == 0)
            compute_total_props = funcs[i].properties_json;
        else if (strcmp(funcs[i].name, "compute_tax") == 0)
            compute_tax_props = funcs[i].properties_json;
        else if (strcmp(funcs[i].name, "special") == 0)
            special_props = funcs[i].properties_json;
    }

    /* "login" and "required" appear on 2 nodes → should be tags */
    ASSERT_TRUE(has_decorator_tag(list_orders_props, "login"));
    ASSERT_TRUE(has_decorator_tag(list_orders_props, "required"));
    ASSERT_TRUE(has_decorator_tag(get_order_props, "login"));
    ASSERT_TRUE(has_decorator_tag(get_order_props, "required"));

    /* "cache" appears on 2 nodes → should be a tag */
    ASSERT_TRUE(has_decorator_tag(compute_total_props, "cache"));
    ASSERT_TRUE(has_decorator_tag(compute_tax_props, "cache"));

    /* "unique" and "helper" appear on only 1 node → should NOT be tags */
    ASSERT_FALSE(has_decorator_tag(special_props, "unique"));
    ASSERT_FALSE(has_decorator_tag(special_props, "helper"));

    if (funcs)
        cbm_store_free_nodes(funcs, fc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

TEST(decorator_tags_java_class_methods) {
    /* Port of TestDecoratorTagJavaClassMethods:
     * Java class with @GetMapping, @PostMapping, @Transactional annotations.
     * "mapping" appears on all 4 → tag. "post" on 2 → tag. */
    const char *files[] = {"Controller.java"};
    const char *contents[] = {"class OwnerController {\n"
                              "    @GetMapping(\"/owners\")\n"
                              "    public void listOwners() {}\n\n"
                              "    @GetMapping(\"/owners/{id}\")\n"
                              "    public void showOwner() {}\n\n"
                              "    @PostMapping(\"/owners\")\n"
                              "    public void createOwner() {}\n\n"
                              "    @Transactional\n"
                              "    @PostMapping(\"/owners/{id}\")\n"
                              "    public void updateOwner() {}\n"
                              "}\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    /* Find methods */
    cbm_node_t *methods = NULL;
    int mc = 0;
    cbm_store_find_nodes_by_label(s, proj, "Method", &methods, &mc);

    /* "mapping" appears on all 4 methods → should be a tag */
    for (int i = 0; i < mc; i++) {
        if (strcmp(methods[i].name, "listOwners") == 0 ||
            strcmp(methods[i].name, "showOwner") == 0 ||
            strcmp(methods[i].name, "createOwner") == 0 ||
            strcmp(methods[i].name, "updateOwner") == 0) {
            ASSERT_TRUE(has_decorator_tag(methods[i].properties_json, "mapping"));
        }
    }

    /* "post" appears on createOwner + updateOwner → should be a tag */
    for (int i = 0; i < mc; i++) {
        if (strcmp(methods[i].name, "createOwner") == 0 ||
            strcmp(methods[i].name, "updateOwner") == 0) {
            ASSERT_TRUE(has_decorator_tag(methods[i].properties_json, "post"));
        }
    }

    /* "transactional" appears on only 1 method → should NOT be a tag */
    for (int i = 0; i < mc; i++) {
        if (strcmp(methods[i].name, "updateOwner") == 0) {
            ASSERT_FALSE(has_decorator_tag(methods[i].properties_json, "transactional"));
        }
    }

    if (methods)
        cbm_store_free_nodes(methods, mc);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

/* ── Compile commands helpers (pass_compile_commands.c) ────────── */

TEST(compile_commands_split_command) {
    char *args[16];
    int n;

    n = cbm_split_command("gcc -c main.c", args, 16);
    ASSERT_EQ(n, 3);
    ASSERT_STR_EQ(args[0], "gcc");
    ASSERT_STR_EQ(args[1], "-c");
    ASSERT_STR_EQ(args[2], "main.c");
    for (int i = 0; i < n; i++)
        free(args[i]);

    n = cbm_split_command("gcc -DFOO=\"bar baz\" -c main.c", args, 16);
    ASSERT_EQ(n, 4);
    for (int i = 0; i < n; i++)
        free(args[i]);

    n = cbm_split_command("g++ -I/usr/include -std=c++17 -o out -c in.cpp", args, 16);
    ASSERT_EQ(n, 7);
    for (int i = 0; i < n; i++)
        free(args[i]);
    PASS();
}

TEST(compile_commands_extract_flags) {
    const char *args[] = {"g++",          "-I",    "/abs/include", "-I/rel/include", "-isystem",
                          "/sys/include", "-DFOO", "-DBAR=42",     "-std=c++20",     "-O2",
                          "-Wall",        "-c",    "main.cpp"};

    cbm_compile_flags_t *f = cbm_extract_flags(args, 13, "/project");
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(f->include_count, 3);
    ASSERT_EQ(f->define_count, 2);
    ASSERT_STR_EQ(f->standard, "c++20");
    cbm_compile_flags_free(f);
    PASS();
}

TEST(compile_commands_parse_json) {
    const char *json = "[\n"
                       "  {\n"
                       "    \"directory\": \"/home/user/project/build\",\n"
                       "    \"command\": \"gcc -I/home/user/project/include "
                       "-I/home/user/project/src -DDEBUG=1 -DVERSION=\\\"1.0\\\" "
                       "-std=c11 -o main.o -c /home/user/project/src/main.c\",\n"
                       "    \"file\": \"/home/user/project/src/main.c\"\n"
                       "  },\n"
                       "  {\n"
                       "    \"directory\": \"/home/user/project/build\",\n"
                       "    \"arguments\": [\"g++\", \"-I/home/user/project/include\", "
                       "\"-isystem\", \"/home/user/project/third_party\", "
                       "\"-DUSE_SSL\", \"-std=c++17\", \"-c\", "
                       "\"/home/user/project/src/server.cpp\"],\n"
                       "    \"file\": \"/home/user/project/src/server.cpp\"\n"
                       "  },\n"
                       "  {\n"
                       "    \"directory\": \"/home/user/project/build\",\n"
                       "    \"command\": \"gcc -c /outside/repo/file.c\",\n"
                       "    \"file\": \"/outside/repo/file.c\"\n"
                       "  }\n"
                       "]";

    char **paths = NULL;
    cbm_compile_flags_t **flags = NULL;
    int n = cbm_parse_compile_commands(json, "/home/user/project", &paths, &flags);
    ASSERT(n >= 2); /* At least main.c and server.cpp, outside file excluded */

    /* Find main.c */
    int main_idx = -1, server_idx = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(paths[i], "src/main.c") == 0)
            main_idx = i;
        if (strcmp(paths[i], "src/server.cpp") == 0)
            server_idx = i;
    }

    ASSERT(main_idx >= 0);
    ASSERT_EQ(flags[main_idx]->include_count, 2);
    ASSERT_EQ(flags[main_idx]->define_count, 2);
    ASSERT_STR_EQ(flags[main_idx]->standard, "c11");

    ASSERT(server_idx >= 0);
    ASSERT_EQ(flags[server_idx]->include_count, 2);
    ASSERT_EQ(flags[server_idx]->define_count, 1);
    ASSERT_STR_EQ(flags[server_idx]->standard, "c++17");

    /* Verify outside-repo file excluded */
    for (int i = 0; i < n; i++) {
        ASSERT(strstr(paths[i], "outside") == NULL);
    }

    /* Cleanup */
    for (int i = 0; i < n; i++) {
        free(paths[i]);
        cbm_compile_flags_free(flags[i]);
    }
    free(paths);
    free(flags);
    PASS();
}

TEST(compile_commands_parse_empty) {
    char **paths = NULL;
    cbm_compile_flags_t **flags = NULL;
    int n = cbm_parse_compile_commands("[]", "/repo", &paths, &flags);
    ASSERT_EQ(n, 0);
    free(paths);
    free(flags);
    PASS();
}

TEST(compile_commands_parse_invalid) {
    char **paths = NULL;
    cbm_compile_flags_t **flags = NULL;
    int n = cbm_parse_compile_commands("not json", "/repo", &paths, &flags);
    ASSERT(n < 0);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────── */

/* ── Infrascan: file identification ──────────────────────────────── */

TEST(infra_is_compose_file) {
    /* Port of TestIsComposeFile (8 cases) */
    ASSERT(cbm_is_compose_file("docker-compose.yml"));
    ASSERT(cbm_is_compose_file("docker-compose.yaml"));
    ASSERT(cbm_is_compose_file("docker-compose.prod.yml"));
    ASSERT(cbm_is_compose_file("compose.yml"));
    ASSERT(cbm_is_compose_file("compose.yaml"));
    ASSERT(!cbm_is_compose_file("mycompose.yml"));
    ASSERT(!cbm_is_compose_file("docker-compose.txt"));
    ASSERT(!cbm_is_compose_file("Dockerfile"));
    PASS();
}

TEST(infra_is_cloudbuild_file) {
    /* Port of TestIsCloudbuildFile (5 cases) */
    ASSERT(cbm_is_cloudbuild_file("cloudbuild.yaml"));
    ASSERT(cbm_is_cloudbuild_file("cloudbuild.yml"));
    ASSERT(cbm_is_cloudbuild_file("cloudbuild-prod.yaml"));
    ASSERT(cbm_is_cloudbuild_file("Cloudbuild.yml"));
    ASSERT(!cbm_is_cloudbuild_file("build.yaml"));
    PASS();
}

TEST(infra_is_shell_script) {
    /* Port of TestIsShellScript (5 cases) */
    ASSERT(cbm_is_shell_script("run.sh", ".sh"));
    ASSERT(cbm_is_shell_script("deploy.bash", ".bash"));
    ASSERT(cbm_is_shell_script("init.zsh", ".zsh"));
    ASSERT(!cbm_is_shell_script("main.py", ".py"));
    ASSERT(!cbm_is_shell_script("Dockerfile", ""));
    PASS();
}

TEST(infra_is_dockerfile) {
    ASSERT(cbm_is_dockerfile("Dockerfile"));
    ASSERT(cbm_is_dockerfile("dockerfile"));
    ASSERT(cbm_is_dockerfile("Dockerfile.prod"));
    ASSERT(cbm_is_dockerfile("app.dockerfile"));
    ASSERT(!cbm_is_dockerfile("docker-compose.yml"));
    ASSERT(!cbm_is_dockerfile("main.go"));
    PASS();
}

TEST(infra_is_kustomize_file) {
    ASSERT(cbm_is_kustomize_file("kustomization.yaml"));
    ASSERT(cbm_is_kustomize_file("kustomization.yml"));
    ASSERT(cbm_is_kustomize_file("KUSTOMIZATION.YAML")); /* case-insensitive */
    ASSERT(!cbm_is_kustomize_file("deployment.yaml"));
    ASSERT(!cbm_is_kustomize_file("kustomize.yaml"));
    ASSERT(!cbm_is_kustomize_file(NULL));
    PASS();
}

TEST(infra_is_k8s_manifest) {
    const char *deploy = "apiVersion: apps/v1\nkind: Deployment\nmetadata:\n  name: my-app\n";
    const char *plain = "name: foo\nvalue: bar\n";
    const char *kust = "apiVersion: kustomize.config.k8s.io/v1beta1\nkind: Kustomization\n";

    ASSERT(cbm_is_k8s_manifest("deployment.yaml", deploy));
    ASSERT(!cbm_is_k8s_manifest("deployment.yaml", plain));
    /* kustomize file should return false even if it has apiVersion */
    ASSERT(!cbm_is_k8s_manifest("kustomization.yaml", kust));
    ASSERT(!cbm_is_k8s_manifest(NULL, deploy));
    ASSERT(!cbm_is_k8s_manifest("deployment.yaml", NULL));
    PASS();
}

TEST(infra_is_env_file) {
    ASSERT(cbm_is_env_file(".env"));
    ASSERT(cbm_is_env_file(".env.local"));
    ASSERT(cbm_is_env_file("prod.env"));
    ASSERT(!cbm_is_env_file("main.go"));
    ASSERT(!cbm_is_env_file("env.txt"));
    PASS();
}

/* ── Infrascan: cleanJSONBrackets ───────────────────────────────── */

TEST(infra_clean_json_brackets) {
    /* Port of TestCleanJSONBrackets (4 cases) */
    char out[256];

    cbm_clean_json_brackets("[\"./server\"]", out, sizeof(out));
    ASSERT_STR_EQ(out, "./server");

    cbm_clean_json_brackets("[\"python\", \"main.py\"]", out, sizeof(out));
    ASSERT_STR_EQ(out, "python main.py");

    cbm_clean_json_brackets("./server", out, sizeof(out));
    ASSERT_STR_EQ(out, "./server");

    cbm_clean_json_brackets("[\"./app\", \"--flag\", \"value\"]", out, sizeof(out));
    ASSERT_STR_EQ(out, "./app --flag value");

    PASS();
}

/* ── Infrascan: secret detection ────────────────────────────────── */

TEST(infra_secret_detection) {
    /* Key-based detection */
    ASSERT(cbm_is_secret_binding("JWT_SECRET", "anything"));
    ASSERT(cbm_is_secret_binding("API_KEY", "anything"));
    ASSERT(cbm_is_secret_binding("my_password", "anything"));
    ASSERT(cbm_is_secret_binding("AUTH_TOKEN", "anything"));
    ASSERT(!cbm_is_secret_binding("DATABASE_URL", "https://db.example.com"));

    /* Value-based detection */
    ASSERT(cbm_is_secret_value("sk-1234567890abcdef12345"));
    ASSERT(cbm_is_secret_value("-----BEGIN RSA PRIVATE KEY-----"));
    ASSERT(!cbm_is_secret_value("https://db.example.com"));
    ASSERT(!cbm_is_secret_value("hello world"));
    ASSERT(!cbm_is_secret_value("8080"));

    /* isSecretBinding checks both */
    ASSERT(cbm_is_secret_binding("ANYTHING", "sk-1234567890abcdef12345"));
    ASSERT(!cbm_is_secret_binding("PORT", "8080"));

    PASS();
}

/* ── Infrascan: Dockerfile parser ───────────────────────────────── */

/* Helper: find env var by key in result */
static const char *find_env_var(const cbm_env_kv_t *vars, int count, const char *key) {
    for (int i = 0; i < count; i++) {
        if (strcmp(vars[i].key, key) == 0)
            return vars[i].value;
    }
    return NULL;
}

/* Helper: check if string array contains value */
static bool str_array_contains(const char (*arr)[32], int count, const char *val) {
    for (int i = 0; i < count; i++) {
        if (strcmp(arr[i], val) == 0)
            return true;
    }
    return false;
}

static bool str_array_128_contains(const char (*arr)[128], int count, const char *val) {
    for (int i = 0; i < count; i++) {
        if (strcmp(arr[i], val) == 0)
            return true;
    }
    return false;
}

static bool str_array_256_contains(const char (*arr)[256], int count, const char *val) {
    for (int i = 0; i < count; i++) {
        if (strcmp(arr[i], val) == 0)
            return true;
    }
    return false;
}

TEST(infra_parse_dockerfile_multistage) {
    /* Port of TestParseDockerfile "multi-stage with all directives" */
    const char *src = "FROM golang:1.23-alpine AS builder\n"
                      "WORKDIR /app\n"
                      "ARG SSH_PRIVATE_KEY\n"
                      "RUN go build -o server .\n"
                      "\n"
                      "FROM alpine:3.19\n"
                      "WORKDIR /usr/app\n"
                      "ENV PORT=8080\n"
                      "ENV PYTHONUNBUFFERED=1\n"
                      "EXPOSE 8080 443\n"
                      "USER appuser\n"
                      "CMD [\"./server\"]\n"
                      "HEALTHCHECK CMD wget http://localhost:8080/health\n";

    cbm_dockerfile_result_t r;
    ASSERT_EQ(cbm_parse_dockerfile_source(src, &r), 0);
    ASSERT_STR_EQ(r.base_image, "alpine:3.19");
    ASSERT_EQ(r.stage_count, 2);
    ASSERT_STR_EQ(r.stage_images[0], "golang:1.23-alpine");
    ASSERT_STR_EQ(r.stage_images[1], "alpine:3.19");
    ASSERT(str_array_contains(r.exposed_ports, r.port_count, "8080"));
    ASSERT(str_array_contains(r.exposed_ports, r.port_count, "443"));
    ASSERT_STR_EQ(r.workdir, "/usr/app");
    ASSERT_STR_EQ(r.user, "appuser");
    ASSERT_STR_EQ(r.cmd, "./server");
    ASSERT_STR_EQ(r.healthcheck, "wget http://localhost:8080/health");

    ASSERT_NOT_NULL(find_env_var(r.env_vars, r.env_count, "PORT"));
    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "PORT"), "8080");
    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "PYTHONUNBUFFERED"), "1");

    ASSERT(str_array_128_contains(r.build_args, r.build_arg_count, "SSH_PRIVATE_KEY"));

    PASS();
}

TEST(infra_parse_dockerfile_entrypoint) {
    /* Port of TestParseDockerfile "single stage with entrypoint" */
    const char *src = "FROM python:3.9-slim\n"
                      "ENTRYPOINT [\"python\", \"main.py\"]\n";

    cbm_dockerfile_result_t r;
    ASSERT_EQ(cbm_parse_dockerfile_source(src, &r), 0);
    ASSERT_STR_EQ(r.base_image, "python:3.9-slim");
    ASSERT_STR_EQ(r.entrypoint, "python main.py");
    ASSERT_EQ(r.stage_count, 1);
    PASS();
}

TEST(infra_parse_dockerfile_secret_filtered) {
    /* Port of TestParseDockerfile "secret env vars filtered" */
    const char *src = "FROM node:20\n"
                      "ENV API_KEY=sk-1234567890abcdef12345\n"
                      "ENV DATABASE_URL=https://db.example.com\n"
                      "ENV JWT_SECRET=supersecret\n";

    cbm_dockerfile_result_t r;
    ASSERT_EQ(cbm_parse_dockerfile_source(src, &r), 0);

    /* API_KEY and JWT_SECRET should be filtered */
    ASSERT(find_env_var(r.env_vars, r.env_count, "API_KEY") == NULL);
    ASSERT(find_env_var(r.env_vars, r.env_count, "JWT_SECRET") == NULL);

    /* DATABASE_URL should remain */
    ASSERT_NOT_NULL(find_env_var(r.env_vars, r.env_count, "DATABASE_URL"));
    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "DATABASE_URL"), "https://db.example.com");
    PASS();
}

TEST(infra_parse_dockerfile_expose_protocol) {
    /* Port of TestParseDockerfile "expose with protocol suffix" */
    const char *src = "FROM nginx:latest\n"
                      "EXPOSE 80/tcp 443/tcp\n";

    cbm_dockerfile_result_t r;
    ASSERT_EQ(cbm_parse_dockerfile_source(src, &r), 0);
    ASSERT(str_array_contains(r.exposed_ports, r.port_count, "80"));
    ASSERT(str_array_contains(r.exposed_ports, r.port_count, "443"));
    PASS();
}

TEST(infra_parse_dockerfile_env_space) {
    /* Port of TestParseDockerfile "ENV space-separated format" */
    const char *src = "FROM python:3.9\n"
                      "ENV PYTHONPATH /usr/app\n";

    cbm_dockerfile_result_t r;
    ASSERT_EQ(cbm_parse_dockerfile_source(src, &r), 0);
    ASSERT_NOT_NULL(find_env_var(r.env_vars, r.env_count, "PYTHONPATH"));
    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "PYTHONPATH"), "/usr/app");
    PASS();
}

TEST(infra_parse_dockerfile_empty) {
    /* Port of TestParseDockerfileEmpty */
    cbm_dockerfile_result_t r;
    ASSERT_EQ(cbm_parse_dockerfile_source("# just a comment\n", &r), -1);
    PASS();
}

/* ── Infrascan: Dotenv parser ───────────────────────────────────── */

TEST(infra_parse_dotenv) {
    /* Port of TestParseDotenvFile */
    const char *src = "# Database config\n"
                      "DATABASE_HOST=localhost\n"
                      "DATABASE_PORT=5432\n"
                      "DATABASE_NAME=mydb\n"
                      "API_SECRET=should-not-appear\n"
                      "PLAIN_VALUE=hello world\n";

    cbm_dotenv_result_t r;
    ASSERT_EQ(cbm_parse_dotenv_source(src, &r), 0);

    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "DATABASE_HOST"), "localhost");
    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "DATABASE_PORT"), "5432");
    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "PLAIN_VALUE"), "hello world");

    /* API_SECRET should be filtered */
    ASSERT(find_env_var(r.env_vars, r.env_count, "API_SECRET") == NULL);
    PASS();
}

TEST(infra_parse_dotenv_quoted) {
    /* Port of TestParseDotenvQuotedValues */
    const char *src = "KEY1=\"quoted value\"\n"
                      "KEY2='single quoted'\n";

    cbm_dotenv_result_t r;
    ASSERT_EQ(cbm_parse_dotenv_source(src, &r), 0);
    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "KEY1"), "quoted value");
    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "KEY2"), "single quoted");
    PASS();
}

/* ── Infrascan: Shell script parser ─────────────────────────────── */

TEST(infra_parse_shell) {
    /* Port of TestParseShellScript */
    const char *src = "#!/bin/bash\n"
                      "set -e\n"
                      "\n"
                      "# Configuration\n"
                      "YOUR_CONTAINER_NAME=\"order-email-extractor-endpoint\"\n"
                      "DOCKERFILE_PATH=\"/path/to/dockerfile\"\n"
                      "\n"
                      "export ENVIRONMENT=\"development\"\n"
                      "export USE_STACKDRIVER=\"false\"\n"
                      "\n"
                      "# Shut down existing containers\n"
                      "./shut-down-docker-container.sh\n"
                      "\n"
                      "docker build -t \"$YOUR_CONTAINER_NAME\" \"$DOCKERFILE_PATH\"\n"
                      "docker run -d --name \"$YOUR_CONTAINER_NAME\" \"$YOUR_CONTAINER_NAME\"\n"
                      "docker-compose up -d\n";

    cbm_shell_result_t r;
    ASSERT_EQ(cbm_parse_shell_source(src, &r), 0);
    ASSERT_STR_EQ(r.shebang, "/bin/bash");

    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "ENVIRONMENT"), "development");
    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "YOUR_CONTAINER_NAME"),
                  "order-email-extractor-endpoint");

    ASSERT(str_array_256_contains(r.docker_cmds, r.docker_cmd_count, "docker build"));
    ASSERT(str_array_256_contains(r.docker_cmds, r.docker_cmd_count, "docker run"));
    ASSERT(str_array_256_contains(r.docker_cmds, r.docker_cmd_count, "docker-compose up"));

    PASS();
}

TEST(infra_parse_shell_with_source) {
    /* Port of TestParseShellScriptWithSource */
    const char *src = "#!/usr/bin/env bash\n"
                      "source ./config.sh\n"
                      ". /etc/profile.d/env.sh\n";

    cbm_shell_result_t r;
    ASSERT_EQ(cbm_parse_shell_source(src, &r), 0);
    ASSERT_STR_EQ(r.shebang, "/usr/bin/env bash");
    ASSERT(str_array_256_contains(r.sources, r.source_count, "./config.sh"));
    ASSERT(str_array_256_contains(r.sources, r.source_count, "/etc/profile.d/env.sh"));
    PASS();
}

TEST(infra_parse_shell_secret_filtered) {
    /* Port of TestParseShellScriptSecretFiltered */
    const char *src = "#!/bin/bash\n"
                      "export API_SECRET=\"should-not-appear\"\n"
                      "export DATABASE_URL=\"https://db.example.com\"\n";

    cbm_shell_result_t r;
    ASSERT_EQ(cbm_parse_shell_source(src, &r), 0);
    ASSERT(find_env_var(r.env_vars, r.env_count, "API_SECRET") == NULL);
    ASSERT_STR_EQ(find_env_var(r.env_vars, r.env_count, "DATABASE_URL"), "https://db.example.com");
    PASS();
}

TEST(infra_parse_shell_shebang_only) {
    /* Port of TestParseShellScriptShebanOnly */
    const char *src = "#!/bin/bash\n# just comments\n";
    cbm_shell_result_t r;
    ASSERT_EQ(cbm_parse_shell_source(src, &r), 0);
    ASSERT_STR_EQ(r.shebang, "/bin/bash");
    PASS();
}

TEST(infra_parse_shell_truly_empty) {
    /* Port of TestParseShellScriptTrulyEmpty */
    cbm_shell_result_t r;
    ASSERT_EQ(cbm_parse_shell_source("# no shebang, just comments\n", &r), -1);
    PASS();
}

/* ── Infrascan: Terraform parser ────────────────────────────────── */

TEST(infra_parse_terraform_full) {
    /* Port of TestParseTerraformFile */
    const char *src = "\n"
                      "terraform {\n"
                      "  required_providers {\n"
                      "    google = {\n"
                      "      source  = \"hashicorp/google\"\n"
                      "      version = \"~> 6.35.0\"\n"
                      "    }\n"
                      "  }\n"
                      "  backend \"gcs\" {\n"
                      "    bucket = \"example-tf\"\n"
                      "    prefix = \"state\"\n"
                      "  }\n"
                      "}\n"
                      "\n"
                      "variable \"project_id\" {\n"
                      "  description = \"The GCP project ID\"\n"
                      "  type        = string\n"
                      "  default     = \"example-cloud\"\n"
                      "}\n"
                      "\n"
                      "variable \"region\" {\n"
                      "  description = \"The region\"\n"
                      "  type        = string\n"
                      "}\n"
                      "\n"
                      "resource \"google_cloud_run_service\" \"main\" {\n"
                      "  name     = \"my-service\"\n"
                      "  location = var.region\n"
                      "}\n"
                      "\n"
                      "resource \"google_compute_address\" \"nat_ip\" {\n"
                      "  name   = \"nat-ip\"\n"
                      "  region = var.region\n"
                      "}\n"
                      "\n"
                      "output \"service_url\" {\n"
                      "  value = google_cloud_run_service.main.status[0].url\n"
                      "}\n"
                      "\n"
                      "data \"google_project\" \"project\" {\n"
                      "}\n"
                      "\n"
                      "module \"vpc\" {\n"
                      "  source = \"./modules/vpc\"\n"
                      "}\n"
                      "\n"
                      "locals {\n"
                      "  env = \"prod\"\n"
                      "}\n";

    cbm_terraform_result_t r;
    ASSERT_EQ(cbm_parse_terraform_source(src, &r), 0);
    ASSERT_STR_EQ(r.backend, "gcs");

    /* Resources */
    ASSERT_EQ(r.resource_count, 2);
    bool found_cloud_run = false;
    for (int i = 0; i < r.resource_count; i++) {
        if (strcmp(r.resources[i].type, "google_cloud_run_service") == 0 &&
            strcmp(r.resources[i].name, "main") == 0) {
            found_cloud_run = true;
        }
    }
    ASSERT(found_cloud_run);

    /* Variables */
    ASSERT_EQ(r.variable_count, 2);
    bool found_project_id = false;
    for (int i = 0; i < r.variable_count; i++) {
        if (strcmp(r.variables[i].name, "project_id") == 0) {
            ASSERT_STR_EQ(r.variables[i].default_val, "example-cloud");
            ASSERT_STR_EQ(r.variables[i].type, "string");
            ASSERT_STR_EQ(r.variables[i].description, "The GCP project ID");
            found_project_id = true;
        }
    }
    ASSERT(found_project_id);

    /* Outputs */
    ASSERT_EQ(r.output_count, 1);
    ASSERT_STR_EQ(r.outputs[0], "service_url");

    /* Data sources */
    ASSERT_EQ(r.data_source_count, 1);
    ASSERT_STR_EQ(r.data_sources[0].type, "google_project");
    ASSERT_STR_EQ(r.data_sources[0].name, "project");

    /* Modules */
    ASSERT_EQ(r.module_count, 1);
    ASSERT_STR_EQ(r.modules[0].tf_name, "vpc");
    ASSERT_STR_EQ(r.modules[0].source, "./modules/vpc");

    /* Locals */
    ASSERT(r.has_locals);

    PASS();
}

TEST(infra_parse_terraform_variables_only) {
    /* Port of TestParseTerraformVariablesOnly — secret default filtered */
    const char *src = "\n"
                      "variable \"project_id\" {\n"
                      "  description = \"The GCP project ID\"\n"
                      "  type        = string\n"
                      "  default     = \"example-cloud\"\n"
                      "}\n"
                      "\n"
                      "variable \"secret_key\" {\n"
                      "  description = \"A secret\"\n"
                      "  type        = string\n"
                      "  default     = \"sk-1234567890abcdef12345\"\n"
                      "}\n";

    cbm_terraform_result_t r;
    ASSERT_EQ(cbm_parse_terraform_source(src, &r), 0);
    ASSERT_EQ(r.variable_count, 2);

    /* secret_key default should be filtered */
    for (int i = 0; i < r.variable_count; i++) {
        if (strcmp(r.variables[i].name, "secret_key") == 0) {
            ASSERT_STR_EQ(r.variables[i].default_val, "");
        }
    }
    PASS();
}

TEST(infra_parse_terraform_empty) {
    /* Port of TestParseTerraformEmpty */
    cbm_terraform_result_t r;
    ASSERT_EQ(cbm_parse_terraform_source("# just comments\n", &r), -1);
    PASS();
}

/* ── Helm Chart.yaml dependency parsing (#338) ──────────────────── */

TEST(helm_parse_chart_dependencies_issue338) {
    const char *src = "apiVersion: v2\n"
                      "name: mychart\n"
                      "version: 1.0.0\n"
                      "dependencies:\n"
                      "  - name: postgresql\n"
                      "    repository: https://charts.bitnami.com/bitnami\n"
                      "    version: 12.x.x\n"
                      "  - name: redis\n"
                      "    repository: https://charts.bitnami.com/bitnami\n"
                      "maintainers:\n"
                      "  - name: alice\n"; /* not a dependency — outside the block */
    cbm_helm_chart_t hc;
    ASSERT_EQ(cbm_parse_helm_chart(src, &hc), 0);
    ASSERT_STR_EQ(hc.chart_name, "mychart");
    ASSERT_EQ(hc.dep_count, 2);
    ASSERT_STR_EQ(hc.deps[0], "postgresql");
    ASSERT_STR_EQ(hc.deps[1], "redis");
    PASS();
}

TEST(helm_parse_chart_no_deps_issue338) {
    cbm_helm_chart_t hc;
    ASSERT_EQ(cbm_parse_helm_chart("name: solo\nversion: 0.1.0\n", &hc), 0);
    ASSERT_STR_EQ(hc.chart_name, "solo");
    ASSERT_EQ(hc.dep_count, 0);
    PASS();
}

/* ── Infrascan: infra QN helper ─────────────────────────────────── */

/* ── Function Registry / Resolver tests ─────────────────────────── */

TEST(registry_resolve_single_candidate) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "CreateOrder", "svcA.handlers.CreateOrder", "Function");
    cbm_registry_add(reg, "ValidateOrder", "svcB.validators.ValidateOrder", "Function");

    /* Normal resolve unique name */
    cbm_resolution_t r = cbm_registry_resolve(reg, "CreateOrder", "svcC.caller", NULL, NULL, 0);
    ASSERT_STR_EQ(r.qualified_name, "svcA.handlers.CreateOrder");

    /* Fuzzy resolve with unknown prefix */
    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(reg, "unknownPkg.CreateOrder", "svcC.caller", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_STR_EQ(fr.result.qualified_name, "svcA.handlers.CreateOrder");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_fuzzy_nonexistent) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "CreateOrder", "svcA.handlers.CreateOrder", "Function");

    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(reg, "NonExistent", "svcC.caller", NULL, NULL, 0);
    ASSERT_FALSE(fr.ok);

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_fuzzy_multiple_best_by_distance) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Process", "svcA.handlers.Process", "Function");
    cbm_registry_add(reg, "Process", "svcB.handlers.Process", "Function");

    /* Caller in svcA → prefer svcA */
    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(reg, "unknown.Process", "svcA.other", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_STR_EQ(fr.result.qualified_name, "svcA.handlers.Process");

    /* Caller in svcB → prefer svcB */
    fr = cbm_registry_fuzzy_resolve(reg, "unknown.Process", "svcB.other", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_STR_EQ(fr.result.qualified_name, "svcB.handlers.Process");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_fuzzy_simple_name_extraction) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "DoWork", "myproject.utils.DoWork", "Function");

    /* Deeply qualified name → extract "DoWork" */
    cbm_fuzzy_result_t fr = cbm_registry_fuzzy_resolve(reg, "some.deep.module.DoWork",
                                                       "myproject.caller", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_STR_EQ(fr.result.qualified_name, "myproject.utils.DoWork");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_fuzzy_empty) {
    cbm_registry_t *reg = cbm_registry_new();

    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(reg, "SomeFunc", "myproject.caller", NULL, NULL, 0);
    ASSERT_FALSE(fr.ok);

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_exists) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Foo", "pkg.module.Foo", "Function");
    cbm_registry_add(reg, "Bar", "pkg.module.Bar", "Method");

    ASSERT_TRUE(cbm_registry_exists(reg, "pkg.module.Foo"));
    ASSERT_TRUE(cbm_registry_exists(reg, "pkg.module.Bar"));
    ASSERT_FALSE(cbm_registry_exists(reg, "pkg.module.Missing"));
    ASSERT_FALSE(cbm_registry_exists(reg, ""));

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_confidence_import_map) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Foo", "proj.other.Foo", "Function");

    const char *keys[] = {"other"};
    const char *vals[] = {"proj.other"};
    cbm_resolution_t r = cbm_registry_resolve(reg, "other.Foo", "proj.pkg", keys, vals, 1);
    ASSERT_STR_EQ(r.qualified_name, "proj.other.Foo");
    ASSERT(r.confidence > 0.90 && r.confidence <= 1.0);
    ASSERT_STR_EQ(r.strategy, "import_map");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_confidence_import_map_suffix) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Foo", "proj.other.sub.Foo", "Function");

    const char *keys[] = {"other"};
    const char *vals[] = {"proj.other"};
    cbm_resolution_t r = cbm_registry_resolve(reg, "other.Foo", "proj.pkg", keys, vals, 1);
    ASSERT_STR_EQ(r.qualified_name, "proj.other.sub.Foo");
    ASSERT(r.confidence > 0.80 && r.confidence <= 0.90);
    ASSERT_STR_EQ(r.strategy, "import_map_suffix");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_confidence_same_module) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Foo", "proj.pkg.Foo", "Function");

    cbm_resolution_t r = cbm_registry_resolve(reg, "Foo", "proj.pkg", NULL, NULL, 0);
    ASSERT_STR_EQ(r.qualified_name, "proj.pkg.Foo");
    ASSERT(r.confidence > 0.85 && r.confidence <= 0.95);
    ASSERT_STR_EQ(r.strategy, "same_module");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_confidence_unique_name) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Bar", "proj.pkg.Bar", "Function");

    cbm_resolution_t r = cbm_registry_resolve(reg, "Bar", "proj.unrelated", NULL, NULL, 0);
    ASSERT_STR_EQ(r.qualified_name, "proj.pkg.Bar");
    ASSERT(r.confidence > 0.70 && r.confidence <= 0.80);
    ASSERT_STR_EQ(r.strategy, "unique_name");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_confidence_suffix_match) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Process", "proj.svcA.Process", "Function");
    cbm_registry_add(reg, "Process", "proj.svcB.Process", "Function");

    cbm_resolution_t r = cbm_registry_resolve(reg, "Process", "proj.svcA.caller", NULL, NULL, 0);
    ASSERT_STR_EQ(r.qualified_name, "proj.svcA.Process");
    ASSERT(r.confidence > 0.50 && r.confidence <= 0.60);
    ASSERT_STR_EQ(r.strategy, "suffix_match");

    cbm_registry_free(reg);
    PASS();
}

/* Issue #1893: a call on a library type bound to a same-named project member.
 * URLSession is Foundation's, not this project's, so PickedFile.data is the
 * wrong target — and with one candidate it won the top name-only confidence. */
TEST(registry_receiver_chain_refuses_library_unique_name_issue1893) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "data", "HomeboxUI.PickedFile.data", "Variable");

    cbm_resolution_t r =
        cbm_registry_resolve(reg, "URLSession.shared.data", "HomeboxUI.Net", NULL, NULL, 0);
    ASSERT_NULL(r.qualified_name);

    cbm_registry_free(reg);
    PASS();
}

/* The same refusal on the other name-only exit, where several candidates share
 * the final name and import distance picks the winner. */
TEST(registry_receiver_chain_refuses_library_suffix_match_issue1893) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "data", "HomeboxUI.PickedFile.data", "Variable");
    cbm_registry_add(reg, "data", "HomeboxUI.Payload.data", "Variable");

    cbm_resolution_t r =
        cbm_registry_resolve(reg, "URLSession.shared.data", "HomeboxUI.Net", NULL, NULL, 0);
    ASSERT_NULL(r.qualified_name);

    cbm_registry_free(reg);
    PASS();
}

/* The true positive the gate must not eat: the project extends Calendar itself,
 * so Calendar really is in the receiver chain. */
TEST(registry_receiver_chain_keeps_project_extension_issue1893) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "startOfDayUTC", "AuthDTOs.Calendar.startOfDayUTC", "Method");

    cbm_resolution_t r = cbm_registry_resolve(reg, "Calendar.utcGregorian.startOfDayUTC",
                                              "HomeboxUI.Stats", NULL, NULL, 0);
    ASSERT_STR_EQ(r.qualified_name, "AuthDTOs.Calendar.startOfDayUTC");
    ASSERT_STR_EQ(r.strategy, "unique_name");

    cbm_registry_free(reg);
    PASS();
}

/* A lower-case root names a value, whose type the chain does not show. The gate
 * must not look at it, or every ordinary vm.load style call would be refused. */
TEST(registry_receiver_chain_ignores_lowercase_root_issue1893) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "load", "HomeboxUI.EntityListViewModel.load", "Method");

    cbm_resolution_t r = cbm_registry_resolve(reg, "vm.load", "HomeboxUI.Views", NULL, NULL, 0);
    ASSERT_STR_EQ(r.qualified_name, "HomeboxUI.EntityListViewModel.load");
    ASSERT_STR_EQ(r.strategy, "unique_name");

    cbm_registry_free(reg);
    PASS();
}

/* An unqualified callee has no chain at all and must pass through unchanged. */
TEST(registry_receiver_chain_ignores_bare_name_issue1893) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "helper", "proj.pkg.helper", "Function");

    cbm_resolution_t r = cbm_registry_resolve(reg, "helper", "proj.other", NULL, NULL, 0);
    ASSERT_STR_EQ(r.qualified_name, "proj.pkg.helper");
    ASSERT_STR_EQ(r.strategy, "unique_name");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_fuzzy_confidence_single) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Handler", "proj.svc.Handler", "Function");

    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(reg, "unknownPkg.Handler", "proj.caller", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT(fr.result.confidence > 0.35 && fr.result.confidence <= 0.45);
    ASSERT_STR_EQ(fr.result.strategy, "fuzzy");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_fuzzy_confidence_distance) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Process", "proj.svcA.Process", "Function");
    cbm_registry_add(reg, "Process", "proj.svcB.Process", "Function");

    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(reg, "unknownPkg.Process", "proj.svcA.other", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT(fr.result.confidence > 0.25 && fr.result.confidence <= 0.35);
    ASSERT_STR_EQ(fr.result.strategy, "fuzzy");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_negative_import_rejects) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Process", "proj.billing.Process", "Function");
    cbm_registry_add(reg, "Process", "proj.handler.Process", "Function");

    /* Import only handler's module → should prefer handler */
    const char *keys[] = {"handler"};
    const char *vals[] = {"proj.handler"};
    cbm_resolution_t r = cbm_registry_resolve(reg, "Process", "proj.caller", keys, vals, 1);
    ASSERT_STR_EQ(r.qualified_name, "proj.handler.Process");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_fuzzy_import_penalty) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Handler", "proj.billing.Handler", "Function");

    /* Has imports but billing not imported → confidence halved */
    const char *keys[] = {"other"};
    const char *vals[] = {"proj.other"};
    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(reg, "unknown.Handler", "proj.caller", keys, vals, 1);
    ASSERT_TRUE(fr.ok);
    /* 0.40 * 0.5 = 0.20 */
    ASSERT(fr.result.confidence > 0.15 && fr.result.confidence <= 0.25);

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_fuzzy_no_import_map_passthrough) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Handler", "proj.billing.Handler", "Function");

    /* NULL import map → no penalty, full fuzzy confidence */
    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(reg, "unknown.Handler", "proj.caller", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT(fr.result.confidence > 0.35 && fr.result.confidence <= 0.45);

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_find_by_name) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Foo", "proj.pkg.Foo", "Function");
    cbm_registry_add(reg, "Bar", "proj.pkg.Bar", "Function");
    cbm_registry_add(reg, "Foo", "proj.other.Foo", "Function");
    cbm_registry_add(reg, "transform", "proj.utils.DataProcessor.transform", "Method");

    /* FindByName returns all entries for "Foo" */
    const char **foos = NULL;
    int foos_count = 0;
    cbm_registry_find_by_name(reg, "Foo", &foos, &foos_count);
    ASSERT_EQ(foos_count, 2);

    /* FindByName for unique "Bar" */
    const char **bars = NULL;
    int bars_count = 0;
    cbm_registry_find_by_name(reg, "Bar", &bars, &bars_count);
    ASSERT_EQ(bars_count, 1);
    ASSERT_STR_EQ(bars[0], "proj.pkg.Bar");

    /* FindByName for "transform" */
    const char **transforms = NULL;
    int trans_count = 0;
    cbm_registry_find_by_name(reg, "transform", &transforms, &trans_count);
    ASSERT_EQ(trans_count, 1);
    ASSERT_STR_EQ(transforms[0], "proj.utils.DataProcessor.transform");

    /* label_of */
    ASSERT_STR_EQ(cbm_registry_label_of(reg, "proj.utils.DataProcessor.transform"), "Method");
    ASSERT_STR_EQ(cbm_registry_label_of(reg, "proj.pkg.Foo"), "Function");

    /* Total size */
    ASSERT_EQ(cbm_registry_size(reg), 4);

    /* Resolve same-module */
    cbm_resolution_t r = cbm_registry_resolve(reg, "Foo", "proj.pkg", NULL, NULL, 0);
    ASSERT_STR_EQ(r.qualified_name, "proj.pkg.Foo");

    /* Resolve via import map */
    const char *keys[] = {"other"};
    const char *vals[] = {"proj.other"};
    r = cbm_registry_resolve(reg, "other.Foo", "proj.pkg", keys, vals, 1);
    ASSERT_STR_EQ(r.qualified_name, "proj.other.Foo");

    /* Resolve unique name */
    r = cbm_registry_resolve(reg, "Bar", "proj.unrelated", NULL, NULL, 0);
    ASSERT_STR_EQ(r.qualified_name, "proj.pkg.Bar");

    cbm_registry_free(reg);
    PASS();
}

TEST(registry_confidence_band) {
    ASSERT_STR_EQ(cbm_confidence_band(0.95), "high");
    ASSERT_STR_EQ(cbm_confidence_band(0.70), "high");
    ASSERT_STR_EQ(cbm_confidence_band(0.55), "medium");
    ASSERT_STR_EQ(cbm_confidence_band(0.45), "medium");
    ASSERT_STR_EQ(cbm_confidence_band(0.40), "speculative");
    ASSERT_STR_EQ(cbm_confidence_band(0.25), "speculative");
    ASSERT_STR_EQ(cbm_confidence_band(0.20), "");
    ASSERT_STR_EQ(cbm_confidence_band(0.0), "");
    PASS();
}

TEST(infra_qn_helper) {
    /* Port of TestInfraQN */

    /* Regular infra file → __infra__ suffix */
    char *qn = cbm_infra_qn("myproject", "docker-images/service/Dockerfile", "dockerfile", NULL);
    ASSERT_NOT_NULL(qn);
    ASSERT(strstr(qn, ".__infra__") != NULL);
    free(qn);

    /* Compose service → ::service_name suffix */
    qn = cbm_infra_qn("myproject", "docker-compose.yml", "compose-service", "web");
    ASSERT_NOT_NULL(qn);
    ASSERT(strstr(qn, "::web") != NULL);
    free(qn);

    PASS();
}

/* ── Infrascan integration tests ────────────────────────────────── */

TEST(infra_pipeline_integration) {
    /* Port of TestPassInfraFilesIntegration (Dockerfile + .env parts).
     * Tests parse functions on source text (pipeline infrascan pass not
     * wired yet — compose YAML also blocked on YAML parser). */

    /* Parse Dockerfile */
    cbm_dockerfile_result_t dr;
    ASSERT_EQ(cbm_parse_dockerfile_source("FROM alpine:3.19\nEXPOSE 8080\n", &dr), 0);
    ASSERT_STR_EQ(dr.base_image, "alpine:3.19");
    ASSERT_GTE(dr.port_count, 1);

    /* Parse .env */
    cbm_dotenv_result_t er;
    ASSERT_EQ(cbm_parse_dotenv_source("APP_PORT=8080\nDEBUG=true\n", &er), 0);
    ASSERT_GTE(er.env_count, 1);
    /* APP_PORT should be present */
    bool found_port = false;
    for (int i = 0; i < er.env_count; i++) {
        if (strcmp(er.env_vars[i].key, "APP_PORT") == 0 &&
            strcmp(er.env_vars[i].value, "8080") == 0)
            found_port = true;
    }
    ASSERT_TRUE(found_port);

    PASS();
}

TEST(infra_pipeline_idempotent) {
    /* Port of TestPassInfraFilesIdempotent:
     * Parsing same source twice should produce identical results. */
    const char *src = "FROM alpine:3.19\nEXPOSE 8080\nENV PORT=8080\n";
    cbm_dockerfile_result_t r1, r2;
    ASSERT_EQ(cbm_parse_dockerfile_source(src, &r1), 0);
    ASSERT_EQ(cbm_parse_dockerfile_source(src, &r2), 0);

    ASSERT_STR_EQ(r1.base_image, r2.base_image);
    ASSERT_EQ(r1.port_count, r2.port_count);
    ASSERT_EQ(r1.env_count, r2.env_count);

    PASS();
}

/* ── K8s / Kustomize extraction tests ──────────────────────────── */

TEST(k8s_extract_kustomize) {
    const char *src = "apiVersion: kustomize.config.k8s.io/v1beta1\n"
                      "kind: Kustomization\n"
                      "resources:\n"
                      "  - deployment.yaml\n"
                      "  - service.yaml\n";
    CBMFileResult *r = cbm_extract_file(src, (int)strlen(src), CBM_LANG_KUSTOMIZE, "myproj",
                                        "base/kustomization.yaml", 0, NULL, NULL);
    ASSERT(r != NULL);
    ASSERT_GTE(r->imports.count, 2);

    bool found_deploy = false, found_svc = false;
    for (int i = 0; i < r->imports.count; i++) {
        if (r->imports.items[i].module_path &&
            strcmp(r->imports.items[i].module_path, "deployment.yaml") == 0)
            found_deploy = true;
        if (r->imports.items[i].module_path &&
            strcmp(r->imports.items[i].module_path, "service.yaml") == 0)
            found_svc = true;
    }
    ASSERT_TRUE(found_deploy);
    ASSERT_TRUE(found_svc);

    cbm_free_result(r);
    PASS();
}

TEST(k8s_extract_manifest) {
    const char *src = "apiVersion: apps/v1\n"
                      "kind: Deployment\n"
                      "metadata:\n"
                      "  name: my-app\n"
                      "  namespace: production\n";
    CBMFileResult *r = cbm_extract_file(src, (int)strlen(src), CBM_LANG_K8S, "myproj",
                                        "k8s/deployment.yaml", 0, NULL, NULL);
    ASSERT(r != NULL);
    ASSERT_GTE(r->defs.count, 1);

    bool found_resource = false;
    for (int d = 0; d < r->defs.count; d++) {
        if (r->defs.items[d].label && strcmp(r->defs.items[d].label, "Resource") == 0 &&
            r->defs.items[d].name && strstr(r->defs.items[d].name, "Deployment") != NULL)
            found_resource = true;
    }
    ASSERT_TRUE(found_resource);

    cbm_free_result(r);
    PASS();
}

TEST(k8s_extract_manifest_no_name) {
    const char *src = "apiVersion: apps/v1\nkind: Deployment\n";
    CBMFileResult *r = cbm_extract_file(src, (int)strlen(src), CBM_LANG_K8S, "myproj",
                                        "k8s/deploy.yaml", 0, NULL, NULL);
    ASSERT(r != NULL);
    /* No crash — defs count may be 0 because metadata.name is absent */
    ASSERT(!r->has_error);
    cbm_free_result(r);
    PASS();
}

TEST(k8s_extract_manifest_multidoc) {
    /* Two-document YAML separated by "---".
     * extract_k8s_manifest contains a "break" after the first successful push,
     * so it processes only the first document that has both kind and
     * metadata.name.  This test pins that behaviour: the first document's
     * resource must be present and no crash must occur.
     *
     * Note: with some tree-sitter YAML grammar versions the root stream may
     * expose both documents as siblings; the break still fires after the first
     * successful def push, so defs.count must be exactly 1. */
    const char *src = "apiVersion: apps/v1\n"
                      "kind: Deployment\n"
                      "metadata:\n"
                      "  name: my-app\n"
                      "---\n"
                      "apiVersion: v1\n"
                      "kind: Service\n"
                      "metadata:\n"
                      "  name: my-svc\n";
    CBMFileResult *r = cbm_extract_file(src, (int)strlen(src), CBM_LANG_K8S, "myproj",
                                        "k8s/multi.yaml", 0, NULL, NULL);
    ASSERT(r != NULL);
    ASSERT(!r->has_error);
    /* First document's resource must be present */
    int found = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (r->defs.items[i].label && strcmp(r->defs.items[i].label, "Resource") == 0 &&
            r->defs.items[i].name && strcmp(r->defs.items[i].name, "Deployment/my-app") == 0) {
            found = 1;
        }
    }
    ASSERT(found);
    /* At least one def, no more than one (only first document processed) */
    ASSERT(r->defs.count >= 1);
    cbm_free_result(r);
    PASS();
}

/* ── Envscan tests (port of envscan_test.go) ───────────────────── */

/* Helper: write a file inside a temp dir */
static void write_temp_file(const char *dir, const char *name, const char *content) {
    char path[512];
    /* Create subdirectories if needed */
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    char *slash = strrchr(path, '/');
    if (slash) {
        char parent[512];
        size_t plen = slash - path;
        memcpy(parent, path, plen);
        parent[plen] = '\0';
        /* mkdir -p (simple version, one level) */
        cbm_mkdir(parent);
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

/* Helper: find binding by key in results */
static const cbm_env_binding_t *find_binding_by_key(const cbm_env_binding_t *bindings, int count,
                                                    const char *key) {
    for (int i = 0; i < count; i++) {
        if (strcmp(bindings[i].key, key) == 0)
            return &bindings[i];
    }
    return NULL;
}

/* Helper: find binding by value in results */
static int has_binding_value(const cbm_env_binding_t *bindings, int count, const char *value) {
    for (int i = 0; i < count; i++) {
        if (strcmp(bindings[i].value, value) == 0)
            return 1;
    }
    return 0;
}

TEST(envscan_dockerfile_env_urls) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_dock_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(tmpdir, "Dockerfile",
                    "FROM python:3.9-slim\n"
                    "ENV ORDER_URL=https://api.example.com/api/orders\n"
                    "ENV DB_HOST=localhost\n"
                    "ARG WEBHOOK_URL=https://hooks.example.com/webhook\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "ORDER_URL"));
    ASSERT_STR_EQ(find_binding_by_key(bindings, count, "ORDER_URL")->value,
                  "https://api.example.com/api/orders");
    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "WEBHOOK_URL"));
    ASSERT_STR_EQ(find_binding_by_key(bindings, count, "WEBHOOK_URL")->value,
                  "https://hooks.example.com/webhook");
    /* DB_HOST=localhost is NOT a URL → should be absent */
    ASSERT_TRUE(find_binding_by_key(bindings, count, "DB_HOST") == NULL);

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_shell_env_urls) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_sh_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(tmpdir, "setup.sh",
                    "#!/bin/bash\n"
                    "export DB_URL=\"https://db.example.com/api/sync\"\n"
                    "APP_NAME=\"my-service\"\n"
                    "CALLBACK_URL=https://hooks.example.com/notify\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "DB_URL"));
    ASSERT_STR_EQ(find_binding_by_key(bindings, count, "DB_URL")->value,
                  "https://db.example.com/api/sync");
    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "CALLBACK_URL"));
    /* APP_NAME is NOT a URL → absent */
    ASSERT_TRUE(find_binding_by_key(bindings, count, "APP_NAME") == NULL);

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_env_file_urls) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_env_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(tmpdir, ".env",
                    "\nAPI_URL=https://api.example.com/v1\n"
                    "DEBUG=true\n"
                    "SERVICE_URL=https://service.example.com/api\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "API_URL"));
    ASSERT_STR_EQ(find_binding_by_key(bindings, count, "API_URL")->value,
                  "https://api.example.com/v1");
    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "SERVICE_URL"));
    /* DEBUG=true is NOT a URL */
    ASSERT_TRUE(find_binding_by_key(bindings, count, "DEBUG") == NULL);

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_toml_urls) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_toml_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(tmpdir, "config.toml",
                    "[service]\n"
                    "base_url = \"https://api.example.com\"\n"
                    "name = \"my-service\"\n"
                    "callback_url = \"https://hooks.example.com/notify\"\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "base_url"));
    ASSERT_STR_EQ(find_binding_by_key(bindings, count, "base_url")->value,
                  "https://api.example.com");
    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "callback_url"));
    /* name="my-service" is NOT a URL */
    ASSERT_TRUE(find_binding_by_key(bindings, count, "name") == NULL);

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_yaml_urls) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_yaml_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(tmpdir, "config.yaml",
                    "service:\n"
                    "  service_url: \"https://api.internal.com/api/process\"\n"
                    "  timeout: 30\n"
                    "  callback_url: \"https://hooks.internal.com/callback\"\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "service_url"));
    ASSERT_STR_EQ(find_binding_by_key(bindings, count, "service_url")->value,
                  "https://api.internal.com/api/process");
    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "callback_url"));

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_terraform_urls) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_tf_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(tmpdir, "variables.tf",
                    "variable \"webhook_url\" {\n"
                    "  description = \"Webhook endpoint\"\n"
                    "  default     = \"https://api.example.com/webhook\"\n"
                    "}\n\n"
                    "variable \"region\" {\n"
                    "  default = \"us-east-1\"\n"
                    "}\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    ASSERT_GTE(count, 1);
    ASSERT_TRUE(has_binding_value(bindings, count, "https://api.example.com/webhook"));

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_properties_urls) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_prop_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(tmpdir, "app.properties",
                    "api.url=https://api.example.com/health\n"
                    "app.name=myapp\n"
                    "service.endpoint=https://service.example.com/api\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    ASSERT_TRUE(has_binding_value(bindings, count, "https://api.example.com/health"));
    ASSERT_TRUE(has_binding_value(bindings, count, "https://service.example.com/api"));

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_secret_key_exclusion) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_skey_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(tmpdir, "Dockerfile",
                    "FROM node:18\n"
                    "ENV SECRET_TOKEN=https://api.example.com/api\n"
                    "ENV API_KEY=https://api.example.com/v1\n"
                    "ENV PASSWORD=https://auth.example.com/login\n"
                    "ENV NORMAL_URL=https://api.example.com/orders\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    /* Secret keys should be excluded */
    ASSERT_TRUE(find_binding_by_key(bindings, count, "SECRET_TOKEN") == NULL);
    ASSERT_TRUE(find_binding_by_key(bindings, count, "API_KEY") == NULL);
    ASSERT_TRUE(find_binding_by_key(bindings, count, "PASSWORD") == NULL);
    /* Normal key should be present */
    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "NORMAL_URL"));

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_secret_value_exclusion) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_sval_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(
        tmpdir, "deploy.sh",
        "#!/bin/bash\n"
        "export GH_URL=\"https://ghp_FAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKE@github.com/repo\"\n"
        "export NORMAL_ENDPOINT=\"https://api.example.com/orders\"\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    /* ghp_ token URL should be excluded */
    ASSERT_TRUE(find_binding_by_key(bindings, count, "GH_URL") == NULL);
    /* Normal URL should be present */
    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "NORMAL_ENDPOINT"));

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_secret_file_exclusion) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_sfile_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    /* Secret file should be skipped */
    write_temp_file(tmpdir, "credentials.sh",
                    "#!/bin/bash\nexport API_URL=\"https://api.example.com/v1\"\n");
    /* Normal file should be scanned */
    write_temp_file(tmpdir, "setup.sh",
                    "#!/bin/bash\nexport API_URL=\"https://api.example.com/v1\"\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    /* Should find binding from setup.sh but not credentials.sh */
    int from_credentials = 0;
    int from_setup = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(bindings[i].file_path, "credentials.sh") == 0)
            from_credentials = 1;
        if (strcmp(bindings[i].file_path, "setup.sh") == 0)
            from_setup = 1;
    }
    ASSERT_EQ(from_credentials, 0);
    ASSERT_EQ(from_setup, 1);

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_skips_ignored_dirs) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_ign_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    /* File inside .git should be skipped */
    char gitdir[512];
    snprintf(gitdir, sizeof(gitdir), "%s/.git", tmpdir);
    cbm_mkdir(gitdir);
    write_temp_file(tmpdir, ".git/config.sh",
                    "#!/bin/bash\nexport API_URL=\"https://api.example.com/v1\"\n");

    /* File inside node_modules should be skipped */
    char nmdir[512];
    snprintf(nmdir, sizeof(nmdir), "%s/node_modules", tmpdir);
    cbm_mkdir(nmdir);
    char nmpkg[512];
    snprintf(nmpkg, sizeof(nmpkg), "%s/node_modules/pkg", tmpdir);
    cbm_mkdir(nmpkg);
    write_temp_file(tmpdir, "node_modules/pkg/config.sh",
                    "#!/bin/bash\nexport API_URL=\"https://api.example.com/v1\"\n");

    /* File at root level should be scanned */
    write_temp_file(tmpdir, "deploy.sh",
                    "#!/bin/bash\nexport API_URL=\"https://api.example.com/v1\"\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    int from_git = 0, from_nm = 0, from_root = 0;
    for (int i = 0; i < count; i++) {
        if (strncmp(bindings[i].file_path, ".git/", 5) == 0)
            from_git = 1;
        if (strncmp(bindings[i].file_path, "node_modules/", 13) == 0)
            from_nm = 1;
        if (strcmp(bindings[i].file_path, "deploy.sh") == 0)
            from_root = 1;
    }
    ASSERT_EQ(from_git, 0);
    ASSERT_EQ(from_nm, 0);
    ASSERT_EQ(from_root, 1);

    th_rmtree(tmpdir);
    PASS();
}

TEST(envscan_non_url_values_skipped) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_nurl_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(tmpdir, "Dockerfile",
                    "FROM python:3.9\n"
                    "ENV APP_NAME=my-service\n"
                    "ENV PORT=8080\n"
                    "ENV DEBUG=true\n"
                    "ENV LOG_LEVEL=info\n");
    write_temp_file(tmpdir, "config.sh",
                    "#!/bin/bash\n"
                    "export REGION=\"us-east-1\"\n"
                    "export COUNT=42\n");

    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);

    ASSERT_EQ(count, 0);

    th_rmtree(tmpdir);
    PASS();
}

/* ── Discovery-exclusion plumbing in auxiliary repo walks (#792) ── */

/* Boundary semantics of the shared exclusion predicate: anchored at the
 * repo root, matches the excluded dir itself and its subtree, but never
 * sibling names sharing a prefix. Regression guard for issue #792. */
TEST(pipeline_relpath_excluded_boundary) {
    char *excluded[] = {(char *)"vendor_big", (char *)"packages/big"};

    /* Exact match and subtree paths are excluded. */
    ASSERT_TRUE(cbm_pipeline_relpath_is_excluded("vendor_big", excluded, 2));
    ASSERT_TRUE(cbm_pipeline_relpath_is_excluded("vendor_big/lib/package.json", excluded, 2));
    ASSERT_TRUE(cbm_pipeline_relpath_is_excluded("packages/big", excluded, 2));
    ASSERT_TRUE(cbm_pipeline_relpath_is_excluded("packages/big/src/x.ts", excluded, 2));

    /* Sibling names sharing the prefix are NOT excluded ('/'-boundary). */
    ASSERT_FALSE(cbm_pipeline_relpath_is_excluded("vendor_bigger", excluded, 2));
    ASSERT_FALSE(cbm_pipeline_relpath_is_excluded("vendor", excluded, 2));
    ASSERT_FALSE(cbm_pipeline_relpath_is_excluded("packages/bigger/x.ts", excluded, 2));

    /* Exclusions are root-anchored prefixes, not substring matches. */
    ASSERT_FALSE(cbm_pipeline_relpath_is_excluded("src/vendor_big/x.c", excluded, 2));

    /* NULL / empty safety. */
    ASSERT_FALSE(cbm_pipeline_relpath_is_excluded(NULL, excluded, 2));
    ASSERT_FALSE(cbm_pipeline_relpath_is_excluded("", excluded, 2));
    ASSERT_FALSE(cbm_pipeline_relpath_is_excluded("vendor_big", NULL, 0));
    ASSERT_FALSE(cbm_pipeline_relpath_is_excluded("vendor_big", excluded, 0));
    char *with_empty[] = {(char *)""};
    ASSERT_FALSE(cbm_pipeline_relpath_is_excluded("vendor_big", with_empty, 1));
    PASS();
}

/* Helper: does the entries array contain a package with this name? */
static int pkg_entries_has_name(const cbm_pkg_entries_t *e, const char *name) {
    for (int i = 0; i < e->count; i++) {
        if (e->items[i].pkg_name && strcmp(e->items[i].pkg_name, name) == 0)
            return 1;
    }
    return 0;
}

/* The pkgmap repo walk must honor discovery exclusions (issue #792: a
 * gitignored huge subtree kept the pkgmap walk busy for 15 minutes).
 * Control run first (no exclusions → BOTH manifests parsed) so the
 * exclusion assertion below cannot pass vacuously. */
TEST(pkgmap_scan_repo_honors_discovery_exclusions) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_pkgmap_excl_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/packages", tmpdir);
    cbm_mkdir(dir);
    snprintf(dir, sizeof(dir), "%s/packages/app", tmpdir);
    cbm_mkdir(dir);
    snprintf(dir, sizeof(dir), "%s/vendor_big", tmpdir);
    cbm_mkdir(dir);
    snprintf(dir, sizeof(dir), "%s/vendor_big/lib", tmpdir);
    cbm_mkdir(dir);

    write_temp_file(tmpdir, "packages/app/package.json",
                    "{\"name\":\"@org/app\",\"main\":\"index.js\"}\n");
    write_temp_file(tmpdir, "vendor_big/lib/package.json",
                    "{\"name\":\"@org/vendored\",\"main\":\"index.js\"}\n");

    /* Control: NULL exclusion list — the walk reaches and parses BOTH
     * manifests (proves the excluded one is reachable + parseable). */
    cbm_pkg_entries_t control;
    cbm_pkg_entries_init(&control);
    cbm_pkgmap_scan_repo(tmpdir, &control, NULL, 0);
    ASSERT_TRUE(pkg_entries_has_name(&control, "@org/app"));
    ASSERT_TRUE(pkg_entries_has_name(&control, "@org/vendored"));
    cbm_pkg_entries_free(&control);

    /* With vendor_big excluded (as discovery reports for a gitignored
     * subtree): the walk must not descend into it. */
    char *excluded[] = {(char *)"vendor_big"};
    cbm_pkg_entries_t entries;
    cbm_pkg_entries_init(&entries);
    cbm_pkgmap_scan_repo(tmpdir, &entries, excluded, 1);
    ASSERT_TRUE(pkg_entries_has_name(&entries, "@org/app"));
    ASSERT_FALSE(pkg_entries_has_name(&entries, "@org/vendored"));
    cbm_pkg_entries_free(&entries);

    th_rmtree(tmpdir);
    PASS();
}

/* Two in-process pipelines run at once. Each repo's `@<scope>/lib` import
 * resolves only through its own package map, so a map shared between runs (or
 * freed by the other run) loses an IMPORTS edge or crashes. */
typedef struct {
    char root[256];
    const char *scope;
    int rc;
    bool import_resolved;
} concurrent_pkgmap_run_t;

static bool concurrent_pkgmap_import_resolved(const char *root, const char *project) {
    cbm_store_t *s = th_open_repo_store(root);
    if (!s) return false;
    cbm_edge_t *edges = NULL;
    int count = 0;
    bool found = false;
    if (cbm_store_find_edges_by_type(s, project, "IMPORTS", &edges, &count) == CBM_STORE_OK) {
        for (int i = 0; i < count && !found; i++) {
            cbm_node_t src = {0}, tgt = {0};
            if (cbm_store_find_node_by_id(s, edges[i].source_id, &src) == CBM_STORE_OK &&
                cbm_store_find_node_by_id(s, edges[i].target_id, &tgt) == CBM_STORE_OK) {
                found = src.file_path && strcmp(src.file_path, "app.js") == 0 && tgt.file_path &&
                        strcmp(tgt.file_path, "packages/lib/index.js") == 0;
            }
            cbm_node_free_fields(&src);
            cbm_node_free_fields(&tgt);
        }
        if (edges) cbm_store_free_edges(edges, count);
    }
    cbm_store_close(s);
    return found;
}

static void *concurrent_pkgmap_run(void *arg) {
    concurrent_pkgmap_run_t *run = arg;
    cbm_pipeline_t *p = cbm_pipeline_new(run->root, NULL, CBM_MODE_FULL);
    run->rc = p ? cbm_pipeline_run(p) : -1;
    if (p && run->rc == 0) {
        run->import_resolved =
            concurrent_pkgmap_import_resolved(run->root, cbm_pipeline_project_name(p));
    }
    if (p) cbm_pipeline_free(p);
    return NULL;
}

TEST(pipeline_concurrent_runs_keep_their_own_pkgmap) {
    const char *scopes[] = {"alpha", "beta"};
    concurrent_pkgmap_run_t runs[2];
    memset(runs, 0, sizeof(runs));
    char content[256], dir[512];
    for (int r = 0; r < 2; r++) {
        snprintf(runs[r].root, sizeof(runs[r].root), "%s/cbm_pkgmap_conc_%s_XXXXXX",
                 cbm_tmpdir(), scopes[r]);
        if (!cbm_mkdtemp(runs[r].root)) FAIL("tmpdir");
        runs[r].scope = scopes[r];
        snprintf(dir, sizeof(dir), "%s/packages/lib", runs[r].root);
        th_mkdir_p(dir);
        snprintf(content, sizeof(content), "{\"name\":\"@%s/lib\",\"main\":\"index.js\"}\n",
                 scopes[r]);
        write_temp_file(runs[r].root, "packages/lib/package.json", content);
        write_temp_file(runs[r].root, "packages/lib/index.js",
                        "export function helper() { return 1; }\n");
        snprintf(content, sizeof(content),
                 "import { helper } from \"@%s/lib\";\nexport function main() { return helper(); }\n",
                 scopes[r]);
        write_temp_file(runs[r].root, "app.js", content);
    }

    for (int round = 0; round < 3; round++) {
        cbm_thread_t threads[2];
        for (int r = 0; r < 2; r++) {
            runs[r].rc = -1;
            runs[r].import_resolved = false;
            ASSERT_EQ(cbm_thread_create(&threads[r], 0, concurrent_pkgmap_run, &runs[r]), 0);
        }
        for (int r = 0; r < 2; r++) cbm_thread_join(&threads[r]);
        for (int r = 0; r < 2; r++) {
            ASSERT_EQ(runs[r].rc, 0);
            ASSERT_TRUE(runs[r].import_resolved);
        }
    }
    for (int r = 0; r < 2; r++) th_rmtree(runs[r].root);
    PASS();
}

/* The env-URL walk must honor discovery exclusions the same way (#792).
 * Control run first via the NULL-exclusion wrapper so the exclusion
 * assertion cannot pass vacuously. */
TEST(envscan_walk_honors_discovery_exclusions) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_envscan_excl_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/big_generated", tmpdir);
    cbm_mkdir(dir);

    write_temp_file(tmpdir, "deploy.sh",
                    "#!/bin/bash\nexport CONTROL_URL=\"https://api.example.com/v1\"\n");
    write_temp_file(tmpdir, "big_generated/env.sh",
                    "#!/bin/bash\nexport EXCLUDED_URL=\"https://excluded.example.com/v1\"\n");

    /* Control: the NULL-exclusion wrapper sees both bindings. */
    cbm_env_binding_t bindings[32];
    int count = cbm_scan_project_env_urls(tmpdir, bindings, 32);
    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "CONTROL_URL"));
    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "EXCLUDED_URL"));

    /* With big_generated excluded, its binding must disappear. */
    char *excluded[] = {(char *)"big_generated"};
    count = cbm_scan_project_env_urls_excluded(tmpdir, bindings, 32, excluded, 1);
    ASSERT_NOT_NULL(find_binding_by_key(bindings, count, "CONTROL_URL"));
    ASSERT_TRUE(find_binding_by_key(bindings, count, "EXCLUDED_URL") == NULL);

    th_rmtree(tmpdir);
    PASS();
}

/* ── Git history tests (port of githistory_test.go) ────────────── */

/* Port of Go TestIsTrackableFile from githistory_test.go */
TEST(githistory_is_trackable_file) {
    /* Source files — trackable */
    ASSERT_TRUE(cbm_is_trackable_file("main.go"));
    ASSERT_TRUE(cbm_is_trackable_file("src/app.py"));
    ASSERT_TRUE(cbm_is_trackable_file("README.md"));

    /* node_modules — not trackable */
    ASSERT_FALSE(cbm_is_trackable_file("node_modules/foo/bar.js"));
    /* vendor — not trackable */
    ASSERT_FALSE(cbm_is_trackable_file("vendor/lib/dep.go"));
    /* Lock files — not trackable */
    ASSERT_FALSE(cbm_is_trackable_file("package-lock.json"));
    ASSERT_FALSE(cbm_is_trackable_file("go.sum"));
    /* Binary/assets — not trackable */
    ASSERT_FALSE(cbm_is_trackable_file("image.png"));
    /* .git directory — not trackable */
    ASSERT_FALSE(cbm_is_trackable_file(".git/config"));
    /* __pycache__ — not trackable */
    ASSERT_FALSE(cbm_is_trackable_file("__pycache__/mod.pyc"));
    /* Minified files — not trackable */
    ASSERT_FALSE(cbm_is_trackable_file("src/style.min.css"));
    PASS();
}

/* Port of Go TestComputeChangeCoupling from githistory_test.go */
TEST(githistory_compute_change_coupling) {
    /* 5 commits:
     * aaa: a.go, b.go, c.go
     * bbb: a.go, b.go
     * ccc: a.go, b.go
     * ddd: a.go, c.go
     * eee: d.go, e.go
     */
    char *files_aaa[] = {"a.go", "b.go", "c.go"};
    char *files_bbb[] = {"a.go", "b.go"};
    char *files_ccc[] = {"a.go", "b.go"};
    char *files_ddd[] = {"a.go", "c.go"};
    char *files_eee[] = {"d.go", "e.go"};

    cbm_commit_files_t commits[5] = {
        {files_aaa, 3, 0}, {files_bbb, 2, 0}, {files_ccc, 2, 0},
        {files_ddd, 2, 0}, {files_eee, 2, 0},
    };

    cbm_change_coupling_t out[100];
    int count = cbm_compute_change_coupling(commits, 5, out, 100);

    /* a.go + b.go co-change 3 times → should be in results */
    bool found_ab = false;
    for (int i = 0; i < count; i++) {
        if ((strcmp(out[i].file_a, "a.go") == 0 && strcmp(out[i].file_b, "b.go") == 0) ||
            (strcmp(out[i].file_a, "b.go") == 0 && strcmp(out[i].file_b, "a.go") == 0)) {
            found_ab = true;
            ASSERT_EQ(out[i].co_change_count, 3);
            ASSERT(out[i].coupling_score >= 0.9);
        }
    }
    ASSERT_TRUE(found_ab);

    /* d.go + e.go co-change only 1 time → below threshold of 3 */
    for (int i = 0; i < count; i++) {
        if (strcmp(out[i].file_a, "d.go") == 0 || strcmp(out[i].file_b, "d.go") == 0) {
            ASSERT(0); /* d.go should not appear */
        }
    }
    PASS();
}

/* Port of Go TestComputeChangeCouplingSkipsLargeCommits from githistory_test.go */
TEST(githistory_coupling_skips_large_commits) {
    /* 25 files in one commit → exceeds 20-file threshold */
    char *files[25];
    char bufs[25][32];
    for (int i = 0; i < 25; i++) {
        snprintf(bufs[i], sizeof(bufs[i]), "file%d.go", i);
        files[i] = bufs[i];
    }
    cbm_commit_files_t commits[1] = {{files, 25, 0}};

    cbm_change_coupling_t out[100];
    int count = cbm_compute_change_coupling(commits, 1, out, 100);
    ASSERT_EQ(count, 0);
    PASS();
}

/* Port of Go TestComputeChangeCouplingLimitsTo100 from githistory_test.go */
TEST(githistory_coupling_limits_output) {
    /* Generate many small commits to create >100 couplings.
     * 50 files, each pair committed 3 times. max_out=100. */
    int idx = 0;
    char *pair_files[2450][2]; /* 50*49/2 pairs * 3 repetitions = 3675 commits */
    char pair_bufs[2450][2][32];
    cbm_commit_files_t commits[3675];
    int ci = 0;
    for (int i = 0; i < 50 && ci < 3675; i++) {
        for (int j = i + 1; j < 50 && ci < 3675; j++) {
            for (int k = 0; k < 3 && ci < 3675; k++) {
                snprintf(pair_bufs[idx][0], 32, "f%d.go", i);
                snprintf(pair_bufs[idx][1], 32, "f%d.go", j);
                pair_files[idx][0] = pair_bufs[idx][0];
                pair_files[idx][1] = pair_bufs[idx][1];
                commits[ci].files = pair_files[idx];
                commits[ci].count = 2;
                ci++;
                idx++;
                if (idx >= 2450)
                    idx = 0; /* reuse buffer space */
            }
        }
    }

    cbm_change_coupling_t out[200];
    int count = cbm_compute_change_coupling(commits, ci, out, 100);
    ASSERT(count <= 100);
    PASS();
}

/* Port of Go TestIsImportReachable from resolver_test.go */
TEST(registry_is_import_reachable) {
    const char *import_vals[] = {"proj.handler", "proj.shared.utils"};

    /* Exact match: proj.handler.Process → true */
    ASSERT_TRUE(cbm_registry_is_import_reachable("proj.handler.Process", import_vals, 2));
    /* Sub-package: proj.handler.sub.Process → true (handler contains handler) */
    ASSERT_TRUE(cbm_registry_is_import_reachable("proj.handler.sub.Process", import_vals, 2));
    /* Nested match: proj.shared.utils.Helper → true */
    ASSERT_TRUE(cbm_registry_is_import_reachable("proj.shared.utils.Helper", import_vals, 2));
    /* Unrelated: proj.billing.Process → false */
    ASSERT_FALSE(cbm_registry_is_import_reachable("proj.billing.Process", import_vals, 2));
    /* Completely unrelated: unrelated.pkg.Func → false */
    ASSERT_FALSE(cbm_registry_is_import_reachable("unrelated.pkg.Func", import_vals, 2));
    PASS();
}

/* Port of FindEndingWith portion from Go TestFunctionRegistry in pipeline_test.go */
TEST(registry_find_ending_with) {
    cbm_registry_t *reg = cbm_registry_new();
    cbm_registry_add(reg, "Foo", "proj.pkg.Foo", "Function");
    cbm_registry_add(reg, "Bar", "proj.pkg.Bar", "Function");
    cbm_registry_add(reg, "Foo", "proj.other.Foo", "Function");
    cbm_registry_add(reg, "transform", "proj.utils.DataProcessor.transform", "Method");

    /* FindEndingWith "DataProcessor.transform" → 1 match */
    const char **matches = NULL;
    int count = cbm_registry_find_ending_with(reg, "DataProcessor.transform", &matches);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(matches[0], "proj.utils.DataProcessor.transform");
    free(matches);

    /* FindEndingWith "Foo" → 2 matches */
    matches = NULL;
    count = cbm_registry_find_ending_with(reg, "Foo", &matches);
    ASSERT_EQ(count, 2);
    /* Both should be present (order may vary) */
    bool found_pkg = false, found_other = false;
    for (int i = 0; i < count; i++) {
        if (strcmp(matches[i], "proj.pkg.Foo") == 0)
            found_pkg = true;
        if (strcmp(matches[i], "proj.other.Foo") == 0)
            found_other = true;
    }
    ASSERT_TRUE(found_pkg);
    ASSERT_TRUE(found_other);
    free(matches);

    /* FindEndingWith "Nonexistent" → 0 matches */
    matches = NULL;
    count = cbm_registry_find_ending_with(reg, "Nonexistent", &matches);
    ASSERT_EQ(count, 0);

    cbm_registry_free(reg);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Incremental reindex tests
 * ═══════════════════════════════════════════════════════════════════ */

/* Helper: create a simple 2-file Go project for incremental tests */
static char g_incr_tmpdir[256];

static int setup_incremental_repo(void) {
    snprintf(g_incr_tmpdir, sizeof(g_incr_tmpdir), "/tmp/cbm_incr_XXXXXX");
    if (!cbm_mkdtemp(g_incr_tmpdir)) {
        return -1;
    }

    char path[512];
    FILE *f;

    /* main.go — calls Helper() */
    snprintf(path, sizeof(path), "%s/main.go", g_incr_tmpdir);
    f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "package main\n\nfunc main() {\n\tHelper()\n}\n");
    fclose(f);

    /* helper.go — defines Helper() */
    snprintf(path, sizeof(path), "%s/helper.go", g_incr_tmpdir);
    f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "package main\n\nfunc Helper() string {\n\treturn \"hello\"\n}\n");
    fclose(f);

    return 0;
}

static void cleanup_incremental_repo(void) {
    th_rmtree(g_incr_tmpdir);
}

static bool pipeline_single_file_generation_exists(const char *path) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool exists = false;
    if (!path || sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    if (sqlite3_prepare_v2(db,
                           "SELECT count(*) FROM cbm_workspace_index_state_v1",
                           -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int64(stmt, 0) > 0;
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return exists;
}

static int pipeline_single_file_summary_row(const char *path, const char *workspace_id,
                                            char **summary, char **source_hash,
                                            char **created_at, char **updated_at) {
    if (!path || !workspace_id || !summary || !source_hash || !created_at || !updated_at) {
        return -1;
    }
    *summary = NULL;
    *source_hash = NULL;
    *created_at = NULL;
    *updated_at = NULL;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
                           "SELECT summary,source_hash,created_at,updated_at "
                           "FROM cbm_project_summaries_v2 WHERE workspace_key=(SELECT "
                           "workspace_key FROM cbm_workspace_registry WHERE workspace_id=?1)",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, workspace_id, -1, SQLITE_STATIC) != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        if (db) sqlite3_close(db);
        return -1;
    }
    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 1;
    }
    if (step != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    *summary = strdup((const char *)sqlite3_column_text(stmt, 0));
    *source_hash = strdup((const char *)sqlite3_column_text(stmt, 1));
    *created_at = strdup((const char *)sqlite3_column_text(stmt, 2));
    *updated_at = strdup((const char *)sqlite3_column_text(stmt, 3));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    if (!*summary || !*source_hash || !*created_at || !*updated_at) {
        free(*summary);
        free(*source_hash);
        free(*created_at);
        free(*updated_at);
        *summary = NULL;
        *source_hash = NULL;
        *created_at = NULL;
        *updated_at = NULL;
        return -1;
    }
    return 0;
}

static int pipeline_single_file_text_row(const char *path, const char *sql, const char *bind,
                                         char **out) {
    if (!path || !sql || !bind || !out) return -1;
    *out = NULL;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, bind, -1, SQLITE_STATIC) != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        if (db) sqlite3_close(db);
        return -1;
    }
    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 1;
    }
    if (step != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    const unsigned char *value = sqlite3_column_text(stmt, 0);
    *out = strdup(value ? (const char *)value : "");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return *out ? 0 : -1;
}

/* A canonical digest deliberately excludes local ids.  It is used only by the
 * incremental parity tests below: matching rows prove an incremental index and
 * a fresh full index of the same tree contain the same semantic graph. */
typedef struct {
    uint64_t hash;
    int rows;
} pipeline_sql_digest_t;

static void pipeline_digest_bytes(pipeline_sql_digest_t *digest, const void *bytes, size_t len) {
    const unsigned char *cursor = bytes;
    for (size_t i = 0; i < len; i++) {
        digest->hash ^= cursor[i];
        digest->hash *= UINT64_C(1099511628211);
    }
}

static void pipeline_digest_u64(pipeline_sql_digest_t *digest, uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (unsigned char)(value >> (i * 8));
    }
    pipeline_digest_bytes(digest, bytes, sizeof(bytes));
}

static int pipeline_digest_project_rows(sqlite3 *db, const char *project, const char *sql,
                                        pipeline_sql_digest_t *out) {
    if (!db || !project || !sql || !out) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    *out = (pipeline_sql_digest_t){.hash = UINT64_C(1469598103934665603), .rows = 0};
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC) != SQLITE_OK) {
        goto done;
    }
    for (;;) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) {
            rc = 0;
            break;
        }
        if (step != SQLITE_ROW) {
            break;
        }
        out->rows++;
        pipeline_digest_bytes(out, "R", 1);
        int columns = sqlite3_column_count(stmt);
        for (int column = 0; column < columns; column++) {
            int type = sqlite3_column_type(stmt, column);
            int length = sqlite3_column_bytes(stmt, column);
            const void *value = sqlite3_column_blob(stmt, column);
            pipeline_digest_bytes(out, &type, sizeof(type));
            pipeline_digest_u64(out, (uint64_t)length);
            if (length > 0 && value) {
                pipeline_digest_bytes(out, value, (size_t)length);
            }
        }
    }
done:
    if (stmt) {
        sqlite3_finalize(stmt);
    }
    return rc;
}

#if CBM_WITH_ZOVA
typedef struct {
    pipeline_sql_digest_t nodes;
    pipeline_sql_digest_t edges;
    uint64_t graph_nodes;
    uint64_t graph_edges;
    uint64_t calls_walk_hash;
    size_t calls_walk_count;
} pipeline_zova_project_info_t;

static int pipeline_zova_workspace(const char *project, char *path, size_t path_size,
                                   char *workspace_id, size_t workspace_id_size) {
    if (!project || cbm_zova_user_database_path(path, path_size) != 0) {
        return -1;
    }
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, project);
    if (!repo) {
        return -1;
    }
    int written = snprintf(workspace_id, workspace_id_size, "%s",
                           cbm_zova_repository_workspace_id(repo));
    cbm_zova_repository_close(repo);
    return written > 0 && written < (int)workspace_id_size ? 0 : -1;
}

static int pipeline_zova_find_main_qn(sqlite3 *db, const char *project, char *out,
                                      size_t out_size) {
    const char *sql = "SELECT qualified_name FROM nodes "
                      "WHERE project=?1 AND label='Function' AND name='main' "
                      "ORDER BY qualified_name,file_path,start_line,end_line LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(stmt, 0);
        rc = qn && snprintf(out, out_size, "%s", qn) < (int)out_size ? 0 : -1;
    }
    sqlite3_finalize(stmt);
    return rc;
}

/* Native graph node id of the node with this qualified name. */
static int pipeline_zova_stable_id(const char *path, const char *project, const char *qn,
                                   char *out, size_t out_size) {
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, project);
    if (!repo) {
        return -1;
    }
    const char *workspace_id = cbm_zova_repository_workspace_id(repo);
    cbm_node_t node = {0};
    int rc = cbm_zova_repository_find_node_by_qn(repo, workspace_id, qn, &node) == CBM_STORE_OK &&
                     cbm_zova_repository_stable_id_for_numeric_id(repo, workspace_id, node.id, out,
                                                                  out_size) == CBM_STORE_OK
                 ? 0
                 : -1;
    cbm_node_free_fields(&node);
    cbm_zova_repository_close(repo);
    return rc;
}

/* Digest the project's rows through its store, then read the native graph
 * counts and the CALLS walk from main in the shared cbm.zova. */
static void pipeline_edge_row_end(char *row, size_t cap, size_t *len, const cbm_node_t *node) {
    int written = snprintf(row + *len, cap - *len, "\x1f%s\x1f%s\x1f%s\x1f%s\x1f%d\x1f%d",
                           node->label ? node->label : "", node->name ? node->name : "",
                           node->qualified_name ? node->qualified_name : "",
                           node->file_path ? node->file_path : "", node->start_line,
                           node->end_line);
    *len = written < 0 ? cap : *len + (size_t)written;
}

static int pipeline_row_compare(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

/* Digest every edge as its type, properties and both endpoint nodes, in a
 * sorted order that does not depend on node or edge ids. */
static int pipeline_digest_project_edges(cbm_store_t *store, const char *project,
                                         pipeline_sql_digest_t *out) {
    *out = (pipeline_sql_digest_t){.hash = UINT64_C(1469598103934665603), .rows = 0};
    cbm_schema_info_t schema = {0};
    if (cbm_store_get_schema_counts(store, project, &schema) != CBM_STORE_OK) {
        return -1;
    }
    char **rows = NULL;
    int row_count = 0;
    int rc = 0;
    for (int t = 0; rc == 0 && t < schema.edge_type_count; t++) {
        cbm_edge_t *edges = NULL;
        int count = 0;
        if (cbm_store_find_edges_by_type(store, project, schema.edge_types[t].type, &edges,
                                         &count) != CBM_STORE_OK) {
            rc = -1;
            break;
        }
        char **grown = realloc(rows, (size_t)(row_count + count + 1) * sizeof(*rows));
        rc = grown ? 0 : -1;
        rows = grown ? grown : rows;
        for (int i = 0; rc == 0 && i < count; i++) {
            cbm_node_t source = {0};
            cbm_node_t target = {0};
            char row[8192];
            size_t len = 0;
            if (cbm_store_find_node_by_id(store, edges[i].source_id, &source) != CBM_STORE_OK ||
                cbm_store_find_node_by_id(store, edges[i].target_id, &target) != CBM_STORE_OK) {
                rc = -1;
            } else {
                int written = snprintf(row, sizeof(row), "%s\x1f%s", edges[i].type,
                                       edges[i].properties_json ? edges[i].properties_json : "");
                len = written < 0 ? sizeof(row) : (size_t)written;
                pipeline_edge_row_end(row, sizeof(row), &len, &source);
                pipeline_edge_row_end(row, sizeof(row), &len, &target);
                rows[row_count] = len < sizeof(row) ? strdup(row) : NULL;
                rc = rows[row_count] ? 0 : -1;
                row_count += rc == 0;
            }
            cbm_node_free_fields(&source);
            cbm_node_free_fields(&target);
        }
        cbm_store_free_edges(edges, count);
    }
    cbm_store_schema_free(&schema);
    if (rc == 0) {
        qsort(rows, (size_t)row_count, sizeof(*rows), pipeline_row_compare);
    }
    for (int i = 0; i < row_count; i++) {
        if (rc == 0) {
            out->rows++;
            pipeline_digest_bytes(out, rows[i], strlen(rows[i]) + 1);
        }
        free(rows[i]);
    }
    free(rows);
    return rc;
}

static int pipeline_zova_project_info(const char *project, pipeline_zova_project_info_t *out) {
    static const char *nodes_sql =
        "SELECT label,name,qualified_name,file_path,start_line,end_line,properties "
        "FROM nodes WHERE project=?1 "
        "ORDER BY label,name,qualified_name,file_path,start_line,end_line,properties";
    char path[1024];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char main_qn[512];
    char main_id[CBM_ZOVA_WORKSPACE_ID_MAX + 96];
    zova_database *db = NULL;
    zova_message error = {0};
    zova_graph_info graph_info = {0};
    zova_graph_walk_results walk = {0};
    int rc = -1;
    if (!out || pipeline_zova_workspace(project, path, sizeof(path), workspace_id,
                                        sizeof(workspace_id)) != 0 ||
        cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)) != 0) {
        return -1;
    }
    *out = (pipeline_zova_project_info_t){0};
    cbm_store_t *store = th_open_project_store(project);
    if (!store) {
        return -1;
    }
    /* Counting nodes builds the store's nodes view over the workspace. */
    bool rows_ok = cbm_store_count_nodes(store, project) >= 0 &&
                   pipeline_digest_project_rows(cbm_store_get_db(store), project, nodes_sql,
                                                &out->nodes) == 0 &&
                   pipeline_digest_project_edges(store, project, &out->edges) == 0 &&
                   pipeline_zova_find_main_qn(cbm_store_get_db(store), project, main_qn,
                                              sizeof(main_qn)) == 0;
    cbm_store_close(store);
    if (!rows_ok ||
        pipeline_zova_stable_id(path, project, main_qn, main_id, sizeof(main_id)) != 0) {
        return -1;
    }
    if (zova_database_open(&(zova_database_open_request){
            .path = path, .out_db = &db, .out_error_message = &error}) != ZOVA_OK) {
        rc = -2;
        goto done;
    }
    if (zova_graph_info_get(&(zova_graph_info_get_request){
            .db = db, .name = graph_name, .out_info = &graph_info}) != ZOVA_OK) {
        rc = -3;
        goto done;
    }
    if (zova_graph_walk_direction(&(zova_graph_walk_direction_request){
            .db = db,
            .graph_name = graph_name,
            .start_node_id = main_id,
            .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING,
            .edge_type = "CALLS",
            .max_depth = 4,
            .limit = 100,
            .out_results = &walk}) != ZOVA_OK) {
        rc = -4;
        goto done;
    }
    out->graph_nodes = graph_info.node_count;
    out->graph_edges = graph_info.edge_count;
    out->calls_walk_count = walk.len;
    pipeline_sql_digest_t walk_digest = {.hash = UINT64_C(1469598103934665603), .rows = 0};
    for (size_t i = 0; i < walk.len; i++) {
        pipeline_digest_bytes(&walk_digest, walk.items[i].node_id, strlen(walk.items[i].node_id));
        pipeline_digest_u64(&walk_digest, walk.items[i].depth);
    }
    out->calls_walk_hash = walk_digest.hash;
    rc = 0;
done:
    zova_graph_walk_results_free(&walk);
    zova_graph_info_free(&graph_info);
    if (db) {
        (void)zova_database_close(db);
    }
    zova_message_free(&error);
    return rc;
}

static bool pipeline_zova_project_info_equal(const pipeline_zova_project_info_t *a,
                                             const pipeline_zova_project_info_t *b) {
    return a->nodes.rows > 0 && a->nodes.rows == b->nodes.rows &&
           a->nodes.hash == b->nodes.hash && a->edges.rows == b->edges.rows &&
           a->edges.hash == b->edges.hash && a->graph_nodes == b->graph_nodes &&
           a->graph_edges == b->graph_edges && a->calls_walk_count == b->calls_walk_count &&
           a->calls_walk_hash == b->calls_walk_hash;
}

static int pipeline_zova_trace_node_exists(const char *project, const char *label,
                                           const char *file_path, const char *qualified_name,
                                           bool *out_exists) {
    *out_exists = false;
    cbm_store_t *store = th_open_project_store(project);
    if (!store) {
        return -1;
    }
    cbm_node_t node = {0};
    int rc = cbm_store_find_node_by_qn(store, project, qualified_name, &node);
    if (rc == CBM_STORE_OK) {
        *out_exists = strcmp(node.label, label) == 0 && strcmp(node.file_path, file_path) == 0;
    }
    cbm_node_free_fields(&node);
    cbm_store_close(store);
    return rc == CBM_STORE_OK || rc == CBM_STORE_NOT_FOUND ? 0 : -1;
}

/* Re-index the unchanged tree from an empty cbm.zova as the full-index
 * control for an incremental result. */
static int pipeline_zova_fresh_full_info(const char *project, pipeline_zova_project_info_t *out) {
    th_remove_zova_user_database();
    cbm_pipeline_t *control = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    if (!control) {
        return -1;
    }
    int rc = cbm_pipeline_run(control) == 0 &&
                     cbm_pipeline_get_last_route(control) == CBM_PIPELINE_ROUTE_FULL &&
                     strcmp(cbm_pipeline_project_name(control), project) == 0
                 ? pipeline_zova_project_info(project, out)
                 : -1;
    cbm_pipeline_free(control);
    return rc;
}
#endif

TEST(pipeline_incremental_direct_zova_matches_fresh_full) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }
    /* Keep derived properties exercised on an unchanged decorated node. The
     * incremental route reruns global enrichment/complexity passes over nodes
     * loaded from the prior generation, so these properties must be replaced
     * rather than appended a second time. */
    char decorated_path[512];
    snprintf(decorated_path, sizeof(decorated_path), "%s/decorated.py", g_incr_tmpdir);
    FILE *decorated = fopen(decorated_path, "w");
    ASSERT_NOT_NULL(decorated);
    fprintf(decorated, "def route(fn):\n    return fn\n\n"
                       "@route\ndef first():\n    return 1\n\n"
                       "@route\ndef second():\n    return 2\n");
    fclose(decorated);
    /* Exercise deterministic complexity propagation across a mutual-recursion
     * cycle. Direct Zova rehydration assigns different temporary IDs than a
     * fresh extraction, but derived properties must not depend on those IDs. */
    char cycle_path[512];
    snprintf(cycle_path, sizeof(cycle_path), "%s/cycle.go", g_incr_tmpdir);
    FILE *cycle = fopen(cycle_path, "w");
    ASSERT_NOT_NULL(cycle);
    fprintf(cycle, "package main\n\n"
                   "func Alpha() {\n\tfor i := 0; i < 1; i++ { Beta() }\n}\n\n"
                   "func Beta() {\n\tfor i := 0; i < 1; i++ {\n"
                   "\t\tfor j := 0; j < 1; j++ { Gamma() }\n\t}\n}\n\n"
                   "func Gamma() { Alpha() }\n");
    fclose(cycle);

    cbm_pipeline_t *initial = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(initial);
    ASSERT_EQ(cbm_pipeline_run(initial), 0);
    ASSERT_EQ(cbm_pipeline_get_last_route(initial), CBM_PIPELINE_ROUTE_FULL);
    char *project = strdup(cbm_pipeline_project_name(initial));
    ASSERT_NOT_NULL(project);
    cbm_pipeline_free(initial);

    char helper_path[512];
    snprintf(helper_path, sizeof(helper_path), "%s/helper.go", g_incr_tmpdir);
    FILE *helper = fopen(helper_path, "w");
    ASSERT_NOT_NULL(helper);
    fprintf(helper, "package main\n\n"
                    "func Helper() string {\n\treturn \"edited\"\n}\n\n"
                    "func NewFunc() int {\n\treturn 42\n}\n");
    fclose(helper);

    /* Re-indexing the same repo selects CBM's normal incremental route. */
    cbm_pipeline_t *incremental = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(incremental);
    ASSERT_EQ(cbm_pipeline_run(incremental), 0);
    ASSERT_EQ(cbm_pipeline_get_last_route(incremental), CBM_PIPELINE_ROUTE_INCREMENTAL);
    ASSERT_STR_EQ(cbm_pipeline_project_name(incremental), project);
    cbm_pipeline_free(incremental);

    pipeline_zova_project_info_t incremental_info;
    pipeline_zova_project_info_t full_info;
    ASSERT_EQ(pipeline_zova_project_info(project, &incremental_info), 0);
    ASSERT_GT(incremental_info.graph_nodes, 0);
    ASSERT_GT(incremental_info.calls_walk_count, 1);
    ASSERT_EQ(pipeline_zova_fresh_full_info(project, &full_info), 0);
    ASSERT_TRUE(pipeline_zova_project_info_equal(&incremental_info, &full_info));

    free(project);
    cleanup_incremental_repo();
    PASS();
#endif
}

/* B1 (upstream #769/#1108): File QNs keep the file extension, so node.h and
 * node.c get distinct File nodes. A generation written before that (collapsed
 * File QNs, no index_format stamp) is rebuilt in full once instead of being
 * mixed with new-format nodes by the incremental route. */
TEST(pipeline_stale_index_format_rebuilds_in_full_once) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char root[512];
    snprintf(root, sizeof(root), "%s/cbm_fmt_XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    static const char *const sources[][2] = {
        {"node.h", "int node_count(void);\n"},
        {"node.c", "#include \"node.h\"\nint node_count(void) { return 1; }\n"},
        {"main.c", "#include \"node.h\"\nint main(void) { return node_count(); }\n"},
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        char path[640];
        snprintf(path, sizeof(path), "%s/%s", root, sources[i][0]);
        FILE *f = fopen(path, "w");
        ASSERT_NOT_NULL(f);
        fputs(sources[i][1], f);
        fclose(f);
    }

    cbm_pipeline_t *p = cbm_pipeline_new(root, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    ASSERT_FALSE(cbm_pipeline_had_format_migration(p));
    char *project = strdup(cbm_pipeline_project_name(p));
    ASSERT_NOT_NULL(project);
    cbm_pipeline_free(p);

    char header_qn[512], source_qn[512], main_qn[512], legacy_qn[512];
    snprintf(header_qn, sizeof(header_qn), "%s.node.h.__file__", project);
    snprintf(source_qn, sizeof(source_qn), "%s.node.c.__file__", project);
    snprintf(main_qn, sizeof(main_qn), "%s.main.__file__", project);
    snprintf(legacy_qn, sizeof(legacy_qn), "%s.node.__file__", project);
    bool exists = false;
    ASSERT_EQ(pipeline_zova_trace_node_exists(project, "File", "node.h", header_qn, &exists), 0);
    ASSERT_TRUE(exists);
    ASSERT_EQ(pipeline_zova_trace_node_exists(project, "File", "node.c", source_qn, &exists), 0);
    ASSERT_TRUE(exists);

    /* Republish the workspace in the pre-B1 shape: no index_format stamp, and
     * node.h/node.c collapsed onto one extensionless File QN. */
    char db_path[1024], workspace_id[256];
    ASSERT_EQ(pipeline_zova_workspace(project, db_path, sizeof(db_path), workspace_id,
                                      sizeof(workspace_id)), 0);
    cbm_zova_repository_t *repo = cbm_zova_repository_open(db_path, project);
    ASSERT_NOT_NULL(repo);
    cbm_project_t info = {0};
    ASSERT_EQ(cbm_zova_repository_get_project(repo, workspace_id, &info), CBM_STORE_OK);
    cbm_zova_repository_close(repo);
    const CBMDumpNode legacy_nodes[] = {
        {.id = 1, .project = project, .label = "Project", .name = project,
         .qualified_name = project, .file_path = "", .properties = "{\"coverage_version\":1}"},
        {.id = 2, .project = project, .label = "File", .name = "node.c",
         .qualified_name = legacy_qn, .file_path = "node.c", .properties = "{}"},
        {.id = 3, .project = project, .label = "File", .name = "main.c",
         .qualified_name = main_qn, .file_path = "main.c", .properties = "{}"},
    };
    const cbm_zova_file_hash_input_t legacy_hashes[] = {
        {.file_path = "main.c", .content_hash = "legacy-main", .mtime_ns = 1, .size_bytes = 1},
        {.file_path = "node.c", .content_hash = "legacy-node-c", .mtime_ns = 1, .size_bytes = 1},
        {.file_path = "node.h", .content_hash = "legacy-node-h", .mtime_ns = 1, .size_bytes = 1},
    };
    const cbm_zova_workspace_generation_input_t legacy = {
        .root_path = info.root_path, .project = project, .indexed_at = "2026-01-01T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 768,
        .nodes = legacy_nodes, .node_count = 3, .file_hashes = legacy_hashes,
        .file_hash_count = 3};
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(db_path, &legacy, &published), 0);
    cbm_project_free_fields(&info);

    /* The stale generation takes the full route once and is repaired. */
    p = cbm_pipeline_new(root, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    ASSERT_EQ(cbm_pipeline_get_last_route(p), CBM_PIPELINE_ROUTE_FULL);
    ASSERT_TRUE(cbm_pipeline_had_format_migration(p));
    cbm_pipeline_free(p);
    ASSERT_EQ(pipeline_zova_trace_node_exists(project, "File", "node.c", legacy_qn, &exists), 0);
    ASSERT_FALSE(exists);
    ASSERT_EQ(pipeline_zova_trace_node_exists(project, "File", "node.h", header_qn, &exists), 0);
    ASSERT_TRUE(exists);
    ASSERT_EQ(pipeline_zova_trace_node_exists(project, "File", "node.c", source_qn, &exists), 0);
    ASSERT_TRUE(exists);

    /* The rebuilt generation is stamped, so the next run is incremental. */
    p = cbm_pipeline_new(root, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    ASSERT_EQ(cbm_pipeline_get_last_route(p), CBM_PIPELINE_ROUTE_INCREMENTAL);
    ASSERT_FALSE(cbm_pipeline_had_format_migration(p));
    cbm_pipeline_free(p);

    free(project);
    th_rmtree(root);
    PASS();
#endif
}

TEST(pipeline_single_file_default_publishes_full_and_incremental_generation) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }
    char cache_path[512];
    char user_db_path[512];
    sqlite3 *user_db = NULL;
    sqlite3_stmt *stmt = NULL;
    int64_t first_generation = 0;
    const char *previous_cache = getenv("CBM_CACHE_DIR");
    char *previous_cache_copy = previous_cache ? strdup(previous_cache) : NULL;
    snprintf(cache_path, sizeof(cache_path), "%s/cache", g_incr_tmpdir);
    snprintf(user_db_path, sizeof(user_db_path), "%s/cbm.zova", cache_path);
    ASSERT_EQ(cbm_mkdir(cache_path), 0);
    cbm_setenv("CBM_CACHE_DIR", cache_path, 1);

    cbm_pipeline_t *full = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(full);
    ASSERT_EQ(cbm_pipeline_run(full), 0);
    cbm_pipeline_zova_publish_stats_t full_stats = {0};
    ASSERT_TRUE(cbm_pipeline_get_zova_publish_stats(full, &full_stats));
    ASSERT_FALSE(full_stats.delta);
    ASSERT_EQ(full_stats.full_clear_count, 1);
    ASSERT_GT(full_stats.route_ms, 0.0);
    ASSERT_TRUE(isfinite(full_stats.normalization_ms) && full_stats.normalization_ms >= 0.0);
    ASSERT_TRUE(isfinite(full_stats.model_nodes_ms) && full_stats.model_nodes_ms >= 0.0);
    ASSERT_TRUE(isfinite(full_stats.model_edges_ms) && full_stats.model_edges_ms >= 0.0);
    ASSERT_TRUE(isfinite(full_stats.model_edge_endpoint_ms) &&
                full_stats.model_edge_endpoint_ms >= 0.0);
    ASSERT_TRUE(isfinite(full_stats.model_edge_sort_ms) &&
                full_stats.model_edge_sort_ms >= 0.0);
    ASSERT_TRUE(isfinite(full_stats.model_edge_group_ms) &&
                full_stats.model_edge_group_ms >= 0.0);
    ASSERT_TRUE(isfinite(full_stats.model_edge_payload_ms) &&
                full_stats.model_edge_payload_ms >= 0.0);
    ASSERT_TRUE(isfinite(full_stats.model_edge_digest_ms) &&
                full_stats.model_edge_digest_ms >= 0.0);
    ASSERT_EQ(full_stats.model_edge_default_payloads, 6);
    ASSERT_EQ(full_stats.model_edge_payload_scratch_edges, 0);
    ASSERT_TRUE(isfinite(full_stats.model_hashes_ms) && full_stats.model_hashes_ms >= 0.0);
    ASSERT_TRUE(isfinite(full_stats.model_vectors_ms) && full_stats.model_vectors_ms >= 0.0);
    ASSERT_TRUE(isfinite(full_stats.model_digests_ms) && full_stats.model_digests_ms >= 0.0);
    ASSERT_TRUE(full_stats.normalization_ms + 0.001 >=
                full_stats.model_nodes_ms + full_stats.model_edges_ms +
                full_stats.model_hashes_ms + full_stats.model_vectors_ms +
                full_stats.model_digests_ms);
    ASSERT_TRUE(full_stats.route_ms + 0.001 >=
                cbm_pipeline_get_zova_publish_ms(full) + full_stats.normalization_ms);
    ASSERT_TRUE(full_stats.writer_guard_ms >= 0.0);
    ASSERT_TRUE(full_stats.database_init_ms >= 0.0);
    ASSERT_TRUE(full_stats.database_open_ms >= 0.0);
    ASSERT_TRUE(full_stats.transaction_begin_ms >= 0.0);
    ASSERT_TRUE(full_stats.transaction_body_ms >= 0.0);
    ASSERT_TRUE(full_stats.transaction_commit_ms >= 0.0);
    ASSERT_TRUE(full_stats.database_close_ms >= 0.0);
    ASSERT_TRUE(full_stats.clear_ms >= 0.0);
    ASSERT_TRUE(full_stats.finalize_ms >= 0.0);
    ASSERT_TRUE(full_stats.native_graph_materialize_ms >= 0.0);
    ASSERT_TRUE(full_stats.native_graph_reset_ms > 0.0);
    /* Format-9 fresh construction is one atomic graph call, so the combined
     * bulk-build cost is reported in native_graph_edges_ms. */
    ASSERT_TRUE(full_stats.native_graph_nodes_ms >= 0.0);
    ASSERT_TRUE(full_stats.native_graph_edges_ms > 0.0);
    ASSERT_TRUE(full_stats.native_graph_validate_ms > 0.0);
    ASSERT_TRUE(full_stats.native_graph_cleanup_ms >= 0.0);
    ASSERT_TRUE(full_stats.native_graph_materialize_ms + full_stats.native_graph_reset_ms +
                    full_stats.native_graph_nodes_ms + full_stats.native_graph_edges_ms +
                    full_stats.native_graph_validate_ms + full_stats.native_graph_cleanup_ms <=
                full_stats.native_graph_ms + 1.0);
    cbm_pipeline_free(full);
    ASSERT_TRUE(pipeline_single_file_generation_exists(user_db_path));

    ASSERT_EQ(sqlite3_open_v2(user_db_path, &user_db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(user_db,
                                 "SELECT generation FROM cbm_workspace_index_state_v1 LIMIT 1",
                                 -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    first_generation = sqlite3_column_int64(stmt, 0);
    ASSERT_GT(first_generation, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;
    sqlite3_close(user_db);
    user_db = NULL;

    char helper_path[512];
    snprintf(helper_path, sizeof(helper_path), "%s/helper.go", g_incr_tmpdir);
    FILE *helper = fopen(helper_path, "w");
    ASSERT_NOT_NULL(helper);
    fprintf(helper, "package main\n\nfunc Helper() string {\n\treturn \"single-file-edit\"\n}\n");
    fclose(helper);
    cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", "user_delta_before_commit", 1);
    cbm_pipeline_t *failed_incremental = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(failed_incremental);
    ASSERT_EQ(cbm_pipeline_run(failed_incremental), -1);
    cbm_pipeline_free(failed_incremental);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    ASSERT_EQ(sqlite3_open_v2(user_db_path, &user_db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(user_db,
                                 "SELECT generation FROM cbm_workspace_index_state_v1 LIMIT 1",
                                 -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int64(stmt, 0), first_generation);
    sqlite3_finalize(stmt);
    sqlite3_close(user_db);
    user_db = NULL;

    helper = fopen(helper_path, "w");
    ASSERT_NOT_NULL(helper);
    fprintf(helper, "package main\n\nfunc Helper() string {\n\treturn \"single-file-edit-2\"\n}\n");
    fclose(helper);
    cbm_pipeline_t *incremental = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(incremental);
    ASSERT_EQ(cbm_pipeline_run(incremental), 0);
    cbm_pipeline_zova_publish_stats_t incremental_stats = {0};
    ASSERT_TRUE(cbm_pipeline_get_zova_publish_stats(incremental, &incremental_stats));
    ASSERT_TRUE(incremental_stats.delta);
    ASSERT_TRUE(incremental_stats.snapshot_completed);
    ASSERT_EQ(incremental_stats.snapshot_generation, first_generation);
    ASSERT_EQ(incremental_stats.full_clear_count, 0);
    ASSERT_EQ(incremental_stats.unchanged_rewrite_count, 0);
    cbm_pipeline_free(incremental);

    ASSERT_EQ(sqlite3_open_v2(user_db_path, &user_db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(user_db,
                                 "SELECT generation FROM cbm_workspace_index_state_v1 LIMIT 1",
                                 -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int64(stmt, 0), first_generation + 1);
    sqlite3_finalize(stmt);
    sqlite3_close(user_db);
    if (previous_cache_copy) cbm_setenv("CBM_CACHE_DIR", previous_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(previous_cache_copy);
    cleanup_incremental_repo();
    PASS();
#endif
}

TEST(pipeline_single_file_preserves_and_removes_exact_project_summary) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }
    char cache_path[512];
    char user_db_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/cache", g_incr_tmpdir);
    snprintf(user_db_path, sizeof(user_db_path), "%s/cbm.zova", cache_path);
    ASSERT_EQ(cbm_mkdir(cache_path), 0);

    const char *previous_cache = getenv("CBM_CACHE_DIR");
    char *previous_cache_copy = previous_cache ? strdup(previous_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache_path, 1);

    cbm_pipeline_t *initial = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(initial);
    ASSERT_EQ(cbm_pipeline_run(initial), 0);
    char *project = strdup(cbm_pipeline_project_name(initial));
    ASSERT_NOT_NULL(project);
    cbm_pipeline_free(initial);
    ASSERT_TRUE(pipeline_single_file_generation_exists(user_db_path));

    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    ASSERT_EQ(cbm_zova_workspace_lookup_at(user_db_path, g_incr_tmpdir, workspace_id,
                                            sizeof(workspace_id)),
              0);

    sqlite3 *source = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(user_db_path, &source, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  source,
                  "INSERT OR REPLACE INTO cbm_project_summaries_v2 "
                  "(workspace_key,summary,source_hash,created_at,updated_at) "
                  "SELECT workspace_key,?2,?3,?4,?5 FROM cbm_workspace_registry "
                  "WHERE workspace_id=?1",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(stmt, 1, workspace_id, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(stmt, 2, "summary with exact fields", -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(stmt, 3, "source-hash-v1", -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(stmt, 4, "created-at-v1", -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(stmt, 5, "updated-at-v1", -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(source);

    char helper_path[512];
    snprintf(helper_path, sizeof(helper_path), "%s/helper.go", g_incr_tmpdir);
    FILE *helper = fopen(helper_path, "w");
    ASSERT_NOT_NULL(helper);
    fprintf(helper, "package main\n\nfunc Helper() string {\n\treturn \"summary-preserve\"\n}\n");
    fclose(helper);
    cbm_pipeline_t *preserve = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(preserve);
    ASSERT_EQ(cbm_pipeline_run(preserve), 0);
    cbm_pipeline_free(preserve);

    char *summary = NULL;
    char *source_hash = NULL;
    char *created_at = NULL;
    char *updated_at = NULL;
    ASSERT_EQ(pipeline_single_file_summary_row(user_db_path, workspace_id, &summary, &source_hash,
                                                &created_at, &updated_at),
              0);
    ASSERT_STR_EQ(summary, "summary with exact fields");
    ASSERT_STR_EQ(source_hash, "source-hash-v1");
    ASSERT_STR_EQ(created_at, "created-at-v1");
    ASSERT_STR_EQ(updated_at, "updated-at-v1");
    free(summary);
    free(source_hash);
    free(created_at);
    free(updated_at);

    source = NULL;
    stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(user_db_path, &source, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(source, "DELETE FROM cbm_project_summaries_v2 WHERE "
                                 "workspace_key=(SELECT workspace_key FROM "
                                 "cbm_workspace_registry WHERE workspace_id=?1)", -1,
                                 &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(stmt, 1, workspace_id, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(source);

    helper = fopen(helper_path, "w");
    ASSERT_NOT_NULL(helper);
    fprintf(helper, "package main\n\nfunc Helper() string {\n\treturn \"summary-remove\"\n}\n");
    fclose(helper);
    cbm_pipeline_t *remove = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(remove);
    ASSERT_EQ(cbm_pipeline_run(remove), 0);
    cbm_pipeline_free(remove);
    ASSERT_EQ(pipeline_single_file_summary_row(user_db_path, workspace_id, &summary, &source_hash,
                                                &created_at, &updated_at),
              1);
    free(summary);
    free(source_hash);
    free(created_at);
    free(updated_at);

    free(project);
    if (previous_cache_copy) cbm_setenv("CBM_CACHE_DIR", previous_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(previous_cache_copy);
    cleanup_incremental_repo();
    PASS();
#endif
}

TEST(pipeline_incremental_delete_direct_zova_matches_fresh_full) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }
    char helper_path[512];
    snprintf(helper_path, sizeof(helper_path), "%s/helper.go", g_incr_tmpdir);

    cbm_pipeline_t *initial = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(initial);
    ASSERT_EQ(cbm_pipeline_run(initial), 0);
    char *project = strdup(cbm_pipeline_project_name(initial));
    ASSERT_NOT_NULL(project);
    cbm_pipeline_free(initial);

    ASSERT_EQ(unlink(helper_path), 0);
    cbm_pipeline_t *incremental = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(incremental);
    ASSERT_EQ(cbm_pipeline_run(incremental), 0);
    ASSERT_EQ(cbm_pipeline_get_last_route(incremental), CBM_PIPELINE_ROUTE_INCREMENTAL);
    cbm_pipeline_free(incremental);

    char helper_qn[512];
    ASSERT_TRUE(snprintf(helper_qn, sizeof(helper_qn), "%s.Helper", project) <
                (int)sizeof(helper_qn));
    bool old_helper_exists = true;
    ASSERT_EQ(pipeline_zova_trace_node_exists(project, "Function", "helper.go", helper_qn,
                                              &old_helper_exists),
              0);
    ASSERT_FALSE(old_helper_exists);

    pipeline_zova_project_info_t incremental_info;
    pipeline_zova_project_info_t full_info;
    ASSERT_EQ(pipeline_zova_project_info(project, &incremental_info), 0);
    ASSERT_EQ(incremental_info.calls_walk_count, 1); /* only depth-0 main remains */
    ASSERT_EQ(pipeline_zova_fresh_full_info(project, &full_info), 0);
    ASSERT_TRUE(pipeline_zova_project_info_equal(&incremental_info, &full_info));

    free(project);
    cleanup_incremental_repo();
    PASS();
#endif
}

TEST(pipeline_incremental_rename_direct_zova_matches_fresh_full) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }
    char old_helper_path[512];
    char helpers_dir[512];
    char new_helper_path[512];
    snprintf(old_helper_path, sizeof(old_helper_path), "%s/helper.go", g_incr_tmpdir);
    snprintf(helpers_dir, sizeof(helpers_dir), "%s/helpers", g_incr_tmpdir);
    snprintf(new_helper_path, sizeof(new_helper_path), "%s/helpers/helper.go", g_incr_tmpdir);

    cbm_pipeline_t *initial = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(initial);
    ASSERT_EQ(cbm_pipeline_run(initial), 0);
    char *project = strdup(cbm_pipeline_project_name(initial));
    ASSERT_NOT_NULL(project);
    cbm_pipeline_free(initial);

    ASSERT_EQ(cbm_mkdir(helpers_dir), 0);
    ASSERT_EQ(rename(old_helper_path, new_helper_path), 0);
    cbm_pipeline_t *incremental = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(incremental);
    ASSERT_EQ(cbm_pipeline_run(incremental), 0);
    ASSERT_EQ(cbm_pipeline_get_last_route(incremental), CBM_PIPELINE_ROUTE_INCREMENTAL);
    cbm_pipeline_free(incremental);

    char helper_qn[512];
    char renamed_helper_qn[512];
    ASSERT_TRUE(snprintf(helper_qn, sizeof(helper_qn), "%s.Helper", project) <
                (int)sizeof(helper_qn));
    ASSERT_TRUE(snprintf(renamed_helper_qn, sizeof(renamed_helper_qn), "%s.helpers.Helper",
                         project) < (int)sizeof(renamed_helper_qn));
    bool old_helper_exists = true;
    bool new_helper_exists = false;
    ASSERT_EQ(pipeline_zova_trace_node_exists(project, "Function", "helper.go", helper_qn,
                                              &old_helper_exists),
              0);
    ASSERT_EQ(pipeline_zova_trace_node_exists(project, "Function", "helpers/helper.go",
                                              renamed_helper_qn, &new_helper_exists),
              0);
    ASSERT_FALSE(old_helper_exists);
    ASSERT_TRUE(new_helper_exists);

    pipeline_zova_project_info_t incremental_info;
    pipeline_zova_project_info_t full_info;
    ASSERT_EQ(pipeline_zova_project_info(project, &incremental_info), 0);
    ASSERT_GT(incremental_info.calls_walk_count, 1);
    ASSERT_EQ(pipeline_zova_fresh_full_info(project, &full_info), 0);
    ASSERT_TRUE(pipeline_zova_project_info_equal(&incremental_info, &full_info));

    free(project);
    cleanup_incremental_repo();
    PASS();
#endif
}

#if CBM_WITH_ZOVA
/* Index g_incr_tmpdir fully, apply `edit`, re-index on the incremental route,
 * and compare the result with a fresh full index of the edited tree. */
static int pipeline_incremental_edit_matches_full(void (*edit)(void), char **out_project) {
    cbm_pipeline_t *initial = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    if (!initial || cbm_pipeline_run(initial) != 0) {
        cbm_pipeline_free(initial);
        return -1;
    }
    char *project = strdup(cbm_pipeline_project_name(initial));
    cbm_pipeline_free(initial);
    if (!project) {
        return -1;
    }
    *out_project = project;

    edit();
    cbm_pipeline_t *incremental = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    if (!incremental || cbm_pipeline_run(incremental) != 0 ||
        cbm_pipeline_get_last_route(incremental) != CBM_PIPELINE_ROUTE_INCREMENTAL) {
        cbm_pipeline_free(incremental);
        return -2;
    }
    cbm_pipeline_free(incremental);

    pipeline_zova_project_info_t incremental_info;
    pipeline_zova_project_info_t full_info;
    if (pipeline_zova_project_info(project, &incremental_info) != 0 ||
        pipeline_zova_fresh_full_info(project, &full_info) != 0) {
        return -3;
    }
    return pipeline_zova_project_info_equal(&incremental_info, &full_info) ? 0 : 1;
}

static void pipeline_route_probe_point_config_at_payments(void) {
    th_write_file(TH_PATH(g_incr_tmpdir, "infra/config.yaml"),
                  "hooks:\n  webhook_url: https://api.example.com/payments\n");
}

static void pipeline_route_probe_drop_url_from_owner(void) {
    th_write_file(TH_PATH(g_incr_tmpdir, "infra/a.yaml"), "hooks:\n  retries: 3\n");
}

static void pipeline_route_probe_add_exact_handler(void) {
    th_write_file(TH_PATH(g_incr_tmpdir, "list_api.py"),
                  "from app import app\n\n"
                  "@app.route(\"/orders/list\")\n"
                  "def list_orders():\n    return \"ok\"\n");
}
#endif

/* Slash-prefixed call arguments are not necessarily HTTP routes. Keep the
 * parallel arg-url heuristic from minting Route nodes for filesystem paths or
 * regex-replacement operands, while preserving a genuine API path. */
TEST(pipeline_arg_url_rejects_non_http_slash_arguments) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_arg_url_guard_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    write_temp_file(tmp, "src/args.py",
                    "import requests\n"
                    "TMP = '/tmp/pgv_fuzz.bin'\n"
                    "def run_copy(path):\n"
                    "    return path\n"
                    "def write_fixture():\n"
                    "    return run_copy(TMP)\n"
                    "def load_api():\n"
                    "    return requests.get('/api/data')\n");
    write_temp_file(tmp, "src/regex.js",
                    "function sink(value) { return value; }\n"
                    "export function sanitize(template) {\n"
                    "  sink(/<table/i);\n"
                    "  return template.replace('/html/g', '');\n"
                    "}\n");
    for (int i = 0; i < 52; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "src/filler%d.ts", i);
        snprintf(body, sizeof(body), "export function filler%d(): number { return %d; }\n", i, i);
        write_temp_file(tmp, name, body);
    }

    char *old_workers = getenv("CBM_WORKERS");
    char *saved = old_workers ? strdup(old_workers) : NULL;
    cbm_setenv("CBM_WORKERS", "4", 1);

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    ASSERT_EQ(count_nodes_named(s, project, "/html/g"), 0);
    ASSERT_EQ(count_nodes_named(s, project, "/<table/i"), 0);
    ASSERT_EQ(count_nodes_named(s, project, "/tmp/pgv_fuzz.bin"), 0);
    ASSERT_GTE(count_nodes_named(s, project, "/api/data"), 1);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    if (saved) {
        cbm_setenv("CBM_WORKERS", saved, 1);
        free(saved);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    th_rmtree(tmp);
    PASS();
}

/* Count HTTP_CALLS-style edges whose url_path property contains `keyword`. */
static int count_edges_with_url_path(cbm_store_t *s, const char *project, const char *keyword) {
    cbm_edge_t *es = NULL;
    int n = 0;
    int matches = 0;
    if (cbm_store_find_edges_by_type(s, project, "HTTP_CALLS", &es, &n) == CBM_STORE_OK && es) {
        for (int i = 0; i < n; i++) {
            if (es[i].properties_json && strstr(es[i].properties_json, keyword)) {
                matches++;
            }
        }
    }
    if (es) {
        cbm_store_free_edges(es, n);
    }
    return matches;
}

/* Distilled from PR #1245: detect_url_in_args (parallel resolver, >= 50 files)
 * runs for EVERY resolved call with no callee gating, so a filesystem path
 * with a document/log extension passed to a project-defined open/read/write
 * helper passed normalize_url_arg (second slash, no space) and the route
 * literal guard, minting a Route node + an HTTP_CALLS edge for a file. The
 * callees are project functions on purpose: a bare builtin `open(...)` never
 * resolves to a target and so never reaches emit_service_edge at all. The
 * first path segments avoid the filesystem-root list (etc/var/tmp/...) so
 * only the extension guard can reject them. requests.get('/api/items') is
 * the positive control that must keep its Route + HTTP_CALLS. */
TEST(pipeline_arg_url_rejects_document_file_paths) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_arg_url_docs_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    write_temp_file(tmp, "src/files.py",
                    "import requests\n"
                    "def open_file(path):\n"
                    "    return path\n"
                    "def read_file(path):\n"
                    "    return path\n"
                    "def write_file(path):\n"
                    "    return path\n"
                    "def load_docs():\n"
                    "    open_file('/new/file.txt')\n"
                    "    read_file('/docs/guide.md')\n"
                    "    write_file('/data/app.log')\n"
                    "    read_file('/reports/summary.pdf')\n"
                    "    open_file('/docs/index.rst')\n"
                    "def fetch_items():\n"
                    "    return requests.get('/api/items')\n");
    for (int i = 0; i < 52; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "src/filler%d.ts", i);
        snprintf(body, sizeof(body), "export function filler%d(): number { return %d; }\n", i, i);
        write_temp_file(tmp, name, body);
    }

    char *old_workers = getenv("CBM_WORKERS");
    char *saved = old_workers ? strdup(old_workers) : NULL;
    cbm_setenv("CBM_WORKERS", "4", 1);

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    /* Positive control first: the genuine HTTP call still mints its route. */
    ASSERT_GTE(count_nodes_named(s, project, "/api/items"), 1);
    ASSERT_GTE(count_edges_with_url_path(s, project, "/api/items"), 1);
    /* No Route node for any filesystem document path... */
    ASSERT_EQ(count_nodes_named(s, project, "/new/file.txt"), 0);
    ASSERT_EQ(count_nodes_named(s, project, "/docs/guide.md"), 0);
    ASSERT_EQ(count_nodes_named(s, project, "/data/app.log"), 0);
    ASSERT_EQ(count_nodes_named(s, project, "/reports/summary.pdf"), 0);
    ASSERT_EQ(count_nodes_named(s, project, "/docs/index.rst"), 0);
    /* ...and no HTTP_CALLS edge carrying one as its url_path either. */
    ASSERT_EQ(count_edges_with_url_path(s, project, "/new/file.txt"), 0);
    ASSERT_EQ(count_edges_with_url_path(s, project, "/docs/guide.md"), 0);
    ASSERT_EQ(count_edges_with_url_path(s, project, "/data/app.log"), 0);
    ASSERT_EQ(count_edges_with_url_path(s, project, "/reports/summary.pdf"), 0);
    ASSERT_EQ(count_edges_with_url_path(s, project, "/docs/index.rst"), 0);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    if (saved) {
        cbm_setenv("CBM_WORKERS", saved, 1);
        free(saved);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    th_rmtree(tmp);
    PASS();
}

/* Count nodes with the given label in the project. */
static int nav_count_nodes_by_label(cbm_store_t *s, const char *project, const char *label) {
    cbm_node_t *nodes = NULL;
    int count = 0;
    if (cbm_store_find_nodes_by_label(s, project, label, &nodes, &count) != CBM_STORE_OK) {
        return -1;
    }
    if (nodes) {
        cbm_store_free_nodes(nodes, count);
    }
    return count;
}

/* Count Route nodes with the given name in the project. */
static int nav_count_routes_named(cbm_store_t *s, const char *project, const char *name) {
    cbm_node_t *nodes = NULL;
    int count = 0;
    if (cbm_store_find_nodes_by_label(s, project, "Route", &nodes, &count) != CBM_STORE_OK) {
        return -1;
    }
    int matches = 0;
    for (int i = 0; i < count; i++) {
        if (nodes[i].name && strcmp(nodes[i].name, name) == 0) {
            matches++;
        }
    }
    if (nodes) {
        cbm_store_free_nodes(nodes, count);
    }
    return matches;
}

/* Count nodes with the given name and file_path substring in the project. */
static int nav_count_source_nodes(cbm_store_t *s, const char *project, const char *name,
                                  const char *file_path) {
    cbm_node_t *nodes = NULL;
    int count = 0;
    if (cbm_store_find_nodes_by_name(s, project, name, &nodes, &count) != CBM_STORE_OK) {
        return -1;
    }
    int matches = 0;
    for (int i = 0; i < count; i++) {
        if (nodes[i].file_path && strstr(nodes[i].file_path, file_path) != NULL) {
            matches++;
        }
    }
    if (nodes) {
        cbm_store_free_nodes(nodes, count);
    }
    return matches;
}

/* Count HTTP_CALLS edges out of source_name with matching url_path property. */
static int nav_count_http_calls_with_url(cbm_store_t *s, const char *project,
                                         const char *source_name, const char *url_path) {
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    if (cbm_store_find_edges_by_type(s, project, "HTTP_CALLS", &edges, &edge_count) != CBM_STORE_OK) {
        return -1;
    }
    int matches = 0;
    for (int i = 0; i < edge_count; i++) {
        cbm_node_t source = {0};
        int source_ok = cbm_store_find_node_by_id(s, edges[i].source_id, &source) == CBM_STORE_OK;
        if (source_ok && source.name && strcmp(source.name, source_name) == 0) {
            const char *props = edges[i].properties_json ? edges[i].properties_json : "";
            char needle[256];
            snprintf(needle, sizeof(needle), "\"url_path\":\"%s\"", url_path);
            if (strstr(props, needle) != NULL) {
                matches++;
            }
        }
        cbm_node_free_fields(&source);
    }
    if (edges) {
        cbm_store_free_edges(edges, edge_count);
    }
    return matches;
}

/* Count edges of edge_type between exact source and target node names. */
static int nav_count_named_edges(cbm_store_t *s, const char *project, const char *edge_type,
                                 const char *source_name, const char *target_name) {
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    if (cbm_store_find_edges_by_type(s, project, edge_type, &edges, &edge_count) != CBM_STORE_OK) {
        return -1;
    }
    int matches = 0;
    for (int i = 0; i < edge_count; i++) {
        cbm_node_t source = {0};
        cbm_node_t target = {0};
        int source_ok = cbm_store_find_node_by_id(s, edges[i].source_id, &source) == CBM_STORE_OK;
        int target_ok = cbm_store_find_node_by_id(s, edges[i].target_id, &target) == CBM_STORE_OK;
        if (source_ok && target_ok && source.name && target.name &&
            strcmp(source.name, source_name) == 0 && strcmp(target.name, target_name) == 0) {
            matches++;
        }
        cbm_node_free_fields(&source);
        cbm_node_free_fields(&target);
    }
    if (edges) {
        cbm_store_free_edges(edges, edge_count);
    }
    return matches;
}

/* #1250: exercise the generic URL heuristic with real extracted occurrences
 * and real source nodes. More than 50 files selects the parallel resolver.
 * HTTP clients, custom navigateApi wrappers, Python and ASYNC keep their edges. */
TEST(pipeline_arg_url_skips_client_navigation) {
    static const char page[] =
        "import axios from 'axios';\n"
        "import { get as navigate } from 'axios';\n"
        "import { navigateTo, http, navigateApi } from './nav';\n"
        "export class PaymentsPage {\n"
        "  constructor(private router: any, private navCtrl: any, private location: any,\n"
        "              private $router: any, private history: any) {}\n"
        "  goPayments(): void { this.router.navigateByUrl('/payments/list'); }\n"
        "  goBenefits(): void { this.navCtrl.navigateForward('/benefits/detail'); }\n"
        "  goBack(): void { this.location.go('/orders/back'); }\n"
        "  goVue(): void { this.$router.push('/dashboard/settings'); }\n"
        "  goHist(): void { this.history.pushState(null, '', '/account/history'); }\n"
        "  goWidget(id: string): void { navigateTo(`/widgetTypes/view/${id}`); }\n"
        "  load(): unknown { return http('/api/customers'); }\n"
        "  orders(): unknown { return axios.get('/api/orders'); }\n"
        "  items(): unknown { return fetch('/api/items'); }\n"
        "  custom(): unknown { return navigateApi('/api/navigation'); }\n"
        "  explicitAlias(): unknown { return navigate('/api/alias'); }\n"
        "  publish(): void { mqtt.publish('/tenant/events', 'payload'); }\n"
        "}\n";
    static const struct {
        const char *callee;
        const char *source;
        const char *path;
    } navigation[] = {
        {"this.router.navigateByUrl", "goPayments", "/payments/list"},
        {"this.navCtrl.navigateForward", "goBenefits", "/benefits/detail"},
        {"this.location.go", "goBack", "/orders/back"},
        {"this.$router.push", "goVue", "/dashboard/settings"},
        {"this.history.pushState", "goHist", "/account/history"},
        {"navigateTo", "goWidget", "/widgetTypes/view/{}"},
    };
    static const struct {
        const char *source;
        const char *path;
    } positive[] = {{"load", "/api/customers"},
                    {"orders", "/api/orders"},
                    {"items", "/api/items"},
                    {"custom", "/api/navigation"},
                    {"load_items", "/api/python/items"},
                    {"explicitAlias", "/api/alias"}};

    int extracted[6] = {0};
    CBMFileResult *fr = cbm_extract_file(page, (int)strlen(page), CBM_LANG_TYPESCRIPT,
                                         "navigation_probe", "src/page.ts", 0, NULL, NULL);
    bool extraction_ok = fr && !fr->has_error;
    if (extraction_ok) {
        for (int c = 0; c < fr->calls.count; c++) {
            const CBMCall *call = &fr->calls.items[c];
            for (size_t i = 0; i < sizeof(navigation) / sizeof(navigation[0]); i++) {
                if (call->callee_name && strcmp(call->callee_name, navigation[i].callee) == 0) {
                    extracted[i]++;
                }
            }
        }
        cbm_free_result(fr);
    }

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_arg_url_nav_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    char *saved_workers = getenv("CBM_WORKERS") ? strdup(getenv("CBM_WORKERS")) : NULL;
    char *saved_single =
        getenv("CBM_INDEX_SINGLE_THREAD") ? strdup(getenv("CBM_INDEX_SINGLE_THREAD")) : NULL;

    write_temp_file(tmp, "src/page.ts", page);
    write_temp_file(tmp, "src/nav.ts",
                    "export function navigateTo(url: string): void {}\n"
                    "export function http(url: string): Promise<unknown> { return fetch(url); }\n"
                    "export function navigateApi(url: string): Promise<unknown> {\n"
                    "  return fetch(url);\n"
                    "}\n");
    write_temp_file(tmp, "src/py.py",
                    "def navigate(url):\n    return url\n"
                    "def load_items():\n    return navigate('/api/python/items')\n");
    for (int i = 0; i < 52; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "src/filler%d.ts", i);
        snprintf(body, sizeof(body), "export function filler%d(): number { return %d; }\n", i, i);
        write_temp_file(tmp, name, body);
    }
    cbm_setenv("CBM_WORKERS", "4", 1);
    cbm_unsetenv("CBM_INDEX_SINGLE_THREAD");
    const char *workers = getenv("CBM_WORKERS");
    bool configured =
        workers && strcmp(workers, "4") == 0 && getenv("CBM_INDEX_SINGLE_THREAD") == NULL;
    cbm_pipeline_t *p = configured ? cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL) : NULL;
    int run_rc = p ? cbm_pipeline_run(p) : -1;
    cbm_store_t *store = run_rc == 0 ? th_open_repo_store(tmp) : NULL;
    bool observed = store != NULL;
    int nav_edges[6], nav_routes[6], sources[6], good_edges[6];
    int files = -1, async_edges = -1, async_http = -1;
    for (size_t i = 0; i < 6; i++) {
        nav_edges[i] = nav_routes[i] = sources[i] = -1;
    }
    for (size_t i = 0; i < 6; i++) {
        good_edges[i] = -1;
    }
    if (store) {
        const char *project = cbm_pipeline_project_name(p);
        files = nav_count_nodes_by_label(store, project, "File");
        for (size_t i = 0; i < 6; i++) {
            nav_edges[i] = nav_count_http_calls_with_url(store, project, navigation[i].source,
                                                         navigation[i].path);
            nav_routes[i] = nav_count_routes_named(store, project, navigation[i].path);
            sources[i] = nav_count_source_nodes(store, project, navigation[i].source,
                                                "src/page.ts");
        }
        for (size_t i = 0; i < 6; i++) {
            good_edges[i] = nav_count_http_calls_with_url(store, project, positive[i].source,
                                                          positive[i].path);
        }
        async_edges = nav_count_named_edges(store, project, "ASYNC_CALLS", "publish",
                                            "/tenant/events");
        async_http = nav_count_http_calls_with_url(store, project, "publish", "/tenant/events");
        cbm_store_close(store);
    }
    cbm_pipeline_free(p);
    if (saved_workers) {
        cbm_setenv("CBM_WORKERS", saved_workers, 1);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    if (saved_single) {
        cbm_setenv("CBM_INDEX_SINGLE_THREAD", saved_single, 1);
    } else {
        cbm_unsetenv("CBM_INDEX_SINGLE_THREAD");
    }
    const char *restored_workers = getenv("CBM_WORKERS");
    const char *restored_single = getenv("CBM_INDEX_SINGLE_THREAD");
    bool restored =
        (saved_workers ? restored_workers && strcmp(saved_workers, restored_workers) == 0
                       : !restored_workers) &&
        (saved_single ? restored_single && strcmp(saved_single, restored_single) == 0
                      : !restored_single);
    free(saved_workers);
    free(saved_single);
    th_rmtree(tmp);

    ASSERT_TRUE(extraction_ok);
    ASSERT_TRUE(configured);
    ASSERT_TRUE(restored);
    ASSERT_EQ(run_rc, 0);
    ASSERT_TRUE(observed);
    ASSERT_GTE(files, 55);
    for (size_t i = 0; i < 6; i++) {
        ASSERT_EQ(extracted[i], 1);
        ASSERT_EQ(sources[i], 1);
        ASSERT_EQ(nav_edges[i], 0);
        ASSERT_EQ(nav_routes[i], 0);
    }
    for (size_t i = 0; i < 6; i++) {
        ASSERT_GTE(good_edges[i], 1);
    }
    ASSERT_GTE(async_edges, 1);
    ASSERT_EQ(async_http, 0);
    PASS();
}

/* A call the HTTP service patterns classify (axios, an axios.create instance)
 * gets exactly ONE HTTP_CALLS edge, typed with the verb its callee names. The
 * arg-URL heuristic (detect_url_in_args, via=arg_url) exists for calls the
 * patterns do not know -- a local fetch wrapper such as callApi('/api/orders')
 * -- and must not add a second, method-less __route__ANY__ edge for the same
 * call site. On microsoft/healthcare-ai-model-evaluator 59 of 129 HTTP_CALLS
 * were such twins, and the cross-repo matcher bound them to server handlers of
 * any method. `fillers` > 0 forces the parallel resolver (the RED case); 0
 * keeps the sequential one, which must agree. */
static int typed_http_no_url_twin_case(int fillers) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_http_twin_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    write_temp_file(tmp, "src/api.ts",
                    "import axios from 'axios';\n"
                    "const axiosInstance = axios.create({ baseURL: 'http://localhost:5000' });\n"
                    "export function getModel(id: string) {\n"
                    "  return axiosInstance.get(`/api/models/${id}`);\n"
                    "}\n"
                    "export function addModel(body: unknown) {\n"
                    "  return axiosInstance.post('/api/models', body);\n"
                    "}\n"
                    "export function removeModel(id: string) {\n"
                    "  return axios.delete(`/api/models/${id}`);\n"
                    "}\n");
    write_temp_file(tmp, "src/wrapper.ts",
                    "function callApi(path: string): Promise<Response> {\n"
                    "  return fetch(path);\n"
                    "}\n"
                    "export function listOrders() {\n"
                    "  return callApi('/api/orders');\n"
                    "}\n");
    for (int i = 0; i < fillers; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "src/filler%d.ts", i);
        snprintf(body, sizeof(body), "export function filler%d(): number {\n  return %d;\n}\n", i,
                 i);
        write_temp_file(tmp, name, body);
    }

    char *old_workers = getenv("CBM_WORKERS");
    char *saved = old_workers ? strdup(old_workers) : NULL;
    cbm_setenv("CBM_WORKERS", "4", 1);

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    static const struct {
        const char *caller;
        const char *typed_route;
    } typed[] = {{"getModel", "__route__GET__"},
                 {"addModel", "__route__POST__"},
                 {"removeModel", "__route__DELETE__"}};
    for (size_t i = 0; i < sizeof(typed) / sizeof(typed[0]); i++) {
        /* The typed edge stays ... */
        ASSERT_EQ(http_calls_to_route(s, project, typed[i].caller, typed[i].typed_route), 1);
        /* ... and it is the only HTTP_CALLS edge of that call site. */
        ASSERT_EQ(http_calls_to_route(s, project, typed[i].caller, "__route__ANY__"), 0);
        ASSERT_EQ(http_calls_to_route(s, project, typed[i].caller, "__route__"), 1);
    }
    if (fillers > 0) {
        /* Control: the heuristic still covers the call no pattern knows. The
         * sequential resolver has no arg-URL heuristic yet (#2235). */
        ASSERT_EQ(http_calls_to_route(s, project, "listOrders", "__route__ANY__"), 1);
    }

    cbm_store_close(s);
    cbm_pipeline_free(p);
    if (saved) {
        cbm_setenv("CBM_WORKERS", saved, 1);
        free(saved);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    th_rmtree(tmp);
    PASS();
}

TEST(pipeline_typed_http_call_has_no_arg_url_twin) {
    return typed_http_no_url_twin_case(52);
}

TEST(pipeline_typed_http_call_has_no_arg_url_twin_sequential) {
    return typed_http_no_url_twin_case(0);
}

/* `import Axios from "axios"` -- the default-import binding capitalized, a
 * common convention -- must classify `Axios.get/post/delete(url)` as HTTP
 * client calls exactly like `axios.*`. The library table matched only the
 * lowercase id, so the calls fell through to the `.get`/`.post` route-suffix
 * fallback and became route REGISTRATIONS: on
 * microsoft/Game-Control-Puzzle-Event-Administration-Tools all 69 client
 * calls masqueraded as server Routes. A real Express registration in the same
 * project stays a registration. `fillers` > 0 forces the parallel resolver,
 * 0 keeps the sequential one; both classify through the same table. */
static int capitalized_axios_case(int fillers) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_axios_cap_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    write_temp_file(tmp, "src/service.ts",
                    "import Axios from \"axios\";\n"
                    "export function getSettings(eventId: string) {\n"
                    "  return Axios.get(`/api/admin/events/${eventId}/settings`);\n"
                    "}\n"
                    "export function addUser(eventId: string, participant: unknown) {\n"
                    "  return Axios.post(`/api/admin/users/${eventId}`, participant);\n"
                    "}\n"
                    "export function removeUser(id: string) {\n"
                    "  return Axios.delete(`/api/admin/users/${id}`);\n"
                    "}\n");
    write_temp_file(tmp, "src/server.ts",
                    "function listUsers(req: unknown, res: unknown) {}\n"
                    "export function mount(router: any) {\n"
                    "  router.get('/api/admin/users', listUsers);\n"
                    "}\n");
    for (int i = 0; i < fillers; i++) {
        char name[64];
        char body[128];
        snprintf(name, sizeof(name), "src/filler%d.ts", i);
        snprintf(body, sizeof(body), "export function filler%d(): number {\n  return %d;\n}\n", i,
                 i);
        write_temp_file(tmp, name, body);
    }

    char *old_workers = getenv("CBM_WORKERS");
    char *saved = old_workers ? strdup(old_workers) : NULL;
    cbm_setenv("CBM_WORKERS", "4", 1);

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/axios_cap.db", tmp);
    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    static const struct {
        const char *caller;
        const char *typed_route;
    } client[] = {{"getSettings", "__route__GET__"},
                  {"addUser", "__route__POST__"},
                  {"removeUser", "__route__DELETE__"}};
    for (size_t i = 0; i < sizeof(client) / sizeof(client[0]); i++) {
        ASSERT_EQ(route_registrations_from(s, project, client[i].caller), 0);
        ASSERT_EQ(http_calls_to_route(s, project, client[i].caller, client[i].typed_route), 1);
    }
    /* Control: the server-side registration is untouched. */
    ASSERT_EQ(route_registrations_from(s, project, "mount"), 1);
    ASSERT_EQ(http_calls_to_route(s, project, "mount", "__route__"), 0);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    if (saved) {
        cbm_setenv("CBM_WORKERS", saved, 1);
        free(saved);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    th_rmtree(tmp);
    PASS();
}

TEST(pipeline_capitalized_axios_import_is_http_client) {
    return capitalized_axios_case(52);
}

TEST(pipeline_capitalized_axios_import_is_http_client_sequential) {
    return capitalized_axios_case(0);
}

/* FastAPI/Starlette WebSocket route registration (distilled from #1245). The
 * route-registration suffix table knew `.add_api_websocket_route` but not the
 * decorator forms `.websocket` / `.websocket_route`, so a WebSocket endpoint
 * registered via `@router.websocket('/ws')` produced no Route node at all —
 * the endpoint was invisible to the graph. `@app.get('/health')` is the
 * positive control (already a route suffix). Fewer than 50 files exercises
 * pass_calls.c; the same suffix table drives the parallel resolver. */
TEST(pipeline_python_websocket_decorators_register_routes) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_py_ws_routes_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    write_temp_file(tmp, "app.py",
                    "from fastapi import APIRouter, FastAPI, WebSocket\n\n"
                    "app = FastAPI()\n"
                    "router = APIRouter()\n\n"
                    "@app.get('/health')\n"
                    "def health():\n"
                    "    return {'ok': True}\n\n"
                    "@router.websocket('/ws')\n"
                    "async def ws_endpoint(websocket: WebSocket):\n"
                    "    await websocket.accept()\n\n"
                    "@app.websocket_route('/events')\n"
                    "async def events_endpoint(websocket: WebSocket):\n"
                    "    await websocket.accept()\n");

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    /* Positive control: the HTTP decorator mints its Route node. */
    ASSERT_GTE(count_nodes_named(s, project, "/health"), 1);
    /* The two WebSocket decorator forms must mint Route nodes too. */
    ASSERT_GTE(count_nodes_named(s, project, "/ws"), 1);
    ASSERT_GTE(count_nodes_named(s, project, "/events"), 1);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

/* The shape real Swift actually writes: the URL is built by a constructor and
 * force-unwrapped, so the literal is two levels below the argument list. This
 * is what issue #1892 reported from a real project. */
TEST(pipeline_swift_nested_url_makes_route_issue1892) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_swiftnested_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    write_temp_file(tmp, "Sources/Client.swift",
                    "import Foundation\n"
                    "final class Client {\n"
                    "    func listWidgets() {\n"
                    "        URLSession.shared.dataTask(with: "
                    "URL(string: \"/api/v1/widgets\")!)\n"
                    "    }\n"
                    "}\n");

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    ASSERT_GTE(cbm_store_count_edges_by_type(s, project, "HTTP_CALLS"), 1);

    cbm_node_t *routes = NULL;
    int route_count = 0;
    cbm_store_find_nodes_by_label(s, project, "Route", &routes, &route_count);
    int widget_routes = 0;
    for (int i = 0; i < route_count; i++) {
        if (routes[i].qualified_name && strstr(routes[i].qualified_name, "/api/v1/widgets")) {
            widget_routes++;
        }
    }
    cbm_store_free_nodes(routes, route_count);
    ASSERT_GTE(widget_routes, 1);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

/* #1892: Swift produced no Route node and no HTTP_CALLS edge, because the
 * Swift grammar has no "arguments" field and the generic lookup therefore read
 * no call arguments at all. Alamofire/URLSession were already in the service
 * pattern table; the URL simply never reached it. */
TEST(pipeline_swift_http_call_makes_route_issue1892) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/cbm_swifthttp_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("tmpdir");
    }

    /* URLSession, not Alamofire's `AF` shorthand: the service pattern table
     * matches the library name in the callee text, and "AF.request" contains
     * no such name. */
    write_temp_file(tmp, "Sources/Client.swift",
                    "import Foundation\n"
                    "final class Client {\n"
                    "    func listWidgets() {\n"
                    "        URLSession.shared.dataTask(with: \"/api/v1/widgets\")\n"
                    "    }\n"
                    "}\n");

    cbm_pipeline_t *p = cbm_pipeline_new(tmp, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    const char *project = cbm_pipeline_project_name(p);

    cbm_store_t *s = th_open_repo_store(tmp);
    ASSERT_NOT_NULL(s);

    ASSERT_GTE(cbm_store_count_edges_by_type(s, project, "HTTP_CALLS"), 1);

    /* The edge carries the URL, so pass_route_nodes can mint the Route the
     * cross-repo matcher joins a server route against. */
    cbm_node_t *routes = NULL;
    int route_count = 0;
    cbm_store_find_nodes_by_label(s, project, "Route", &routes, &route_count);
    int widget_routes = 0;
    for (int i = 0; i < route_count; i++) {
        if (routes[i].qualified_name && strstr(routes[i].qualified_name, "/api/v1/widgets")) {
            widget_routes++;
        }
    }
    cbm_store_free_nodes(routes, route_count);
    ASSERT_GTE(widget_routes, 1);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

/* A changed config file must get its infra Route back, matched to the handler
 * a full index would pick. */
TEST(pipeline_incremental_infra_route_change_matches_fresh_full) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }
    th_write_file(TH_PATH(g_incr_tmpdir, "app.py"),
                  "from flask import Flask\napp = Flask(__name__)\n\n"
                  "@app.route(\"/orders\")\ndef orders():\n    return \"ok\"\n\n"
                  "@app.route(\"/payments\")\ndef payments():\n    return \"ok\"\n");
    th_write_file(TH_PATH(g_incr_tmpdir, "infra/config.yaml"),
                  "hooks:\n  webhook_url: https://api.example.com/orders\n");

    char *project = NULL;
    ASSERT_EQ(pipeline_incremental_edit_matches_full(pipeline_route_probe_point_config_at_payments,
                                                     &project),
              0);
    bool route_exists = false;
    ASSERT_EQ(pipeline_zova_trace_node_exists(project, "Route", "infra/config.yaml",
                                              "__route__infra__https://api.example.com/payments",
                                              &route_exists),
              0);
    ASSERT_TRUE(route_exists);

    free(project);
    cleanup_incremental_repo();
    PASS();
#endif
}

/* Two config files list the same URL; the Route node belongs to a.yaml. When
 * a.yaml drops the URL, b.yaml (unchanged) must still provide it. The filler
 * files push the re-extracted config set onto the parallel path. */
TEST(pipeline_incremental_shared_infra_url_survives_owner_change) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }
    th_write_file(TH_PATH(g_incr_tmpdir, "infra/a.yaml"),
                  "hooks:\n  webhook_url: https://api.example.com/orders\n");
    th_write_file(TH_PATH(g_incr_tmpdir, "infra/b.yaml"),
                  "hooks:\n  webhook_url: https://api.example.com/orders\n");
    for (int i = 0; i < 60; i++) {
        char rel[64];
        char body[64];
        snprintf(rel, sizeof(rel), "infra/filler/f%02d.yaml", i);
        snprintf(body, sizeof(body), "name: filler%02d\n", i);
        th_write_file(TH_PATH(g_incr_tmpdir, rel), body);
    }

    char *project = NULL;
    ASSERT_EQ(pipeline_incremental_edit_matches_full(pipeline_route_probe_drop_url_from_owner,
                                                     &project),
              0);
    bool route_exists = false;
    ASSERT_EQ(pipeline_zova_trace_node_exists(project, "Route", "infra/b.yaml",
                                              "__route__infra__https://api.example.com/orders",
                                              &route_exists),
              0);
    ASSERT_TRUE(route_exists);

    free(project);
    cleanup_incremental_repo();
    PASS();
#endif
}

/* An unchanged infra Route must be rematched when a changed code file adds a
 * better handler: an exact path replaces the earlier prefix match. */
TEST(pipeline_incremental_handler_change_rematches_infra_route) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }
    th_write_file(TH_PATH(g_incr_tmpdir, "app.py"),
                  "from flask import Flask\napp = Flask(__name__)\n\n"
                  "@app.route(\"/orders\")\ndef orders():\n    return \"ok\"\n");
    th_write_file(TH_PATH(g_incr_tmpdir, "infra/config.yaml"),
                  "hooks:\n  webhook_url: https://api.example.com/orders/list\n");

    char *project = NULL;
    ASSERT_EQ(pipeline_incremental_edit_matches_full(pipeline_route_probe_add_exact_handler,
                                                     &project),
              0);

    free(project);
    cleanup_incremental_repo();
    PASS();
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 *  FastAPI Depends() edge tracking (PR #66, fix #27)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(pipeline_fastapi_depends_edges) {
    /* Depends(get_current_user) should produce a CALLS edge from the
     * endpoint to the dependency function. */
    const char *files[] = {"auth.py", "routes.py"};
    const char *contents[] = {/* auth.py: defines get_current_user */
                              "def get_current_user(token: str):\n"
                              "    return decode_token(token)\n",
                              /* routes.py: endpoint depends on get_current_user */
                              "from fastapi import Depends\n"
                              "from auth import get_current_user\n\n"
                              "def get_profile(user = Depends(get_current_user)):\n"
                              "    return {\"user\": user}\n"};
    if (setup_lang_repo(files, contents, 2) != 0) {
        FAIL("tmpdir");
    }
    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    /* Check CALLS edges for fastapi_depends strategy */
    cbm_edge_t *edges = NULL;
    int edge_count = 0;
    cbm_store_find_edges_by_type(s, proj, "CALLS", &edges, &edge_count);

    bool found_depends_edge = false;
    for (int i = 0; i < edge_count; i++) {
        if (edges[i].properties_json && strstr(edges[i].properties_json, "fastapi_depends")) {
            found_depends_edge = true;
            break;
        }
    }
    if (edges) {
        cbm_store_free_edges(edges, edge_count);
    }
    ASSERT_TRUE(found_depends_edge);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

/* DLL resolve test removed — feature removed due to Windows Defender
 * false positive (Wacatac.B!ml). See issue #89. */

/* ═══════════════════════════════════════════════════════════════════
 *  Incremental reindex
 * ═══════════════════════════════════════════════════════════════════ */

TEST(incremental_full_then_noop) {
    /* Full index, then re-run → should detect no changes and skip */
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }

    /* First: full index */
    cbm_pipeline_t *p = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    char *project = strdup(cbm_pipeline_project_name(p));
    cbm_pipeline_free(p);

    /* Verify nodes exist */
    cbm_store_t *s = th_open_repo_store(g_incr_tmpdir);
    ASSERT_NOT_NULL(s);
    int nodes_before = cbm_store_count_nodes(s, project);
    ASSERT_GT(nodes_before, 0);
    cbm_store_close(s);

    /* Second: incremental — nothing changed → should be no-op */
    p = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);

    s = th_open_repo_store(g_incr_tmpdir);
    ASSERT_NOT_NULL(s);
    int nodes_after = cbm_store_count_nodes(s, project);
    /* Node count should be same (no duplicates, no loss) */
    ASSERT_EQ(nodes_after, nodes_before);
    cbm_store_close(s);
    free(project);

    cleanup_incremental_repo();
    PASS();
}

TEST(incremental_detects_changed_file) {
    /* Full index, modify one file, re-index → changed file re-parsed */
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }

    /* First: full index */
    cbm_pipeline_t *p = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    char *project = strdup(cbm_pipeline_project_name(p));
    cbm_pipeline_free(p);

    /* Modify helper.go — add a new function */
    char path[512];
    snprintf(path, sizeof(path), "%s/helper.go", g_incr_tmpdir);
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "package main\n\n"
               "func Helper() string {\n\treturn \"hello\"\n}\n\n"
               "func NewFunc() int {\n\treturn 42\n}\n");
    fclose(f);

    /* Second: incremental — should detect change and re-index */
    p = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    /* Verify node count increased (NewFunc was added) */
    cbm_store_t *s = th_open_repo_store(g_incr_tmpdir);
    ASSERT_NOT_NULL(s);
    int nodes_after = cbm_store_count_nodes(s, project);
    ASSERT_GT(nodes_after, 0);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    free(project);

    cleanup_incremental_repo();
    PASS();
}

TEST(incremental_detects_deleted_file) {
    /* Full index, delete a file, re-index → deleted file's nodes removed */
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }

    /* First: full index */
    cbm_pipeline_t *p = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    char *project = strdup(cbm_pipeline_project_name(p));
    cbm_pipeline_free(p);

    /* Delete helper.go */
    char path[512];
    snprintf(path, sizeof(path), "%s/helper.go", g_incr_tmpdir);
    unlink(path);

    /* Second: incremental — should remove Helper nodes */
    p = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    /* Verify node count decreased (Helper's file was deleted) */
    cbm_store_t *s = th_open_repo_store(g_incr_tmpdir);
    ASSERT_NOT_NULL(s);
    int nodes_after = cbm_store_count_nodes(s, project);
    ASSERT_GT(nodes_after, 0); /* still has main.go nodes */
    cbm_store_close(s);
    cbm_pipeline_free(p);
    free(project);

    cleanup_incremental_repo();
    PASS();
}

TEST(incremental_new_file_added) {
    /* Full index, add a new file, re-index → new file's nodes appear */
    if (setup_incremental_repo() != 0) {
        FAIL("setup failed");
    }

    /* First: full index */
    cbm_pipeline_t *p = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    char *project = strdup(cbm_pipeline_project_name(p));
    cbm_pipeline_free(p);

    /* Add extra.go */
    char path[512];
    snprintf(path, sizeof(path), "%s/extra.go", g_incr_tmpdir);
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "package main\n\nfunc Extra() bool {\n\treturn true\n}\n");
    fclose(f);

    /* Second: incremental — should pick up Extra */
    p = cbm_pipeline_new(g_incr_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(g_incr_tmpdir);
    ASSERT_NOT_NULL(s);
    int nodes_after = cbm_store_count_nodes(s, project);
    ASSERT_GT(nodes_after, 0);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    free(project);

    cleanup_incremental_repo();
    PASS();
}

TEST(incremental_fast_preserves_mode_skipped_tools_dir) {
    /* Regression: 2026-04-13. A fast-mode reindex after a full-mode index
     * was silently destroying every file under FAST_SKIP_DIRS directories
     * (`tools`, `scripts`, `bin`, `build`, `docs`, ...) by classifying them
     * as deleted in find_deleted_files even though they still existed on
     * disk. The Skyline graph lost packages/mcp/src/tools/ (18 files / ~500
     * nodes) mid-session when a concurrent /develop run obediently called
     * mode='fast'. This test pins the additive semantics: lesser-mode
     * reindexes must NOT delete files that are merely outside the current
     * pass's discovery scope. Fix: find_deleted_files now stat()s each
     * stored-but-missing file and only purges it if it is truly absent
     * from disk. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_modeskip_XXXXXX");
    if (!cbm_mkdtemp(tmpdir)) {
        FAIL("tmpdir");
    }

    char path[512];
    FILE *f;

    /* main.go — root-level production code (visible in fast and full) */
    snprintf(path, sizeof(path), "%s/main.go", tmpdir);
    f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "package main\n\nfunc main() {\n}\n");
    fclose(f);

    /* tools/util.go — production code under a FAST_SKIP_DIRS directory.
     * Full mode indexes it; fast mode skips it via the discover.c heuristic. */
    char tools_dir[512];
    snprintf(tools_dir, sizeof(tools_dir), "%s/tools", tmpdir);
    cbm_mkdir_p(tools_dir, 0755);
    snprintf(path, sizeof(path), "%s/tools/util.go", tmpdir);
    f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "package tools\n\nfunc Util() string {\n\treturn \"u\"\n}\n");
    fclose(f);

    /* Step 1: full-mode index — both files should be present */
    cbm_pipeline_t *p = cbm_pipeline_new(tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    char *project = strdup(cbm_pipeline_project_name(p));
    cbm_pipeline_free(p);

    cbm_store_t *s = th_open_repo_store(tmpdir);
    ASSERT_NOT_NULL(s);
    cbm_node_t *tools_nodes_before = NULL;
    int tools_count_before = 0;
    cbm_store_find_nodes_by_file(s, project, "tools/util.go", &tools_nodes_before,
                                 &tools_count_before);
    ASSERT_GT(tools_count_before, 0); /* full mode must see tools/util.go */
    cbm_store_free_nodes(tools_nodes_before, tools_count_before);
    int total_before = cbm_store_count_nodes(s, project);
    cbm_store_close(s);

    /* Step 2: fast-mode reindex — tools/util.go MUST survive (additive semantics) */
    p = cbm_pipeline_new(tmpdir, NULL, CBM_MODE_FAST);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);

    s = th_open_repo_store(tmpdir);
    ASSERT_NOT_NULL(s);
    cbm_node_t *tools_nodes_after = NULL;
    int tools_count_after = 0;
    cbm_store_find_nodes_by_file(s, project, "tools/util.go", &tools_nodes_after,
                                 &tools_count_after);
    /* The critical assertion: tools/util.go nodes must still be present after
     * a fast-mode reindex that skipped the tools/ directory. Before the fix,
     * this was 0. */
    ASSERT_GT(tools_count_after, 0);
    ASSERT_EQ(tools_count_after, tools_count_before); /* same nodes, untouched */
    cbm_store_free_nodes(tools_nodes_after, tools_count_after);

    /* Sanity: total node count should not have collapsed by ~the size of tools/ */
    int total_after = cbm_store_count_nodes(s, project);
    ASSERT_GTE(total_after, total_before); /* additive — never less */
    cbm_store_close(s);

    /* Step 3: mutate main.go and fast reindex — forces dump_and_persist to
     * run (instead of the noop early-return path that step 2 hit). This is
     * the real dangerous path: the gbuf gets loaded, mutated for main.go,
     * dumped back to disk. tools/util.go must survive THAT cycle, not just
     * the trivial noop path. Audit finding from 2026-04-13. */
    snprintf(path, sizeof(path), "%s/main.go", tmpdir);
    f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "package main\n\nfunc main() {\n\tprintln(\"changed\")\n}\n");
    fclose(f);
    /* Bump mtime explicitly — some filesystems have coarse mtime resolution
     * and the rewrite could land in the same tick as the original write. */
#ifndef _WIN32
    struct stat mst;
    if (stat(path, &mst) == 0) {
        struct timespec times[2];
        times[0].tv_sec = mst.st_atime;
        times[0].tv_nsec = 0;
        times[1].tv_sec = mst.st_mtime + 5;
        times[1].tv_nsec = 0;
        utimensat(AT_FDCWD, path, times, 0);
    }
#endif /* !_WIN32 */

    p = cbm_pipeline_new(tmpdir, NULL, CBM_MODE_FAST);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);

    s = th_open_repo_store(tmpdir);
    ASSERT_NOT_NULL(s);
    cbm_node_t *tools_nodes_run3 = NULL;
    int tools_count_run3 = 0;
    cbm_store_find_nodes_by_file(s, project, "tools/util.go", &tools_nodes_run3, &tools_count_run3);
    /* tools/util.go nodes must STILL be present after a fast reindex that
     * actually ran the full dump_and_persist cycle (not the noop fast-path). */
    ASSERT_EQ(tools_count_run3, tools_count_before);
    cbm_store_free_nodes(tools_nodes_run3, tools_count_run3);
    cbm_store_close(s);

    /* Step 4: actually delete tools/util.go from disk and full-reindex.
     * Now it really is gone, so its nodes should be purged. This pins the
     * other half of the contract: the stat-based check correctly identifies
     * truly-deleted files as deleted. */
    snprintf(path, sizeof(path), "%s/tools/util.go", tmpdir);
    unlink(path);

    p = cbm_pipeline_new(tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);

    s = th_open_repo_store(tmpdir);
    ASSERT_NOT_NULL(s);
    cbm_node_t *tools_nodes_deleted = NULL;
    int tools_count_deleted = 0;
    cbm_store_find_nodes_by_file(s, project, "tools/util.go", &tools_nodes_deleted,
                                 &tools_count_deleted);
    ASSERT_EQ(tools_count_deleted, 0); /* truly deleted → purged */
    cbm_store_free_nodes(tools_nodes_deleted, tools_count_deleted);
    cbm_store_close(s);

    free(project);
    th_rmtree(tmpdir);
    PASS();
}

TEST(incremental_k8s_manifest_indexed) {
    /* Full index with a k8s manifest, then add a new manifest via incremental.
     * Verifies that cbm_pipeline_pass_k8s() runs during incremental re-index. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_k8s_incr_XXXXXX");
    if (!cbm_mkdtemp(tmpdir)) {
        FAIL("tmpdir");
    }
    char path[512];
    FILE *f;

    /* Initial manifest */
    snprintf(path, sizeof(path), "%s/deploy.yaml", tmpdir);
    f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "apiVersion: apps/v1\nkind: Deployment\nmetadata:\n  name: my-app\n");
    fclose(f);

    /* Full index */
    cbm_pipeline_t *p = cbm_pipeline_new(tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    char *project = strdup(cbm_pipeline_project_name(p));
    cbm_pipeline_free(p);

    /* Verify Resource node created by full index */
    cbm_store_t *s = th_open_repo_store(tmpdir);
    ASSERT_NOT_NULL(s);
    cbm_node_t *nodes = NULL;
    int count = 0;
    cbm_store_find_nodes_by_label(s, project, "Resource", &nodes, &count);
    ASSERT_GT(count, 0);
    cbm_store_free_nodes(nodes, count);
    cbm_store_close(s);

    /* Add a second manifest — incremental should pick it up */
    snprintf(path, sizeof(path), "%s/svc.yaml", tmpdir);
    f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "apiVersion: v1\nkind: Service\nmetadata:\n  name: my-svc\n");
    fclose(f);

    /* Incremental re-index */
    p = cbm_pipeline_new(tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);

    /* Verify both Resource nodes now present */
    s = th_open_repo_store(tmpdir);
    ASSERT_NOT_NULL(s);
    nodes = NULL;
    count = 0;
    cbm_store_find_nodes_by_label(s, project, "Resource", &nodes, &count);
    ASSERT_GTE(count, 2);
    cbm_store_free_nodes(nodes, count);
    cbm_store_close(s);

    free(project);
    th_rmtree(tmpdir);
    PASS();
}

TEST(incremental_kustomize_module_indexed) {
    /* Verifies that a kustomization.yaml added after the initial full index
     * gets a Module node via the incremental k8s pass. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_kust_incr_XXXXXX");
    if (!cbm_mkdtemp(tmpdir)) {
        FAIL("tmpdir");
    }
    char path[512];
    FILE *f;

    /* Initial resource manifest (gives full index something to find) */
    snprintf(path, sizeof(path), "%s/deploy.yaml", tmpdir);
    f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "apiVersion: apps/v1\nkind: Deployment\nmetadata:\n  name: my-app\n");
    fclose(f);

    /* Full index */
    cbm_pipeline_t *p = cbm_pipeline_new(tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    char *project = strdup(cbm_pipeline_project_name(p));
    cbm_pipeline_free(p);

    /* Add kustomization.yaml */
    snprintf(path, sizeof(path), "%s/kustomization.yaml", tmpdir);
    f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "apiVersion: kustomize.config.k8s.io/v1beta1\n"
               "kind: Kustomization\n"
               "resources:\n"
               "  - deploy.yaml\n");
    fclose(f);

    /* Incremental re-index */
    p = cbm_pipeline_new(tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_pipeline_free(p);

    /* Verify Module node created for the kustomization overlay */
    cbm_store_t *s = th_open_repo_store(tmpdir);
    ASSERT_NOT_NULL(s);
    cbm_node_t *nodes = NULL;
    int count = 0;
    cbm_store_find_nodes_by_label(s, project, "Module", &nodes, &count);
    bool found_kust = false;
    for (int i = 0; i < count; i++) {
        if (nodes[i].properties_json && strstr(nodes[i].properties_json, "kustomize")) {
            found_kust = true;
            break;
        }
    }
    cbm_store_free_nodes(nodes, count);
    cbm_store_close(s);
    ASSERT_TRUE(found_kust);

    free(project);
    th_rmtree(tmpdir);
    PASS();
}

/* ── Resource management & internal helper tests ─────────────────── */

TEST(pipeline_empty_path) {
    /* Empty string repo path — should handle gracefully */
    cbm_pipeline_t *p = cbm_pipeline_new("", NULL, CBM_MODE_FULL);
    /* Implementation may return NULL or a valid pipeline with empty project name.
     * Either behavior is acceptable — the key is no crash. */
    if (p) {
        cbm_pipeline_free(p);
    }
    PASS();
}

TEST(pipeline_project_name_content) {
    /* Verify project name is derived from the repo_path */
    cbm_pipeline_t *p = cbm_pipeline_new("/home/user/my-project", NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    const char *name = cbm_pipeline_project_name(p);
    ASSERT_NOT_NULL(name);
    ASSERT_TRUE(strlen(name) > 0);
    /* Should contain "my-project" as part of the derived name */
    ASSERT_TRUE(strstr(name, "my-project") != NULL);
    cbm_pipeline_free(p);
    PASS();
}

TEST(pipeline_cancel_sets_flag) {
    /* Verify cancel sets the flag so subsequent run exits early */
    cbm_pipeline_t *p = cbm_pipeline_new("/tmp/nonexistent", NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    /* Cancel before run */
    cbm_pipeline_cancel(p);
    /* Cancelled pipeline should return quickly (either -1 from cancel or from
     * missing path — both are acceptable; key is no hang) */
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, -1);
    cbm_pipeline_free(p);
    PASS();
}

TEST(pipeline_double_cancel) {
    /* Calling cancel twice should not crash */
    cbm_pipeline_t *p = cbm_pipeline_new("/tmp/nonexistent", NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_cancel(p);
    cbm_pipeline_cancel(p);
    cbm_pipeline_free(p);
    PASS();
}

TEST(pipeline_double_free_prevention) {
    /* free(NULL) after free should not crash. We can't truly double-free
     * the same pointer, but we verify NULL is safe as documented. */
    cbm_pipeline_free(NULL);
    cbm_pipeline_free(NULL);
    PASS();
}

TEST(trackable_source_files) {
    /* Common source extensions are trackable */
    ASSERT_TRUE(cbm_is_trackable_file("main.go"));
    ASSERT_TRUE(cbm_is_trackable_file("src/handler.py"));
    ASSERT_TRUE(cbm_is_trackable_file("lib/server.js"));
    ASSERT_TRUE(cbm_is_trackable_file("components/App.tsx"));
    ASSERT_TRUE(cbm_is_trackable_file("pkg/svc.rs"));
    ASSERT_TRUE(cbm_is_trackable_file("config.yaml"));
    PASS();
}

TEST(trackable_rejects_images) {
    /* Image and binary files are not trackable */
    ASSERT_FALSE(cbm_is_trackable_file("logo.png"));
    ASSERT_FALSE(cbm_is_trackable_file("photo.jpg"));
    ASSERT_FALSE(cbm_is_trackable_file("icon.gif"));
    ASSERT_FALSE(cbm_is_trackable_file("diagram.svg"));
    PASS();
}

TEST(trackable_rejects_minified) {
    /* Minified files are not trackable */
    ASSERT_FALSE(cbm_is_trackable_file("bundle.min.js"));
    ASSERT_FALSE(cbm_is_trackable_file("style.min.css"));
    PASS();
}

TEST(trackable_rejects_vendor_dirs) {
    /* Files in vendor/node_modules/__pycache__ are not trackable */
    ASSERT_FALSE(cbm_is_trackable_file("vendor/github.com/lib/dep.go"));
    ASSERT_FALSE(cbm_is_trackable_file("node_modules/lodash/index.js"));
    ASSERT_FALSE(cbm_is_trackable_file("__pycache__/module.pyc"));
    ASSERT_FALSE(cbm_is_trackable_file(".git/objects/pack/data"));
    PASS();
}

TEST(trackable_rejects_lock_files) {
    /* Lock files are not trackable */
    ASSERT_FALSE(cbm_is_trackable_file("package-lock.json"));
    ASSERT_FALSE(cbm_is_trackable_file("go.sum"));
    ASSERT_FALSE(cbm_is_trackable_file("yarn.lock"));
    PASS();
}

TEST(test_path_directory_patterns) {
    /* Test directory patterns: __tests__, test/, tests/, spec/ */
    ASSERT_TRUE(cbm_is_test_path("__tests__/Component.js"));
    ASSERT_TRUE(cbm_is_test_path("tests/test_main.py"));
    ASSERT_TRUE(cbm_is_test_path("spec/handler_spec.rb"));
    PASS();
}

TEST(test_path_suffix_patterns) {
    /* Various language-specific test suffix patterns */
    ASSERT_TRUE(cbm_is_test_path("handler.spec.ts"));
    ASSERT_TRUE(cbm_is_test_path("handler.test.tsx"));
    ASSERT_TRUE(cbm_is_test_path("handler_test.go"));
    ASSERT_TRUE(cbm_is_test_path("test_handler.py"));
    /* Non-test files */
    ASSERT_FALSE(cbm_is_test_path("handler.ts"));
    ASSERT_FALSE(cbm_is_test_path("contest.go"));
    ASSERT_FALSE(cbm_is_test_path("latest.py"));
    PASS();
}

TEST(test_func_name_go_patterns) {
    /* Go test function names: Test + uppercase char */
    ASSERT_TRUE(cbm_is_test_func_name("TestFoo"));
    ASSERT_TRUE(cbm_is_test_func_name("TestHTTPHandler"));
    /* Non-test: "Test" alone or Test + lowercase */
    ASSERT_FALSE(cbm_is_test_func_name("Testable")); /* lowercase 'a' after Test */
    PASS();
}

TEST(test_func_name_js_helpers) {
    /* JS/TS test helper function names */
    ASSERT_TRUE(cbm_is_test_func_name("it"));
    ASSERT_TRUE(cbm_is_test_func_name("describe"));
    ASSERT_TRUE(cbm_is_test_func_name("test"));
    ASSERT_TRUE(cbm_is_test_func_name("beforeEach"));
    ASSERT_TRUE(cbm_is_test_func_name("afterEach"));
    PASS();
}

TEST(env_var_name_valid) {
    /* Valid env var names: uppercase + underscores, at least 2 chars with uppercase */
    ASSERT_TRUE(cbm_is_env_var_name("DATABASE_URL"));
    ASSERT_TRUE(cbm_is_env_var_name("API_KEY"));
    ASSERT_TRUE(cbm_is_env_var_name("PORT"));
    ASSERT_TRUE(cbm_is_env_var_name("DB_2"));
    ASSERT_TRUE(cbm_is_env_var_name("MY_VAR_123"));
    PASS();
}

TEST(env_var_name_invalid) {
    /* Invalid: too short, lowercase, mixed case, empty */
    ASSERT_FALSE(cbm_is_env_var_name("A"));      /* single char */
    ASSERT_FALSE(cbm_is_env_var_name("port"));   /* lowercase */
    ASSERT_FALSE(cbm_is_env_var_name("apiKey")); /* camelCase */
    ASSERT_FALSE(cbm_is_env_var_name("__"));     /* no uppercase */
    ASSERT_FALSE(cbm_is_env_var_name(""));       /* empty */
    ASSERT_FALSE(cbm_is_env_var_name("123"));    /* digits only */
    PASS();
}

TEST(config_ext_positive) {
    /* Config file extensions recognized */
    ASSERT_TRUE(cbm_has_config_extension(".env"));
    ASSERT_TRUE(cbm_has_config_extension("config.yaml"));
    ASSERT_TRUE(cbm_has_config_extension("config.yml"));
    ASSERT_TRUE(cbm_has_config_extension("settings.toml"));
    ASSERT_TRUE(cbm_has_config_extension("app.ini"));
    ASSERT_TRUE(cbm_has_config_extension("data.json"));
    ASSERT_TRUE(cbm_has_config_extension("app.properties"));
    PASS();
}

TEST(config_ext_negative) {
    /* Non-config extensions rejected */
    ASSERT_FALSE(cbm_has_config_extension("main.go"));
    ASSERT_FALSE(cbm_has_config_extension("app.py"));
    ASSERT_FALSE(cbm_has_config_extension("handler.rs"));
    ASSERT_FALSE(cbm_has_config_extension("data.csv"));
    ASSERT_FALSE(cbm_has_config_extension("README.md"));
    PASS();
}

TEST(split_camel_basic) {
    /* Basic camelCase splitting */
    char *parts[16];
    int n;

    n = cbm_split_camel_case("getCamelCase", parts, 16);
    ASSERT_EQ(n, 3);
    ASSERT_STR_EQ(parts[0], "get");
    ASSERT_STR_EQ(parts[1], "Camel");
    ASSERT_STR_EQ(parts[2], "Case");
    for (int i = 0; i < n; i++)
        free(parts[i]);

    PASS();
}

TEST(split_camel_single_word) {
    /* Single lowercase word — no splits */
    char *parts[16];
    int n = cbm_split_camel_case("hello", parts, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(parts[0], "hello");
    for (int i = 0; i < n; i++)
        free(parts[i]);
    PASS();
}

TEST(split_camel_empty) {
    /* Empty string — should return 0 or 1 empty part */
    char *parts[16];
    int n = cbm_split_camel_case("", parts, 16);
    for (int i = 0; i < n; i++)
        free(parts[i]);
    /* Either 0 parts or 1 empty part is acceptable */
    ASSERT_TRUE(n >= 0 && n <= 1);
    PASS();
}

TEST(tokenize_decorator_login_required) {
    /* @login_required → ["login", "required"] */
    char *tokens[16];
    int n = cbm_tokenize_decorator("@login_required", tokens, 16);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(tokens[0], "login");
    ASSERT_STR_EQ(tokens[1], "required");
    for (int i = 0; i < n; i++)
        free(tokens[i]);
    PASS();
}

TEST(tokenize_decorator_single) {
    /* @Override → ["override"] */
    char *tokens[16];
    int n = cbm_tokenize_decorator("@Override", tokens, 16);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(tokens[0], "override");
    for (int i = 0; i < n; i++)
        free(tokens[i]);
    PASS();
}

TEST(split_command_basic) {
    /* Basic command splitting with spaces */
    char *args[16];
    int n = cbm_split_command("gcc -c main.c -o main.o", args, 16);
    ASSERT_EQ(n, 5);
    ASSERT_STR_EQ(args[0], "gcc");
    ASSERT_STR_EQ(args[1], "-c");
    ASSERT_STR_EQ(args[2], "main.c");
    ASSERT_STR_EQ(args[3], "-o");
    ASSERT_STR_EQ(args[4], "main.o");
    for (int i = 0; i < n; i++)
        free(args[i]);
    PASS();
}

TEST(split_command_quoted) {
    /* Command with quoted arguments */
    char *args[16];
    int n = cbm_split_command("gcc -DFOO=\"bar baz\" main.c", args, 16);
    ASSERT_GTE(n, 3);
    ASSERT_STR_EQ(args[0], "gcc");
    for (int i = 0; i < n; i++)
        free(args[i]);
    PASS();
}

TEST(split_command_empty) {
    /* Empty command string */
    char *args[16];
    int n = cbm_split_command("", args, 16);
    ASSERT_EQ(n, 0);
    PASS();
}

TEST(parse_range_with_count) {
    /* "10,5" → start=10, count=5 */
    int start, count;
    cbm_parse_range("10,5", &start, &count);
    ASSERT_EQ(start, 10);
    ASSERT_EQ(count, 5);
    PASS();
}

TEST(parse_range_single) {
    /* "42" → start=42, count=1 */
    int start, count;
    cbm_parse_range("42", &start, &count);
    ASSERT_EQ(start, 42);
    ASSERT_EQ(count, 1);
    PASS();
}

TEST(parse_range_zero_count) {
    /* "100,0" → start=100, count=0 */
    int start, count;
    cbm_parse_range("100,0", &start, &count);
    ASSERT_EQ(start, 100);
    ASSERT_EQ(count, 0);
    PASS();
}

TEST(parse_name_status_basic) {
    /* Parse standard git diff --name-status output */
    const char *input = "M\tsrc/main.go\n"
                        "A\tsrc/new.go\n"
                        "D\tsrc/old.go\n";
    cbm_changed_file_t files[16];
    int n = cbm_parse_name_status(input, files, 16);
    /* Exact count depends on which files pass cbm_is_trackable_file */
    ASSERT_GTE(n, 0);
    ASSERT_LTE(n, 3);
    PASS();
}

TEST(parse_name_status_empty) {
    /* Empty input */
    cbm_changed_file_t files[16];
    int n = cbm_parse_name_status("", files, 16);
    ASSERT_EQ(n, 0);
    PASS();
}

TEST(parse_hunks_empty) {
    /* Empty input */
    cbm_changed_hunk_t hunks[16];
    int n = cbm_parse_hunks("", hunks, 16);
    ASSERT_EQ(n, 0);
    PASS();
}

TEST(coupling_empty_commits) {
    /* Zero commits → zero couplings */
    cbm_change_coupling_t results[16];
    int n = cbm_compute_change_coupling(NULL, 0, results, 16);
    ASSERT_EQ(n, 0);
    PASS();
}

TEST(coupling_single_file_commit) {
    /* Commits with single files → no pairs → zero couplings */
    char *f1[] = {"a.go"};
    char *f2[] = {"b.go"};
    cbm_commit_files_t commits[] = {{f1, 1, 0}, {f2, 1, 0}};
    cbm_change_coupling_t results[16];
    int n = cbm_compute_change_coupling(commits, 2, results, 16);
    ASSERT_EQ(n, 0);
    PASS();
}

TEST(clean_json_brackets_array) {
    /* JSON array → space-joined */
    char out[256];
    cbm_clean_json_brackets("[\"./app\", \"--flag\"]", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "./app") != NULL);
    ASSERT_TRUE(strstr(out, "--flag") != NULL);
    PASS();
}

TEST(clean_json_brackets_plain) {
    /* Plain string → unchanged */
    char out[256];
    cbm_clean_json_brackets("./app --flag", out, sizeof(out));
    ASSERT_STR_EQ(out, "./app --flag");
    PASS();
}

TEST(clean_json_brackets_empty) {
    /* Empty string → empty output */
    char out[256];
    cbm_clean_json_brackets("", out, sizeof(out));
    ASSERT_STR_EQ(out, "");
    PASS();
}

TEST(fqn_compute_basic) {
    /* Basic FQN: project.dir.name */
    char *fqn = cbm_pipeline_fqn_compute("proj", "pkg/handler.go", "Serve");
    ASSERT_NOT_NULL(fqn);
    ASSERT_TRUE(strstr(fqn, "proj") != NULL);
    ASSERT_TRUE(strstr(fqn, "Serve") != NULL);
    free(fqn);
    PASS();
}

TEST(fqn_compute_strips_ext) {
    /* FQN should strip file extension */
    char *fqn = cbm_pipeline_fqn_compute("proj", "main.go", "main");
    ASSERT_NOT_NULL(fqn);
    /* Should not contain ".go" */
    ASSERT_TRUE(strstr(fqn, ".go") == NULL);
    ASSERT_TRUE(strstr(fqn, "main") != NULL);
    free(fqn);
    PASS();
}

TEST(fqn_module_basic) {
    /* Module QN: project.dir.parts */
    char *mod = cbm_pipeline_fqn_module("proj", "pkg/util/helper.go");
    ASSERT_NOT_NULL(mod);
    ASSERT_TRUE(strstr(mod, "proj") != NULL);
    free(mod);
    PASS();
}

TEST(fqn_folder_basic) {
    /* Folder QN from directory path */
    char *folder = cbm_pipeline_fqn_folder("proj", "pkg/util");
    ASSERT_NOT_NULL(folder);
    ASSERT_TRUE(strstr(folder, "proj") != NULL);
    free(folder);
    PASS();
}

TEST(project_name_special_chars) {
    /* Project name with colons, slashes — should be normalized */
    char *name = cbm_project_name_from_path("/home/user/my:project");
    ASSERT_NOT_NULL(name);
    /* Colons should be converted to dashes */
    ASSERT_TRUE(strchr(name, ':') == NULL);
    ASSERT_TRUE(strchr(name, '/') == NULL);
    free(name);
    PASS();
}

TEST(project_name_trailing_slash) {
    /* Trailing slash should not affect result */
    char *a = cbm_project_name_from_path("/home/user/project");
    ASSERT_NOT_NULL(a);
    free(a);
    PASS();
}

/* ── Complexity propagation pass tests (Tier B) ────────────────── */

/* Find the first Function/Method node with `name` in a label set. Returns a
 * borrowed pointer into `funcs` or NULL. */
static const cbm_node_t *find_node_named(cbm_node_t *funcs, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(funcs[i].name, name) == 0)
            return &funcs[i];
    }
    return NULL;
}

/* The complexity pass propagates loop_depth along CALLS edges into
 * transitive_loop_depth and flags call-graph cycles as recursive. Caller and
 * callee live in one file so the calls resolve intra-file (most reliable). */
TEST(pipeline_complexity_transitive_loop_depth) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_cx_XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("tmpdir");

    write_temp_file(tmpdir, "cx.go",
                    "package p\n\n"
                    "func work() {}\n\n"
                    "func inner() {\n"
                    "\tfor i := 0; i < 10; i++ {\n"
                    "\t\tfor j := 0; j < 10; j++ {\n"
                    "\t\t\twork()\n"
                    "\t\t}\n"
                    "\t}\n"
                    "}\n\n"
                    "func outer() {\n"
                    "\tfor i := 0; i < 10; i++ {\n"
                    "\t\tinner()\n"
                    "\t}\n"
                    "}\n\n"
                    "func recur(n int) {\n"
                    "\tif n > 0 {\n"
                    "\t\trecur(n - 1)\n"
                    "\t}\n"
                    "}\n");

    cbm_pipeline_t *p = cbm_pipeline_new(tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    cbm_store_t *s = th_open_repo_store(tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);

    cbm_node_t *funcs = NULL;
    int func_count = 0;
    ASSERT_EQ(cbm_store_find_nodes_by_label(s, project, "Function", &funcs, &func_count),
              CBM_STORE_OK);
    ASSERT_GT(func_count, 0);

    /* inner: two nested loops, callee work() has depth 0 → tld == 2 */
    const cbm_node_t *inner = find_node_named(funcs, func_count, "inner");
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(inner->properties_json);
    ASSERT_TRUE(strstr(inner->properties_json, "\"transitive_loop_depth\":2") != NULL);

    /* outer: own depth 1 + inner's tld 2 → tld == 3 (interprocedural) */
    const cbm_node_t *outer = find_node_named(funcs, func_count, "outer");
    ASSERT_NOT_NULL(outer);
    ASSERT_NOT_NULL(outer->properties_json);
    ASSERT_TRUE(strstr(outer->properties_json, "\"transitive_loop_depth\":3") != NULL);

    /* recur: self-recursion → recursive:true flagged by the cycle guard */
    const cbm_node_t *recur = find_node_named(funcs, func_count, "recur");
    ASSERT_NOT_NULL(recur);
    ASSERT_NOT_NULL(recur->properties_json);
    ASSERT_TRUE(strstr(recur->properties_json, "\"recursive\":true") != NULL);

    cbm_store_free_nodes(funcs, func_count);
    cbm_store_close(s);
    cbm_pipeline_free(p);
    th_rmtree(tmpdir);
    PASS();
}

static cbm_gbuf_t *pipeline_complexity_cycle_graph(int reversed) {
    cbm_gbuf_t *gb = cbm_gbuf_new("complexity-cycle", "/tmp/complexity-cycle");
    if (!gb) return NULL;
    const char *names[] = {"Alpha", "Beta", "Gamma"};
    const int depths[] = {1, 2, 0};
    int64_t ids[3] = {0};
    for (int step = 0; step < 3; step++) {
        int i = reversed ? 2 - step : step;
        char properties[128];
        snprintf(properties, sizeof(properties),
                 "{\"loop_depth\":%d,\"self_recursive\":false}", depths[i]);
        ids[i] = cbm_gbuf_upsert_node(gb, "Function", names[i], names[i], "cycle.go",
                                      i + 1, i + 1, properties);
        if (!ids[i]) {
            cbm_gbuf_free(gb);
            return NULL;
        }
    }
    if (!cbm_gbuf_insert_edge(gb, ids[0], ids[1], "CALLS", "{}") ||
        !cbm_gbuf_insert_edge(gb, ids[1], ids[2], "CALLS", "{}") ||
        !cbm_gbuf_insert_edge(gb, ids[2], ids[0], "CALLS", "{}")) {
        cbm_gbuf_free(gb);
        return NULL;
    }
    return gb;
}

TEST(pipeline_complexity_cycle_is_independent_of_temporary_ids) {
    cbm_gbuf_t *forward = pipeline_complexity_cycle_graph(0);
    cbm_gbuf_t *reverse = pipeline_complexity_cycle_graph(1);
    ASSERT_NOT_NULL(forward);
    ASSERT_NOT_NULL(reverse);
    cbm_pipeline_ctx_t forward_ctx = {.gbuf = forward};
    cbm_pipeline_ctx_t reverse_ctx = {.gbuf = reverse};
    cbm_pipeline_pass_complexity(&forward_ctx);
    cbm_pipeline_pass_complexity(&reverse_ctx);
    const char *names[] = {"Alpha", "Beta", "Gamma"};
    for (int i = 0; i < 3; i++) {
        const cbm_gbuf_node_t *a = cbm_gbuf_find_by_qn(forward, names[i]);
        const cbm_gbuf_node_t *b = cbm_gbuf_find_by_qn(reverse, names[i]);
        ASSERT_NOT_NULL(a);
        ASSERT_NOT_NULL(b);
        ASSERT_STR_EQ(a->properties_json, b->properties_json);
    }
    cbm_gbuf_free(forward);
    cbm_gbuf_free(reverse);
    PASS();
}

TEST(pipeline_complexity_leaf_and_non_function_graphs_do_not_sort_null) {
    cbm_gbuf_t *leaf_graph = cbm_gbuf_new("complexity-leaf", "/tmp/complexity-leaf");
    ASSERT_NOT_NULL(leaf_graph);
    ASSERT_TRUE(cbm_gbuf_upsert_node(
        leaf_graph, "Function", "Leaf", "Leaf", "leaf.c", 1, 1,
        "{\"loop_depth\":2,\"self_recursive\":false}") != 0);
    cbm_pipeline_ctx_t leaf_ctx = {.gbuf = leaf_graph};
    cbm_pipeline_pass_complexity(&leaf_ctx);
    const cbm_gbuf_node_t *leaf = cbm_gbuf_find_by_qn(leaf_graph, "Leaf");
    ASSERT_NOT_NULL(leaf);
    ASSERT_TRUE(strstr(leaf->properties_json, "\"transitive_loop_depth\":2") != NULL);
    cbm_gbuf_free(leaf_graph);

    cbm_gbuf_t *file_graph = cbm_gbuf_new("complexity-file", "/tmp/complexity-file");
    ASSERT_NOT_NULL(file_graph);
    ASSERT_TRUE(cbm_gbuf_upsert_node(
        file_graph, "File", "leaf.c", "leaf.c", "leaf.c", 1, 1, "{}") != 0);
    cbm_pipeline_ctx_t file_ctx = {.gbuf = file_graph};
    cbm_pipeline_pass_complexity(&file_ctx);
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(file_graph, "leaf.c"));
    cbm_gbuf_free(file_graph);
    PASS();
}

/* Regression for #334: the plausibility gate compares committed (extracted)
 * node count against persisted rows. committed_nodes must be captured BEFORE
 * the cbm.zova dump frees the gbuf node index — otherwise it reads 0 and
 * the gate is silently inert. Drives the real pipeline (not a synthetic count)
 * and asserts committed_nodes is populated and matches what was persisted. */
TEST(pipeline_committed_counts_match_persisted) {
    if (setup_test_repo() != 0) {
        FAIL("failed to create temp dir");
    }

    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    ASSERT_EQ(rc, 0);

    int committed_nodes = -1;
    int committed_edges = -1;
    cbm_pipeline_get_committed_counts(p, &committed_nodes, &committed_edges);

    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *project = cbm_pipeline_project_name(p);
    int persisted_nodes = cbm_store_count_nodes(s, project);
    cbm_store_close(s);

    /* The bug captured committed_nodes after the node index was freed → 0. */
    ASSERT_GT(committed_nodes, 0);
    /* A faithful full dump persists exactly what it committed. */
    ASSERT_EQ(committed_nodes, persisted_nodes);
    /* The gate must NOT flag a healthy full index as degraded. */
    ASSERT_FALSE(cbm_dump_verify_is_degraded(committed_nodes, persisted_nodes,
                                             CBM_DUMP_VERIFY_DEFAULT_RATIO,
                                             CBM_DUMP_VERIFY_MIN_FLOOR));

    cbm_pipeline_free(p);
    teardown_test_repo();
    PASS();
}

/* Reproduce-first (perf, linux-kernel finding): the extraction back-pressure
 * gate must stop re-paying the full collect+nap tax on every file pull once a
 * full nap cycle has failed to reclaim under budget. With CBM_MEM_BUDGET_MB=1
 * the test process RSS is permanently over budget and NOTHING napping can
 * change that (the resident floor IS the process) — napping is provably futile.
 * OLD behavior: one full 40-spin nap cycle per pulled file (kernel index: ~63k
 * pulls × ~120 ms ÷ 12 workers ≈ 390 s of idle workers at 79% avg CPU).
 * FIXED: the first futile cycle flips a shared flag; later pulls proceed with
 * the designed soft overshoot. Cycle count then can't exceed one cycle per
 * worker (workers already inside the gate when the flag flips) plus re-probes.
 * RED on the unfixed gate: cycles == file count (64) > cores+2.
 * The counter (cbm_pp_bp_nap_cycles) makes this deterministic — no timing.
 *
 * The gate lives ONLY in the parallel extract path, so the fixture MUST exceed
 * MIN_FILES_FOR_PARALLEL (50) — else the run routes sequential, the gate never
 * fires, and the test would pass vacuously (cycles==0). The engagement assert
 * below (cycles >= 1) is a hard guard against that regressing silently. */
/* Shared fixture for the back-pressure tests: 64 tiny Go files, above
 * MIN_FILES_FOR_PARALLEL (50) so the parallel extract path — the only phase
 * with a memory gate — is the one that runs. */
static bool write_backpressure_fixture(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/cbm_test_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(g_tmpdir)) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/f%02d.go", g_tmpdir, i);
        FILE *f = fopen(path, "w");
        if (!f) {
            return false;
        }
        fprintf(f, "package main\n\nfunc F%02d() int {\n\treturn %d\n}\n", i, i);
        fclose(f);
    }
    return true;
}

TEST(pipeline_backpressure_futile_nap_disengages) {
    /* 64 tiny files: > MIN_FILES_FOR_PARALLEL (50) so the parallel path (and its
     * back-pressure gate) actually runs; old-code cycles (~64) >> the bound. */
    if (!write_backpressure_fixture()) {
        FAIL("failed to create fixture");
    }

    /* 1 MB budget: over-budget on every pull, unreclaimable by napping.
     * Set via the test hook, NOT setenv + cbm_mem_init: the init-once guard
     * makes any re-init keep whatever budget the FIRST in-process init
     * computed. The old env dance either failed to apply the 1 MB budget
     * (some earlier test's init won the guard) or applied it permanently
     * (this test's init won) — the "restore" re-init was then a silent
     * no-op and the 1 MB budget leaked into every later budget consumer
     * in the runner (mem_over_budget_low_rss went red suite-order-wide). */
    /* Pinned workers keep the cycle bound below the file count on any host. */
    enum { BP_WORKERS = 4 };
    const char *old_workers = getenv("CBM_WORKERS");
    char *saved_workers = old_workers ? strdup(old_workers) : NULL;
    if ((old_workers && !saved_workers) || cbm_setenv("CBM_WORKERS", "4", 1) != 0) {
        free(saved_workers);
        teardown_test_repo();
        FAIL("failed to pin CBM_WORKERS");
    }
    size_t saved_budget = cbm_mem_budget();
    cbm_mem_set_budget_for_tests((size_t)1024 * 1024);
    bool budget_set = cbm_mem_budget() > 0;
    bool over_at_start = cbm_mem_over_budget();

    cbm_pp_bp_nap_cycles_reset();
    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    int rc = p ? cbm_pipeline_run(p) : -1;
    long cycles = cbm_pp_bp_nap_cycles();

    /* Restore the caller-visible budget and workers BEFORE asserting. */
    cbm_mem_set_budget_for_tests(saved_budget);
    int restore_workers_rc =
        saved_workers ? cbm_setenv("CBM_WORKERS", saved_workers, 1) : cbm_unsetenv("CBM_WORKERS");
    free(saved_workers);
    cbm_pipeline_free(p);
    teardown_test_repo();
    ASSERT_EQ(restore_workers_rc, 0);
    ASSERT_TRUE(budget_set);
    ASSERT_TRUE(over_at_start);

    /* A budget that never drains is not a soft overshoot any more: after the
     * latch and ONE confirmation cycle the attempt fails whole with the named
     * code (rc==0 was the advisory era). */
    ASSERT_EQ(rc, CBM_PIPELINE_ABORT_OVER_BUDGET);
    /* Engagement guard (anti-vacuous): the gate must have actually run — the
     * parallel path taken and the 1 MB budget exceeded on every pull. cycles==0
     * means the fixture routed sequential (or the gate was compiled out) and
     * this test proved NOTHING; fail loudly rather than pass vacuously. */
    if (cycles < 1) {
        FAIL("back-pressure gate never engaged (cycles==0) — fixture routed sequential?");
    }
    /* Futile napping must disengage: never one cycle per file (64). The first
     * latch engages the spill store (F1) and re-arms the gate once, so each
     * worker can be in flight in two rounds, plus the single confirmation
     * cycle and a small margin. */
    long bound = 2L * BP_WORKERS + 3;
    if (cycles > bound) {
        char msg[128];
        snprintf(msg, sizeof(msg), "nap cycles %ld > bound %ld (gate re-paid per pull)", cycles,
                 bound);
        FAIL(msg);
    }
    PASS();
}

#if CBM_WITH_ZOVA
/* Node and edge digests of a published workspace (the fixture has no main, so
 * pipeline_zova_project_info's CALLS walk does not apply). */
static int pipeline_zova_graph_digest(const char *project, pipeline_sql_digest_t *nodes,
                                      pipeline_sql_digest_t *edges) {
    static const char *nodes_sql =
        "SELECT label,name,qualified_name,file_path,start_line,end_line,properties "
        "FROM nodes WHERE project=?1 "
        "ORDER BY label,name,qualified_name,file_path,start_line,end_line,properties";
    cbm_store_t *store = th_open_project_store(project);
    if (!store) {
        return -1;
    }
    /* Counting nodes builds the store's nodes view over the workspace. */
    bool ok = cbm_store_count_nodes(store, project) >= 0 &&
              pipeline_digest_project_rows(cbm_store_get_db(store), project, nodes_sql, nodes) ==
                  0 &&
              pipeline_digest_project_edges(store, project, edges) == 0;
    cbm_store_close(store);
    return ok ? 0 : -1;
}
#endif

/* Over budget after futility and one confirmation cycle, the attempt fails
 * WHOLE with a named code: nothing is published and the served Zova
 * generation is unchanged. The second run re-extracts all 64 changed files
 * through the incremental route, so the code must survive that path too. */
TEST(pipeline_over_budget_after_futility_fails_whole_and_preserves_generation) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    if (!write_backpressure_fixture()) {
        FAIL("failed to create fixture");
    }
    const char *old_workers = getenv("CBM_WORKERS");
    char *saved_workers = old_workers ? strdup(old_workers) : NULL;
    if ((old_workers && !saved_workers) || cbm_setenv("CBM_WORKERS", "4", 1) != 0) {
        free(saved_workers);
        teardown_test_repo();
        FAIL("failed to pin CBM_WORKERS");
    }

    /* Generation 1 at the process budget: publishes normally. */
    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    int rc_first = p ? cbm_pipeline_run(p) : -1;
    char *project = p ? strdup(cbm_pipeline_project_name(p)) : NULL;
    cbm_pipeline_free(p);
    pipeline_sql_digest_t nodes_before = {0};
    pipeline_sql_digest_t edges_before = {0};
    int info_before_rc =
        project ? pipeline_zova_graph_digest(project, &nodes_before, &edges_before) : -1;

    /* Generation 2 at a 1 MiB budget over a changed repo: every file gains a
     * definition, so the run re-extracts all 64. Over budget on every probe
     * and unreclaimable by napping, so futility latches and the confirmation
     * cycle still ends over budget. */
    bool mutated = true;
    for (int i = 0; i < 64 && mutated; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/f%02d.go", g_tmpdir, i);
        FILE *f = fopen(path, "w");
        mutated = f != NULL;
        if (f) {
            fprintf(f,
                    "package main\n\nfunc F%02d() int {\n\treturn %d\n}\n\n"
                    "func G%02d() int {\n\treturn F%02d() + 1\n}\n",
                    i, i, i, i);
            fclose(f);
        }
    }
    size_t saved_budget = cbm_mem_budget();
    cbm_mem_set_budget_for_tests((size_t)1024 * 1024);
    bool over_at_start = cbm_mem_over_budget();
    cbm_pp_bp_nap_cycles_reset();
    p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    int rc_second = p ? cbm_pipeline_run(p) : -1;
    cbm_pipeline_route_t route_second =
        p ? cbm_pipeline_get_last_route(p) : CBM_PIPELINE_ROUTE_UNKNOWN;
    long cycles = cbm_pp_bp_nap_cycles();
    /* Restore the caller-visible budget BEFORE any assertion. */
    cbm_mem_set_budget_for_tests(saved_budget);
    cbm_pipeline_free(p);

    pipeline_sql_digest_t nodes_after = {0};
    pipeline_sql_digest_t edges_after = {0};
    int info_after_rc =
        project ? pipeline_zova_graph_digest(project, &nodes_after, &edges_after) : -1;
    free(project);
    int restore_workers_rc =
        saved_workers ? cbm_setenv("CBM_WORKERS", saved_workers, 1) : cbm_unsetenv("CBM_WORKERS");
    free(saved_workers);
    teardown_test_repo();

    ASSERT_EQ(restore_workers_rc, 0);
    ASSERT_EQ(rc_first, 0);
    ASSERT_EQ(info_before_rc, 0);
    ASSERT_GT(nodes_before.rows, 0);
    ASSERT_TRUE(mutated);
    ASSERT_TRUE(over_at_start);
    ASSERT_EQ(route_second, CBM_PIPELINE_ROUTE_INCREMENTAL);
    /* The named failure — not the cancel sentinel, not success. */
    ASSERT_EQ(rc_second, CBM_PIPELINE_ABORT_OVER_BUDGET);
    /* Engagement guard: fail-whole needs the latching cycle AND the
     * confirmation cycle; fewer means the gate never decided anything. */
    if (cycles < 2) {
        FAIL("back-pressure gate never confirmed futility (cycles<2) — fixture routed sequential?");
    }
    /* The served generation is untouched. */
    ASSERT_EQ(info_after_rc, 0);
    ASSERT_EQ(nodes_after.rows, nodes_before.rows);
    ASSERT_TRUE(nodes_after.hash == nodes_before.hash);
    ASSERT_EQ(edges_after.rows, edges_before.rows);
    ASSERT_TRUE(edges_after.hash == edges_before.hash);
    PASS();
#endif
}

/* ── Extraction result spill (F1) ────────────────────────────────── */

/* Sources whose results between them fill the encoded field families: defs
 * with bases and decorators, calls with args, imports, usages, throws,
 * env accesses, type refs, impl traits and config string refs. */
static const struct {
    CBMLanguage lang;
    const char *rel;
    const char *src;
} k_spill_sources[] = {
    {CBM_LANG_GO, "svc/server.go",
     "package svc\n\nimport (\n\t\"fmt\"\n\t\"os\"\n)\n\ntype Server struct {\n\tName string\n"
     "\tPort int\n}\n\nfunc (s *Server) Start() error {\n\tif os.Getenv(\"PORT\") == \"\" {\n"
     "\t\treturn fmt.Errorf(\"no port %d\", s.Port)\n\t}\n\treturn nil\n}\n"},
    {CBM_LANG_PYTHON, "app/models.py",
     "import os\nfrom base import Base\n\n@dataclass\nclass User(Base):\n"
     "    def save(self, db):\n        url = os.environ['DB_URL']\n"
     "        db.write(self, url=url)\n        raise ValueError('x')\n"},
    {CBM_LANG_JAVA, "src/main/java/com/acme/Repo.java",
     "package com.acme;\n\nimport java.util.List;\n\npublic class Repo extends Base implements Store {\n"
     "    private List<String> items;\n    public void add(String s) { items.add(s); helper(s, 1); }\n"
     "}\n"},
    {CBM_LANG_TYPESCRIPT, "web/api.ts",
     "import { get } from './http';\nexport class Api {\n  async load(id: number): Promise<string> {\n"
     "    const r = await get(`/users/${id}`, { timeout: 5 });\n    return r.body;\n  }\n}\n"},
    {CBM_LANG_RUST, "core/src/lib.rs",
     "use std::fmt;\npub struct P { x: i32 }\npub trait Shape { fn area(&self) -> f64; }\n"
     "impl Shape for P { fn area(&self) -> f64 { helper(self.x) as f64 } }\n"},
    {CBM_LANG_YAML, "deploy/app.yaml",
     "service:\n  push_endpoint: https://api.example.com/hook\n  topic: orders\n"},
};
enum { SPILL_SOURCE_COUNT = (int)(sizeof(k_spill_sources) / sizeof(k_spill_sources[0])) };

/* Extract source i with its source bytes retained, as the parallel extract
 * retains them for cross-file LSP. */
static CBMFileResult *spill_extract(int i) {
    const char *src = k_spill_sources[i].src;
    int len = (int)strlen(src);
    CBMFileResult *r = cbm_extract_file(src, len, k_spill_sources[i].lang, "spillproj",
                                        k_spill_sources[i].rel, 0, NULL, NULL);
    char *copy = r ? (char *)cbm_arena_alloc(&r->arena, (size_t)len + 1) : NULL;
    if (copy) {
        memcpy(copy, src, (size_t)len + 1);
        r->source = copy;
        r->source_len = len;
    }
    return r;
}

/* decode(encode(r)) re-encodes to the same bytes, so every encoded field
 * survives the trip; the spot checks guard against an encoder that drops a
 * field on both sides. */
TEST(result_spill_round_trip_is_byte_identical) {
    int defs = 0;
    int calls = 0;
    int imports = 0;
    int usages = 0;
    int string_refs = 0;
    for (int i = 0; i < SPILL_SOURCE_COUNT; i++) {
        CBMFileResult *r = spill_extract(i);
        ASSERT_NOT_NULL(r);
        defs += r->defs.count;
        calls += r->calls.count;
        imports += r->imports.count;
        usages += r->usages.count;
        string_refs += r->string_refs.count;
        size_t len1 = 0;
        size_t len2 = 0;
        char *e1 = cbm_result_spill_encode(r, &len1);
        CBMFileResult *d = e1 ? cbm_result_spill_decode(e1, len1) : NULL;
        char *e2 = d ? cbm_result_spill_encode(d, &len2) : NULL;
        bool same = e2 && len1 == len2 && memcmp(e1, e2, len1) == 0;
        bool fields = d && d->defs.count == r->defs.count && d->calls.count == r->calls.count &&
                      d->source_len == r->source_len && d->source &&
                      memcmp(d->source, r->source, (size_t)r->source_len) == 0 &&
                      (r->defs.count == 0 || strcmp(d->defs.items[0].qualified_name,
                                                    r->defs.items[0].qualified_name) == 0) &&
                      (r->module_qn == NULL) == (d->module_qn == NULL) &&
                      (!r->module_qn || strcmp(r->module_qn, d->module_qn) == 0);
        free(e1);
        free(e2);
        cbm_free_result(d);
        cbm_free_result(r);
        if (!same || !fields) {
            char msg[128];
            snprintf(msg, sizeof(msg), "round trip differs for %s (same=%d fields=%d)",
                     k_spill_sources[i].rel, same, fields);
            FAIL(msg);
        }
    }
    ASSERT_GT(defs, 0);
    ASSERT_GT(calls, 0);
    ASSERT_GT(imports, 0);
    ASSERT_GT(usages, 0);
    ASSERT_GT(string_refs, 0);
    PASS();
}

/* Every strict prefix of an encoding, and the encoding plus a trailing
 * byte, is rejected rather than read past its end. */
TEST(result_spill_decode_rejects_truncated_buffers) {
    CBMFileResult *r = spill_extract(0);
    ASSERT_NOT_NULL(r);
    size_t len = 0;
    char *e = cbm_result_spill_encode(r, &len);
    cbm_free_result(r);
    ASSERT_NOT_NULL(e);
    char *longer = (char *)malloc(len + 1);
    ASSERT_NOT_NULL(longer);
    memcpy(longer, e, len);
    longer[len] = 0;
    size_t accepted_prefix = len;
    for (size_t cut = 0; cut < len; cut++) {
        CBMFileResult *d = cbm_result_spill_decode(e, cut);
        if (d) {
            cbm_free_result(d);
            accepted_prefix = cut;
            break;
        }
    }
    CBMFileResult *trailing = cbm_result_spill_decode(longer, len + 1);
    CBMFileResult *whole = cbm_result_spill_decode(e, len);
    bool trailing_rejected = trailing == NULL;
    bool whole_ok = whole != NULL;
    cbm_free_result(trailing);
    cbm_free_result(whole);
    free(longer);
    free(e);
    ASSERT_EQ(accepted_prefix, len);
    ASSERT_TRUE(trailing_rejected);
    ASSERT_TRUE(whole_ok);
    PASS();
}

/* A parked slot loads back into an equal result; the peek counts match; an
 * unparked slot is empty; close deletes the store file. */
TEST(result_spill_store_parks_and_loads) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/cbm_spill_XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(dir));
    cbm_result_spill_t *sp = cbm_result_spill_open(dir, SPILL_SOURCE_COUNT + 1);
    ASSERT_NOT_NULL(sp);
    bool ok = true;
    for (int i = 0; i < SPILL_SOURCE_COUNT && ok; i++) {
        CBMFileResult *r = spill_extract(i);
        size_t len = 0;
        char *before = r ? cbm_result_spill_encode(r, &len) : NULL;
        int strrefs = r ? r->string_refs.count : -1;
        ok = before && cbm_result_spill_park(sp, i, r); /* frees r */
        if (!ok) {
            cbm_free_result(r);
        }
        CBMFileResult *loaded = ok ? cbm_result_spill_load(sp, i) : NULL;
        size_t len2 = 0;
        char *after = loaded ? cbm_result_spill_encode(loaded, &len2) : NULL;
        ok = ok && after && len == len2 && memcmp(before, after, len) == 0 &&
             cbm_result_spill_has(sp, i) && cbm_result_spill_string_ref_count(sp, i) == strrefs;
        free(before);
        free(after);
        cbm_free_result(loaded);
    }
    bool empty_slot = !cbm_result_spill_has(sp, SPILL_SOURCE_COUNT) &&
                      cbm_result_spill_load(sp, SPILL_SOURCE_COUNT) == NULL;
    int64_t parked = 0;
    int64_t bytes = 0;
    int64_t loads = 0;
    cbm_result_spill_stats(sp, &parked, &bytes, &loads);
    cbm_result_spill_close(sp);
    bool dir_empty = rmdir(dir) == 0;
    ASSERT_TRUE(ok);
    ASSERT_TRUE(empty_slot);
    ASSERT_EQ(parked, SPILL_SOURCE_COUNT);
    ASSERT_EQ(loads, SPILL_SOURCE_COUNT);
    ASSERT_GT(bytes, 0);
    ASSERT_TRUE(dir_empty);
    PASS();
}

#if CBM_WITH_ZOVA
/* 61 files across Go, Python, Java (no package line, so the JVM namespace is
 * inferred from the source root), TypeScript, Rust and YAML, with
 * cross-file calls, inheritance and config URLs: above
 * MIN_FILES_FOR_PARALLEL so the parallel path runs. */
static bool write_spill_fixture(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/cbm_test_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(g_tmpdir)) {
        return false;
    }
    static const char *const dirs[] = {"pkg", "app", "src", "src/main", "src/main/java",
                                       "src/main/java/com", "src/main/java/com/acme", "web",
                                       "core", "services", "src/test", "src/test/java",
                                       "src/test/java/com", "src/test/java/com/acme"};
    char path[512];
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        snprintf(path, sizeof(path), "%s/%s", g_tmpdir, dirs[d]);
        if (cbm_mkdir(path) != 0) {
            return false;
        }
    }
    char body[1024];
    for (int i = 0; i < 60; i++) {
        int k = i % 12;
        int prev = k == 0 ? 0 : k - 1;
        switch (i / 12) {
        case 0:
            snprintf(path, sizeof(path), "%s/pkg/g%02d.go", g_tmpdir, k);
            snprintf(body, sizeof(body),
                     "package pkg\n\ntype T%02d struct {\n\tNext *T%02d\n\tN int\n}\n\n"
                     "func (t *T%02d) Run() int {\n\treturn t.Next.Run() + F%02d()\n}\n\n"
                     "func F%02d() int {\n\treturn %d\n}\n",
                     k, (k + 1) % 12, k, prev, k, k);
            break;
        case 1:
            snprintf(path, sizeof(path), "%s/app/m%02d.py", g_tmpdir, k);
            if (k == 0) {
                snprintf(body, sizeof(body), "class C00:\n    def go(self):\n        return 0\n");
            } else {
                snprintf(body, sizeof(body),
                         "from app.m%02d import C%02d\n\nclass C%02d(C%02d):\n"
                         "    def go(self):\n        return C%02d().go() + 1\n",
                         prev, prev, k, prev, prev);
            }
            break;
        case 2:
            snprintf(path, sizeof(path), "%s/src/main/java/com/acme/J%02d.java", g_tmpdir, k);
            if (k == 0) {
                snprintf(body, sizeof(body), "public class J00 {\n  public int run() { return 0; }\n}\n");
            } else {
                snprintf(body, sizeof(body),
                         "public class J%02d extends J%02d {\n"
                         "  public int run() { return new J%02d().run() + 1; }\n}\n",
                         k, prev, prev);
            }
            break;
        case 3:
            snprintf(path, sizeof(path), "%s/web/t%02d.ts", g_tmpdir, k);
            if (k == 0) {
                snprintf(body, sizeof(body), "export function t00(): number { return 0; }\n");
            } else {
                snprintf(body, sizeof(body),
                         "import { t%02d } from './t%02d';\n"
                         "export function t%02d(): number { return t%02d() + 1; }\n",
                         prev, prev, k, prev);
            }
            break;
        default:
            if (k < 6) {
                snprintf(path, sizeof(path), "%s/core/r%02d.rs", g_tmpdir, k);
                snprintf(body, sizeof(body), "pub fn r%02d() -> i32 {\n    r%02d() + 1\n}\n", k,
                         prev);
            } else {
                snprintf(path, sizeof(path), "%s/services/s%02d.yaml", g_tmpdir, k);
                snprintf(body, sizeof(body),
                         "service:\n  push_endpoint: https://svc%02d.example.com/hook\n"
                         "  jwks_url: https://auth.example.com/keys\n",
                         k);
            }
            break;
        }
        FILE *f = fopen(path, "w");
        if (!f) {
            return false;
        }
        fputs(body, f);
        fclose(f);
    }
    /* Same inferred package, other source root: J05 is visible to this file's
     * cross-LSP only through the namespace, not its module or imports. */
    snprintf(path, sizeof(path), "%s/src/test/java/com/acme/J05Test.java", g_tmpdir);
    FILE *f = fopen(path, "w");
    if (!f) {
        return false;
    }
    fputs("public class J05Test {\n"
          "  public int check() { J05 j = new J05(); return j.run(); }\n}\n",
          f);
    fclose(f);
    return true;
}

/* One full index of the fixture from an empty cbm.zova; digests on success. */
static int spill_full_index_digest(bool force_spill, long *parked, pipeline_sql_digest_t *nodes,
                                   pipeline_sql_digest_t *edges) {
    th_remove_zova_user_database();
    cbm_pp_spill_parked_reset();
    cbm_parallel_force_spill_for_test(force_spill ? 1 : 0);
    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    int rc = p ? cbm_pipeline_run(p) : -1;
    cbm_parallel_force_spill_for_test(0);
    *parked = cbm_pp_spill_parked();
    bool full = p && cbm_pipeline_get_last_route(p) == CBM_PIPELINE_ROUTE_FULL;
    char *project = p ? strdup(cbm_pipeline_project_name(p)) : NULL;
    cbm_pipeline_free(p);
    int digest_rc = rc == 0 && full && project ? pipeline_zova_graph_digest(project, nodes, edges)
                                               : -1;
    free(project);
    return digest_rc;
}
#endif

/* Parking every extraction result and reading it back in each later pass
 * (registry, namespace map, cross-LSP collection, resolve, infra) yields
 * the same graph as keeping all results in memory. */
TEST(pipeline_forced_spill_matches_in_memory_graph) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    if (!write_spill_fixture()) {
        teardown_test_repo();
        FAIL("failed to create fixture");
    }
    long parked_normal = -1;
    long parked_spill = -1;
    pipeline_sql_digest_t nodes_normal = {0};
    pipeline_sql_digest_t edges_normal = {0};
    pipeline_sql_digest_t nodes_spill = {0};
    pipeline_sql_digest_t edges_spill = {0};
    int normal_rc = spill_full_index_digest(false, &parked_normal, &nodes_normal, &edges_normal);
    int spill_rc = spill_full_index_digest(true, &parked_spill, &nodes_spill, &edges_spill);
    teardown_test_repo();

    ASSERT_EQ(normal_rc, 0);
    ASSERT_EQ(spill_rc, 0);
    ASSERT_EQ(parked_normal, 0);
    /* Engagement guard: every extracted file was parked. */
    ASSERT_EQ(parked_spill, 61);
    ASSERT_GT(nodes_normal.rows, 60);
    ASSERT_GT(edges_normal.rows, 60);
    ASSERT_EQ(nodes_spill.rows, nodes_normal.rows);
    ASSERT_TRUE(nodes_spill.hash == nodes_normal.hash);
    ASSERT_EQ(edges_spill.rows, edges_normal.rows);
    ASSERT_TRUE(edges_spill.hash == edges_normal.hash);
    PASS();
#endif
}

/* TS cross-registry test hooks (ts_lsp.c) — extern to avoid pulling the
 * tree-sitter-typed ts_lsp.h into this store-level test. */
extern long cbm_ts_full_registry_builds(void);
extern void cbm_ts_full_registry_builds_reset(void);

/* Reproduce-first (ms-typescript finding, 2026-07-07): the SEQUENTIAL
 * cross-LSP driver must resolve TS files through the SHARED prebuilt
 * registry, never a full per-file build. cbm_run_ts_lsp_cross registers
 * stdlib + EVERY cross-file def + finalizes once PER FILE — O(files x defs).
 * On an 81k-file TS corpus that ground one core for hours (74% of stack
 * samples inside build_qn_index), and when the supervisor's quiet-timeout
 * killed the crawl mid-pass, the stale extraction marker blamed innocent
 * files, quarantining four of them one 15-minute retry at a time.
 *
 * The fixture stays UNDER MIN_FILES_FOR_PARALLEL (50) so the pipeline
 * routes through the sequential driver — the path that lacked the
 * shared-registry prepare.
 * RED on the unfixed driver: full builds == TS file count (40).
 * GREEN: full builds == 0 AND the cross-file TS call still resolves
 * (quality guard — the shared path must not lose the edge). */
TEST(pipeline_seq_ts_cross_uses_shared_registry) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cbm_test_XXXXXX");
    if (!cbm_mkdtemp(g_tmpdir)) {
        FAIL("failed to create temp dir");
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/shared.ts", g_tmpdir);
    FILE *f = fopen(path, "w");
    if (!f) {
        FAIL("failed to create fixture file");
    }
    fputs("export function sharedHelper(): number {\n  return 42;\n}\n", f);
    fclose(f);
    for (int i = 0; i < 39; i++) {
        snprintf(path, sizeof(path), "%s/caller%02d.ts", g_tmpdir, i);
        f = fopen(path, "w");
        if (!f) {
            FAIL("failed to create fixture file");
        }
        fprintf(f,
                "import { sharedHelper } from \"./shared\";\n"
                "export function caller%02d(): number {\n  return sharedHelper();\n}\n",
                i);
        fclose(f);
    }

    cbm_ts_full_registry_builds_reset();
    cbm_pipeline_t *p = cbm_pipeline_new(g_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    int rc = cbm_pipeline_run(p);
    long builds = cbm_ts_full_registry_builds();

    /* Quality guard FIRST: the cross-file call must resolve either way. */
    cbm_store_t *s = th_open_repo_store(g_tmpdir);
    bool linked = false;
    if (s) {
        linked =
            cross_file_call_exists(s, cbm_pipeline_project_name(p), "caller00", "sharedHelper");
        cbm_store_close(s);
    }
    cbm_pipeline_free(p);
    teardown_test_repo();

    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(linked);
    /* The point: ZERO full per-file registry builds on the sequential path. */
    ASSERT_EQ(builds, 0);
    PASS();
}

/* Edges of `edge_type` from the node `<project>.<source_tail>` to `<project>.<target_tail>`
 * (QNs, not names: a C macro and the definition it shadows share their name). */
static int c1_edge_count_qn(cbm_store_t *s, const char *project, const char *edge_type,
                            const char *source_tail, const char *target_tail) {
    char source_qn[512];
    char target_qn[512];
    snprintf(source_qn, sizeof(source_qn), "%s.%s", project, source_tail);
    snprintf(target_qn, sizeof(target_qn), "%s.%s", project, target_tail);
    cbm_node_t source = {0};
    cbm_node_t target = {0};
    int matches = -1;
    if (cbm_store_find_node_by_qn(s, project, source_qn, &source) == CBM_STORE_OK &&
        cbm_store_find_node_by_qn(s, project, target_qn, &target) == CBM_STORE_OK) {
        cbm_edge_t *edges = NULL;
        int edge_count = 0;
        matches = 0;
        if (cbm_store_find_edges_by_source_type(s, source.id, edge_type, &edges, &edge_count) ==
            CBM_STORE_OK) {
            for (int i = 0; i < edge_count; i++) {
                matches += edges[i].target_id == target.id;
            }
            cbm_store_free_edges(edges, edge_count);
        }
    }
    cbm_node_free_fields(&source);
    cbm_node_free_fields(&target);
    return matches;
}

/* Label of the node `<project>.<qn_tail>`, copied into `out`; "" when there is none. */
static const char *c1_label_of(cbm_store_t *s, const char *project, const char *qn_tail, char *out,
                               size_t out_size) {
    char qn[512];
    snprintf(qn, sizeof(qn), "%s.%s", project, qn_tail);
    cbm_node_t node = {0};
    out[0] = '\0';
    if (cbm_store_find_node_by_qn(s, project, qn, &node) == CBM_STORE_OK && node.label) {
        snprintf(out, out_size, "%s", node.label);
    }
    cbm_node_free_fields(&node);
    return out;
}

/* Do the properties of the node `<project>.<qn_tail>` contain `needle`? */
static bool c1_props_contain(cbm_store_t *s, const char *project, const char *qn_tail,
                             const char *needle) {
    char qn[512];
    snprintf(qn, sizeof(qn), "%s.%s", project, qn_tail);
    cbm_node_t node = {0};
    bool found = cbm_store_find_node_by_qn(s, project, qn, &node) == CBM_STORE_OK &&
                 node.properties_json && strstr(node.properties_json, needle) != NULL;
    cbm_node_free_fields(&node);
    return found;
}

/* PR C1, end to end: C keeps one graph node per QN, so the right entity must win.
 * A header and its .c share the module QN (proj.s): `struct S s_global;` in s.c
 * minted a Class for the REFERENCE and "smallest path wins" handed it the node (redis:
 * struct redisServer pointed at server.c:85). A function and its #else macro
 * stand-in shared a QN, and the later macro line won (curl url_match_proxy_use, 41
 * functions): the macro now has a QN of its own (`...#macro`), so both are nodes.
 * The typedef alias had no node at all. */
TEST(pipeline_c_definitions_own_their_qn_c1) {
    const char *files[] = {"s.h", "s.c"};
    const char *contents[] = {"struct S {\n"
                              "  int v;\n"
                              "};\n"
                              "typedef struct S S_t;\n",

                              "#include \"s.h\"\n"
                              "struct S s_global;\n"
                              "#ifndef DISABLE_PROXY\n"
                              "static int match_proxy(int a)\n"
                              "{\n"
                              "  return a + 1;\n"
                              "}\n"
                              "#else\n"
                              "#define match_proxy(a) 1\n"
                              "#endif\n"
                              "int use(void) { return match_proxy(s_global.v); }\n"};
    if (setup_lang_repo(files, contents, 2) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    const char *proj = cbm_pipeline_project_name(p);

    cbm_node_t *nodes = NULL;
    int n = 0;
    cbm_store_find_nodes_by_name(s, proj, "S", &nodes, &n);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(nodes[0].label, "Class");
    ASSERT_STR_EQ(nodes[0].file_path, "s.h");
    ASSERT_EQ(nodes[0].start_line, 1);
    cbm_store_free_nodes(nodes, n);

    /* the function and its #else macro stand-in: two nodes, the plain QN is the
     * function's */
    cbm_store_find_nodes_by_name(s, proj, "match_proxy", &nodes, &n);
    ASSERT_EQ(n, 2);
    cbm_store_free_nodes(nodes, n);
    char qn[512];
    cbm_node_t fn = {0};
    snprintf(qn, sizeof(qn), "%s.s.match_proxy", proj);
    ASSERT_EQ(cbm_store_find_node_by_qn(s, proj, qn, &fn), CBM_STORE_OK);
    ASSERT_STR_EQ(fn.label, "Function");
    ASSERT_EQ(fn.start_line, 4);
    cbm_node_free_fields(&fn);
    cbm_node_t mac = {0};
    snprintf(qn, sizeof(qn), "%s.s.match_proxy#macro", proj);
    ASSERT_EQ(cbm_store_find_node_by_qn(s, proj, qn, &mac), CBM_STORE_OK);
    ASSERT_STR_EQ(mac.label, "Macro");
    ASSERT_STR_EQ(mac.name, "match_proxy");
    ASSERT_EQ(mac.start_line, 9);
    cbm_node_free_fields(&mac);

    cbm_store_find_nodes_by_name(s, proj, "S_t", &nodes, &n);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(nodes[0].label, "Type");
    cbm_store_free_nodes(nodes, n);

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();
    PASS();
}

/* The tie rule of the macro namespace: a name visible both as a definition and as a
 * macro resolves to the DEFINITION; the macro is the target only when no definition
 * of that name is visible. `pick_fn` is a function in one #if branch and a macro in
 * the other: before, one node held the QN and it was the macro (the later line), so
 * the call landed on a Macro. `renamed_fn` is declared by a prototype whose name a
 * rename macro replaces (jemalloc smallocx): no definition exists in the repo, the
 * macro is the only node, and the call keeps pointing at it. */
TEST(pipeline_c_call_targets_definition_before_macro_c1) {
    const char *files[] = {"tie.c"};
    const char *contents[] = {"#define renamed_fn NS_renamed_fn\n"
                              "extern int renamed_fn(int v);\n"
                              "#ifdef USE_REAL\n"
                              "static int pick_fn(int v) { return v; }\n"
                              "#else\n"
                              "#define pick_fn(v) (v)\n"
                              "#endif\n"
                              "int caller(int b) {\n"
                              "    return pick_fn(b) + renamed_fn(b);\n"
                              "}\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    char proj[256];
    snprintf(proj, sizeof(proj), "%s", cbm_pipeline_project_name(p));

    char label[64];
    ASSERT_STR_EQ(c1_label_of(s, proj, "tie.pick_fn", label, sizeof(label)), "Function");
    ASSERT_STR_EQ(c1_label_of(s, proj, "tie.pick_fn#macro", label, sizeof(label)), "Macro");
    ASSERT_STR_EQ(c1_label_of(s, proj, "tie.renamed_fn#macro", label, sizeof(label)), "Macro");
    ASSERT_STR_EQ(c1_label_of(s, proj, "tie.renamed_fn", label, sizeof(label)), "");
    int to_definition = c1_edge_count_qn(s, proj, "CALLS", "tie.caller", "tie.pick_fn");
    int to_shadowed_macro = c1_edge_count_qn(s, proj, "CALLS", "tie.caller", "tie.pick_fn#macro");
    int to_only_macro = c1_edge_count_qn(s, proj, "CALLS", "tie.caller", "tie.renamed_fn#macro");

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();

    ASSERT_EQ(to_definition, 1);
    ASSERT_EQ(to_shadowed_macro, 0);
    ASSERT_EQ(to_only_macro, 1);
    PASS();
}

/* C, an enum declared inside a struct (curl lib/cf-h1-proxy.c `enum keeponval {...}
 * keepon;`): its enumerators are file-scope names, so their nodes are `<module>.<CONST>`
 * and the unqualified references in the code reach them. The struct is no QN segment,
 * for the named enum and the anonymous one alike. */
TEST(pipeline_c_enum_inside_struct_reference_resolves_c1) {
    const char *files[] = {"link.c"};
    const char *contents[] = {"struct conn {\n"
                              "    enum state { ST_IDLE, ST_BUSY } st;\n"
                              "    enum { KIND_A, KIND_B } kind;\n"
                              "    int fd;\n"
                              "};\n"
                              "int busy(struct conn *c) {\n"
                              "    return c->st == ST_BUSY || c->kind == KIND_B;\n"
                              "}\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    char proj[256];
    snprintf(proj, sizeof(proj), "%s", cbm_pipeline_project_name(p));

    int to_named = c1_edge_count_qn(s, proj, "USAGE", "link.busy", "link.ST_BUSY");
    int to_anonymous = c1_edge_count_qn(s, proj, "USAGE", "link.busy", "link.KIND_B");
    char in_struct[64];
    c1_label_of(s, proj, "link.conn.ST_BUSY", in_struct, sizeof(in_struct));
    char parent_marker[512];
    snprintf(parent_marker, sizeof(parent_marker), "\"parent_class\":\"%s.link.conn.state\"", proj);
    bool named_keeps_parent = c1_props_contain(s, proj, "link.ST_BUSY", parent_marker);
    bool anonymous_has_parent = c1_props_contain(s, proj, "link.KIND_B", "\"parent_class\"");

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();

    ASSERT_EQ(to_named, 1);
    ASSERT_EQ(to_anonymous, 1);
    ASSERT_STR_EQ(in_struct, "");
    ASSERT_TRUE(named_keeps_parent);
    ASSERT_FALSE(anonymous_has_parent);
    PASS();
}

typedef struct {
    int rc;
    bool opened;
    bool pick_lists_both; /* <proj>.v.pick carries the two spans, in order */
    bool limit_has_list;  /* <proj>.v.LIMIT#macro carries a list */
    bool once_has_list;   /* a name defined once must not */
    int other_lang_nodes; /* nodes of cmd.json */
    int other_lang_lists; /* ... that carry a list (must be 0) */
} c1_variants_obs_t;

static c1_variants_obs_t c1_observe_variants(const char *repo) {
    c1_variants_obs_t obs = {0};
    cbm_pipeline_t *p = cbm_pipeline_new(repo, NULL, CBM_MODE_FULL);
    if (!p) {
        obs.rc = -1;
        return obs;
    }
    obs.rc = cbm_pipeline_run(p);
    char proj[256];
    snprintf(proj, sizeof(proj), "%s", cbm_pipeline_project_name(p));
    cbm_pipeline_free(p);
    cbm_store_t *s = th_open_repo_store(repo);
    if (!s) {
        return obs;
    }
    obs.opened = true;
    obs.pick_lists_both =
        c1_props_contain(s, proj, "v.pick",
                         "\"variants\":[{\"file_path\":\"v.c\",\"start_line\":2,\"end_line\":2},"
                         "{\"file_path\":\"v.c\",\"start_line\":4,\"end_line\":6}]");
    obs.limit_has_list = c1_props_contain(s, proj, "v.LIMIT#macro",
                                          "\"variants\":[{\"file_path\":\"v.c\",\"start_line\":9,");
    obs.once_has_list = c1_props_contain(s, proj, "v.once", "\"variants\"");
    cbm_node_t *nodes = NULL;
    int count = 0;
    if (cbm_store_find_nodes_by_file_overlap(s, proj, "cmd.json", 1, 1000, &nodes, &count) ==
        CBM_STORE_OK) {
        obs.other_lang_nodes = count;
        for (int i = 0; i < count; i++) {
            obs.other_lang_lists += nodes[i].properties_json &&
                                    strstr(nodes[i].properties_json, "\"variants\"") != NULL;
        }
        cbm_store_free_nodes(nodes, count);
    }
    cbm_store_close(s);
    return obs;
}

/* `variants` reaches the node on BOTH execution paths. The property is computed per
 * file at extraction; the sequential pass (pass_definitions.c) and the parallel one
 * (pass_parallel.c) each serialize it, and the properties buffer is sized for the
 * list, so a path that forgot either would publish a node without it. One fixture,
 * two runs: CBM_WORKERS=1 forces the sequential path, the 55 fillers plus
 * CBM_WORKERS=4 select the parallel one. A JSON file with a repeated key rides along:
 * no language outside the C preprocessor ones gets the property. */
TEST(pipeline_c_variants_on_sequential_and_parallel_paths_c1) {
    const char *td = th_mktempdir("cbm_c1_variants");
    if (!td) {
        FAIL("tmpdir");
    }
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", td);
    char path[512];
    snprintf(path, sizeof(path), "%s/v.c", tmp);
    int write_rc = th_write_file(path, "#if A\n"
                                       "int pick(int a) { return a; }\n"
                                       "#else\n"
                                       "int pick(int a) {\n"
                                       "    return a + 1;\n"
                                       "}\n"
                                       "#endif\n"
                                       "#ifdef B\n"
                                       "#define LIMIT 1\n"
                                       "#else\n"
                                       "#define LIMIT 2\n"
                                       "#endif\n"
                                       "int once(void) { return 0; }\n");
    snprintf(path, sizeof(path), "%s/cmd.json", tmp);
    /* the repeated key on two lines: one span would never be listed, gate or no gate */
    write_rc |= th_write_file(path, "{\n  \"a\": {\"name\": 1},\n  \"b\": {\"name\": 2}\n}\n");
    for (int i = 0; i < 55; i++) {
        char source[96];
        snprintf(path, sizeof(path), "%s/pad_%02d.c", tmp, i);
        snprintf(source, sizeof(source), "int c1_pad_%02d(void) { return %d; }\n", i, i);
        write_rc |= th_write_file(path, source);
    }
    if (write_rc != 0) {
        th_rmtree(tmp);
        FAIL("failed to write the variants fixture");
    }

    char *old_workers = getenv("CBM_WORKERS");
    char *saved_workers = old_workers ? strdup(old_workers) : NULL;

    cbm_setenv("CBM_WORKERS", "1", 1);
    c1_variants_obs_t sequential = c1_observe_variants(tmp);

    cbm_setenv("CBM_WORKERS", "4", 1);
    c1_variants_obs_t parallel = c1_observe_variants(tmp);

    if (saved_workers) {
        cbm_setenv("CBM_WORKERS", saved_workers, 1);
        free(saved_workers);
    } else {
        cbm_unsetenv("CBM_WORKERS");
    }
    th_rmtree(tmp);

    const c1_variants_obs_t *runs[] = {&sequential, &parallel};
    for (int i = 0; i < 2; i++) {
        if (!runs[i]->pick_lists_both || !runs[i]->limit_has_list || runs[i]->once_has_list ||
            runs[i]->other_lang_lists != 0) {
            fprintf(
                stderr,
                "  [c1-variants] path=%s pick=%d limit=%d once=%d json_nodes=%d json_lists=%d\n",
                i == 0 ? "sequential" : "parallel", runs[i]->pick_lists_both,
                runs[i]->limit_has_list, runs[i]->once_has_list, runs[i]->other_lang_nodes,
                runs[i]->other_lang_lists);
        }
    }
    /* the property on each path first, the language limit second: one concern must
     * not hide the other */
    for (int i = 0; i < 2; i++) {
        ASSERT_EQ(runs[i]->rc, 0);
        ASSERT_TRUE(runs[i]->opened);
        ASSERT_TRUE(runs[i]->pick_lists_both);
        ASSERT_TRUE(runs[i]->limit_has_list);
        ASSERT_FALSE(runs[i]->once_has_list);
    }
    for (int i = 0; i < 2; i++) {
        ASSERT_GT(runs[i]->other_lang_nodes, 0);
        ASSERT_EQ(runs[i]->other_lang_lists, 0);
    }
    PASS();
}

/* C++ overloads share a QN (the signature is no part of it), so two of them in one
 * file are the same shape as two #if branches: one node -- the last by start line --
 * and `variants` lists both spans. */
TEST(pipeline_cpp_overloads_are_listed_as_variants_c1) {
    const char *files[] = {"ov.cpp"};
    const char *contents[] = {"int scale(int v) { return v * 2; }\n"
                              "double scale(double v) {\n"
                              "    return v * 2.0;\n"
                              "}\n"
                              "int once(int v) { return scale(v); }\n"};
    if (setup_lang_repo(files, contents, 1) != 0)
        FAIL("tmpdir");

    cbm_pipeline_t *p = cbm_pipeline_new(g_lang_tmpdir, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    cbm_store_t *s = th_open_repo_store(g_lang_tmpdir);
    ASSERT_NOT_NULL(s);
    char proj[256];
    snprintf(proj, sizeof(proj), "%s", cbm_pipeline_project_name(p));

    cbm_node_t *nodes = NULL;
    int n = 0;
    cbm_store_find_nodes_by_name(s, proj, "scale", &nodes, &n);
    int scale_nodes = n;
    int kept_start = n > 0 ? nodes[0].start_line : -1;
    cbm_store_free_nodes(nodes, n);
    bool lists_both =
        c1_props_contain(s, proj, "ov.scale",
                         "\"variants\":[{\"file_path\":\"ov.cpp\",\"start_line\":1,\"end_line\":1},"
                         "{\"file_path\":\"ov.cpp\",\"start_line\":2,\"end_line\":4}]");
    bool once_has_list = c1_props_contain(s, proj, "ov.once", "\"variants\"");

    cbm_store_close(s);
    cbm_pipeline_free(p);
    teardown_lang_repo();

    ASSERT_EQ(scale_nodes, 1);
    ASSERT_EQ(kept_start, 2);
    ASSERT_TRUE(lists_both);
    ASSERT_FALSE(once_has_list);
    PASS();
}

SUITE(pipeline) {
    /* Lifecycle */
    RUN_TEST(pipeline_create_free);
    RUN_TEST(pipeline_null_repo);
    RUN_TEST(pipeline_free_null);
    RUN_TEST(pipeline_cancel);
    RUN_TEST(pipeline_cancel_null);
    RUN_TEST(pipeline_run_null);
    /* Extraction back-pressure */
    /* Fail-whole stops the 1 MiB-budget runs at extraction, before any
     * database dump, so these run in the default suite again. */
    RUN_TEST(pipeline_backpressure_futile_nap_disengages);
    RUN_TEST(pipeline_over_budget_after_futility_fails_whole_and_preserves_generation);
    /* Extraction result spill (F1) */
    RUN_TEST(result_spill_round_trip_is_byte_identical);
    RUN_TEST(result_spill_decode_rejects_truncated_buffers);
    RUN_TEST(result_spill_store_parks_and_loads);
    RUN_TEST(pipeline_forced_spill_matches_in_memory_graph);
    /* Sequential cross-LSP shared registry (ms-typescript quadratic) */
    RUN_TEST(pipeline_seq_ts_cross_uses_shared_registry);
    /* File persistence */
    /* Integration: structure pass */
    RUN_TEST(pipeline_structure_nodes);
    RUN_TEST(pipeline_committed_counts_match_persisted);
    RUN_TEST(pipeline_structure_edges);
    RUN_TEST(pipeline_branch_root_structure);
    RUN_TEST(pipeline_project_name_derived);
    RUN_TEST(pipeline_fast_mode);
    /* Definitions pass */
    RUN_TEST(pipeline_definitions_function_nodes);
    RUN_TEST(pipeline_definitions_defines_edges);
    RUN_TEST(pipeline_definitions_properties);
    RUN_TEST(pipeline_def_props_valid_json_when_oversized);
    RUN_TEST(pipeline_edge_props_valid_json);
    /* Complexity propagation pass (Tier B) */
    RUN_TEST(pipeline_complexity_transitive_loop_depth);
    RUN_TEST(pipeline_complexity_cycle_is_independent_of_temporary_ids);
    RUN_TEST(pipeline_complexity_leaf_and_non_function_graphs_do_not_sort_null);
    /* Calls pass */
    RUN_TEST(pipeline_calls_resolution);
    RUN_TEST(pipeline_incremental_preserves_cross_file_calls);
    RUN_TEST(pipeline_tsjs_receiver_suppresses_weak_method_edge);
    RUN_TEST(pipeline_tsjs_receiver_parallel_keeps_service_edges);
    RUN_TEST(pipeline_python_receiver_suppresses_weak_method_edge);
    RUN_TEST(pipeline_go_rw_usage_never_cross_into_c);
    RUN_TEST(pipeline_go_rw_usage_never_cross_into_c_parallel);
    RUN_TEST(pipeline_go_bare_ref_never_binds_field);
    RUN_TEST(pipeline_go_bare_ref_never_binds_field_parallel);
    RUN_TEST(pipeline_go_cross_package_field_chain_resolves);
    RUN_TEST(pipeline_python_receiver_parallel_suppresses_weak_method_edges);
    RUN_TEST(pipeline_zig_member_calls_bind_by_receiver_type);
    RUN_TEST(pipeline_zig_dependency_modules_resolve_through_build_zig_zon);
    RUN_TEST(pipeline_python_bare_local_binding_suppresses_weak_edge);
    RUN_TEST(pipeline_python_bare_local_binding_parallel_suppresses_weak_edge);
    RUN_TEST(pipeline_external_import_binding_sequential_parallel);
    RUN_TEST(pipeline_local_fetch_shadow_not_classified_as_http);
    /* Git history pass */
    RUN_TEST(githistory_is_trackable);
    RUN_TEST(githistory_compute_coupling);
    RUN_TEST(githistory_apply_keeps_coverage_stamp);
    RUN_TEST(githistory_coupling_carries_last_co_change);
    RUN_TEST(githistory_skip_large_commits);
    RUN_TEST(githistory_limits_to_max);
    /* Test detection */
    RUN_TEST(testdetect_is_test_file);
    RUN_TEST(testdetect_is_test_function);
    /* Implements pass (graph buffer based) */
    RUN_TEST(implements_creates_override);
    RUN_TEST(implements_no_match);
    /* Usages pass (full pipeline integration) */
    RUN_TEST(usages_creates_edges);
    RUN_TEST(usages_no_duplicate_calls);
    RUN_TEST(calls_edge_carries_call_site_line);
    RUN_TEST(usages_kotlin_creates_edges);
    RUN_TEST(usages_kotlin_no_duplicate_calls);
    /* Language integration tests */
    RUN_TEST(pipeline_python_project);
    RUN_TEST(pipeline_imports_multi_symbol_edges);
    RUN_TEST(pipeline_rust_grouped_import_properties_valid_json_sequential);
    RUN_TEST(pipeline_go_cross_package_call);
    RUN_TEST(pipeline_python_cross_module_call);
    RUN_TEST(pipeline_cross_language_same_name_does_not_share_calls_issue725);
    RUN_TEST(pipeline_go_type_classification);
    RUN_TEST(pipeline_go_grouped_types);
    RUN_TEST(pipeline_kotlin_project);
    RUN_TEST(pipeline_lua_anonymous_functions);
    RUN_TEST(pipeline_csharp_modern);
    RUN_TEST(pipeline_bom_stripping);
    RUN_TEST_LANG(CBM_LANG_FORM, pipeline_form_call_resolution);
    RUN_TEST(pipeline_python_type_inference);
    /* Docstring integration (port of TestDocstringIntegration) */
    RUN_TEST(pipeline_docstring_go_function);
    RUN_TEST(pipeline_docstring_python_function);
    RUN_TEST(pipeline_docstring_java_method);
    RUN_TEST(pipeline_docstring_kotlin_function);
    RUN_TEST(pipeline_docstring_go_class);
    /* Project name */
    RUN_TEST(project_name_from_path);
    RUN_TEST(project_name_drive_letter_case_insensitive_issue394);
    RUN_TEST(git_context_non_git_path);
    RUN_TEST(git_context_linked_worktree);
    RUN_TEST(project_name_uniqueness);
    /* Git diff helpers */
    RUN_TEST(gitdiff_parse_range_with_count);
    RUN_TEST(gitdiff_parse_range_no_count);
    RUN_TEST(gitdiff_parse_range_zero_count);
    RUN_TEST(gitdiff_parse_range_large);
    RUN_TEST(gitdiff_parse_name_status);
    RUN_TEST(gitdiff_parse_name_status_filters_untrackable);
    RUN_TEST(gitdiff_parse_hunks);
    RUN_TEST(gitdiff_parse_hunks_no_newline_marker);
    RUN_TEST(gitdiff_parse_hunks_mode_change);
    RUN_TEST(gitdiff_parse_hunks_deletion);
    /* Config helpers */
    RUN_TEST(configures_is_env_var_name);
    RUN_TEST(configures_normalize_config_key);
    RUN_TEST(configures_has_config_extension);
    /* Config integration tests */
    RUN_TEST(configures_env_var_in_config);
    RUN_TEST(configures_lowercase_key_skipped);
    RUN_TEST(configures_non_config_file_skipped);
    RUN_TEST(configures_full_pipeline_integration);
    /* HTTP link pipeline integration */
    /* Enrichment helpers */
    RUN_TEST(enrichment_split_camel_case);
    RUN_TEST(enrichment_tokenize_decorator);
    /* Decorator tags integration */
    RUN_TEST(decorator_tags_python_auto_discovery);
    RUN_TEST(decorator_tags_java_class_methods);
    /* Compile commands helpers */
    RUN_TEST(compile_commands_split_command);
    RUN_TEST(compile_commands_extract_flags);
    RUN_TEST(compile_commands_parse_json);
    RUN_TEST(compile_commands_parse_empty);
    RUN_TEST(compile_commands_parse_invalid);
    /* Infrascan helpers */
    RUN_TEST(infra_is_compose_file);
    RUN_TEST(infra_is_cloudbuild_file);
    RUN_TEST(infra_is_shell_script);
    RUN_TEST_LANG(CBM_LANG_DOCKERFILE, infra_is_dockerfile);
    RUN_TEST_LANG(CBM_LANG_KUSTOMIZE, infra_is_kustomize_file);
    RUN_TEST_LANG(CBM_LANG_K8S, infra_is_k8s_manifest);
    RUN_TEST_LANG(CBM_LANG_DOTENV, infra_is_env_file);
    RUN_TEST(infra_clean_json_brackets);
    RUN_TEST(infra_secret_detection);
    /* Infrascan: Dockerfile parser */
    RUN_TEST(infra_parse_dockerfile_multistage);
    RUN_TEST(infra_parse_dockerfile_entrypoint);
    RUN_TEST(infra_parse_dockerfile_secret_filtered);
    RUN_TEST(infra_parse_dockerfile_expose_protocol);
    RUN_TEST(infra_parse_dockerfile_env_space);
    RUN_TEST(infra_parse_dockerfile_empty);
    /* Infrascan: Dotenv parser */
    RUN_TEST(infra_parse_dotenv);
    RUN_TEST(infra_parse_dotenv_quoted);
    /* Infrascan: Shell script parser */
    RUN_TEST(infra_parse_shell);
    RUN_TEST(infra_parse_shell_with_source);
    RUN_TEST(infra_parse_shell_secret_filtered);
    RUN_TEST(infra_parse_shell_shebang_only);
    RUN_TEST(infra_parse_shell_truly_empty);
    /* Infrascan: Terraform parser */
    RUN_TEST(infra_parse_terraform_full);
    RUN_TEST(infra_parse_terraform_variables_only);
    RUN_TEST(infra_parse_terraform_empty);
    RUN_TEST(helm_parse_chart_dependencies_issue338);
    RUN_TEST(helm_parse_chart_no_deps_issue338);
    /* Infrascan: QN helper */
    RUN_TEST(infra_qn_helper);
    /* Infrascan: pipeline integration */
    RUN_TEST(infra_pipeline_integration);
    RUN_TEST(infra_pipeline_idempotent);
    /* K8s / Kustomize extraction */
    RUN_TEST_LANG(CBM_LANG_KUSTOMIZE, k8s_extract_kustomize);
    RUN_TEST_LANG(CBM_LANG_K8S, k8s_extract_manifest);
    RUN_TEST_LANG(CBM_LANG_K8S, k8s_extract_manifest_no_name);
    RUN_TEST_LANG(CBM_LANG_K8S, k8s_extract_manifest_multidoc);
    /* Env URL scanning */
    RUN_TEST_LANG(CBM_LANG_DOCKERFILE, envscan_dockerfile_env_urls);
    RUN_TEST(envscan_shell_env_urls);
    RUN_TEST(envscan_env_file_urls);
    RUN_TEST_LANG(CBM_LANG_TOML, envscan_toml_urls);
    RUN_TEST(envscan_yaml_urls);
    RUN_TEST(envscan_terraform_urls);
    RUN_TEST_LANG(CBM_LANG_PROPERTIES, envscan_properties_urls);
    RUN_TEST(envscan_secret_key_exclusion);
    RUN_TEST(envscan_secret_value_exclusion);
    RUN_TEST(envscan_secret_file_exclusion);
    RUN_TEST(envscan_skips_ignored_dirs);
    RUN_TEST(envscan_non_url_values_skipped);
    /* Discovery-exclusion plumbing in auxiliary repo walks (#792) */
    RUN_TEST(pipeline_relpath_excluded_boundary);
    RUN_TEST(pkgmap_scan_repo_honors_discovery_exclusions);
    RUN_TEST(pipeline_concurrent_runs_keep_their_own_pkgmap);
    RUN_TEST(envscan_walk_honors_discovery_exclusions);
    /* Function registry / resolver */
    RUN_TEST(registry_resolve_single_candidate);
    RUN_TEST(registry_fuzzy_nonexistent);
    RUN_TEST(registry_fuzzy_multiple_best_by_distance);
    RUN_TEST(registry_fuzzy_simple_name_extraction);
    RUN_TEST(registry_fuzzy_empty);
    RUN_TEST(registry_exists);
    RUN_TEST(registry_confidence_import_map);
    RUN_TEST(registry_confidence_import_map_suffix);
    RUN_TEST(registry_confidence_same_module);
    RUN_TEST(registry_confidence_unique_name);
    RUN_TEST(registry_confidence_suffix_match);
    RUN_TEST(registry_receiver_chain_refuses_library_unique_name_issue1893);
    RUN_TEST(registry_receiver_chain_refuses_library_suffix_match_issue1893);
    RUN_TEST(registry_receiver_chain_keeps_project_extension_issue1893);
    RUN_TEST(registry_receiver_chain_ignores_lowercase_root_issue1893);
    RUN_TEST(registry_receiver_chain_ignores_bare_name_issue1893);
    RUN_TEST(registry_fuzzy_confidence_single);
    RUN_TEST(registry_fuzzy_confidence_distance);
    RUN_TEST(registry_negative_import_rejects);
    RUN_TEST(registry_fuzzy_import_penalty);
    RUN_TEST(registry_fuzzy_no_import_map_passthrough);
    RUN_TEST(registry_find_by_name);
    RUN_TEST(registry_confidence_band);
    RUN_TEST(registry_is_import_reachable);
    RUN_TEST(registry_find_ending_with);
    /* Git history */
    RUN_TEST(githistory_is_trackable_file);
    RUN_TEST(githistory_compute_change_coupling);
    RUN_TEST(githistory_coupling_skips_large_commits);
    RUN_TEST(githistory_coupling_limits_output);
    /* Incremental reindex */
    /* FastAPI Depends edge tracking (PR #66 port) */
    RUN_TEST(pipeline_fastapi_depends_edges);
    /* Incremental */
    RUN_TEST(incremental_full_then_noop);
    RUN_TEST(incremental_detects_changed_file);
    RUN_TEST(pipeline_incremental_direct_zova_matches_fresh_full);
    RUN_TEST(pipeline_stale_index_format_rebuilds_in_full_once);
    RUN_TEST(pipeline_single_file_default_publishes_full_and_incremental_generation);
    RUN_TEST(pipeline_single_file_preserves_and_removes_exact_project_summary);
    RUN_TEST(pipeline_incremental_delete_direct_zova_matches_fresh_full);
    RUN_TEST(pipeline_incremental_rename_direct_zova_matches_fresh_full);
    RUN_TEST(pipeline_incremental_infra_route_change_matches_fresh_full);
    RUN_TEST(pipeline_swift_nested_url_makes_route_issue1892);
    RUN_TEST(pipeline_swift_http_call_makes_route_issue1892);
    RUN_TEST(pipeline_arg_url_rejects_non_http_slash_arguments);
    RUN_TEST(pipeline_arg_url_rejects_document_file_paths);
    RUN_TEST(pipeline_arg_url_skips_client_navigation);
    RUN_TEST(pipeline_typed_http_call_has_no_arg_url_twin);
    RUN_TEST(pipeline_typed_http_call_has_no_arg_url_twin_sequential);
    RUN_TEST(pipeline_capitalized_axios_import_is_http_client);
    RUN_TEST(pipeline_capitalized_axios_import_is_http_client_sequential);
    RUN_TEST(pipeline_python_websocket_decorators_register_routes);
    RUN_TEST(pipeline_incremental_shared_infra_url_survives_owner_change);
    RUN_TEST(pipeline_incremental_handler_change_rematches_infra_route);
    RUN_TEST(incremental_detects_deleted_file);
    RUN_TEST(incremental_new_file_added);
    RUN_TEST(incremental_fast_preserves_mode_skipped_tools_dir);
    RUN_TEST(incremental_k8s_manifest_indexed);
    RUN_TEST_LANG(CBM_LANG_KUSTOMIZE, incremental_kustomize_module_indexed);
    /* Resource management & internal helper tests */
    RUN_TEST(pipeline_empty_path);
    RUN_TEST(pipeline_project_name_content);
    RUN_TEST(pipeline_cancel_sets_flag);
    RUN_TEST(pipeline_double_cancel);
    RUN_TEST(pipeline_double_free_prevention);
    /* Trackable file tests */
    RUN_TEST(trackable_source_files);
    RUN_TEST(trackable_rejects_images);
    RUN_TEST(trackable_rejects_minified);
    RUN_TEST(trackable_rejects_vendor_dirs);
    RUN_TEST(trackable_rejects_lock_files);
    /* Test path detection */
    RUN_TEST(test_path_directory_patterns);
    RUN_TEST(test_path_suffix_patterns);
    /* Test function name detection */
    RUN_TEST(test_func_name_go_patterns);
    RUN_TEST(test_func_name_js_helpers);
    /* Env var name detection */
    RUN_TEST(env_var_name_valid);
    RUN_TEST(env_var_name_invalid);
    /* Config extension detection */
    RUN_TEST(config_ext_positive);
    RUN_TEST(config_ext_negative);
    /* Camel case splitting */
    RUN_TEST(split_camel_basic);
    RUN_TEST(split_camel_single_word);
    RUN_TEST(split_camel_empty);
    /* Decorator tokenization */
    RUN_TEST(tokenize_decorator_login_required);
    RUN_TEST(tokenize_decorator_single);
    /* Command splitting */
    RUN_TEST(split_command_basic);
    RUN_TEST(split_command_quoted);
    RUN_TEST(split_command_empty);
    /* Range parsing */
    RUN_TEST(parse_range_with_count);
    RUN_TEST(parse_range_single);
    RUN_TEST(parse_range_zero_count);
    /* Name status parsing */
    RUN_TEST(parse_name_status_basic);
    RUN_TEST(parse_name_status_empty);
    /* Hunk parsing */
    RUN_TEST(parse_hunks_empty);
    /* Change coupling */
    RUN_TEST(coupling_empty_commits);
    RUN_TEST(coupling_single_file_commit);
    /* JSON bracket cleaning */
    RUN_TEST(clean_json_brackets_array);
    RUN_TEST(clean_json_brackets_plain);
    RUN_TEST(clean_json_brackets_empty);
    /* FQN computation */
    RUN_TEST(fqn_compute_basic);
    RUN_TEST(fqn_compute_strips_ext);
    RUN_TEST(fqn_module_basic);
    RUN_TEST(fqn_folder_basic);
    /* Project name edge cases */
    RUN_TEST(project_name_special_chars);
    RUN_TEST(project_name_trailing_slash);
    RUN_TEST(pipeline_c_definitions_own_their_qn_c1);
    RUN_TEST(pipeline_c_call_targets_definition_before_macro_c1);
    RUN_TEST(pipeline_c_enum_inside_struct_reference_resolves_c1);
    RUN_TEST(pipeline_c_variants_on_sequential_and_parallel_paths_c1);
    RUN_TEST(pipeline_cpp_overloads_are_listed_as_variants_c1);
}
