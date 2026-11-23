/*
 * test_subprocess.c — foundation/subprocess: spawn + supervise + classify.
 *
 * Two layers:
 *   1. cbm_proc_classify() — pure, exercised on EVERY platform (so the Windows
 *      NTSTATUS crash-code mapping is guarded on Linux/macOS CI too, not just an
 *      untested Windows branch).
 *   2. cbm_subprocess_run() — real spawn/reap, exercised on POSIX via /bin/sh
 *      (SKIP_PLATFORM on Windows, which lacks it).
 */
#include "test_framework.h"
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/file_lock.h"
#include "../src/foundation/git_env.h"
#include "../src/foundation/subprocess.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h> /* _NSGetExecutablePath — deleted-self driver */
#endif

/* ── Layer 1: pure classifier (all platforms) ─────────────────────────────── */

TEST(subprocess_classify_clean) {
    ASSERT_EQ(cbm_proc_classify(true, 0, 0, false), CBM_PROC_CLEAN);
    PASS();
}

TEST(subprocess_classify_exit_nonzero) {
    ASSERT_EQ(cbm_proc_classify(true, 3, 0, false), CBM_PROC_EXIT_NONZERO);
    ASSERT_EQ(cbm_proc_classify(true, 127, 0, false), CBM_PROC_EXIT_NONZERO);
    PASS();
}

/* Windows exception exit codes classify as CRASH — the key reason to unit-test
 * the classifier cross-platform. 0xC0000005 = access violation (SIGSEGV analog),
 * 0xC00000FD = stack overflow (the #668 reporter's 0xC00000FD). */
TEST(subprocess_classify_windows_crash_codes) {
    ASSERT_EQ(cbm_proc_classify(true, (int)0xC0000005u, 0, false), CBM_PROC_CRASH);
    ASSERT_EQ(cbm_proc_classify(true, (int)0xC00000FDu, 0, false), CBM_PROC_CRASH);
    ASSERT_EQ(cbm_proc_classify(true, (int)0xC000001Du, 0, false), CBM_PROC_CRASH);
    PASS();
}

TEST(subprocess_classify_posix_fault_signal_is_crash) {
#ifndef _WIN32
    ASSERT_EQ(cbm_proc_classify(false, -1, SIGSEGV, false), CBM_PROC_CRASH);
    ASSERT_EQ(cbm_proc_classify(false, -1, SIGABRT, false), CBM_PROC_CRASH);
    ASSERT_EQ(cbm_proc_classify(false, -1, SIGBUS, false), CBM_PROC_CRASH);
#endif
    PASS();
}

TEST(subprocess_classify_non_fault_signal_is_killed) {
#ifndef _WIN32
    ASSERT_EQ(cbm_proc_classify(false, -1, SIGTERM, false), CBM_PROC_KILLED);
    ASSERT_EQ(cbm_proc_classify(false, -1, SIGKILL, false), CBM_PROC_KILLED);
#endif
    PASS();
}

/* timed_out dominates every other signal — a killed-for-hang child is HANG,
 * not KILLED, even though we deliver SIGKILL/TerminateProcess to end it. */
TEST(subprocess_classify_timeout_dominates) {
    ASSERT_EQ(cbm_proc_classify(false, -1, 9 /*SIGKILL*/, true), CBM_PROC_HANG);
    ASSERT_EQ(cbm_proc_classify(true, 0, 0, true), CBM_PROC_HANG);
    PASS();
}

TEST(subprocess_outcome_str) {
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_CLEAN), "clean");
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_CRASH), "crash");
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_HANG), "hang");
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_EXIT_NONZERO), "exit_nonzero");
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_KILLED), "killed");
    PASS();
}

/* ── Layer 2: real spawn/reap (POSIX) ─────────────────────────────────────── */

#ifndef _WIN32
static cbm_proc_result_t run_sh(const char *script, int quiet_timeout_ms) {
    const char *argv[] = {"/bin/sh", "-c", script, NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = "/bin/sh";
    opts.argv = argv;
    opts.log_file = NULL;
    opts.quiet_timeout_ms = quiet_timeout_ms;
    cbm_proc_result_t r;
    cbm_subprocess_run(&opts, &r);
    return r;
}
#endif

TEST(subprocess_run_clean) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX /bin/sh spawn");
#else
    cbm_proc_result_t r = run_sh("exit 0", 0);
    ASSERT_EQ(r.outcome, CBM_PROC_CLEAN);
    ASSERT_EQ(r.exit_code, 0);
    PASS();
#endif
}

