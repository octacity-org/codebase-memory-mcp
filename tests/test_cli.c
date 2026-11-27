/*
 * test_cli.c — Tests for CLI subcommands: install, uninstall, update, version.
 *
 * Port of Go test files:
 *   - cmd/codebase-memory-mcp/cli_test.go (11 tests)
 *   - cmd/codebase-memory-mcp/install_test.go (24 tests)
 *   - cmd/codebase-memory-mcp/update_test.go (5 tests)
 *   - internal/selfupdate/selfupdate_test.go (7 tests)
 *
 * Total: 47 Go tests → 47 C tests
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/sha256.h"
#include "../src/foundation/sha256_backend.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <cli/cli.h>
#include <mcp/mcp.h>
#include <foundation/yaml.h>
#include <yyjson/yyjson.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/wait.h> /* fork/waitpid: hook-augment arms a process-wide SIGALRM */
#endif
#include <sqlite3.h>
#include <zlib.h>

/* Helper: create a file with content */
static int write_test_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "%s", content);
    fclose(f);
    return 0;
}

/* Helper: read a file into static buffer */
static const char *read_test_file(const char *path) {
    static char buf[8192];
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Helper: mkdirp */
static int test_mkdirp(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            cbm_mkdir(tmp);
            *p = '/';
        }
    }
    return cbm_mkdir(tmp) == 0 || errno == EEXIST ? 0 : -1;
}

/* Helper: recursive remove */
static void test_rmdir_r(const char *path) {
    th_rmtree(path);
}

/* Helper: create tar.gz with a single file */
static unsigned char *create_test_targz(const char *filename, const unsigned char *content,
                                        int content_len, int *out_len) {
    /* Build tar data: 512-byte header + content padded to 512-byte boundary + 2x512 zero blocks */
    int data_blocks = (content_len + 511) / 512;
    int tar_size = 512 + data_blocks * 512 + 1024; /* header + data + end-of-archive */
    unsigned char *tar = calloc(1, (size_t)tar_size);
    if (!tar)
        return NULL;

    /* Filename (bytes 0-99) */
    strncpy((char *)tar, filename, 99);

    /* Mode (bytes 100-107): octal 0700 */
    memcpy(tar + 100, "0000700\0", 8);

    /* UID/GID (bytes 108-123): 0 */
    memcpy(tar + 108, "0000000\0", 8);
    memcpy(tar + 116, "0000000\0", 8);

    /* Size (bytes 124-135): octal */
    char size_str[12];
    snprintf(size_str, sizeof(size_str), "%011o", content_len);
    memcpy(tar + 124, size_str, 11);

    /* Mtime (bytes 136-147): 0 */
    memcpy(tar + 136, "00000000000\0", 12);

    /* Type flag (byte 156): '0' = regular file */
    tar[156] = '0';

    /* Checksum (bytes 148-155): compute over header with checksum field as spaces */
    memset(tar + 148, ' ', 8);
    unsigned int checksum = 0;
    for (int i = 0; i < 512; i++)
        checksum += tar[i];
    snprintf((char *)tar + 148, 7, "%06o", checksum);
    tar[154] = '\0';

    /* File content */
    memcpy(tar + 512, content, (size_t)content_len);

    /* Compress with gzip */
    z_stream strm = {0};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        free(tar);
        return NULL;
    }

    size_t gz_cap = (size_t)tar_size + 256;
    unsigned char *gz = malloc(gz_cap);
    if (!gz) {
        deflateEnd(&strm);
        free(tar);
        return NULL;
    }

    strm.next_in = tar;
    strm.avail_in = (unsigned int)tar_size;
    strm.next_out = gz;
    strm.avail_out = (unsigned int)gz_cap;

    deflate(&strm, Z_FINISH);
    *out_len = (int)(gz_cap - strm.avail_out);

    deflateEnd(&strm);
    free(tar);
    return gz;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Version comparison tests (port of selfupdate_test.go)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_compare_versions) {
    /* Port of TestCompareVersions — 13 cases */
    ASSERT(cbm_compare_versions("0.2.1", "0.2.0") > 0);
    ASSERT_EQ(cbm_compare_versions("0.2.0", "0.2.0"), 0);
    ASSERT(cbm_compare_versions("0.1.9", "0.2.0") < 0);
    ASSERT(cbm_compare_versions("0.10.0", "0.2.0") > 0);
    ASSERT(cbm_compare_versions("1.0.0", "0.99.99") > 0);
    ASSERT(cbm_compare_versions("0.0.1", "0.0.2") < 0);
    ASSERT_EQ(cbm_compare_versions("v0.2.1", "0.2.1"), 0);
    ASSERT_EQ(cbm_compare_versions("0.2.1", "v0.2.1"), 0);
    ASSERT(cbm_compare_versions("0.2.1-dev", "0.2.1") < 0);
    ASSERT(cbm_compare_versions("0.2.1", "0.2.1-dev") > 0);
    ASSERT_EQ(cbm_compare_versions("0.2.1-dev", "0.2.1-dev"), 0);
    ASSERT(cbm_compare_versions("0.3.0", "0.2.1-dev") > 0);
    ASSERT(cbm_compare_versions("0.2.0", "0.2.1-dev") < 0);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Version get/set (port of TestCLI_Version)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_version_get_set) {
    cbm_cli_set_version("1.2.3");
    ASSERT_STR_EQ(cbm_cli_get_version(), "1.2.3");
    cbm_cli_set_version("dev");
    ASSERT_STR_EQ(cbm_cli_get_version(), "dev");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Shell RC detection (port of TestDetectShellRC + BashWithBashrc)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_detect_shell_rc_zsh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-rc-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    /* Save and override SHELL — must strdup because setenv may realloc env block */
    const char *raw = getenv("SHELL");
    char *old_shell = raw ? strdup(raw) : NULL;
    cbm_setenv("SHELL", "/bin/zsh", 1);

    const char *rc = cbm_detect_shell_rc(tmpdir);
    ASSERT_NOT_NULL(rc);
    ASSERT(strstr(rc, ".zshrc") != NULL);

    if (old_shell) {
        cbm_setenv("SHELL", old_shell, 1);
        free(old_shell);
    } else
        cbm_unsetenv("SHELL");
    rmdir(tmpdir);
    PASS();
}

TEST(cli_detect_shell_rc_bash) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-rc-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *raw = getenv("SHELL");
    char *old_shell = raw ? strdup(raw) : NULL;
    cbm_setenv("SHELL", "/bin/bash", 1);

    /* No .bashrc → falls back to .bash_profile */
    const char *rc = cbm_detect_shell_rc(tmpdir);
    ASSERT_NOT_NULL(rc);
    ASSERT(strstr(rc, ".bash_profile") != NULL);

    if (old_shell) {
        cbm_setenv("SHELL", old_shell, 1);
        free(old_shell);
    } else
        cbm_unsetenv("SHELL");
    rmdir(tmpdir);
    PASS();
}

TEST(cli_detect_shell_rc_bash_with_bashrc) {
    /* Port of TestDetectShellRC_BashWithBashrc */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-rc-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *raw = getenv("SHELL");
    char *old_shell = raw ? strdup(raw) : NULL;
    cbm_setenv("SHELL", "/bin/bash", 1);

    /* Create .bashrc */
    char bashrc[512];
    snprintf(bashrc, sizeof(bashrc), "%s/.bashrc", tmpdir);
    write_test_file(bashrc, "# test\n");

    const char *rc = cbm_detect_shell_rc(tmpdir);
    ASSERT_STR_EQ(rc, bashrc);

    unlink(bashrc);
    if (old_shell) {
        cbm_setenv("SHELL", old_shell, 1);
        free(old_shell);
    } else
        cbm_unsetenv("SHELL");
    rmdir(tmpdir);
    PASS();
}

TEST(cli_detect_shell_rc_fish) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-rc-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *raw = getenv("SHELL");
    char *old_shell = raw ? strdup(raw) : NULL;
    cbm_setenv("SHELL", "/usr/bin/fish", 1);

    const char *rc = cbm_detect_shell_rc(tmpdir);
    ASSERT(strstr(rc, ".config/fish/config.fish") != NULL);

    if (old_shell) {
        cbm_setenv("SHELL", old_shell, 1);
        free(old_shell);
    } else
        cbm_unsetenv("SHELL");
    rmdir(tmpdir);
    PASS();
}

TEST(cli_detect_shell_rc_default) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-rc-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *raw = getenv("SHELL");
    char *old_shell = raw ? strdup(raw) : NULL;
    cbm_setenv("SHELL", "/bin/sh", 1);

    const char *rc = cbm_detect_shell_rc(tmpdir);
    ASSERT(strstr(rc, ".profile") != NULL);

    if (old_shell) {
        cbm_setenv("SHELL", old_shell, 1);
        free(old_shell);
    } else
        cbm_unsetenv("SHELL");
    rmdir(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  CLI binary detection (port of TestFindCLI_*)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_find_cli_not_found) {
    /* Port of TestFindCLI_NotFound */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-find-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *raw = getenv("PATH");
    char *old_path = raw ? strdup(raw) : NULL;
    cbm_setenv("PATH", tmpdir, 1);

    const char *result = cbm_find_cli("nonexistent-binary-xyz", tmpdir);
    ASSERT_STR_EQ(result, "");

    if (old_path) {
        cbm_setenv("PATH", old_path, 1);
        free(old_path);
    }
    rmdir(tmpdir);
    PASS();
}

TEST(cli_find_cli_on_path) {
    /* Port of TestFindCLI_FoundOnPATH */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-find-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char fakecli[512];
    snprintf(fakecli, sizeof(fakecli), "%s/fakecli", tmpdir);
    write_test_file(fakecli, "#!/bin/sh\n");
    th_make_executable(fakecli);

#ifdef _WIN32
    rmdir(tmpdir);
    SKIP_PLATFORM("Windows: PATH-based CLI lookup uses POSIX semantics");
#endif
    const char *raw = getenv("PATH");
    char *old_path = raw ? strdup(raw) : NULL;
    cbm_setenv("PATH", tmpdir, 1);

    const char *result = cbm_find_cli("fakecli", tmpdir);
    ASSERT(result[0] != '\0');
    ASSERT(strstr(result, "fakecli") != NULL);

    if (old_path) {
        cbm_setenv("PATH", old_path, 1);
        free(old_path);
    }
    unlink(fakecli);
    rmdir(tmpdir);
    PASS();
}

TEST(cli_find_cli_fallback_paths) {
    /* Port of TestFindCLI_FallbackPaths */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-find-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

#ifdef _WIN32
    rmdir(tmpdir);
    SKIP_PLATFORM("Windows: fallback path lookup uses POSIX semantics");
#endif
    char localbin[512];
    snprintf(localbin, sizeof(localbin), "%s/.local/bin", tmpdir);
    test_mkdirp(localbin);

    char fakecli[512];
    snprintf(fakecli, sizeof(fakecli), "%s/testcli", localbin);
    write_test_file(fakecli, "#!/bin/sh\n");
    th_make_executable(fakecli);

    const char *raw = getenv("PATH");
    char *old_path = raw ? strdup(raw) : NULL;
    cbm_setenv("PATH", "/nonexistent", 1);

    const char *result = cbm_find_cli("testcli", tmpdir);
    ASSERT_STR_EQ(result, fakecli);

    if (old_path) {
        cbm_setenv("PATH", old_path, 1);
        free(old_path);
    }
    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Dry-run flag parsing (port of TestDryRun)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_dry_run_flags) {
    /* Port of TestDryRun — just verifies the pattern */
    bool dry_run = false, force = false;
    const char *args[] = {"--dry-run", "--force"};
    for (int i = 0; i < 2; i++) {
        if (strcmp(args[i], "--dry-run") == 0)
            dry_run = true;
        if (strcmp(args[i], "--force") == 0)
            force = true;
    }
    ASSERT_TRUE(dry_run);
    ASSERT_TRUE(force);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Skill file management tests (port of install_test.go skill tests)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_skill_creation) {
    /* Port of TestInstallSkillCreation */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-skill-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    int written = cbm_install_skills(skills_dir, false, false);
    ASSERT_EQ(written, CBM_SKILL_COUNT);

    /* Verify all 4 skills exist and have content */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, sk[i].name);
        const char *data = read_test_file(path);
        ASSERT_NOT_NULL(data);
        ASSERT(strlen(data) > 0);
        /* Check YAML frontmatter */
        ASSERT(strncmp(data, "---\n", 4) == 0);
        /* Check name field */
        ASSERT(strstr(data, sk[i].name) != NULL);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_skill_idempotent) {
    /* Port of TestInstallIdempotent */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-skill-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    /* Install twice */
    cbm_install_skills(skills_dir, false, false);
    int second = cbm_install_skills(skills_dir, false, false);

    /* Second install should write 0 (skills exist, no force) */
    ASSERT_EQ(second, 0);

    /* All skills should still exist */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, sk[i].name);
        struct stat st;
        ASSERT_EQ(stat(path, &st), 0);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_skill_force_overwrite) {
    /* Port of TestCLI_InstallForceOverwrites */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-skill-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    cbm_install_skills(skills_dir, false, false);
    int force_count = cbm_install_skills(skills_dir, true, false);

    /* Force should overwrite all */
    ASSERT_EQ(force_count, CBM_SKILL_COUNT);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_uninstall_removes_skills) {
    /* Port of TestUninstallRemovesSkills */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-skill-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    cbm_install_skills(skills_dir, false, false);
    int removed = cbm_remove_skills(skills_dir, false);
    ASSERT_EQ(removed, CBM_SKILL_COUNT);

    /* Verify all removed */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", skills_dir, sk[i].name);
        struct stat st;
        ASSERT(stat(path, &st) != 0);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_remove_old_monolithic_skill) {
    /* Port of TestRemoveOldMonolithicSkill */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-skill-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    /* Create old monolithic skill */
    char old_dir[1024];
    snprintf(old_dir, sizeof(old_dir), "%s/codebase-memory-mcp", skills_dir);
    test_mkdirp(old_dir);
    char old_file[1024];
    snprintf(old_file, sizeof(old_file), "%s/SKILL.md", old_dir);
    write_test_file(old_file, "old skill");

    bool removed = cbm_remove_old_monolithic_skill(skills_dir, false);
    ASSERT_TRUE(removed);

    struct stat st;
    ASSERT(stat(old_dir, &st) != 0);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_skill_files_content) {
    /* Consolidated skill: all 4 former skills merged into one. */
    const cbm_skill_t *sk = cbm_get_skills();
    ASSERT_EQ(CBM_SKILL_COUNT, 1);
    ASSERT(strcmp(sk[0].name, "codebase-memory") == 0);

    /* Exploring capabilities */
    ASSERT(strstr(sk[0].content, "search_graph") != NULL);
    ASSERT(strstr(sk[0].content, "get_graph_schema") != NULL);

    /* Tracing capabilities */
    ASSERT(strstr(sk[0].content, "trace_path") != NULL);
    ASSERT(strstr(sk[0].content, "direction") != NULL);
    ASSERT(strstr(sk[0].content, "detect_changes") != NULL);

    /* Quality capabilities */
    ASSERT(strstr(sk[0].content, "max_degree=0") != NULL);
    ASSERT(strstr(sk[0].content, "exclude_entry_points") != NULL);

    /* Reference capabilities */
    ASSERT(strstr(sk[0].content, "query_graph") != NULL);
    ASSERT(strstr(sk[0].content, "Cypher") != NULL);
    char tools_header[64];
    snprintf(tools_header, sizeof(tools_header), "## %d MCP Tools", cbm_mcp_tool_count());
    ASSERT(strstr(sk[0].content, tools_header) != NULL);
    /* Strict YAML parsers reject an unquoted description containing ": ". */
    ASSERT(strstr(sk[0].content, "\ndescription: \"") != NULL);

    /* Gotchas section */
    ASSERT(strstr(sk[0].content, "Gotchas") != NULL);

    PASS();
}

/* #19: every tool in TOOLS[] appears in the installed skill, the --help
 * tool list and the README, and the skill's tool count is the registry's. */
TEST(cli_tool_registry_matches_docs) {
    FILE *f = fopen("README.md", "rb");
    if (!f) {
        FAIL("README.md not found (run from repo root)");
    }
    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    char *readme = malloc((size_t)size + 1);
    ASSERT_NOT_NULL(readme);
    size_t n = fread(readme, 1, (size_t)size, f);
    fclose(f);
    readme[n] = '\0';

    const cbm_skill_t *sk = cbm_get_skills();
    char *help = cbm_cli_tools_help();
    ASSERT_NOT_NULL(help);
    int count = cbm_mcp_tool_count();
    ASSERT(count > 0);
    char header[64];
    snprintf(header, sizeof(header), "## %d MCP Tools", count);
    ASSERT(strstr(sk[0].content, header) != NULL);
    for (int i = 0; i < count; i++) {
        const char *name = cbm_mcp_tool_name(i);
        char quoted[128];
        snprintf(quoted, sizeof(quoted), "`%s`", name);
        if (!strstr(sk[0].content, name) || !strstr(help, name) || !strstr(readme, quoted)) {
            printf("  tool %s missing: skill=%d help=%d readme=%d\n", name,
                   strstr(sk[0].content, name) != NULL, strstr(help, name) != NULL,
                   strstr(readme, quoted) != NULL);
            free(help);
            free(readme);
            FAIL("tool registry and docs disagree");
        }
    }
    free(help);
    free(readme);
    PASS();
}

