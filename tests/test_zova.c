#include "test_framework.h"

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/compat_thread.h"
#include "../src/foundation/platform.h"
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_edge_payload.h"
#include "zova/cbm_zova_publish_model.h"
#include "zova/cbm_zova_delta.h"
#include "zova/cbm_zova_route.h"
#include "zova/cbm_zova_repository.h"
#include "cypher/cypher.h"
#include "zova/cbm_zova_writer_gate.h"
#include "zova/cbm_zova_coordination.h"
#include "foundation/file_lock.h"
#include "graph_buffer/graph_buffer.h"
#include "../internal/cbm/dump_types.h"

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if CBM_WITH_ZOVA
#include "zova.h"
#endif

#include <sqlite3.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#endif

enum { TZ_VEC_DIM = 768, TZ_PATH_MAX = 512 };

typedef struct {
    cbm_zova_edge_payload_record_t records[4];
    size_t count;
} zova_payload_capture_t;

static int zova_capture_payload_record(
    const cbm_zova_edge_payload_record_t *record, void *context) {
    zova_payload_capture_t *capture = context;
    if (!capture || !record || capture->count >= 4) return -1;
    capture->records[capture->count++] = *record;
    return 0;
}

static int zova_payload_slice_equals(cbm_zova_edge_payload_slice_t slice,
                                     const char *expected) {
    size_t expected_len = strlen(expected);
    return slice.len == expected_len &&
           (expected_len == 0 || memcmp(slice.data, expected, expected_len) == 0);
}

TEST(zova_edge_payload_codec_is_compact_deterministic_and_strict) {
    const CBMDumpEdge default_edge = {
        .properties = "{}", .url_path = "", .local_name = "",
    };
    const CBMDumpEdge *default_edges[] = {&default_edge};
    size_t payload_size = SIZE_MAX;
    ASSERT_EQ(cbm_zova_edge_payload_encoded_size(default_edges, 1, &payload_size), 0);
    ASSERT_EQ(payload_size, 0);
    size_t payload_len = SIZE_MAX;
    ASSERT_EQ(cbm_zova_edge_payload_encode(default_edges, 1, NULL, 0, &payload_len), 0);
    ASSERT_EQ(payload_len, 0);

    zova_payload_capture_t capture = {0};
    size_t decoded_count = 0;
    ASSERT_EQ(cbm_zova_edge_payload_visit(NULL, 0, zova_capture_payload_record,
                                          &capture, &decoded_count), 0);
    ASSERT_EQ(decoded_count, 1);
    ASSERT_EQ(capture.count, 1);
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].properties, "{}"));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].url_path, ""));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].local_name, ""));

    const CBMDumpEdge first = {
        .properties = "{\"slot\":1}", .url_path = "", .local_name = "alpha",
    };
    const CBMDumpEdge second = {
        .properties = "{}", .url_path = "/docs", .local_name = "beta",
    };
    const CBMDumpEdge *logical_edges[] = {&first, &second};
    ASSERT_EQ(cbm_zova_edge_payload_encoded_size(logical_edges, 2, &payload_size), 0);
    ASSERT_TRUE(payload_size > 0 && payload_size < 64);
    uint8_t payload[64] = {0};
    ASSERT_EQ(cbm_zova_edge_payload_encode(logical_edges, 2, payload, sizeof(payload),
                                           &payload_len), 0);
    ASSERT_EQ(payload_len, payload_size);
    uint8_t second_encoding[64] = {0};
    size_t second_len = 0;
    ASSERT_EQ(cbm_zova_edge_payload_encode(logical_edges, 2, second_encoding,
                                           sizeof(second_encoding), &second_len), 0);
    ASSERT_EQ(second_len, payload_len);
    ASSERT_EQ(memcmp(payload, second_encoding, payload_len), 0);

    memset(&capture, 0, sizeof(capture));
    ASSERT_EQ(cbm_zova_edge_payload_visit(payload, payload_len,
                                          zova_capture_payload_record, &capture,
                                          &decoded_count), 0);
    ASSERT_EQ(decoded_count, 2);
    ASSERT_EQ(capture.count, 2);
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].properties,
                                          "{\"slot\":1}"));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].url_path, ""));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[0].local_name, "alpha"));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[1].properties, "{}"));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[1].url_path, "/docs"));
    ASSERT_TRUE(zova_payload_slice_equals(capture.records[1].local_name, "beta"));

    const uint8_t invalid_flags[] = {1, 1, 0x80};
    ASSERT_TRUE(cbm_zova_edge_payload_visit(invalid_flags, sizeof(invalid_flags),
                                            NULL, NULL, NULL) != 0);
    PASS();
}

#ifndef _WIN32
TEST(zova_writer_gate_serializes_processes_and_releases_after_crash) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-zova-writer-gate-%d.zova", cbm_tmpdir(), getpid());
    int ready[2], release[2], acquired[2];
    ASSERT_EQ(pipe(ready), 0);
    ASSERT_EQ(pipe(release), 0);
    ASSERT_EQ(pipe(acquired), 0);

    pid_t holder = fork();
    ASSERT_TRUE(holder >= 0);
    if (holder == 0) {
        close(ready[0]); close(release[1]); close(acquired[0]); close(acquired[1]);
        cbm_zova_writer_guard_t guard = {0};
        if (cbm_zova_writer_guard_acquire(path, &guard) != 0) _exit(10);
        (void)write(ready[1], "R", 1);
        char byte;
        if (read(release[0], &byte, 1) != 1) _exit(11);
        cbm_zova_writer_guard_release(&guard);
        _exit(0);
    }
    close(ready[1]); close(release[0]);
    char byte;
    ASSERT_EQ(read(ready[0], &byte, 1), 1);

    pid_t waiter = fork();
    ASSERT_TRUE(waiter >= 0);
    if (waiter == 0) {
        close(acquired[0]); close(release[1]); close(ready[0]);
        cbm_zova_writer_guard_t guard = {0};
        if (cbm_zova_writer_guard_acquire(path, &guard) != 0) _exit(12);
        (void)write(acquired[1], "A", 1);
        cbm_zova_writer_guard_release(&guard);
        _exit(0);
    }
    close(acquired[1]);
    int flags = fcntl(acquired[0], F_GETFL, 0);
    ASSERT_TRUE(flags >= 0);
    ASSERT_EQ(fcntl(acquired[0], F_SETFL, flags | O_NONBLOCK), 0);
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
    nanosleep(&delay, NULL);
    ASSERT_EQ(read(acquired[0], &byte, 1), -1);
    ASSERT_EQ(errno, EAGAIN);
    ASSERT_EQ(write(release[1], "X", 1), 1);
    ASSERT_EQ(fcntl(acquired[0], F_SETFL, flags), 0);
    ASSERT_EQ(read(acquired[0], &byte, 1), 1);
    int status = 0;
    ASSERT_EQ(waitpid(holder, &status, 0), holder);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ASSERT_EQ(waitpid(waiter, &status, 0), waiter);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    int crash_ready[2];
    ASSERT_EQ(pipe(crash_ready), 0);
    pid_t crashed = fork();
    ASSERT_TRUE(crashed >= 0);
    if (crashed == 0) {
        close(crash_ready[0]);
        cbm_zova_writer_guard_t guard = {0};
        if (cbm_zova_writer_guard_acquire(path, &guard) != 0) _exit(13);
        (void)write(crash_ready[1], "C", 1);
        for (;;) pause();
    }
    close(crash_ready[1]);
    ASSERT_EQ(read(crash_ready[0], &byte, 1), 1);
    ASSERT_EQ(kill(crashed, SIGKILL), 0);
    ASSERT_EQ(waitpid(crashed, &status, 0), crashed);
    cbm_zova_writer_guard_t after_crash = {0};
    ASSERT_EQ(cbm_zova_writer_guard_acquire(path, &after_crash), 0);
    cbm_zova_writer_guard_release(&after_crash);
    char lock_path[TZ_PATH_MAX + 32];
    snprintf(lock_path, sizeof(lock_path), "%s.writer.lock", path);
    cbm_unlink(lock_path);
    PASS();
}
#endif

#ifndef _WIN32
/* Child holds `path` in `mode` until the parent writes to *release_fd (or kills
 * it). Returns the child pid, or -1 when the child could not take the lock. */
static pid_t tz_lock_holder(const char *path, cbm_file_lock_mode_t mode, int *release_fd) {
    int ready[2], release[2];
    if (pipe(ready) != 0 || pipe(release) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(ready[0]); close(release[1]);
        cbm_file_lock_t lock;
        if (cbm_file_lock_acquire(path, mode, false, &lock) != 0) {
            (void)write(ready[1], "F", 1);
            _exit(10);
        }
        (void)write(ready[1], "R", 1);
        char byte;
        (void)read(release[0], &byte, 1);
        cbm_file_lock_release(&lock);
        _exit(0);
    }
    close(ready[1]); close(release[0]);
    char byte = 0;
    ssize_t n = read(ready[0], &byte, 1);
    close(ready[0]);
    *release_fd = release[1];
    if (n != 1 || byte != 'R') {
        close(release[1]);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    return pid;
}

static bool tz_lock_holder_finish(pid_t pid, int release_fd) {
    (void)write(release_fd, "X", 1);
    close(release_fd);
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

TEST(file_lock_modes_conflict_across_processes_and_release_on_crash) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-file-lock-%d.lock", cbm_tmpdir(), getpid());
    cbm_file_lock_t lock;
    int release_fd = -1;

    /* shared + shared is allowed; shared blocks exclusive */
    pid_t holder = tz_lock_holder(path, CBM_FILE_LOCK_SHARED, &release_fd);
    ASSERT_TRUE(holder > 0);
    ASSERT_EQ(cbm_file_lock_acquire(path, CBM_FILE_LOCK_SHARED, false, &lock), 0);
    cbm_file_lock_release(&lock);
    ASSERT_EQ(lock.handle, -1);
    ASSERT_EQ(cbm_file_lock_acquire(path, CBM_FILE_LOCK_EXCLUSIVE, false, &lock),
              CBM_FILE_LOCK_BUSY);
    ASSERT_EQ(lock.handle, -1);
    ASSERT_TRUE(tz_lock_holder_finish(holder, release_fd));

    /* exclusive blocks shared, and the blocking acquire returns once released */
    holder = tz_lock_holder(path, CBM_FILE_LOCK_EXCLUSIVE, &release_fd);
    ASSERT_TRUE(holder > 0);
    ASSERT_EQ(cbm_file_lock_acquire(path, CBM_FILE_LOCK_SHARED, false, &lock),
              CBM_FILE_LOCK_BUSY);
    ASSERT_TRUE(tz_lock_holder_finish(holder, release_fd));
    ASSERT_EQ(cbm_file_lock_acquire(path, CBM_FILE_LOCK_EXCLUSIVE, true, &lock), 0);
    cbm_file_lock_release(&lock);
    cbm_file_lock_release(&lock); /* idempotent */

    /* a killed holder never leaves the lock stuck */
    holder = tz_lock_holder(path, CBM_FILE_LOCK_EXCLUSIVE, &release_fd);
    ASSERT_TRUE(holder > 0);
    ASSERT_EQ(kill(holder, SIGKILL), 0);
    ASSERT_EQ(waitpid(holder, NULL, 0), holder);
    close(release_fd);
    ASSERT_EQ(cbm_file_lock_acquire(path, CBM_FILE_LOCK_EXCLUSIVE, false, &lock), 0);
    cbm_file_lock_release(&lock);

    ASSERT_EQ(cbm_file_lock_acquire("", CBM_FILE_LOCK_SHARED, false, &lock), -1);
    cbm_unlink(path);
    PASS();
}

static int64_t tz_now_ms(void) {
    struct timespec ts;
    cbm_clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

TEST(zova_coordination_lease_and_barrier_in_process) {
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    char base[TZ_PATH_MAX], cache[TZ_PATH_MAX + 32], zova_path[TZ_PATH_MAX + 64];
    snprintf(base, sizeof(base), "%s/cbm-coord-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(base));
    /* The cache directory does not exist yet: the first hold creates it. */
    snprintf(cache, sizeof(cache), "%s/cache/nested", base);
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    ASSERT_EQ(cbm_zova_user_database_path(zova_path, sizeof(zova_path)), 0);

    cbm_zova_index_hold_t a, again, b;
    char workspace_id[64] = {0};
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-root-a", 0, &a, workspace_id,
                                          sizeof(workspace_id)),
              CBM_ZOVA_COORD_OK);
    ASSERT_TRUE(strncmp(workspace_id, "w1_", 3) == 0);
    ASSERT_TRUE(a.lease.held && a.barrier.held);
    struct stat st;
    ASSERT_EQ(stat(a.lease.path, &st), 0);
    ASSERT_EQ(stat(a.barrier.path, &st), 0);

    /* Same workspace from the same process: the lease excludes it (threads of
     * one process share the process's file lock, so this is the in-process
     * accounting at work). The failed attempt leaves nothing extra held. */
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-root-a", 0, &again, NULL, 0),
              CBM_ZOVA_COORD_BUSY);
    ASSERT_FALSE(again.barrier_busy);
    ASSERT_FALSE(again.lease.held || again.barrier.held);
    cbm_zova_coord_hold_t lease;
    ASSERT_EQ(cbm_zova_workspace_lease_acquire(zova_path, workspace_id, 0, &lease),
              CBM_ZOVA_COORD_BUSY);

    /* Bounded wait gives up at the deadline. */
    int64_t started = tz_now_ms();
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-root-a", 150, &again, NULL, 0),
              CBM_ZOVA_COORD_BUSY);
    ASSERT_TRUE(tz_now_ms() - started >= 140);

    /* Another workspace runs alongside: both share the barrier. */
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-root-b", 0, &b, NULL, 0),
              CBM_ZOVA_COORD_OK);
    cbm_zova_coord_hold_t barrier;
    ASSERT_EQ(cbm_zova_prepare_barrier_acquire(zova_path, true, 0, &barrier),
              CBM_ZOVA_COORD_BUSY);
    cbm_zova_index_hold_release(&a);
    cbm_zova_index_hold_release(&a); /* idempotent */
    ASSERT_EQ(cbm_zova_prepare_barrier_acquire(zova_path, true, 0, &barrier),
              CBM_ZOVA_COORD_BUSY); /* b still holds it shared */
    cbm_zova_index_hold_release(&b);

    /* A database replacement holds the barrier exclusive: index runs refuse. */
    ASSERT_EQ(cbm_zova_prepare_barrier_acquire(zova_path, true, 0, &barrier), CBM_ZOVA_COORD_OK);
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-root-a", 0, &again, NULL, 0),
              CBM_ZOVA_COORD_BUSY);
    ASSERT_TRUE(again.barrier_busy);
    cbm_zova_coord_release(&barrier);

    /* Shutdown cancels waits. */
    cbm_zova_coord_set_cancelled(true);
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-root-a", 1000, &again, NULL, 0),
              CBM_ZOVA_COORD_CANCELLED);
    cbm_zova_coord_set_cancelled(false);

    ASSERT_EQ(cbm_zova_workspace_lease_acquire(zova_path, "not a workspace id", 0, &lease),
              CBM_ZOVA_COORD_ERROR);
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-root-a", 0, &a, NULL, 0), CBM_ZOVA_COORD_OK);
    cbm_unlink(a.lease.path);
    cbm_unlink(a.barrier.path);
    char turnstile[TZ_PATH_MAX + 96];
    snprintf(turnstile, sizeof(turnstile), "%s.prepare.turnstile.lock", zova_path);
    cbm_unlink(turnstile);
    cbm_zova_index_hold_release(&a);
    char lease_b[TZ_PATH_MAX + 128], wid_b[64];
    ASSERT_EQ(cbm_zova_workspace_id_for_root("/tmp/coord-root-b", wid_b, sizeof(wid_b)), 0);
    snprintf(lease_b, sizeof(lease_b), "%s.ws-%s.lock", zova_path, wid_b);
    cbm_unlink(lease_b);
    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    char dir[TZ_PATH_MAX + 32];
    rmdir(cache);
    snprintf(dir, sizeof(dir), "%s/cache", base);
    rmdir(dir);
    rmdir(base);
    PASS();
}

typedef struct {
    const char *zova_path;
    cbm_zova_coord_status_t status;
    cbm_zova_coord_hold_t hold;
} tz_barrier_writer_t;

static void *tz_barrier_writer(void *arg) {
    tz_barrier_writer_t *writer = arg;
    writer->status = cbm_zova_prepare_barrier_acquire(writer->zova_path, true, 10000,
                                                      &writer->hold);
    return NULL;
}

/* A waiting restore/repack is not starved by overlapping index runs: once it
 * waits, new runs refuse until it has replaced the file. */
TEST(zova_coordination_waiting_writer_blocks_new_index_runs) {
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    char cache[TZ_PATH_MAX], zova_path[TZ_PATH_MAX + 32];
    snprintf(cache, sizeof(cache), "%s/cbm-coord-writer-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    ASSERT_EQ(cbm_zova_user_database_path(zova_path, sizeof(zova_path)), 0);

    cbm_zova_index_hold_t running, late;
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-writer-a", 0, &running, NULL, 0),
              CBM_ZOVA_COORD_OK);
    tz_barrier_writer_t writer = {.zova_path = zova_path, .status = CBM_ZOVA_COORD_ERROR};
    cbm_thread_t thread;
    ASSERT_EQ(cbm_thread_create(&thread, 0, tz_barrier_writer, &writer), 0);

    /* Until the writer reaches the turnstile a new run may still start. */
    bool writer_waiting = false;
    for (int i = 0; i < 400 && !writer_waiting; i++) {
        cbm_zova_coord_status_t status =
            cbm_zova_index_hold_acquire("/tmp/coord-writer-b", 0, &late, NULL, 0);
        if (status == CBM_ZOVA_COORD_OK) {
            cbm_zova_index_hold_release(&late);
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 5000000};
            nanosleep(&pause, NULL);
        } else {
            writer_waiting = status == CBM_ZOVA_COORD_BUSY && late.barrier_busy;
        }
    }
    ASSERT_TRUE(writer_waiting);

    cbm_zova_index_hold_release(&running);
    cbm_thread_join(&thread);
    ASSERT_EQ(writer.status, CBM_ZOVA_COORD_OK);
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-writer-b", 0, &late, NULL, 0),
              CBM_ZOVA_COORD_BUSY);
    ASSERT_TRUE(late.barrier_busy);
    cbm_zova_coord_release(&writer.hold);
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-writer-b", 0, &late, NULL, 0),
              CBM_ZOVA_COORD_OK);
    cbm_zova_index_hold_release(&late);

    const char *roots[] = {"/tmp/coord-writer-a", "/tmp/coord-writer-b"};
    char file[TZ_PATH_MAX + 128], wid[64];
    for (int i = 0; i < 2; i++) {
        ASSERT_EQ(cbm_zova_workspace_id_for_root(roots[i], wid, sizeof(wid)), 0);
        snprintf(file, sizeof(file), "%s.ws-%s.lock", zova_path, wid);
        cbm_unlink(file);
    }
    snprintf(file, sizeof(file), "%s.prepare.lock", zova_path);
    cbm_unlink(file);
    snprintf(file, sizeof(file), "%s.prepare.turnstile.lock", zova_path);
    cbm_unlink(file);
    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    rmdir(cache);
    PASS();
}

TEST(zova_coordination_lease_excludes_other_processes_and_survives_crash) {
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    char cache[TZ_PATH_MAX];
    snprintf(cache, sizeof(cache), "%s/cbm-coord-proc-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    int ready[2];
    ASSERT_EQ(pipe(ready), 0);
    pid_t holder = fork();
    ASSERT_TRUE(holder >= 0);
    if (holder == 0) {
        close(ready[0]);
        cbm_zova_index_hold_t hold;
        if (cbm_zova_index_hold_acquire("/tmp/coord-proc-root", 0, &hold, NULL, 0) !=
            CBM_ZOVA_COORD_OK)
            _exit(10);
        (void)write(ready[1], "R", 1);
        for (;;) pause();
    }
    close(ready[1]);
    char byte = 0;
    ASSERT_EQ(read(ready[0], &byte, 1), 1);
    close(ready[0]);

    cbm_zova_index_hold_t hold, other;
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-proc-root", 0, &hold, NULL, 0),
              CBM_ZOVA_COORD_BUSY);
    ASSERT_FALSE(hold.barrier_busy);
    /* Another workspace is unaffected; a replacement must wait for the holder. */
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-proc-other", 0, &other, NULL, 0),
              CBM_ZOVA_COORD_OK);
    cbm_zova_index_hold_release(&other);
    char zova_path[TZ_PATH_MAX + 32];
    ASSERT_EQ(cbm_zova_user_database_path(zova_path, sizeof(zova_path)), 0);
    cbm_zova_coord_hold_t barrier;
    ASSERT_EQ(cbm_zova_prepare_barrier_acquire(zova_path, true, 0, &barrier),
              CBM_ZOVA_COORD_BUSY);

    ASSERT_EQ(kill(holder, SIGKILL), 0);
    ASSERT_EQ(waitpid(holder, NULL, 0), holder);
    ASSERT_EQ(cbm_zova_index_hold_acquire("/tmp/coord-proc-root", 0, &hold, NULL, 0),
              CBM_ZOVA_COORD_OK);
    cbm_unlink(hold.lease.path);
    cbm_zova_index_hold_release(&hold);
    ASSERT_EQ(cbm_zova_prepare_barrier_acquire(zova_path, true, 0, &barrier), CBM_ZOVA_COORD_OK);
    cbm_unlink(barrier.path);
    char turnstile[TZ_PATH_MAX + 96];
    snprintf(turnstile, sizeof(turnstile), "%s.prepare.turnstile.lock", zova_path);
    cbm_unlink(turnstile);
    cbm_zova_coord_release(&barrier);
    char wid[64], lease_other[TZ_PATH_MAX + 128];
    ASSERT_EQ(cbm_zova_workspace_id_for_root("/tmp/coord-proc-other", wid, sizeof(wid)), 0);
    snprintf(lease_other, sizeof(lease_other), "%s.ws-%s.lock", zova_path, wid);
    cbm_unlink(lease_other);
    if (saved_cache_copy) cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
    else cbm_unsetenv("CBM_CACHE_DIR");
    free(saved_cache_copy);
    rmdir(cache);
    PASS();
}
#endif

TEST(zova_route_uses_user_cache_database) {
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", "/tmp/cbm-route-cache", 1);
    char path[1024];
    ASSERT_EQ(cbm_zova_user_database_path(path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cbm-route-cache/cbm.zova");
    if (saved_cache_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
        free(saved_cache_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    PASS();
}

#if CBM_WITH_ZOVA
TEST(zova_workspace_id_validation_rejects_missing_and_malformed_values) {
    ASSERT_EQ(cbm_zova_workspace_id_validate(NULL), CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_workspace_id_validate(""), CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_workspace_id_validate("workspace-with-dash"), CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_workspace_id_validate("workspace/path"), CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_workspace_id_validate("workspace_A1"), CBM_STORE_OK);
    PASS();
}

TEST(zova_repository_requires_ready_workspace_and_reads_metadata) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-zova-repository-%d.zova", cbm_tmpdir(), getpid());
    cbm_unlink(path);
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "fixture", .label = "Function", .name = "alpha",
         .qualified_name = "fixture.alpha", .file_path = "src/alpha.c", .start_line = 2,
         .end_line = 4, .properties = "{\"visibility\":\"public\"}"},
        {.id = 2, .project = "fixture", .label = "Function", .name = "beta",
         .qualified_name = "fixture.beta", .file_path = "src/beta.c", .start_line = 5,
         .end_line = 8, .properties = "{}"},
    };
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/fixture", "fixture", 1,
                                                       nodes, 2, NULL, 0), 0);
    cbm_zova_repository_t *repo = cbm_zova_repository_open(path, "fixture");
    ASSERT_NOT_NULL(repo);
    const char *workspace_id = cbm_zova_repository_workspace_id(repo);
    ASSERT_TRUE(workspace_id[0] != '\0');
    cbm_node_t node = {0};
    ASSERT_EQ(cbm_zova_repository_find_node_by_qn(repo, workspace_id, "fixture.alpha", &node),
              CBM_STORE_OK);
    ASSERT_STR_EQ(node.file_path, "src/alpha.c");
    ASSERT_STR_EQ(node.properties_json, "{\"visibility\":\"public\"}");
    cbm_node_free_fields(&node);
    cbm_search_output_t out = {0};
    cbm_search_params_t params = {.project = "fixture", .name_pattern = "alpha", .limit = 20,
                                  .min_degree = -1, .max_degree = -1};
    ASSERT_EQ(cbm_zova_repository_search(repo, workspace_id, &params, &out), CBM_STORE_OK);
    ASSERT_EQ(out.count, 1);
    cbm_store_search_free(&out);
    ASSERT_EQ(cbm_zova_repository_find_node_by_qn(repo, NULL, "fixture.alpha", &node),
              CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_repository_find_node_by_qn(repo, "workspace_B", "fixture.alpha", &node),
              CBM_STORE_ERR);
    ASSERT_EQ(cbm_zova_repository_search(repo, "workspace_B", &params, &out), CBM_STORE_ERR);
    cbm_zova_repository_close(repo);
    ASSERT_NULL(cbm_zova_repository_open(path, "missing"));
    cbm_unlink(path);
    PASS();
}
#endif