TEST(subprocess_run_exit_nonzero) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX /bin/sh spawn");
#else
    cbm_proc_result_t r = run_sh("exit 7", 0);
    ASSERT_EQ(r.outcome, CBM_PROC_EXIT_NONZERO);
    ASSERT_EQ(r.exit_code, 7);
    ASSERT_GT(r.pid, 0); /* the child is identifiable after it exits (#152) */
    PASS();
#endif
}

/* A child that dies of SIGSEGV must classify as CRASH — NOT exit_nonzero and NOT
 * killed. This is the whole point of the primitive: distinguish a crash from a
 * clean failure so the supervisor can quarantine the culprit. */
TEST(subprocess_run_crash_is_crash) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX signal semantics");
#else
    cbm_proc_result_t r = run_sh("kill -SEGV $$", 0);
    ASSERT_EQ(r.outcome, CBM_PROC_CRASH);
    ASSERT_EQ(r.term_signal, SIGSEGV);
    PASS();
#endif
}

/* A child that makes no progress within the quiet-timeout is killed and reported
 * as HANG — the sibling failure mode of a crash (external-scanner infinite loop). */
TEST(subprocess_run_hang_is_hang) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX /bin/sh spawn");
#else
    cbm_proc_result_t r = run_sh("sleep 30", 300 /* ms quiet-timeout */);
    ASSERT_EQ(r.outcome, CBM_PROC_HANG);
    PASS();
#endif
}

/* A spawn of a non-existent binary fails cleanly (no child), not a crash. */
TEST(subprocess_run_spawn_failure) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX exec semantics");
#else
    /* execv of a bogus path: fork succeeds, child _exit(127). We classify the
     * reaped 127 as exit_nonzero — spawn_failed is reserved for fork() failing. */
    const char *argv[] = {"/nonexistent/cbm-bogus-binary", NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = "/nonexistent/cbm-bogus-binary";
    opts.argv = argv;
    cbm_proc_result_t r;
    int rc = cbm_subprocess_run(&opts, &r);
    ASSERT_EQ(rc, 0); /* fork itself succeeded */
    ASSERT_EQ(r.outcome, CBM_PROC_EXIT_NONZERO);
    ASSERT_EQ(r.exit_code, 127);
    PASS();
#endif
}

/* The wall-clock deadline fires even while the child keeps making progress
 * (a quiet-timeout alone would not), and with own_process_group the kill takes
 * the backgrounded grandchild too — the search_code sh/xargs/grep shape. */
TEST(subprocess_run_deadline_kills_process_group) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX process groups");
#else
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/cbm_deadline_XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(dir));
    char log[1100];
    char pidfile[1100];
    char script[2400];
    snprintf(log, sizeof(log), "%s/out", dir);
    snprintf(pidfile, sizeof(pidfile), "%s/grandchild.pid", dir);
    snprintf(script, sizeof(script),
             "sleep 30 & echo $! > '%s'; while :; do echo tick; sleep 0.05; done", pidfile);
    const char *argv[] = {"/bin/sh", "-c", script, NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = "/bin/sh";
    opts.argv = argv;
    opts.log_file = log;
    opts.quiet_timeout_ms = 5000;
    opts.deadline_ms = 400;
    opts.own_process_group = true;
    cbm_proc_result_t r;
    ASSERT_EQ(cbm_subprocess_run(&opts, &r), 0);
    ASSERT_EQ(r.outcome, CBM_PROC_HANG);

    FILE *pf = fopen(pidfile, "r");
    ASSERT_NOT_NULL(pf);
    int grandchild = 0;
    ASSERT_EQ(fscanf(pf, "%d", &grandchild), 1);
    (void)fclose(pf);
    ASSERT_GT(grandchild, 1);
    /* The grandchild was reparented, so it is reaped by init; poll briefly. */
    bool gone = false;
    for (int i = 0; i < 50 && !gone; i++) {
        gone = kill((pid_t)grandchild, 0) != 0;
        if (!gone) {
            usleep(20000);
        }
    }
    if (!gone) {
        (void)kill((pid_t)grandchild, SIGKILL);
    }
    (void)unlink(pidfile);
    (void)unlink(log);
    (void)rmdir(dir);
    ASSERT_TRUE(gone);
    PASS();
#endif
}