/* #19 (upstream 1e6825f9): an existing opencode.jsonc is the install target,
 * not a second opencode.json. */
TEST(cli_install_plan_targets_existing_opencode_jsonc) {
    char tmpdir[512];
    snprintf(tmpdir, sizeof(tmpdir), "%s/cli-ocjsonc-XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");
    char path[768];
    snprintf(path, sizeof(path), "%s/.local/bin", tmpdir);
    test_mkdirp(path);
    snprintf(path, sizeof(path), "%s/.local/bin/opencode", tmpdir);
    write_test_file(path, "#!/bin/sh\n");
    chmod(path, 0755);
    snprintf(path, sizeof(path), "%s/.config/opencode", tmpdir);
    test_mkdirp(path);
    snprintf(path, sizeof(path), "%s/.config/opencode/opencode.jsonc", tmpdir);
    write_test_file(path, "// user config\n{\"theme\":\"dark\",}\n");

    char *json = cbm_build_install_plan_json(tmpdir, "/usr/local/bin/codebase-memory-mcp");
    ASSERT_NOT_NULL(json);
    bool targets_jsonc = strstr(json, "opencode/opencode.jsonc") != NULL;
    bool targets_json = strstr(json, "opencode/opencode.json\"") != NULL;
    free(json);

    /* The upsert accepts JSONC input and keeps the user's keys. */
    ASSERT_EQ(cbm_upsert_opencode_mcp("/usr/local/bin/codebase-memory-mcp", path), 0);
    const char *data = read_test_file(path);
    bool kept = data && strstr(data, "\"theme\"") && strstr(data, "codebase-memory-mcp");
    test_rmdir_r(tmpdir);
    ASSERT_TRUE(targets_jsonc);
    ASSERT_FALSE(targets_json);
    ASSERT_TRUE(kept);
    PASS();
}

/* #19 (upstream 33d1ecea): reinstalling replaces only the fields we own in
 * our entry; client-added keys and other servers survive. */
TEST(cli_upsert_keeps_client_keys_in_our_entry) {
    char tmpdir[512];
    snprintf(tmpdir, sizeof(tmpdir), "%s/cli-keepkeys-XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");
    char path[768];
    snprintf(path, sizeof(path), "%s/mcp.json", tmpdir);
    write_test_file(path, "{\"mcpServers\":{\"codebase-memory-mcp\":{\"command\":\"/old/cbm\","
                          "\"disabled\":false,\"env\":{\"A\":\"1\"}},"
                          "\"other\":{\"command\":\"/usr/bin/other\"}},\"keep\":1}");
    ASSERT_EQ(cbm_install_editor_mcp("/new/cbm", path), 0);
    const char *data = read_test_file(path);
    ASSERT_NOT_NULL(data);
    bool ok = strstr(data, "/new/cbm") && !strstr(data, "/old/cbm") &&
              strstr(data, "\"disabled\"") && strstr(data, "\"A\"") &&
              strstr(data, "/usr/bin/other") && strstr(data, "\"keep\"");
    test_rmdir_r(tmpdir);
    ASSERT_TRUE(ok);
    PASS();
}

/* #19 (upstream 33d1ecea): a config that exists but does not parse is left
 * untouched instead of being replaced. */
TEST(cli_upsert_refuses_malformed_config) {
    char tmpdir[512];
    snprintf(tmpdir, sizeof(tmpdir), "%s/cli-badcfg-XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");
    char path[768];
    snprintf(path, sizeof(path), "%s/mcp.json", tmpdir);
    const char *broken = "{\"mcpServers\": {\"other\": ";
    write_test_file(path, broken);
    int rc = cbm_install_editor_mcp("/new/cbm", path);
    const char *data = read_test_file(path);
    bool untouched = data && strcmp(data, broken) == 0;
    test_rmdir_r(tmpdir);
    ASSERT(rc != 0);
    ASSERT_TRUE(untouched);
    PASS();
}

TEST(cli_codex_instructions) {
    /* Port of TestCodexInstructionsCreation */
    const char *instr = cbm_get_codex_instructions();
    ASSERT_NOT_NULL(instr);
    ASSERT(strstr(instr, "Codebase Knowledge Graph") != NULL);
    ASSERT(strstr(instr, "trace_path") != NULL);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Editor MCP config tests (Cursor/Windsurf/Gemini)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_editor_mcp_install) {
    /* Port of TestEditorMCPInstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.cursor/mcp.json", tmpdir);

    int rc = cbm_install_editor_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "mcpServers") != NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_editor_mcp_idempotent) {
    /* Port of TestEditorMCPInstallIdempotent */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.cursor/mcp.json", tmpdir);

    cbm_install_editor_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    int rc = cbm_install_editor_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    /* Should still parse as valid JSON with only 1 server */
    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Count occurrences of "codebase-memory-mcp" (should be exactly 1 in mcpServers) */
    int count = 0;
    const char *p = data;
    while ((p = strstr(p, "\"codebase-memory-mcp\"")) != NULL) {
        count++;
        p += 20;
    }
    /* The key appears once as key name */
    ASSERT_EQ(count, 1);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_editor_mcp_preserves_others) {
    /* Port of TestEditorMCPPreservesOtherServers */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.cursor/mcp.json", tmpdir);
    test_mkdirp(tmpdir);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.cursor", tmpdir);
    test_mkdirp(dir);

    /* Write config with existing server */
    write_test_file(configpath,
                    "{\"mcpServers\": {\"other-server\": {\"command\": \"/usr/bin/other\"}}}");

    cbm_install_editor_mcp("/usr/local/bin/codebase-memory-mcp", configpath);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "other-server") != NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_editor_mcp_uninstall) {
    /* Port of TestEditorMCPUninstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.cursor/mcp.json", tmpdir);

    cbm_install_editor_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    int rc = cbm_remove_editor_mcp(configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* codebase-memory-mcp should be removed */
    ASSERT(strstr(data, "\"codebase-memory-mcp\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_junie_mcp_install_issue651) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.junie/mcp/mcp.json", tmpdir);

    int rc = cbm_upsert_junie_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "mcpServers") != NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/codebase-memory-mcp") != NULL);

    rc = cbm_upsert_junie_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    int count = 0;
    const char *p = data;
    while ((p = strstr(p, "\"codebase-memory-mcp\"")) != NULL) {
        count++;
        p += 20;
    }
    ASSERT_EQ(count, 1);

    rc = cbm_remove_junie_mcp(configpath);
    ASSERT_EQ(rc, 0);

    data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"codebase-memory-mcp\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_gemini_mcp_install) {
    /* Port of TestGeminiMCPInstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.gemini/settings.json", tmpdir);

    /* Gemini uses same mcpServers format as Cursor */
    int rc = cbm_install_editor_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "mcpServers") != NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_openclaw_mcp_install_uses_nested_servers) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-openclaw-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.openclaw/openclaw.json", tmpdir);

    int rc = cbm_install_openclaw_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"mcp\"") != NULL);
    ASSERT(strstr(data, "\"servers\"") != NULL);
    ASSERT(strstr(data, "\"enabled\": true") != NULL);
    ASSERT(strstr(data, "\"command\": \"/usr/local/bin/codebase-memory-mcp\"") != NULL);
    ASSERT(strstr(data, "\"args\": []") != NULL);
    ASSERT(strstr(data, "\"mcpServers\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_openclaw_mcp_preserves_existing_config) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-openclaw-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.openclaw", tmpdir);
    test_mkdirp(dir);

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/openclaw.json", dir);
    write_test_file(configpath,
                    "{\"theme\":\"dark\",\"mcp\":{\"servers\":{\"other\":{\"command\":\"x\"}}}}");

    int rc = cbm_install_openclaw_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "theme") != NULL);
    ASSERT(strstr(data, "other") != NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "\"mcpServers\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_openclaw_mcp_uninstall_uses_nested_servers) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-openclaw-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.openclaw/openclaw.json", tmpdir);

    ASSERT_EQ(cbm_install_openclaw_mcp("/usr/local/bin/codebase-memory-mcp", configpath), 0);
    ASSERT_EQ(cbm_remove_openclaw_mcp(configpath), 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"mcp\"") != NULL);
    ASSERT(strstr(data, "\"servers\"") != NULL);
    ASSERT(strstr(data, "\"codebase-memory-mcp\"") == NULL);
    ASSERT(strstr(data, "\"mcpServers\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  VS Code MCP config tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_vscode_mcp_install) {
    /* Port of TestVSCodeMCPInstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/Code/User/mcp.json", tmpdir);

    int rc = cbm_install_vscode_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"servers\"") != NULL);
    ASSERT(strstr(data, "\"type\"") != NULL);
    ASSERT(strstr(data, "\"stdio\"") != NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_vscode_mcp_uninstall) {
    /* Port of TestVSCodeMCPUninstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/Code/User/mcp.json", tmpdir);

    cbm_install_vscode_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    int rc = cbm_remove_vscode_mcp(configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"codebase-memory-mcp\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Zed MCP config tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_zed_mcp_install) {
    /* Port of TestZedMCPInstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.config/zed/settings.json", tmpdir);

    int rc = cbm_install_zed_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"context_servers\"") != NULL);
    ASSERT(strstr(data, "\"command\"") != NULL);
    ASSERT(strstr(data, "\"args\"") == NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_zed_mcp_preserves_settings) {
    /* Port of TestZedMCPPreservesSettings */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.config/zed/settings.json", tmpdir);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/zed", tmpdir);
    test_mkdirp(dir);

    /* Pre-existing Zed settings */
    write_test_file(configpath, "{\"theme\": \"One Dark\", \"vim_mode\": true}");

    cbm_install_zed_mcp("/usr/local/bin/codebase-memory-mcp", configpath);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Original settings preserved */
    ASSERT(strstr(data, "One Dark") != NULL);
    ASSERT(strstr(data, "vim_mode") != NULL);
    /* MCP server added */
    ASSERT(strstr(data, "context_servers") != NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_zed_mcp_uninstall) {
    /* Port of TestZedMCPUninstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.config/zed/settings.json", tmpdir);

    cbm_install_zed_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    int rc = cbm_remove_zed_mcp(configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"codebase-memory-mcp\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_zed_mcp_jsonc_comments) {
    /* Issue #24: Zed settings.json uses JSONC (comments + trailing commas) */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.config/zed/settings.json", tmpdir);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/zed", tmpdir);
    test_mkdirp(dir);

    /* JSONC with comments and trailing commas — must not fail */
    write_test_file(configpath, "// Zed settings\n"
                                "{\n"
                                "  \"theme\": \"One Dark\",\n"
                                "  /* multi-line\n"
                                "     comment */\n"
                                "  \"vim_mode\": true,\n" /* trailing comma */
                                "}\n");

    int rc = cbm_install_zed_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Original settings preserved */
    ASSERT(strstr(data, "One Dark") != NULL);
    ASSERT(strstr(data, "vim_mode") != NULL);
    /* MCP server added */
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "context_servers") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  PATH management tests (port of TestCLI_InstallPATHAppend)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_ensure_path_append) {
    /* Port of TestCLI_InstallPATHAppend */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-path-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char rcfile[512];
    snprintf(rcfile, sizeof(rcfile), "%s/.zshrc", tmpdir);
    write_test_file(rcfile, "# existing content\n");

    int rc = cbm_ensure_path("/usr/local/bin", rcfile, false);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(rcfile);
    ASSERT(strstr(data, "export PATH=\"/usr/local/bin:$PATH\"") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_ensure_path_already_present) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-path-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char rcfile[512];
    snprintf(rcfile, sizeof(rcfile), "%s/.zshrc", tmpdir);
    write_test_file(rcfile, "export PATH=\"/usr/local/bin:$PATH\"\n");

    int rc = cbm_ensure_path("/usr/local/bin", rcfile, false);
    ASSERT_EQ(rc, 1); /* 1 = already present */

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_ensure_path_dry_run) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-path-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char rcfile[512];
    snprintf(rcfile, sizeof(rcfile), "%s/.zshrc", tmpdir);
    write_test_file(rcfile, "# clean\n");

    int rc = cbm_ensure_path("/usr/local/bin", rcfile, true);
    ASSERT_EQ(rc, 0);

    /* File should NOT be modified */
    const char *data = read_test_file(rcfile);
    ASSERT(strstr(data, "export PATH") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #319: a fish config must get fish-native syntax, never `export PATH=`
 * (which is a syntax error in fish and breaks config.fish). */
TEST(cli_ensure_path_fish_syntax_issue319) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-path-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char rcfile[512];
    snprintf(rcfile, sizeof(rcfile), "%s/config.fish", tmpdir);
    write_test_file(rcfile, "# existing fish config\n");

    int rc = cbm_ensure_path("/usr/local/bin", rcfile, false);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(rcfile);
    ASSERT_NOT_NULL(data);
    /* fish-native form, and NO sh-style export. */
    ASSERT(strstr(data, "fish_add_path /usr/local/bin") != NULL);
    ASSERT(strstr(data, "export PATH") == NULL);

    /* Idempotent: a second call detects the existing fish line. */
    int rc2 = cbm_ensure_path("/usr/local/bin", rcfile, false);
    ASSERT_EQ(rc2, 1);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  File copy tests (port of update_test.go)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_copy_file) {
    /* Port of TestCopyFile */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-copy-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/source", tmpdir);
    snprintf(dst, sizeof(dst), "%s/dest", tmpdir);

    write_test_file(src, "test content for copy");

    int rc = cbm_copy_file(src, dst);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(dst);
    ASSERT_STR_EQ(data, "test content for copy");

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_copy_file_source_not_found) {
    /* Port of TestCopyFile_SourceNotFound */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-copy-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/nonexistent", tmpdir);
    snprintf(dst, sizeof(dst), "%s/dest", tmpdir);

    int rc = cbm_copy_file(src, dst);
    ASSERT(rc != 0);

    rmdir(tmpdir);
    PASS();
}

/* #472: install --force must copy the freshly-built binary to the target and
 * make it executable — previously it re-signed whatever was already there. */
TEST(cli_install_copies_binary_to_target_issue472) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-binswap-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/new-build", tmpdir);
    snprintf(dst, sizeof(dst), "%s/installed", tmpdir);

    write_test_file(src, "fresh build bytes");

    /* Target does not exist yet → must be created with the source content. */
    int rc = cbm_copy_binary_to_target(src, dst);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(dst);
    ASSERT_STR_EQ(data, "fresh build bytes");

#ifndef _WIN32
    /* The exec bit is set via chmod, which is POSIX-only; on Windows it is not
     * meaningful and MinGW stat() derives it from the file extension. */
    struct stat st;
    ASSERT_EQ(stat(dst, &st), 0);
    ASSERT((st.st_mode & S_IXUSR) != 0); /* executable bit set */
#endif

    /* Overwrite an existing (stale) target with new content. */
    write_test_file(dst, "STALE");
    write_test_file(src, "upgraded build bytes");
    rc = cbm_copy_binary_to_target(src, dst);
    ASSERT_EQ(rc, 0);
    data = read_test_file(dst);
    ASSERT_STR_EQ(data, "upgraded build bytes");

    test_rmdir_r(tmpdir);
    PASS();
}

/* #472: copying the running binary onto itself must NOT truncate it. */
TEST(cli_install_same_file_guard_issue472) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-samefile-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char path[512];
    snprintf(path, sizeof(path), "%s/self", tmpdir);
    write_test_file(path, "must survive self-copy");

    int rc = cbm_copy_binary_to_target(path, path);
    ASSERT_EQ(rc, 0); /* skipped, not failed */

    const char *data = read_test_file(path);
    ASSERT_STR_EQ(data, "must survive self-copy"); /* intact, not zeroed */

#ifndef _WIN32
    /* Distinct path strings resolving to the same inode (a symlink — exactly
     * what a non-canonical cbm_detect_self_path vs the hardcoded target can
     * produce) must also be detected as same-file and skipped, not truncated. */
    char link[512];
    snprintf(link, sizeof(link), "%s/self-link", tmpdir);
    if (symlink(path, link) == 0) {
        rc = cbm_copy_binary_to_target(link, path);
        ASSERT_EQ(rc, 0);
        data = read_test_file(path);
        ASSERT_STR_EQ(data, "must survive self-copy"); /* still intact via symlink */
    }
#endif

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Tar.gz extraction tests (port of update_test.go)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_extract_binary_from_targz) {
    /* Port of TestExtractBinaryFromTarGz */
    const char *content = "fake binary content";
    int gz_len;
    unsigned char *gz =
        create_test_targz("codebase-memory-mcp-linux-amd64", (const unsigned char *)content,
                          (int)strlen(content), &gz_len);
    ASSERT_NOT_NULL(gz);

    int out_len;
    unsigned char *extracted = cbm_extract_binary_from_targz(gz, gz_len, &out_len);
    ASSERT_NOT_NULL(extracted);
    ASSERT_EQ(out_len, (int)strlen(content));
    ASSERT_MEM_EQ(extracted, content, out_len);

    free(extracted);
    free(gz);
    PASS();
}

