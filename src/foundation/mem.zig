//! mem.zig — Unified memory management via mimalloc.
//!
//! Implements src/foundation/mem.h. Budget tracking based on actual RSS via
//! mi_process_info(). When MI_OVERRIDE=0 (ASan builds), falls back to
//! OS-specific RSS queries (task_info on macOS, /proc/self/statm on Linux,
//! GetProcessMemoryInfo on Windows).

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("interop.zig");

const is_windows = builtin.os.tag == .windows;
const is_apple = builtin.os.tag.isDarwin();
const is_linux = builtin.os.tag == .linux;

const MAX_RAM_FRACTION = 1.0;
const DEFAULT_RAM_FRACTION = 0.5;

// ── Static state ─────────────────────────────────────────────────

var g_budget: usize = 0; // budget in bytes
var g_initialized = std.atomic.Value(c_int).init(0); // init guard
var g_was_over = std.atomic.Value(c_int).init(0); // pressure hysteresis
// 1 = the user set CBM_MEM_BUDGET_MB, 0 = RAM-fraction detection chose it.
// Post-extraction phases only hard-fail on an explicit budget: the detected
// default is a soft target that extraction already handles with spill.
var g_budget_explicit = std.atomic.Value(c_int).init(0);

const MB_DIVISOR: usize = 1024 * 1024;

// ── OS fallback for RSS (ASan builds where MI_OVERRIDE=0) ────────

fn osRss() usize {
    if (is_windows) {
        var pmc: c.PROCESS_MEMORY_COUNTERS = undefined;
        if (c.K32GetProcessMemoryInfo(c.GetCurrentProcess(), &pmc, @sizeOf(c.PROCESS_MEMORY_COUNTERS)) != 0) {
            return pmc.WorkingSetSize;
        }
        return 0;
    } else if (is_apple) {
        var info = std.mem.zeroes(c.struct_mach_task_basic_info);
        var count: c.mach_msg_type_number_t = @intCast(c.MACH_TASK_BASIC_INFO_COUNT);
        if (c.task_info(c.mach_task_self(), c.MACH_TASK_BASIC_INFO, @ptrCast(&info), &count) == c.KERN_SUCCESS) {
            return @intCast(info.resident_size);
        }
        return 0;
    } else {
        const f = c.fopen("/proc/self/statm", "r") orelse return 0;
        var pages: c_ulong = 0;
        var rss_pages: c_ulong = 0;
        if (c.fscanf(f, "%lu %lu", &pages, &rss_pages) != 2) rss_pages = 0;
        _ = c.fclose(f);
        const ps = c.sysconf(c._SC_PAGESIZE);
        return @as(usize, rss_pages) *% (if (ps > 0) @as(usize, @intCast(ps)) else 4096);
    }
}

// ── Pressure logging (hysteresis) ────────────────────────────────

fn checkPressure(rss: usize) void {
    if (g_budget == 0) return;
    const over = rss > g_budget;
    const was = g_was_over.load(.seq_cst) != 0;
    if (over == was) return;

    g_was_over.store(@intFromBool(over), .seq_cst);
    var rss_mb: [32]u8 = undefined;
    var budget_mb: [32]u8 = undefined;
    var pct_str: [16]u8 = undefined;
    _ = interop.bprint(&rss_mb, "{d}", .{rss / MB_DIVISOR});
    _ = interop.bprint(&budget_mb, "{d}", .{g_budget / MB_DIVISOR});
    _ = interop.bprint(&pct_str, "{d}", .{(rss *% 100) / g_budget});
    interop.log(
        if (over) c.CBM_LOG_WARN else c.CBM_LOG_INFO,
        if (over) "mem.pressure.warn" else "mem.pressure.ok",
        .{ "rss_mb", cstr(&rss_mb), "budget_mb", cstr(&budget_mb), "pct", cstr(&pct_str) },
    );
}

fn cstr(buf: []const u8) interop.Str {
    return @ptrCast(buf.ptr);
}

// ── Public API ───────────────────────────────────────────────────

const RAM_FRACTION_DEFAULT = 0.5;
const RAM_FRACTION_16GB = 0.25;
const RAM_FRACTION_32GB = 0.35;
const RAM_BYTES_PER_GB: u64 = 1024 * 1024 * 1024;

pub export fn cbm_mem_ram_fraction_for_total(total_ram_bytes: usize) f64 {
    if (total_ram_bytes <= 16 * RAM_BYTES_PER_GB) return RAM_FRACTION_16GB;
    if (total_ram_bytes <= 32 * RAM_BYTES_PER_GB) return RAM_FRACTION_32GB;
    return RAM_FRACTION_DEFAULT;
}