/* A child whose output grows past max_log_bytes is killed and classified
 * OUTPUT_LIMIT, not HANG or a clean exit. */
TEST(subprocess_run_output_limit) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX /bin/sh spawn");
#else
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/cbm_outlimit_XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(dir));
    char log[1100];
    snprintf(log, sizeof(log), "%s/out", dir);
    const char *argv[] = {"/bin/sh", "-c", "while :; do echo 0123456789abcdef; done", NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = "/bin/sh";
    opts.argv = argv;
    opts.log_file = log;
    opts.deadline_ms = 20000;
    opts.max_log_bytes = 4096;
    opts.own_process_group = true;
    opts.delete_log_on_exit = true;
    cbm_proc_result_t r;
    ASSERT_EQ(cbm_subprocess_run(&opts, &r), 0);
    (void)rmdir(dir);
    ASSERT_EQ(r.outcome, CBM_PROC_OUTPUT_LIMIT);
    ASSERT_STR_EQ(cbm_proc_outcome_str(r.outcome), "output_limit");
    PASS();
#endif
}

TEST(subprocess_run_null_bin_rejected) {
    cbm_proc_opts_t opts = {0};
    opts.bin = NULL;
    cbm_proc_result_t r;
    int rc = cbm_subprocess_run(&opts, &r);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(r.outcome, CBM_PROC_SPAWN_FAILED);
    PASS();
}

/* ── Layer 3: Windows command-line quoting (pure; every platform) ─────────────
 *
 * The Windows index-worker "crash" was a quoting bug: the Windows spawn wrapped each
 * argv element in bare quotes without escaping, so a JSON argument like
 * {"repo_path":"C:/r"} lost its inner quotes when the child re-parsed the command
 * line — the worker then failed at JSON-arg parse and exited non-zero, which the
 * supervisor misreported as a per-file crash. We guard cbm_build_win_cmdline by
 * ROUND-TRIP: a reference implementation of the Windows CommandLineToArgvW rules
 * (the inverse of the builder) must re-parse the emitted line back into the exact
 * original argv. Testing the invariant — not a hand-computed escaped string — keeps
 * the guard honest and readable, and runs on Linux/macOS CI (the builder is pure). */

/* Reference re-parser: the subset of CommandLineToArgvW our builder emits (every
 * arg quote-wrapped; \" for embedded quotes; backslashes doubled before a quote). */
static int parse_win_cmdline(const char *cmd, char out[][256], int max_args) {
    int argc = 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p || argc >= max_args) {
            break;
        }
        char *o = out[argc];
        size_t oi = 0;
        bool in_quotes = false;
        /* Guard every write: each out[] row is 256 bytes; test args stay well under
         * that, but cap defensively so a future longer arg fails a length assertion
         * rather than smashing the stack. */
#define PUTO(ch)            \
    do {                    \
        if (oi < 255) {     \
            o[oi++] = (ch); \
        }                   \
    } while (0)
        for (;;) {
            size_t nbs = 0;
            while (*p == '\\') {
                nbs++;
                p++;
            }
            if (*p == '"') {
                for (size_t k = 0; k < nbs / 2; k++) {
                    PUTO('\\');
                }
                if (nbs % 2) {
                    PUTO('"'); /* odd run → the quote is an escaped literal */
                } else {
                    in_quotes = !in_quotes; /* even run → the quote is a delimiter */
                }
                p++;
            } else {
                for (size_t k = 0; k < nbs; k++) {
                    PUTO('\\');
                }
                if (*p == '\0' || (!in_quotes && (*p == ' ' || *p == '\t'))) {
                    break;
                }
                PUTO(*p);
                p++;
            }
        }
#undef PUTO
        o[oi] = '\0';
        argc++;
    }
    return argc;
}

static bool cmdline_roundtrips(const char *const *argv) {
    char cmd[4096];
    if (!cbm_build_win_cmdline(cmd, sizeof(cmd), argv)) {
        return false;
    }
    char parsed[16][256];
    int pc = parse_win_cmdline(cmd, parsed, 16);
    int oc = 0;
    while (argv[oc]) {
        oc++;
    }
    if (pc != oc) {
        return false;
    }
    for (int i = 0; i < oc; i++) {
        if (strcmp(argv[i], parsed[i]) != 0) {
            return false;
        }
    }
    return true;
}