TEST(cli_extract_binary_from_targz_not_found) {
    /* Port of TestExtractBinaryFromTarGz_NotFound */
    const char *content = "hello";
    int gz_len;
    unsigned char *gz = create_test_targz("some-other-file", (const unsigned char *)content,
                                          (int)strlen(content), &gz_len);
    ASSERT_NOT_NULL(gz);

    int out_len;
    unsigned char *extracted = cbm_extract_binary_from_targz(gz, gz_len, &out_len);
    ASSERT_NULL(extracted);

    free(gz);
    PASS();
}

TEST(cli_extract_binary_from_targz_invalid_data) {
    /* Port of TestExtractBinaryFromTarGz_InvalidData */
    const unsigned char bad_data[] = "not a valid tar.gz";
    int out_len;
    unsigned char *extracted = cbm_extract_binary_from_targz(bad_data, sizeof(bad_data), &out_len);
    ASSERT_NULL(extracted);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Zip extraction tests
 * ═══════════════════════════════════════════════════════════════════ */

/* Build a minimal zip file with one stored (uncompressed) entry. */
static unsigned char *create_test_zip_stored(const char *filename, const unsigned char *content,
                                             int content_len, int *out_len) {
    /* Local file header (30 bytes) + filename + content + central dir + EOCD */
    int name_len = (int)strlen(filename);
    int local_hdr_sz = 30 + name_len;
    int cd_hdr_sz = 46 + name_len;
    int eocd_sz = 22;
    int total = local_hdr_sz + content_len + cd_hdr_sz + eocd_sz;
    unsigned char *zip = calloc(1, (size_t)total);
    if (!zip)
        return NULL;
    int pos = 0;

    /* Local file header */
    zip[pos] = 0x50;
    zip[pos + 1] = 0x4B;
    zip[pos + 2] = 0x03;
    zip[pos + 3] = 0x04; /* signature */
    zip[pos + 4] = 20;
    zip[pos + 5] = 0; /* version needed = 2.0 */
    zip[pos + 8] = 0;
    zip[pos + 9] = 0; /* compression = stored */
    zip[pos + 18] = (unsigned char)(content_len & 0xFF);
    zip[pos + 19] = (unsigned char)((content_len >> 8) & 0xFF);
    zip[pos + 20] = (unsigned char)((content_len >> 16) & 0xFF);
    zip[pos + 21] = (unsigned char)((content_len >> 24) & 0xFF);
    zip[pos + 22] = zip[pos + 18];
    zip[pos + 23] = zip[pos + 19];
    zip[pos + 24] = zip[pos + 20];
    zip[pos + 25] = zip[pos + 21];
    zip[pos + 26] = (unsigned char)(name_len & 0xFF);
    zip[pos + 27] = (unsigned char)((name_len >> 8) & 0xFF);
    memcpy(zip + pos + 30, filename, (size_t)name_len);
    pos += 30 + name_len;
    memcpy(zip + pos, content, (size_t)content_len);
    pos += content_len;

    int cd_start = pos;
    /* Central directory header */
    zip[pos] = 0x50;
    zip[pos + 1] = 0x4B;
    zip[pos + 2] = 0x01;
    zip[pos + 3] = 0x02;
    zip[pos + 10] = 0;
    zip[pos + 11] = 0; /* compression = stored */
    zip[pos + 20] = (unsigned char)(content_len & 0xFF);
    zip[pos + 21] = (unsigned char)((content_len >> 8) & 0xFF);
    zip[pos + 22] = (unsigned char)((content_len >> 16) & 0xFF);
    zip[pos + 23] = (unsigned char)((content_len >> 24) & 0xFF);
    zip[pos + 24] = zip[pos + 20];
    zip[pos + 25] = zip[pos + 21];
    zip[pos + 26] = zip[pos + 22];
    zip[pos + 27] = zip[pos + 23];
    zip[pos + 28] = (unsigned char)(name_len & 0xFF);
    zip[pos + 29] = (unsigned char)((name_len >> 8) & 0xFF);
    pos += 46 + name_len;

    /* EOCD */
    zip[pos] = 0x50;
    zip[pos + 1] = 0x4B;
    zip[pos + 2] = 0x05;
    zip[pos + 3] = 0x06;
    zip[pos + 8] = 1;  /* num entries this disk */
    zip[pos + 10] = 1; /* total entries */
    int cd_size = pos - cd_start;
    zip[pos + 12] = (unsigned char)(cd_size & 0xFF);
    zip[pos + 13] = (unsigned char)((cd_size >> 8) & 0xFF);
    zip[pos + 16] = (unsigned char)(cd_start & 0xFF);
    zip[pos + 17] = (unsigned char)((cd_start >> 8) & 0xFF);

    *out_len = total;
    return zip;
}

TEST(cli_extract_binary_from_zip) {
    const char *content = "#!/bin/sh\necho test\n";
    int zip_len = 0;
    unsigned char *zip = create_test_zip_stored(
        "codebase-memory-mcp", (const unsigned char *)content, (int)strlen(content), &zip_len);
    ASSERT_NOT_NULL(zip);

    int out_len = 0;
    unsigned char *extracted = cbm_extract_binary_from_zip(zip, zip_len, &out_len);
    ASSERT_NOT_NULL(extracted);
    ASSERT_EQ(out_len, (int)strlen(content));
    ASSERT_MEM_EQ(extracted, content, (size_t)out_len);
    free(extracted);
    free(zip);
    PASS();
}

TEST(cli_extract_binary_from_zip_not_found) {
    const char *content = "data";
    int zip_len = 0;
    unsigned char *zip = create_test_zip_stored("other-file.txt", (const unsigned char *)content,
                                                (int)strlen(content), &zip_len);
    ASSERT_NOT_NULL(zip);

    int out_len = 0;
    unsigned char *extracted = cbm_extract_binary_from_zip(zip, zip_len, &out_len);
    ASSERT_NULL(extracted);
    free(zip);
    PASS();
}

TEST(cli_extract_binary_from_zip_path_traversal) {
    const char *content = "malicious";
    int zip_len = 0;
    unsigned char *zip =
        create_test_zip_stored("../../etc/codebase-memory-mcp", (const unsigned char *)content,
                               (int)strlen(content), &zip_len);
    ASSERT_NOT_NULL(zip);

    int out_len = 0;
    unsigned char *extracted = cbm_extract_binary_from_zip(zip, zip_len, &out_len);
    ASSERT_NULL(extracted);
    free(zip);
    PASS();
}

TEST(cli_extract_binary_from_zip_invalid) {
    const unsigned char bad_data[] = "not a zip file";
    int out_len = 0;
    unsigned char *extracted = cbm_extract_binary_from_zip(bad_data, sizeof(bad_data), &out_len);
    ASSERT_NULL(extracted);
    PASS();
}

TEST(cli_extract_binary_from_zip_rejects_truncated_deflate_size_over_int_max) {
    const char *filename = "codebase-memory-mcp";
    const unsigned char deflated[] = {0xAB, 0x00, 0x00}; /* raw DEFLATE for "x" */
    size_t name_len = strlen(filename);
    size_t zip_len = 30 + name_len + sizeof(deflated);
    unsigned char *zip = calloc(1, zip_len);
    ASSERT_NOT_NULL(zip);

    uint32_t comp_size = 0xFFFF0000U;
    uint32_t uncomp_size = 1U;
    zip[0] = 0x50;
    zip[1] = 0x4B;
    zip[2] = 0x03;
    zip[3] = 0x04;
    zip[8] = 8;
    zip[9] = 0;
    zip[18] = (unsigned char)(comp_size & 0xFF);
    zip[19] = (unsigned char)((comp_size >> 8) & 0xFF);
    zip[20] = (unsigned char)((comp_size >> 16) & 0xFF);
    zip[21] = (unsigned char)((comp_size >> 24) & 0xFF);
    zip[22] = (unsigned char)(uncomp_size & 0xFF);
    zip[23] = (unsigned char)((uncomp_size >> 8) & 0xFF);
    zip[24] = (unsigned char)((uncomp_size >> 16) & 0xFF);
    zip[25] = (unsigned char)((uncomp_size >> 24) & 0xFF);
    zip[26] = (unsigned char)(name_len & 0xFF);
    zip[27] = (unsigned char)((name_len >> 8) & 0xFF);
    memcpy(zip + 30, filename, name_len);
    memcpy(zip + 30 + name_len, deflated, sizeof(deflated));

    int out_len = 0;
    unsigned char *extracted = cbm_extract_binary_from_zip(zip, (int)zip_len, &out_len);
    if (extracted) {
        free(extracted);
        free(zip);
        FAIL("accepted a truncated deflated zip entry with a wrapped compressed size");
    }
    ASSERT_EQ(out_len, 0);
    free(zip);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Skill dry-run tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_install_dry_run) {
    /* Port of TestCLI_InstallDryRun */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-dry-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    int count = cbm_install_skills(skills_dir, false, true);
    ASSERT_EQ(count, CBM_SKILL_COUNT);

    /* Skills should NOT be created */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, sk[i].name);
        struct stat st;
        ASSERT(stat(path, &st) != 0);
    }

    rmdir(tmpdir);
    PASS();
}

TEST(cli_uninstall_dry_run) {
    /* Port of TestCLI_UninstallDryRun */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-dry-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    cbm_install_skills(skills_dir, false, false);
    int removed = cbm_remove_skills(skills_dir, true);
    ASSERT_EQ(removed, CBM_SKILL_COUNT);

    /* Skills should still exist */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, sk[i].name);
        struct stat st;
        ASSERT_EQ(stat(path, &st), 0);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Full install + uninstall lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_install_and_uninstall) {
    /* Port of TestCLI_InstallAndUninstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-full-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    /* Install */
    int written = cbm_install_skills(skills_dir, false, false);
    ASSERT_EQ(written, CBM_SKILL_COUNT);

    /* Verify */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, sk[i].name);
        struct stat st;
        ASSERT_EQ(stat(path, &st), 0);
    }

    /* Uninstall */
    int removed = cbm_remove_skills(skills_dir, false);
    ASSERT_EQ(removed, CBM_SKILL_COUNT);

    /* Verify removed */
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", skills_dir, sk[i].name);
        struct stat st;
        ASSERT(stat(path, &st) != 0);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

static int copy_binary_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in)
        return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

TEST(cli_uninstall_help) {
    char *argv[] = {"uninstall", "--help"};
    int rc = cbm_cmd_uninstall(2, argv);
    ASSERT_EQ(rc, 0);

    char *argv_short[] = {"uninstall", "-h"};
    rc = cbm_cmd_uninstall(2, argv_short);
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(cli_uninstall_indexes_kept_by_default) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-uninst-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", tmpdir);
    test_mkdirp(cache_dir);

    char zova_dest[512];
    snprintf(zova_dest, sizeof(zova_dest), "%s/cbm.zova", cache_dir);

    const char *old_home = getenv("HOME");
    char *saved_home = old_home ? strdup(old_home) : NULL;
    const char *old_up = getenv("USERPROFILE");
    char *saved_up = old_up ? strdup(old_up) : NULL;
    const char *old_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache = old_cache ? strdup(old_cache) : NULL;

    cbm_setenv("HOME", tmpdir, 1);
    cbm_setenv("USERPROFILE", tmpdir, 1);
    cbm_setenv("CBM_CACHE_DIR", cache_dir, 1);

    cbm_store_t *setup_store = cbm_store_open_memory();
    ASSERT_NOT_NULL(setup_store);
    int s_rc = cbm_store_upsert_project(setup_store, "test-cli-uninstall", "/test/project");
    ASSERT_EQ(s_rc, CBM_STORE_OK);
    s_rc = th_publish_store_project(setup_store, "test-cli-uninstall");
    cbm_store_close(setup_store);
    ASSERT_EQ(s_rc, 0);

    /* 1. uninstall -y should KEEP indexes by default */
    char *argv_y[] = {"uninstall", "-y"};
    int rc = cbm_cmd_uninstall(2, argv_y);
    ASSERT_EQ(rc, 0);
    struct stat st;
    ASSERT_EQ(stat(zova_dest, &st), 0);

    /* 2. uninstall --dry-run --delete-indexes should STILL keep indexes */
    char *argv_dry[] = {"uninstall", "-y", "--dry-run", "--delete-indexes"};
    rc = cbm_cmd_uninstall(4, argv_dry);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(stat(zova_dest, &st), 0);

    /* 3. uninstall -y --delete-indexes should remove the index */
    char *argv_del[] = {"uninstall", "-y", "--delete-indexes"};
    rc = cbm_cmd_uninstall(3, argv_del);
    ASSERT_EQ(rc, 0);
    ASSERT(stat(zova_dest, &st) != 0);

    if (saved_home) {
        cbm_setenv("HOME", saved_home, 1);
        free(saved_home);
    }
    if (saved_up) {
        cbm_setenv("USERPROFILE", saved_up, 1);
        free(saved_up);
    }
    if (saved_cache) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
        free(saved_cache);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  YAML parser unit tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_yaml_parse_simple) {
    /* Basic key-value parsing */
    const char *yaml = "name: test\nversion: 1.0\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    ASSERT_STR_EQ(cbm_yaml_get_str(root, "name"), "test");
    ASSERT_STR_EQ(cbm_yaml_get_str(root, "version"), "1.0");
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_parse_nested) {
    /* Nested map */
    const char *yaml = "parent:\n"
                       "  child: value\n"
                       "  number: 42\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    ASSERT_STR_EQ(cbm_yaml_get_str(root, "parent.child"), "value");
    ASSERT_FLOAT_EQ(cbm_yaml_get_float(root, "parent.number", 0), 42.0, 0.001);
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_parse_list) {
    /* String list */
    const char *yaml = "items:\n"
                       "  - alpha\n"
                       "  - beta\n"
                       "  - gamma\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    const char *items[8];
    int count = cbm_yaml_get_str_list(root, "items", items, 8);
    ASSERT_EQ(count, 3);
    ASSERT_STR_EQ(items[0], "alpha");
    ASSERT_STR_EQ(items[1], "beta");
    ASSERT_STR_EQ(items[2], "gamma");
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_parse_bool) {
    const char *yaml = "enabled: true\n"
                       "disabled: false\n"
                       "on_flag: yes\n"
                       "off_flag: no\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    ASSERT_TRUE(cbm_yaml_get_bool(root, "enabled", false));
    ASSERT_FALSE(cbm_yaml_get_bool(root, "disabled", true));
    ASSERT_TRUE(cbm_yaml_get_bool(root, "on_flag", false));
    ASSERT_FALSE(cbm_yaml_get_bool(root, "off_flag", true));
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_parse_comments) {
    const char *yaml = "# This is a comment\n"
                       "key: value # inline comment\n"
                       "\n"
                       "# Another comment\n"
                       "other: data\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    ASSERT_STR_EQ(cbm_yaml_get_str(root, "key"), "value");
    ASSERT_STR_EQ(cbm_yaml_get_str(root, "other"), "data");
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_parse_empty) {
    cbm_yaml_node_t *root = cbm_yaml_parse("", 0);
    ASSERT_NOT_NULL(root);
    ASSERT_NULL(cbm_yaml_get_str(root, "anything"));
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_has) {
    const char *yaml = "a:\n  b: c\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    ASSERT_TRUE(cbm_yaml_has(root, "a"));
    ASSERT_TRUE(cbm_yaml_has(root, "a.b"));
    ASSERT_FALSE(cbm_yaml_has(root, "a.c"));
    ASSERT_FALSE(cbm_yaml_has(root, "x"));
    cbm_yaml_free(root);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group A: Agent Detection
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_detect_agents_finds_claude) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.claude", tmpdir);
    test_mkdirp(dir);

    /* Unset CLAUDE_CONFIG_DIR so detection is exercised against home_dir/.claude
     * and the runner's real env (which may set it) does not leak in. */
    const char *saved_ccd = getenv("CLAUDE_CONFIG_DIR");
    char *saved_ccd_copy = saved_ccd ? strdup(saved_ccd) : NULL;
    cbm_unsetenv("CLAUDE_CONFIG_DIR");

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.claude_code);

    if (saved_ccd_copy) {
        cbm_setenv("CLAUDE_CONFIG_DIR", saved_ccd_copy, 1);
        free(saved_ccd_copy);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_claude_via_env) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    /* Config dir lives OUTSIDE home_dir/.claude, pointed at by CLAUDE_CONFIG_DIR. */
    char ccd[512];
    snprintf(ccd, sizeof(ccd), "%s/custom-claude", tmpdir);
    test_mkdirp(ccd);

    const char *saved_ccd = getenv("CLAUDE_CONFIG_DIR");
    char *saved_ccd_copy = saved_ccd ? strdup(saved_ccd) : NULL;
    cbm_setenv("CLAUDE_CONFIG_DIR", ccd, 1);

    /* home_dir has no .claude, but detection must still find Claude via the env var. */
    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.claude_code);

    if (saved_ccd_copy) {
        cbm_setenv("CLAUDE_CONFIG_DIR", saved_ccd_copy, 1);
        free(saved_ccd_copy);
    } else {
        cbm_unsetenv("CLAUDE_CONFIG_DIR");
    }

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_codex) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.codex", tmpdir);
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.codex);

    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #222: Cursor (~/.cursor/) must be detected so install/update registers
 * the MCP server in ~/.cursor/mcp.json — previously it was never discovered. */