static int zova_scalar_int64(const char *path, const char *sql, int64_t *out) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return -1;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    int rc = sqlite3_step(stmt) == SQLITE_ROW ? 0 : -1;
    if (rc == 0) {
        *out = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

static int zova_generation_state(const char *path, const char *workspace_id, int64_t generation,
                                 char *out, size_t out_size) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (!path || !workspace_id || !out || out_size == 0 ||
        sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return -1;
    }
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT state FROM cbm_database_generation_v1 WHERE workspace_key=(SELECT workspace_key "
        "FROM cbm_workspace_registry WHERE workspace_id=?1) AND generation=?2",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, 1, workspace_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt, 2, generation);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (!text || strlen((const char *)text) + 1 > out_size) {
            rc = SQLITE_TOOBIG;
        } else {
            snprintf(out, out_size, "%s", (const char *)text);
            rc = SQLITE_OK;
        }
    } else if (rc == SQLITE_OK) {
        rc = SQLITE_NOTFOUND;
    }
    if (stmt)
        sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

TEST(zova_workspace_scoped_names_and_node_ids_are_deterministic) {
    const char *workspace_id = "w1_0123456789abcdef0123456789abcdef";
    char graph_name[128] = {0};
    char collection_name[192] = {0};
    char node_id[128] = {0};
    char changed_node_id[128] = {0};

    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_STR_EQ(graph_name, "cbm_graph_w1_0123456789abcdef0123456789abcdef");
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  workspace_id, "nomic_embed_code_v1", TZ_VEC_DIM, collection_name,
                  sizeof(collection_name)),
              0);
    ASSERT_STR_EQ(collection_name,
                  "cbm_nodes_i8_w1_0123456789abcdef0123456789abcdef_nomic_embed_code_v1_d768");

    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "src/main.c", "main.run",
                                             "named", node_id, sizeof(node_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "src/main.c", "main.run",
                                             "named", changed_node_id, sizeof(changed_node_id)),
              0);
    ASSERT_STR_EQ(node_id, changed_node_id);
    ASSERT_EQ(cbm_zova_workspace_node_id_v1(workspace_id, "Function", "src/main.c", "main.run",
                                             "lambda:1:8", changed_node_id,
                                             sizeof(changed_node_id)),
              0);
    ASSERT_NEQ(strcmp(node_id, changed_node_id), 0);
    ASSERT_TRUE(strncmp(node_id, "n:v1:w1_0123456789abcdef0123456789abcdef:", 40) == 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(
                  workspace_id, "Variable", "src/main.c", "", "owner:main|local:item|span:8:8",
                  node_id, sizeof(node_id)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(
                  workspace_id, "Variable", "src/main.c", "", "owner:main|local:item|span:8:8",
                  changed_node_id, sizeof(changed_node_id)), 0);
    ASSERT_STR_EQ(node_id, changed_node_id);
    ASSERT_TRUE(strncmp(node_id, "n:v2:", 5) == 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(
                  workspace_id, "Variable", "src/main.c", "", "owner:other|local:item|span:8:8",
                  changed_node_id, sizeof(changed_node_id)), 0);
    ASSERT_NEQ(strcmp(node_id, changed_node_id), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(workspace_id, "Variable", "src/main.c", "", "",
                                             changed_node_id, sizeof(changed_node_id)), -1);
    PASS();
}

TEST(zova_workspace_registry_path_uses_cache_dir) {
    char path[TZ_PATH_MAX] = {0};
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", "/tmp/cbm-zova-workspace-cache", 1);
    ASSERT_EQ(cbm_zova_workspace_registry_path(path, sizeof(path)), 0);
    ASSERT_STR_EQ(path, "/tmp/cbm-zova-workspace-cache/cbm.zova");
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(saved_copy);
    PASS();
}

#if !CBM_WITH_ZOVA

#else

TEST(zova_workspace_registry_preserves_ready_generation) {
    char registry_path[TZ_PATH_MAX];
    snprintf(registry_path, sizeof(registry_path), "%s/cbm_workspace_registry_%d_%p.zova",
             cbm_tmpdir(), (int)getpid(), (void *)&registry_path);
    cbm_unlink(registry_path);

    char workspace_id[96] = {0};
    char repeated_id[96] = {0};
    ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, "/tmp/cbm-registry-root",
                                                  workspace_id, sizeof(workspace_id)),
              0);
    ASSERT_TRUE(strncmp(workspace_id, "w1_", 3) == 0);
    ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, "/tmp/cbm-registry-root",
                                                  repeated_id, sizeof(repeated_id)),
              0);
    ASSERT_STR_EQ(workspace_id, repeated_id);

    int64_t active_generation = -1;
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                       &active_generation),
              0);
    ASSERT_EQ(active_generation, 0);

    ASSERT_EQ(cbm_zova_workspace_generation_begin_at(registry_path, workspace_id, 1), 0);
    ASSERT_EQ(cbm_zova_workspace_generation_finish_at(registry_path, workspace_id, 1, true), 0);
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                       &active_generation),
              0);
    ASSERT_EQ(active_generation, 1);

    ASSERT_EQ(cbm_zova_workspace_generation_begin_at(registry_path, workspace_id, 2), 0);
    ASSERT_EQ(cbm_zova_workspace_generation_finish_at(registry_path, workspace_id, 2, false), 0);
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(registry_path, workspace_id,
                                                       &active_generation),
              0);
    ASSERT_EQ(active_generation, 1);

    ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, "", repeated_id,
                                                  sizeof(repeated_id)),
              -1);
    cbm_unlink(registry_path);
    PASS();
}

TEST(zova_workspace_registry_lookup_is_read_only) {
    char registry_path[TZ_PATH_MAX];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    char lookup_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(registry_path, sizeof(registry_path), "%s/cbm_workspace_lookup_%d_%p.zova",
             cbm_tmpdir(), (int)getpid(), (void *)registry_path);
    cbm_unlink(registry_path);

    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, "/tmp/cbm-lookup-missing", lookup_id,
                                            sizeof(lookup_id)),
              -1);
    ASSERT_FALSE(cbm_file_exists(registry_path));

    ASSERT_EQ(cbm_zova_workspace_get_or_create_at(registry_path, "/tmp/cbm-lookup-present",
                                                   workspace_id, sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_lookup_at(registry_path, "/tmp/cbm-lookup-present", lookup_id,
                                            sizeof(lookup_id)),
              0);
    ASSERT_STR_EQ(lookup_id, workspace_id);

    cbm_unlink(registry_path);
    PASS();
}

TEST(zova_graph_session_hydrates_scoped_directional_walk) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-canonical-graph-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)path);
    cbm_unlink(path);
    const CBMDumpNode nodes[] = {
        {.id=1,.project="proj",.label="Function",.name="Alpha",
         .qualified_name="proj.Alpha",.file_path="alpha.c",.start_line=1,.end_line=4,
         .properties="{}"},
        {.id=2,.project="proj",.label="Function",.name="Beta",
         .qualified_name="proj.Beta",.file_path="beta.c",.start_line=5,.end_line=9,
         .properties="{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id=1,.project="proj",.source_id=1,.target_id=2,.type="CALLS",
         .properties="{\"kind\":\"call\"}",.url_path="/api/beta",.local_name="Beta"},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path="/tmp/canonical-graph",.project="proj",
        .indexed_at="2026-07-15T00:00:00Z",
        .model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,.vector_dimensions=2,
        .nodes=nodes,.node_count=2,.edges=edges,.edge_count=1,
    };
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &published), 0);

    const char *workspace_id = published.workspace_id;
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char alpha_node_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    char beta_node_id[CBM_ZOVA_WORKSPACE_ID_MAX + 64];
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(workspace_id, "Function", "alpha.c", "proj.Alpha",
                                             "named:proj.Alpha", alpha_node_id,
                                             sizeof(alpha_node_id)),
              0);
    ASSERT_EQ(cbm_zova_workspace_node_id_v2(workspace_id, "Function", "beta.c", "proj.Beta",
                                             "named:proj.Beta", beta_node_id,
                                             sizeof(beta_node_id)),
              0);

    cbm_zova_graph_session_t *session = cbm_zova_graph_session_open(path);
    ASSERT_NOT_NULL(session);
    cbm_zova_graph_visit_t *visits = NULL;
    int count = 0;
    cbm_zova_graph_metrics_t metrics = {0};
    cbm_setenv("CBM_ZOVA_GRAPH_PROFILE", "1", 1);
    ASSERT_EQ(cbm_zova_graph_session_walk_calls(session, workspace_id, graph_name, alpha_node_id,
                                                 "outbound", 2, 20, &visits, &count, &metrics),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(visits[0].node_id, beta_node_id);
    ASSERT_STR_EQ(visits[0].name, "Beta");
    ASSERT_NOT_NULL(visits[0].properties);
    ASSERT_STR_EQ(visits[0].properties, "{}");
    ASSERT_EQ(visits[0].hop, 1);
    ASSERT_FALSE(metrics.fallback);
    ASSERT_TRUE(metrics.native_profiled);
    ASSERT_GT(metrics.native_result_count, 0);
    ASSERT_GT(metrics.frontier_expansions, 0);
    cbm_unsetenv("CBM_ZOVA_GRAPH_PROFILE");
    cbm_zova_graph_visits_free(visits, count);

    const char *call_types[] = {"CALLS"};
    cbm_zova_graph_adjacency_t *adjacency = NULL;
    int adjacency_count = 0;
    ASSERT_EQ(cbm_zova_graph_session_adjacency(
                  session, workspace_id, graph_name, alpha_node_id, "outbound", call_types, 1,
                  10, &adjacency, &adjacency_count), 0);
    ASSERT_EQ(adjacency_count, 1);
    ASSERT_STR_EQ(adjacency[0].source_node_id, alpha_node_id);
    ASSERT_STR_EQ(adjacency[0].target_node_id, beta_node_id);
    ASSERT_STR_EQ(adjacency[0].edge_type, "CALLS");
    ASSERT_NOT_NULL(adjacency[0].properties);
    ASSERT_STR_EQ(adjacency[0].properties, "{\"kind\":\"call\"}");
    cbm_zova_graph_adjacency_free(adjacency, adjacency_count);
    ASSERT_EQ(cbm_zova_graph_session_adjacency_prepare_count(session), 0);
    adjacency = NULL;
    adjacency_count = 0;
    ASSERT_EQ(cbm_zova_graph_session_adjacency(
                  session, workspace_id, graph_name, beta_node_id, "inbound", call_types, 1,
                  10, &adjacency, &adjacency_count), 0);
    ASSERT_EQ(adjacency_count, 1);
    ASSERT_EQ(cbm_zova_graph_session_adjacency_prepare_count(session), 0);
    cbm_zova_graph_adjacency_free(adjacency, adjacency_count);
    int native_degree = -1;
    ASSERT_EQ(cbm_zova_graph_session_degree(session, workspace_id, graph_name, alpha_node_id,
                                             "both", call_types, 1, &native_degree), 0);
    ASSERT_EQ(native_degree, 1);

    visits = NULL;
    count = 0;
    ASSERT_EQ(cbm_zova_graph_session_walk_calls(session, workspace_id, graph_name, beta_node_id,
                                                 "inbound", 2, 20, &visits, &count, &metrics),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(visits[0].node_id, alpha_node_id);
    ASSERT_EQ(visits[0].hop, 1);
    cbm_zova_graph_visits_free(visits, count);
    cbm_zova_graph_session_close(session);
    cbm_unlink(path);
    PASS();
}

TEST(zova_gbuf_prepare_owns_full_publication_view) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char cache_path[TZ_PATH_MAX];
    snprintf(cache_path, sizeof(cache_path), "%s/cbm-zova-prepared-view-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache_path));
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache_path, 1);

    cbm_gbuf_t *gb = cbm_gbuf_new("prepared", "/tmp/prepared-view");
    ASSERT_NOT_NULL(gb);
    int64_t root = cbm_gbuf_upsert_node(
        gb, "Function", "Root", "prepared.Root", "root.c", 1, 2, "{}");
    int64_t leaf = cbm_gbuf_upsert_node(
        gb, "Function", "Leaf", "prepared.Leaf", "leaf.c", 3, 4, "{}");
    ASSERT_GT(root, 0);
    ASSERT_GT(leaf, 0);
    ASSERT_GT(cbm_gbuf_insert_edge(gb, root, leaf, "CALLS", "{}"), 0);
    ASSERT_EQ(cbm_gbuf_prepare_zova_dump(gb), 0);

    cbm_zova_publish_model_test_fail_allocation_at(0);
    cbm_zova_workspace_generation_result_t result = {0};
    ASSERT_EQ(cbm_gbuf_publish_zova_user_database(gb, NULL, 0, NULL, &result), 0);
    cbm_zova_publish_model_test_fail_allocation_at(-1);
    ASSERT_EQ(result.graph_nodes, 2);
    ASSERT_EQ(result.graph_edges, 1);

    char user_database_path[TZ_PATH_MAX];
    ASSERT_EQ(cbm_zova_workspace_registry_path(user_database_path,
                                                sizeof(user_database_path)), 0);
    cbm_gbuf_free(gb);
    cbm_unlink(user_database_path);
    char user_wal[TZ_PATH_MAX];
    char user_shm[TZ_PATH_MAX];
    snprintf(user_wal, sizeof(user_wal), "%s-wal", user_database_path);
    snprintf(user_shm, sizeof(user_shm), "%s-shm", user_database_path);
    cbm_unlink(user_wal);
    cbm_unlink(user_shm);
    if (saved_cache_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
        free(saved_cache_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    PASS();
#endif
}

TEST(zova_user_database_bootstrap_creates_versioned_workspace_schema) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-schema-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);

    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    /* Bootstrap is intentionally idempotent so a restarted migration can
     * safely resume without replacing the existing user-local database. */
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);

    const char *required[] = {
        "cbm_database_schema_v1", "cbm_workspace_registry", "cbm_database_generation_v1",
        "cbm_projects_v1", "cbm_nodes_v1",
        "cbm_files_v1", "cbm_file_hashes_v1",
        "cbm_token_vector_metadata_v1", "cbm_nodes_fts_v1",
        "cbm_workspace_index_state_v1", "cbm_project_summaries_v2",
        "cbm_generation_integrity_v2", "cbm_workspace_migrations_v1",
    };
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        ASSERT_EQ(sqlite3_prepare_v2(
                      db,
                      "SELECT count(*) FROM sqlite_master WHERE name = ?1 AND "
                      "type IN ('table','virtual table')",
                      -1, &stmt, NULL),
                  SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(stmt, 1, required[i], -1, SQLITE_STATIC), SQLITE_OK);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
        sqlite3_finalize(stmt);
    }
    const char *forbidden[] = {
        "cbm_node_vectors_compat_v1", "cbm_token_vectors_compat_v1", "cbm_fts_rowmap_v1",
        "cbm_workspace_generations", "cbm_project_summaries_v1",
        "cbm_generation_integrity_v1", "cbm_edges_v1", "cbm_edges_v1_workspace_zova_edge",
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        ASSERT_EQ(sqlite3_prepare_v2(
                      db, "SELECT count(*) FROM sqlite_master WHERE name=?1", -1, &stmt, NULL),
                  SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(stmt, 1, forbidden[i], -1, SQLITE_STATIC), SQLITE_OK);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_stmt *metadata_columns = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT group_concat(name,',') FROM pragma_table_info('cbm_token_vector_metadata_v1')",
                  -1, &metadata_columns, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(metadata_columns), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(metadata_columns, 0),
                  "token_key,workspace_key,token,idf");
    sqlite3_finalize(metadata_columns);
    sqlite3_stmt *workspace_columns = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT group_concat(name,',') FROM pragma_table_info('cbm_workspace_registry')",
                  -1, &workspace_columns, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(workspace_columns), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(workspace_columns, 0),
                  "workspace_key,workspace_id,canonical_root,id_format_version,active_generation");
    sqlite3_finalize(workspace_columns);
    sqlite3_stmt *node_columns = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT group_concat(name,',') FROM pragma_table_info('cbm_nodes_v1')",
                  -1, &node_columns, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(node_columns), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(node_columns, 0),
                  "zova_node_key,workspace_key,label,name,qualified_name,file_key,start_line,"
                  "end_line,properties,source_ordinal");
    sqlite3_finalize(node_columns);
    sqlite3_stmt *file_columns = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT group_concat(name,',') FROM pragma_table_info('cbm_files_v1')",
                  -1, &file_columns, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(file_columns), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(file_columns, 0),
                  "file_key,workspace_key,file_path");
    sqlite3_finalize(file_columns);
    sqlite3_close(db);

    int64_t schema_version = 0;
    ASSERT_EQ(zova_scalar_int64(path, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id = 1",
                                &schema_version),
              0);
    ASSERT_EQ(schema_version, CBM_ZOVA_DATABASE_SCHEMA_VERSION);

    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_stmt *fk_stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*) FROM pragma_foreign_key_list('cbm_nodes_v1') "
                  "WHERE \"table\"='cbm_files_v1' AND ((seq=0 AND \"from\"='workspace_key' "
                  "AND \"to\"='workspace_key') OR (seq=1 AND \"from\"='file_key' "
                  "AND \"to\"='file_key'))",
                  -1, &fk_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(fk_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(fk_stmt, 0), 2);
    sqlite3_finalize(fk_stmt);
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name IN ("
                  "'cbm_nodes_v1_file_workspace_bi','cbm_nodes_v1_file_workspace_bu',"
                  "'cbm_edges_v1_endpoint_workspace_bi','cbm_edges_v1_endpoint_workspace_bu')",
                  -1, &fk_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(fk_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(fk_stmt, 0), 0);
    sqlite3_finalize(fk_stmt);
    sqlite3_close(db);
    zova_database *zdb = NULL;
    zova_message zerr = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path=path,.out_db=&zdb,.out_error_message=&zerr}), ZOVA_OK);
    zova_message_free(&zerr);
    ASSERT_EQ(cbm_zova_register_sql_functions(zdb), 0);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db=zdb,.sql="PRAGMA foreign_keys=ON"}), ZOVA_OK);
    zova_status bootstrap_insert_status = zova_database_exec(&(zova_database_exec_request){
                  .db=zdb,.sql=
                      "INSERT INTO cbm_workspace_registry(workspace_id,canonical_root,id_format_version) "
                      "VALUES('workspace_A','/tmp/a',1),('workspace_B','/tmp/b',1);"
                      "INSERT INTO cbm_files_v1(workspace_key,file_path) VALUES"
                      "((SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id='workspace_A'),'a.c'),"
                      "((SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id='workspace_B'),'b.c');"
                      "INSERT INTO cbm_nodes_v1(zova_node_key,workspace_key,label,name,qualified_name,"
                      "file_key,start_line,end_line) VALUES"
                      "(101,(SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id='workspace_A'),"
                      "'Function','a','p.a',(SELECT file_key FROM cbm_files_v1 WHERE file_path='a.c'),1,1),"
                      "(102,(SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id='workspace_B'),"
                      "'Function','b','p.b',(SELECT file_key FROM cbm_files_v1 WHERE file_path='b.c'),1,1)"});
    if (bootstrap_insert_status != ZOVA_OK)
        fprintf(stderr, "bootstrap compact insert: %s\n", zova_database_last_error_message(zdb));
    ASSERT_EQ(bootstrap_insert_status, ZOVA_OK);
    ASSERT_NEQ(zova_database_exec(&(zova_database_exec_request){
                   .db=zdb,.sql=
                       "INSERT INTO cbm_nodes_v1(zova_node_key,workspace_key,label,name,qualified_name,"
                       "file_key,start_line,end_line) VALUES("
                       "103,(SELECT workspace_key FROM cbm_workspace_registry WHERE workspace_id='workspace_A'),"
                       "'Function','bad','p.bad',"
                       "(SELECT file_key FROM cbm_files_v1 WHERE file_path='b.c'),1,1)"}),
               ZOVA_OK);
    ASSERT_NEQ(zova_database_exec(&(zova_database_exec_request){
                   .db=zdb,.sql=
                       "UPDATE cbm_nodes_v1 SET file_key=(SELECT file_key FROM cbm_files_v1 "
                       "WHERE file_path='b.c') WHERE node_id='node_A'"}),
               ZOVA_OK);
    int64_t fts_rows = 0;
    ASSERT_EQ(zova_scalar_int64(path,
                                "SELECT count(*) FROM cbm_nodes_fts_v1_docsize",
                                &fts_rows), 0);
    ASSERT_EQ(fts_rows, 2);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db=zdb,.sql="UPDATE cbm_nodes_v1 SET name='GammaNode' "
                               "WHERE zova_node_key=101"}), ZOVA_OK);
    ASSERT_EQ(zova_scalar_int64(path,
                                "SELECT count(*) FROM cbm_nodes_fts_v1 "
                                "WHERE cbm_nodes_fts_v1 MATCH 'Gamma'",
                                &fts_rows), 0);
    ASSERT_EQ(fts_rows, 1);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db=zdb,.sql="BEGIN; UPDATE cbm_nodes_v1 SET name='DeltaNode' "
                               "WHERE zova_node_key=101; ROLLBACK"}), ZOVA_OK);
    ASSERT_EQ(zova_scalar_int64(path,
                                "SELECT count(*) FROM cbm_nodes_fts_v1 "
                                "WHERE cbm_nodes_fts_v1 MATCH 'Delta'",
                                &fts_rows), 0);
    ASSERT_EQ(fts_rows, 0);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db=zdb,.sql="DELETE FROM cbm_nodes_v1 WHERE zova_node_key=101;"
                               "INSERT INTO cbm_nodes_fts_v1(cbm_nodes_fts_v1) VALUES('rebuild');"
                               "INSERT INTO cbm_nodes_fts_v1(cbm_nodes_fts_v1,rank) "
                               "VALUES('integrity-check',1)"}), ZOVA_OK);
    ASSERT_EQ(zova_scalar_int64(path,
                                "SELECT count(*) FROM cbm_nodes_fts_v1_docsize",
                                &fts_rows), 0);
    ASSERT_EQ(fts_rows, 1);
    ASSERT_EQ(cbm_zova_user_database_verify_workspace_schema(zdb), 0);
    ASSERT_EQ(zova_database_close(zdb), ZOVA_OK);

    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "EXPLAIN QUERY PLAN SELECT file_key FROM cbm_files_v1 f "
                  "WHERE f.workspace_key=1 AND NOT EXISTS(SELECT 1 FROM cbm_nodes_v1 n "
                  "WHERE n.workspace_key=f.workspace_key AND n.file_key=f.file_key)",
                  -1, &fk_stmt, NULL), SQLITE_OK);
    bool indexed_file_reference_lookup=false;
    while(sqlite3_step(fk_stmt)==SQLITE_ROW){
        const char *detail=(const char *)sqlite3_column_text(fk_stmt,3);
        if(detail&&strstr(detail,"cbm_nodes_v1_workspace_file")&&
           strstr(detail,"workspace_key=? AND file_key=?"))
            indexed_file_reference_lookup=true;
    }
    ASSERT_TRUE(indexed_file_reference_lookup);
    sqlite3_finalize(fk_stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA foreign_key_check", -1, &fk_stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(fk_stmt), SQLITE_DONE);
    sqlite3_finalize(fk_stmt);
    sqlite3_close(db);
    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_schema_uses_zova_as_sole_topology_authority) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-sole-topology-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)path);
    cbm_unlink(path);
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);

    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT group_concat(name,',') FROM pragma_table_info('cbm_nodes_v1')",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "zova_node_key,workspace_key,label,name,qualified_name,file_key,start_line,"
                  "end_line,properties,source_ordinal");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*) FROM sqlite_master WHERE type='table' "
                  "AND name='cbm_edges_v1'",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*) FROM sqlite_master WHERE type='index' AND name IN ("
                  "'cbm_edges_v1_source_type','cbm_edges_v1_target_type')",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*) FROM sqlite_master WHERE type='index' "
                  "AND name='cbm_edges_v1_workspace_zova_edge'",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    cbm_unlink(path);
    PASS();
}