/* The exact index-worker argv: the command line with a JSON arg full of quotes.
 * Round-trips, AND the emitted line must contain an ESCAPED quote (\") — the bare
 * `"%s"` wrap that caused the bug never would. */
TEST(win_cmdline_index_worker_json) {
    const char *const argv[] = {"C:/bin/cbm.exe",           "cli",
                                "--index-worker",           "index_repository",
                                "{\"repo_path\":\"C:/r\"}", "--response-out",
                                "C:/c/w.response",          NULL};
    ASSERT(cmdline_roundtrips(argv));
    char cmd[4096];
    ASSERT(cbm_build_win_cmdline(cmd, sizeof(cmd), argv));
    ASSERT(strstr(cmd, "\\\"repo_path\\\"") != NULL); /* inner quotes are escaped */
    PASS();
}

/* A battery of adversarial argv (spaces, tabs, embedded quotes, backslash runs,
 * trailing backslashes, backslash-before-quote, real Windows paths) must all
 * round-trip byte-for-byte through the builder + reference parser. */
TEST(win_cmdline_roundtrip_battery) {
    const char *const a1[] = {"a", "b", NULL};
    const char *const a2[] = {"has space", "tab\there", NULL};
    const char *const a3[] = {"trailing\\", "a\\b\\c", NULL};
    const char *const a4[] = {"a\\\"b", "\"", "\\\\\"", NULL};
    const char *const a5[] = {"C:\\Users\\me\\my repo", "{\"name\":\"a b\",\"x\":\"y\\\\z\"}",
                              NULL};
    const char *const a6[] = {"", "plain", NULL};
    ASSERT(cmdline_roundtrips(a1));
    ASSERT(cmdline_roundtrips(a2));
    ASSERT(cmdline_roundtrips(a3));
    ASSERT(cmdline_roundtrips(a4));
    ASSERT(cmdline_roundtrips(a5));
    ASSERT(cmdline_roundtrips(a6));
    PASS();
}

/* Overflow is reported (false), never a silent truncation that would spawn a
 * corrupted command line. */
TEST(win_cmdline_overflow_rejected) {
    const char *const argv[] = {"aaaaaaaaaa", "bbbbbbbbbb", NULL};
    char tiny[8];
    ASSERT_FALSE(cbm_build_win_cmdline(tiny, sizeof(tiny), argv));
    PASS();
}

/* Reproduce-first guard for the overflow CONTRACT (subprocess.h): on overflow buf
 * must be left a valid (empty) string. RED on the pre-fix code — the overflow path
 * returned without terminating buf, so buf[0] held the first quoted byte ('"') —
 * and GREEN once the overflow path sets buf[0] = '\0'. */
TEST(win_cmdline_overflow_leaves_empty_string) {
    char buf[8];
    const char *const argv[] = {"averylongprogramname", "x", NULL};
    ASSERT_FALSE(cbm_build_win_cmdline(buf, sizeof(buf), argv)); /* overflows cap */
    ASSERT_EQ(buf[0], '\0'); /* pre-fix: buf[0] == '"' (a partial byte), not NUL */
    PASS();
}

TEST(subprocess_resolves_bare_binary_path_from_path) {
#ifdef _WIN32
    PASS();
#else
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_subproc_bin_XXXXXX");
    char *td = cbm_mkdtemp(tmpdir);
    ASSERT_NOT_NULL(td);

    char exe[512];
    snprintf(exe, sizeof(exe), "%s/codebase-memory-mcp", td);
    FILE *f = fopen(exe, "w");
    ASSERT_NOT_NULL(f);
    fputs("#!/bin/sh\nexit 0\n", f);
    fclose(f);
    ASSERT_EQ(chmod(exe, 0755), 0);

    char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
    cbm_setenv("PATH", td, 1);

    char resolved[1024];
    ASSERT_TRUE(
        cbm_resolve_binary_path("codebase-memory-mcp", resolved, sizeof(resolved)));
    ASSERT_STR_EQ(resolved, exe);

    if (old_path) {
        cbm_setenv("PATH", old_path, 1);
        free(old_path);
    } else {
        cbm_unsetenv("PATH");
    }
    PASS();
#endif
}