TEST(cli_detect_agents_finds_cursor_issue222) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.cursor", tmpdir);
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.cursor);

    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #388: `install --plan` must emit a machine-readable receipt of planned
 * writes WITHOUT mutating any config (the pre-mutation trust primitive). */
TEST(cli_install_plan_receipt_no_mutation_issue388) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-plan-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    /* Make Cursor + Codex "detected". */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.cursor", tmpdir);
    test_mkdirp(dir);
    snprintf(dir, sizeof(dir), "%s/.codex", tmpdir);
    test_mkdirp(dir);

    char *json = cbm_build_install_plan_json(tmpdir, "/usr/local/bin/codebase-memory-mcp");
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "agent.install.plan.v1") != NULL);
    ASSERT(strstr(json, "writes_started") != NULL);
    ASSERT(strstr(json, "next_safe_command") != NULL);
    ASSERT(strstr(json, "binary_target_planned") != NULL);
    ASSERT(strstr(json, "shell_rc_planned") != NULL);
    ASSERT(strstr(json, "human_summary") != NULL);
    ASSERT(strstr(json, "cursor") != NULL);
    ASSERT(strstr(json, ".cursor/mcp.json") != NULL);
    ASSERT(strstr(json, ".codex/config.toml") != NULL);
    free(json);

    /* Critical: building the plan must NOT have created any config file. */
    char cfg[512];
    struct stat st;
    snprintf(cfg, sizeof(cfg), "%s/.cursor/mcp.json", tmpdir);
    ASSERT(stat(cfg, &st) != 0); /* must not exist */
    snprintf(cfg, sizeof(cfg), "%s/.codex/config.toml", tmpdir);
    ASSERT(stat(cfg, &st) != 0); /* must not exist */

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_install_help_no_mutation) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-install-help-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *old_home = getenv("HOME");
    char *old_home_copy = old_home ? strdup(old_home) : NULL;
    cbm_setenv("HOME", tmpdir, 1);

    char *args[] = {"--help"};
    ASSERT_EQ(cbm_cmd_install(1, args), 0);

    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%s/.codex/config.toml", tmpdir);
    ASSERT(stat(path, &st) != 0);
    snprintf(path, sizeof(path), "%s/.local/bin/codebase-memory-mcp", tmpdir);
    ASSERT(stat(path, &st) != 0);

    if (old_home_copy) {
        cbm_setenv("HOME", old_home_copy, 1);
        free(old_home_copy);
    } else {
        cbm_unsetenv("HOME");
    }
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_install_refuses_conflicting_mcp_config_without_replace_config) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-install-conflict-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *old_home = getenv("HOME");
    char *old_home_copy = old_home ? strdup(old_home) : NULL;
    cbm_setenv("HOME", tmpdir, 1);

    char cursor_dir[512];
    char config_path[512];
    snprintf(cursor_dir, sizeof(cursor_dir), "%s/.cursor", tmpdir);
    snprintf(config_path, sizeof(config_path), "%s/mcp.json", cursor_dir);
    ASSERT_EQ(test_mkdirp(cursor_dir), 0);
    ASSERT_EQ(write_test_file(config_path,
                              "{\"mcpServers\":{\"codebase-memory-mcp\":{"
                              "\"command\":\"/opt/upstream/codebase-memory-mcp\"}}}"),
              0);

    char *refuse_args[] = {"--dry-run", "-y"};
    ASSERT_EQ(cbm_cmd_install(2, refuse_args), 1);
    ASSERT(strstr(read_test_file(config_path), "/opt/upstream/codebase-memory-mcp") != NULL);

    char *replace_args[] = {"--dry-run", "--replace-config", "-y"};
    ASSERT_EQ(cbm_cmd_install(3, replace_args), 0);
    ASSERT(strstr(read_test_file(config_path), "/opt/upstream/codebase-memory-mcp") != NULL);

    if (old_home_copy) {
        cbm_setenv("HOME", old_home_copy, 1);
        free(old_home_copy);
    } else {
        cbm_unsetenv("HOME");
    }
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_doctor_where_reports_paths) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-doctor-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");
    char cache[512];
    snprintf(cache, sizeof(cache), "%s/cache", tmpdir);
    test_mkdirp(cache);
    char db[512];
    snprintf(db, sizeof(db), "%s/cbm.zova", cache);
    write_test_file(db, "");

    const char *old_home = getenv("HOME");
    const char *old_cache = getenv("CBM_CACHE_DIR");
    char *old_home_copy = old_home ? strdup(old_home) : NULL;
    char *old_cache_copy = old_cache ? strdup(old_cache) : NULL;
    cbm_setenv("HOME", tmpdir, 1);
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    ASSERT_EQ(cbm_cmd_doctor(0, NULL), 0);
    ASSERT_EQ(cbm_cmd_where(0, NULL), 0);

    if (old_home_copy) {
        cbm_setenv("HOME", old_home_copy, 1);
        free(old_home_copy);
    } else {
        cbm_unsetenv("HOME");
    }
    if (old_cache_copy) {
        cbm_setenv("CBM_CACHE_DIR", old_cache_copy, 1);
        free(old_cache_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #330: Codex SessionStart reminder hook in config.toml — installed,
 * idempotent, preserves other content, and cleanly removed. */
TEST(cli_codex_session_hook_issue330) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codexhook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/config.toml", tmpdir);
    write_test_file(cfg, "[mcp_servers.other]\ncommand = \"x\"\n");

    ASSERT_EQ(cbm_upsert_codex_hooks(cfg), 0);
    const char *d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    ASSERT(strstr(d, "[[hooks.SessionStart]]") != NULL);
    ASSERT(strstr(d, "[[hooks.SessionStart.hooks]]") != NULL);
    ASSERT(strstr(d, "search_graph") != NULL);
    ASSERT(strstr(d, "[mcp_servers.other]") != NULL); /* pre-existing content preserved */
    /* Idempotent: a second upsert leaves exactly ONE hook block. */
    ASSERT_EQ(cbm_upsert_codex_hooks(cfg), 0);
    d = read_test_file(cfg);
    const char *first = strstr(d, "[[hooks.SessionStart]]");
    ASSERT_NOT_NULL(first);
    ASSERT_NULL(strstr(first + 1, "[[hooks.SessionStart]]"));

    ASSERT_EQ(cbm_remove_codex_hooks(cfg), 0);
    d = read_test_file(cfg);
    ASSERT_NULL(strstr(d, "hooks.SessionStart"));
    ASSERT(strstr(d, "[mcp_servers.other]") != NULL); /* still preserved after removal */

    write_test_file(cfg, "[mcp_servers.other]\ncommand = \"x\"\n"
                         "# >>> codebase-memory-mcp SessionStart >>>\n"
                         "[[hooks.SessionStart]]\n"
                         "# <<< codebase-memory-mcp SessionStart <<<\n"
                         "# <<< codebase-memory-mcp SessionStart <<<\n"
                         "# >>> codebase-memory-mcp SessionStart >>>\n");
    ASSERT_EQ(cbm_upsert_codex_hooks(cfg), 0);
    d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    first = strstr(d, "[[hooks.SessionStart]]");
    ASSERT_NOT_NULL(first);
    ASSERT_NULL(strstr(first + 1, "[[hooks.SessionStart]]"));

    test_rmdir_r(tmpdir);
    PASS();
}

/* Gemini/Antigravity SessionStart reminder parity (settings.json JSON path). */
TEST(cli_gemini_session_hook_parity) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-gemhook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/settings.json", tmpdir);

    ASSERT_EQ(cbm_upsert_gemini_session_hooks(cfg), 0);
    const char *d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    ASSERT(strstr(d, "SessionStart") != NULL);
    ASSERT(strstr(d, "search_graph") != NULL);

    ASSERT_EQ(cbm_remove_gemini_session_hooks(cfg), 0);
    d = read_test_file(cfg);
    ASSERT_NULL(strstr(d, "SessionStart"));

    test_rmdir_r(tmpdir);
    PASS();
}

/* Claude SubagentStart reminder: subagents spawned via the Agent tool do not
 * fire SessionStart, so this hook is their code-discovery channel. Verify the
 * install shape, idempotent re-install, and clean removal. */
TEST(cli_claude_subagent_hook) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-subhook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/settings.json", tmpdir);

    ASSERT_EQ(cbm_upsert_claude_subagent_hooks(cfg), 0);
    const char *d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    ASSERT(strstr(d, "SubagentStart") != NULL);
    ASSERT(strstr(d, "\"*\"") != NULL);                 /* match-all matcher */
    ASSERT(strstr(d, "cbm-subagent-reminder") != NULL); /* points at the hook script */

    /* Idempotent: a second upsert must not duplicate our entry. */
    ASSERT_EQ(cbm_upsert_claude_subagent_hooks(cfg), 0);
    d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    int count = 0;
    for (const char *p = d; (p = strstr(p, "cbm-subagent-reminder")) != NULL; p++)
        count++;
    ASSERT_EQ(count, 1);

    ASSERT_EQ(cbm_remove_claude_subagent_hooks(cfg), 0);
    d = read_test_file(cfg);
    ASSERT_NULL(strstr(d, "SubagentStart"));

    test_rmdir_r(tmpdir);
    PASS();
}

/* A user's own catch-all ("*") SubagentStart hook must survive CMM install and
 * uninstall: ownership is keyed on the command, not just the "*" matcher. */
TEST(cli_claude_subagent_hook_preserves_user_entry) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-subuser-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/settings.json", tmpdir);
    /* Pre-existing user SubagentStart hook, also matcher "*", different command. */
    write_test_file(
        cfg, "{\"hooks\":{\"SubagentStart\":[{\"matcher\":\"*\","
             "\"hooks\":[{\"type\":\"command\",\"command\":\"echo user-subagent-hook\"}]}]}}");

    /* Install CMM's hook: the user's "*" entry must remain, ours added alongside. */
    ASSERT_EQ(cbm_upsert_claude_subagent_hooks(cfg), 0);
    const char *d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    ASSERT(strstr(d, "echo user-subagent-hook") != NULL); /* user's hook untouched */
    ASSERT(strstr(d, "cbm-subagent-reminder") != NULL);   /* ours added */

    /* Remove CMM's hook: the user's entry must still be intact, ours gone. */
    ASSERT_EQ(cbm_remove_claude_subagent_hooks(cfg), 0);
    d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    ASSERT(strstr(d, "echo user-subagent-hook") != NULL); /* user's hook preserved */
    ASSERT_NULL(strstr(d, "cbm-subagent-reminder"));      /* only ours removed */

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_gemini) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.gemini", tmpdir);
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.gemini);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_zed) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
#ifdef __APPLE__
    snprintf(dir, sizeof(dir), "%s/.config/zed", tmpdir);
#elif defined(_WIN32)
    snprintf(dir, sizeof(dir), "%s/AppData/Local/Zed", tmpdir);
#else
    snprintf(dir, sizeof(dir), "%s/.config/zed", tmpdir);
#endif
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.zed);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_antigravity) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    /* Antigravity CLI installs under ~/.gemini/antigravity-cli/ (2026). */
    snprintf(dir, sizeof(dir), "%s/.gemini/antigravity-cli", tmpdir);
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.antigravity);
    ASSERT_TRUE(agents.gemini); /* parent ~/.gemini implies gemini too */

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_kilocode) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
#ifdef __APPLE__
    snprintf(dir, sizeof(dir),
             "%s/Library/Application Support/Code/User/globalStorage/kilocode.kilo-code", tmpdir);
#elif defined(_WIN32)
    snprintf(dir, sizeof(dir), "%s/AppData/Roaming/Code/User/globalStorage/kilocode.kilo-code",
             tmpdir);
#else
    snprintf(dir, sizeof(dir), "%s/.config/Code/User/globalStorage/kilocode.kilo-code", tmpdir);
#endif
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.kilocode);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_kiro) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.kiro", tmpdir);
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.kiro);

    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #651: Junie (~/.junie/) must be detected so install registers the
 * MCP server in ~/.junie/mcp/mcp.json. */