// Bounds for the free-memory clamp.
const AVAIL_HEADROOM_MIN: usize = RAM_BYTES_PER_GB;
const AVAIL_HEADROOM_MAX: usize = 8 * RAM_BYTES_PER_GB;
const BUDGET_FLOOR: usize = RAM_BYTES_PER_GB / 2;

/// Total RAM is the ceiling for a budget; what is free right now is the
/// constraint. A budget sized from total RAM alone plans to use memory that
/// may already belong to other programs (#138).
pub export fn cbm_mem_clamp_to_available(budget: usize, available: usize) usize {
    if (available == 0) return budget;
    const headroom = std.math.clamp(available / 4, AVAIL_HEADROOM_MIN, AVAIL_HEADROOM_MAX);
    const cap = @max(available -| headroom, BUDGET_FLOOR);
    return @min(budget, cap);
}

pub export fn cbm_mem_init(ram_fraction_in: f64) void {
    if (g_initialized.cmpxchgStrong(0, 1, .seq_cst, .seq_cst) != null) return;

    var ram_fraction = ram_fraction_in;
    // Written so that NaN keeps flowing through, as in C.
    if (ram_fraction <= 0.0 or ram_fraction > MAX_RAM_FRACTION) {
        ram_fraction = DEFAULT_RAM_FRACTION;
    }

    // Reduce upfront memory: don't eagerly commit arenas.
    // Force decommit on purge (MADV_FREE_REUSABLE on macOS) so RSS
    // drops immediately instead of staying high until memory pressure.
    c.mi_option_set(c.mi_option_arena_eager_commit, 0);
    c.mi_option_set(c.mi_option_purge_decommits, 1);
    c.mi_option_set(c.mi_option_purge_delay, 0); // immediate purge, no 1s delay

    // v3 (#832): reclaim abandoned pages on ANY thread's free (=1), restoring the
    // v2 behaviour. mimalloc v3 defaults page_reclaim_on_free=0, so pages a worker
    // thread abandons at exit are NOT reclaimed when the main thread later frees
    // their blocks (and mi_collect cannot touch abandoned pages) — RSS then
    // ratchets across repeated in-process index cycles. The supervised subprocess
    // is the primary cure (the child returns 100% RSS on exit); this is the
    // in-process fallback for any path that stays in-process (kill switch,
    // spawn-fail degrade, embedders).
    c.mi_option_set(c.mi_option_page_reclaim_on_free, 1);

    // CBM_MEM_BUDGET_MB env override (memory analogue of CBM_WORKERS).
    // Lets users cap the budget directly without an enclosing cgroup —
    // useful on bare-metal hosts where cgroup memory limits are absent
    // (#363). Explicit override > implicit RAM/cgroup detection.
    var env_buf: [32]u8 = undefined;
    if (c.cbm_safe_getenv("CBM_MEM_BUDGET_MB", &env_buf, env_buf.len, null) != null) {
        // Whole-string read: "512MB" or " 512" is refused, not read as 512.
        var mb: c_long = 0;
        if (!c.cbm_env_long("CBM_MEM_BUDGET_MB", &mb)) mb = 0;
        if (mb > 0) {
            g_budget = @as(usize, @intCast(mb)) *% MB_DIVISOR;
            g_budget_explicit.store(1, .seq_cst);
            var ovr_mb: [32]u8 = undefined;
            _ = interop.bprint(&ovr_mb, "{d}", .{mb});
            interop.logInfo("mem.init", .{ "budget_mb", cstr(&ovr_mb), "source", "CBM_MEM_BUDGET_MB" });
            // The user's choice stands, but say so when it exceeds what is free.
            const free_now = c.cbm_system_available_ram();
            if (free_now > 0 and g_budget > free_now) {
                var free_mb: [32]u8 = undefined;
                _ = interop.bprint(&free_mb, "{d}", .{free_now / MB_DIVISOR});
                interop.logWarn("mem.budget.over_available", .{ "budget_mb", cstr(&ovr_mb), "available_mb", cstr(&free_mb) });
            }
            return;
        }
        interop.logWarn("mem.budget.env.invalid", .{ "value", cstr(&env_buf), "fallback", "ram_fraction" });
    }

    const info = c.cbm_system_info();
    const from_total: usize = @intFromFloat(@as(f64, @floatFromInt(info.total_ram)) * ram_fraction);
    const available = c.cbm_system_available_ram();
    g_budget = cbm_mem_clamp_to_available(from_total, available);

    var budget_mb: [32]u8 = undefined;
    var ram_mb: [32]u8 = undefined;
    var avail_mb: [32]u8 = undefined;
    _ = interop.bprint(&budget_mb, "{d}", .{g_budget / MB_DIVISOR});
    _ = interop.bprint(&ram_mb, "{d}", .{info.total_ram / MB_DIVISOR});
    _ = interop.bprint(&avail_mb, "{d}", .{available / MB_DIVISOR});
    interop.logInfo("mem.init", .{ "budget_mb", cstr(&budget_mb), "total_ram_mb", cstr(&ram_mb), "available_mb", cstr(&avail_mb), "source", @as(interop.Str, if (g_budget < from_total) "available_ram" else "ram_fraction") });
}