TEST(zova_database_format_status_requires_atomic_repack_for_v5_v7) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-format-status-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    ASSERT_EQ(cbm_zova_database_format_status(path), CBM_ZOVA_DATABASE_COMPATIBLE);

    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "UPDATE _zova_meta SET value='12' WHERE key='format_version'",
                           NULL, NULL, NULL),
              SQLITE_OK);
    sqlite3_close(db);
    ASSERT_EQ(cbm_zova_database_format_status(path), CBM_ZOVA_DATABASE_MIGRATION_REQUIRED);

    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "UPDATE _zova_meta SET value='9' WHERE key='format_version'",
                           NULL, NULL, NULL),
              SQLITE_OK);
    sqlite3_close(db);
    ASSERT_EQ(cbm_zova_database_format_status(path), CBM_ZOVA_DATABASE_MIGRATION_REQUIRED);

    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
                           "UPDATE _zova_meta SET value='7' WHERE key='format_version';"
                           "UPDATE cbm_database_schema_v1 SET schema_version=5 WHERE id=1;",
                           NULL, NULL, NULL),
              SQLITE_OK);
    sqlite3_close(db);
    ASSERT_EQ(cbm_zova_database_format_status(path), CBM_ZOVA_DATABASE_REPACK_REQUIRED);

    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "UPDATE cbm_database_schema_v1 SET schema_version=999 WHERE id=1",
                           NULL, NULL, NULL),
              SQLITE_OK);
    sqlite3_close(db);
    ASSERT_EQ(cbm_zova_database_format_status(path), CBM_ZOVA_DATABASE_INCOMPATIBLE);

    static const char *expected[] = {
        "source_snapshot", "temp_creation", "workspace_publish", "verification", "fsync",
        "live_to_recovery_rename", "temp_to_live_rename", "final_reopen",
    };
    ASSERT_EQ((int)(sizeof(expected) / sizeof(expected[0])), CBM_ZOVA_REPACK_PHASE_COUNT);
    for (int i = 0; i < CBM_ZOVA_REPACK_PHASE_COUNT; i++)
        ASSERT_STR_EQ(cbm_zova_repack_phase_name((cbm_zova_repack_phase_t)i), expected[i]);
    ASSERT_NULL(cbm_zova_repack_phase_name((cbm_zova_repack_phase_t)-1));
    ASSERT_NULL(cbm_zova_repack_phase_name(CBM_ZOVA_REPACK_PHASE_COUNT));
    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_has_one_canonical_fts_without_rowmap) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-fts-schema-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)path);
    cbm_unlink(path);
    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    int64_t count = 0;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE sql LIKE 'CREATE VIRTUAL TABLE%'",
                  &count),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE name='cbm_fts_rowmap_v1' "
                  "OR name GLOB 'cbm_fts_w1_*'",
                  &count),
              0);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE name='cbm_nodes_fts_v1_content'",
                  &count),
              0);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE type='view' "
                  "AND name='cbm_nodes_fts_content_v1'",
                  &count),
              0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE type='trigger' "
                  "AND name IN ('cbm_nodes_v1_fts_ai','cbm_nodes_v1_fts_bd',"
                  "'cbm_nodes_v1_fts_bu','cbm_nodes_v1_fts_au')",
                  &count),
              0);
    ASSERT_EQ(count, 4);
    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_generation_state_commits_and_rolls_back) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-generation-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);

    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    ASSERT_EQ(cbm_zova_user_database_generation_begin(
                  path, "/tmp/cbm-user-generation-project", 1, workspace_id,
                  sizeof(workspace_id)),
              0);
    ASSERT_TRUE(workspace_id[0] != '\0');
    char state[16] = {0};
    ASSERT_EQ(zova_generation_state(path, workspace_id, 1, state, sizeof(state)), 0);
    ASSERT_STR_EQ(state, "building");

    ASSERT_EQ(cbm_zova_user_database_generation_finish(path, workspace_id, 1, true), 0);
    ASSERT_EQ(zova_generation_state(path, workspace_id, 1, state, sizeof(state)), 0);
    ASSERT_STR_EQ(state, "ready");
    int64_t active_generation = 0;
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(path, workspace_id, &active_generation), 0);
    ASSERT_EQ(active_generation, 1);

    ASSERT_EQ(cbm_zova_user_database_generation_begin(
                  path, "/tmp/cbm-user-generation-project", 2, workspace_id,
                  sizeof(workspace_id)),
              0);
    ASSERT_EQ(cbm_zova_user_database_generation_finish(path, workspace_id, 2, false), 0);
    ASSERT_EQ(zova_generation_state(path, workspace_id, 2, state, sizeof(state)), 0);
    ASSERT_STR_EQ(state, "failed");
    ASSERT_EQ(cbm_zova_workspace_active_generation_at(path, workspace_id, &active_generation), 0);
    ASSERT_EQ(active_generation, 1);

    /* A generation can only transition once from building; a failed/ready
     * generation cannot be silently rewritten by a finishing call. */
    ASSERT_EQ(cbm_zova_user_database_generation_finish(path, workspace_id, 2, true), -1);
    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_capability_probe_covers_empty_schema_surface) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-capabilities-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);

    ASSERT_EQ(cbm_zova_user_database_init(path), 0);
    cbm_zova_sql_capabilities_t capabilities = {0};
    ASSERT_EQ(cbm_zova_probe_sql_capabilities(path, &capabilities), 0);
    ASSERT_TRUE(capabilities.canonical_workspace_metadata);
    ASSERT_TRUE(capabilities.fts5);
    ASSERT_TRUE(capabilities.bm25);
    ASSERT_TRUE(capabilities.json);
    ASSERT_TRUE(capabilities.recursive_cte);
    ASSERT_TRUE(capabilities.transactions);
    ASSERT_TRUE(capabilities.cbm_scalar_functions);

    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_imports_workspace_metadata_and_fts) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-import-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);

    const CBMDumpNode nodes[] = {
        {.id = 1,
         .project = "proj",
         .label = "Function",
         .name = "AlphaBeta",
         .qualified_name = "proj.AlphaBeta",
         .file_path = "src/alpha.c",
         .start_line = 3,
         .end_line = 8,
         .properties = "{\"risk\":2}"},
        {.id = 2,
         .project = "proj",
         .label = "Function",
         .name = "Helper",
         .qualified_name = "proj.Helper",
         .file_path = "src/helper.c",
         .start_line = 10,
         .end_line = 12,
         .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1,
         .project = "proj",
         .source_id = 1,
         .target_id = 2,
         .type = "CALLS",
         .properties = "{\"weight\":1}",
         .url_path = "",
         .local_name = ""},
    };

    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/user-import-a", "proj", 1,
                                                       nodes, 2, edges, 1),
              0);
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/user-import-a", "proj", 2,
                                                       nodes, 2, edges, 1),
              0);
    cbm_zova_graph_session_t *generation_session = cbm_zova_graph_session_open(path);
    ASSERT_NOT_NULL(generation_session);
    int64_t session_generation = 0;
    ASSERT_EQ(cbm_zova_graph_session_generation(generation_session, &session_generation), 0);
    ASSERT_EQ(session_generation, 2);
    cbm_zova_graph_session_close(generation_session);

    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    ASSERT_EQ(cbm_zova_workspace_lookup_at(path, "/tmp/user-import-a", workspace_id,
                                            sizeof(workspace_id)),
              0);
    cbm_store_t *workspace_store = cbm_store_open_zova_workspace_query(path, workspace_id);
    ASSERT_NOT_NULL(workspace_store);
    sqlite3_stmt *cache_size_stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(cbm_store_get_db(workspace_store), "PRAGMA cache_size", -1,
                                 &cache_size_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(cache_size_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(cache_size_stmt, 0), -262144);
    sqlite3_finalize(cache_size_stmt);
    cbm_search_output_t workspace_search = {0};
    cbm_search_params_t workspace_params = {.project = "proj", .min_degree = -1,
                                             .max_degree = -1, .limit = 20};
    (void)cbm_store_search(workspace_store, &workspace_params, &workspace_search);
    if (workspace_search.count != 2) fprintf(stderr, "workspace search error: %s\n", cbm_store_error(workspace_store));
    ASSERT_EQ(workspace_search.count, 2);
    cbm_store_search_free(&workspace_search);
    cbm_cypher_result_t workspace_cypher = {0};
    ASSERT_EQ(cbm_cypher_execute(workspace_store,
              "MATCH (n) WHERE n.name IS NOT NULL RETURN n.name ORDER BY n.name", "proj", 20,
              &workspace_cypher), 0);
    ASSERT_EQ(workspace_cypher.row_count, 2);
    cbm_cypher_result_free(&workspace_cypher);
    cbm_store_close(workspace_store);
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    int64_t count = 0;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT workspace_key "
             "FROM cbm_workspace_registry WHERE workspace_id='%s')", workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 2);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_fts_v1 f JOIN cbm_nodes_v1 n ON n.zova_node_key=f.rowid "
             "WHERE n.workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
             "WHERE workspace_id='%s') AND cbm_nodes_fts_v1 MATCH 'Alpha'",
             workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 1);
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    ASSERT_EQ(cbm_zova_workspace_graph_name(workspace_id, graph_name, sizeof(graph_name)), 0);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM _zova_graph_nodes n JOIN _zova_graphs g "
             "ON g.graph_key=n.graph_key WHERE g.name='%s' AND "
             "(n.target_type<>'none' OR n.target_namespace IS NOT NULL OR n.target_ref IS NOT NULL)",
             graph_name);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 0);

    /* A failed replacement must roll back the clear-and-rewrite transaction,
     * leaving generation 2's rows available. */
    cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", "user_after_nodes", 1);
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/user-import-a", "proj", 3,
                                                       nodes, 2, edges, 1),
              -1);
    cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT workspace_key "
             "FROM cbm_workspace_registry WHERE workspace_id='%s')", workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 2);

    /* Reusing the same numeric dump ids for another workspace must not cause
     * metadata or FTS rows to collide. */
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/user-import-b", "proj-b", 1,
                                                       nodes, 2, edges, 1),
              0);
    char workspace_b[CBM_ZOVA_WORKSPACE_ID_MAX] = {0};
    ASSERT_EQ(cbm_zova_workspace_lookup_at(path, "/tmp/user-import-b", workspace_b,
                                            sizeof(workspace_b)),
              0);
    ASSERT_TRUE(strcmp(workspace_id, workspace_b) != 0);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             workspace_b);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 2);

    /* Replacing workspace A must leave workspace B untouched. */
    const CBMDumpNode replacement_nodes[] = {nodes[0]};
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/user-import-a", "proj", 4,
                                                       replacement_nodes, 1, NULL, 0),
              0);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 1);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             workspace_b);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 2);
    sqlite3_close(db);
    cbm_unlink(path);
    PASS();
}

TEST(zova_catalog_assigns_stable_shortest_path_selectors) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-catalog-selectors-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)path);
    cbm_unlink(path);

    const CBMDumpNode first_node = {
        .id = 1, .project = "internal-a", .label = "Function", .name = "ClientCall",
        .qualified_name = "internal-a.ClientCall", .file_path = "src/client.c",
        .start_line = 1, .end_line = 2, .properties = "{}"};
    const CBMDumpNode second_node = {
        .id = 1, .project = "internal-b", .label = "Function", .name = "ServiceHandler",
        .qualified_name = "internal-b.ServiceHandler", .file_path = "src/service.c",
        .start_line = 1, .end_line = 2, .properties = "{}"};
    ASSERT_EQ(cbm_zova_user_database_import_workspace(
                  path, "/tmp/Primary/shared-project", "internal-a", 1,
                  &first_node, 1, NULL, 0),
              0);
    ASSERT_EQ(cbm_zova_user_database_import_workspace(
                  path, "/tmp/AnotherFolder/shared-project", "internal-b", 1,
                  &second_node, 1, NULL, 0),
              0);

    cbm_zova_catalog_t *catalog = cbm_zova_catalog_open(path);
    ASSERT_NOT_NULL(catalog);
    cbm_zova_catalog_scope_t scope = {0};
    ASSERT_EQ(cbm_zova_catalog_resolve(catalog, NULL, 0, true, &scope), CBM_STORE_OK);
    ASSERT_EQ(scope.count, 2);
    ASSERT_STR_EQ(scope.projects[0].selector, "AnotherFolder/shared-project");
    ASSERT_STR_EQ(scope.projects[0].project, "internal-b");
    ASSERT_STR_EQ(scope.projects[1].selector, "shared-project");
    ASSERT_STR_EQ(scope.projects[1].project, "internal-a");
    cbm_zova_repository_t *selected = cbm_zova_repository_open(path, "shared-project");
    ASSERT_NOT_NULL(selected);
    ASSERT_STR_EQ(cbm_zova_repository_workspace_id(selected), scope.projects[1].workspace_id);
    cbm_zova_repository_close(selected);
    cbm_search_output_t search = {0};
    ASSERT_EQ(cbm_zova_catalog_search_fts(catalog, &scope, "Client OR Service", NULL, 10, 0,
                                          &search),
              CBM_STORE_OK);
    ASSERT_EQ(search.total, 2);
    ASSERT_EQ(search.count, 2);
    ASSERT_STR_EQ(search.results[0].node.project, "AnotherFolder/shared-project");
    ASSERT_STR_EQ(search.results[1].node.project, "shared-project");
    cbm_store_search_free(&search);
    cbm_search_params_t regular_params = {
        .min_degree = -1, .max_degree = -1, .limit = 1, .offset = 1};
    ASSERT_EQ(cbm_zova_catalog_search(catalog, &scope, &regular_params, &search),
              CBM_STORE_OK);
    ASSERT_EQ(search.total, 2);
    ASSERT_EQ(search.count, 1);
    ASSERT_STR_EQ(search.results[0].node.name, "ServiceHandler");
    ASSERT_STR_EQ(search.results[0].node.project, "AnotherFolder/shared-project");
    cbm_store_search_free(&search);
    cbm_zova_catalog_scope_free(&scope);
    cbm_zova_catalog_close(catalog);

    zova_database *writer = NULL;
    zova_message writer_error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path=path,.out_db=&writer,.out_error_message=&writer_error}), ZOVA_OK);
    zova_message_free(&writer_error);
    ASSERT_EQ(zova_database_exec(&(zova_database_exec_request){
                  .db=writer,
                  .sql="INSERT INTO cbm_workspace_health_v1(workspace_key,state,reason,"
                       "checked_generation,checked_at) SELECT workspace_key,'rebuild_required',"
                       "'fixture requires rebuild',1,'2026-07-22T00:00:00Z' FROM "
                       "cbm_projects_v1 WHERE project='internal-b' ON CONFLICT(workspace_key) "
                       "DO UPDATE SET state=excluded.state,reason=excluded.reason"}),
              ZOVA_OK);
    ASSERT_EQ(zova_database_close(writer), ZOVA_OK);
    catalog = cbm_zova_catalog_open(path);
    ASSERT_NOT_NULL(catalog);
    ASSERT_EQ(cbm_zova_catalog_resolve(catalog, NULL, 0, true, &scope), CBM_STORE_OK);
    ASSERT_EQ(scope.count, 1);
    ASSERT_EQ(scope.excluded_count, 1);
    ASSERT_STR_EQ(scope.excluded_projects[0].selector, "AnotherFolder/shared-project");
    ASSERT_STR_EQ(scope.excluded_projects[0].health_reason, "fixture requires rebuild");
    cbm_zova_catalog_scope_free(&scope);
    cbm_zova_catalog_close(catalog);
    cbm_unlink(path);
    PASS();
}

TEST(zova_catalog_semantic_search_merges_workspace_collections_globally) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-catalog-semantic-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)path);
    cbm_unlink(path);
    int8_t client_vector[768] = {0};
    int8_t service_vector[768] = {0};
    client_vector[0] = 127;
    service_vector[1] = 127;
    const CBMDumpNode client = {
        .id=1,.project="internal-client",.label="Function",.name="ClientCall",
        .qualified_name="internal-client.ClientCall",.file_path="src/client.c",.properties="{}"};
    const CBMDumpNode service = {
        .id=1,.project="internal-service",.label="Function",.name="ServiceHandler",
        .qualified_name="internal-service.ServiceHandler",.file_path="src/service.c",.properties="{}"};
    const CBMDumpVector client_node_vector = {
        .node_id=1,.project="internal-client",.vector=(const uint8_t *)client_vector,
        .vector_len=sizeof(client_vector)};
    const CBMDumpVector service_node_vector = {
        .node_id=1,.project="internal-service",.vector=(const uint8_t *)service_vector,
        .vector_len=sizeof(service_vector)};
    const CBMDumpTokenVec client_token = {
        .id=1,.project="internal-client",.token="request",
        .vector=(const uint8_t *)client_vector,.vector_len=sizeof(client_vector),.idf=1.0f};
    const CBMDumpTokenVec service_token = {
        .id=1,.project="internal-service",.token="request",
        .vector=(const uint8_t *)client_vector,.vector_len=sizeof(client_vector),.idf=1.0f};
    const cbm_zova_workspace_generation_input_t client_input = {
        .root_path="/tmp/Primary/shared-project",.project="internal-client",
        .indexed_at="2026-07-22T00:00:00Z",.model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions=768,.nodes=&client,.node_count=1,
        .node_vectors=&client_node_vector,.node_vector_count=1,
        .token_vectors=&client_token,.token_vector_count=1};
    const cbm_zova_workspace_generation_input_t service_input = {
        .root_path="/tmp/AnotherFolder/shared-project",.project="internal-service",
        .indexed_at="2026-07-22T00:00:00Z",.model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions=768,.nodes=&service,.node_count=1,
        .node_vectors=&service_node_vector,.node_vector_count=1,
        .token_vectors=&service_token,.token_vector_count=1};
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &client_input, &published), 0);
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &service_input, &published), 0);

    cbm_zova_catalog_t *catalog = cbm_zova_catalog_open(path);
    ASSERT_NOT_NULL(catalog);
    cbm_zova_catalog_scope_t scope = {0};
    ASSERT_EQ(cbm_zova_catalog_resolve(catalog, NULL, 0, true, &scope), CBM_STORE_OK);
    const char *keywords[] = {"request"};
    cbm_vector_result_t *results = NULL;
    int count = 0;
    int total = 0;
    ASSERT_EQ(cbm_zova_catalog_search_semantic(
                  catalog, path, &scope, keywords, 1, 10, 0, &results, &count, &total),
              CBM_STORE_OK);
    ASSERT_EQ(count, 2);
    ASSERT_EQ(total, 2);
    ASSERT_STR_EQ(results[0].name, "ClientCall");
    ASSERT_STR_EQ(results[0].project, "shared-project");
    ASSERT_STR_EQ(results[1].name, "ServiceHandler");
    ASSERT_STR_EQ(results[1].project, "AnotherFolder/shared-project");
    cbm_store_free_vector_results(results, count);
    cbm_zova_catalog_scope_free(&scope);
    cbm_zova_catalog_close(catalog);
    cbm_unlink(path);
    PASS();
}

TEST(zova_user_database_delete_workspace_is_isolated_and_idempotent) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-user-delete-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);
    const CBMDumpNode a[] = {{.id=1,.project="shared",.label="Function",.name="same",
        .qualified_name="shared.sameA",.file_path="same.c",.start_line=1,.end_line=2,
        .properties="{}"}};
    const CBMDumpNode b[] = {{.id=1,.project="shared_b",.label="Function",.name="same",
        .qualified_name="shared.sameB",.file_path="same.c",.start_line=1,.end_line=2,
        .properties="{}"}};
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/delete-a", "shared", 1,
                                                       a, 1, NULL, 0), 0);
    ASSERT_EQ(cbm_zova_user_database_import_workspace(path, "/tmp/delete-b", "shared_b", 1,
                                                       b, 1, NULL, 0), 0);
    cbm_zova_repository_t *repo_a = cbm_zova_repository_open(path, "shared");
    cbm_zova_repository_t *repo_b = cbm_zova_repository_open(path, "shared_b");
    ASSERT_NOT_NULL(repo_a); ASSERT_NOT_NULL(repo_b);
    char workspace_a[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_a, sizeof(workspace_a), "%s", cbm_zova_repository_workspace_id(repo_a));
    char workspace_b[CBM_ZOVA_WORKSPACE_ID_MAX];
    snprintf(workspace_b, sizeof(workspace_b), "%s", cbm_zova_repository_workspace_id(repo_b));
    cbm_zova_repository_close(repo_a); cbm_zova_repository_close(repo_b);
    ASSERT_EQ(cbm_zova_user_database_delete_workspace(path, workspace_a), 0);
    ASSERT_NULL(cbm_zova_repository_open(path, "shared"));
    repo_b = cbm_zova_repository_open(path, "shared_b");
    ASSERT_NOT_NULL(repo_b);
    ASSERT_STR_EQ(cbm_zova_repository_workspace_id(repo_b), workspace_b);
    cbm_zova_repository_close(repo_b);
    ASSERT_EQ(cbm_zova_user_database_delete_workspace(path, workspace_a), 0);
    ASSERT_EQ(cbm_zova_user_database_delete_workspace(path, "bad-id"), -1);
    cbm_unlink(path);
    PASS();
}