TEST(cli_detect_agents_finds_junie_issue651) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.junie", tmpdir);
    test_mkdirp(dir);
    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.junie);
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_none_found) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    /* Empty home dir → no config dirs → no directory-based agents detected.
     * Note: opencode/aider may still be detected via system fallback paths
     * (e.g. /usr/local/bin) so we only assert on directory-based agents.
     * Unset CLAUDE_CONFIG_DIR so the runner's real env does not leak in. */
    const char *saved_ccd = getenv("CLAUDE_CONFIG_DIR");
    char *saved_ccd_copy = saved_ccd ? strdup(saved_ccd) : NULL;
    cbm_unsetenv("CLAUDE_CONFIG_DIR");

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_FALSE(agents.claude_code);
    ASSERT_FALSE(agents.codex);
    ASSERT_FALSE(agents.gemini);
    ASSERT_FALSE(agents.zed);
    ASSERT_FALSE(agents.antigravity);
    ASSERT_FALSE(agents.kilocode);
    ASSERT_FALSE(agents.kiro);

    if (saved_ccd_copy) {
        cbm_setenv("CLAUDE_CONFIG_DIR", saved_ccd_copy, 1);
        free(saved_ccd_copy);
    }

    rmdir(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group B: MCP Config Upsert — Codex TOML
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_codex_mcp_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codex-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);

    int rc = cbm_upsert_codex_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "[mcp_servers.codebase-memory-mcp]") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "env_vars = [\"CBM_CACHE_DIR\"]") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_codex_mcp_existing) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codex-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);
    write_test_file(configpath, "model = \"gpt-4\"\n\n[other_setting]\nfoo = \"bar\"\n");

    int rc = cbm_upsert_codex_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Existing settings preserved */
    ASSERT(strstr(data, "model = \"gpt-4\"") != NULL);
    ASSERT(strstr(data, "[other_setting]") != NULL);
    /* Our entry added */
    ASSERT(strstr(data, "[mcp_servers.codebase-memory-mcp]") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_codex_mcp_replace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codex-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);
    write_test_file(configpath, "[mcp_servers.codebase-memory-mcp]\n"
                                "command = \"/old/path/codebase-memory-mcp\"\n"
                                "\n"
                                "[other_setting]\nfoo = \"bar\"\n");

    int rc = cbm_upsert_codex_mcp("/new/path/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Old path replaced */
    ASSERT(strstr(data, "/old/path") == NULL);
    ASSERT(strstr(data, "/new/path/codebase-memory-mcp") != NULL);
    /* Other settings preserved */
    ASSERT(strstr(data, "[other_setting]") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_codex_mcp_quoted_and_whitespace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codex-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);

    /* 1. Quoted key: [mcp_servers."codebase-memory-mcp"] */
    write_test_file(configpath, "model = \"gpt-4\"\n\n"
                                "[mcp_servers.\"codebase-memory-mcp\"]\n"
                                "command = \"/old/path/codebase-memory-mcp\"\n"
                                "env_vars = [\"CBM_CACHE_DIR\"]\n\n"
                                "[other_setting]\nfoo = \"bar\"\n");

    int rc = cbm_upsert_codex_mcp("/new/path/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "/old/path") == NULL);
    ASSERT(strstr(data, "/new/path/codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "[other_setting]") != NULL);
    ASSERT(strstr(data, "model = \"gpt-4\"") != NULL);

    /* Ensure there is only 1 table and not duplicate */
    int count = 0;
    const char *p = data;
    while ((p = strstr(p, "codebase-memory-mcp")) != NULL) {
        count++;
        p += strlen("codebase-memory-mcp");
    }
    ASSERT_EQ(count, 2);

    /* 2. Remove the table */
    rc = cbm_remove_codex_mcp(configpath);
    ASSERT_EQ(rc, 0);
    data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "codebase-memory-mcp") == NULL);
    ASSERT(strstr(data, "[other_setting]") != NULL);
    ASSERT(strstr(data, "model = \"gpt-4\"") != NULL);

    /* 3. Whitespace and single-quote variation */
    write_test_file(configpath, "[ mcp_servers . 'codebase-memory-mcp' ]\n"
                                "command = \"/old2/path\"\n\n"
                                "[other_setting]\nfoo = \"bar\"\n");
    rc = cbm_upsert_codex_mcp("/new2/path/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);
    data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "/old2/path") == NULL);
    ASSERT(strstr(data, "/new2/path/codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "[other_setting]") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_codex_mcp_header_directly_followed_by_header) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codex-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);

    /* Header with no body, followed directly by another header */
    write_test_file(configpath, "[mcp_servers.codebase-memory-mcp]\n"
                                "[other]\n"
                                "foo = 1\n"
                                "[third]\n"
                                "bar = 2\n");

    int rc = cbm_upsert_codex_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "[other]") != NULL);
    ASSERT(strstr(data, "foo = 1") != NULL);
    ASSERT(strstr(data, "[third]") != NULL);
    ASSERT(strstr(data, "bar = 2") != NULL);
    ASSERT(strstr(data, "[mcp_servers.codebase-memory-mcp]") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/codebase-memory-mcp") != NULL);

    /* Test remove on header directly followed by another header */
    write_test_file(configpath, "[mcp_servers.codebase-memory-mcp]\n"
                                "[other]\n"
                                "foo = 1\n");
    rc = cbm_remove_codex_mcp(configpath);
    ASSERT_EQ(rc, 0);

    data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "codebase-memory-mcp") == NULL);
    ASSERT(strstr(data, "[other]") != NULL);
    ASSERT(strstr(data, "foo = 1") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_codex_mcp_two_tables_cleaned_up) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codex-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);

    /* Two existing tables: one quoted, one unquoted */
    write_test_file(configpath, "[mcp_servers.\"codebase-memory-mcp\"]\n"
                                "command = \"/old1/path\"\n\n"
                                "[other]\n"
                                "foo = 1\n\n"
                                "[mcp_servers.codebase-memory-mcp]\n"
                                "command = \"/old2/path\"\n");

    int rc = cbm_upsert_codex_mcp("/new/path/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Exactly one table remains */
    int count = 0;
    const char *p = data;
    while ((p = strstr(p, "mcp_servers.")) != NULL) {
        count++;
        p += strlen("mcp_servers.");
    }
    ASSERT_EQ(count, 1);
    ASSERT(strstr(data, "/old1/path") == NULL);
    ASSERT(strstr(data, "/old2/path") == NULL);
    ASSERT(strstr(data, "/new/path/codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "[other]") != NULL);
    ASSERT(strstr(data, "foo = 1") != NULL);

    /* Test remove drops all matching tables */
    write_test_file(configpath, "[mcp_servers.\"codebase-memory-mcp\"]\n"
                                "command = \"/old1/path\"\n\n"
                                "[other]\n"
                                "foo = 1\n\n"
                                "[mcp_servers.codebase-memory-mcp]\n"
                                "command = \"/old2/path\"\n");
    rc = cbm_remove_codex_mcp(configpath);
    ASSERT_EQ(rc, 0);

    data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "codebase-memory-mcp") == NULL);
    ASSERT(strstr(data, "[other]") != NULL);
    ASSERT(strstr(data, "foo = 1") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group B: MCP Config Upsert — Zed (corrected format)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_zed_mcp_uses_args_format) {
    /* Verify Zed omits empty args and does not use source:"custom" */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-zed-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/settings.json", tmpdir);

    cbm_install_zed_mcp("/usr/local/bin/codebase-memory-mcp", configpath);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"args\"") == NULL);
    /* Must NOT have source:"custom" */
    ASSERT(strstr(data, "\"source\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group B: MCP Config Upsert — OpenCode
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_opencode_mcp_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ocode-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/opencode.json", tmpdir);

    int rc = cbm_upsert_opencode_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/codebase-memory-mcp") != NULL);
    ASSERT(strstr(data, "\"enabled\":true") != NULL || strstr(data, "\"enabled\": true") != NULL);
    /* command must be emitted as an array, not a string */
    ASSERT(strstr(data, "\"command\":[") != NULL || strstr(data, "\"command\": [") != NULL);
    /* type must be explicitly set to \"local\" */
    ASSERT(strstr(data, "\"type\":\"local\"") != NULL ||
           strstr(data, "\"type\": \"local\"") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_opencode_mcp_existing) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ocode-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/opencode.json", tmpdir);
    write_test_file(configpath, "{\"mcp\":{\"other-server\":{\"command\":\"/usr/bin/other\"}}}");

    int rc = cbm_upsert_opencode_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "other-server") != NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group B: MCP Config Upsert — Antigravity
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_antigravity_mcp_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-anti-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/mcp_config.json", tmpdir);

    int rc = cbm_upsert_antigravity_mcp("/usr/local/bin/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_antigravity_mcp_replace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-anti-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/mcp_config.json", tmpdir);
    write_test_file(configpath,
                    "{\"mcpServers\":{\"codebase-memory-mcp\":{\"command\":\"/old/path\"}}}");

    int rc = cbm_upsert_antigravity_mcp("/new/path/codebase-memory-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "/old/path") == NULL);
    ASSERT(strstr(data, "/new/path/codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group C: Instructions File Upsert
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_instructions_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-instr-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/AGENTS.md", tmpdir);

    int rc = cbm_upsert_instructions(filepath, "# Test content\nHello world\n");
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(filepath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "<!-- codebase-memory-mcp:start -->") != NULL);
    ASSERT(strstr(data, "<!-- codebase-memory-mcp:end -->") != NULL);
    ASSERT(strstr(data, "Hello world") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_instructions_existing) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-instr-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/AGENTS.md", tmpdir);
    write_test_file(filepath, "# My Project Rules\n\nDo the thing.\n");

    int rc = cbm_upsert_instructions(filepath, "# CMM\nUse search_graph\n");
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(filepath);
    ASSERT_NOT_NULL(data);
    /* Original content preserved */
    ASSERT(strstr(data, "My Project Rules") != NULL);
    ASSERT(strstr(data, "Do the thing") != NULL);
    /* CMM section appended */
    ASSERT(strstr(data, "codebase-memory-mcp:start") != NULL);
    ASSERT(strstr(data, "search_graph") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_instructions_replace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-instr-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/AGENTS.md", tmpdir);
    write_test_file(filepath, "# Rules\n"
                              "<!-- codebase-memory-mcp:start -->\n"
                              "OLD CONTENT\n"
                              "<!-- codebase-memory-mcp:end -->\n"
                              "# Other stuff\n");

    int rc = cbm_upsert_instructions(filepath, "NEW CONTENT\n");
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(filepath);
    ASSERT_NOT_NULL(data);
    /* Old content replaced */
    ASSERT(strstr(data, "OLD CONTENT") == NULL);
    ASSERT(strstr(data, "NEW CONTENT") != NULL);
    /* Surrounding content preserved */
    ASSERT(strstr(data, "# Rules") != NULL);
    ASSERT(strstr(data, "# Other stuff") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_instructions_no_duplicate) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-instr-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/AGENTS.md", tmpdir);

    /* Install twice */
    cbm_upsert_instructions(filepath, "Content v1\n");
    cbm_upsert_instructions(filepath, "Content v2\n");

    const char *data = read_test_file(filepath);
    ASSERT_NOT_NULL(data);
    /* Only one start marker */
    int count = 0;
    const char *p = data;
    while ((p = strstr(p, "codebase-memory-mcp:start")) != NULL) {
        count++;
        p += 25;
    }
    ASSERT_EQ(count, 1);
    /* Latest content */
    ASSERT(strstr(data, "Content v2") != NULL);
    ASSERT(strstr(data, "Content v1") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_remove_instructions) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-instr-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/AGENTS.md", tmpdir);
    write_test_file(filepath, "# Rules\n"
                              "<!-- codebase-memory-mcp:start -->\n"
                              "CMM Content\n"
                              "<!-- codebase-memory-mcp:end -->\n"
                              "# Other\n");

    int rc = cbm_remove_instructions(filepath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(filepath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "CMM Content") == NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") == NULL);
    ASSERT(strstr(data, "# Rules") != NULL);
    ASSERT(strstr(data, "# Other") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_agent_instructions_content) {
    const char *instr = cbm_get_agent_instructions();
    ASSERT_NOT_NULL(instr);
    ASSERT(strstr(instr, "search_graph") != NULL);
    ASSERT(strstr(instr, "trace_path") != NULL);
    ASSERT(strstr(instr, "get_code_snippet") != NULL);
    ASSERT(strstr(instr, "list_projects") != NULL);
    ASSERT(strstr(instr, "projects=[\"*\"]") != NULL);
    ASSERT(strstr(instr, "AnotherFolder/codebase-memory-mcp") != NULL);
    ASSERT(strstr(instr, "returned `project`") != NULL);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group D: Pre-Tool Hook Upsert — Claude Code
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_claude_hook_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-hook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);

    int rc = cbm_upsert_claude_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "PreToolUse") != NULL);
    /* Matcher excludes Read per issue #362 (gating Read breaks the
     * read-before-edit invariant). Assert exact matcher value AND that no
     * Read-chained matcher slipped back in. */
    ASSERT(strstr(data, "\"Grep|Glob\"") != NULL);
    ASSERT(strstr(data, "Glob|Read") == NULL);
    ASSERT(strstr(data, "cbm-code-discovery-gate") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* Count our exec-form hooks ({command: binary, args: ["hook-augment"]}) and
 * foreign hooks under one settings.json hook event. */
static void exec_hook_counts(yyjson_val *hooks, const char *event, const char *binary,
                             int *exec_out, int *other_out) {
    *exec_out = 0;
    *other_out = 0;
    yyjson_val *arr = yyjson_obj_get(hooks, event);
    size_t i, n, j, m;
    yyjson_val *entry, *h;
    yyjson_arr_foreach(arr, i, n, entry) {
        yyjson_arr_foreach(yyjson_obj_get(entry, "hooks"), j, m, h) {
            yyjson_val *args = yyjson_obj_get(h, "args");
            const char *cmd = yyjson_get_str(yyjson_obj_get(h, "command"));
            if (cmd && strcmp(cmd, binary) == 0 && yyjson_arr_size(args) == 1 &&
                strcmp(yyjson_get_str(yyjson_arr_get(args, 0)), "hook-augment") == 0) {
                (*exec_out)++;
            } else {
                (*other_out)++;
            }
        }
    }
}

/* Install registers every Claude hook as the shell-free exec form
 * `<binary> hook-augment` (upstream 7d20ff93), converges earlier shell-form
 * entries onto it, stays idempotent, and leaves a user's own hooks alone. */
TEST(cli_claude_exec_hooks_replace_shell_form) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "%s/cli-exechook-XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");
    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/settings.json", tmpdir);
    const char *bin = "/opt/cbm/bin/codebase-memory-mcp";

    write_test_file(
        cfg, "{\"hooks\":{\"SubagentStart\":[{\"matcher\":\"*\","
             "\"hooks\":[{\"type\":\"command\",\"command\":\"echo user-subagent-hook\"}]}]}}");
    /* Earlier releases registered the shell-form scripts. */
    ASSERT_EQ(cbm_upsert_claude_hooks(cfg), 0);
    ASSERT_EQ(cbm_upsert_claude_subagent_hooks(cfg), 0);
    ASSERT_NOT_NULL(strstr(read_test_file(cfg), "cbm-code-discovery-gate"));

    for (int pass = 0; pass < 2; pass++) {
        ASSERT_EQ(cbm_upsert_claude_exec_hooks(cfg, bin), 0);
        const char *d = read_test_file(cfg);
        ASSERT_NOT_NULL(d);
        ASSERT_NULL(strstr(d, "cbm-code-discovery-gate"));
        ASSERT_NULL(strstr(d, "cbm-subagent-reminder"));
        yyjson_doc *doc = yyjson_read(d, strlen(d), 0);
        ASSERT_NOT_NULL(doc);
        yyjson_val *hooks = yyjson_obj_get(yyjson_doc_get_root(doc), "hooks");
        int exec_n, other_n;
        exec_hook_counts(hooks, "PreToolUse", bin, &exec_n, &other_n);
        ASSERT_EQ(exec_n, 1);
        ASSERT_EQ(other_n, 0);
        exec_hook_counts(hooks, "SessionStart", bin, &exec_n, &other_n);
        ASSERT_EQ(exec_n, 4); /* startup, resume, clear, compact */
        ASSERT_EQ(other_n, 0);
        exec_hook_counts(hooks, "SubagentStart", bin, &exec_n, &other_n);
        ASSERT_EQ(exec_n, 1);
        ASSERT_EQ(other_n, 1); /* the user's "*" hook survives */
        yyjson_doc_free(doc);
    }

    /* Uninstall recognizes the exec form and still spares the user's hook. */
    ASSERT_EQ(cbm_remove_claude_hooks(cfg), 0);
    ASSERT_EQ(cbm_remove_claude_subagent_hooks(cfg), 0);
    const char *d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    ASSERT_NULL(strstr(d, "PreToolUse"));
    ASSERT_NOT_NULL(strstr(d, "echo user-subagent-hook"));
    yyjson_doc *doc = yyjson_read(d, strlen(d), 0);
    ASSERT_NOT_NULL(doc);
    int exec_n, other_n;
    exec_hook_counts(yyjson_obj_get(yyjson_doc_get_root(doc), "hooks"), "SubagentStart", bin,
                     &exec_n, &other_n);
    ASSERT_EQ(exec_n, 0);
    ASSERT_EQ(other_n, 1);
    yyjson_doc_free(doc);

    test_rmdir_r(tmpdir);
    PASS();
}

#ifndef _WIN32
/* Run `hook-augment` on a payload in a child (it arms a process-wide SIGALRM
 * deadline) and return its stdout in a static buffer. */
static const char *run_hook_augment(const char *dir, const char *payload) {
    char in_path[512], out_path[512];
    snprintf(in_path, sizeof(in_path), "%s/hook-in.json", dir);
    snprintf(out_path, sizeof(out_path), "%s/hook-out.txt", dir);
    if (write_test_file(in_path, payload) != 0) {
        return NULL;
    }
    (void)fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int in_fd = open(in_path, O_RDONLY);
        int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (in_fd < 0 || out_fd < 0 || dup2(in_fd, STDIN_FILENO) < 0 ||
            dup2(out_fd, STDOUT_FILENO) < 0) {
            _exit(2);
        }
        int rc = cbm_cmd_hook_augment();
        (void)fflush(stdout);
        _exit(rc == 0 ? 0 : 1);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return NULL;
    }
    return read_test_file(out_path);
}

/* The exec-form SessionStart/SubagentStart hooks run `hook-augment`, so it
 * must emit each event's reminder in the shape Claude injects. */
TEST(cli_hook_augment_lifecycle_reminders) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "%s/cli-hookaug-XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *out = run_hook_augment(tmpdir, "{\"hook_event_name\":\"SessionStart\","
                                               "\"source\":\"startup\"}");
    ASSERT_NOT_NULL(out);
    ASSERT_NOT_NULL(strstr(out, "CRITICAL - Code Discovery Protocol:"));
    ASSERT_NOT_NULL(strstr(out, "run index_repository FIRST"));

    out = run_hook_augment(tmpdir, "{\"hook_event_name\":\"SubagentStart\","
                                   "\"agent_type\":\"Explore\"}");
    ASSERT_NOT_NULL(out);
    yyjson_doc *doc = yyjson_read(out, strlen(out), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *hso = yyjson_obj_get(yyjson_doc_get_root(doc), "hookSpecificOutput");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(hso, "hookEventName")), "SubagentStart");
    ASSERT_NOT_NULL(strstr(yyjson_get_str(yyjson_obj_get(hso, "additionalContext")),
                           "codebase-memory-mcp tools"));
    yyjson_doc_free(doc);

    /* Other tools still pass through silently. */
    out = run_hook_augment(tmpdir, "{\"hook_event_name\":\"PreToolUse\","
                                   "\"tool_name\":\"Read\",\"tool_input\":{}}");
    ASSERT_NOT_NULL(out);
    ASSERT_STR_EQ(out, "");

    test_rmdir_r(tmpdir);
    PASS();
}
#endif

/* issue #384: the PreToolUse gate shim must never use a predictable /tmp
 * filename (the old `/tmp/cbm-code-discovery-gate-$PPID` was a symlink-attack
 * vector). The shim is now a stateless wrapper around the compiled augmenter. */
