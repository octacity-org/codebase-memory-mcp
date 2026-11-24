/*
 * test_discover.c — Tests for directory skip logic, suffix filters, and file walk.
 *
 * RED phase: Tests define expected filtering behavior for the discover module.
 */
#include "test_framework.h"
#include "test_helpers.h"
#include "discover/discover.h"

typedef struct {
    char *home;
    char *xdg_config_home;
} git_env_snapshot_t;

static git_env_snapshot_t save_git_env(void) {
    git_env_snapshot_t snapshot = {0};
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    snapshot.home = home ? cbm_strdup(home) : NULL;
    snapshot.xdg_config_home = xdg ? cbm_strdup(xdg) : NULL;
    return snapshot;
}

static void restore_git_env(git_env_snapshot_t *snapshot) {
    if (snapshot->home) {
        cbm_setenv("HOME", snapshot->home, 1);
        free(snapshot->home);
    } else {
        cbm_unsetenv("HOME");
    }

    if (snapshot->xdg_config_home) {
        cbm_setenv("XDG_CONFIG_HOME", snapshot->xdg_config_home, 1);
        free(snapshot->xdg_config_home);
    } else {
        cbm_unsetenv("XDG_CONFIG_HOME");
    }
}

static bool discover_has_rel_path(const cbm_file_info_t *files, int count, const char *rel_path) {
    for (int i = 0; i < count; i++) {
        if (strcmp(files[i].rel_path, rel_path) == 0) {
            return true;
        }
    }
    return false;
}