pub export fn cbm_mem_rss() usize {
    if (is_linux) {
        // Linux: mimalloc's _mi_prim_process_info() (vendored/mimalloc/src/prim/
        // unix/prim.c) never sets pinfo->current_rss on Linux — it only sets
        // peak_rss (from getrusage's ru_maxrss). current_rss therefore keeps
        // mi_process_info()'s default of pinfo.current_commit: mimalloc's OWN
        // committed-page counter, which this project deliberately tunes low via
        // mi_option_arena_eager_commit=0 + purge_decommits=1 + purge_delay=0
        // (cbm_mem_init) to reduce upfront memory. So on Linux "current_rss" is a
        // low-biased mimalloc-internal metric, not true RSS: under concurrent
        // large-file parsing it can read a few MB while real RSS is multiple GB,
        // silently blinding cbm_mem_over_budget()'s backpressure to real memory
        // pressure (small-but-nonzero, so the `current_rss > 0` guard below never
        // catches it). osRss() reads /proc/self/statm — authoritative OS RSS,
        // unaffected by mimalloc's accounting — so it is the PRIMARY source on
        // Linux, not a last-resort fallback. macOS/Windows are unaffected:
        // mimalloc sets current_rss correctly there via task_info /
        // GetProcessMemoryInfo.
        const proc_rss = osRss();
        if (proc_rss > 0) return proc_rss;
        // Extremely unlikely (/proc unavailable) — fall through to mimalloc.
    }
    var current_rss: usize = 0;
    var peak_rss: usize = 0;
    c.mi_process_info(null, null, null, &current_rss, &peak_rss, null, null, null);
    if (current_rss > 0) return current_rss;
    // Fallback for ASan builds (MI_OVERRIDE=0) and any platform where
    // mimalloc's current_rss is unavailable/zero.
    return osRss();
}

pub export fn cbm_mem_peak_rss() usize {
    var peak_rss: usize = 0;
    c.mi_process_info(null, null, null, null, &peak_rss, null, null, null);
    // Peak RSS is by definition >= current RSS. On Linux cbm_mem_rss() reads the
    // live /proc/self/statm value (page-granular), while mimalloc's peak_rss
    // comes from getrusage's ru_maxrss (KB-granular, and it can lag the live
    // statm read by a few pages). Reading the two sources independently lets a
    // fresh current read momentarily exceed the reported peak, breaking the
    // peak >= current invariant. Reconcile them: the true peak is at least the
    // current RSS. (Not observable on macOS, where both come from mimalloc.)
    const current = cbm_mem_rss();
    if (current > peak_rss) peak_rss = current;
    if (peak_rss > 0) return peak_rss;
    // No OS fallback for peak — return current as best approximation
    return osRss();
}

pub export fn cbm_mem_budget() usize {
    return g_budget;
}

pub export fn cbm_mem_set_budget_for_tests(bytes: usize, explicit_budget: bool) void {
    g_budget = bytes;
    g_budget_explicit.store(@intFromBool(explicit_budget), .seq_cst);
}

pub export fn cbm_mem_budget_explicit() bool {
    return g_budget_explicit.load(.seq_cst) != 0;
}

pub export fn cbm_mem_over_budget() bool {
    const rss = cbm_mem_rss();
    checkPressure(rss);
    return rss > g_budget;
}

pub export fn cbm_mem_worker_budget(num_workers: c_int) usize {
    const n: usize = if (num_workers <= 0) 1 else @intCast(num_workers);
    return g_budget / n;
}

pub export fn cbm_mem_collect() void {
    c.mi_collect(true);
}