TEST(cli_hook_gate_script_no_predictable_tmp_issue384) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-gate-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_install_hook_gate_script(tmpdir, "/usr/local/bin/codebase-memory-mcp");

    char script_path[512];
    snprintf(script_path, sizeof(script_path), "%s/.claude/hooks/cbm-code-discovery-gate", tmpdir);
    const char *data = read_test_file(script_path);
    ASSERT_NOT_NULL(data);
    /* No predictable temp/state file and no PPID-derived path. */
    ASSERT(strstr(data, "/tmp") == NULL);
    ASSERT(strstr(data, "PPID") == NULL);
    /* It delegates to the stateless compiled augmenter (stdout only). */
    ASSERT(strstr(data, "hook-augment") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #618: hook-augment was a structural no-op on Windows because its path
 * guards required POSIX-style '/'-prefixed absolute paths, so a drive-letter
 * cwd (C:/repo) was rejected before any search_graph query. The predicate must
 * accept POSIX and Windows drive roots alike (callers normalize '\\' to '/'). */
TEST(cli_hook_augment_path_is_abs) {
    /* POSIX absolute (unchanged behavior) */
    ASSERT(cbm_hook_path_is_abs("/home/u/proj"));
    /* Windows drive roots — the #618 regression */
    ASSERT(cbm_hook_path_is_abs("C:/Users/me/proj"));
    ASSERT(cbm_hook_path_is_abs("C:/"));
    ASSERT(cbm_hook_path_is_abs("C:"));
    ASSERT(cbm_hook_path_is_abs("d:/lowercase/drive"));
    /* Not absolute → augmenter no-ops cleanly */
    ASSERT(!cbm_hook_path_is_abs("relative/path"));
    ASSERT(!cbm_hook_path_is_abs("proj"));
    ASSERT(!cbm_hook_path_is_abs(""));
    ASSERT(!cbm_hook_path_is_abs(NULL));
    PASS();
}

TEST(cli_upsert_claude_hook_existing) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-hook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);
    /* Pre-existing settings with other hooks */
    write_test_file(settingspath,
                    "{\"hooks\":{\"PreToolUse\":[{\"matcher\":\"Bash\","
                    "\"hooks\":[{\"type\":\"command\",\"command\":\"echo firewall\"}]}]}}");

    int rc = cbm_upsert_claude_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    /* Our hook added with the non-blocking matcher (issue #362). */
    ASSERT(strstr(data, "\"Grep|Glob\"") != NULL);
    ASSERT(strstr(data, "Glob|Read") == NULL);
    /* Existing hook preserved */
    ASSERT(strstr(data, "Bash") != NULL);
    ASSERT(strstr(data, "firewall") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_claude_hook_replace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-hook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);
    /* Pre-existing CMM hook with old message */
    write_test_file(settingspath,
                    "{\"hooks\":{\"PreToolUse\":[{\"matcher\":\"Grep|Glob|Read\","
                    "\"hooks\":[{\"type\":\"command\",\"command\":\"echo old-cmm-message\"}]}]}}");

    int rc = cbm_upsert_claude_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    /* Old message gone, new hook script path present */
    ASSERT(strstr(data, "old-cmm-message") == NULL);
    ASSERT(strstr(data, "cbm-code-discovery-gate") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_claude_hook_preserves_others) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-hook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);
    write_test_file(settingspath,
                    "{\"apiKey\":\"sk-123\","
                    "\"hooks\":{\"PreToolUse\":[{\"matcher\":\"Bash\","
                    "\"hooks\":[{\"type\":\"command\",\"command\":\"echo guard\"}]}]}}");

    cbm_upsert_claude_hooks(settingspath);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    /* Non-hook settings preserved */
    ASSERT(strstr(data, "apiKey") != NULL);
    ASSERT(strstr(data, "sk-123") != NULL);
    /* Bash hook preserved */
    ASSERT(strstr(data, "Bash") != NULL);
    ASSERT(strstr(data, "guard") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_remove_claude_hooks) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-hook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);

    /* Install then remove */
    cbm_upsert_claude_hooks(settingspath);
    int rc = cbm_remove_claude_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "Grep|Glob|Read") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group D: Pre-Tool Hook Upsert — Gemini CLI / Antigravity
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_gemini_hook_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ghook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);

    int rc = cbm_upsert_gemini_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "BeforeTool") != NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_gemini_hook_existing) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ghook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);
    write_test_file(settingspath,
                    "{\"hooks\":{\"BeforeTool\":[{\"matcher\":\"shell\","
                    "\"hooks\":[{\"type\":\"command\",\"command\":\"echo guard\"}]}]}}");

    int rc = cbm_upsert_gemini_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    /* Our hook added */
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);
    /* Existing hook preserved */
    ASSERT(strstr(data, "shell") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_gemini_hook_replace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ghook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);
    write_test_file(
        settingspath,
        "{\"hooks\":{\"BeforeTool\":[{\"matcher\":\"google_search|read_file|grep_search\","
        "\"hooks\":[{\"type\":\"command\",\"command\":\"echo old-cmm\"}]}]}}");

    int rc = cbm_upsert_gemini_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "old-cmm") == NULL);
    ASSERT(strstr(data, "codebase-memory-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_remove_gemini_hooks) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ghook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);

    cbm_upsert_gemini_hooks(settingspath);
    int rc = cbm_remove_gemini_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "codebase-memory-mcp") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group E: Skill descriptions use directive pattern
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_skill_descriptions_directive) {
    /* Verify skill description has trigger phrases for agent matching */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        ASSERT(strstr(sk[i].content, "Triggers on:") != NULL);
        ASSERT(strstr(sk[i].content, "search_graph") != NULL);
    }
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group F: Config store (persistent key-value)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_config_open_close) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);
    cbm_config_close(cfg);

    /* DB file should exist */
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/_config.db", tmpdir);
    struct stat st;
    ASSERT_EQ(stat(dbpath, &st), 0);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_config_get_set) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);

    /* Default when key doesn't exist */
    ASSERT_STR_EQ(cbm_config_get(cfg, "foo", "default"), "default");

    /* Set and get */
    ASSERT_EQ(cbm_config_set(cfg, "foo", "bar"), 0);
    ASSERT_STR_EQ(cbm_config_get(cfg, "foo", "default"), "bar");

    /* Overwrite */
    ASSERT_EQ(cbm_config_set(cfg, "foo", "baz"), 0);
    ASSERT_STR_EQ(cbm_config_get(cfg, "foo", "default"), "baz");

    cbm_config_close(cfg);
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_config_get_bool) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);

    /* Default */
    ASSERT_FALSE(cbm_config_get_bool(cfg, "auto_index", false));
    ASSERT_TRUE(cbm_config_get_bool(cfg, "auto_index", true));

    /* true variants */
    cbm_config_set(cfg, "k1", "true");
    ASSERT_TRUE(cbm_config_get_bool(cfg, "k1", false));
    cbm_config_set(cfg, "k2", "1");
    ASSERT_TRUE(cbm_config_get_bool(cfg, "k2", false));
    cbm_config_set(cfg, "k3", "on");
    ASSERT_TRUE(cbm_config_get_bool(cfg, "k3", false));

    /* false variants */
    cbm_config_set(cfg, "k4", "false");
    ASSERT_FALSE(cbm_config_get_bool(cfg, "k4", true));
    cbm_config_set(cfg, "k5", "0");
    ASSERT_FALSE(cbm_config_get_bool(cfg, "k5", true));

    cbm_config_close(cfg);
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_config_get_int) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);

    ASSERT_EQ(cbm_config_get_int(cfg, "limit", 50000), 50000);

    cbm_config_set(cfg, "limit", "20000");
    ASSERT_EQ(cbm_config_get_int(cfg, "limit", 50000), 20000);

    /* Non-numeric → default */
    cbm_config_set(cfg, "limit", "abc");
    ASSERT_EQ(cbm_config_get_int(cfg, "limit", 50000), 50000);

    cbm_config_close(cfg);
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_config_delete) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);

    cbm_config_set(cfg, "foo", "bar");
    ASSERT_STR_EQ(cbm_config_get(cfg, "foo", ""), "bar");

    cbm_config_delete(cfg, "foo");
    ASSERT_STR_EQ(cbm_config_get(cfg, "foo", "gone"), "gone");

    cbm_config_close(cfg);
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_config_persists) {
    /* Values survive close + reopen */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);
    cbm_config_set(cfg, "auto_index", "true");
    cbm_config_close(cfg);

    /* Reopen */
    cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);
    ASSERT_TRUE(cbm_config_get_bool(cfg, "auto_index", false));
    cbm_config_close(cfg);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group H: cbm_replace_binary (update command helper)
 * ═══════════════════════════════════════════════════════════════════ */

#ifndef _WIN32

TEST(replace_binary_overwrites_readonly) {
    /* Simulate #114: existing binary has mode 0500 (no write permission).
     * cbm_replace_binary must unlink first, then create with 0755. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-replace-XXXXXX");
    if (!cbm_mkdtemp(tmpdir)) {
        FAIL("cbm_mkdtemp failed");
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/test-binary", tmpdir);

    /* Create a read-only file (simulating an installed binary with 0500) */
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fputs("old-content", f);
    fclose(f);
    th_make_executable(path); /* r-x------ */

    /* Replace it with new content */
    const unsigned char new_data[] = "new-content-replaced";
    int rc = cbm_replace_binary(path, new_data, (int)sizeof(new_data) - 1, 0755);
    ASSERT_EQ(rc, 0);

    /* Verify new content was written */
    FILE *check = fopen(path, "r");
    ASSERT_NOT_NULL(check);
    char buf[64] = {0};
    fread(buf, 1, sizeof(buf) - 1, check);
    fclose(check);
    ASSERT_STR_EQ(buf, "new-content-replaced");

    /* Verify permissions are 0755 */
    struct stat st;
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_mode & 0777, 0755);

    remove(path);
    rmdir(tmpdir);
    PASS();
}

TEST(replace_binary_creates_new_file) {
    /* If no existing file, cbm_replace_binary should create it. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-replace2-XXXXXX");
    if (!cbm_mkdtemp(tmpdir)) {
        FAIL("cbm_mkdtemp failed");
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/new-binary", tmpdir);

    const unsigned char data[] = "brand-new";
    int rc = cbm_replace_binary(path, data, (int)sizeof(data) - 1, 0755);
    ASSERT_EQ(rc, 0);

    FILE *check = fopen(path, "r");
    ASSERT_NOT_NULL(check);
    char buf[64] = {0};
    fread(buf, 1, sizeof(buf) - 1, check);
    fclose(check);
    ASSERT_STR_EQ(buf, "brand-new");

    remove(path);
    rmdir(tmpdir);
    PASS();
}

/* ── update: a failure names its step and the OS error (#154) ──────── */

/* Internal seam (not in cli.h): the extract and replace steps of `update`. */
extern int cbm_update_extract_and_install_for_test(const char *archive_path, const char *ext,
                                                   const char *bin_dest);

/* Runs the extract-and-replace steps with stderr sent to a file under dir and
 * copies what they printed into msg. Returns their result, or -1 if the
 * redirect failed. */
static int update_install_stderr(const char *dir, const char *archive, const char *dest, char *msg,
                                 size_t msg_size) {
    msg[0] = '\0';
    char err_path[600];
    snprintf(err_path, sizeof(err_path), "%s/stderr.txt", dir);
    (void)fflush(stderr);
    int saved = dup(fileno(stderr));
    FILE *out = fopen(err_path, "w+");
    if (saved < 0 || !out || dup2(fileno(out), fileno(stderr)) < 0) {
        if (out)
            (void)fclose(out);
        if (saved >= 0)
            close(saved);
        return -1;
    }
    int rc = cbm_update_extract_and_install_for_test(archive, "tar.gz", dest);
    (void)fflush(stderr);
    (void)dup2(saved, fileno(stderr));
    close(saved);
    rewind(out);
    size_t n = fread(msg, 1, msg_size - 1, out);
    msg[n] = '\0';
    (void)fclose(out);
    remove(err_path);
    return rc;
}

TEST(update_failure_names_extract_step_and_os_error) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "%s/cli-updext-XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmpdir)) {
        FAIL("cbm_mkdtemp failed");
    }
    char archive[512];
    char dest[512];
    char msg[2048];
    snprintf(archive, sizeof(archive), "%s/cbm-update.tar.gz", tmpdir);
    snprintf(dest, sizeof(dest), "%s/codebase-memory-mcp", tmpdir);

    /* The archive is missing: the step and the OS error are both named. */
    int rc = update_install_stderr(tmpdir, archive, dest, msg, sizeof(msg));
    ASSERT_EQ(rc, 1);
    ASSERT(strstr(msg, "error: update failed while extracting the archive: cannot open ") != NULL);
    ASSERT(strstr(msg, archive) != NULL);
    ASSERT(strstr(msg, strerror(ENOENT)) != NULL);

    /* The archive is not a tar.gz: the same step, with no OS error to add. */
    write_test_file(archive, "not a valid tar.gz");
    rc = update_install_stderr(tmpdir, archive, dest, msg, sizeof(msg));
    ASSERT_EQ(rc, 1);
    ASSERT(strstr(msg, "error: update failed while extracting the archive: the archive is "
                       "damaged") != NULL);
    struct stat st;
    ASSERT(stat(archive, &st) != 0); /* the temporary archive is cleaned up */
    ASSERT(stat(dest, &st) != 0);    /* nothing was installed */

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(update_failure_names_replace_step_and_os_error) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "%s/cli-updrep-XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmpdir)) {
        FAIL("cbm_mkdtemp failed");
    }
    char archive[512];
    char dest[512];
    char msg[2048];
    snprintf(archive, sizeof(archive), "%s/cbm-update.tar.gz", tmpdir);
    snprintf(dest, sizeof(dest), "%s/codebase-memory-mcp", tmpdir);

    const char *content = "new binary";
    int gz_len = 0;
    unsigned char *gz = create_test_targz("codebase-memory-mcp-linux-amd64",
                                          (const unsigned char *)content, (int)strlen(content),
                                          &gz_len);
    ASSERT_NOT_NULL(gz);
    FILE *f = fopen(archive, "wb");
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(fwrite(gz, 1, (size_t)gz_len, f), (size_t)gz_len);
    fclose(f);
    free(gz);

    /* The install target is a directory, so it can be neither unlinked nor
     * written: unlink fails with EPERM (macOS) or EISDIR (Linux). */
    ASSERT_EQ(cbm_mkdir(dest), 0);

    int rc = update_install_stderr(tmpdir, archive, dest, msg, sizeof(msg));
    ASSERT_EQ(rc, 1);
    ASSERT(strstr(msg, "error: update failed while replacing the installed binary: cannot "
                       "write ") != NULL);
    ASSERT(strstr(msg, dest) != NULL);
    ASSERT(strstr(msg, strerror(EPERM)) != NULL || strstr(msg, strerror(EISDIR)) != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

#endif /* _WIN32 */

/* ═══════════════════════════════════════════════════════════════════
 *  CLI tool-argument flags / per-tool --help (#680)
 * ═══════════════════════════════════════════════════════════════════ */

/* A plain `--flag value` pair maps to a string property by schema type. */
TEST(cli_build_args_json_string_flag_issue680) {
    char *err = NULL;
    char *argv[] = {"--repo-path", "/x"};
    char *json = cbm_cli_build_args_json("index_repository", 2, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT_NULL(err);
    ASSERT(strstr(json, "\"repo_path\":\"/x\"") != NULL);
    free(json);
    PASS();
}

/* An integer-typed property serializes as a JSON NUMBER, not a quoted string. */
TEST(cli_build_args_json_integer_flag_issue680) {
    char *err = NULL;
    char *argv[] = {"--limit", "100"};
    char *json = cbm_cli_build_args_json("search_graph", 2, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"limit\":100") != NULL);
    ASSERT(strstr(json, "\"limit\":\"100\"") == NULL);
    free(json);
    PASS();
}

/* A bare boolean flag (no value) becomes true. */
TEST(cli_build_args_json_bare_boolean_issue680) {
    char *err = NULL;
    char *argv[] = {"--exclude-entry-points"};
    char *json = cbm_cli_build_args_json("search_graph", 1, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"exclude_entry_points\":true") != NULL);
    free(json);
    PASS();
}

/* A repeated array-typed flag accumulates into a JSON array. */
TEST(cli_build_args_json_repeated_array_issue680) {
    char *err = NULL;
    char *argv[] = {"--semantic-query", "send", "--semantic-query", "publish"};
    char *json = cbm_cli_build_args_json("search_graph", 4, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"semantic_query\":[\"send\",\"publish\"]") != NULL);
    free(json);
    PASS();
}

/* An array-typed flag also accepts the JSON-array spelling shown by the MCP
 * schemas, expanding it into individual items instead of one opaque keyword. */
TEST(cli_build_args_json_array_literal) {
    char *err = NULL;
    char *argv[] = {"--semantic-query", "[\"create\",\"billing\",\"account\"]"};
    char *json = cbm_cli_build_args_json("search_graph", 2, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"semantic_query\":[\"create\",\"billing\",\"account\"]") != NULL);
    free(json);
    PASS();
}

/* An explicit empty JSON array contributes no items; it must not turn into
 * the literal keyword "[]". */
TEST(cli_build_args_json_array_literal_empty) {
    char *err = NULL;
    char *argv[] = {"--semantic-query", "[]"};
    char *json = cbm_cli_build_args_json("search_graph", 2, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"semantic_query\":[]") != NULL);
    ASSERT(strstr(json, "\"[]\"") == NULL);
    free(json);
    PASS();
}

/* Malformed JSON keeps the pre-existing behaviour: the value stays one
 * literal string and no error is raised. */
TEST(cli_build_args_json_array_literal_malformed) {
    char *err = NULL;
    char *argv[] = {"--semantic-query", "[\"create\","};
    char *json = cbm_cli_build_args_json("search_graph", 2, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT_NULL(err);
    ASSERT(strstr(json, "\"semantic_query\":[\"[\\\"create\\\",\"]") != NULL);
    free(json);
    PASS();
}

/* Non-string items of a well-formed JSON array are copied as typed values,
 * not stringified and not collapsed into one literal element. */
TEST(cli_build_args_json_array_literal_non_string_items) {
    char *err = NULL;
    char *argv[] = {"--semantic-query", "[1,2]"};
    char *json = cbm_cli_build_args_json("search_graph", 2, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"semantic_query\":[1,2]") != NULL);
    ASSERT(strstr(json, "\"[1,2]\"") == NULL);
    free(json);
    PASS();
}