TEST(zova_atomic_workspace_publisher_commits_complete_generation) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-atomic-publish-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "atomic", .label = "Function", .name = "Root",
         .qualified_name = "atomic.Root", .file_path = "root.c", .properties = "{}"},
        {.id = 2, .project = "atomic", .label = "Function", .name = "Leaf",
         .qualified_name = "atomic.Leaf", .file_path = "leaf.c", .properties = "{\"risk\":1}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1, .project = "atomic", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    const int8_t root_vec[] = {127, 0};
    const int8_t leaf_vec[] = {0, 127};
    const CBMDumpVector vectors[] = {
        {.node_id = 1, .project = "atomic", .vector = (const uint8_t *)root_vec, .vector_len = 2},
        {.node_id = 2, .project = "atomic", .vector = (const uint8_t *)leaf_vec, .vector_len = 2},
    };
    const CBMDumpTokenVec tokens[] = {
        {.id = 1, .project = "atomic", .token = "root", .vector = (const uint8_t *)root_vec,
         .vector_len = 2, .idf = 1.0f},
        {.id = 2, .project = "atomic", .token = "leaf", .vector = (const uint8_t *)leaf_vec,
         .vector_len = 2, .idf = 0.5f},
    };
    const cbm_zova_file_hash_input_t hashes[] = {
        {.file_path = "root.c", .content_hash = "hash-root", .mtime_ns = 1, .size_bytes = 10},
        {.file_path = "leaf.c", .content_hash = "hash-leaf", .mtime_ns = 2, .size_bytes = 11},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/atomic-publish",
        .project = "atomic",
        .indexed_at = "2026-07-12T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 2,
        .nodes = nodes,
        .node_count = 2,
        .edges = edges,
        .edge_count = 1,
        .node_vectors = vectors,
        .node_vector_count = 2,
        .token_vectors = tokens,
        .token_vector_count = 2,
        .file_hashes = hashes,
        .file_hash_count = 2,
        .project_summary = {.present = true, .summary = "atomic summary", .source_hash = "sum-hash",
                            .created_at = "created", .updated_at = "updated"},
    };
    cbm_zova_workspace_generation_result_t result = {0};
    cbm_zova_publish_test_metrics_reset();
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &result), 0);
    cbm_zova_publish_test_metrics_t publish_metrics = {0};
    cbm_zova_publish_test_metrics_get(&publish_metrics);
    ASSERT_EQ(publish_metrics.database_open_count, 1);
    ASSERT_EQ(publish_metrics.database_close_count, 1);
    ASSERT_EQ(publish_metrics.database_handle_open_count, 1);
    ASSERT_EQ(publish_metrics.fresh_page_size_vacuum_count, 0);
    ASSERT_EQ(publish_metrics.transaction_count, 1);
    ASSERT_EQ(publish_metrics.full_clear_count, 1);
    ASSERT_EQ(publish_metrics.canonical_node_fts_passes, 1);
    ASSERT_EQ(publish_metrics.canonical_edge_passes, 0);
    ASSERT_EQ(publish_metrics.native_graph_fresh_calls, 1);
    ASSERT_EQ(publish_metrics.native_graph_prepared_calls, 1);
    ASSERT_EQ(publish_metrics.native_graph_node_calls, 0);
    ASSERT_EQ(publish_metrics.native_graph_edge_calls, 0);
    ASSERT_EQ(publish_metrics.native_node_vector_calls, 0);
    ASSERT_EQ(publish_metrics.native_token_vector_calls, 0);
    ASSERT_EQ(publish_metrics.integrity_writes, 1);
    ASSERT_EQ(publish_metrics.full_fts_bulk_statements, 0);
    ASSERT_EQ(publish_metrics.full_fts_trigger_rows_avoided, 2);
    ASSERT_EQ(publish_metrics.readback_count_scan_count, 0);
    ASSERT_EQ(publish_metrics.full_node_guard_validation_statements, 0);
    ASSERT_EQ(publish_metrics.full_edge_guard_validation_statements, 0);
    ASSERT_EQ(publish_metrics.canonical_files_sql.rows, 2);
    ASSERT_EQ(publish_metrics.canonical_files_sql.bind_i64_calls, 4);
    ASSERT_EQ(publish_metrics.canonical_files_sql.bind_text_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_files_sql.bind_double_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_files_sql.step_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_files_sql.reset_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_files_sql.clear_bindings_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.rows, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.bind_i64_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.bind_text_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.bind_double_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.step_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.reset_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_nodes_sql.clear_bindings_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.rows, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.bind_i64_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.bind_text_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.bind_double_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.step_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.reset_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_edges_sql.clear_bindings_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.rows, 2);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.bind_i64_calls, 6);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.bind_text_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.bind_double_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.step_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.reset_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_hashes_sql.clear_bindings_calls, 0);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.rows, 2);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.bind_i64_calls, 4);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.bind_text_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.bind_double_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.step_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.reset_calls, 2);
    ASSERT_EQ(publish_metrics.canonical_token_metadata_sql.clear_bindings_calls, 0);
    ASSERT_TRUE(isfinite(result.canonical_files_ms) && result.canonical_files_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.canonical_nodes_ms) && result.canonical_nodes_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.canonical_edges_ms) && result.canonical_edges_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.canonical_hashes_ms) && result.canonical_hashes_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.fts_ms) && result.fts_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.token_metadata_ms) && result.token_metadata_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_ms) && result.native_graph_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_materialize_ms) &&
                result.native_graph_materialize_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_reset_ms) && result.native_graph_reset_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_nodes_ms) && result.native_graph_nodes_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_edges_ms) && result.native_graph_edges_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_validate_ms) &&
                result.native_graph_validate_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_key_generation_ms) &&
                result.native_graph_key_generation_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.native_graph_cleanup_ms) &&
                result.native_graph_cleanup_ms >= 0.0);
    double native_graph_subtotal = result.native_graph_materialize_ms +
        result.native_graph_reset_ms + result.native_graph_nodes_ms +
        result.native_graph_edges_ms + result.native_graph_validate_ms +
        result.native_graph_key_generation_ms + result.native_graph_cleanup_ms;
    ASSERT_TRUE(native_graph_subtotal <= result.native_graph_ms + 1.0);
    ASSERT_TRUE(isfinite(result.fresh_validation_ms) && result.fresh_validation_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.fresh_index_ms) && result.fresh_index_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.fresh_commit_ms) && result.fresh_commit_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.fresh_build_ms) && result.fresh_build_ms >= 0.0);
    double fresh_phase_subtotal = result.fresh_validation_ms +
        result.native_graph_validate_ms + result.native_graph_key_generation_ms +
        result.native_graph_nodes_ms + result.native_graph_edges_ms +
        result.canonical_nodes_ms + result.fts_ms + result.native_vectors_ms +
        result.fresh_index_ms + result.fresh_commit_ms;
    ASSERT_TRUE(fresh_phase_subtotal <= result.fresh_build_ms + 1.0);
    ASSERT_TRUE(result.native_graph_cleanup_ms <= result.fresh_build_ms + 1.0);
    ASSERT_TRUE(isfinite(result.native_vectors_ms) && result.native_vectors_ms >= 0.0);
    ASSERT_TRUE(isfinite(result.readback_ms) && result.readback_ms >= 0.0);
    ASSERT_TRUE(result.workspace_id[0] != '\0');
    ASSERT_EQ(result.generation, 1);
    ASSERT_EQ(result.graph_nodes, 2);
    ASSERT_EQ(result.graph_edges, 1);
    ASSERT_EQ(result.metadata_nodes, 2);
    ASSERT_EQ(result.metadata_edges, 1);
    ASSERT_EQ(result.fts_rows, 2);
    ASSERT_EQ(result.node_vectors, 2);
    ASSERT_EQ(result.token_vectors, 2);
    int64_t schema_version = 0;
    ASSERT_EQ(zova_scalar_int64(
                  path, "SELECT schema_version FROM cbm_database_schema_v1 WHERE id=1",
                  &schema_version),
              0);
    ASSERT_EQ(schema_version, CBM_ZOVA_DATABASE_SCHEMA_VERSION);
    int64_t projection_tables = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
                  "('cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1')",
                  &projection_tables),
              0);
    ASSERT_EQ(projection_tables, 0);
    int64_t forbidden_objects = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE "
                  "name IN ('cbm_zova_trace_nodes_v1','cbm_zova_edge_metadata_v1',"
                  "'cbm_fts_rowmap_v1','cbm_node_vectors_compat_v1',"
                  "'cbm_token_vectors_compat_v1','_zova_vector_norms') "
                  "OR name GLOB 'cbm_fts_w1_*'",
                  &forbidden_objects),
              0);
    ASSERT_EQ(forbidden_objects, 0);
    int64_t token_columns = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM pragma_table_info('cbm_token_vector_metadata_v1') "
                  "WHERE (cid=0 AND name='token_key' AND type='INTEGER' AND pk=1) OR "
                  "(cid=1 AND name='workspace_key' AND type='INTEGER') OR "
                  "(cid=2 AND name='token' AND type='TEXT') OR "
                  "(cid=3 AND name='idf' AND type='REAL')",
                  &token_columns),
              0);
    ASSERT_EQ(token_columns, 4);
    int64_t duplicate_fts_objects = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE name='cbm_fts_rowmap_v1' "
                  "OR name GLOB 'cbm_fts_w1_*'",
                  &duplicate_fts_objects),
              0);
    ASSERT_EQ(duplicate_fts_objects, 0);
    int64_t canonical_fts_rows = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path, "SELECT count(*) FROM cbm_nodes_fts_v1_docsize", &canonical_fts_rows),
              0);
    ASSERT_EQ(canonical_fts_rows, result.metadata_nodes);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND "
                  "name='cbm_nodes_v1_fts_ai'",
                  &canonical_fts_rows),
              0);
    ASSERT_EQ(canonical_fts_rows, 1);
    cbm_store_t *cypher_store =
        cbm_store_open_zova_workspace_query(path, result.workspace_id);
    ASSERT_NOT_NULL(cypher_store);
    cbm_cypher_result_t native_cypher = {0};
    ASSERT_EQ(cbm_cypher_execute(cypher_store,
              "MATCH (a:Function)-[:CALLS]->(b:Function) RETURN a.name,b.name",
              "atomic", 20, &native_cypher), 0);
    ASSERT_EQ(native_cypher.row_count, 1);
    ASSERT_GT(cbm_store_zova_native_topology_ops(cypher_store), 0);
    cbm_cypher_result_free(&native_cypher);
    int topology_before_degree = cbm_store_zova_native_topology_ops(cypher_store);
    cbm_cypher_result_t degree_cypher = {0};
    ASSERT_EQ(cbm_cypher_execute(cypher_store,
              "MATCH (a:Function) RETURN a.name,a.in_degree,a.out_degree ORDER BY a.name",
              "atomic", 20, &degree_cypher), 0);
    ASSERT_EQ(degree_cypher.row_count, 2);
    ASSERT_GT(cbm_store_zova_native_topology_ops(cypher_store), topology_before_degree);
    cbm_cypher_result_free(&degree_cypher);
    cbm_store_close(cypher_store);
    int64_t count = 0;
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_workspace_index_state_v1 WHERE "
                              "workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
                              "WHERE workspace_id='%s') AND generation=1", result.workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 1);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_generation_integrity_v2 WHERE "
                              "workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
                              "WHERE workspace_id='%s') AND generation=1", result.workspace_id);
    ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
    ASSERT_EQ(count, 1);

    const char *fault_phases[] = {
        "user_after_nodes_before_bulk_finalize", "user_after_bulk_fts",
        "user_after_edges_before_bulk_finalize",
        "user_after_metadata",     "user_after_fts",       "user_after_graph_nodes",
        "user_after_graph_edges",  "user_after_node_vectors", "user_after_token_vectors",
        "user_after_integrity",    "user_before_commit",
    };
    for (size_t i = 0; i < sizeof(fault_phases) / sizeof(fault_phases[0]); i++) {
        cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE", fault_phases[i], 1);
        cbm_zova_workspace_generation_result_t failed = {0};
        ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &failed), -1);
        cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
        snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
                 "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
                 result.workspace_id);
        ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
        ASSERT_EQ(count, 2);
        snprintf(sql, sizeof(sql),
                 "SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id='%s'",
                 result.workspace_id);
        ASSERT_EQ(zova_scalar_int64(path, sql, &count), 0);
        ASSERT_EQ(count, 1);
        ASSERT_EQ(zova_scalar_int64(
                      path,
                      "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND "
                      "name='cbm_nodes_v1_fts_ai'",
                      &count),
                  0);
        ASSERT_EQ(count, 1);
        ASSERT_EQ(zova_scalar_int64(path, "SELECT count(*) FROM cbm_nodes_fts_v1_docsize",
                                    &count), 0);
        ASSERT_EQ(count, 2);
    }
    const CBMDumpNode reordered_nodes[] = {nodes[1], nodes[0]};
    const CBMDumpEdge reordered_edges[] = {edges[0]};
    const CBMDumpVector reordered_vectors[] = {vectors[1], vectors[0]};
    const CBMDumpTokenVec reordered_tokens[] = {tokens[1], tokens[0]};
    const cbm_zova_file_hash_input_t reordered_hashes[] = {hashes[1], hashes[0]};
    cbm_zova_workspace_generation_input_t reordered_input = input;
    reordered_input.nodes = reordered_nodes;
    reordered_input.edges = reordered_edges;
    reordered_input.node_vectors = reordered_vectors;
    reordered_input.token_vectors = reordered_tokens;
    reordered_input.file_hashes = reordered_hashes;
    cbm_zova_workspace_generation_result_t replacement = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &reordered_input, &replacement), 0);
    ASSERT_EQ(replacement.generation, 2);
    ASSERT_STR_EQ(replacement.metadata_sha256, result.metadata_sha256);
    ASSERT_STR_EQ(replacement.fts_sha256, result.fts_sha256);
    ASSERT_STR_EQ(replacement.topology_sha256, result.topology_sha256);
    ASSERT_STR_EQ(replacement.node_vector_sha256, result.node_vector_sha256);
    ASSERT_STR_EQ(replacement.token_vector_sha256, result.token_vector_sha256);

    /* A semantic property byte must affect only the metadata digest, while a
     * raw vector byte must affect only the corresponding vector digest. */
    CBMDumpNode changed_nodes[2];
    memcpy(changed_nodes, nodes, sizeof(changed_nodes));
    changed_nodes[1].properties = "{\"risk\":2}";
    int8_t changed_leaf_values[] = {1, 126};
    CBMDumpVector changed_vectors[2];
    memcpy(changed_vectors, vectors, sizeof(changed_vectors));
    changed_vectors[1].vector = (const uint8_t *)changed_leaf_values;
    cbm_zova_workspace_generation_input_t changed_input = input;
    changed_input.nodes = changed_nodes;
    changed_input.node_vectors = changed_vectors;
    cbm_zova_workspace_generation_result_t changed = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &changed_input, &changed), 0);
    ASSERT_EQ(changed.generation, 3);
    ASSERT_TRUE(strcmp(changed.metadata_sha256, replacement.metadata_sha256) != 0);
    ASSERT_STR_EQ(changed.fts_sha256, replacement.fts_sha256);
    ASSERT_STR_EQ(changed.topology_sha256, replacement.topology_sha256);
    ASSERT_TRUE(strcmp(changed.node_vector_sha256, replacement.node_vector_sha256) != 0);
    ASSERT_STR_EQ(changed.token_vector_sha256, replacement.token_vector_sha256);
    cbm_unlink(path);
    PASS();
}

TEST(zova_full_edge_publication_batches_128_rows) {
    enum { NODE_COUNT = 130, EDGE_COUNT = 129 };
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-edge-batch-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);

    CBMDumpNode nodes[NODE_COUNT];
    CBMDumpEdge edges[EDGE_COUNT];
    char names[NODE_COUNT][32];
    char qualified_names[NODE_COUNT][48];
    char file_paths[NODE_COUNT][32];
    memset(nodes, 0, sizeof(nodes));
    memset(edges, 0, sizeof(edges));
    for (int i = 0; i < NODE_COUNT; i++) {
        snprintf(names[i], sizeof(names[i]), "Node%d", i);
        snprintf(qualified_names[i], sizeof(qualified_names[i]), "edge_batch.Node%d", i);
        snprintf(file_paths[i], sizeof(file_paths[i]), "node_%d.c", i);
        nodes[i] = (CBMDumpNode){
            .id = i + 1,
            .project = "edge_batch",
            .label = "Function",
            .name = names[i],
            .qualified_name = qualified_names[i],
            .file_path = file_paths[i],
            .start_line = 1,
            .end_line = 1,
            .properties = "{}",
        };
    }
    for (int i = 0; i < EDGE_COUNT; i++) {
        edges[i] = (CBMDumpEdge){
            .id = i + 1,
            .project = "edge_batch",
            .source_id = i + 1,
            .target_id = i + 2,
            .type = "CALLS",
            .properties = "{}",
            .url_path = "",
            .local_name = "",
        };
    }
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/edge-batch",
        .project = "edge_batch",
        .indexed_at = "2026-07-17T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 2,
        .nodes = nodes,
        .node_count = NODE_COUNT,
        .edges = edges,
        .edge_count = EDGE_COUNT,
    };
    cbm_zova_workspace_generation_result_t result = {0};
    cbm_zova_publish_test_metrics_reset();
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &result), 0);
    cbm_zova_publish_test_metrics_t metrics = {0};
    cbm_zova_publish_test_metrics_get(&metrics);
    ASSERT_EQ(result.metadata_edges, EDGE_COUNT);
    ASSERT_EQ(result.graph_edges, EDGE_COUNT);
    ASSERT_EQ(metrics.canonical_edges_sql.rows, 0);
    ASSERT_EQ(metrics.canonical_edges_sql.bind_i64_calls, 0);
    ASSERT_EQ(metrics.canonical_edges_sql.bind_text_calls, 0);
    ASSERT_EQ(metrics.canonical_edges_sql.step_calls, 0);
    ASSERT_EQ(metrics.canonical_edges_sql.reset_calls, 0);
    ASSERT_EQ(metrics.canonical_edges_sql.clear_bindings_calls, 0);
    cbm_unlink(path);
    PASS();
}