#if defined(__linux__) || defined(__APPLE__)
static int copy_exec_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    fclose(in);
    if (fclose(out) != 0) {
        rc = -1;
    }
    return rc == 0 ? chmod(dst, 0755) : rc;
}
#endif

/* #1204 (upstream 11bbf0d8, 5b96b067): after an installer replaces the running
 * binary, the self-resolved path is dead. Linux must hand back the
 * /proc/self/exe magic link; macOS must refuse. The child side is
 * tf_maybe_run_deleted_self_probe in test_main.c. */
TEST(subprocess_deleted_self_spawn_path_follows_platform_ruling) {
#if defined(__linux__) || defined(__APPLE__)
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "%s/cbm_deleted_self_XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(tmpdir));

    char current[1024];
#if defined(__linux__)
    ssize_t current_len = readlink("/proc/self/exe", current, sizeof(current) - 1);
    ASSERT_GT(current_len, 0);
    current[current_len] = '\0';
#else
    uint32_t current_sz = sizeof(current);
    ASSERT_EQ(_NSGetExecutablePath(current, &current_sz), 0);
#endif

    char launch_path[512];
    char replacement_path[512];
    snprintf(launch_path, sizeof(launch_path), "%s/test-runner", tmpdir);
    snprintf(replacement_path, sizeof(replacement_path), "%s/test-runner.new", tmpdir);
    ASSERT_EQ(copy_exec_file(current, launch_path), 0);

    int ready_pipe[2];
    int continue_pipe[2];
    ASSERT_EQ(pipe(ready_pipe), 0);
    ASSERT_EQ(pipe(continue_pipe), 0);

    pid_t child = fork();
    if (child == 0) {
        close(ready_pipe[0]);
        close(continue_pipe[1]);
        char ready_fd[32];
        char continue_fd[32];
        snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
        snprintf(continue_fd, sizeof(continue_fd), "%d", continue_pipe[0]);
        execl(launch_path, launch_path, "__cbm_deleted_self_probe", ready_fd, continue_fd,
              (char *)NULL);
        _exit(127);
    }
    ASSERT_GT(child, 0);

    close(ready_pipe[1]);
    close(continue_pipe[0]);
    char ready = '\0';
    ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
    ASSERT_EQ(ready, 'R');

#if defined(__linux__)
    /* Atomic rename-over, the installer's real move. */
    ASSERT_EQ(copy_exec_file(current, replacement_path), 0);
    ASSERT_EQ(rename(replacement_path, launch_path), 0);
#else
    /* A rename-over on macOS leaves a valid executable that resolve may return;
     * the fail-closed branch guards the image-gone case. */
    ASSERT_EQ(unlink(launch_path), 0);
#endif
    ASSERT_EQ(write(continue_pipe[1], "G", 1), 1);
    close(ready_pipe[0]);
    close(continue_pipe[1]);

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    unlink(launch_path);
    rmdir(tmpdir);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    PASS();
#else
    PASS();
#endif
}

/* ── Git child environment (#56) ──────────────────────────────────────────────
 * cbm_popen_git drops every variable of `git rev-parse --local-env-vars` from
 * the child only: the parent keeps its environment, other variables pass
 * through, and cbm_pclose reports the raw wait status like pclose(). The test
 * sets the variables itself (the runner clears them at startup) and restores
 * them before asserting. */
#ifndef _WIN32
typedef struct {
    char *saved[CBM_GIT_REPO_ENV_VAR_COUNT];
    bool present[CBM_GIT_REPO_ENV_VAR_COUNT];
} gitenv_snapshot_t;

static void gitenv_enter(gitenv_snapshot_t *snap) {
    for (int i = 0; i < CBM_GIT_REPO_ENV_VAR_COUNT; i++) {
        const char *v = getenv(cbm_git_repo_env_vars[i]);
        snap->present[i] = v != NULL;
        snap->saved[i] = v ? strdup(v) : NULL;
        setenv(cbm_git_repo_env_vars[i], "/cbm-decoy-repo", 1);
    }
    setenv("CBM_GITENV_KEEP", "kept", 1);
}