/* Repeated flags and array literals may be mixed; items accumulate in
 * argument order. */
TEST(cli_build_args_json_array_literal_mixed_with_repeated) {
    char *err = NULL;
    char *argv[] = {"--semantic-query", "send", "--semantic-query", "[\"publish\",\"emit\"]",
                    "--semantic-query", "notify"};
    char *json = cbm_cli_build_args_json("search_graph", 6, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"semantic_query\":[\"send\",\"publish\",\"emit\",\"notify\"]") != NULL);
    free(json);
    PASS();
}

/* A repeated array literal accumulates into one array. */
TEST(cli_build_args_json_array_literal_repeated) {
    char *err = NULL;
    char *argv[] = {"--semantic-query", "[\"send\"]", "--semantic-query", "[\"publish\"]"};
    char *json = cbm_cli_build_args_json("search_graph", 4, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"semantic_query\":[\"send\",\"publish\"]") != NULL);
    free(json);
    PASS();
}

/* kebab-case flag names map to snake_case JSON keys. */
TEST(cli_build_args_json_kebab_to_snake_issue680) {
    char *err = NULL;
    char *argv[] = {"--name-pattern", "Foo.*"};
    char *json = cbm_cli_build_args_json("search_graph", 2, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"name_pattern\":\"Foo.*\"") != NULL);
    free(json);
    PASS();
}

/* `--key=value` form splits on the FIRST `=`; value may contain spaces/dashes. */
TEST(cli_build_args_json_key_equals_value_issue680) {
    char *err = NULL;
    char *argv[] = {"--repo-path=/a b"};
    char *json = cbm_cli_build_args_json("index_repository", 1, argv, &err);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"repo_path\":\"/a b\"") != NULL);
    free(json);
    PASS();
}

/* A non-`--` positional is an error: returns NULL and sets *err_out. */
TEST(cli_build_args_json_bad_positional_errors_issue680) {
    char *err = NULL;
    char *argv[] = {"foo"};
    char *json = cbm_cli_build_args_json("search_graph", 1, argv, &err);
    ASSERT_NULL(json);
    ASSERT_NOT_NULL(err);
    free(err);
    PASS();
}

/* Per-tool --help returns 0 for a known tool, -1 for an unknown one. */
TEST(cli_print_tool_help_issue680) {
    ASSERT_EQ(cbm_cli_print_tool_help("index_repository"), 0);
    ASSERT_EQ(cbm_cli_print_tool_help("nope_not_a_tool"), -1);
    PASS();
}

/* #1359 (upstream 9ca108fc): a tool without required arguments never reads
 * stdin, so an inherited pipe nobody closes cannot block it. Tools that require
 * an argument keep the piped-JSON channel; a terminal or an unknown tool never
 * reads. list_projects has only optional paging arguments. */
TEST(cli_zero_argument_tool_never_reads_stdin_issue1359) {
    ASSERT_FALSE(cbm_cli_args_from_stdin_allowed("list_projects", false));

    ASSERT_TRUE(cbm_cli_args_from_stdin_allowed("index_status", false));
    ASSERT_TRUE(cbm_cli_args_from_stdin_allowed("search_graph", false));
    ASSERT_TRUE(cbm_cli_args_from_stdin_allowed("index_repository", false));

    ASSERT_FALSE(cbm_cli_args_from_stdin_allowed("index_status", true));
    ASSERT_FALSE(cbm_cli_args_from_stdin_allowed("list_projects", true));

    ASSERT_FALSE(cbm_cli_args_from_stdin_allowed("nope_not_a_tool", false));
    ASSERT_FALSE(cbm_cli_args_from_stdin_allowed(NULL, false));
    PASS();
}

/* The self-update path verifies a downloaded archive against a published
 * checksum. That check is only meaningful if the digest is actually computed —
 * a broken hash command (it once invoked `shasum -a CBM_SZ_256`, an invalid
 * algorithm, from a bad macro rename inside the shell string) makes every
 * digest fail, and the caller then falls through and installs unverified.
 * Guard the digest itself against a known vector. */
extern int cbm_cli_sha256_file(const char *path, char *out, size_t out_size);

/* Hash `content` (len bytes) via a temp file and compare to expected hex.
 * Returns 1 on match, 0 otherwise. */
static int sha256_vector_ok(const void *content, size_t len, const char *expected) {
    char path[512];
    snprintf(path, sizeof(path), "%s/cbm_sha_XXXXXX", cbm_tmpdir());
    int fd = cbm_mkstemp(path);
    if (fd < 0) {
        return 0;
    }
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        return 0;
    }
    if (len > 0) {
        fwrite(content, 1, len, fp);
    }
    fclose(fp);

    char digest[128] = {0};
    int rc = cbm_cli_sha256_file(path, digest, sizeof(digest));
    remove(path);
    return rc == 0 && strcmp(digest, expected) == 0;
}

/* NIST FIPS 180-4 SHA-256 test vectors: empty input, a single block ("abc"),
 * and a 56-byte input that forces the length padding into a second block. */