static bool discover_excluded_contains(char **excluded, int count, const char *rel_path) {
    for (int i = 0; i < count; i++) {
        if (strcmp(excluded[i], rel_path) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Directory skip (always skipped) ───────────────────────────── */

TEST(skip_git) {
    ASSERT_TRUE(cbm_should_skip_dir(".git", CBM_MODE_FULL));
    PASS();
}
TEST(skip_node_modules) {
    ASSERT_TRUE(cbm_should_skip_dir("node_modules", CBM_MODE_FULL));
    PASS();
}
TEST(skip_pycache) {
    ASSERT_TRUE(cbm_should_skip_dir("__pycache__", CBM_MODE_FULL));
    PASS();
}
TEST(skip_venv) {
    ASSERT_TRUE(cbm_should_skip_dir("venv", CBM_MODE_FULL));
    PASS();
}
TEST(skip_dist) {
    ASSERT_TRUE(cbm_should_skip_dir("dist", CBM_MODE_FULL));
    PASS();
}
TEST(skip_target) {
    ASSERT_TRUE(cbm_should_skip_dir("target", CBM_MODE_FULL));
    PASS();
}
TEST(skip_vendor) {
    ASSERT_TRUE(cbm_should_skip_dir("vendor", CBM_MODE_FULL));
    PASS();
}
TEST(skip_vendored) {
    ASSERT_TRUE(cbm_should_skip_dir("vendored", CBM_MODE_FULL));
    PASS();
}
TEST(skip_terraform) {
    ASSERT_TRUE(cbm_should_skip_dir(".terraform", CBM_MODE_FULL));
    PASS();
}
TEST(skip_coverage) {
    ASSERT_TRUE(cbm_should_skip_dir("coverage", CBM_MODE_FULL));
    PASS();
}
TEST(skip_idea) {
    ASSERT_TRUE(cbm_should_skip_dir(".idea", CBM_MODE_FULL));
    PASS();
}
TEST(skip_claude) {
    ASSERT_TRUE(cbm_should_skip_dir(".claude", CBM_MODE_FULL));
    PASS();
}

/* Not skipped in full mode */
TEST(no_skip_src) {
    ASSERT_FALSE(cbm_should_skip_dir("src", CBM_MODE_FULL));
    PASS();
}
TEST(no_skip_lib) {
    ASSERT_FALSE(cbm_should_skip_dir("lib", CBM_MODE_FULL));
    PASS();
}
TEST(no_skip_docs_full) {
    ASSERT_FALSE(cbm_should_skip_dir("docs", CBM_MODE_FULL));
    PASS();
}
TEST(no_skip_test_full) {
    ASSERT_FALSE(cbm_should_skip_dir("__tests__", CBM_MODE_FULL));
    PASS();
}

/* Fast mode additional skips */
TEST(skip_fast_docs) {
    ASSERT_TRUE(cbm_should_skip_dir("docs", CBM_MODE_FAST));
    PASS();
}
TEST(skip_fast_examples) {
    ASSERT_TRUE(cbm_should_skip_dir("examples", CBM_MODE_FAST));
    PASS();
}
TEST(skip_fast_tests) {
    ASSERT_TRUE(cbm_should_skip_dir("__tests__", CBM_MODE_FAST));
    PASS();
}
TEST(skip_fast_fixtures) {
    ASSERT_TRUE(cbm_should_skip_dir("fixtures", CBM_MODE_FAST));
    PASS();
}
TEST(skip_fast_testdata) {
    ASSERT_TRUE(cbm_should_skip_dir("testdata", CBM_MODE_FAST));
    PASS();
}
TEST(skip_fast_generated) {
    ASSERT_TRUE(cbm_should_skip_dir("generated", CBM_MODE_FAST));
    PASS();
}
TEST(skip_fast_assets) {
    ASSERT_TRUE(cbm_should_skip_dir("assets", CBM_MODE_FAST));
    PASS();
}
TEST(skip_fast_3rdparty) {
    ASSERT_TRUE(cbm_should_skip_dir("third_party", CBM_MODE_FAST));
    PASS();
}
TEST(skip_fast_e2e) {
    ASSERT_TRUE(cbm_should_skip_dir("e2e", CBM_MODE_FAST));
    PASS();
}

/* ── Suffix filters ────────────────────────────────────────────── */

TEST(suffix_pyc) {
    ASSERT_TRUE(cbm_has_ignored_suffix("module.pyc", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_o) {
    ASSERT_TRUE(cbm_has_ignored_suffix("main.o", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_so) {
    ASSERT_TRUE(cbm_has_ignored_suffix("lib.so", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_png) {
    ASSERT_TRUE(cbm_has_ignored_suffix("icon.png", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_jpg) {
    ASSERT_TRUE(cbm_has_ignored_suffix("photo.jpg", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_wasm) {
    ASSERT_TRUE(cbm_has_ignored_suffix("app.wasm", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_db) {
    ASSERT_TRUE(cbm_has_ignored_suffix("data.db", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_sqlite) {
    ASSERT_TRUE(cbm_has_ignored_suffix("store.sqlite3", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_tmp) {
    ASSERT_TRUE(cbm_has_ignored_suffix("file.tmp", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_tilde) {
    ASSERT_TRUE(cbm_has_ignored_suffix("file~", CBM_MODE_FULL));
    PASS();
}

/* Not ignored */
TEST(suffix_go) {
    ASSERT_FALSE(cbm_has_ignored_suffix("main.go", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_py) {
    ASSERT_FALSE(cbm_has_ignored_suffix("app.py", CBM_MODE_FULL));
    PASS();
}
TEST(suffix_c) {
    ASSERT_FALSE(cbm_has_ignored_suffix("lib.c", CBM_MODE_FULL));
    PASS();
}

/* Fast mode additional suffixes */
TEST(suffix_fast_zip) {
    ASSERT_TRUE(cbm_has_ignored_suffix("archive.zip", CBM_MODE_FAST));
    PASS();
}
TEST(suffix_fast_pdf) {
    ASSERT_TRUE(cbm_has_ignored_suffix("manual.pdf", CBM_MODE_FAST));
    PASS();
}
TEST(suffix_fast_mp3) {
    ASSERT_TRUE(cbm_has_ignored_suffix("sound.mp3", CBM_MODE_FAST));
    PASS();
}
TEST(suffix_fast_pem) {
    ASSERT_TRUE(cbm_has_ignored_suffix("cert.pem", CBM_MODE_FAST));
    PASS();
}

/* ── Filename skip (fast mode) ─────────────────────────────────── */

TEST(fn_skip_license) {
    ASSERT_TRUE(cbm_should_skip_filename("LICENSE", CBM_MODE_FAST));
    PASS();
}
TEST(fn_skip_changelog) {
    ASSERT_TRUE(cbm_should_skip_filename("CHANGELOG.md", CBM_MODE_FAST));
    PASS();
}
TEST(fn_skip_gosum) {
    ASSERT_TRUE(cbm_should_skip_filename("go.sum", CBM_MODE_FAST));
    PASS();
}
TEST(fn_skip_yarnlock) {
    ASSERT_TRUE(cbm_should_skip_filename("yarn.lock", CBM_MODE_FAST));
    PASS();
}
TEST(fn_skip_pkglock) {
    ASSERT_TRUE(cbm_should_skip_filename("package-lock.json", CBM_MODE_FAST));
    PASS();
}

/* Not skipped in full mode */
TEST(fn_no_skip_license_full) {
    ASSERT_FALSE(cbm_should_skip_filename("LICENSE", CBM_MODE_FULL));
    PASS();
}

/* ── Fast mode patterns ────────────────────────────────────────── */

TEST(pattern_dts) {
    ASSERT_TRUE(cbm_matches_fast_pattern("types.d.ts", CBM_MODE_FAST));
    PASS();
}
TEST(pattern_pbgo) {
    ASSERT_TRUE(cbm_matches_fast_pattern("service.pb.go", CBM_MODE_FAST));
    PASS();
}
TEST(pattern_pb2py) {
    ASSERT_TRUE(cbm_matches_fast_pattern("api_pb2.py", CBM_MODE_FAST));
    PASS();
}
TEST(pattern_mock) {
    ASSERT_TRUE(cbm_matches_fast_pattern("mock_service.go", CBM_MODE_FAST));
    PASS();
}
TEST(pattern_test_dot) {
    ASSERT_TRUE(cbm_matches_fast_pattern("App.test.js", CBM_MODE_FAST));
    PASS();
}
TEST(pattern_spec) {
    ASSERT_TRUE(cbm_matches_fast_pattern("App.spec.ts", CBM_MODE_FAST));
    PASS();
}
TEST(pattern_stories) {
    ASSERT_TRUE(cbm_matches_fast_pattern("Button.stories.tsx", CBM_MODE_FAST));
    PASS();
}

/* Not matched in full mode */
TEST(pattern_dts_full) {
    ASSERT_FALSE(cbm_matches_fast_pattern("types.d.ts", CBM_MODE_FULL));
    PASS();
}

/* ── File discovery (integration) — cross-platform via test_helpers.h ── */

TEST(discover_simple) {
    char *base = th_mktempdir("cbm_disc_simple");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");
    th_write_file(TH_PATH(base, "src/app.py"), "print(1)\n");
    th_write_file(TH_PATH(base, "src/icon.png"), "binary\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 2); /* main.go + app.py, not icon.png */

    bool found_go = false, found_py = false;
    for (int i = 0; i < count; i++) {
        if (files[i].language == CBM_LANG_GO)
            found_go = true;
        if (files[i].language == CBM_LANG_PYTHON)
            found_py = true;
    }
    ASSERT_TRUE(found_go);
    ASSERT_TRUE(found_py);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_skips_git_dir) {
    char *base = th_mktempdir("cbm_disc_git");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".git/config"), "x\n");
    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_with_gitignore) {
    char *base = th_mktempdir("cbm_disc_gi");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "*.log\n");
    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");
    th_write_file(TH_PATH(base, "src/debug.log"), "error\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(files[0].language, CBM_LANG_GO);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_with_global_xdg_ignore) {
    git_env_snapshot_t env = save_git_env();
    char *tmp = th_mktempdir("cbm_disc_global_xdg");
    ASSERT(tmp != NULL);

    char base[512], repo[512], home[512], xdg[512], xdg_env[512];
    snprintf(base, sizeof(base), "%s", tmp);
    snprintf(repo, sizeof(repo), "%s/repo", base);
    snprintf(home, sizeof(home), "%s/home", base);
    snprintf(xdg, sizeof(xdg), "%s/xdg", base);
    snprintf(xdg_env, sizeof(xdg_env), "%s/", xdg);

    cbm_setenv("HOME", home, 1);
    cbm_setenv("XDG_CONFIG_HOME", xdg_env, 1);

    th_mkdir_p(TH_PATH(repo, ".git"));
    th_write_file(TH_PATH(repo, ".git/config"), "[core]\n");
    th_write_file(TH_PATH(xdg, "git/ignore"), "secret.go\nignored_dir/\n");
    th_write_file(TH_PATH(repo, "main.go"), "package main\n");
    th_write_file(TH_PATH(repo, "secret.go"), "package secret\n");
    th_write_file(TH_PATH(repo, "ignored_dir/thing.go"), "package ignored\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(repo, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(discover_has_rel_path(files, count, "main.go"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "secret.go"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "ignored_dir/thing.go"));

    cbm_discover_free(files, count);
    restore_git_env(&env);
    th_cleanup(base);
    PASS();
}

TEST(discover_global_excludesfile_from_gitconfig_tilde) {
    git_env_snapshot_t env = save_git_env();
    char *tmp = th_mktempdir("cbm_disc_global_cfg");
    ASSERT(tmp != NULL);

    char base[512], repo[512], home[512];
    snprintf(base, sizeof(base), "%s", tmp);
    snprintf(repo, sizeof(repo), "%s/repo", base);
    snprintf(home, sizeof(home), "%s/home", base);

    cbm_setenv("HOME", home, 1);
    cbm_unsetenv("XDG_CONFIG_HOME");

    th_mkdir_p(TH_PATH(repo, ".git"));
    th_write_file(TH_PATH(repo, ".git/config"), "[core]\n");
    th_write_file(TH_PATH(home, ".gitconfig"), "[core]\n    excludesFile = ~/custom-ignore\n");
    th_write_file(TH_PATH(home, "custom-ignore"), "skip-me.go\n");
    th_write_file(TH_PATH(repo, "keep.go"), "package keep\n");
    th_write_file(TH_PATH(repo, "skip-me.go"), "package skip\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(repo, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(discover_has_rel_path(files, count, "keep.go"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "skip-me.go"));

    cbm_discover_free(files, count);
    restore_git_env(&env);
    th_cleanup(base);
    PASS();
}

TEST(discover_repo_local_excludesfile_is_ignored) {
    git_env_snapshot_t env = save_git_env();
    char *tmp = th_mktempdir("cbm_disc_repo_cfg_ignored");
    ASSERT(tmp != NULL);

    char base[512], repo[512], home[512], secret[512], config[1024];
    snprintf(base, sizeof(base), "%s", tmp);
    snprintf(repo, sizeof(repo), "%s/repo", base);
    snprintf(home, sizeof(home), "%s/home", base);
    snprintf(secret, sizeof(secret), "%s/secret-ignore", home);
    snprintf(config, sizeof(config), "[core]\n    excludesFile = %s\n", secret);

    cbm_setenv("HOME", home, 1);
    cbm_unsetenv("XDG_CONFIG_HOME");

    th_mkdir_p(TH_PATH(repo, ".git"));
    th_write_file(TH_PATH(repo, ".git/config"), config);
    th_write_file(secret, "skip-me.go\n");
    th_write_file(TH_PATH(repo, "keep.go"), "package keep\n");
    th_write_file(TH_PATH(repo, "skip-me.go"), "package skip\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(repo, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(discover_has_rel_path(files, count, "keep.go"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "skip-me.go"));

    cbm_discover_free(files, count);
    restore_git_env(&env);
    th_cleanup(base);
    PASS();
}

TEST(discover_missing_global_excludes_is_noop) {
    git_env_snapshot_t env = save_git_env();
    char *tmp = th_mktempdir("cbm_disc_global_missing");
    ASSERT(tmp != NULL);

    char base[512], repo[512], home[512];
    snprintf(base, sizeof(base), "%s", tmp);
    snprintf(repo, sizeof(repo), "%s/repo", base);
    snprintf(home, sizeof(home), "%s/home", base);

    cbm_setenv("HOME", home, 1);
    cbm_unsetenv("XDG_CONFIG_HOME");

    th_mkdir_p(TH_PATH(repo, ".git"));
    th_write_file(TH_PATH(repo, ".git/config"), "[core]\n");
    th_write_file(TH_PATH(repo, "main.go"), "package main\n");
    th_write_file(TH_PATH(repo, "would-be-global.go"), "package global\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(repo, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(discover_has_rel_path(files, count, "main.go"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "would-be-global.go"));

    cbm_discover_free(files, count);
    restore_git_env(&env);
    th_cleanup(base);
    PASS();
}

TEST(discover_cbmignore_negates_global_ignore) {
    git_env_snapshot_t env = save_git_env();
    char *tmp = th_mktempdir("cbm_disc_global_neg");
    ASSERT(tmp != NULL);

    char base[512], repo[512], home[512], xdg[512];
    snprintf(base, sizeof(base), "%s", tmp);
    snprintf(repo, sizeof(repo), "%s/repo", base);
    snprintf(home, sizeof(home), "%s/home", base);
    snprintf(xdg, sizeof(xdg), "%s/xdg", base);

    cbm_setenv("HOME", home, 1);
    cbm_setenv("XDG_CONFIG_HOME", xdg, 1);

    th_mkdir_p(TH_PATH(repo, ".git"));
    th_write_file(TH_PATH(repo, ".git/config"), "[core]\n");
    th_write_file(TH_PATH(xdg, "git/ignore"), "rescued.go\nblocked.go\n");
    th_write_file(TH_PATH(repo, ".cbmignore"), "!rescued.go\n");
    th_write_file(TH_PATH(repo, "main.go"), "package main\n");
    th_write_file(TH_PATH(repo, "rescued.go"), "package rescued\n");
    th_write_file(TH_PATH(repo, "blocked.go"), "package blocked\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(repo, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(discover_has_rel_path(files, count, "main.go"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "rescued.go"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "blocked.go"));

    cbm_discover_free(files, count);
    restore_git_env(&env);
    th_cleanup(base);
    PASS();
}

/* issue #234: a directory listed in the root .gitignore (e.g. "vendor/") must
 * be excluded from discovery even when untracked — Composer/PHP projects rely
 * on this. */
TEST(discover_gitignore_dir_excluded_issue234) {
    char *base = th_mktempdir("cbm_disc_gi234");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "vendor/\n");
    th_write_file(TH_PATH(base, "src/main.php"), "<?php\nfunction appmain() {}\n");
    th_write_file(TH_PATH(base, "vendor/autoload.php"), "<?php\nfunction autoload() {}\n");
    th_write_file(TH_PATH(base, "vendor/pkg/lib.php"), "<?php\nfunction lib() {}\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    /* Nothing under vendor/ should be discovered. */
    for (int i = 0; i < count; i++) {
        ASSERT_TRUE(strstr(files[i].rel_path, "vendor") == NULL);
    }
    ASSERT_EQ(count, 1); /* only src/main.php */

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* ── Tracked files are never git-ignored (upstream a90076a3) ───── */

#ifndef _WIN32
/* Run a git command inside dir, return 0 on success. */
static int disc_git(const char *dir, const char *args) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "git -C \"%s\" -c user.name=t -c user.email=t@t.io -c init.defaultBranch=main %s "
             ">/dev/null 2>&1",
             dir, args);
    return system(cmd);
}

/* A real repository whose .gitignore matches tracked files: `manager` (a
 * directory holding tracked files, one of them a level deeper) and
 * `*.gen.go` (one tracked, one not). Returns false when git cannot build it. */
static bool disc_tracked_fixture(const char *base) {
    if (disc_git(base, "init -q") != 0) {
        return false;
    }
    th_write_file(TH_PATH(base, ".gitignore"), "manager\n*.gen.go\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n\nfunc main() {}\n");
    th_write_file(TH_PATH(base, "config/manager/manager.go"), "package manager\n\nfunc Run() {}\n");
    th_write_file(TH_PATH(base, "config/manager/scratch.go"), "package manager\n\nfunc S() {}\n");
    th_write_file(TH_PATH(base, "config/manager/sub/deep.go"), "package sub\n\nfunc Deep() {}\n");
    th_write_file(TH_PATH(base, "config/manager/tmpdir/build.go"),
                  "package tmpdir\n\nfunc B() {}\n");
    th_write_file(TH_PATH(base, "api/types.gen.go"), "package api\n\nfunc Gen() {}\n");
    th_write_file(TH_PATH(base, "api/other.gen.go"), "package api\n\nfunc Other() {}\n");
    return disc_git(base, "add .gitignore main.go") == 0 &&
           disc_git(base, "add -f config/manager/manager.go config/manager/sub/deep.go "
                          "api/types.gen.go") == 0;
}
#endif

/* A .gitignore never hides a file git tracks: git applies ignore rules to
 * untracked paths only. cluster-api's nested `.gitignore` line `manager`,
 * meant for a built binary, hid its tracked config/manager/ kustomization.
 * Untracked ignored files stay out, also inside a directory that is entered
 * only for its tracked content. */
TEST(discover_gitignore_never_hides_tracked_files_issue55) {
#ifdef _WIN32
    SKIP_PLATFORM("git-based fixture not supported on Windows CI");
#else
    char *base = th_mktempdir("cbm_disc_tracked");
    ASSERT(base != NULL);
    if (!disc_tracked_fixture(base)) {
        th_cleanup(base);
        SKIP_PLATFORM("git not available to build the fixture");
    }

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    bool main_found = rc == 0 && discover_has_rel_path(files, count, "main.go");
    bool manager_found =
        rc == 0 && discover_has_rel_path(files, count, "config/manager/manager.go");
    bool deep_found = rc == 0 && discover_has_rel_path(files, count, "config/manager/sub/deep.go");
    bool gen_found = rc == 0 && discover_has_rel_path(files, count, "api/types.gen.go");
    bool scratch_found =
        rc == 0 && discover_has_rel_path(files, count, "config/manager/scratch.go");
    bool build_found =
        rc == 0 && discover_has_rel_path(files, count, "config/manager/tmpdir/build.go");
    bool other_found = rc == 0 && discover_has_rel_path(files, count, "api/other.gen.go");
    cbm_discover_free(files, count);
    th_cleanup(base);

    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(main_found);
    /* Tracked: kept although .gitignore matches them or a directory above. */
    ASSERT_TRUE(manager_found);
    ASSERT_TRUE(deep_found);
    ASSERT_TRUE(gen_found);
    /* Untracked and ignored: still out, also below the entered directory. */
    ASSERT_FALSE(scratch_found);
    ASSERT_FALSE(build_found);
    ASSERT_FALSE(other_found);
    PASS();
#endif
}

/* When git cannot run, the tracked-file lookup yields nothing and the ignore
 * rules apply exactly as before: the git-ignored manager/ directory and the
 * ignored *.gen.go files stay out, tracked or not. */
TEST(discover_tracked_lookup_without_git_keeps_old_rules_issue55) {
#ifdef _WIN32
    SKIP_PLATFORM("git-based fixture not supported on Windows CI");
#else
    char *base = th_mktempdir("cbm_disc_tracked_nogit");
    ASSERT(base != NULL);
    if (!disc_tracked_fixture(base)) {
        th_cleanup(base);
        SKIP_PLATFORM("git not available to build the fixture");
    }

    const char *path_env = getenv("PATH");
    char *saved_path = path_env ? cbm_strdup(path_env) : NULL;
    cbm_setenv("PATH", TH_PATH(base, "no-such-bin"), 1);
    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    if (saved_path) {
        cbm_setenv("PATH", saved_path, 1);
        free(saved_path);
    } else {
        cbm_unsetenv("PATH");
    }
    bool main_found = rc == 0 && discover_has_rel_path(files, count, "main.go");
    bool manager_found =
        rc == 0 && discover_has_rel_path(files, count, "config/manager/manager.go");
    bool gen_found = rc == 0 && discover_has_rel_path(files, count, "api/types.gen.go");
    int found = rc == 0 ? count : -1;
    cbm_discover_free(files, count);
    th_cleanup(base);

    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(main_found);
    ASSERT_FALSE(manager_found);
    ASSERT_FALSE(gen_found);
    ASSERT_EQ(found, 1);
    PASS();
#endif
}

TEST(discover_max_file_size) {
    char *base = th_mktempdir("cbm_disc_size");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "small.go"), "small\n");
    /* Create a large file (> 1KB) */
    char bigpath[512];
    snprintf(bigpath, sizeof(bigpath), "%s/big.go", base);
    FILE *f = fopen(bigpath, "w");
    ASSERT(f != NULL);
    for (int i = 0; i < 200; i++) {
        fprintf(f, "// padding line %d to exceed 1KB\n", i);
    }
    fclose(f);

    cbm_discover_opts_t opts = {0};
    opts.max_file_size = 1024;
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_null_path) {
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(NULL, NULL, &files, &count);
    ASSERT_EQ(rc, -1);
    PASS();
}

TEST(discover_nonexistent_path) {
    char *base = th_mktempdir("cbm_disc_noexist");
    char fake[512];
    snprintf(fake, sizeof(fake), "%s/nonexistent_12345", base ? base : "/tmp");
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(fake, NULL, &files, &count);
    ASSERT_EQ(rc, -1);
    th_cleanup(base);
    PASS();
}

TEST(discover_free_null) {
    cbm_discover_free(NULL, 0);
    PASS();
}

TEST(discover_skips_worktrees) {
    char *base = th_mktempdir("cbm_disc_wt");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");
    th_write_file(TH_PATH(base, ".worktrees/feature/src/app.go"), "package app\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);

    bool found_main = false;
    for (int i = 0; i < count; i++) {
        if (strstr(files[i].rel_path, "main.go"))
            found_main = true;
        ASSERT_NULL(strstr(files[i].rel_path, ".worktrees"));
    }
    ASSERT_TRUE(found_main);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* ── Wide-directory discovery (growing walk stack, #17 / upstream 03dc9c91) ── */

/* The walk stack GROWS on demand (upstream 03dc9c91): a directory with more
 * immediate subdirectories than the initial 512-frame capacity must still be
 * indexed COMPLETELY — wide fanout is an indexing failure, not a depth guard
 * (dotnet/runtime carries 855 sibling dirs in one JIT regression folder).
 * Every file is discovered, nothing is reported excluded, and the walk is
 * not degraded. */
TEST(discover_wide_fanout_indexed_completely) {
    char *base = th_mktempdir("cbm_disc_wide_stack");
    ASSERT(base != NULL);

    /* 600 subdirs, one file each: 512 initial frames cannot hold them all. */
    enum { SUBDIRS = 600 };
    char dir[4096];
    char file[4200];
    for (int i = 0; i < SUBDIRS; i++) {
        snprintf(dir, sizeof(dir), "%s/d%03d", base, i);
        ASSERT(th_mkdir_p(dir) == 0);
        snprintf(file, sizeof(file), "%s/f.go", dir);
        ASSERT(th_write_file(file, "package d\n") == 0);
    }

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    char **excluded = NULL;
    int excluded_count = 0;
    bool degraded = false;

    int rc = cbm_discover_ex(base, &opts, &files, &count, &excluded, &excluded_count, &degraded);
    ASSERT_EQ(rc, 0);
    /* Complete walk: stack grew (512 → 1024), nothing dropped. */
    ASSERT_FALSE(degraded);
    ASSERT_EQ(count, SUBDIRS);
    ASSERT_EQ(excluded_count, 0);
    for (int i = 0; i < count; i++) {
        ASSERT_TRUE(strstr(files[i].rel_path, "/f.go") != NULL);
    }

    cbm_discover_free_excluded(excluded, excluded_count);
    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Repeated growth: the fanout must survive MULTIPLE doublings (512 → 1024 →
 * 2048), which also proves reallocation preserves queued sibling frames
 * (the popped parent is a local copy, so nothing dangles mid-walk). */
TEST(discover_stack_grows_repeatedly) {
    char *base = th_mktempdir("cbm_disc_stack_grow2");
    ASSERT(base != NULL);

    enum { SUBDIRS = 1300 }; /* crosses both the 512 and 1024 boundaries */
    char dir[4096];
    char file[4200];
    for (int i = 0; i < SUBDIRS; i++) {
        snprintf(dir, sizeof(dir), "%s/d%04d", base, i);
        ASSERT(th_mkdir_p(dir) == 0);
        snprintf(file, sizeof(file), "%s/f.go", dir);
        ASSERT(th_write_file(file, "package d\n") == 0);
    }

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    bool degraded = false;

    int rc = cbm_discover_ex(base, &opts, &files, &count, NULL, NULL, &degraded);
    ASSERT_EQ(rc, 0);
    ASSERT_FALSE(degraded);
    ASSERT_EQ(count, SUBDIRS);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Growth must not disturb nested-gitignore inheritance: the local gitignore
 * borrowed by QUEUED frames stays valid across realloc, and deep subtrees
 * still honour their own .gitignore after the stack has grown. */
TEST(discover_wide_fanout_keeps_nested_gitignores) {
    char *base = th_mktempdir("cbm_disc_wide_gi");
    ASSERT(base != NULL);

    enum { SUBDIRS = 600 };
    char dir[4096];
    char file[4200];
    for (int i = 0; i < SUBDIRS; i++) {
        snprintf(dir, sizeof(dir), "%s/d%03d", base, i);
        ASSERT(th_mkdir_p(dir) == 0);
        snprintf(file, sizeof(file), "%s/keep.go", dir);
        ASSERT(th_write_file(file, "package d\n") == 0);
    }
    /* A nested .gitignore deep inside the fanout excludes generated files. */
    char nested[4096];
    snprintf(nested, sizeof(nested), "%s/d%03d", base, SUBDIRS - 1);
    th_write_file(TH_PATH(nested, ".gitignore"), "generated.go\n");
    th_write_file(TH_PATH(nested, "generated.go"), "package gen\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    bool degraded = false;

    int rc = cbm_discover_ex(base, &opts, &files, &count, NULL, NULL, &degraded);
    ASSERT_EQ(rc, 0);
    ASSERT_FALSE(degraded);
    /* One keep.go per subdir, generated.go excluded by the nested ignore. */
    ASSERT_EQ(count, SUBDIRS);
    for (int i = 0; i < count; i++) {
        ASSERT_NULL(strstr(files[i].rel_path, "generated.go"));
    }

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Every nested .gitignore applies, however many there are: past the old
 * fixed ownership cap (64) the rules were freed and the walk went on, so the
 * files they exclude were discovered. */
TEST(discover_many_nested_gitignores_all_apply) {
    char *base = th_mktempdir("cbm_disc_many_gi");
    ASSERT(base != NULL);

    enum { SUBDIRS = 80 };
    char dir[4096];
    for (int i = 0; i < SUBDIRS; i++) {
        snprintf(dir, sizeof(dir), "%s/d%03d", base, i);
        ASSERT(th_mkdir_p(dir) == 0);
        ASSERT(th_write_file(TH_PATH(dir, ".gitignore"), "secret.go\n") == 0);
        ASSERT(th_write_file(TH_PATH(dir, "keep.go"), "package d\n") == 0);
        ASSERT(th_write_file(TH_PATH(dir, "secret.go"), "package d\n") == 0);
    }

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    bool degraded = false;

    int rc = cbm_discover_ex(base, &opts, &files, &count, NULL, NULL, &degraded);
    ASSERT_EQ(rc, 0);
    ASSERT_FALSE(degraded);
    ASSERT_EQ(count, SUBDIRS);
    for (int i = 0; i < count; i++) {
        ASSERT_NULL(strstr(files[i].rel_path, "secret.go"));
    }

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* When a nested .gitignore cannot be tracked, its directory is dropped and
 * the walk is flagged degraded; it is never walked without its rules. */
TEST(discover_gitignore_grow_fault_drops_subtree) {
    char *base = th_mktempdir("cbm_disc_oom_gi");
    ASSERT(base != NULL);

    enum { SUBDIRS = 3 };
    char dir[4096];
    for (int i = 0; i < SUBDIRS; i++) {
        snprintf(dir, sizeof(dir), "%s/d%d", base, i);
        ASSERT(th_mkdir_p(dir) == 0);
        ASSERT(th_write_file(TH_PATH(dir, ".gitignore"), "secret.go\n") == 0);
        ASSERT(th_write_file(TH_PATH(dir, "keep.go"), "package d\n") == 0);
        ASSERT(th_write_file(TH_PATH(dir, "secret.go"), "package d\n") == 0);
    }

    cbm_setenv("CBM_DISCOVER_TEST_FAIL_PHASE", "gi_owned_grow", 1);
    cbm_setenv("CBM_DISCOVER_TEST_FAIL_NTH", "1", 1);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    bool degraded = false;

    int rc = cbm_discover_ex(base, &opts, &files, &count, NULL, NULL, &degraded);
    cbm_unsetenv("CBM_DISCOVER_TEST_FAIL_PHASE");
    cbm_unsetenv("CBM_DISCOVER_TEST_FAIL_NTH");
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(degraded);
    /* The first directory is dropped; the others keep only keep.go. */
    ASSERT_EQ(count, SUBDIRS - 1);
    for (int i = 0; i < count; i++) {
        ASSERT_NULL(strstr(files[i].rel_path, "secret.go"));
    }

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* OOM paths keep the walk coherent: the file entry is ROLLED BACK (never a
 * NULL-path record downstream), the walk is flagged degraded, and whatever
 * was discovered before the fault is still returned intact. */
TEST(discover_files_grow_fault_rolls_back_entry) {
    char *base = th_mktempdir("cbm_disc_oom_files");
    ASSERT(base != NULL);

    char file[4096];
    for (int i = 0; i < 257; i++) {
        snprintf(file, sizeof(file), "%s/f%03d.go", base, i);
        ASSERT(th_write_file(file, "package p\n") == 0);
    }

    cbm_setenv("CBM_DISCOVER_TEST_FAIL_PHASE", "files_grow", 1);
    cbm_setenv("CBM_DISCOVER_TEST_FAIL_NTH", "1", 1);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    bool degraded = false;

    int rc = cbm_discover_ex(base, &opts, &files, &count, NULL, NULL, &degraded);
    cbm_unsetenv("CBM_DISCOVER_TEST_FAIL_PHASE");
    cbm_unsetenv("CBM_DISCOVER_TEST_FAIL_NTH");
    ASSERT_EQ(rc, 0);
    /* The 257th file triggers the 256 -> 512 grow and is rolled back. */
    ASSERT_TRUE(degraded);
    ASSERT_EQ(count, 256);
    for (int i = 0; i < count; i++) {
        ASSERT_NOT_NULL(files[i].path);
        ASSERT_NOT_NULL(files[i].rel_path);
    }

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* A failed stack grow must retain the original allocation and finish safely
 * with a degraded result. This catches fault injection performed after a
 * successful realloc, which would otherwise discard the moved pointer. */
TEST(discover_stack_grow_fault_preserves_allocation) {
    char *base = th_mktempdir("cbm_disc_oom_stack_grow");
    ASSERT(base != NULL);

    char dir[4096];
    char file[4200];
    for (int i = 0; i < 513; i++) {
        snprintf(dir, sizeof(dir), "%s/d%03d", base, i);
        ASSERT(th_mkdir_p(dir) == 0);
        snprintf(file, sizeof(file), "%s/f.go", dir);
        ASSERT(th_write_file(file, "package p\n") == 0);
    }

    cbm_setenv("CBM_DISCOVER_TEST_FAIL_PHASE", "stack_grow", 1);
    cbm_setenv("CBM_DISCOVER_TEST_FAIL_NTH", "1", 1);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    bool degraded = false;
    int rc = cbm_discover_ex(base, &opts, &files, &count, NULL, NULL, &degraded);

    cbm_unsetenv("CBM_DISCOVER_TEST_FAIL_PHASE");
    cbm_unsetenv("CBM_DISCOVER_TEST_FAIL_NTH");
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(degraded);
    ASSERT_EQ(count, 512);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* An excluded-list grow fault degrades the walk without losing files. */
TEST(discover_excluded_grow_fault_degrades_walk) {
    char *base = th_mktempdir("cbm_disc_oom_excl");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "a.go"), "package a\n");
    th_write_file(TH_PATH(base, "node_modules/dep.go"), "package dep\n");

    cbm_setenv("CBM_DISCOVER_TEST_FAIL_PHASE", "excluded_grow", 1);
    cbm_setenv("CBM_DISCOVER_TEST_FAIL_NTH", "1", 1);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    bool degraded = false;

    int rc = cbm_discover_ex(base, &opts, &files, &count, NULL, NULL, &degraded);
    cbm_unsetenv("CBM_DISCOVER_TEST_FAIL_PHASE");
    cbm_unsetenv("CBM_DISCOVER_TEST_FAIL_NTH");
    ASSERT_EQ(rc, 0);
    /* Files are unaffected; only the skip-point record was dropped. */
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(degraded);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* A custom CBM_CACHE_DIR may legitimately sit inside a repository — tests do it
 * routinely, and nothing forbids it in production. Walking into it would pull
 * other workspaces' databases and cache state into this project's file list
 * (#17 boundary: never traverse the shared CBM cache as repository input).
 * "cache" is deliberately not on the built-in skip list, so the walk prunes
 * the configured cache tree by absolute path (upstream 7814dacf). */
TEST(discover_prunes_the_cache_tree) {
    char *base = th_mktempdir("cbm_disc_cache");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");
    /* Source-looking files inside the cache must not be discovered. */
    th_write_file(TH_PATH(base, "cache/other_workspace/leaked.go"), "package leaked\n");

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    char cache_dir[1024];
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", base);
    cbm_setenv("CBM_CACHE_DIR", cache_dir, 1);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);

    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }

    ASSERT_EQ(rc, 0);
    for (int i = 0; i < count; i++) {
        ASSERT(strstr(files[i].rel_path, "leaked.go") == NULL);
    }
    ASSERT_EQ(count, 1); /* only src/main.go */

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* The cache prune must not swallow a directory that merely shares a prefix:
 * "<cache>-alt" is a sibling, not a child of "<cache>". */
TEST(discover_cache_prune_is_boundary_aware) {
    char *base = th_mktempdir("cbm_disc_cachebx");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");
    th_write_file(TH_PATH(base, "cache-alt/kept.go"), "package kept\n");

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    char cache_dir[1024];
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", base);
    cbm_setenv("CBM_CACHE_DIR", cache_dir, 1);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);

    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 2); /* src/main.go AND cache-alt/kept.go survive */

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* The configured cache path may contain aliases such as "..". Compare
 * canonical paths so the same directory is still pruned. */
TEST(discover_prunes_canonical_cache_alias) {
    char *base = th_mktempdir("cbm_disc_cache_alias");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");
    th_write_file(TH_PATH(base, "cache/other_workspace/leaked.go"), "package leaked\n");
    ASSERT(th_mkdir_p(TH_PATH(base, "alias_anchor")) == 0);

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    char cache_alias[1024];
    snprintf(cache_alias, sizeof(cache_alias), "%s/alias_anchor/../cache", base);
    cbm_setenv("CBM_CACHE_DIR", cache_alias, 1);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);

    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }

    ASSERT_EQ(rc, 0);
    ASSERT_FALSE(discover_has_rel_path(files, count, "cache/other_workspace/leaked.go"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "src/main.go"));

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* A walk-stack calloc fault degrades to an empty-but-flagged result: the
 * caller must not mistake the empty file list for an empty tree. */
TEST(discover_stack_calloc_fault_flags_empty_result) {
    char *base = th_mktempdir("cbm_disc_oom_stack");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "a.go"), "package a\n");

    cbm_setenv("CBM_DISCOVER_TEST_FAIL_PHASE", "stack_calloc", 1);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    bool degraded = false;

    int rc = cbm_discover_ex(base, &opts, &files, &count, NULL, NULL, &degraded);
    cbm_unsetenv("CBM_DISCOVER_TEST_FAIL_PHASE");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 0);
    ASSERT_TRUE(degraded); /* empty result, NOT an empty tree */

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* A normal shallow tree is NOT degraded, and a NULL degraded_out stays legal. */
TEST(discover_not_degraded_on_complete_walk) {
    char *base = th_mktempdir("cbm_disc_degraded_ok");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "a.go"), "package a\n");
    th_write_file(TH_PATH(base, "sub/b.go"), "package b\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    bool degraded = true;

    int rc = cbm_discover_ex(base, &opts, &files, &count, NULL, NULL, &degraded);
    ASSERT_EQ(rc, 0);
    ASSERT_FALSE(degraded);
    ASSERT_EQ(count, 2);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_cbmignore) {
    char *base = th_mktempdir("cbm_disc_cbmi");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".cbmignore"), "generated/\n*.pb.go\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, "generated/types.go"), "package gen\n");
    th_write_file(TH_PATH(base, "api.pb.go"), "package api\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(strstr(files[0].rel_path, "main.go") != NULL);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_cbmignore_stacks) {
    char *base = th_mktempdir("cbm_disc_stack");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "*.log\n");
    th_write_file(TH_PATH(base, ".cbmignore"), "docs/\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, "docs/api.go"), "package docs\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);

    bool found_docs = false;
    for (int i = 0; i < count; i++) {
        if (strstr(files[i].rel_path, "docs/"))
            found_docs = true;
    }
    ASSERT_FALSE(found_docs);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_symlink_skipped) {
#ifdef _WIN32
    /* Symlinks require elevated privileges on Windows — skip.
     * Guard the entire body: symlink() doesn't exist on Windows. */
    SKIP_PLATFORM("Windows: symlinks need admin / symlink() unavailable");
#else
    char *base = th_mktempdir("cbm_disc_sym");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "real.go"), "package main\n");
    char real_path[512], link_path[512];
    snprintf(real_path, sizeof(real_path), "%s/real.go", base);
    snprintf(link_path, sizeof(link_path), "%s/link.go", base);
    symlink(real_path, link_path);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);

    bool found_real = false, found_link = false;
    for (int i = 0; i < count; i++) {
        if (strstr(files[i].rel_path, "real.go"))
            found_real = true;
        if (strstr(files[i].rel_path, "link.go"))
            found_link = true;
    }
    ASSERT_TRUE(found_real);
    ASSERT_FALSE(found_link);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
#endif
}

TEST(discover_new_ignore_patterns) {
    char *base = th_mktempdir("cbm_disc_newign");
    ASSERT(base != NULL);

    const char *dirs[] = {".next", ".terraform", "zig-cache", ".cargo", "elm-stuff", "bazel-out"};
    for (int i = 0; i < 6; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s/file.go", base, dirs[i]);
        th_write_file(path, "package x\n");
    }
    th_write_file(TH_PATH(base, "main.go"), "package main\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(strstr(files[0].rel_path, "main.go") != NULL);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_generic_dirs_full_mode) {
    char *base = th_mktempdir("cbm_disc_genfull");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "bin/main.go"), "package bin\n");
    th_write_file(TH_PATH(base, "build/main.go"), "package build\n");
    th_write_file(TH_PATH(base, "out/main.go"), "package out\n");

    cbm_discover_opts_t opts = {.mode = CBM_MODE_FULL};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 3);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_generic_dirs_fast_mode) {
    char *base = th_mktempdir("cbm_disc_genfast");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "bin/main.go"), "package bin\n");
    th_write_file(TH_PATH(base, "build/main.go"), "package build\n");
    th_write_file(TH_PATH(base, "out/main.go"), "package out\n");

    cbm_discover_opts_t opts = {.mode = CBM_MODE_FAST};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 0);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_deploy_excluded_full_mode) {
    char *base = th_mktempdir("cbm_disc_deploy");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");
    th_write_file(TH_PATH(base, "deploy/main.go"), "package deploy\n");
    th_write_file(TH_PATH(base, "deployed/main.go"), "package deployed\n");

    cbm_discover_opts_t opts = {.mode = CBM_MODE_FULL};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_cbmignore_no_git) {
    char *base = th_mktempdir("cbm_disc_nogit");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, ".cbmignore"), "scratch/\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, "scratch/tmp.go"), "package scratch\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(strstr(files[0].rel_path, "main.go") != NULL);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* ── .cbmignore negation vs built-in skip dirs (issue #500) ────── */

/* A "!obj/" negation in .cbmignore must un-skip the built-in ALWAYS_SKIP
 * "obj" dir so files inside it get discovered — and the un-skipped dir must
 * not be reported as an excluded subtree (#411 list stays coherent). */
TEST(discover_cbmignore_negates_always_skip_dir) {
    char *base = th_mktempdir("cbm_disc_cbmi_neg_obj");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, ".cbmignore"), "!obj/\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, "obj/generated.go"), "package obj\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    char **excluded = NULL;
    int excluded_count = 0;

    int rc = cbm_discover_ex(base, &opts, &files, &count, &excluded, &excluded_count, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(discover_has_rel_path(files, count, "main.go"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "obj/generated.go"));
    ASSERT_FALSE(discover_excluded_contains(excluded, excluded_count, "obj"));

    cbm_discover_free_excluded(excluded, excluded_count);
    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* ── Laravel compiled Blade view cache (issue #1735) ─────────────── */

/* storage/framework/views/ holds Laravel's compiled Blade templates: generated
 * PHP with no source value that the php_only grammar flags as parse damage.
 * It is skipped by default even when Laravel's stock .gitignore in that folder
 * is missing. The match is on the path suffix, component-aligned, so ordinary
 * views/ dirs (resources/views, app/views, app/framework/views) and look-alike
 * prefixes (mystorage/...) stay indexed. */
TEST(discover_skips_laravel_compiled_views_issue1735) {
    char *base = th_mktempdir("cbm_disc_laravel_views");
    ASSERT(base != NULL);

    /* No .gitignore anywhere: the skip must come from the built-in rule. */
    th_write_file(TH_PATH(base, "storage/framework/views/abc123.php"),
                  "<?php $__env->startSection('title'); ?>\n"
                  "<?php echo e($title); ?>\n"
                  "<?php $__env->stopSection(); ?>\n");
    th_write_file(TH_PATH(base, "backend/storage/framework/views/def456.php"),
                  "<?php echo e($x); ?>\n");
    th_write_file(TH_PATH(base, "resources/views/welcome.blade.php"), "<h1>{{ $title }}</h1>\n");
    th_write_file(TH_PATH(base, "app/views/x.php"), "<?php function x() { return 1; }\n");
    th_write_file(TH_PATH(base, "app/framework/views/y.php"), "<?php function y() {}\n");
    th_write_file(TH_PATH(base, "mystorage/framework/views/z.php"), "<?php function z() {}\n");
    th_write_file(TH_PATH(base, "storage/framework/views_old/w.php"), "<?php function w() {}\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    char **excluded = NULL;
    int excluded_count = 0;

    int rc = cbm_discover_ex(base, &opts, &files, &count, &excluded, &excluded_count, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_FALSE(discover_has_rel_path(files, count, "storage/framework/views/abc123.php"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "backend/storage/framework/views/def456.php"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "resources/views/welcome.blade.php"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "app/views/x.php"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "app/framework/views/y.php"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "mystorage/framework/views/z.php"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "storage/framework/views_old/w.php"));
    ASSERT_EQ(count, 5);
    /* Reported as an excluded subtree (#411), like every built-in skip dir. */
    ASSERT_TRUE(discover_excluded_contains(excluded, excluded_count, "storage/framework/views"));
    ASSERT_TRUE(
        discover_excluded_contains(excluded, excluded_count, "backend/storage/framework/views"));
    ASSERT_FALSE(discover_excluded_contains(excluded, excluded_count, "app/views"));
    ASSERT_FALSE(discover_excluded_contains(excluded, excluded_count, "resources/views"));

    cbm_discover_free_excluded(excluded, excluded_count);
    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Like the other ordinary built-in skip dirs (#500), a .cbmignore negation
 * un-skips the compiled-view cache for a user who really wants it indexed. */
TEST(discover_cbmignore_negates_laravel_compiled_views_issue1735) {
    char *base = th_mktempdir("cbm_disc_laravel_views_neg");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, ".cbmignore"), "!storage/framework/views/\n");
    th_write_file(TH_PATH(base, "storage/framework/views/abc123.php"), "<?php echo 1; ?>\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(discover_has_rel_path(files, count, "storage/framework/views/abc123.php"));

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* An anchored negation ("!src/target/") un-skips only that nested dir; other
 * dirs with the same basename stay built-in-skipped. */
TEST(discover_cbmignore_negates_only_nested_skip_dir) {
    char *base = th_mktempdir("cbm_disc_cbmi_neg_nested");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, ".cbmignore"), "!src/target/\n");
    th_write_file(TH_PATH(base, "src/main.go"), "package src\n");
    th_write_file(TH_PATH(base, "src/target/lib.go"), "package target\n");
    th_write_file(TH_PATH(base, "other/target/lib.go"), "package other\n");
    th_write_file(TH_PATH(base, "target/root.go"), "package root\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(discover_has_rel_path(files, count, "src/main.go"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "src/target/lib.go"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "other/target/lib.go"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "target/root.go"));

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Negation also un-skips FAST-mode skip dirs ("docs" is in FAST_SKIP_DIRS). */
TEST(discover_cbmignore_negates_fast_skip_dir) {
    char *base = th_mktempdir("cbm_disc_cbmi_neg_fast");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, ".cbmignore"), "!docs/\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, "docs/guide.go"), "package docs\n");

    cbm_discover_opts_t opts = {.mode = CBM_MODE_FAST};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(discover_has_rel_path(files, count, "main.go"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "docs/guide.go"));

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Last-match-wins ordering: "obj/" then "!obj/" un-skips; "!obj/" then
 * "obj/" re-ignores, so the built-in skip stands. */
TEST(discover_cbmignore_negation_last_match_wins) {
    char *base = th_mktempdir("cbm_disc_cbmi_order1");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, ".cbmignore"), "obj/\n!obj/\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, "obj/generated.go"), "package obj\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(discover_has_rel_path(files, count, "obj/generated.go"));

    cbm_discover_free(files, count);
    th_cleanup(base);

    base = th_mktempdir("cbm_disc_cbmi_order2");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, ".cbmignore"), "!obj/\nobj/\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, "obj/generated.go"), "package obj\n");

    files = NULL;
    count = 0;

    rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(discover_has_rel_path(files, count, "main.go"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "obj/generated.go"));

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Safety-core policy (#489, #802): .git, node_modules, and the
 * worktree-internal dirs can never be un-skipped, even by an explicit
 * .cbmignore negation. Green by construction; guards the policy. */
TEST(discover_cbmignore_negation_cannot_unskip_safety_core) {
    char *base = th_mktempdir("cbm_disc_cbmi_safety");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, ".cbmignore"),
                  "!.git/\n!node_modules/\n!.worktrees/\n!.claude-worktrees/\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, ".git/hooks/hook.go"), "package hooks\n");
    th_write_file(TH_PATH(base, "node_modules/pkg/index.js"), "module.exports = 1;\n");
    th_write_file(TH_PATH(base, ".worktrees/wt/dup.go"), "package dup\n");
    th_write_file(TH_PATH(base, ".claude-worktrees/wt/dup.go"), "package dup\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;

    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(discover_has_rel_path(files, count, "main.go"));
    ASSERT_FALSE(discover_has_rel_path(files, count, ".git/hooks/hook.go"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "node_modules/pkg/index.js"));
    ASSERT_FALSE(discover_has_rel_path(files, count, ".worktrees/wt/dup.go"));
    ASSERT_FALSE(discover_has_rel_path(files, count, ".claude-worktrees/wt/dup.go"));

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* ── .git/info/exclude tests (issue #489) ─────────────────────── */

/* Per-clone excludes written to .git/info/exclude (not committed) must be
 * honored the same as .gitignore.  Without this, repos that keep worktrees
 * under a path excluded only via info/exclude hit OOM during indexing. */
TEST(discover_git_info_exclude) {
    char *base = th_mktempdir("cbm_disc_exc");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git/info"));
    th_write_file(TH_PATH(base, ".git/info/exclude"), "worktrees/\n");
    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");
    th_write_file(TH_PATH(base, "worktrees/feature/app.go"), "package app\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(strstr(files[0].rel_path, "main.go") != NULL);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_git_info_exclude_stacks_with_gitignore) {
    char *base = th_mktempdir("cbm_disc_exc_stack");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git/info"));
    th_write_file(TH_PATH(base, ".gitignore"), "*.log\n");
    th_write_file(TH_PATH(base, ".git/info/exclude"), "scratch/\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, "debug.log"), "log\n");
    th_write_file(TH_PATH(base, "scratch/tmp.go"), "package scratch\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);

    bool found_log = false;
    bool found_scratch = false;
    for (int i = 0; i < count; i++) {
        if (strstr(files[i].rel_path, ".log"))    found_log     = true;
        if (strstr(files[i].rel_path, "scratch")) found_scratch = true;
    }
    ASSERT_FALSE(found_log);
    ASSERT_FALSE(found_scratch);
    ASSERT_TRUE(strstr(files[0].rel_path, "main.go") != NULL);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* ── Linked-worktree ignore tests ──────────────────────────────── */

/* In a linked worktree, <repo>/.git is a regular file ("gitdir: <path>"), not a
 * directory, and info/exclude + config live in the shared common dir named by
 * <gitdir>/commondir. The discover step must follow the gitlink so per-clone
 * excludes are honored exactly as in an ordinary checkout. Regression for the
 * worktree gap left by issue #489 (which only handled .git as a directory). */
TEST(discover_worktree_info_exclude) {
    char *base = th_mktempdir("cbm_disc_wt_exc");
    ASSERT(base != NULL);

    /* Worktree gitlink -> per-worktree gitdir (relative to the worktree root). */
    th_write_file(TH_PATH(base, ".git"), "gitdir: maingit/worktrees/wt\n");
    /* Per-worktree gitdir points back to the shared common dir via commondir. */
    th_mkdir_p(TH_PATH(base, "maingit/worktrees/wt"));
    th_write_file(TH_PATH(base, "maingit/worktrees/wt/commondir"), "../..\n");
    /* Shared exclude lives in the common dir, not the per-worktree gitdir. */
    th_mkdir_p(TH_PATH(base, "maingit/info"));
    th_write_file(TH_PATH(base, "maingit/info/exclude"), "build/\n");

    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");
    th_write_file(TH_PATH(base, "build/gen.go"), "package build\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(strstr(files[0].rel_path, "main.go") != NULL);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Committed .gitignore must still apply in a worktree even when commondir is
 * absent (older gitdir layouts): the gitlink resolver falls back to the gitdir
 * itself, and the root .gitignore is loaded unconditionally (issue #510). */
TEST(discover_worktree_committed_gitignore) {
    char *base = th_mktempdir("cbm_disc_wt_gi");
    ASSERT(base != NULL);

    th_write_file(TH_PATH(base, ".git"), "gitdir: maingit/worktrees/wt\n");
    th_mkdir_p(TH_PATH(base, "maingit/worktrees/wt"));
    th_write_file(TH_PATH(base, ".gitignore"), "build/\n");

    th_write_file(TH_PATH(base, "src/main.go"), "package main\n");
    th_write_file(TH_PATH(base, "build/gen.go"), "package build\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, 1);
    ASSERT_TRUE(strstr(files[0].rel_path, "main.go") != NULL);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* ── Enclosing-repository ignore rules (upstream 92910147) ─────── */

/* Discover `sub` below base and keep the result for assertions. */
static int disc_sub(const char *base, const char *sub, cbm_file_info_t **files, int *count) {
    cbm_discover_opts_t opts = {0};
    *files = NULL;
    *count = 0;
    return cbm_discover(TH_PATH(base, sub), &opts, files, count);
}

/* repo_path itself has no .git (indexing a git-less subfolder of a larger
 * repository). The enclosing repository's root .gitignore must still be
 * honored, as `git status` run from that subfolder would. */
TEST(discover_enclosing_repo_gitignore_issue55) {
    char *base = th_mktempdir("cbm_disc_enc_gi");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "secret.py\n");
    th_write_file(TH_PATH(base, "pkg/secret.py"), "TOKEN = 1\n");
    th_write_file(TH_PATH(base, "pkg/keep.py"), "pass\n");

    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = disc_sub(base, "pkg", &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(discover_has_rel_path(files, count, "keep.py"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "secret.py"));
    ASSERT_EQ(count, 1);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* The enclosing repository's <common>/info/exclude is honored the same way. */
TEST(discover_enclosing_repo_info_exclude_issue55) {
    char *base = th_mktempdir("cbm_disc_enc_exc");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git/info"));
    th_write_file(TH_PATH(base, ".git/info/exclude"), "scratch/\n");
    th_write_file(TH_PATH(base, "pkg/main.py"), "pass\n");
    th_write_file(TH_PATH(base, "pkg/scratch/tmp.py"), "pass\n");

    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = disc_sub(base, "pkg", &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(discover_has_rel_path(files, count, "main.py"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "scratch/tmp.py"));
    ASSERT_EQ(count, 1);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* The indexed directory's own .gitignore is deeper than the enclosing root's
 * and wins on conflict, negations included. */
TEST(discover_enclosing_repo_gitignore_local_overrides_issue55) {
    char *base = th_mktempdir("cbm_disc_enc_ovr");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "*.py\n");
    th_write_file(TH_PATH(base, "pkg/.gitignore"), "!keep.py\n");
    th_write_file(TH_PATH(base, "pkg/keep.py"), "pass\n");
    th_write_file(TH_PATH(base, "pkg/drop.py"), "pass\n");

    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = disc_sub(base, "pkg", &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(discover_has_rel_path(files, count, "keep.py"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "drop.py"));
    ASSERT_EQ(count, 1);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* An enclosing repository's patterns stay anchored to its own root:
 * `/secret.py` there means "secret.py in the ROOT", so git keeps
 * pkg/secret.py. Merged into pkg's matcher it would hide it. */
TEST(discover_enclosing_rooted_pattern_not_reanchored_issue55) {
    char *base = th_mktempdir("cbm_disc_enc_anchor");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "/secret.py\n");
    th_write_file(TH_PATH(base, "pkg/secret.py"), "TOKEN = 1\n");
    th_write_file(TH_PATH(base, "pkg/keep.py"), "pass\n");

    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = disc_sub(base, "pkg", &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(discover_has_rel_path(files, count, "secret.py"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "keep.py"));
    ASSERT_EQ(count, 2);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* The mirror case: a pattern the enclosing repository anchors THROUGH the
 * indexed subfolder ("pkg/scratch/") still excludes pkg/scratch. */
TEST(discover_enclosing_info_exclude_rooted_subpath_issue55) {
    char *base = th_mktempdir("cbm_disc_enc_exc_sub");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git/info"));
    th_write_file(TH_PATH(base, ".git/info/exclude"), "pkg/scratch/\n");
    th_write_file(TH_PATH(base, "pkg/main.py"), "pass\n");
    th_write_file(TH_PATH(base, "pkg/scratch/tmp.py"), "pass\n");

    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = disc_sub(base, "pkg", &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(discover_has_rel_path(files, count, "main.py"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "scratch/tmp.py"));
    ASSERT_EQ(count, 1);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* git consults the .gitignore of EVERY directory between the enclosing root
 * and the indexed one. Only <root>/a/.gitignore has an opinion here, rooted
 * at a/, so it must be loaded and matched relative to a/. */
TEST(discover_enclosing_intermediate_gitignore_issue55) {
    char *base = th_mktempdir("cbm_disc_enc_mid");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, "a/.gitignore"), "/b/drop.py\n");
    th_write_file(TH_PATH(base, "a/b/drop.py"), "pass\n");
    th_write_file(TH_PATH(base, "a/b/keep.py"), "pass\n");

    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = disc_sub(base, "a/b", &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(discover_has_rel_path(files, count, "keep.py"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "drop.py"));
    ASSERT_EQ(count, 1);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Among ancestors the deeper .gitignore wins over the shallower one,
 * negations included: <root>/.gitignore ignores every *.py and
 * <root>/a/.gitignore re-includes b/keep.py. */
TEST(discover_enclosing_deeper_ancestor_negation_wins_issue55) {
    char *base = th_mktempdir("cbm_disc_enc_neg");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "*.py\n");
    th_write_file(TH_PATH(base, "a/.gitignore"), "!/b/keep.py\n");
    th_write_file(TH_PATH(base, "a/b/drop.py"), "pass\n");
    th_write_file(TH_PATH(base, "a/b/keep.py"), "pass\n");

    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = disc_sub(base, "a/b", &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(discover_has_rel_path(files, count, "keep.py"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "drop.py"));
    ASSERT_EQ(count, 1);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* ── Nested .gitignore tests (issue #178) ──────────────────────── */

TEST(discover_nested_gitignore) {
    char *base = th_mktempdir("cbm_disc_ngi");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, "webapp/.gitignore"), "generated/\n");
    th_write_file(TH_PATH(base, "webapp/src/routes.js"), "export default []\n");
    th_write_file(TH_PATH(base, "webapp/generated/types.js"), "export {}\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);

    bool found_generated = false;
    bool found_routes = false;
    for (int i = 0; i < count; i++) {
        if (strstr(files[i].rel_path, "generated"))
            found_generated = true;
        if (strstr(files[i].rel_path, "routes.js"))
            found_routes = true;
    }
    ASSERT_FALSE(found_generated);
    ASSERT_TRUE(found_routes);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

TEST(discover_nested_gitignore_stacks_with_root) {
    char *base = th_mktempdir("cbm_disc_ngi_stack");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "*.log\n");
    th_write_file(TH_PATH(base, "webapp/.gitignore"), ".output/\n");
    th_write_file(TH_PATH(base, "main.go"), "package main\n");
    th_write_file(TH_PATH(base, "error.log"), "error log\n");
    th_write_file(TH_PATH(base, "webapp/src/app.js"), "const x = 1\n");
    th_write_file(TH_PATH(base, "webapp/.output/data.js"), "output data\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);

    bool found_log = false;
    bool found_output = false;
    bool found_main = false;
    bool found_app = false;
    for (int i = 0; i < count; i++) {
        if (strstr(files[i].rel_path, ".log"))
            found_log = true;
        if (strstr(files[i].rel_path, ".output"))
            found_output = true;
        if (strstr(files[i].rel_path, "main.go"))
            found_main = true;
        if (strstr(files[i].rel_path, "app.js"))
            found_app = true;
    }
    ASSERT_FALSE(found_log);
    ASSERT_FALSE(found_output);
    ASSERT_TRUE(found_main);
    ASSERT_TRUE(found_app);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* ── Nested .gitignore below another .gitignore (upstream d3eb7e98) ── */

/* Laravel layout: storage/.gitignore exists (here: EMPTY) and
 * storage/dump/.gitignore contains "*". git ignores every file under
 * storage/dump/; the walk used to load only the SHALLOWEST nested .gitignore
 * on a path, so the deeper "*" was never consulted. */
TEST(discover_nested_gitignore_below_ancestor_gitignore_issue55) {
    char *base = th_mktempdir("cbm_disc_ngi_below");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "node_modules/\n");
    th_write_file(TH_PATH(base, "storage/.gitignore"), "");
    th_write_file(TH_PATH(base, "storage/dump/.gitignore"), "*\n");
    th_write_file(TH_PATH(base, "app/x.php"), "<?php\nfunction appFn() { return 1; }\n");
    th_write_file(TH_PATH(base, "storage/dump/00001.json"), "{\"route\": \"/api\"}\n");
    th_write_file(TH_PATH(base, "storage/dump/nested/deep.json"), "{\"deep\": true}\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    for (int i = 0; i < count; i++) {
        ASSERT(strstr(files[i].rel_path, "storage/dump/") == NULL);
    }
    ASSERT_TRUE(discover_has_rel_path(files, count, "app/x.php"));
    ASSERT_EQ(count, 1);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Precedence between .gitignore files on one path is git's: a deeper file
 * overrides a shallower one, and every ancestor's patterns still apply to the
 * subtree. A negation in the deepest file re-includes a file its ancestor
 * ignores; siblings it does not mention stay ignored; a root pattern still
 * reaches two levels down. */
TEST(discover_nested_gitignore_negation_overrides_ancestor_issue55) {
    char *base = th_mktempdir("cbm_disc_ngi_neg");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "*.log\n");
    th_write_file(TH_PATH(base, "storage/.gitignore"), "*.json\n");
    th_write_file(TH_PATH(base, "storage/dump/.gitignore"), "!keep.json\n");
    th_write_file(TH_PATH(base, "app/x.php"), "<?php\nfunction appFn() { return 1; }\n");
    th_write_file(TH_PATH(base, "storage/top.json"), "{\"top\": 1}\n");
    th_write_file(TH_PATH(base, "storage/dump/keep.json"), "{\"keep\": 1}\n");
    th_write_file(TH_PATH(base, "storage/dump/drop.json"), "{\"drop\": 1}\n");
    th_write_file(TH_PATH(base, "storage/dump/run.log"), "log line\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(discover_has_rel_path(files, count, "app/x.php"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "storage/dump/keep.json"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "storage/top.json"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "storage/dump/drop.json"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "storage/dump/run.log"));
    ASSERT_EQ(count, 2);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* Three nesting levels: the deepest file wins for the paths it names, an
 * intermediate negation reaches through an EMPTY .gitignore below it, and the
 * root pattern still governs everything outside the re-included subtree. */
TEST(discover_nested_gitignore_three_levels_deeper_file_wins_issue55) {
    char *base = th_mktempdir("cbm_disc_ngi_3lvl");
    ASSERT(base != NULL);

    th_mkdir_p(TH_PATH(base, ".git"));
    th_write_file(TH_PATH(base, ".gitignore"), "*.json\n");
    th_write_file(TH_PATH(base, "a/.gitignore"), "!*.json\n");
    th_write_file(TH_PATH(base, "a/b/.gitignore"), "");
    th_write_file(TH_PATH(base, "a/b/c/.gitignore"), "drop.json\n");
    th_write_file(TH_PATH(base, "main.py"), "x = 1\n");
    th_write_file(TH_PATH(base, "root.json"), "{\"root\": 1}\n");
    th_write_file(TH_PATH(base, "a/x.json"), "{\"a\": 1}\n");
    th_write_file(TH_PATH(base, "a/b/x.json"), "{\"b\": 1}\n");
    th_write_file(TH_PATH(base, "a/b/c/keep.json"), "{\"keep\": 1}\n");
    th_write_file(TH_PATH(base, "a/b/c/drop.json"), "{\"drop\": 1}\n");

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    int rc = cbm_discover(base, &opts, &files, &count);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(discover_has_rel_path(files, count, "main.py"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "a/x.json"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "a/b/x.json"));
    ASSERT_TRUE(discover_has_rel_path(files, count, "a/b/c/keep.json"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "root.json"));
    ASSERT_FALSE(discover_has_rel_path(files, count, "a/b/c/drop.json"));
    ASSERT_EQ(count, 4);

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────── */

/* Ported from upstream 5fe1f4f8. */
/* ── Shebang fallback for extensionless scripts (issue #1199) ────── */

/* Language detected for a discovered file by relative path, or CBM_LANG_COUNT
 * if the file was not indexed. */
static CBMLanguage discover_lang_of(const cbm_file_info_t *files, int count, const char *rel) {
    for (int i = 0; i < count; i++) {
        if (strcmp(files[i].rel_path, rel) == 0) {
            return files[i].language;
        }
    }
    return CBM_LANG_COUNT;
}

/* Write raw bytes (may contain embedded NUL) to base/rel. Parent must exist. */
static int write_bytes_file(const char *path, const void *data, size_t n) {
    FILE *f = cbm_fopen(path, "wb");
    if (!f) {
        return -1;
    }
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    return w == n ? 0 : -1;
}

/* Discover a single file with the given first-line content and report the
 * language it was classified as via *out_lang (CBM_LANG_COUNT if unindexed).
 * Returns true only when temp-dir creation, the file write, and discovery all
 * succeeded, so callers can assert setup success before asserting the language
 * -- otherwise a negative expectation (CBM_LANG_COUNT) could pass vacuously
 * when setup silently failed. */
static bool shebang_probe(const char *prefix, const char *rel, const char *content,
                          CBMLanguage *out_lang) {
    *out_lang = CBM_LANG_COUNT;
    char *base = th_mktempdir(prefix);
    if (!base) {
        return false;
    }
    bool ok = false;
    if (th_write_file(TH_PATH(base, rel), content) == 0) {
        cbm_discover_opts_t opts = {0};
        cbm_file_info_t *files = NULL;
        int count = 0;
        if (cbm_discover(base, &opts, &files, &count) == 0) {
            *out_lang = discover_lang_of(files, count, rel);
            ok = true;
        }
        cbm_discover_free(files, count);
    }
    th_cleanup(base);
    return ok;
}

TEST(shebang_direct_python_path) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_dpy", "bin/run", "#!/usr/bin/python3\nprint(1)\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_PYTHON);
    PASS();
}

TEST(shebang_env_python) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_epy", "bin/run", "#!/usr/bin/env python3\nprint(1)\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_PYTHON);
    PASS();
}

TEST(shebang_env_split_string_python_args) {
    CBMLanguage lang;
    ASSERT(
        shebang_probe("cbm_sb_spy", "bin/run", "#!/usr/bin/env -S python3 -u\nprint(1)\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_PYTHON);
    PASS();
}

TEST(shebang_versioned_python) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_vpy", "bin/run", "#!/usr/bin/python3.12\nprint(1)\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_PYTHON);
    PASS();
}

TEST(shebang_bash_direct) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_bash", "bin/run", "#!/bin/bash\necho hi\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_BASH);
    PASS();
}

TEST(shebang_env_node_javascript) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_node", "bin/run", "#!/usr/bin/env node\nconsole.log(1)\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_JAVASCRIPT);
    PASS();
}

TEST(shebang_crlf_first_line) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_crlf", "bin/run", "#!/usr/bin/env python3\r\nprint(1)\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_PYTHON);
    PASS();
}

/* Recognized extension stays authoritative even with a conflicting shebang. */
TEST(shebang_extension_authoritative) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_ext", "weird.py", "#!/bin/bash\nprint(1)\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_PYTHON);
    PASS();
}

/* Special filename stays authoritative even with a conflicting shebang. */
TEST(shebang_special_filename_authoritative) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_mk", "Makefile", "#!/usr/bin/env python3\nall:\n\techo\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_MAKEFILE);
    PASS();
}

TEST(shebang_plain_text_unindexed) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_txt", "notes", "just some plain text\nno shebang here\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_COUNT);
    PASS();
}

TEST(shebang_malformed_unindexed) {
    /* Missing '!' — not a shebang. */
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_mal", "notes", "#/usr/bin/python3\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_COUNT);
    PASS();
}

TEST(shebang_unknown_interpreter_unindexed) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_unk", "notes", "#!/usr/bin/frobnicate\ndata\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_COUNT);
    PASS();
}

/* Guard against arbitrary prefix matches (python-wrapper must NOT map). */
TEST(shebang_prefix_not_matched_unindexed) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_pw", "notes", "#!/usr/bin/python-wrapper\nx\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_COUNT);
    PASS();
}

/* Numeric version components must be non-empty (reject trailing/doubled dots). */
TEST(shebang_malformed_python_version_unindexed) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_pdot", "notes", "#!/usr/bin/python3.\nx\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_COUNT);
    ASSERT(shebang_probe("cbm_sb_pddot", "notes", "#!/usr/bin/python3..12\nx\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_COUNT);
    PASS();
}

/* env NAME=value assignment (with a slash in the value) must NOT be mistaken
 * for an interpreter: env would set the var, not run "python" (issue #1199,
 * Finding 2). */
TEST(shebang_env_assignment_not_matched_unindexed) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_asn", "notes",
                         "#!/usr/bin/env PYTHON=/usr/bin/python python-wrapper\nx\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_COUNT);
    /* Same shape behind -S must also stay unindexed. */
    ASSERT(shebang_probe("cbm_sb_asn2", "notes",
                         "#!/usr/bin/env -S PYTHON=/usr/bin/python python\nx\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_COUNT);
    PASS();
}

/* Unsupported env option forms (a leading '-' token that is not -S/
 * --split-string) must stay unindexed rather than be treated as the
 * interpreter (issue #1199, Finding 2). */
TEST(shebang_env_unsupported_option_unindexed) {
    CBMLanguage lang;
    ASSERT(shebang_probe("cbm_sb_eopt", "notes", "#!/usr/bin/env -i python3\nx\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_COUNT);
    ASSERT(shebang_probe("cbm_sb_eopt2", "notes", "#!/usr/bin/env -S -i python3\nx\n", &lang));
    ASSERT_EQ(lang, CBM_LANG_COUNT);
    PASS();
}

TEST(shebang_embedded_nul_unindexed) {
    char *base = th_mktempdir("cbm_sb_nul");
    ASSERT(base != NULL);
    /* NUL embedded within the first line -> fail closed. */
    static const char bytes[] = "#!/usr/bin/py\0thon3\nprint(1)\n";
    ASSERT_EQ(write_bytes_file(TH_PATH(base, "run"), bytes, sizeof(bytes) - 1), 0);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    ASSERT_EQ(cbm_discover(base, &opts, &files, &count), 0);
    ASSERT_FALSE(discover_has_rel_path(files, count, "run"));

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

/* A first line longer than the bounded read window (255 bytes) with no
 * newline inside that window must fail closed even when it begins with an
 * otherwise-valid Python shebang: the interpreter token is truncated, so the
 * file must remain unindexed (issue #1199, Finding 1). */
TEST(shebang_oversized_first_line_unindexed) {
    char *base = th_mktempdir("cbm_sb_long");
    ASSERT(base != NULL);

    /* "#!/usr/bin/python3 " + padding to push the newline past byte 255. */
    char content[512];
    int off = snprintf(content, sizeof(content), "#!/usr/bin/python3 ");
    ASSERT(off > 0 && off < (int)sizeof(content));
    for (int i = off; i < 400; i++) {
        content[i] = 'a';
    }
    content[400] = '\n';
    content[401] = '\0';
    ASSERT_EQ(write_bytes_file(TH_PATH(base, "run"), content, 401), 0);

    cbm_discover_opts_t opts = {0};
    cbm_file_info_t *files = NULL;
    int count = 0;
    ASSERT_EQ(cbm_discover(base, &opts, &files, &count), 0);
    ASSERT_FALSE(discover_has_rel_path(files, count, "run"));

    cbm_discover_free(files, count);
    th_cleanup(base);
    PASS();
}

    /* Shebang fallback for extensionless scripts (issue #1199) */

SUITE(discover) {
    /* Directory skip — always */
    RUN_TEST(skip_git);
    RUN_TEST(skip_node_modules);
    RUN_TEST(skip_pycache);
    RUN_TEST(skip_venv);
    RUN_TEST(skip_dist);
    RUN_TEST(skip_target);
    RUN_TEST(skip_vendor);
    RUN_TEST(skip_vendored);
    RUN_TEST(skip_terraform);
    RUN_TEST(skip_coverage);
    RUN_TEST(skip_idea);
    RUN_TEST(skip_claude);

    /* Not skipped */
    RUN_TEST(no_skip_src);
    RUN_TEST(no_skip_lib);
    RUN_TEST(no_skip_docs_full);
    RUN_TEST(no_skip_test_full);

    /* Fast mode directory skips */
    RUN_TEST(skip_fast_docs);
    RUN_TEST(skip_fast_examples);
    RUN_TEST(skip_fast_tests);
    RUN_TEST(skip_fast_fixtures);
    RUN_TEST(skip_fast_testdata);
    RUN_TEST(skip_fast_generated);
    RUN_TEST(skip_fast_assets);
    RUN_TEST(skip_fast_3rdparty);
    RUN_TEST(skip_fast_e2e);

    /* Suffix filters */
    RUN_TEST(suffix_pyc);
    RUN_TEST(suffix_o);
    RUN_TEST(suffix_so);
    RUN_TEST(suffix_png);
    RUN_TEST(suffix_jpg);
    RUN_TEST(suffix_wasm);
    RUN_TEST(suffix_db);
    RUN_TEST(suffix_sqlite);
    RUN_TEST(suffix_tmp);
    RUN_TEST(suffix_tilde);
    RUN_TEST(suffix_go);
    RUN_TEST(suffix_py);
    RUN_TEST(suffix_c);
    RUN_TEST(suffix_fast_zip);
    RUN_TEST(suffix_fast_pdf);
    RUN_TEST(suffix_fast_mp3);
    RUN_TEST(suffix_fast_pem);

    /* Filename skip */
    RUN_TEST(fn_skip_license);
    RUN_TEST(fn_skip_changelog);
    RUN_TEST(fn_skip_gosum);
    RUN_TEST(fn_skip_yarnlock);
    RUN_TEST(fn_skip_pkglock);
    RUN_TEST(fn_no_skip_license_full);

    /* Fast mode patterns */
    RUN_TEST(pattern_dts);
    RUN_TEST(pattern_pbgo);
    RUN_TEST(pattern_pb2py);
    RUN_TEST(pattern_mock);
    RUN_TEST(pattern_test_dot);
    RUN_TEST(pattern_spec);
    RUN_TEST(pattern_stories);
    RUN_TEST(pattern_dts_full);

    /* Integration tests (cross-platform) */
    RUN_TEST(discover_simple);
    RUN_TEST(discover_skips_git_dir);
    RUN_TEST(discover_with_gitignore);
    RUN_TEST(discover_with_global_xdg_ignore);
    RUN_TEST(discover_global_excludesfile_from_gitconfig_tilde);
    RUN_TEST(discover_repo_local_excludesfile_is_ignored);
    RUN_TEST(discover_missing_global_excludes_is_noop);
    RUN_TEST(discover_cbmignore_negates_global_ignore);
    RUN_TEST(discover_gitignore_dir_excluded_issue234);
    RUN_TEST(discover_gitignore_never_hides_tracked_files_issue55);
    RUN_TEST(discover_tracked_lookup_without_git_keeps_old_rules_issue55);
    RUN_TEST(discover_max_file_size);
    RUN_TEST(discover_null_path);
    RUN_TEST(discover_nonexistent_path);
    RUN_TEST(discover_free_null);

    /* Go test ports (cross-platform) */
    RUN_TEST(discover_skips_worktrees);
    RUN_TEST(discover_cbmignore);
    RUN_TEST(discover_cbmignore_stacks);
    RUN_TEST(discover_symlink_skipped);
    RUN_TEST(discover_new_ignore_patterns);
    RUN_TEST(discover_generic_dirs_full_mode);
    RUN_TEST(discover_generic_dirs_fast_mode);
    RUN_TEST(discover_deploy_excluded_full_mode);
    RUN_TEST(discover_cbmignore_no_git);

    /* .cbmignore negation vs built-in skip dirs (issue #500) */
    RUN_TEST(discover_cbmignore_negates_always_skip_dir);
    RUN_TEST(discover_cbmignore_negates_only_nested_skip_dir);
    RUN_TEST(discover_cbmignore_negates_fast_skip_dir);
    RUN_TEST(discover_cbmignore_negation_last_match_wins);
    RUN_TEST(discover_cbmignore_negation_cannot_unskip_safety_core);

    /* Laravel compiled Blade view cache (issue #1735) */
    RUN_TEST(discover_skips_laravel_compiled_views_issue1735);
    RUN_TEST(discover_cbmignore_negates_laravel_compiled_views_issue1735);

    /* .git/info/exclude support (issue #489) */
    RUN_TEST(discover_git_info_exclude);
    RUN_TEST(discover_git_info_exclude_stacks_with_gitignore);

    /* Linked-worktree ignore resolution (gitlink + commondir) */
    RUN_TEST(discover_worktree_info_exclude);
    RUN_TEST(discover_worktree_committed_gitignore);

    /* Enclosing-repository ignore rules (#55, upstream 92910147) */
    RUN_TEST(discover_enclosing_repo_gitignore_issue55);
    RUN_TEST(discover_enclosing_repo_info_exclude_issue55);
    RUN_TEST(discover_enclosing_repo_gitignore_local_overrides_issue55);
    RUN_TEST(discover_enclosing_rooted_pattern_not_reanchored_issue55);
    RUN_TEST(discover_enclosing_info_exclude_rooted_subpath_issue55);
    RUN_TEST(discover_enclosing_intermediate_gitignore_issue55);
    RUN_TEST(discover_enclosing_deeper_ancestor_negation_wins_issue55);

    /* Nested .gitignore tests (issue #178) */
    RUN_TEST(discover_nested_gitignore);
    RUN_TEST(discover_nested_gitignore_stacks_with_root);

    /* Nested .gitignore below another .gitignore (#55, upstream d3eb7e98) */
    RUN_TEST(discover_nested_gitignore_below_ancestor_gitignore_issue55);
    RUN_TEST(discover_nested_gitignore_negation_overrides_ancestor_issue55);
    RUN_TEST(discover_nested_gitignore_three_levels_deeper_file_wins_issue55);

    /* Cache-tree pruning (#17 boundary / upstream 7814dacf) */
    RUN_TEST(discover_prunes_the_cache_tree);
    RUN_TEST(discover_cache_prune_is_boundary_aware);
    RUN_TEST(discover_prunes_canonical_cache_alias);

    /* Wide-directory discovery (growing walk stack, #17 / upstream 03dc9c91) */
    RUN_TEST(discover_wide_fanout_indexed_completely);
    RUN_TEST(discover_stack_grows_repeatedly);
    RUN_TEST(discover_wide_fanout_keeps_nested_gitignores);
    RUN_TEST(discover_files_grow_fault_rolls_back_entry);
    RUN_TEST(discover_stack_grow_fault_preserves_allocation);
    RUN_TEST(discover_many_nested_gitignores_all_apply);
    RUN_TEST(discover_gitignore_grow_fault_drops_subtree);
    RUN_TEST(discover_excluded_grow_fault_degrades_walk);
    RUN_TEST(discover_stack_calloc_fault_flags_empty_result);
    RUN_TEST(discover_not_degraded_on_complete_walk);
    RUN_TEST(shebang_direct_python_path);
    RUN_TEST(shebang_env_python);
    RUN_TEST(shebang_env_split_string_python_args);
    RUN_TEST(shebang_versioned_python);
    RUN_TEST(shebang_bash_direct);
    RUN_TEST(shebang_env_node_javascript);
    RUN_TEST(shebang_crlf_first_line);
    RUN_TEST(shebang_extension_authoritative);
    RUN_TEST(shebang_special_filename_authoritative);
    RUN_TEST(shebang_plain_text_unindexed);
    RUN_TEST(shebang_malformed_unindexed);
    RUN_TEST(shebang_unknown_interpreter_unindexed);
    RUN_TEST(shebang_prefix_not_matched_unindexed);
    RUN_TEST(shebang_malformed_python_version_unindexed);
    RUN_TEST(shebang_env_assignment_not_matched_unindexed);
    RUN_TEST(shebang_env_unsupported_option_unindexed);
    RUN_TEST(shebang_embedded_nul_unindexed);
    RUN_TEST(shebang_oversized_first_line_unindexed);
}