static void gitenv_leave(gitenv_snapshot_t *snap) {
    for (int i = 0; i < CBM_GIT_REPO_ENV_VAR_COUNT; i++) {
        if (snap->present[i]) {
            setenv(cbm_git_repo_env_vars[i], snap->saved[i], 1);
        } else {
            unsetenv(cbm_git_repo_env_vars[i]);
        }
        free(snap->saved[i]);
    }
    unsetenv("CBM_GITENV_KEEP");
}

/* exit 0: no repository-local git variable is set and CBM_GITENV_KEEP=kept;
 * exit 3: a git variable leaked; exit 4: the unrelated variable was lost. */
static void gitenv_probe_script(char *buf, size_t cap) {
    size_t n = (size_t)snprintf(buf, cap, "[ \"$CBM_GITENV_KEEP\" = kept ] || exit 4;");
    for (int i = 0; i < CBM_GIT_REPO_ENV_VAR_COUNT && n < cap; i++) {
        n += (size_t)snprintf(buf + n, cap - n, " [ -z \"${%s+x}\" ] || exit 3;",
                              cbm_git_repo_env_vars[i]);
    }
    if (n < cap) {
        (void)snprintf(buf + n, cap - n, " exit 0");
    }
}
#endif

TEST(popen_git_strips_repo_env_and_reports_exit_status) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX /bin/sh spawn");
#else
    char script[2048];
    gitenv_probe_script(script, sizeof(script));
    char cmd[2200];
    snprintf(cmd, sizeof(cmd), "echo probe; %s", script);
    gitenv_snapshot_t snap;
    gitenv_enter(&snap);
    FILE *fp = cbm_popen_git(cmd);
    char line[64] = {0};
    bool got_line = fp && fgets(line, sizeof(line), fp) != NULL;
    int status = fp ? cbm_pclose(fp) : -1;
    FILE *inherited = cbm_popen(script, "r");
    int inherited_status = inherited ? cbm_pclose(inherited) : -1;
    FILE *fail = cbm_popen_git("exit 7");
    int fail_status = fail ? cbm_pclose(fail) : -1;
    const char *parent_git_dir = getenv("GIT_DIR");
    bool parent_kept = parent_git_dir && strcmp(parent_git_dir, "/cbm-decoy-repo") == 0;
    gitenv_leave(&snap);
    ASSERT_TRUE(got_line);
    ASSERT_STR_EQ(line, "probe\n");
    ASSERT_TRUE(status >= 0 && WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0); /* git vars gone, CBM_GITENV_KEEP passed */
    ASSERT_TRUE(inherited_status >= 0 && WIFEXITED(inherited_status));
    ASSERT_EQ(WEXITSTATUS(inherited_status), 3); /* cbm_popen still inherits */
    ASSERT_TRUE(fail_status >= 0 && WIFEXITED(fail_status));
    ASSERT_EQ(WEXITSTATUS(fail_status), 7); /* pclose() semantics: raw wait status */
    ASSERT_TRUE(parent_kept);               /* the parent environment is unchanged */
    PASS();
#endif
}

/* POSIX: Child processes must NOT inherit the server's open lock file descriptors (#77).
 * Tests that both cbm_subprocess_run (via cbm_run_posix) and cbm_popen spawn children
 * without leaking open lock file descriptors. */
TEST(posix_child_does_not_inherit_lock_fd) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX close-on-exec descriptor inheritance");
#else
    char lock_path[512];
    snprintf(lock_path, sizeof(lock_path), "%s/cbm_test_lock_fd_%d.lock", cbm_tmpdir(), (int)getpid());

    cbm_file_lock_t lock;
    int rc = cbm_file_lock_acquire(lock_path, CBM_FILE_LOCK_EXCLUSIVE, false, &lock);
    int lock_fd = (int)lock.handle;

    int run_outcome = -1;
    int run_exit_code = -1;
    int popen_status = -1;
    bool popen_exited = false;

    if (rc == 0) {
        char script[512];
        snprintf(script, sizeof(script),
                 "if [ -e /proc/self/fd/%d ] || [ -e /dev/fd/%d ] || (: <&%d) 2>/dev/null; then exit 42; fi; exit 0",
                 lock_fd, lock_fd, lock_fd);

        /* 1. Test cbm_subprocess_run */
        const char *argv[] = {"/bin/sh", "-c", script, NULL};
        cbm_proc_opts_t opts = {0};
        opts.bin = "/bin/sh";
        opts.argv = argv;
        cbm_proc_result_t r;
        cbm_subprocess_run(&opts, &r);
        run_outcome = r.outcome;
        run_exit_code = r.exit_code;

        /* 2. Test cbm_popen */
        FILE *fp = cbm_popen(script, "r");
        if (fp != NULL) {
            popen_status = cbm_pclose(fp);
            popen_exited = WIFEXITED(popen_status);
            popen_status = WEXITSTATUS(popen_status);
        }

        cbm_file_lock_release(&lock);
    }
    (void)unlink(lock_path);

    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(lock_fd > STDERR_FILENO);
    ASSERT_EQ(run_outcome, CBM_PROC_CLEAN);
    ASSERT_EQ(run_exit_code, 0);
    ASSERT_TRUE(popen_exited);
    ASSERT_EQ(popen_status, 0);
    PASS();
