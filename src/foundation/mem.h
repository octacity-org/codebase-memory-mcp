/*
 * mem.h — Unified memory management via mimalloc.
 *
 * Provides budget tracking based on actual RSS (not partial vmem tracking).
 * Uses mi_process_info() as the single source of truth for memory pressure.
 * Replaces the old vmem.h budget-tracked virtual memory allocator.
 */
#ifndef CBM_MEM_H
#define CBM_MEM_H

#include <stdbool.h>
#include <stddef.h>

/* Tiered default fraction for MCP startup: 25% on <=16GB, 35% on <=32GB, else 50%. */
double cbm_mem_ram_fraction_for_total(size_t total_ram_bytes);

/* Limit a budget derived from total RAM to what is free right now. A quarter
 * of `available` (held between 1 and 8 GB) is left as headroom for the OS and
 * other programs. The result is never clamped below 512 MB, and never raised
 * above `budget`. available == 0 means unknown and returns `budget`. */
size_t cbm_mem_clamp_to_available(size_t budget, size_t available);

/* Initialize memory budget = ram_fraction * total_physical_ram, limited to the
 * memory that is free at the time (cbm_mem_clamp_to_available). An explicit
 * CBM_MEM_BUDGET_MB is used as given.
 * Thread-safe: only the first call takes effect.
 * Configures mimalloc options for reduced upfront memory. */
void cbm_mem_init(double ram_fraction);

/* Current RSS in bytes via mi_process_info().
 * Falls back to OS-specific queries when MI_OVERRIDE=0 (ASan builds). */
size_t cbm_mem_rss(void);

/* Peak RSS in bytes. */
size_t cbm_mem_peak_rss(void);

/* Total budget in bytes. */
size_t cbm_mem_budget(void);

/* True when the budget came from CBM_MEM_BUDGET_MB rather than RAM-fraction
 * detection. Post-extraction hard aborts only apply to an explicit budget. */
bool cbm_mem_budget_explicit(void);

/* TEST HOOK: overwrite the budget directly, bypassing cbm_mem_init's
 * init-once guard (a setenv+re-init dance in tests is a silent no-op once
 * some earlier init won the guard — the poisoned budget then leaks into
 * every later budget consumer in the process). Does NOT flip the init
 * guard: a later cbm_mem_init still initializes normally. `explicit_budget`
 * sets cbm_mem_budget_explicit(). Callers must save cbm_mem_budget() and
 * cbm_mem_budget_explicit() first and restore both before their assertions.
 * Never call from production code. */
void cbm_mem_set_budget_for_tests(size_t bytes, bool explicit_budget);

/* Returns true if current RSS exceeds the budget. */
bool cbm_mem_over_budget(void);

/* Per-worker budget hint: budget / num_workers. */
size_t cbm_mem_worker_budget(int num_workers);

/* Return unused pages to the OS. Call between files to bound per-file peak. */
void cbm_mem_collect(void);

#endif /* CBM_MEM_H */
