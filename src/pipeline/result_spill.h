/*
 * result_spill.h — per-file extraction results parked on disk when the
 * parallel extract runs over its memory budget (F1).
 *
 * Fork-specific counterpart of upstream 7b77fd48 (result_spill.c). Upstream
 * compacts a result into one arena block and relocates its pointers on load;
 * this fork encodes the result field by field instead, so it needs no
 * compaction pass and no allocator changes. A parked result is written to one
 * append-only file under <cache>/spill and read back into a fresh
 * CBMFileResult that the caller owns. The retained parse tree is not kept.
 * Nothing here decides WHEN to spill; pass_parallel.c does, on the budget
 * signal.
 */
#ifndef CBM_RESULT_SPILL_H
#define CBM_RESULT_SPILL_H

#include "cbm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cbm_result_spill cbm_result_spill_t;

/* One store for one pipeline run with `slots` result slots (one per input
 * file). Opening removes spill files that earlier, killed runs left in `dir`.
 * The file is created lazily on the first park; on POSIX it is unlinked as
 * soon as it is open, so it cannot outlive the process. No file is created,
 * and parking fails, when `dir` has less free space than
 * cbm_result_spill_free_floor_bytes(). NULL on OOM. */
cbm_result_spill_t *cbm_result_spill_open(const char *dir, int slots);

/* Free space below which spilling does not start (10 GB). */
uint64_t cbm_result_spill_free_floor_bytes(void);

/* TEST HOOK: a nonzero value replaces the free space read from the disk; 0
 * restores the real reading. Returns the previous value. */
uint64_t cbm_result_spill_pin_free_bytes_for_tests(uint64_t bytes);

/* Encode `result` into slot `slot`, then free it (cbm_free_result). Returns
 * false and leaves the result untouched when encoding or the write fails.
 * Thread-safe; each slot is parked at most once. */
bool cbm_result_spill_park(cbm_result_spill_t *sp, int slot, CBMFileResult *result);

/* True when slot `slot` holds a parked result. */
bool cbm_result_spill_has(const cbm_result_spill_t *sp, int slot);

/* Counts recorded when slot `slot` was parked, so a consumer can skip the
 * load when it reads none of them. Zero for an empty slot. */
int cbm_result_spill_infra_binding_count(const cbm_result_spill_t *sp, int slot);
int cbm_result_spill_string_ref_count(const cbm_result_spill_t *sp, int slot);

/* Load slot `slot` into a fresh CBMFileResult the caller owns
 * (cbm_free_result). NULL when the slot is empty or the read fails.
 * Thread-safe. */
CBMFileResult *cbm_result_spill_load(cbm_result_spill_t *sp, int slot);

/* Loads of a parked slot that failed (read error or corrupt record). The
 * slot's file then went missing from a pass, so the run must not publish. */
int64_t cbm_result_spill_load_failures(const cbm_result_spill_t *sp);

/* Results parked, bytes written and loads served, for the log. */
void cbm_result_spill_stats(const cbm_result_spill_t *sp, int64_t *parked, int64_t *bytes,
                            int64_t *loads);

/* Close and delete the file. NULL-safe. */
void cbm_result_spill_close(cbm_result_spill_t *sp);

/* Encoding primitives, exposed for the round-trip tests. encode returns a
 * malloc'd buffer (NULL on OOM); decode returns NULL on a malformed buffer. */
char *cbm_result_spill_encode(const CBMFileResult *result, size_t *out_len);
CBMFileResult *cbm_result_spill_decode(const char *data, size_t len);

/* Result cache access for passes that run after a possible spill: the cached
 * result, else a loaded copy of the parked one, else NULL. Release with
 * cbm_result_cache_release, which frees only a loaded copy. */
CBMFileResult *cbm_result_cache_acquire(CBMFileResult **cache, cbm_result_spill_t *sp, int i);
void cbm_result_cache_release(CBMFileResult **cache, int i, CBMFileResult *result);

/* True when slot i has a result, in memory or parked. */
bool cbm_result_cache_present(CBMFileResult *const *cache, const cbm_result_spill_t *sp, int i);

#endif /* CBM_RESULT_SPILL_H */