TEST(zova_workspace_vector_search_uses_native_node_and_token_collections) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-native-vector-search-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)path);
    cbm_unlink(path);

    int8_t root_vec[768] = {0};
    int8_t leaf_vec[768] = {0};
    root_vec[0] = 127;
    leaf_vec[1] = 127;
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "native", .label = "Function", .name = "Root",
         .qualified_name = "native.Root", .file_path = "root.c", .properties = "{}"},
        {.id = 2, .project = "native", .label = "Function", .name = "Leaf",
         .qualified_name = "native.Leaf", .file_path = "leaf.c", .properties = "{}"},
        {.id = 3, .project = "native", .label = "Function", .name = "Twin",
         .qualified_name = "native.ZTwin", .file_path = "twin.c", .properties = "{}"},
    };
    const CBMDumpVector node_vectors[] = {
        {.node_id = 1, .project = "native", .vector = (const uint8_t *)root_vec,
         .vector_len = sizeof(root_vec)},
        {.node_id = 2, .project = "native", .vector = (const uint8_t *)leaf_vec,
         .vector_len = sizeof(leaf_vec)},
        {.node_id = 3, .project = "native", .vector = (const uint8_t *)root_vec,
         .vector_len = sizeof(root_vec)},
    };
    const CBMDumpTokenVec token_vectors[] = {
        {.id = 1, .project = "native", .token = "root", .vector = (const uint8_t *)root_vec,
         .vector_len = sizeof(root_vec), .idf = 1.0f},
        {.id = 2, .project = "native", .token = "leaf", .vector = (const uint8_t *)leaf_vec,
         .vector_len = sizeof(leaf_vec), .idf = 1.0f},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/native-vector-search",
        .project = "native",
        .indexed_at = "2026-07-15T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 768,
        .nodes = nodes,
        .node_count = 3,
        .node_vectors = node_vectors,
        .node_vector_count = 3,
        .token_vectors = token_vectors,
        .token_vector_count = 2,
    };
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &published), 0);

    int64_t forbidden = -1;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT count(*) FROM sqlite_master WHERE name IN "
                  "('cbm_node_vectors_compat_v1','cbm_token_vectors_compat_v1')",
                  &forbidden),
              0);
    ASSERT_EQ(forbidden, 0);

    int64_t root_node_key = 0;
    int64_t root_token_key = 0;
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT zova_node_key FROM cbm_nodes_v1 WHERE qualified_name='native.Root'",
                  &root_node_key),
              0);
    ASSERT_GT(root_node_key, 0);
    ASSERT_EQ(zova_scalar_int64(
                  path,
                  "SELECT token_key FROM cbm_token_vector_metadata_v1 WHERE token='root'",
                  &root_token_key),
              0);
    ASSERT_GT(root_token_key, 0);
    zova_database *native_db = NULL;
    zova_message native_error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path = path, .out_db = &native_db, .out_error_message = &native_error}),
              ZOVA_OK);
    zova_message_free(&native_error);
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    char token_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  published.workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 768,
                  node_collection, sizeof(node_collection)),
              0);
    ASSERT_EQ(cbm_zova_workspace_token_vector_collection_name(
                  published.workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 768,
                  token_collection, sizeof(token_collection)),
              0);
    char physical_id[32];
    snprintf(physical_id, sizeof(physical_id), "%lld", (long long)root_node_key);
    uint8_t exists = 0;
    ASSERT_EQ(zova_vector_exists(&(zova_vector_exists_request){
                  .db = native_db, .collection_name = node_collection,
                  .vector_id = physical_id, .out_exists = &exists}),
              ZOVA_OK);
    ASSERT_EQ(exists, 1);
    snprintf(physical_id, sizeof(physical_id), "%lld", (long long)root_token_key);
    exists = 0;
    ASSERT_EQ(zova_vector_exists(&(zova_vector_exists_request){
                  .db = native_db, .collection_name = token_collection,
                  .vector_id = physical_id, .out_exists = &exists}),
              ZOVA_OK);
    ASSERT_EQ(exists, 1);
    zova_database_close(native_db);

    cbm_zova_workspace_snapshot_t public_snapshot = {0};
    ASSERT_EQ(cbm_zova_repository_export_snapshot(
                  path, published.workspace_id, &public_snapshot),
              CBM_ZOVA_SNAPSHOT_OK);
    ASSERT_EQ(public_snapshot.node_vector_count, 3);
    ASSERT_EQ(public_snapshot.token_vector_count, 2);
    for (int i = 0; i < public_snapshot.node_vector_count; i++)
        ASSERT_TRUE(strncmp(public_snapshot.node_vector_ids[i], "n:v2:", 5) == 0);
    for (int i = 0; i < public_snapshot.token_vector_count; i++)
        ASSERT_TRUE(strncmp(public_snapshot.token_vector_ids[i], "t:v1:", 5) == 0);
    cbm_zova_workspace_snapshot_free(&public_snapshot);

    cbm_store_t *store = cbm_store_open_zova_workspace_query(path, published.workspace_id);
    ASSERT_NOT_NULL(store);
    const char *keywords[] = {"root"};
    cbm_vector_result_t *results = NULL;
    int result_count = 0;
    cbm_vector_search_metrics_t metrics = {0};
    ASSERT_EQ(cbm_store_vector_search_ex(store, "native", keywords, 1, 3, &results,
                                         &result_count, &metrics),
              CBM_STORE_OK);
    ASSERT_EQ(result_count, 3);
    ASSERT_STR_EQ(results[0].qualified_name, "native.Root");
    ASSERT_STR_EQ(results[0].name, "Root");
    ASSERT_STR_EQ(results[0].file_path, "root.c");
    ASSERT_STR_EQ(results[0].label, "Function");
    ASSERT_STR_EQ(results[1].qualified_name, "native.ZTwin");
    ASSERT_EQ(results[0].score, results[1].score);
    ASSERT_TRUE(results[1].score > results[2].score);
    ASSERT_TRUE(metrics.used_zova);
    cbm_store_free_vector_results(results, result_count);

    results = NULL;
    result_count = 0;
    memset(&metrics, 0, sizeof(metrics));
    ASSERT_EQ(cbm_store_vector_search_ex(store, "native", keywords, 1, 1, &results,
                                         &result_count, &metrics), CBM_STORE_OK);
    ASSERT_EQ(result_count, 1);
    ASSERT_STR_EQ(results[0].qualified_name, "native.Root");
    cbm_store_free_vector_results(results, result_count);

    const char *multi_keywords[] = {"root", "leaf"};
    results = NULL;
    result_count = 0;
    memset(&metrics, 0, sizeof(metrics));
    ASSERT_EQ(cbm_store_vector_search_ex(store, "native", multi_keywords, 2, 3, &results,
                                         &result_count, &metrics), CBM_STORE_OK);
    ASSERT_EQ(result_count, 3);
    ASSERT_TRUE(metrics.used_zova);
    ASSERT_TRUE(metrics.zova_native_multi_query);
    cbm_store_free_vector_results(results, result_count);
    cbm_store_close(store);

    sqlite3 *sql_db = NULL;
    ASSERT_EQ(sqlite3_open(path, &sql_db), SQLITE_OK);
    sqlite3_stmt *tamper = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(sql_db,
              "UPDATE cbm_workspace_index_state_v1 SET model_fingerprint=?1 "
              "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
              "WHERE workspace_id=?2)",
              -1, &tamper, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(tamper, 1, "missing_model", -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(tamper, 2, published.workspace_id, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(tamper), SQLITE_DONE);
    sqlite3_finalize(tamper);
    sqlite3_close(sql_db);
    store = cbm_store_open_zova_workspace_query(path, published.workspace_id);
    ASSERT_NOT_NULL(store);
    results = NULL;
    result_count = 0;
    ASSERT_EQ(cbm_store_vector_search_ex(store, "native", keywords, 1, 3, &results,
                                         &result_count, NULL), CBM_STORE_ERR);
    ASSERT_NULL(results);
    cbm_store_close(store);

    ASSERT_EQ(sqlite3_open(path, &sql_db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(sql_db,
              "UPDATE cbm_workspace_index_state_v1 SET model_fingerprint=?1,generation=999 "
              "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
              "WHERE workspace_id=?2)", -1, &tamper, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(tamper, 1, CBM_ZOVA_MODEL_FINGERPRINT, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(tamper, 2, published.workspace_id, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(tamper), SQLITE_DONE);
    sqlite3_finalize(tamper);
    sqlite3_close(sql_db);
    store = cbm_store_open_zova_workspace_query(path, published.workspace_id);
    ASSERT_NOT_NULL(store);
    memset(&metrics, 0, sizeof(metrics));
    ASSERT_EQ(cbm_store_vector_search_ex(store, "native", keywords, 1, 3, &results,
                                         &result_count, &metrics), CBM_STORE_ERR);
    ASSERT_TRUE(metrics.generation_mismatch);
    cbm_store_close(store);

    ASSERT_EQ(sqlite3_open(path, &sql_db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(sql_db,
              "UPDATE cbm_workspace_index_state_v1 SET generation=?1 "
              "WHERE workspace_key=(SELECT workspace_key FROM cbm_workspace_registry "
              "WHERE workspace_id=?2)",
              -1, &tamper, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int64(tamper, 1, published.generation), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(tamper, 2, published.workspace_id, -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(tamper), SQLITE_DONE);
    sqlite3_finalize(tamper);
    sqlite3_close(sql_db);

    const CBMDumpNode empty_nodes[] = {{
        .id = 1, .project = "empty", .label = "Function", .name = "Only",
        .qualified_name = "empty.Only", .file_path = "only.c", .properties = "{}"}};
    const CBMDumpTokenVec empty_tokens[] = {{
        .id = 1, .project = "empty", .token = "root", .vector = (const uint8_t *)root_vec,
        .vector_len = sizeof(root_vec), .idf = 1.0f}};
    cbm_zova_workspace_generation_input_t empty_input = input;
    empty_input.root_path = "/tmp/native-vector-search-empty";
    empty_input.project = "empty";
    empty_input.nodes = empty_nodes;
    empty_input.node_count = 1;
    empty_input.node_vectors = NULL;
    empty_input.node_vector_count = 0;
    empty_input.token_vectors = empty_tokens;
    empty_input.token_vector_count = 1;
    cbm_zova_workspace_generation_result_t empty_published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &empty_input, &empty_published), 0);
    store = cbm_store_open_zova_workspace_query(path, empty_published.workspace_id);
    ASSERT_NOT_NULL(store);
    results = NULL;
    result_count = -1;
    memset(&metrics, 0, sizeof(metrics));
    ASSERT_EQ(cbm_store_vector_search_ex(store, "empty", keywords, 1, 3, &results,
                                         &result_count, &metrics), CBM_STORE_OK);
    ASSERT_EQ(result_count, 0);
    ASSERT_NULL(results);
    ASSERT_TRUE(metrics.used_zova);
    cbm_store_close(store);
    cbm_unlink(path);
    PASS();
}

/* Exercise the public native-object APIs inside a caller-owned transaction.
 * This deliberately uses a separate reader handle and only public graph,
 * vector, and SQL operations; it must not depend on Zova private tables. */
static int zova_tx_exec(zova_database *db, const char *sql) {
    zova_database_exec_request req = {.db = db, .sql = sql};
    zova_status status = zova_database_exec(&req);
    if (status != ZOVA_OK) {
        fprintf(stderr, "zova tx exec failed status=%s sql=%s err=%s\n", zova_status_name(status),
                sql, zova_database_last_error_message(db));
    }
    return status == ZOVA_OK ? 0 : -1;
}

static int zova_tx_scalar_i64(zova_database *db, const char *sql, int64_t *out) {
    if (!db || !sql || !out) return -1;
    zova_statement *stmt = NULL;
    if (zova_database_prepare(&(zova_database_prepare_request){
            .db = db, .sql = sql, .out_statement = &stmt}) != ZOVA_OK || !stmt) {
        return -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    int rc = zova_statement_step(&(zova_statement_step_request){
        .statement = stmt, .out_result = &step}) == ZOVA_OK && step == ZOVA_STEP_ROW
                 ? 0 : -1;
    if (rc == 0) {
        rc = zova_statement_column_int64(&(zova_statement_column_int64_request){
            .statement = stmt, .index = 0, .out_value = out}) == ZOVA_OK ? 0 : -1;
    }
    (void)zova_statement_finalize(stmt);
    return rc;
}

static int zova_tx_graph_count(zova_database *db, const char *graph_name, uint64_t expected) {
    zova_graph_info info = {0};
    zova_status status = zova_graph_info_get(&(zova_graph_info_get_request){
        .db = db, .name = graph_name, .out_info = &info});
    int rc = status == ZOVA_OK && info.node_count == expected ? 0 : -1;
    zova_graph_info_free(&info);
    return rc;
}

static int zova_tx_vector_count(zova_database *db, const char *collection, uint64_t expected) {
    zova_vector_collection_info info = {0};
    zova_status status = zova_vector_collection_info_get(&(zova_vector_collection_info_get_request){
        .db = db, .name = collection, .out_info = &info});
    int rc = status == ZOVA_OK && info.vector_count == expected ? 0 : -1;
    zova_vector_collection_info_free(&info);
    return rc;
}

static int zova_tx_commit_checked(zova_database *db, const char *who) {
    zova_status status = zova_database_commit(&(zova_database_simple_request){.db = db});
    if (status != ZOVA_OK) {
        fprintf(stderr, "zova tx commit failed who=%s status=%s err=%s\n", who,
                zova_status_name(status), zova_database_last_error_message(db));
    }
    return status == ZOVA_OK ? 0 : -1;
}

static int zova_tx_graph_is(zova_database *db, const char *expected) {
    zova_graph_neighbor_results results = {0};
    zova_graph_neighbors_request req = {
        .db = db,
        .graph_name = "tx_graph",
        .node_id = "A",
        .direction = ZOVA_GRAPH_NEIGHBOR_OUTGOING,
        .edge_type = "CALLS",
        .limit = 8,
        .out_results = &results,
    };
    zova_status status = zova_graph_neighbors(&req);
    int rc = status == ZOVA_OK && results.len == 1 && results.items &&
                     results.items[0].node_id && strcmp(results.items[0].node_id, expected) == 0
                 ? 0
                 : -1;
    if (rc != 0) {
        fprintf(stderr, "zova tx graph mismatch expected=%s status=%s len=%zu\n", expected,
                zova_status_name(status), results.len);
    }
    zova_graph_neighbor_results_free(&results);
    return rc;
}

static int zova_tx_vector_is(zova_database *db, const char *expected) {
    const int8_t query_values[] = {127, 0};
    zova_vector_values query = {
        .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
        .i8_values = query_values,
        .values_len = 2,
    };
    zova_vector_search_results results = {0};
    zova_vector_search_request req = {
        .db = db,
        .collection_name = "tx_vectors",
        .query = query,
        .limit = 8,
        .out_results = &results,
    };
    zova_status status = zova_vector_search(&req);
    int rc = status == ZOVA_OK && results.len == 1 && results.items &&
                     results.items[0].id && strcmp(results.items[0].id, expected) == 0
                 ? 0
                 : -1;
    if (rc != 0) {
        fprintf(stderr, "zova tx vector mismatch expected=%s status=%s len=%zu\n", expected,
                zova_status_name(status), results.len);
    }
    zova_vector_search_results_free(&results);
    return rc;
}

static int zova_tx_fts_is(zova_database *db, const char *expected) {
    zova_statement *stmt = NULL;
    zova_database_prepare_request prepare = {
        .db = db,
        .sql = "SELECT rowid FROM tx_fts WHERE tx_fts MATCH ?1 LIMIT 1",
        .out_statement = &stmt,
    };
    zova_status prepare_status = zova_database_prepare(&prepare);
    int rc = prepare_status == ZOVA_OK && stmt ? 0 : -1;
    if (rc == 0) {
        zova_statement_bind_text_request bind = {
            .statement = stmt,
            .index = 1,
            .data = (const uint8_t *)expected,
            .len = strlen(expected),
        };
        rc = zova_statement_bind_text(&bind) == ZOVA_OK ? 0 : -1;
    }
    zova_step_result step = ZOVA_STEP_DONE;
    if (rc == 0) {
        zova_statement_step_request step_req = {.statement = stmt, .out_result = &step};
        rc = zova_statement_step(&step_req) == ZOVA_OK && step == ZOVA_STEP_ROW ? 0 : -1;
    }
    if (rc == 0) {
        int64_t rowid = 0;
        zova_statement_column_int64_request column = {
            .statement = stmt,
            .index = 0,
            .out_value = &rowid,
        };
        rc = zova_statement_column_int64(&column) == ZOVA_OK && rowid == 1 ? 0 : -1;
    }
    if (stmt) {
        (void)zova_statement_finalize(stmt);
    }
    if (rc != 0) {
        fprintf(stderr, "zova tx fts mismatch expected=%s prepare=%s\n", expected,
                zova_status_name(prepare_status));
    }
    return rc;
}

static int zova_tx_seed(zova_database *db, const char *node_id, const char *term) {
    if (zova_database_begin_immediate(&(zova_database_simple_request){.db = db}) != ZOVA_OK) {
        return -1;
    }
    int rc = 0;
    zova_graph_create_request graph_create = {.db = db, .name = "tx_graph"};
    if (zova_graph_create(&graph_create) != ZOVA_OK) {
        rc = -1;
    }
    const zova_graph_node_input nodes[] = {
        {.graph_name = "tx_graph", .node_id = "A", .kind = "Function"},
        {.graph_name = "tx_graph", .node_id = node_id, .kind = "Function"},
    };
    if (rc == 0) {
        zova_graph_node_put_many_request put_nodes = {
            .db = db,
            .nodes = nodes,
            .nodes_len = sizeof(nodes) / sizeof(nodes[0]),
        };
        rc = zova_graph_node_put_many(&put_nodes) == ZOVA_OK ? 0 : -1;
    }
    const zova_graph_edge_input edges[] = {
        {.graph_name = "tx_graph", .from_node_id = "A", .edge_type = "CALLS", .to_node_id = node_id},
    };
    if (rc == 0) {
        zova_graph_edge_put_many_request put_edges = {
            .db = db,
            .edges = edges,
            .edges_len = sizeof(edges) / sizeof(edges[0]),
        };
        rc = zova_graph_edge_put_many(&put_edges) == ZOVA_OK ? 0 : -1;
    }
    zova_vector_collection_create_request create_collection = {
        .db = db,
        .name = "tx_vectors",
        .options = {
            .dimensions = 2,
            .metric = ZOVA_VECTOR_METRIC_COSINE,
            .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
        },
    };
    if (rc == 0) {
        rc = zova_vector_collection_create(&create_collection) == ZOVA_OK ? 0 : -1;
    }
    const int8_t vector_values[] = {127, 0};
    const zova_vector_input vectors[] = {
        {.id = node_id,
         .values = {.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                    .i8_values = vector_values,
                    .values_len = 2}},
    };
    if (rc == 0) {
        zova_vector_put_many_request put_vectors = {
            .db = db,
            .collection_name = "tx_vectors",
            .vectors = vectors,
            .vectors_len = sizeof(vectors) / sizeof(vectors[0]),
        };
        rc = zova_vector_put_many(&put_vectors) == ZOVA_OK ? 0 : -1;
    }
    if (rc == 0) {
        char sql[256];
        if (snprintf(sql, sizeof(sql),
                     "CREATE VIRTUAL TABLE tx_fts USING fts5(term, content='');"
                     "INSERT INTO tx_fts(rowid,term) VALUES(1,'%s');",
                     term) >= (int)sizeof(sql) || zova_tx_exec(db, sql) != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        rc = zova_database_commit(&(zova_database_simple_request){.db = db}) == ZOVA_OK ? 0 : -1;
    } else {
        (void)zova_database_rollback(&(zova_database_simple_request){.db = db});
    }
    return rc;
}

TEST(zova_graph_vector_objects_obey_caller_transaction) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-zova-tx-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);
    zova_database *writer = NULL;
    zova_database *reader = NULL;
    zova_message error = {0};
    int rc = -1;
    zova_database_open_request create = {
        .path = path,
        .out_db = &writer,
        .out_error_message = &error,
    };
    if (zova_database_create(&create) != ZOVA_OK || !writer) {
        goto done;
    }
    zova_message_free(&error);
    error = (zova_message){0};
    if (zova_tx_exec(writer, "PRAGMA journal_mode=WAL") != 0) {
        goto done;
    }
    if (zova_tx_seed(writer, "Old", "oldterm") != 0) {
        goto done;
    }
    zova_database_open_request open_reader = {
        .path = path,
        .out_db = &reader,
        .out_error_message = &error,
    };
    if (zova_database_open(&open_reader) != ZOVA_OK || !reader) {
        goto done;
    }
    zova_message_free(&error);
    error = (zova_message){0};

    if (zova_database_exec(&(zova_database_exec_request){.db = reader, .sql = "BEGIN"}) != ZOVA_OK ||
        zova_tx_graph_is(reader, "Old") != 0 || zova_tx_vector_is(reader, "Old") != 0 ||
        zova_tx_fts_is(reader, "oldterm") != 0 ||
        zova_database_begin_immediate(&(zova_database_simple_request){.db = writer}) != ZOVA_OK) {
        goto done;
    }
    /* The replacement helper starts its own transaction; perform the same
     * native replacement inline here because this phase is already inside the
     * caller-owned writer transaction. */
    if (zova_graph_delete(&(zova_graph_delete_request){.db = writer, .name = "tx_graph"}) != ZOVA_OK ||
        zova_graph_create(&(zova_graph_create_request){.db = writer, .name = "tx_graph"}) != ZOVA_OK) {
        goto done_writer;
    }
    const zova_graph_node_input new_nodes[] = {
        {.graph_name = "tx_graph", .node_id = "A", .kind = "Function"},
        {.graph_name = "tx_graph", .node_id = "New", .kind = "Function"},
    };
    const zova_graph_edge_input new_edges[] = {
        {.graph_name = "tx_graph", .from_node_id = "A", .edge_type = "CALLS", .to_node_id = "New"},
    };
    const int8_t new_vector_values[] = {127, 0};
    const zova_vector_input new_vectors[] = {
        {.id = "New",
         .values = {.element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8,
                    .i8_values = new_vector_values,
                    .values_len = 2}},
    };
    if (zova_graph_node_put_many(&(zova_graph_node_put_many_request){
                                     .db = writer,
                                     .nodes = new_nodes,
                                     .nodes_len = sizeof(new_nodes) / sizeof(new_nodes[0]),
                                 }) != ZOVA_OK ||
        zova_graph_edge_put_many(&(zova_graph_edge_put_many_request){
                                     .db = writer,
                                     .edges = new_edges,
                                     .edges_len = sizeof(new_edges) / sizeof(new_edges[0]),
                                 }) != ZOVA_OK ||
        zova_vector_collection_delete(&(zova_vector_collection_delete_request){
            .db = writer,
            .name = "tx_vectors",
        }) != ZOVA_OK ||
        zova_vector_collection_create(&(zova_vector_collection_create_request){
            .db = writer,
            .name = "tx_vectors",
            .options = {.dimensions = 2,
                        .metric = ZOVA_VECTOR_METRIC_COSINE,
                        .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8},
        }) != ZOVA_OK ||
        zova_vector_put_many(&(zova_vector_put_many_request){
            .db = writer,
            .collection_name = "tx_vectors",
            .vectors = new_vectors,
            .vectors_len = sizeof(new_vectors) / sizeof(new_vectors[0]),
        }) != ZOVA_OK ||
        zova_tx_exec(writer, "DROP TABLE tx_fts") != 0 ||
        zova_tx_exec(writer,
                     "CREATE VIRTUAL TABLE tx_fts USING fts5(term, content='');"
                     "INSERT INTO tx_fts(rowid,term) VALUES(1,'newterm');") != 0) {
        goto done_writer;
    }
    /* The reader's open transaction must continue to expose the old snapshot. */
    if (zova_tx_graph_is(reader, "Old") != 0 || zova_tx_vector_is(reader, "Old") != 0 ||
        zova_tx_fts_is(reader, "oldterm") != 0) {
        goto done_writer;
    }
    if (zova_database_rollback(&(zova_database_simple_request){.db = writer}) != ZOVA_OK ||
        zova_tx_commit_checked(reader, "reader-after-rollback") != 0) {
        goto done;
    }
    /* Reopen after rollback so the post-rollback assertion is made from a
     * fresh reader snapshot, not merely the prior transaction handle. */
    (void)zova_database_close(reader);
    reader = NULL;
    zova_message_free(&error);
    error = (zova_message){0};
    if (zova_database_open(&open_reader) != ZOVA_OK || !reader ||
        zova_tx_graph_is(reader, "Old") != 0 || zova_tx_vector_is(reader, "Old") != 0 ||
        zova_tx_fts_is(reader, "oldterm") != 0) {
        goto done;
    }

    if (zova_database_exec(&(zova_database_exec_request){.db = reader, .sql = "BEGIN"}) != ZOVA_OK ||
        zova_tx_graph_is(reader, "Old") != 0 || zova_tx_vector_is(reader, "Old") != 0 ||
        zova_tx_fts_is(reader, "oldterm") != 0 ||
        zova_database_begin_immediate(&(zova_database_simple_request){.db = writer}) != ZOVA_OK) {
        goto done;
    }
    if (zova_graph_delete(&(zova_graph_delete_request){.db = writer, .name = "tx_graph"}) != ZOVA_OK ||
        zova_graph_create(&(zova_graph_create_request){.db = writer, .name = "tx_graph"}) != ZOVA_OK ||
        zova_graph_node_put_many(&(zova_graph_node_put_many_request){
            .db = writer,
            .nodes = new_nodes,
            .nodes_len = sizeof(new_nodes) / sizeof(new_nodes[0]),
        }) != ZOVA_OK ||
        zova_graph_edge_put_many(&(zova_graph_edge_put_many_request){
            .db = writer,
            .edges = new_edges,
            .edges_len = sizeof(new_edges) / sizeof(new_edges[0]),
        }) != ZOVA_OK ||
        zova_vector_collection_delete(&(zova_vector_collection_delete_request){
            .db = writer,
            .name = "tx_vectors",
        }) != ZOVA_OK ||
        zova_vector_collection_create(&(zova_vector_collection_create_request){
            .db = writer,
            .name = "tx_vectors",
            .options = {.dimensions = 2,
                        .metric = ZOVA_VECTOR_METRIC_COSINE,
                        .element_type = ZOVA_VECTOR_ELEMENT_TYPE_I8},
        }) != ZOVA_OK ||
        zova_vector_put_many(&(zova_vector_put_many_request){
            .db = writer,
            .collection_name = "tx_vectors",
            .vectors = new_vectors,
            .vectors_len = sizeof(new_vectors) / sizeof(new_vectors[0]),
        }) != ZOVA_OK ||
        zova_tx_exec(writer, "DROP TABLE tx_fts") != 0 ||
        zova_tx_exec(writer,
                     "CREATE VIRTUAL TABLE tx_fts USING fts5(term, content='');"
                     "INSERT INTO tx_fts(rowid,term) VALUES(1,'newterm');") != 0 ||
        zova_tx_graph_is(reader, "Old") != 0 || zova_tx_vector_is(reader, "Old") != 0 ||
        zova_tx_fts_is(reader, "oldterm") != 0 ||
        zova_tx_commit_checked(writer, "writer-replacement") != 0 ||
        zova_tx_commit_checked(reader, "reader-replacement") != 0) {
        goto done_writer;
    }
    /* A new reader snapshot observes the committed generation atomically. */
    zova_database_close(reader);
    reader = NULL;
    if (zova_database_open(&open_reader) != ZOVA_OK || !reader || zova_tx_graph_is(reader, "New") != 0 ||
        zova_tx_vector_is(reader, "New") != 0 || zova_tx_fts_is(reader, "newterm") != 0) {
        goto done;
    }
    rc = 0;
    goto done;

done_writer:
    (void)zova_database_rollback(&(zova_database_simple_request){.db = writer});
done:
    zova_message_free(&error);
    if (reader) {
        (void)zova_database_close(reader);
    }
    if (writer) {
        (void)zova_database_close(writer);
    }
    cbm_unlink(path);
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(zova_atomic_publisher_reader_snapshot_is_generation_consistent) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-zova-reader-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)path);
    cbm_unlink(path);
    const int8_t root_values[] = {127, 0};
    const int8_t leaf_values[] = {0, 127};
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "reader", .label = "Function", .name = "Root",
         .qualified_name = "reader.Root", .file_path = "root.c", .properties = "{}"},
        {.id = 2, .project = "reader", .label = "Function", .name = "Leaf",
         .qualified_name = "reader.Leaf", .file_path = "leaf.c", .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1, .project = "reader", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{}", .url_path = "", .local_name = ""},
    };
    const CBMDumpVector vectors[] = {
        {.node_id = 1, .project = "reader", .vector = (const uint8_t *)root_values,
         .vector_len = 2},
        {.node_id = 2, .project = "reader", .vector = (const uint8_t *)leaf_values,
         .vector_len = 2},
    };
    const CBMDumpTokenVec tokens[] = {
        {.id = 1, .project = "reader", .token = "root",
         .vector = (const uint8_t *)root_values, .vector_len = 2, .idf = 1.0f},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/reader-snapshot",
        .project = "reader",
        .indexed_at = "2026-07-12T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 2,
        .nodes = nodes,
        .node_count = 2,
        .edges = edges,
        .edge_count = 1,
        .node_vectors = vectors,
        .node_vector_count = 2,
        .token_vectors = tokens,
        .token_vector_count = 1,
    };
    cbm_zova_workspace_generation_result_t first = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &first), 0);

    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    char node_collection[CBM_ZOVA_WORKSPACE_ID_MAX + 128];
    ASSERT_EQ(cbm_zova_workspace_graph_name(first.workspace_id, graph_name, sizeof(graph_name)), 0);
    ASSERT_EQ(cbm_zova_workspace_node_vector_collection_name(
                  first.workspace_id, CBM_ZOVA_MODEL_FINGERPRINT, 2,
                  node_collection, sizeof(node_collection)), 0);

    const CBMDumpNode replacement_nodes[] = {nodes[0]};
    const CBMDumpVector replacement_vectors[] = {vectors[0]};
    cbm_zova_workspace_generation_input_t replacement_input = input;
    replacement_input.nodes = replacement_nodes;
    replacement_input.node_count = 1;
    replacement_input.edges = NULL;
    replacement_input.edge_count = 0;
    replacement_input.node_vectors = replacement_vectors;
    replacement_input.node_vector_count = 1;
    replacement_input.token_vectors = NULL;
    replacement_input.token_vector_count = 0;

    zova_database *writer = NULL;
    zova_database *reader = NULL;
    zova_message writer_error = {0};
    zova_message reader_error = {0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
        .path = path, .out_db = &writer, .out_error_message = &writer_error}), ZOVA_OK);
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
        .path = path, .out_db = &reader, .out_error_message = &reader_error}), ZOVA_OK);
    zova_message_free(&writer_error);
    zova_message_free(&reader_error);
    ASSERT_EQ(zova_tx_exec(reader, "BEGIN"), 0);
    int64_t count = 0;
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             first.workspace_id);
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 2);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_fts_v1_docsize");
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 2);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             first.workspace_id);
    ASSERT_EQ(zova_tx_graph_count(reader, graph_name, 2), 0);
    ASSERT_EQ(zova_tx_vector_count(reader, node_collection, 2), 0);

    ASSERT_EQ(zova_database_begin_immediate(&(zova_database_simple_request){.db = writer}), ZOVA_OK);
    cbm_zova_workspace_generation_result_t second = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace_tx(writer, &replacement_input, &second), 0);
    ASSERT_EQ(second.generation, 2);
    ASSERT_EQ(zova_tx_scalar_i64(writer, "PRAGMA cache_size", &count), 0);
    ASSERT_EQ(count, -65536);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             first.workspace_id);
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 2);
    ASSERT_EQ(zova_tx_graph_count(reader, graph_name, 2), 0);
    ASSERT_EQ(zova_tx_vector_count(reader, node_collection, 2), 0);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_fts_v1_docsize");
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 2);

    ASSERT_EQ(zova_tx_commit_checked(writer, "reader_snapshot_writer"), 0);
    /* The already-open reader transaction keeps generation 1 even after the
     * writer commits. */
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
             "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
             first.workspace_id);
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 2);
    ASSERT_EQ(zova_tx_commit_checked(reader, "reader_snapshot_reader"), 0);
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 1);
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM cbm_nodes_fts_v1_docsize");
    ASSERT_EQ(zova_tx_scalar_i64(reader, sql, &count), 0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(zova_tx_graph_count(reader, graph_name, 1), 0);
    ASSERT_EQ(zova_tx_vector_count(reader, node_collection, 1), 0);

    (void)zova_database_close(reader);
    (void)zova_database_close(writer);
    cbm_unlink(path);
    PASS();
}

static int delta_test_node_index(const cbm_zova_workspace_snapshot_t *snapshot,
                                 const char *stable_id) {
    for (int i = 0; i < snapshot->node_count; i++)
        if (strcmp(snapshot->node_stable_ids[i], stable_id) == 0) return i;
    return -1;
}

static int delta_test_snapshot_from_model(const cbm_zova_publish_model_t *model,
                                          cbm_zova_workspace_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->generation = 1;
    snapshot->integrity = *cbm_zova_publish_model_digests(model);
    snapshot->hydrated_components = CBM_ZOVA_SNAPSHOT_COMPONENT_ALL;
    snapshot->node_count = cbm_zova_publish_model_node_count(model);
    snapshot->edge_count = cbm_zova_publish_model_edge_count(model);
    snapshot->node_vector_count = cbm_zova_publish_model_node_vector_count(model);
    snapshot->token_vector_count = cbm_zova_publish_model_token_vector_count(model);
    snapshot->file_hash_count = cbm_zova_publish_model_file_hash_count(model);
#define DELTA_TEST_ALLOC(field, count) \
    do { \
        snapshot->field = (count) ? calloc((size_t)(count), sizeof(*snapshot->field)) : NULL; \
        if ((count) && !snapshot->field) return -1; \
    } while (0)
    DELTA_TEST_ALLOC(nodes, snapshot->node_count);
    DELTA_TEST_ALLOC(node_stable_ids, snapshot->node_count);
    DELTA_TEST_ALLOC(node_source_ordinals, snapshot->node_count);
    DELTA_TEST_ALLOC(edges, snapshot->edge_count);
    DELTA_TEST_ALLOC(edge_ids, snapshot->edge_count);
    snapshot->topology_edge_count = cbm_zova_publish_model_topology_count(model);
    DELTA_TEST_ALLOC(topology_edges, snapshot->topology_edge_count);
    DELTA_TEST_ALLOC(node_vectors, snapshot->node_vector_count);
    DELTA_TEST_ALLOC(node_vector_ids, snapshot->node_vector_count);
    DELTA_TEST_ALLOC(token_vectors, snapshot->token_vector_count);
    DELTA_TEST_ALLOC(token_vector_ids, snapshot->token_vector_count);
    DELTA_TEST_ALLOC(file_hashes, snapshot->file_hash_count);
#undef DELTA_TEST_ALLOC
    for (int i = 0; i < snapshot->node_count; i++) {
        const cbm_zova_publish_node_t *node = cbm_zova_publish_model_node_at(model, i);
        snapshot->nodes[i] = *node->source;
        snapshot->nodes[i].id = i + 1;
        snapshot->node_stable_ids[i] = (char *)node->stable_id;
        snapshot->node_source_ordinals[i] = node->source_ordinal;
    }
    for (int i = 0; i < snapshot->edge_count; i++) {
        const cbm_zova_publish_edge_t *edge = cbm_zova_publish_model_edge_at(model, i);
        snapshot->edges[i] = *edge->source;
        snapshot->edges[i].id = i + 1;
        snapshot->edges[i].source_id =
            delta_test_node_index(snapshot, edge->source_stable_id) + 1;
        snapshot->edges[i].target_id =
            delta_test_node_index(snapshot, edge->target_stable_id) + 1;
        snapshot->edge_ids[i] = (char *)edge->edge_id;
    }
    for (int i = 0; i < snapshot->topology_edge_count; i++) {
        const cbm_zova_publish_edge_t *edge = cbm_zova_publish_model_topology_at(model, i);
        snapshot->topology_edges[i] = (cbm_zova_snapshot_topology_edge_t){
            .source_stable_id=(char *)edge->source_stable_id,
            .edge_type=(char *)edge->source->type,
            .target_stable_id=(char *)edge->target_stable_id};
    }
    for (int i = 0; i < snapshot->node_vector_count; i++) {
        const cbm_zova_publish_node_vector_t *vector =
            cbm_zova_publish_model_node_vector_at(model, i);
        snapshot->node_vectors[i] = *vector->source;
        snapshot->node_vectors[i].node_id =
            delta_test_node_index(snapshot, vector->stable_id) + 1;
        snapshot->node_vector_ids[i] = (char *)vector->stable_id;
    }
    for (int i = 0; i < snapshot->token_vector_count; i++) {
        const cbm_zova_publish_token_vector_t *vector =
            cbm_zova_publish_model_token_vector_at(model, i);
        snapshot->token_vectors[i] = *vector->source;
        snapshot->token_vector_ids[i] = (char *)vector->token_id;
    }
    for (int i = 0; i < snapshot->file_hash_count; i++)
        snapshot->file_hashes[i] = *cbm_zova_publish_model_file_hash_at(model, i);
    snapshot->project_summary = cbm_zova_publish_model_input(model)->project_summary;
    return 0;
}