#endif
}

/* POSIX: The spawn backstops (cbm_run_posix and cbm_exec_no_shell) must close
 * descriptors that were opened WITHOUT close-on-exec (#77). */
TEST(posix_child_backstop_closes_raw_descriptors) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX descriptor inheritance backstop");
#else
    /* Open a plain descriptor WITHOUT close-on-exec (O_CLOEXEC) */
    int raw_fd = open("/dev/null", O_RDONLY);
    ASSERT_TRUE(raw_fd >= 0);
    ASSERT_TRUE(raw_fd > STDERR_FILENO);

    char script[512];
    snprintf(script, sizeof(script),
             "if [ -e /proc/self/fd/%d ] || [ -e /dev/fd/%d ] || (: <&%d) 2>/dev/null; then exit 42; fi; exit 0",
             raw_fd, raw_fd, raw_fd);

    /* 1. Test cbm_subprocess_run (covers cbm_run_posix: posix_spawn on macOS, fork+exec on Linux) */
    const char *subproc_argv[] = {"/bin/sh", "-c", script, NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = "/bin/sh";
    opts.argv = subproc_argv;
    cbm_proc_result_t r;
    cbm_subprocess_run(&opts, &r);

    /* 2. Test cbm_exec_no_shell (covers posix_spawnp on macOS, fork+exec on Linux) */
    const char *exec_argv[] = {"/bin/sh", "-c", script, NULL};
    int exec_rc = cbm_exec_no_shell(exec_argv);

    (void)close(raw_fd);

    ASSERT_EQ(r.outcome, CBM_PROC_CLEAN);
    ASSERT_EQ(r.exit_code, 0);
    ASSERT_EQ(exec_rc, 0);
    PASS();
#endif
}

SUITE(subprocess) {
    RUN_TEST(subprocess_classify_clean);
    RUN_TEST(subprocess_classify_exit_nonzero);
    RUN_TEST(subprocess_classify_windows_crash_codes);
    RUN_TEST(subprocess_classify_posix_fault_signal_is_crash);
    RUN_TEST(subprocess_classify_non_fault_signal_is_killed);
    RUN_TEST(subprocess_classify_timeout_dominates);
    RUN_TEST(subprocess_outcome_str);
    RUN_TEST(subprocess_run_clean);
    RUN_TEST(subprocess_run_exit_nonzero);
    RUN_TEST(subprocess_run_crash_is_crash);
    RUN_TEST(subprocess_run_hang_is_hang);
    RUN_TEST(subprocess_run_spawn_failure);
    RUN_TEST(subprocess_run_null_bin_rejected);
    RUN_TEST(subprocess_run_deadline_kills_process_group);
    RUN_TEST(subprocess_run_output_limit);
    RUN_TEST(win_cmdline_index_worker_json);
    RUN_TEST(win_cmdline_roundtrip_battery);
    RUN_TEST(win_cmdline_overflow_rejected);
    RUN_TEST(win_cmdline_overflow_leaves_empty_string);
    RUN_TEST(subprocess_resolves_bare_binary_path_from_path);
    RUN_TEST(subprocess_deleted_self_spawn_path_follows_platform_ruling);
    RUN_TEST(popen_git_strips_repo_env_and_reports_exit_status);
    RUN_TEST(posix_child_does_not_inherit_lock_fd);
    RUN_TEST(posix_child_backstop_closes_raw_descriptors);
}