TEST(cli_sha256_file_matches_known_vector) {
    ASSERT_TRUE(sha256_vector_ok(
        "", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    ASSERT_TRUE(sha256_vector_ok(
        "abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ASSERT_TRUE(sha256_vector_ok(
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    PASS();
}

TEST(cli_sha256_update_fragmentation_matches_one_shot) {
    static const size_t boundary_sizes[] = {0, 55, 56, 63, 64, 65, 127, 128, 129};
    uint8_t boundary_data[129];
    for (size_t i = 0; i < sizeof(boundary_data); i++) {
        boundary_data[i] = (uint8_t)((i * 37U + 11U) & 0xffU);
    }

    cbm_sha256_ctx empty;
    uint8_t empty_digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_init(&empty);
    cbm_sha256_update(&empty, NULL, 0);
    cbm_sha256_final(&empty, empty_digest);
    static const uint8_t expected_empty[CBM_SHA256_DIGEST_LEN] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
    };
    ASSERT_TRUE(memcmp(empty_digest, expected_empty, sizeof(empty_digest)) == 0);

    for (size_t size_index = 0;
         size_index < sizeof(boundary_sizes) / sizeof(boundary_sizes[0]); size_index++) {
        const size_t size = boundary_sizes[size_index];
        uint8_t expected[CBM_SHA256_DIGEST_LEN];
        cbm_sha256_ctx one_shot;
        cbm_sha256_init(&one_shot);
        cbm_sha256_update(&one_shot, boundary_data, size);
        cbm_sha256_final(&one_shot, expected);

        for (size_t split = 0; split <= size; split++) {
            uint8_t actual[CBM_SHA256_DIGEST_LEN];
            cbm_sha256_ctx fragmented;
            cbm_sha256_init(&fragmented);
            cbm_sha256_update(&fragmented, boundary_data, split);
            cbm_sha256_update(&fragmented, boundary_data + split, size - split);
            cbm_sha256_final(&fragmented, actual);
            ASSERT_TRUE(memcmp(actual, expected, sizeof(actual)) == 0);
        }
    }

    const size_t large_size = 2U * 1024U * 1024U + 129U;
    uint8_t *large_data = malloc(large_size);
    ASSERT_NOT_NULL(large_data);
    for (size_t i = 0; i < large_size; i++) {
        large_data[i] = (uint8_t)((i * 13U + i / 251U) & 0xffU);
    }
    uint8_t expected[CBM_SHA256_DIGEST_LEN];
    uint8_t actual[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_ctx one_shot;
    cbm_sha256_ctx fragmented;
    cbm_sha256_init(&one_shot);
    cbm_sha256_update(&one_shot, large_data, large_size);
    cbm_sha256_final(&one_shot, expected);
    cbm_sha256_init(&fragmented);
    for (size_t offset = 0, chunk = 1; offset < large_size; chunk = chunk % 127U + 1U) {
        size_t take = chunk < large_size - offset ? chunk : large_size - offset;
        cbm_sha256_update(&fragmented, large_data + offset, take);
        offset += take;
    }
    cbm_sha256_final(&fragmented, actual);
    free(large_data);
    ASSERT_TRUE(memcmp(actual, expected, sizeof(actual)) == 0);
    PASS();
}

TEST(cli_sha256_accelerated_backend_matches_portable) {
    const size_t size = 2U * 1024U * 1024U + 129U;
    uint8_t *data = malloc(size);
    ASSERT_NOT_NULL(data);
    for (size_t i = 0; i < size; i++) data[i] = (uint8_t)((i * 29U + 7U) & 0xffU);

    uint8_t expected[CBM_SHA256_DIGEST_LEN];
    uint8_t actual[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_ctx portable;
    cbm_sha256_backend_ctx backend;
    cbm_sha256_init(&portable);
    cbm_sha256_update(&portable, data, size);
    cbm_sha256_final(&portable, expected);
    cbm_sha256_backend_init(&backend);
    for (size_t offset = 0, chunk = 1; offset < size; chunk = chunk % 4099U + 1U) {
        size_t take = chunk < size - offset ? chunk : size - offset;
        cbm_sha256_backend_update(&backend, data + offset, take);
        offset += take;
    }
    cbm_sha256_backend_final(&backend, actual);
    free(data);
    ASSERT_TRUE(memcmp(actual, expected, sizeof(actual)) == 0);
#if defined(__APPLE__)
    ASSERT_TRUE(cbm_sha256_backend_is_accelerated());
#endif
    PASS();
}

TEST(cli_zova_ops_strict_parser_dispatch_and_exit_mapping) {
    char cache[256], repo[256], repo_link[256], spaced[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-cli-zova-ops-cache-%d", (int)getpid());
    snprintf(repo, sizeof(repo), "/tmp/cbm-cli-zova-ops-repo-%d", (int)getpid());
    snprintf(repo_link, sizeof(repo_link), "/tmp/cbm-cli-zova-ops-link-%d", (int)getpid());
    snprintf(spaced, sizeof(spaced), "%s/output with spaces", cache);
    test_rmdir_r(cache); test_rmdir_r(repo); remove(repo_link);
    ASSERT_EQ(test_mkdirp(cache), 0);
    ASSERT_EQ(test_mkdirp(repo), 0);
#ifndef _WIN32
    ASSERT_EQ(symlink(repo, repo_link), 0);
#else
    snprintf(repo_link, sizeof(repo_link), "%s", repo);
#endif
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    ASSERT_EQ(setenv("CBM_CACHE_DIR", cache, 1), 0);
    char workspace[] = "w1_00000000000000000000000000000000";

    char *status[] = {"status", "--json"};
    char *backup[] = {"backup", "--output", spaced, "--json"};
    char *restore[] = {"restore", "--input", spaced, "--confirm-replace", "--json"};
    char *export_db[] = {"export-database", "--output", spaced, "--json"};
    char *import_db[] = {"import-database", "--input", spaced, "--confirm-replace", "--json"};
    char *export_ws[] = {"export-workspace", "--workspace-id", workspace,
                         "--output", spaced, "--json"};
    char *import_ws[] = {"import-workspace", "--input", spaced, "--replace", "--json"};
    char *delete_ws[] = {"delete-workspace", "--workspace-id", workspace,
                         "--confirm-workspace", workspace, "--json"};
    char *compact[] = {"compact", "--json"};
    char *health[] = {"health", "--workspace-id", workspace, "--json"};
    char *recover[] = {"recover-workspace", "--workspace-id", workspace,
                       "--repo-path", repo_link, "--json"};
    ASSERT_EQ(cbm_cmd_zova_ops(2, status), 1);
    ASSERT_EQ(cbm_cmd_zova_ops(4, backup), 1);
    ASSERT_EQ(cbm_cmd_zova_ops(5, restore), 1);
    ASSERT_EQ(cbm_cmd_zova_ops(4, export_db), 1);
    ASSERT_EQ(cbm_cmd_zova_ops(5, import_db), 1);
    ASSERT_EQ(cbm_cmd_zova_ops(6, export_ws), 1);
    ASSERT_EQ(cbm_cmd_zova_ops(5, import_ws), 1);
    ASSERT_EQ(cbm_cmd_zova_ops(6, delete_ws), 1);
    ASSERT_EQ(cbm_cmd_zova_ops(2, compact), 1);
    ASSERT_EQ(cbm_cmd_zova_ops(4, health), 1);
    ASSERT_EQ(cbm_cmd_zova_ops(6, recover), 1);

    char *missing_action[] = {"--json"};
    char *unknown_action[] = {"vacuum", "--json"};
    char *missing_json[] = {"status"};
    char *duplicate_json[] = {"status", "--json", "--json"};
    char *unknown_flag[] = {"status", "--wat", "--json"};
    char *missing_output[] = {"backup", "--json"};
    char *missing_input[] = {"restore", "--confirm-replace", "--json"};
    char *missing_replace_confirmation[] = {"import-database", "--input", spaced, "--json"};
    char *missing_workspace[] = {"export-workspace", "--output", spaced, "--json"};
    char *missing_delete_confirmation[] = {"delete-workspace", "--workspace-id", workspace,
                                           "--json"};
    char *missing_repo[] = {"recover-workspace", "--workspace-id", workspace, "--json"};
    char *invalid_workspace[] = {"health", "--workspace-id", "workspace", "--json"};
    char *duplicate_input[] = {"import-workspace", "--input", spaced,
                               "--input", spaced, "--json"};
    char *wrong_flag_for_action[] = {"status", "--replace", "--json"};
    ASSERT_EQ(cbm_cmd_zova_ops(1, missing_action), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(2, unknown_action), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(1, missing_json), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(3, duplicate_json), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(3, unknown_flag), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(2, missing_output), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(3, missing_input), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(4, missing_replace_confirmation), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(4, missing_workspace), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(4, missing_delete_confirmation), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(4, missing_repo), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(4, invalid_workspace), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(6, duplicate_input), 2);
    ASSERT_EQ(cbm_cmd_zova_ops(3, wrong_flag_for_action), 2);

    char *mismatch[] = {"delete-workspace", "--workspace-id", workspace,
                        "--confirm-workspace", "w1_11111111111111111111111111111111", "--json"};
    ASSERT_EQ(cbm_cmd_zova_ops(6, mismatch), 1);

    if (saved_cache_copy) ASSERT_EQ(setenv("CBM_CACHE_DIR", saved_cache_copy, 1), 0);
    else ASSERT_EQ(unsetenv("CBM_CACHE_DIR"), 0);
    free(saved_cache_copy);
#ifndef _WIN32
    remove(repo_link);
#endif
    test_rmdir_r(repo); test_rmdir_r(cache);
    PASS();
}

/* Runs zova-ops with stdout sent to out_path and copies the JSON report's
 * "reason" into reason. Returns the exit code, or -1 if the redirect failed. */
static int zova_ops_reason(int argc, char **argv, const char *out_path, char *reason,
                           size_t reason_size) {
    reason[0] = '\0';
    (void)fflush(stdout);
    int saved = dup(fileno(stdout));
    FILE *out = fopen(out_path, "w+");
    if (saved < 0 || !out || dup2(fileno(out), fileno(stdout)) < 0) {
        if (out)
            (void)fclose(out);
        if (saved >= 0)
            close(saved);
        return -1;
    }
    int rc = cbm_cmd_zova_ops(argc, argv);
    (void)fflush(stdout);
    (void)dup2(saved, fileno(stdout));
    close(saved);
    char buf[4096] = {0};
    rewind(out);
    size_t n = fread(buf, 1, sizeof(buf) - 1, out);
    (void)fclose(out);
    yyjson_doc *doc = yyjson_read(buf, n, 0);
    const char *value = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "reason"));
    snprintf(reason, reason_size, "%s", value ? value : "");
    yyjson_doc_free(doc);
    return rc;
}

/* Pins every zova-ops action's flag rules and the order of usage errors. */
TEST(cli_zova_ops_flag_rules_and_error_order) {
    enum { IN, OUT, WS, CONFIRM, REPO, CONFIRM_REPLACE, REPLACE, NFLAGS };
    static const char *const flag_names[NFLAGS] = {
        "--input",     "--output",          "--workspace-id", "--confirm-workspace",
        "--repo-path", "--confirm-replace", "--replace",
    };
    static const struct {
        const char *action;
        unsigned required, optional;
    } rules[] = {
        {"status", 0, 0},
        {"backup", 1u << OUT, 0},
        {"restore", 1u << IN | 1u << CONFIRM_REPLACE, 0},
        {"export-database", 1u << OUT, 0},
        {"import-database", 1u << IN | 1u << CONFIRM_REPLACE, 0},
        {"export-workspace", 1u << WS | 1u << OUT, 0},
        {"import-workspace", 1u << IN, 1u << REPLACE},
        {"delete-workspace", 1u << WS | 1u << CONFIRM, 0},
        {"compact", 0, 0},
        {"health", 0, 1u << WS},
        {"recover-workspace", 1u << WS | 1u << REPO, 0},
        {"migrate-format", 0, 0},
    };
    char cache[256], repo[256], file[512], out_path[512], reason[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-cli-zova-rules-cache-%d", (int)getpid());
    snprintf(repo, sizeof(repo), "/tmp/cbm-cli-zova-rules-repo-%d", (int)getpid());
    snprintf(file, sizeof(file), "%s/archive", cache);
    snprintf(out_path, sizeof(out_path), "%s/stdout.json", cache);
    test_rmdir_r(cache);
    test_rmdir_r(repo);
    ASSERT_EQ(test_mkdirp(cache), 0);
    ASSERT_EQ(test_mkdirp(repo), 0);
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    ASSERT_EQ(setenv("CBM_CACHE_DIR", cache, 1), 0);
    char workspace[] = "w1_00000000000000000000000000000000";
    char *values[NFLAGS] = {file, file, workspace, workspace, repo, NULL, NULL};
    const char *invalid_flags = "invalid flags for zova-ops action";

    /* For each action: its required flags pass validation; dropping one, or
     * adding a flag that is neither required nor optional, is a usage error;
     * adding an optional flag still passes. */
    for (size_t r = 0; r < sizeof(rules) / sizeof(rules[0]); r++) {
        for (int variant = -1; variant < NFLAGS; variant++) {
            unsigned flags = rules[r].required;
            if (variant >= 0)
                flags ^= 1u << variant;
            bool expect_valid = (flags & rules[r].required) == rules[r].required &&
                                (flags & ~(rules[r].required | rules[r].optional)) == 0;
            char *argv[2 + 2 * NFLAGS];
            int argc = 0;
            argv[argc++] = (char *)rules[r].action;
            for (int f = 0; f < NFLAGS; f++) {
                if (!(flags & 1u << f))
                    continue;
                argv[argc++] = (char *)flag_names[f];
                if (values[f])
                    argv[argc++] = values[f];
            }
            argv[argc++] = "--json";
            int rc = zova_ops_reason(argc, argv, out_path, reason, sizeof(reason));
            if (expect_valid) {
                ASSERT_TRUE(rc == 0 || rc == 1);
                ASSERT_STR_NEQ(reason, invalid_flags);
            } else {
                ASSERT_EQ(rc, 2);
                ASSERT_STR_EQ(reason, invalid_flags);
            }
        }
    }

    /* Usage errors, in the order they are checked. */
    const struct {
        int argc;
        const char *argv[6];
        const char *reason;
    } errors[] = {
        {0, {NULL}, "missing zova-ops action"},
        {1, {"--json"}, "missing zova-ops action"},
        {2, {"vacuum", "--wat"}, "unknown zova-ops action"},
        {3, {"status", "--json", "--json"}, "duplicate --json"},
        {3, {"restore", "--confirm-replace", "--confirm-replace"}, "duplicate --confirm-replace"},
        {3, {"import-workspace", "--replace", "--replace"}, "duplicate --replace"},
        {2, {"status", "--wat"}, "unknown zova-ops flag"},
        {3, {"status", "wat", "--json"}, "unknown zova-ops flag"},
        {2, {"backup", "--output"}, "missing or duplicate flag value"},
        {3, {"backup", "--output", "--json"}, "missing or duplicate flag value"},
        {5, {"backup", "--output", "a", "--output", "b"}, "missing or duplicate flag value"},
        {3, {"health", "--workspace-id", "bad"}, "--json is required"},
        {5,
         {"status", "--confirm-workspace", "bad", "--workspace-id", "bad"},
         "--json is required"},
        {6,
         {"status", "--confirm-workspace", "bad", "--workspace-id", "bad", "--json"},
         "invalid --workspace-id"},
        {4, {"status", "--confirm-workspace", "bad", "--json"}, "invalid --confirm-workspace"},
        {4, {"status", "--confirm-workspace", workspace, "--json"}, invalid_flags},
    };
    for (size_t e = 0; e < sizeof(errors) / sizeof(errors[0]); e++) {
        char *argv[6];
        memcpy(argv, errors[e].argv, sizeof(argv));
        ASSERT_EQ(zova_ops_reason(errors[e].argc, argv, out_path, reason, sizeof(reason)), 2);
        ASSERT_STR_EQ(reason, errors[e].reason);
    }

    if (saved_cache_copy)
        ASSERT_EQ(setenv("CBM_CACHE_DIR", saved_cache_copy, 1), 0);
    else
        ASSERT_EQ(unsetenv("CBM_CACHE_DIR"), 0);
    free(saved_cache_copy);
    test_rmdir_r(repo);
    test_rmdir_r(cache);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Suite definition
 * ═══════════════════════════════════════════════════════════════════ */

/* CBM_INDEX_MAX_RESTARTS: 0 means no restarts, and text that does not read
 * cleanly keeps the default (upstream 6b801eec). */
TEST(cli_index_restart_cap_honours_zero_and_refuses_junk) {
    enum { INDEX_RESTART_CAP_DEFAULT = 100 };
    const char *name = "CBM_INDEX_MAX_RESTARTS";
    char *saved = getenv(name) ? strdup(getenv(name)) : NULL;

    cbm_setenv(name, "7", 1);
    ASSERT_EQ(cbm_index_restart_cap_for_testing(), 7);

    cbm_unsetenv(name);
    ASSERT_EQ(cbm_index_restart_cap_for_testing(), INDEX_RESTART_CAP_DEFAULT);

    cbm_setenv(name, "0", 1);
    ASSERT_EQ(cbm_index_restart_cap_for_testing(), 0);

    const char *unreadable[] = {"abc", "5x", " 5", "5 ", ""};
    for (size_t i = 0; i < sizeof(unreadable) / sizeof(unreadable[0]); i++) {
        cbm_setenv(name, unreadable[i], 1);
        ASSERT_EQ(cbm_index_restart_cap_for_testing(), INDEX_RESTART_CAP_DEFAULT);
    }

    if (saved) {
        cbm_setenv(name, saved, 1);
        free(saved);
    } else {
        cbm_unsetenv(name);
    }
    PASS();
}

SUITE(cli) {
    RUN_TEST(cli_index_restart_cap_honours_zero_and_refuses_junk);
    RUN_TEST(cli_sha256_file_matches_known_vector);
    RUN_TEST(cli_sha256_update_fragmentation_matches_one_shot);
    RUN_TEST(cli_sha256_accelerated_backend_matches_portable);
    /* Version (2 tests — selfupdate_test.go) */
    RUN_TEST(cli_compare_versions);
    RUN_TEST(cli_version_get_set);

    /* Shell RC detection (5 tests — install_test.go) */
    RUN_TEST(cli_detect_shell_rc_zsh);
    RUN_TEST(cli_detect_shell_rc_bash);
    RUN_TEST(cli_detect_shell_rc_bash_with_bashrc);
    RUN_TEST(cli_detect_shell_rc_fish);
    RUN_TEST(cli_detect_shell_rc_default);

    /* CLI binary detection (3 tests — install_test.go) */
    RUN_TEST(cli_find_cli_not_found);
    RUN_TEST(cli_find_cli_on_path);
    RUN_TEST(cli_find_cli_fallback_paths);

    /* Dry-run flag parsing (1 test — install_test.go) */
    RUN_TEST(cli_dry_run_flags);

    /* Skill management (7 tests — install_test.go) */
    RUN_TEST(cli_skill_creation);
    RUN_TEST(cli_skill_idempotent);
    RUN_TEST(cli_skill_force_overwrite);
    RUN_TEST(cli_uninstall_removes_skills);
    RUN_TEST(cli_remove_old_monolithic_skill);
    RUN_TEST(cli_skill_files_content);
    RUN_TEST(cli_tool_registry_matches_docs);
    RUN_TEST(cli_install_plan_targets_existing_opencode_jsonc);
    RUN_TEST(cli_upsert_keeps_client_keys_in_our_entry);
    RUN_TEST(cli_upsert_refuses_malformed_config);
    RUN_TEST(cli_codex_instructions);

    /* Editor MCP: Cursor/Windsurf/Gemini (5 tests — install_test.go) */
    RUN_TEST(cli_editor_mcp_install);
    RUN_TEST(cli_editor_mcp_idempotent);
    RUN_TEST(cli_editor_mcp_preserves_others);
    RUN_TEST(cli_editor_mcp_uninstall);
    RUN_TEST(cli_junie_mcp_install_issue651);
    RUN_TEST(cli_gemini_mcp_install);
    RUN_TEST(cli_openclaw_mcp_install_uses_nested_servers);
    RUN_TEST(cli_openclaw_mcp_preserves_existing_config);
    RUN_TEST(cli_openclaw_mcp_uninstall_uses_nested_servers);

    /* VS Code MCP (2 tests — install_test.go) */
    RUN_TEST(cli_vscode_mcp_install);
    RUN_TEST(cli_vscode_mcp_uninstall);

    /* Zed MCP (3 tests — install_test.go) */
    RUN_TEST(cli_zed_mcp_install);
    RUN_TEST(cli_zed_mcp_preserves_settings);
    RUN_TEST(cli_zed_mcp_uninstall);
    RUN_TEST(cli_zed_mcp_jsonc_comments);

    /* PATH management (3 tests) */
    RUN_TEST(cli_ensure_path_append);
    RUN_TEST(cli_ensure_path_already_present);
    RUN_TEST(cli_ensure_path_dry_run);
    RUN_TEST(cli_ensure_path_fish_syntax_issue319);

    /* File copy (2 tests — update_test.go) */
    RUN_TEST(cli_copy_file);
    RUN_TEST(cli_copy_file_source_not_found);

    /* Tar.gz extraction (3 tests — update_test.go) */
    RUN_TEST(cli_extract_binary_from_targz);
    RUN_TEST(cli_extract_binary_from_targz_not_found);
    RUN_TEST(cli_extract_binary_from_targz_invalid_data);
    RUN_TEST(cli_extract_binary_from_zip);
    RUN_TEST(cli_extract_binary_from_zip_not_found);
    RUN_TEST(cli_extract_binary_from_zip_path_traversal);
    RUN_TEST(cli_extract_binary_from_zip_invalid);
    RUN_TEST(cli_extract_binary_from_zip_rejects_truncated_deflate_size_over_int_max);

    /* Dry-run lifecycle (2 tests) */
    RUN_TEST(cli_install_dry_run);
    RUN_TEST(cli_uninstall_dry_run);
    RUN_TEST(cli_uninstall_help);
    RUN_TEST(cli_uninstall_indexes_kept_by_default);

    /* Full lifecycle (1 test — cli_test.go) */
    RUN_TEST(cli_install_and_uninstall);

    /* Binary swap on install --force (#472) */
    RUN_TEST(cli_install_copies_binary_to_target_issue472);
    RUN_TEST(cli_install_same_file_guard_issue472);

    /* YAML parser (7 unit tests) */
    RUN_TEST(cli_yaml_parse_simple);
    RUN_TEST(cli_yaml_parse_nested);
    RUN_TEST(cli_yaml_parse_list);
    RUN_TEST(cli_yaml_parse_bool);
    RUN_TEST(cli_yaml_parse_comments);
    RUN_TEST(cli_yaml_parse_empty);
    RUN_TEST(cli_yaml_has);

    /* Agent detection (6 tests — group A) */
    RUN_TEST(cli_detect_agents_finds_claude);
    RUN_TEST(cli_detect_agents_finds_claude_via_env);
    RUN_TEST(cli_detect_agents_finds_codex);
    RUN_TEST(cli_detect_agents_finds_cursor_issue222);
    RUN_TEST(cli_install_plan_receipt_no_mutation_issue388);
    RUN_TEST(cli_install_help_no_mutation);
    RUN_TEST(cli_doctor_where_reports_paths);
    RUN_TEST(cli_codex_session_hook_issue330);
    RUN_TEST(cli_gemini_session_hook_parity);
    RUN_TEST(cli_claude_subagent_hook);
    RUN_TEST(cli_claude_subagent_hook_preserves_user_entry);
    RUN_TEST(cli_claude_exec_hooks_replace_shell_form);
#ifndef _WIN32
    RUN_TEST(cli_hook_augment_lifecycle_reminders);
#endif
    RUN_TEST(cli_detect_agents_finds_gemini);
    RUN_TEST(cli_detect_agents_finds_zed);
    RUN_TEST(cli_detect_agents_finds_antigravity);
    RUN_TEST(cli_detect_agents_finds_kilocode);
    RUN_TEST(cli_detect_agents_finds_kiro);
    RUN_TEST(cli_detect_agents_finds_junie_issue651);
    RUN_TEST(cli_detect_agents_none_found);

    /* Codex MCP config upsert (6 tests — group B) */
    RUN_TEST(cli_upsert_codex_mcp_fresh);
    RUN_TEST(cli_upsert_codex_mcp_existing);
    RUN_TEST(cli_upsert_codex_mcp_replace);
    RUN_TEST(cli_upsert_codex_mcp_quoted_and_whitespace);
    RUN_TEST(cli_upsert_codex_mcp_header_directly_followed_by_header);
    RUN_TEST(cli_upsert_codex_mcp_two_tables_cleaned_up);

    /* Zed MCP format fix (1 test — group B) */
    RUN_TEST(cli_zed_mcp_uses_args_format);

    /* OpenCode MCP config upsert (2 tests — group B) */
    RUN_TEST(cli_upsert_opencode_mcp_fresh);
    RUN_TEST(cli_upsert_opencode_mcp_existing);

    /* Antigravity MCP config upsert (2 tests — group B) */
    RUN_TEST(cli_upsert_antigravity_mcp_fresh);
    RUN_TEST(cli_upsert_antigravity_mcp_replace);

    /* Instructions file upsert (6 tests — group C) */
    RUN_TEST(cli_upsert_instructions_fresh);
    RUN_TEST(cli_upsert_instructions_existing);
    RUN_TEST(cli_upsert_instructions_replace);
    RUN_TEST(cli_upsert_instructions_no_duplicate);
    RUN_TEST(cli_remove_instructions);
    RUN_TEST(cli_agent_instructions_content);

    /* Claude Code hooks (5 tests — group D) */
    RUN_TEST(cli_hook_gate_script_no_predictable_tmp_issue384);
    RUN_TEST(cli_hook_augment_path_is_abs);
    RUN_TEST(cli_upsert_claude_hook_fresh);
    RUN_TEST(cli_upsert_claude_hook_existing);
    RUN_TEST(cli_upsert_claude_hook_replace);
    RUN_TEST(cli_upsert_claude_hook_preserves_others);
    RUN_TEST(cli_remove_claude_hooks);

    /* Gemini CLI hooks (4 tests — group D) */
    RUN_TEST(cli_upsert_gemini_hook_fresh);
    RUN_TEST(cli_upsert_gemini_hook_existing);
    RUN_TEST(cli_upsert_gemini_hook_replace);
    RUN_TEST(cli_remove_gemini_hooks);

    /* Skill directive descriptions (1 test — group E) */
    RUN_TEST(cli_skill_descriptions_directive);

    /* Config store (6 tests — group F) */
    RUN_TEST(cli_config_open_close);
    RUN_TEST(cli_config_get_set);
    RUN_TEST(cli_config_get_bool);
    RUN_TEST(cli_config_get_int);
    RUN_TEST(cli_config_delete);
    RUN_TEST(cli_config_persists);

    /* Replace binary (update command helper — group H) */
#ifndef _WIN32
    RUN_TEST(replace_binary_overwrites_readonly);
    RUN_TEST(replace_binary_creates_new_file);
    RUN_TEST(update_failure_names_extract_step_and_os_error);
    RUN_TEST(update_failure_names_replace_step_and_os_error);
#endif

    /* CLI tool-argument flags / per-tool --help (#680) */
    RUN_TEST(cli_build_args_json_string_flag_issue680);
    RUN_TEST(cli_build_args_json_integer_flag_issue680);
    RUN_TEST(cli_build_args_json_bare_boolean_issue680);
    RUN_TEST(cli_build_args_json_repeated_array_issue680);
    RUN_TEST(cli_build_args_json_array_literal);
    RUN_TEST(cli_build_args_json_array_literal_empty);
    RUN_TEST(cli_build_args_json_array_literal_malformed);
    RUN_TEST(cli_build_args_json_array_literal_non_string_items);
    RUN_TEST(cli_build_args_json_array_literal_mixed_with_repeated);
    RUN_TEST(cli_build_args_json_array_literal_repeated);
    RUN_TEST(cli_build_args_json_kebab_to_snake_issue680);
    RUN_TEST(cli_build_args_json_key_equals_value_issue680);
    RUN_TEST(cli_build_args_json_bad_positional_errors_issue680);
    RUN_TEST(cli_print_tool_help_issue680);
    RUN_TEST(cli_zero_argument_tool_never_reads_stdin_issue1359);
    RUN_TEST(cli_zova_ops_strict_parser_dispatch_and_exit_mapping);
    RUN_TEST(cli_zova_ops_flag_rules_and_error_order);
}