static void delta_test_snapshot_free(cbm_zova_workspace_snapshot_t *snapshot);

TEST(zova_delta_digest_planner_selects_only_changed_components) {
    static const uint8_t node_bytes[] = {1, 2, 3, 4};
    static const uint8_t token_bytes[] = {4, 3, 2, 1};
    CBMDumpNode nodes[] = {
        {.id=1,.project="digest-plan",.label="Function",.name="Alpha",
         .qualified_name="digest_plan.Alpha",.file_path="src/a.c",.start_line=1,
         .end_line=4,.properties="{}"},
        {.id=2,.project="digest-plan",.label="Function",.name="Beta",
         .qualified_name="digest_plan.Beta",.file_path="src/b.c",.start_line=5,
         .end_line=8,.properties="{}"},
    };
    CBMDumpEdge edge = {.id=1,.project="digest-plan",.source_id=1,.target_id=2,
        .type="CALLS",.properties="{}",.url_path="",.local_name="beta"};
    CBMDumpVector node_vector = {.node_id=1,.project="digest-plan",
        .vector=node_bytes,.vector_len=4};
    CBMDumpTokenVec token_vector = {.id=1,.project="digest-plan",.token="alpha",
        .vector=token_bytes,.vector_len=4,.idf=1.0f};
    cbm_zova_workspace_generation_input_t input = {
        .root_path="/tmp/digest-plan",.project="digest-plan",
        .indexed_at="2026-07-16T00:00:00Z",.model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions=4,.nodes=nodes,.node_count=2,.edges=&edge,.edge_count=1,
        .node_vectors=&node_vector,.node_vector_count=1,
        .token_vectors=&token_vector,.token_vector_count=1,
    };
    cbm_zova_publish_model_t *model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build("w1_11111111111111111111111111111111",
                                           &input, &model), 0);
    cbm_zova_workspace_snapshot_t snapshot = {0};
    ASSERT_EQ(delta_test_snapshot_from_model(model, &snapshot), 0);

    cbm_zova_snapshot_components_t required = UINT32_MAX;
    ASSERT_EQ(cbm_zova_workspace_delta_required_components(&snapshot, model, &required), 0);
    ASSERT_EQ(required, CBM_ZOVA_SNAPSHOT_COMPONENT_NONE);

    snapshot.integrity.topology_sha256[0] =
        snapshot.integrity.topology_sha256[0] == '0' ? '1' : '0';
    ASSERT_EQ(cbm_zova_workspace_delta_required_components(&snapshot, model, &required), 0);
    ASSERT_EQ(required, CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY);
    snapshot.integrity = *cbm_zova_publish_model_digests(model);

    snapshot.integrity.node_vector_sha256[0] =
        snapshot.integrity.node_vector_sha256[0] == '0' ? '1' : '0';
    ASSERT_EQ(cbm_zova_workspace_delta_required_components(&snapshot, model, &required), 0);
    ASSERT_EQ(required, CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS);
    snapshot.integrity = *cbm_zova_publish_model_digests(model);

    snapshot.integrity.token_vector_sha256[0] =
        snapshot.integrity.token_vector_sha256[0] == '0' ? '1' : '0';
    ASSERT_EQ(cbm_zova_workspace_delta_required_components(&snapshot, model, &required), 0);
    ASSERT_EQ(required, CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS);

    cbm_zova_workspace_generation_input_t no_vectors = input;
    no_vectors.node_vectors = NULL;
    no_vectors.node_vector_count = 0;
    no_vectors.token_vectors = NULL;
    no_vectors.token_vector_count = 0;
    cbm_zova_publish_model_t *reused = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build("w1_11111111111111111111111111111111",
                                           &no_vectors, &reused), 0);
    ASSERT_EQ(cbm_zova_publish_model_reuse_vector_digests(
                  reused, &snapshot.integrity,
                  CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS |
                      CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS), 0);
    ASSERT_EQ(cbm_zova_workspace_delta_required_components(&snapshot, reused, &required), 0);
    ASSERT_EQ(required, CBM_ZOVA_SNAPSHOT_COMPONENT_NONE);
    ASSERT_EQ((int)cbm_zova_publish_model_digests(reused)->node_vectors, 1);
    ASSERT_EQ((int)cbm_zova_publish_model_digests(reused)->token_vectors, 1);
    cbm_zova_publish_model_free(reused);

    snapshot.integrity = *cbm_zova_publish_model_digests(model);
    snapshot.integrity.topology_sha256[0] = 'G';
    ASSERT_TRUE(cbm_zova_workspace_delta_required_components(&snapshot, model, &required) != 0);

    delta_test_snapshot_free(&snapshot);
    cbm_zova_publish_model_free(model);
    PASS();
}

static void delta_test_snapshot_free(cbm_zova_workspace_snapshot_t *snapshot) {
    free(snapshot->nodes); free(snapshot->node_stable_ids); free(snapshot->node_source_ordinals);
    free(snapshot->edges); free(snapshot->edge_ids);
    free(snapshot->topology_edges);
    free(snapshot->node_vectors); free(snapshot->node_vector_ids);
    free(snapshot->token_vectors); free(snapshot->token_vector_ids);
    free(snapshot->file_hashes);
    memset(snapshot, 0, sizeof(*snapshot));
}

TEST(zova_workspace_delta_is_exact_and_deterministic) {
    static const uint8_t node_bytes[] = {1, 2, 3, 4};
    static const uint8_t changed_node_bytes[] = {1, 9, 3, 4};
    static const uint8_t token_bytes[] = {4, 3, 2, 1};
    CBMDumpNode base_nodes[] = {
        {.id=1,.project="delta",.label="Function",.name="AlphaNode",
         .qualified_name="delta.AlphaNode",.file_path="src/a.c",.start_line=1,
         .end_line=4,.properties="{\"rank\":1}"},
        {.id=2,.project="delta",.label="Function",.name="BetaNode",
         .qualified_name="delta.BetaNode",.file_path="src/b.c",.start_line=5,
         .end_line=8,.properties="{\"rank\":2}"},
    };
    CBMDumpEdge base_edge = {
        .id=1,.project="delta",.source_id=1,.target_id=2,.type="CALLS",
        .properties="{\"line\":2}",.url_path="",.local_name="beta"};
    CBMDumpVector base_vector = {
        .node_id=1,.project="delta",.vector=node_bytes,.vector_len=4};
    CBMDumpTokenVec base_token = {
        .id=1,.project="delta",.token="alpha",.vector=token_bytes,.vector_len=4,.idf=1.5f};
    cbm_zova_file_hash_input_t base_hash = {
        .file_path="src/a.c",.content_hash="one",.mtime_ns=1,.size_bytes=10};
    cbm_zova_workspace_generation_input_t base = {
        .root_path="/tmp/delta",.project="delta",.indexed_at="2026-07-15T00:00:00Z",
        .model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,.vector_dimensions=4,
        .nodes=base_nodes,.node_count=2,.edges=&base_edge,.edge_count=1,
        .node_vectors=&base_vector,.node_vector_count=1,
        .token_vectors=&base_token,.token_vector_count=1,
        .file_hashes=&base_hash,.file_hash_count=1,
        .project_summary={.present=true,.summary="before",.source_hash="one",
                          .created_at="created",.updated_at="before"},
    };
    const char *workspace_id = "w1_11111111111111111111111111111111";
    cbm_zova_publish_model_t *before_model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &base, &before_model), 0);
    cbm_zova_workspace_snapshot_t snapshot = {0};
    ASSERT_EQ(delta_test_snapshot_from_model(before_model, &snapshot), 0);

    cbm_zova_workspace_delta_t *delta = NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot, before_model, &delta), 0);
    cbm_zova_workspace_delta_metrics_t metrics = {0};
    cbm_zova_workspace_delta_metrics(delta, &metrics);
    ASSERT_EQ(metrics.node_inserts + metrics.node_updates + metrics.node_deletes, 0);
    ASSERT_EQ(metrics.edge_inserts + metrics.edge_deletes, 0);
    ASSERT_EQ(metrics.node_vector_upserts + metrics.node_vector_deletes, 0);
    ASSERT_EQ(metrics.token_vector_upserts + metrics.token_vector_deletes, 0);
    ASSERT_EQ(metrics.file_hash_upserts + metrics.file_hash_deletes, 0);
    ASSERT_FALSE(cbm_zova_workspace_delta_replaces_summary(delta));
    cbm_zova_workspace_delta_free(delta);

    snapshot.hydrated_components = CBM_ZOVA_SNAPSHOT_COMPONENT_NONE;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot, before_model, &delta), 0);
    cbm_zova_workspace_delta_metrics(delta, &metrics);
    ASSERT_EQ(metrics.topology_inserts + metrics.topology_deletes, 0);
    ASSERT_EQ(metrics.node_vector_upserts + metrics.node_vector_deletes, 0);
    ASSERT_EQ(metrics.token_vector_upserts + metrics.token_vector_deletes, 0);
    cbm_zova_workspace_delta_free(delta);

    CBMDumpNode changed_nodes[] = {base_nodes[0], base_nodes[1]};
    changed_nodes[0].properties = "{\"rank\":9}";
    CBMDumpEdge changed_edge = base_edge;
    changed_edge.properties = "{\"line\":3}";
    CBMDumpVector changed_vector = base_vector;
    changed_vector.vector = changed_node_bytes;
    CBMDumpTokenVec changed_token = base_token;
    changed_token.idf = 2.0f;
    cbm_zova_file_hash_input_t changed_hash = base_hash;
    changed_hash.content_hash = "two";
    cbm_zova_workspace_generation_input_t changed = base;
    changed.nodes = changed_nodes;
    changed.edges = &changed_edge;
    changed.node_vectors = &changed_vector;
    changed.token_vectors = &changed_token;
    changed.file_hashes = &changed_hash;
    changed.project_summary.summary = "after";
    changed.project_summary.updated_at = "after";
    cbm_zova_publish_model_t *changed_model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &changed, &changed_model), 0);
    ASSERT_TRUE(cbm_zova_workspace_delta_build(&snapshot, changed_model, &delta) != 0);
    ASSERT_NULL(delta);
    snapshot.hydrated_components = CBM_ZOVA_SNAPSHOT_COMPONENT_ALL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot, changed_model, &delta), 0);
    cbm_zova_workspace_delta_metrics(delta, &metrics);
    ASSERT_EQ(metrics.node_updates, 1);
    ASSERT_EQ(metrics.edge_deletes, 1);
    ASSERT_EQ(metrics.edge_inserts, 1);
    ASSERT_EQ(metrics.topology_inserts + metrics.topology_deletes, 0);
    ASSERT_EQ(metrics.node_vector_upserts, 1);
    ASSERT_EQ(metrics.token_vector_upserts, 1);
    ASSERT_EQ(metrics.file_hash_upserts, 1);
    ASSERT_EQ(metrics.summary_replacements, 1);
    ASSERT_EQ(cbm_zova_workspace_delta_node_update_at(delta, 0)->source, &changed_nodes[0]);
    ASSERT_STR_EQ(cbm_zova_workspace_delta_edge_delete_at(delta, 0)->edge_id,
                  snapshot.edge_ids[0]);
    cbm_zova_workspace_delta_free(delta);
    cbm_zova_publish_model_free(changed_model);

    cbm_zova_workspace_generation_input_t replacement = base;
    CBMDumpNode replacement_nodes[] = {base_nodes[0], base_nodes[1]};
    replacement_nodes[0].qualified_name = "delta.ReplacedAlpha";
    replacement_nodes[0].name = "ReplacedAlpha";
    replacement.nodes = replacement_nodes;
    replacement.edges = NULL;
    replacement.edge_count = 0;
    replacement.node_vectors = NULL;
    replacement.node_vector_count = 0;
    replacement.token_vectors = NULL;
    replacement.token_vector_count = 0;
    replacement.file_hashes = NULL;
    replacement.file_hash_count = 0;
    replacement.project_summary.present = false;
    cbm_zova_publish_model_t *replacement_model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &replacement, &replacement_model), 0);
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot, replacement_model, &delta), 0);
    cbm_zova_workspace_delta_metrics(delta, &metrics);
    ASSERT_EQ(metrics.node_inserts, 1);
    ASSERT_EQ(metrics.node_deletes, 1);
    ASSERT_EQ(metrics.edge_deletes, 1);
    ASSERT_EQ(metrics.topology_deletes, 1);
    ASSERT_EQ(metrics.node_vector_deletes, 1);
    ASSERT_EQ(metrics.token_vector_deletes, 1);
    ASSERT_EQ(metrics.file_hash_deletes, 1);
    ASSERT_EQ(metrics.summary_replacements, 1);
    ASSERT_NOT_NULL(cbm_zova_workspace_delta_node_insert_at(delta, 0));
    ASSERT_NOT_NULL(cbm_zova_workspace_delta_node_delete_at(delta, 0));
    cbm_zova_workspace_delta_free(delta);

    char *original_second = snapshot.node_stable_ids[1];
    snapshot.node_stable_ids[1] = snapshot.node_stable_ids[0];
    ASSERT_TRUE(cbm_zova_workspace_delta_build(&snapshot, replacement_model, &delta) != 0);
    ASSERT_NULL(delta);
    snapshot.node_stable_ids[1] = original_second;

    cbm_zova_publish_model_free(replacement_model);
    delta_test_snapshot_free(&snapshot);
    cbm_zova_publish_model_free(before_model);
    PASS();
}

TEST(zova_publish_model_is_deterministic_and_validates_identity) {
    static const uint8_t node_vector_a[] = {1, 2, 3, 4};
    static const uint8_t node_vector_zero[] = {0, 0, 0, 0};
    static const uint8_t token_vector[] = {4, 3, 2, 1};
    CBMDumpNode nodes[] = {
        {.id=2,.project="model",.label="Function",.name="SecondNode",
         .qualified_name="model.SecondNode",.file_path="src/model.c",.start_line=20,
         .end_line=24,.properties="{\"rank\":2}"},
        {.id=1,.project="model",.label="Function",.name="FirstNode",
         .qualified_name="model.FirstNode",.file_path="src/model.c",.start_line=10,
         .end_line=14,.properties="{\"rank\":1}"},
    };
    CBMDumpEdge edges[] = {
        {.id=2,.project="model",.source_id=1,.target_id=2,.type="CALLS",
         .properties="{\"line\":12}",.url_path="",.local_name="second"},
        {.id=1,.project="model",.source_id=1,.target_id=2,.type="CALLS",
         .properties="{\"line\":11}",.url_path="",.local_name="first"},
    };
    CBMDumpVector node_vectors[] = {
        {.node_id=1,.project="model",.vector=node_vector_a,.vector_len=4},
        {.node_id=2,.project="model",.vector=node_vector_zero,.vector_len=4},
    };
    CBMDumpTokenVec token_vectors[] = {
        {.id=1,.project="model",.token="model",.vector=token_vector,.vector_len=4,.idf=1.5f},
    };
    cbm_zova_file_hash_input_t hashes[] = {
        {.file_path="src/z.c",.content_hash="z",.mtime_ns=2,.size_bytes=20},
        {.file_path="src/a.c",.content_hash="a",.mtime_ns=1,.size_bytes=10},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path="/tmp/model",.project="model",.indexed_at="2026-07-15T00:00:00Z",
        .model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,.vector_dimensions=4,
        .nodes=nodes,.node_count=2,.edges=edges,.edge_count=2,
        .node_vectors=node_vectors,.node_vector_count=2,
        .token_vectors=token_vectors,.token_vector_count=1,
        .file_hashes=hashes,.file_hash_count=2,
    };
    const char *workspace_id = "w1_00000000000000000000000000000000";
    cbm_zova_publish_model_t *first = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &input, &first), 0);
    ASSERT_NOT_NULL(first);
    ASSERT_EQ(cbm_zova_publish_model_node_count(first), 2);
    ASSERT_EQ(cbm_zova_publish_model_edge_count(first), 2);
    ASSERT_EQ(cbm_zova_publish_model_topology_count(first), 1);
    ASSERT_EQ(cbm_zova_publish_model_node_vector_count(first), 1);
    ASSERT_EQ(cbm_zova_publish_model_token_vector_count(first), 1);
    const cbm_zova_publish_node_t *node0 = cbm_zova_publish_model_node_at(first, 0);
    const cbm_zova_publish_node_t *node1 = cbm_zova_publish_model_node_at(first, 1);
    ASSERT_NOT_NULL(node0);
    ASSERT_NOT_NULL(node1);
    ASSERT_TRUE(strcmp(node0->stable_id, node1->stable_id) < 0);
    ASSERT_NOT_NULL(strstr(cbm_zova_publish_model_fts_name_at(first, 0), " "));
    const cbm_zova_publish_edge_t *edge0 = cbm_zova_publish_model_edge_at(first, 0);
    const cbm_zova_publish_edge_t *edge1 = cbm_zova_publish_model_edge_at(first, 1);
    ASSERT_NOT_NULL(edge0);
    ASSERT_NOT_NULL(edge1);
    ASSERT_EQ(edge0->topology_ordinal, 0);
    ASSERT_EQ(edge1->topology_ordinal, 0);
    const cbm_zova_publish_edge_t *topology =
        cbm_zova_publish_model_topology_at(first, 0);
    ASSERT_NOT_NULL(topology);
    ASSERT_EQ(topology->logical_edge_count, 2);
    zova_payload_capture_t payload_records = {0};
    size_t payload_edge_count = 0;
    ASSERT_EQ(cbm_zova_edge_payload_visit(topology->payload, topology->payload_len,
                                          zova_capture_payload_record,
                                          &payload_records, &payload_edge_count), 0);
    ASSERT_EQ(payload_edge_count, 2);
    ASSERT_TRUE(zova_payload_slice_equals(payload_records.records[0].local_name,
                                          edge0->source->local_name));
    ASSERT_TRUE(zova_payload_slice_equals(payload_records.records[1].local_name,
                                          edge1->source->local_name));
    const cbm_zova_publish_node_vector_t *node_vector =
        cbm_zova_publish_model_node_vector_at(first, 0);
    ASSERT_NOT_NULL(node_vector);
    ASSERT_LT(node_vector->node_ordinal,
              (uint64_t)cbm_zova_publish_model_node_count(first));
    ASSERT_STR_EQ(cbm_zova_publish_model_node_at(first, (int)node_vector->node_ordinal)->stable_id,
                  node_vector->stable_id);
    cbm_zova_publish_model_metrics_t metrics = {0};
    cbm_zova_publish_model_metrics(first, &metrics);
    ASSERT_EQ(metrics.stable_node_id_computations, 2);
    ASSERT_EQ(metrics.edge_id_computations, 2);
    ASSERT_EQ(metrics.token_id_computations, 1);
    ASSERT_EQ(metrics.camel_split_computations, 2);
    ASSERT_EQ(metrics.endpoint_lookups, 4);
    /* Dense graph-buffer IDs are already a complete 1..node_count keyspace.
     * The publication model must resolve them directly instead of sorting a
     * second dump-ID lookup table. */
    ASSERT_EQ(metrics.global_sorts, 6);

    CBMDumpNode reversed_nodes[] = {nodes[1], nodes[0]};
    CBMDumpEdge reversed_edges[] = {edges[1], edges[0]};
    cbm_zova_file_hash_input_t reversed_hashes[] = {hashes[1], hashes[0]};
    cbm_zova_workspace_generation_input_t reversed = input;
    reversed.nodes = reversed_nodes;
    reversed.edges = reversed_edges;
    reversed.file_hashes = reversed_hashes;
    cbm_zova_publish_model_t *second = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &reversed, &second), 0);
    ASSERT_NOT_NULL(second);
    const cbm_zova_workspace_generation_result_t *first_digest =
        cbm_zova_publish_model_digests(first);
    const cbm_zova_workspace_generation_result_t *second_digest =
        cbm_zova_publish_model_digests(second);
    ASSERT_STR_EQ(first_digest->metadata_sha256,
                  "cd66fce53b3bd59a8c26b73a0ef7439721cff26734e31650d9ab249ac9e83473");
    ASSERT_STR_EQ(first_digest->fts_sha256,
                  "8f84b1e76baa8136522a66bfeef02459eb32f0c9c33e5f8bd80f3eb4eb82f82e");
    ASSERT_STR_EQ(first_digest->topology_sha256,
                  "6ac5475f393fd82ef029ffd734cb26cbf86ded48d985101bfc436d1b6d73b2b7");
    ASSERT_STR_EQ(first_digest->node_vector_sha256,
                  "9aa146ef90870e005714046363bf34b73108fe04820c430895f27cafaf274311");
    ASSERT_STR_EQ(first_digest->token_vector_sha256,
                  "90093c29d678824c6bdaa6df3ae60824edc56d07addfb9c24941b0cef3d34412");
    ASSERT_STR_EQ(first_digest->metadata_sha256, second_digest->metadata_sha256);
    ASSERT_STR_EQ(first_digest->fts_sha256, second_digest->fts_sha256);
    ASSERT_STR_EQ(first_digest->topology_sha256, second_digest->topology_sha256);
    ASSERT_STR_EQ(first_digest->node_vector_sha256, second_digest->node_vector_sha256);
    ASSERT_STR_EQ(first_digest->token_vector_sha256, second_digest->token_vector_sha256);
    ASSERT_EQ(metrics.digest_row_revisit_count, 0);
    cbm_zova_publish_model_free(second);

    cbm_zova_prepared_view_t *prepared = NULL;
    ASSERT_EQ(cbm_zova_prepared_view_build(workspace_id, &input, &prepared), 0);
    ASSERT_NOT_NULL(prepared);
    const cbm_zova_workspace_generation_result_t *prepared_digest =
        cbm_zova_publish_model_digests(prepared);
    ASSERT_STR_EQ(first_digest->metadata_sha256, prepared_digest->metadata_sha256);
    ASSERT_STR_EQ(first_digest->fts_sha256, prepared_digest->fts_sha256);
    ASSERT_STR_EQ(first_digest->topology_sha256, prepared_digest->topology_sha256);
    ASSERT_STR_EQ(first_digest->node_vector_sha256, prepared_digest->node_vector_sha256);
    ASSERT_STR_EQ(first_digest->token_vector_sha256, prepared_digest->token_vector_sha256);
    cbm_zova_publish_model_metrics_t prepared_metrics = {0};
    cbm_zova_publish_model_metrics(prepared, &prepared_metrics);
    ASSERT_EQ(prepared_metrics.prepared_single_default_payload_count, 0);
    ASSERT_EQ(prepared_metrics.prepared_payload_scratch_edge_count, 2);
    cbm_zova_prepared_view_free(prepared);

    CBMDumpNode sparse_nodes[] = {nodes[0], nodes[1]};
    sparse_nodes[0].id = 20;
    sparse_nodes[1].id = 10;
    CBMDumpEdge sparse_edge = edges[0];
    sparse_edge.source_id = 10;
    sparse_edge.target_id = 20;
    CBMDumpVector sparse_vector = node_vectors[0];
    sparse_vector.node_id = 10;
    cbm_zova_workspace_generation_input_t sparse = input;
    sparse.nodes = sparse_nodes;
    sparse.edges = &sparse_edge;
    sparse.edge_count = 1;
    sparse.node_vectors = &sparse_vector;
    sparse.node_vector_count = 1;
    cbm_zova_publish_model_t *sparse_model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &sparse, &sparse_model), 0);
    ASSERT_NOT_NULL(cbm_zova_publish_model_stable_id_for_dump_id(sparse_model, 10));
    ASSERT_NOT_NULL(cbm_zova_publish_model_stable_id_for_dump_id(sparse_model, 20));
    ASSERT_NULL(cbm_zova_publish_model_stable_id_for_dump_id(sparse_model, 1));
    cbm_zova_publish_model_metrics_t sparse_metrics = {0};
    cbm_zova_publish_model_metrics(sparse_model, &sparse_metrics);
    ASSERT_EQ(sparse_metrics.global_sorts, 7);
    cbm_zova_publish_model_free(sparse_model);

    cbm_zova_publish_model_test_fail_allocation_at(-1);
    cbm_zova_publish_model_t *allocation_probe = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(workspace_id, &input, &allocation_probe), 0);
    int64_t allocation_count = cbm_zova_publish_model_test_allocation_count();
    ASSERT_GT(allocation_count, 0);
    cbm_zova_publish_model_free(allocation_probe);
    for (int64_t allocation = 0; allocation < allocation_count; allocation++) {
        cbm_zova_publish_model_t *failed = NULL;
        cbm_zova_publish_model_test_fail_allocation_at(allocation);
        ASSERT_TRUE(cbm_zova_publish_model_build(workspace_id, &input, &failed) != 0);
        ASSERT_NULL(failed);
    }
    cbm_zova_publish_model_test_fail_allocation_at(-1);

    CBMDumpNode bad_nodes[] = {nodes[0], nodes[1]};
    cbm_zova_publish_model_t *bad = NULL;
    bad_nodes[1].id = bad_nodes[0].id;
    cbm_zova_workspace_generation_input_t invalid = input;
    invalid.nodes = bad_nodes;
    ASSERT_TRUE(cbm_zova_publish_model_build(workspace_id, &invalid, &bad) != 0);
    ASSERT_NULL(bad);
    bad_nodes[1] = bad_nodes[0];
    bad_nodes[1].id = 99;
    ASSERT_TRUE(cbm_zova_publish_model_build(workspace_id, &invalid, &bad) != 0);
    ASSERT_NULL(bad);
    CBMDumpEdge missing = edges[0];
    missing.target_id = 999;
    invalid = input;
    invalid.edges = &missing;
    invalid.edge_count = 1;
    ASSERT_TRUE(cbm_zova_publish_model_build(workspace_id, &invalid, &bad) != 0);
    ASSERT_NULL(bad);

    cbm_zova_publish_model_free(first);
    PASS();
}

TEST(zova_publish_model_dense_camel_name_is_owned_without_overflow) {
    char name[241];
    char expected[602];
    for (size_t i = 0; i < sizeof(name) - 1; i++) name[i] = i % 2 ? 'A' : 'a';
    name[sizeof(name) - 1] = '\0';
    size_t output = 0;
    memcpy(expected + output, name, sizeof(name) - 1);
    output += sizeof(name) - 1;
    expected[output++] = ' ';
    for (size_t i = 0; i < sizeof(name) - 1; i++) {
        if (i > 0 && name[i] == 'A' && name[i - 1] == 'a') expected[output++] = ' ';
        expected[output++] = name[i];
    }
    expected[output] = '\0';
    ASSERT_EQ(output, sizeof(expected) - 1);

    CBMDumpNode node = {
        .id = 1, .project = "camel", .label = "Function", .name = name,
        .qualified_name = "", .file_path = "src/camel.c", .start_line = 1,
        .end_line = 2, .properties = "{}",
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/camel", .project = "camel",
        .indexed_at = "2026-07-15T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 4,
        .nodes = &node, .node_count = 1,
    };
    cbm_zova_publish_model_t *model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build("w1_00000000000000000000000000000000",
                                           &input, &model), 0);
    ASSERT_NOT_NULL(model);
    ASSERT_STR_EQ(cbm_zova_publish_model_fts_name_at(model, 0), expected);
    cbm_zova_publish_model_free(model);
    PASS();
}

TEST(zova_publish_model_uses_bounded_row_storage_allocations) {
    enum { NODE_COUNT = 64, EDGE_COUNT = 63 };
    CBMDumpNode nodes[NODE_COUNT];
    CBMDumpEdge edges[EDGE_COUNT];
    char names[NODE_COUNT][24];
    char qualified_names[NODE_COUNT][48];
    char file_paths[NODE_COUNT][32];
    memset(nodes, 0, sizeof(nodes));
    memset(edges, 0, sizeof(edges));
    for (int i = 0; i < NODE_COUNT; i++) {
        snprintf(names[i], sizeof(names[i]), "Node%d", i);
        snprintf(qualified_names[i], sizeof(qualified_names[i]), "arena.Node%d", i);
        snprintf(file_paths[i], sizeof(file_paths[i]), "src/node_%d.c", i);
        nodes[i] = (CBMDumpNode){
            .id = i + 1,
            .project = "arena",
            .label = "Function",
            .name = names[i],
            .qualified_name = qualified_names[i],
            .file_path = file_paths[i],
            .start_line = i + 1,
            .end_line = i + 1,
            .properties = "{}",
        };
    }
    for (int i = 0; i < EDGE_COUNT; i++) {
        edges[i] = (CBMDumpEdge){
            .id = i + 1,
            .project = "arena",
            .source_id = i + 1,
            .target_id = i + 2,
            .type = "CALLS",
            .properties = "{}",
            .url_path = "",
            .local_name = "",
        };
    }
    const cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/arena",
        .project = "arena",
        .indexed_at = "2026-07-20T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 2,
        .nodes = nodes,
        .node_count = NODE_COUNT,
        .edges = edges,
        .edge_count = EDGE_COUNT,
    };
    cbm_zova_publish_model_test_fail_allocation_at(-1);
    cbm_zova_publish_model_t *model = NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(
                  "w1_55555555555555555555555555555555", &input, &model),
              0);
    ASSERT_LT(cbm_zova_publish_model_test_allocation_count(), 16);
    cbm_zova_publish_model_free(model);
    PASS();
}

TEST(zova_atomic_delta_publisher_preserves_old_generation_on_every_fault) {
    char path[256];
    snprintf(path,sizeof(path),"/tmp/cbm-zova-delta-%ld.zova",(long)getpid());
    cbm_unlink(path);
    static const uint8_t vector_a[]={1,2,3,4};
    static const uint8_t vector_changed[]={4,3,2,1};
    static const uint8_t token_vector[]={2,2,2,2};
    CBMDumpNode nodes[]={
        {.id=1,.project="atomic-delta",.label="Function",.name="Alpha",
         .qualified_name="atomic_delta.Alpha",.file_path="src/a.c",.start_line=1,.end_line=3,
         .properties="{\"rank\":1}"},
        {.id=2,.project="atomic-delta",.label="Function",.name="Beta",
         .qualified_name="atomic_delta.Beta",.file_path="src/b.c",.start_line=4,.end_line=7,
         .properties="{\"rank\":2}"},
    };
    CBMDumpEdge edge={.id=1,.project="atomic-delta",.source_id=1,.target_id=2,
        .type="CALLS",.properties="{}",.url_path="",.local_name="beta"};
    CBMDumpVector vectors[]={
        {.node_id=1,.project="atomic-delta",.vector=vector_a,.vector_len=4},
        {.node_id=2,.project="atomic-delta",.vector=vector_a,.vector_len=4},
    };
    CBMDumpTokenVec token={.id=1,.project="atomic-delta",.token="alpha",
        .vector=token_vector,.vector_len=4,.idf=1.0f};
    cbm_zova_file_hash_input_t hashes[]={
        {.file_path="src/a.c",.content_hash="a1",.mtime_ns=1,.size_bytes=10},
        {.file_path="src/b.c",.content_hash="b1",.mtime_ns=1,.size_bytes=20},
    };
    cbm_zova_workspace_generation_input_t input={
        .root_path="/tmp/atomic-delta",.project="atomic-delta",
        .indexed_at="2026-07-15T00:00:00Z",.model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions=4,.nodes=nodes,.node_count=2,.edges=&edge,.edge_count=1,
        .node_vectors=vectors,.node_vector_count=2,.token_vectors=&token,.token_vector_count=1,
        .file_hashes=hashes,.file_hash_count=2,
        .project_summary={.present=true,.summary="before",.source_hash="a1",
                          .created_at="created",.updated_at="before"},
    };
    cbm_zova_workspace_generation_result_t first={0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path,&input,&first),0);
    ASSERT_EQ(first.generation,1);
    cbm_zova_workspace_snapshot_t incremental_snapshot={0};
    ASSERT_EQ(cbm_zova_repository_export_incremental_snapshot(
                  path,first.workspace_id,&incremental_snapshot),0);
    ASSERT_EQ(incremental_snapshot.generation,first.generation);
    ASSERT_EQ(incremental_snapshot.hydrated_components,
              CBM_ZOVA_SNAPSHOT_COMPONENT_NONE);
    ASSERT_NULL(incremental_snapshot.topology_edges);
    ASSERT_EQ(incremental_snapshot.topology_edge_count,0);
    ASSERT_NULL(incremental_snapshot.node_vectors);
    ASSERT_EQ(incremental_snapshot.node_vector_count,0);
    ASSERT_NULL(incremental_snapshot.token_vectors);
    ASSERT_EQ(incremental_snapshot.token_vector_count,0);
    ASSERT_EQ(cbm_zova_repository_hydrate_incremental_components(
                  path,first.workspace_id,first.generation,
                  CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY,&incremental_snapshot),
              CBM_ZOVA_SNAPSHOT_OK);
    ASSERT_EQ(incremental_snapshot.hydrated_components,
              CBM_ZOVA_SNAPSHOT_COMPONENT_TOPOLOGY);
    ASSERT_EQ(incremental_snapshot.topology_edge_count,1);
    ASSERT_NULL(incremental_snapshot.node_vectors);
    ASSERT_EQ(incremental_snapshot.node_vector_count,0);
    ASSERT_NULL(incremental_snapshot.token_vectors);
    ASSERT_EQ(incremental_snapshot.token_vector_count,0);
    ASSERT_EQ(cbm_zova_repository_hydrate_incremental_components(
                  path,first.workspace_id,first.generation,
                  CBM_ZOVA_SNAPSHOT_COMPONENT_NODE_VECTORS,&incremental_snapshot),
              CBM_ZOVA_SNAPSHOT_OK);
    ASSERT_EQ(incremental_snapshot.node_vector_count,2);
    ASSERT_NULL(incremental_snapshot.token_vectors);
    ASSERT_EQ(incremental_snapshot.token_vector_count,0);
    ASSERT_EQ(cbm_zova_repository_hydrate_incremental_components(
                  path,first.workspace_id,first.generation,
                  CBM_ZOVA_SNAPSHOT_COMPONENT_TOKEN_VECTORS,&incremental_snapshot),
              CBM_ZOVA_SNAPSHOT_OK);
    ASSERT_EQ(incremental_snapshot.token_vector_count,1);
    cbm_zova_snapshot_components_t hydrated_before_stale =
        incremental_snapshot.hydrated_components;
    ASSERT_EQ(cbm_zova_repository_hydrate_incremental_components(
                  path,first.workspace_id,first.generation+1,
                  CBM_ZOVA_SNAPSHOT_COMPONENT_ALL,&incremental_snapshot),
              CBM_ZOVA_SNAPSHOT_STALE);
    ASSERT_EQ(incremental_snapshot.hydrated_components,hydrated_before_stale);
    cbm_zova_workspace_snapshot_free(&incremental_snapshot);

    cbm_zova_workspace_snapshot_t snapshot={0};
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    ASSERT_EQ(snapshot.metrics.base_phase_mask, CBM_ZOVA_SNAPSHOT_BASE_PHASE_ALL);
    ASSERT_EQ(snapshot.metrics.node_rows, 2);
    ASSERT_EQ(snapshot.metrics.edge_rows, 1);
    ASSERT_EQ(snapshot.metrics.file_hash_rows, 2);
    ASSERT_TRUE(snapshot.metrics.open_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.header_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.integrity_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.nodes_sql_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.nodes_native_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.nodes_finalize_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.edges_sql_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.edges_native_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.edges_finalize_ms >= 0.0);
    ASSERT_EQ(snapshot.metrics.edge_scan_pages, 1);
    ASSERT_EQ(snapshot.metrics.edge_native_rows, 1);
    ASSERT_EQ(snapshot.metrics.edge_logical_rows, 1);
    ASSERT_EQ(snapshot.metrics.edge_keyed_read_count, 1);
    ASSERT_TRUE(snapshot.metrics.edge_string_arena_chunks >= 1);
    ASSERT_TRUE(snapshot.metrics.edge_string_arena_bytes > 0);
    ASSERT_TRUE(snapshot.edges[0].project == snapshot.project);
    ASSERT_NOT_NULL(snapshot.edge_ids[0]);
    ASSERT_TRUE(snapshot.metrics.hashes_summary_ms >= 0.0);
    ASSERT_TRUE(snapshot.metrics.close_ms >= 0.0);

    /* A no-op delta cannot create an orphan file, so it must not perform a
     * workspace-wide reconciliation pass. Keep one pre-existing orphan as a
     * sentinel: the old unconditional cleanup deletes it. */
    zova_database *orphan_db=NULL;
    zova_message orphan_error={0};
    ASSERT_EQ(zova_database_open(&(zova_database_open_request){
                  .path=path,.out_db=&orphan_db,.out_error_message=&orphan_error}),ZOVA_OK);
    zova_message_free(&orphan_error);
    char orphan_insert_sql[1024];
    snprintf(orphan_insert_sql,sizeof(orphan_insert_sql),
             "INSERT INTO cbm_files_v1(workspace_key,file_path) "
             "SELECT workspace_key,'src/preexisting-orphan.c' FROM cbm_workspace_registry "
             "WHERE workspace_id='%s'",first.workspace_id);
    ASSERT_EQ(zova_tx_exec(orphan_db,orphan_insert_sql),0);
    ASSERT_EQ(zova_database_close(orphan_db),ZOVA_OK);

    cbm_zova_publish_model_t *unchanged_model=NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(first.workspace_id,&input,&unchanged_model),0);
    cbm_zova_workspace_delta_t *unchanged_delta=NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,unchanged_model,&unchanged_delta),0);
    cbm_zova_publish_test_metrics_reset();
    cbm_zova_workspace_generation_result_t noop={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,unchanged_model,unchanged_delta,&noop),0);
    ASSERT_EQ(noop.generation,2);
    ASSERT_EQ(noop.inserted_count+noop.updated_count+noop.deleted_count,0);
    ASSERT_EQ(noop.full_clear_count,0);
    cbm_zova_publish_test_metrics_t noop_metrics={0};
    cbm_zova_publish_test_metrics_get(&noop_metrics);
    ASSERT_EQ(noop_metrics.full_clear_count,0);
    ASSERT_EQ(noop_metrics.canonical_node_fts_passes,0);
    ASSERT_EQ(noop_metrics.canonical_edge_passes,0);
    ASSERT_EQ(noop_metrics.delta_authoritative_rows_touched,0);
    ASSERT_EQ(noop_metrics.delta_clear_violation_count,0);
    ASSERT_EQ(noop_metrics.full_fts_bulk_statements,0);
    ASSERT_EQ(noop_metrics.full_node_guard_validation_statements,0);
    ASSERT_EQ(noop_metrics.full_edge_guard_validation_statements,0);
    int64_t orphan_files_after_noop=0;
    ASSERT_EQ(zova_scalar_int64(path,
              "SELECT count(*) FROM cbm_files_v1 "
              "WHERE file_path='src/preexisting-orphan.c'",
              &orphan_files_after_noop),0);
    ASSERT_EQ(orphan_files_after_noop,1);
    cbm_zova_workspace_delta_free(unchanged_delta);
    cbm_zova_publish_model_free(unchanged_model);

    CBMDumpNode changed_node=nodes[0];
    changed_node.properties="{\"rank\":9}";
    CBMDumpVector changed_vector={.node_id=1,.project="atomic-delta",
        .vector=vector_changed,.vector_len=4};
    cbm_zova_file_hash_input_t changed_hash={.file_path="src/a.c",.content_hash="a2",
        .mtime_ns=2,.size_bytes=11};
    cbm_zova_workspace_generation_input_t changed=input;
    changed.indexed_at="2026-07-15T00:01:00Z";
    changed.nodes=&changed_node;changed.node_count=1;
    changed.edges=NULL;changed.edge_count=0;
    changed.node_vectors=&changed_vector;changed.node_vector_count=1;
    changed.token_vectors=NULL;changed.token_vector_count=0;
    changed.file_hashes=&changed_hash;changed.file_hash_count=1;
    changed.project_summary.summary="after";
    changed.project_summary.source_hash="a2";
    changed.project_summary.updated_at="after";
    cbm_zova_publish_model_t *model=NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(first.workspace_id,&changed,&model),0);
    cbm_zova_workspace_delta_t *delta=NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,model,&delta),0);
    cbm_zova_workspace_delta_metrics_t metrics={0};
    cbm_zova_workspace_delta_metrics(delta,&metrics);
    ASSERT_EQ(metrics.node_updates,1);
    ASSERT_EQ(metrics.node_deletes,1);
    ASSERT_EQ(metrics.edge_deletes,1);
    ASSERT_EQ(metrics.topology_deletes,1);
    ASSERT_EQ(metrics.node_vector_upserts,1);
    ASSERT_EQ(metrics.node_vector_deletes,1);
    ASSERT_EQ(metrics.token_vector_deletes,1);

    cbm_zova_workspace_generation_result_t stale={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,model,delta,&stale),-1);
    int64_t active_after_stale=0;
    char active_sql[512];
    snprintf(active_sql,sizeof(active_sql),
             "SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id='%s'",
             first.workspace_id);
    ASSERT_EQ(zova_scalar_int64(path,active_sql,&active_after_stale),0);
    ASSERT_EQ(active_after_stale,2);
    cbm_zova_workspace_delta_free(delta);
    delta=NULL;
    cbm_zova_workspace_snapshot_free(&snapshot);
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    ASSERT_EQ(snapshot.generation,2);
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,model,&delta),0);

    const char *faults[]={
        "user_delta_after_generation","user_delta_after_vector_deletes",
        "user_delta_after_graph_deletes","user_delta_after_canonical_deletes",
        "user_delta_after_node_upserts","user_delta_after_edge_inserts",
        "user_delta_after_graph_puts","user_delta_after_vector_puts",
        "user_delta_after_metadata_upserts","user_delta_after_integrity",
        "user_delta_before_commit",
    };
    for(size_t i=0;i<sizeof(faults)/sizeof(faults[0]);i++){
        cbm_setenv("CBM_ZOVA_TEST_FAIL_PHASE",faults[i],1);
        cbm_zova_workspace_generation_result_t failed={0};
        ASSERT_EQ(cbm_zova_user_database_publish_delta(path,model,delta,&failed),-1);
        cbm_unsetenv("CBM_ZOVA_TEST_FAIL_PHASE");
        int64_t active=0;
        char sql[512];
        snprintf(sql,sizeof(sql),"SELECT active_generation FROM cbm_workspace_registry WHERE workspace_id='%s'",first.workspace_id);
        ASSERT_EQ(zova_scalar_int64(path,sql,&active),0);
        ASSERT_EQ(active,2);
        snprintf(sql,sizeof(sql),"SELECT count(*) FROM cbm_nodes_v1 WHERE workspace_key=(SELECT "
                 "workspace_key FROM cbm_workspace_registry WHERE workspace_id='%s')",
                 first.workspace_id);
        ASSERT_EQ(zova_scalar_int64(path,sql,&active),0);
        ASSERT_EQ(active,2);
    }
    cbm_zova_publish_test_metrics_reset();
    cbm_zova_workspace_generation_result_t result={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,model,delta,&result),0);
    ASSERT_EQ(result.generation,3);
    ASSERT_EQ(result.publication_mode,CBM_ZOVA_PUBLICATION_MODE_DELTA);
    ASSERT_EQ(result.full_clear_count,0);
    ASSERT_EQ(result.unchanged_rewrite_count,0);
    ASSERT_EQ(result.metadata_nodes,1);
    ASSERT_EQ(result.metadata_edges,0);
    ASSERT_EQ(result.graph_nodes,1);
    ASSERT_EQ(result.graph_edges,0);
    ASSERT_STR_EQ(result.metadata_sha256,cbm_zova_publish_model_digests(model)->metadata_sha256);
    cbm_zova_publish_test_metrics_t changed_metrics={0};
    cbm_zova_publish_test_metrics_get(&changed_metrics);
    ASSERT_EQ(changed_metrics.native_graph_fresh_calls,0);
    ASSERT_EQ(changed_metrics.native_graph_prepared_calls,0);
    ASSERT_EQ(changed_metrics.database_open_count,1);
    ASSERT_EQ(changed_metrics.database_close_count,1);
    ASSERT_EQ(changed_metrics.delta_file_key_resolutions,1);
    ASSERT_EQ(changed_metrics.delta_endpoint_key_lookups,0);
    ASSERT_EQ(zova_scalar_int64(path,
              "SELECT count(*) FROM cbm_files_v1 "
              "WHERE file_path='src/preexisting-orphan.c'",
              &orphan_files_after_noop),0);
    ASSERT_EQ(orphan_files_after_noop,0);

    cbm_zova_workspace_delta_free(delta);
    cbm_zova_publish_model_free(model);
    cbm_zova_workspace_snapshot_free(&snapshot);

    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    CBMDumpEdge shared_endpoint_edges[]={
        {.id=10,.project="atomic-delta",.source_id=1,.target_id=1,
         .type="CALLS",.properties="{\"slot\":1}",.url_path="",.local_name="alpha"},
        {.id=11,.project="atomic-delta",.source_id=1,.target_id=1,
         .type="CALLS",.properties="{\"slot\":2}",.url_path="",.local_name="alpha-second"},
    };
    cbm_zova_workspace_generation_input_t edge_changed=changed;
    edge_changed.indexed_at="2026-07-15T00:02:00Z";
    edge_changed.edges=shared_endpoint_edges;
    edge_changed.edge_count=2;
    cbm_zova_publish_model_t *edge_model=NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(first.workspace_id,&edge_changed,&edge_model),0);
    cbm_zova_workspace_delta_t *edge_delta=NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,edge_model,&edge_delta),0);
    cbm_zova_workspace_delta_metrics_t edge_metrics={0};
    cbm_zova_workspace_delta_metrics(edge_delta,&edge_metrics);
    ASSERT_EQ(edge_metrics.edge_inserts,2);
    cbm_zova_publish_test_metrics_reset();
    cbm_zova_workspace_generation_result_t edge_result={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,edge_model,edge_delta,&edge_result),0);
    cbm_zova_publish_test_metrics_t edge_publish_metrics={0};
    cbm_zova_publish_test_metrics_get(&edge_publish_metrics);
    ASSERT_EQ(edge_publish_metrics.delta_endpoint_key_lookups,0);
    ASSERT_EQ(edge_publish_metrics.delta_file_key_resolutions,0);
    cbm_zova_workspace_delta_free(edge_delta);
    cbm_zova_publish_model_free(edge_model);
    cbm_zova_workspace_snapshot_free(&snapshot);

    /* Both logical edges share one authoritative Zova topology edge. */
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    ASSERT_EQ(snapshot.edge_count,2);
    ASSERT_EQ(snapshot.topology_edge_count,1);
    ASSERT_EQ(snapshot.metrics.edge_scan_pages,1);
    ASSERT_EQ(snapshot.metrics.edge_native_rows,1);
    ASSERT_EQ(snapshot.metrics.edge_logical_rows,2);
    ASSERT_EQ(snapshot.metrics.edge_keyed_read_count,1);

    cbm_zova_workspace_generation_input_t one_logical_edge=edge_changed;
    one_logical_edge.indexed_at="2026-07-15T00:03:00Z";
    one_logical_edge.edge_count=1;
    cbm_zova_publish_model_t *one_edge_model=NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(first.workspace_id,&one_logical_edge,
                                           &one_edge_model),0);
    cbm_zova_workspace_delta_t *one_edge_delta=NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,one_edge_model,&one_edge_delta),0);
    cbm_zova_workspace_delta_metrics_t one_edge_metrics={0};
    cbm_zova_workspace_delta_metrics(one_edge_delta,&one_edge_metrics);
    ASSERT_EQ(one_edge_metrics.edge_deletes,1);
    ASSERT_EQ(one_edge_metrics.topology_deletes,0);
    cbm_zova_workspace_generation_result_t one_edge_result={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,one_edge_model,one_edge_delta,
                                                    &one_edge_result),0);
    cbm_zova_workspace_delta_free(one_edge_delta);
    cbm_zova_publish_model_free(one_edge_model);
    cbm_zova_workspace_snapshot_free(&snapshot);
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    ASSERT_EQ(snapshot.edge_count,1);
    ASSERT_EQ(snapshot.topology_edge_count,1);

    cbm_zova_workspace_generation_input_t no_logical_edges=one_logical_edge;
    no_logical_edges.indexed_at="2026-07-15T00:04:00Z";
    no_logical_edges.edges=NULL;
    no_logical_edges.edge_count=0;
    cbm_zova_publish_model_t *no_edge_model=NULL;
    ASSERT_EQ(cbm_zova_publish_model_build(first.workspace_id,&no_logical_edges,
                                           &no_edge_model),0);
    cbm_zova_workspace_delta_t *no_edge_delta=NULL;
    ASSERT_EQ(cbm_zova_workspace_delta_build(&snapshot,no_edge_model,&no_edge_delta),0);
    cbm_zova_workspace_delta_metrics_t no_edge_metrics={0};
    cbm_zova_workspace_delta_metrics(no_edge_delta,&no_edge_metrics);
    ASSERT_EQ(no_edge_metrics.edge_deletes,1);
    ASSERT_EQ(no_edge_metrics.topology_deletes,1);
    cbm_zova_workspace_generation_result_t no_edge_result={0};
    ASSERT_EQ(cbm_zova_user_database_publish_delta(path,no_edge_model,no_edge_delta,
                                                    &no_edge_result),0);
    cbm_zova_workspace_delta_free(no_edge_delta);
    cbm_zova_publish_model_free(no_edge_model);
    cbm_zova_workspace_snapshot_free(&snapshot);
    ASSERT_EQ(cbm_zova_repository_export_snapshot(path,first.workspace_id,&snapshot),0);
    ASSERT_EQ(snapshot.edge_count,0);
    ASSERT_EQ(snapshot.topology_edge_count,0);
    cbm_zova_workspace_snapshot_free(&snapshot);
    cbm_unlink(path);
    PASS();
}

TEST(zova_prepared_view_uses_implicit_default_edge_payload) {
    const CBMDumpNode nodes[] = {
        {.id=1,.project="payload_fast",.label="Function",.name="Source",
         .qualified_name="payload_fast.Source",.file_path="src/payload.c",
         .start_line=1,.end_line=2,.properties="{}"},
        {.id=2,.project="payload_fast",.label="Function",.name="Target",
         .qualified_name="payload_fast.Target",.file_path="src/payload.c",
         .start_line=3,.end_line=4,.properties="{}"},
    };
    const CBMDumpEdge edge = {
        .id=1,.project="payload_fast",.source_id=1,.target_id=2,.type="CALLS",
        .properties="{}",.url_path="",.local_name="",
    };
    const cbm_zova_workspace_generation_input_t input = {
        .root_path="/tmp/payload-fast",.project="payload_fast",
        .indexed_at="2026-07-21T00:00:00Z",
        .model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,.vector_dimensions=4,
        .nodes=nodes,.node_count=2,.edges=&edge,.edge_count=1,
    };
    cbm_zova_prepared_view_t *view = NULL;
    ASSERT_EQ(cbm_zova_prepared_view_build(
                  "w1_00000000000000000000000000000000", &input, &view), 0);
    ASSERT_NOT_NULL(view);
    ASSERT_EQ(cbm_zova_publish_model_topology_count(view), 1);
    const cbm_zova_publish_edge_t *topology =
        cbm_zova_publish_model_topology_at(view, 0);
    ASSERT_NOT_NULL(topology);
    ASSERT_EQ(topology->logical_edge_count, 1);
    ASSERT_NULL(topology->payload);
    ASSERT_EQ(topology->payload_len, 0);
    cbm_zova_publish_model_metrics_t metrics = {0};
    cbm_zova_publish_model_metrics(view, &metrics);
    ASSERT_EQ(metrics.prepared_single_default_payload_count, 1);
    ASSERT_EQ(metrics.prepared_payload_scratch_edge_count, 0);
    ASSERT_TRUE(metrics.prepared_endpoint_ms >= 0.0);
    ASSERT_TRUE(metrics.prepared_topology_sort_ms >= 0.0);
    ASSERT_TRUE(metrics.prepared_topology_group_ms >= 0.0);
    ASSERT_TRUE(metrics.prepared_payload_ms >= 0.0);
    ASSERT_TRUE(metrics.prepared_topology_digest_ms >= 0.0);
    double edge_phase_sum = metrics.prepared_endpoint_ms +
                            metrics.prepared_topology_sort_ms +
                            metrics.prepared_topology_group_ms +
                            metrics.prepared_payload_ms +
                            metrics.prepared_topology_digest_ms;
    ASSERT_TRUE(metrics.edges_ms + 0.001 >= edge_phase_sum);
    cbm_zova_prepared_view_free(view);
    PASS();
}

TEST(zova_prepared_view_encodes_single_metadata_edge_without_group_scratch) {
    const CBMDumpNode nodes[] = {
        {.id=1,.project="payload_one",.label="Function",.name="Source",
         .qualified_name="payload_one.Source",.file_path="src/payload.c",
         .start_line=1,.end_line=2,.properties="{}"},
        {.id=2,.project="payload_one",.label="Function",.name="Target",
         .qualified_name="payload_one.Target",.file_path="src/payload.c",
         .start_line=3,.end_line=4,.properties="{}"},
    };
    const CBMDumpEdge edge = {
        .id=1,.project="payload_one",.source_id=1,.target_id=2,.type="IMPORTS",
        .properties="{\"kind\":\"named\"}",.url_path="/module",.local_name="Thing",
    };
    const cbm_zova_workspace_generation_input_t input = {
        .root_path="/tmp/payload-one",.project="payload_one",
        .indexed_at="2026-07-21T00:00:00Z",
        .model_fingerprint=CBM_ZOVA_MODEL_FINGERPRINT,.vector_dimensions=4,
        .nodes=nodes,.node_count=2,.edges=&edge,.edge_count=1,
    };
    cbm_zova_prepared_view_t *view = NULL;
    ASSERT_EQ(cbm_zova_prepared_view_build(
                  "w1_00000000000000000000000000000000", &input, &view), 0);
    ASSERT_NOT_NULL(view);
    const cbm_zova_publish_edge_t *topology =
        cbm_zova_publish_model_topology_at(view, 0);
    ASSERT_NOT_NULL(topology);
    ASSERT_NOT_NULL(topology->payload);
    ASSERT_GT(topology->payload_len, 0);
    size_t decoded_count = 0;
    ASSERT_EQ(cbm_zova_edge_payload_visit(topology->payload, topology->payload_len,
                                          NULL, NULL, &decoded_count), 0);
    ASSERT_EQ(decoded_count, 1);
    cbm_zova_publish_model_metrics_t metrics = {0};
    cbm_zova_publish_model_metrics(view, &metrics);
    ASSERT_EQ(metrics.prepared_single_default_payload_count, 0);
    ASSERT_EQ(metrics.prepared_payload_scratch_edge_count, 0);
    cbm_zova_prepared_view_free(view);
    PASS();
}

#endif

static int zova_benchmark_double_compare(const void *left, const void *right) {
    double left_value = *(const double *)left;
    double right_value = *(const double *)right;
    return left_value < right_value ? -1 : left_value > right_value ? 1 : 0;
}

TEST(zova_prepared_view_normalization_benchmark) {
    enum {
        NODE_COUNT = 124818,
        EDGE_COUNT = 481770,
        SAMPLE_COUNT = 5,
    };
    static const char *edge_types[] = {"CALLS", "CONTAINS", "REFERENCES", "USES"};
    CBMDumpNode *nodes = calloc(NODE_COUNT, sizeof(*nodes));
    CBMDumpEdge *edges = calloc(EDGE_COUNT, sizeof(*edges));
    char (*names)[32] = calloc(NODE_COUNT, sizeof(*names));
    char (*qualified_names)[48] = calloc(NODE_COUNT, sizeof(*qualified_names));
    char (*file_paths)[48] = calloc(NODE_COUNT, sizeof(*file_paths));
    ASSERT_NOT_NULL(nodes);
    ASSERT_NOT_NULL(edges);
    ASSERT_NOT_NULL(names);
    ASSERT_NOT_NULL(qualified_names);
    ASSERT_NOT_NULL(file_paths);

    for (int i = 0; i < NODE_COUNT; i++) {
        snprintf(names[i], sizeof(names[i]), "Node%d", i);
        snprintf(qualified_names[i], sizeof(qualified_names[i]),
                 "normalization.Node%d", i);
        snprintf(file_paths[i], sizeof(file_paths[i]), "src/module_%d.c", i / 32);
        nodes[i] = (CBMDumpNode){
            .id = i + 1,
            .project = "normalization",
            .label = "Function",
            .name = names[i],
            .qualified_name = qualified_names[i],
            .file_path = file_paths[i],
            .start_line = i % 200 + 1,
            .end_line = i % 200 + 2,
            .properties = "{}",
        };
    }
    for (int i = 0; i < EDGE_COUNT; i++) {
        int source = i % NODE_COUNT;
        int lane = i / NODE_COUNT;
        edges[i] = (CBMDumpEdge){
            .id = i + 1,
            .project = "normalization",
            .source_id = source + 1,
            .target_id = (source + lane + 1) % NODE_COUNT + 1,
            .type = edge_types[lane],
            .properties = "{}",
            .url_path = "",
            .local_name = "",
        };
    }

    const cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/normalization-benchmark",
        .project = "normalization",
        .indexed_at = "2026-07-21T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT,
        .vector_dimensions = 384,
        .nodes = nodes,
        .node_count = NODE_COUNT,
        .edges = edges,
        .edge_count = EDGE_COUNT,
    };
    const char *workspace_id = "w1_00000000000000000000000000000000";
    double normalization[SAMPLE_COUNT];
    double node_build[SAMPLE_COUNT];
    double edge_build[SAMPLE_COUNT];
    double endpoint_build[SAMPLE_COUNT];
    double topology_sort[SAMPLE_COUNT];
    double topology_group[SAMPLE_COUNT];
    double payload_build[SAMPLE_COUNT];
    double topology_digest[SAMPLE_COUNT];
    for (int iteration = -1; iteration < SAMPLE_COUNT; iteration++) {
        cbm_zova_prepared_view_t *view = NULL;
        ASSERT_EQ(cbm_zova_prepared_view_build(workspace_id, &input, &view), 0);
        ASSERT_NOT_NULL(view);
        cbm_zova_publish_model_metrics_t metrics = {0};
        cbm_zova_publish_model_metrics(view, &metrics);
        if (iteration >= 0) {
            normalization[iteration] = metrics.normalization_ms;
            node_build[iteration] = metrics.nodes_ms;
            edge_build[iteration] = metrics.edges_ms;
            endpoint_build[iteration] = metrics.prepared_endpoint_ms;
            topology_sort[iteration] = metrics.prepared_topology_sort_ms;
            topology_group[iteration] = metrics.prepared_topology_group_ms;
            payload_build[iteration] = metrics.prepared_payload_ms;
            topology_digest[iteration] = metrics.prepared_topology_digest_ms;
            printf("NORMALIZATION_SAMPLE {\"iteration\":%d,\"normalization_ms\":%.3f,"
                   "\"nodes_ms\":%.3f,\"edges_ms\":%.3f,"
                   "\"endpoint_ms\":%.3f,\"sort_ms\":%.3f,\"group_ms\":%.3f,"
                   "\"payload_ms\":%.3f,\"digest_ms\":%.3f,"
                   "\"default_payloads\":%llu,\"payload_scratch_edges\":%llu}\n",
                   iteration + 1, metrics.normalization_ms, metrics.nodes_ms,
                   metrics.edges_ms, metrics.prepared_endpoint_ms,
                   metrics.prepared_topology_sort_ms,
                   metrics.prepared_topology_group_ms, metrics.prepared_payload_ms,
                   metrics.prepared_topology_digest_ms,
                   (unsigned long long)metrics.prepared_single_default_payload_count,
                   (unsigned long long)metrics.prepared_payload_scratch_edge_count);
            fflush(stdout);
        }
        cbm_zova_prepared_view_free(view);
    }
    qsort(normalization, SAMPLE_COUNT, sizeof(*normalization),
          zova_benchmark_double_compare);
    qsort(node_build, SAMPLE_COUNT, sizeof(*node_build),
          zova_benchmark_double_compare);
    qsort(edge_build, SAMPLE_COUNT, sizeof(*edge_build),
          zova_benchmark_double_compare);
    qsort(endpoint_build, SAMPLE_COUNT, sizeof(*endpoint_build),
          zova_benchmark_double_compare);
    qsort(topology_sort, SAMPLE_COUNT, sizeof(*topology_sort),
          zova_benchmark_double_compare);
    qsort(topology_group, SAMPLE_COUNT, sizeof(*topology_group),
          zova_benchmark_double_compare);
    qsort(payload_build, SAMPLE_COUNT, sizeof(*payload_build),
          zova_benchmark_double_compare);
    qsort(topology_digest, SAMPLE_COUNT, sizeof(*topology_digest),
          zova_benchmark_double_compare);
    printf("NORMALIZATION_MEDIAN {\"normalization_ms\":%.3f,\"nodes_ms\":%.3f,"
           "\"edges_ms\":%.3f,\"endpoint_ms\":%.3f,\"sort_ms\":%.3f,"
           "\"group_ms\":%.3f,\"payload_ms\":%.3f,\"digest_ms\":%.3f}\n",
           normalization[SAMPLE_COUNT / 2], node_build[SAMPLE_COUNT / 2],
           edge_build[SAMPLE_COUNT / 2], endpoint_build[SAMPLE_COUNT / 2],
           topology_sort[SAMPLE_COUNT / 2], topology_group[SAMPLE_COUNT / 2],
           payload_build[SAMPLE_COUNT / 2], topology_digest[SAMPLE_COUNT / 2]);
    fflush(stdout);

    free(file_paths);
    free(qualified_names);
    free(names);
    free(edges);
    free(nodes);
    PASS();
}

#if CBM_WITH_ZOVA
/* Schema, counts and architecture read the workspace through the legacy
 * nodes/edges shape. src/a.c: Alpha, Beta; src/b.c: Gamma; lib/c.py: Widget.
 * Alpha calls Beta twice (one graph edge, two cbm edges). */
TEST(zova_workspace_store_reports_schema_counts_and_architecture) {
    char path[TZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/cbm-zova-schema-%d.zova", cbm_tmpdir(), getpid());
    cbm_unlink(path);
    const CBMDumpNode nodes[] = {
        {.id = 1, .project = "proj", .label = "Function", .name = "Alpha",
         .qualified_name = "proj.src.a.Alpha", .file_path = "src/a.c", .start_line = 1,
         .end_line = 2, .properties = "{\"complexity\":3}"},
        {.id = 2, .project = "proj", .label = "Function", .name = "Beta",
         .qualified_name = "proj.src.a.Beta", .file_path = "src/a.c", .start_line = 3,
         .end_line = 4, .properties = "{}"},
        {.id = 3, .project = "proj", .label = "Function", .name = "Gamma",
         .qualified_name = "proj.src.b.Gamma", .file_path = "src/b.c", .start_line = 1,
         .end_line = 2, .properties = "{}"},
        {.id = 4, .project = "proj", .label = "Class", .name = "Widget",
         .qualified_name = "proj.lib.c.Widget", .file_path = "lib/c.py", .start_line = 1,
         .end_line = 9, .properties = "{}"},
        {.id = 5, .project = "proj", .label = "File", .name = "a.c",
         .qualified_name = "proj.src.a", .file_path = "src/a.c", .properties = "{}"},
        {.id = 6, .project = "proj", .label = "File", .name = "c.py",
         .qualified_name = "proj.lib.c", .file_path = "lib/c.py", .properties = "{}"},
    };
    const CBMDumpEdge edges[] = {
        {.id = 1, .project = "proj", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{\"args\":\"x\"}", .local_name = "Beta"},
        {.id = 2, .project = "proj", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{}", .local_name = "BetaAlias"},
        {.id = 3, .project = "proj", .source_id = 3, .target_id = 2, .type = "CALLS",
         .properties = "{}"},
        {.id = 4, .project = "proj", .source_id = 4, .target_id = 1, .type = "USAGE",
         .properties = "{}"},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/zova-schema", .project = "proj",
        .indexed_at = "2026-10-13T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 2,
        .nodes = nodes, .node_count = 6, .edges = edges, .edge_count = 4,
    };
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(path, &input, &published), 0);
    cbm_store_t *store = cbm_store_open_zova_workspace_query(path, published.workspace_id);
    ASSERT_NOT_NULL(store);

    /* The store is one workspace, so the project selector does not filter. */
    ASSERT_EQ(cbm_store_count_nodes(store, "any-selector"), 6);
    ASSERT_EQ(cbm_store_count_edges(store, "any-selector"), 4);
    ASSERT_EQ(cbm_store_count_edges_by_type(store, "proj", "CALLS"), 3);
    ASSERT_EQ(cbm_store_count_nodes_scoped(store, "proj", "src"), 4);
    ASSERT_EQ(cbm_store_count_edges_scoped(store, "proj", "src"), 3);

    cbm_schema_info_t schema = {0};
    ASSERT_EQ(cbm_store_get_schema(store, "proj", &schema), CBM_STORE_OK);
    ASSERT_EQ(schema.node_label_count, 3);
    ASSERT_STR_EQ(schema.node_labels[0].label, "Function");
    ASSERT_EQ(schema.node_labels[0].count, 3);
    bool has_complexity = false;
    for (int i = 0; i < schema.node_labels[0].property_count; i++) {
        has_complexity |= strcmp(schema.node_labels[0].properties[i], "complexity") == 0;
    }
    ASSERT_TRUE(has_complexity);
    ASSERT_EQ(schema.edge_type_count, 2);
    ASSERT_STR_EQ(schema.edge_types[0].type, "CALLS");
    ASSERT_EQ(schema.edge_types[0].count, 3);
    bool has_args = false;
    for (int i = 0; i < schema.edge_types[0].property_count; i++) {
        has_args |= strcmp(schema.edge_types[0].properties[i], "args") == 0;
    }
    ASSERT_TRUE(has_args);
    ASSERT_EQ(schema.edge_types[0].property_count, 3);
    ASSERT_STR_EQ(schema.edge_types[1].type, "USAGE");
    ASSERT_EQ(schema.edge_types[1].count, 1);
    ASSERT_EQ(schema.edge_types[1].property_count, 2);
    cbm_store_schema_free(&schema);

    ASSERT_EQ(cbm_store_get_schema_counts_scoped(store, "proj", "lib", &schema), CBM_STORE_OK);
    ASSERT_EQ(schema.node_label_count, 2);
    ASSERT_EQ(schema.edge_type_count, 0);
    cbm_store_schema_free(&schema);
    ASSERT_EQ(cbm_store_get_schema_counts_scoped(store, "proj", "src", &schema), CBM_STORE_OK);
    ASSERT_EQ(schema.edge_type_count, 1);
    ASSERT_STR_EQ(schema.edge_types[0].type, "CALLS");
    ASSERT_EQ(schema.edge_types[0].count, 3);
    cbm_store_schema_free(&schema);
    ASSERT_EQ(cbm_store_count_edges_by_type(store, "proj", "USAGE"), 1);

    cbm_architecture_info_t arch = {0};
    ASSERT_EQ(cbm_store_get_architecture(store, "proj", NULL, NULL, 0, &arch), CBM_STORE_OK);
    ASSERT_EQ(arch.language_count, 2);
    ASSERT_EQ(arch.hotspot_count, 1);
    ASSERT_STR_EQ(arch.hotspots[0].name, "Beta");
    ASSERT_EQ(arch.hotspots[0].fan_in, 3);
    ASSERT_EQ(arch.boundary_count, 1);
    ASSERT_STR_EQ(arch.boundaries[0].from, "b");
    ASSERT_STR_EQ(arch.boundaries[0].to, "a");
    ASSERT_EQ(arch.boundaries[0].call_count, 1);
    ASSERT_EQ(arch.cluster_count, 1);
    ASSERT_EQ(arch.clusters[0].members, 3);
    ASSERT_GT(arch.file_tree_count, 0);
    cbm_store_architecture_free(&arch);

    /* Edges are read from the Zova graph, not copied into a temp table. */
    sqlite3_stmt *temp_edges = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(cbm_store_get_db(store),
                                 "SELECT count(*) FROM sqlite_temp_master WHERE name='edges'",
                                 -1, &temp_edges, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(temp_edges), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(temp_edges, 0), 0);
    sqlite3_finalize(temp_edges);

    char **files = NULL;
    int file_count = 0;
    ASSERT_EQ(cbm_store_list_files(store, "any-selector", &files, &file_count), CBM_STORE_OK);
    /* Canonical File-node paths only: src/b.c has symbols but no File node. */
    ASSERT_EQ(file_count, 2);
    for (int i = 0; i < file_count; i++) free(files[i]);
    free(files);

    /* A short name resolves to the same node as its full qualified name. */
    cbm_node_t exact = {0};
    ASSERT_EQ(cbm_store_find_node_by_qn(store, "proj", "proj.src.a.Beta", &exact), CBM_STORE_OK);
    cbm_node_t *matches = NULL;
    int match_count = 0;
    ASSERT_EQ(cbm_store_find_nodes_by_qn_suffix(store, "any-selector", "Beta", &matches,
                                                &match_count),
              CBM_STORE_OK);
    ASSERT_EQ(match_count, 1);
    ASSERT_STR_EQ(matches[0].qualified_name, "proj.src.a.Beta");
    ASSERT_EQ(matches[0].id, exact.id);
    cbm_store_free_nodes(matches, match_count);

    /* Edges by type carry the same node ids as the node reads. */
    cbm_node_t alpha = {0};
    cbm_node_t gamma = {0};
    ASSERT_EQ(cbm_store_find_node_by_qn(store, "proj", "proj.src.a.Alpha", &alpha),
              CBM_STORE_OK);
    ASSERT_EQ(cbm_store_find_node_by_qn(store, "proj", "proj.src.b.Gamma", &gamma),
              CBM_STORE_OK);
    cbm_edge_t *calls = NULL;
    int call_count = 0;
    ASSERT_EQ(cbm_store_find_edges_by_type(store, "proj", "CALLS", &calls, &call_count),
              CBM_STORE_OK);
    ASSERT_EQ(call_count, 3);
    int from_alpha = 0;
    int from_gamma = 0;
    int with_args = 0;
    for (int i = 0; i < call_count; i++) {
        ASSERT_EQ(calls[i].target_id, exact.id);
        ASSERT_STR_EQ(calls[i].type, "CALLS");
        from_alpha += calls[i].source_id == alpha.id;
        from_gamma += calls[i].source_id == gamma.id;
        with_args += strcmp(calls[i].properties_json, "{\"args\":\"x\"}") == 0;
    }
    ASSERT_EQ(from_alpha, 2);
    ASSERT_EQ(from_gamma, 1);
    ASSERT_EQ(with_args, 1);
    ASSERT_NEQ(calls[0].id, calls[1].id);
    cbm_store_free_edges(calls, call_count);
    cbm_node_free_fields(&alpha);
    cbm_node_free_fields(&gamma);

    char **callers = NULL;
    char **callees = NULL;
    int caller_count = 0;
    int callee_count = 0;
    ASSERT_EQ(cbm_store_node_neighbor_names(store, exact.id, 10, &callers, &caller_count,
                                            &callees, &callee_count),
              0);
    ASSERT_EQ(caller_count, 2);
    ASSERT_STR_EQ(callers[0], "Alpha");
    ASSERT_STR_EQ(callers[1], "Gamma");
    ASSERT_EQ(callee_count, 0);
    for (int i = 0; i < caller_count; i++) free(callers[i]);
    free(callers);
    free(callees);
    cbm_node_free_fields(&exact);

    cbm_store_close(store);
    cbm_unlink(path);
    PASS();
}
#endif

SUITE(zova_normalization_benchmark) {
    RUN_TEST(zova_prepared_view_normalization_benchmark);
}

SUITE(zova) {
    RUN_TEST(zova_edge_payload_codec_is_compact_deterministic_and_strict);
#ifndef _WIN32
    RUN_TEST(zova_writer_gate_serializes_processes_and_releases_after_crash);
    RUN_TEST(file_lock_modes_conflict_across_processes_and_release_on_crash);
    RUN_TEST(zova_coordination_lease_and_barrier_in_process);
    RUN_TEST(zova_coordination_lease_excludes_other_processes_and_survives_crash);
    RUN_TEST(zova_coordination_waiting_writer_blocks_new_index_runs);
#endif
    RUN_TEST(zova_route_uses_user_cache_database);
    RUN_TEST(zova_workspace_scoped_names_and_node_ids_are_deterministic);
    RUN_TEST(zova_workspace_registry_path_uses_cache_dir);
#if !CBM_WITH_ZOVA
#else
    RUN_TEST(zova_workspace_id_validation_rejects_missing_and_malformed_values);
    RUN_TEST(zova_repository_requires_ready_workspace_and_reads_metadata);
    RUN_TEST(zova_workspace_registry_preserves_ready_generation);
    RUN_TEST(zova_workspace_registry_lookup_is_read_only);
    RUN_TEST(zova_user_database_bootstrap_creates_versioned_workspace_schema);
    RUN_TEST(zova_user_database_schema_uses_zova_as_sole_topology_authority);
    RUN_TEST(zova_database_format_status_requires_atomic_repack_for_v5_v7);
    RUN_TEST(zova_user_database_has_one_canonical_fts_without_rowmap);
    RUN_TEST(zova_user_database_generation_state_commits_and_rolls_back);
    RUN_TEST(zova_graph_vector_objects_obey_caller_transaction);
    RUN_TEST(zova_atomic_publisher_reader_snapshot_is_generation_consistent);
    RUN_TEST(zova_user_database_capability_probe_covers_empty_schema_surface);
    RUN_TEST(zova_user_database_imports_workspace_metadata_and_fts);
    RUN_TEST(zova_catalog_assigns_stable_shortest_path_selectors);
    RUN_TEST(zova_catalog_semantic_search_merges_workspace_collections_globally);
    RUN_TEST(zova_user_database_delete_workspace_is_isolated_and_idempotent);
    RUN_TEST(zova_atomic_workspace_publisher_commits_complete_generation);
    RUN_TEST(zova_full_edge_publication_batches_128_rows);
    RUN_TEST(zova_workspace_vector_search_uses_native_node_and_token_collections);
    RUN_TEST(zova_graph_session_hydrates_scoped_directional_walk);
    RUN_TEST(zova_gbuf_prepare_owns_full_publication_view);
    RUN_TEST(zova_publish_model_is_deterministic_and_validates_identity);
    RUN_TEST(zova_workspace_delta_is_exact_and_deterministic);
    RUN_TEST(zova_delta_digest_planner_selects_only_changed_components);
    RUN_TEST(zova_atomic_delta_publisher_preserves_old_generation_on_every_fault);
    RUN_TEST(zova_publish_model_dense_camel_name_is_owned_without_overflow);
    RUN_TEST(zova_publish_model_uses_bounded_row_storage_allocations);
    RUN_TEST(zova_prepared_view_uses_implicit_default_edge_payload);
    RUN_TEST(zova_prepared_view_encodes_single_metadata_edge_without_group_scratch);
    RUN_TEST(zova_workspace_store_reports_schema_counts_and_architecture);
#endif
}
