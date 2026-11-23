//! system_info.zig — CPU core count and RAM detection.
//!
//! Implements cbm_system_info / cbm_default_worker_count from
//! src/foundation/platform.h and the cgroup helpers of system_info_internal.h.
//!
//! macOS: sysctlbyname for core counts, hw.memsize for RAM.
//! BSD: sysconf + sysctl(HW_PHYSMEM64 / HW_PHYSMEM).
//! Linux: sysconf + sysinfo(), with cgroup-aware overrides when running
//!        inside a container so the limits reflect the cgroup's effective
//!        CPU quota and memory cap rather than the host's totals.
//! Windows: GetSystemInfo + GlobalMemoryStatusEx.
//!
//! Results are cached after first call (immutable hardware properties).

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("interop.zig");

const os = builtin.os.tag;
const is_linux = os == .linux;

const DEFAULT_CORES = 1;
const MIN_WORKERS = 1;
const CBM_WORKERS_MAX = 256;

// ── macOS detection ──────────────────────────────────────────────

fn sysctlInt(name: [*:0]const u8, fallback: c_int) c_int {
    var val: c_int = 0;
    var len: usize = @sizeOf(c_int);
    if (c.sysctlbyname(name, &val, &len, null, 0) == 0 and val > 0) return val;
    return fallback;
}

fn sysctlSize(name: [*:0]const u8, fallback: usize) usize {
    var val: usize = 0;
    var len: usize = @sizeOf(usize);
    if (c.sysctlbyname(name, &val, &len, null, 0) == 0 and val > 0) return val;
    // Try 64-bit variant
    var val64: u64 = 0;
    len = @sizeOf(u64);
    if (c.sysctlbyname(name, &val64, &len, null, 0) == 0 and val64 > 0) return @truncate(val64);
    return fallback;
}

fn detectSystemMacos() c.cbm_system_info_t {
    var info = std.mem.zeroes(c.cbm_system_info_t);

    info.total_cores = sysctlInt("hw.ncpu", DEFAULT_CORES);
    info.perf_cores = sysctlInt("hw.perflevel0.physicalcpu", info.total_cores);

    // If perflevel sysctls fail (Intel Mac), perf = total
    const eff = sysctlInt("hw.perflevel1.physicalcpu", 0);
    if (info.perf_cores +% eff > info.total_cores) info.perf_cores = info.total_cores;

    info.total_ram = sysctlSize("hw.memsize", 0);
    return info;
}

// ── BSD detection ────────────────────────────────────────────────

fn detectSystemBsd() c.cbm_system_info_t {
    var info = std.mem.zeroes(c.cbm_system_info_t);

    const nprocs = c.sysconf(c._SC_NPROCESSORS_ONLN);
    info.total_cores = if (nprocs > 0) @truncate(nprocs) else 1;
    info.perf_cores = info.total_cores;

    var mib = [2]c_int{ c.CTL_HW, if (os == .openbsd) c.HW_PHYSMEM else c.HW_PHYSMEM64 };
    var physmem: u64 = 0;
    var len: usize = @sizeOf(u64);
    if (c.sysctl(&mib, 2, &physmem, &len, null, 0) == 0 and physmem > 0) {
        info.total_ram = @truncate(physmem);
    }
    return info;
}

// ── Linux detection ──────────────────────────────────────────────

/// Read up to (buf.len-1) bytes from `path` into `buf`, NUL-terminate, and strip
/// trailing whitespace. Returns the (stripped) byte count, or -1 if the file
/// could not be opened or read.
fn readSmallFile(path: [*:0]const u8, buf: []u8) c_int {
    const fp = c.fopen(path, "re") orelse return -1;
    var n = c.fread(buf.ptr, 1, buf.len - 1, fp);
    _ = c.fclose(fp);
    while (n > 0 and (buf[n - 1] == '\n' or buf[n - 1] == ' ' or buf[n - 1] == '\t')) n -= 1;
    buf[n] = 0;
    return @intCast(n);
}

/// The cgroup probes exist on Linux only (system_info_internal.h declares them
/// under __linux__).
pub const linux = if (is_linux) struct {
    /// Effective CPU count from a cgroup file tree. See header for contract.
    pub export fn cbm_detect_cgroup_cpus(cgroup_root: [*:0]const u8) c_int {
        var path: [c.CBM_PATH_MAX]u8 = undefined;
        var buf: [64]u8 = undefined;

        // cgroup v2: "<root>/cpu.max" — "<quota> <period>" or "max <period>".
        _ = c.snprintf(&path, path.len, "%s/cpu.max", cgroup_root);
        if (readSmallFile(@ptrCast(&path), &buf) > 0) {
            if (std.mem.startsWith(u8, std.mem.sliceTo(&buf, 0), "max")) {
                return -1; // no quota → caller falls back to sysconf
            }
            var quota: c_long = 0;
            var period: c_long = 0;
            if (c.sscanf(&buf, "%ld %ld", &quota, &period) == 2 and quota > 0 and period > 0) {
                const n = @divTrunc(quota +% period -% 1, period); // ceil(quota/period)
                return if (n > 0) @truncate(n) else MIN_WORKERS;
            }
            return -1;
        }

        // cgroup v1: ".../cpu/cpu.cfs_quota_us" and ".../cpu/cpu.cfs_period_us".
        // A quota of -1 means unlimited in cgroup v1.
        _ = c.snprintf(&path, path.len, "%s/cpu/cpu.cfs_quota_us", cgroup_root);
        if (readSmallFile(@ptrCast(&path), &buf) <= 0) return -1;
        const quota = c.strtol(&buf, null, 10);
        if (quota <= 0) return -1;

        _ = c.snprintf(&path, path.len, "%s/cpu/cpu.cfs_period_us", cgroup_root);
        if (readSmallFile(@ptrCast(&path), &buf) <= 0) return -1;
        const period = c.strtol(&buf, null, 10);
        if (period <= 0) return -1;

        const n = @divTrunc(quota +% period -% 1, period);
        return if (n > 0) @truncate(n) else MIN_WORKERS;
    }

    /// Effective memory limit from a cgroup file tree. See header for contract.
    pub export fn cbm_detect_cgroup_mem(cgroup_root: [*:0]const u8) usize {
        var path: [c.CBM_PATH_MAX]u8 = undefined;
        var buf: [64]u8 = undefined;

        // cgroup v2: "<root>/memory.max" — "max" or integer bytes.
        _ = c.snprintf(&path, path.len, "%s/memory.max", cgroup_root);
        if (readSmallFile(@ptrCast(&path), &buf) > 0) {
            if (std.mem.startsWith(u8, std.mem.sliceTo(&buf, 0), "max")) return 0;
            var end: [*c]u8 = null;
            const n = c.strtoull(&buf, &end, 10);
            if (end == @as([*c]u8, &buf) or n == 0) return 0;
            return @truncate(n);
        }

        // cgroup v1: ".../memory/memory.limit_in_bytes". The sentinel for
        // "unlimited" is a very large value (~PAGE_COUNTER_MAX); treat anything
        // past half of ULLONG_MAX as effectively unlimited.
        _ = c.snprintf(&path, path.len, "%s/memory/memory.limit_in_bytes", cgroup_root);
        if (readSmallFile(@ptrCast(&path), &buf) <= 0) return 0;
        var end: [*c]u8 = null;
        const n = c.strtoull(&buf, &end, 10);
        if (end == @as([*c]u8, &buf) or n == 0 or n >= std.math.maxInt(c_ulonglong) / 2) return 0;
        return @truncate(n);
    }
} else struct {};

fn detectSystemLinux() c.cbm_system_info_t {
    var info = std.mem.zeroes(c.cbm_system_info_t);

    // Host fallbacks.
    const nprocs = c.sysconf(c._SC_NPROCESSORS_ONLN);
    const host_cpus: c_int = if (nprocs > 0) @truncate(nprocs) else DEFAULT_CORES;

    var host_ram: usize = 0;
    var si: c.struct_sysinfo = undefined;
    if (c.sysinfo(&si) == 0) {
        host_ram = @as(usize, si.totalram) *% @as(usize, si.mem_unit);
    }

    // Cgroup-aware overrides. min(cgroup, host) defends against
    // mis-mounted cgroups that report values larger than the host.
    const cg_cpus = linux.cbm_detect_cgroup_cpus("/sys/fs/cgroup");
    info.total_cores = if (cg_cpus > 0 and cg_cpus < host_cpus) cg_cpus else host_cpus;
    info.perf_cores = info.total_cores; // Linux doesn't distinguish P/E

    const cg_ram = linux.cbm_detect_cgroup_mem("/sys/fs/cgroup");
    info.total_ram = if (cg_ram > 0 and (host_ram == 0 or cg_ram < host_ram)) cg_ram else host_ram;
    return info;
}

// ── Windows detection ────────────────────────────────────────────

fn detectSystemWindows() c.cbm_system_info_t {
    var info = std.mem.zeroes(c.cbm_system_info_t);

    var si: c.SYSTEM_INFO = undefined;
    c.GetSystemInfo(&si);
    info.total_cores = @bitCast(si.dwNumberOfProcessors);
    if (info.total_cores < 1) info.total_cores = 1;
    info.perf_cores = info.total_cores;

    var ms: c.MEMORYSTATUSEX = undefined;
    ms.dwLength = @sizeOf(c.MEMORYSTATUSEX);
    if (c.GlobalMemoryStatusEx(&ms) != 0) {
        info.total_ram = @truncate(ms.ullTotalPhys);
    }
    return info;
}

// ── Public API ───────────────────────────────────────────────────

var info_cached: bool = false;
var cached_info: c.cbm_system_info_t = undefined;

pub export fn cbm_system_info() c.cbm_system_info_t {
    if (!info_cached) {
        cached_info = switch (os) {
            .windows => detectSystemWindows(),
            .macos => detectSystemMacos(),
            .netbsd, .freebsd, .openbsd => detectSystemBsd(),
            else => detectSystemLinux(),
        };
        info_cached = true;
    }
    return cached_info;
}

/// macOS: free + inactive + purgeable pages. Free pages alone would report
/// pressure on any machine that is merely warm; inactive and purgeable pages
/// are reclaimed on demand.
fn availableRamMacos() usize {
    const host = c.mach_host_self();
    var page_size: c.vm_size_t = 0;
    if (c.host_page_size(host, &page_size) != c.KERN_SUCCESS or page_size == 0) return 0;
    var vm = std.mem.zeroes(c.vm_statistics64_data_t);
    var count: c.mach_msg_type_number_t = @sizeOf(c.vm_statistics64_data_t) / @sizeOf(c.integer_t);
    if (c.host_statistics64(host, c.HOST_VM_INFO64, @ptrCast(&vm), &count) != c.KERN_SUCCESS) return 0;
    const pages: u64 = @as(u64, vm.free_count) + vm.inactive_count + vm.purgeable_count;
    return @truncate(pages * page_size);
}

/// Linux: MemAvailable is the kernel's own estimate and counts reclaimable
/// page cache and slab, which MemFree does not.
fn availableRamLinux() usize {
    const f = c.fopen("/proc/meminfo", "re") orelse return 0;
    defer _ = c.fclose(f);
    var line: [256]u8 = undefined;
    const key = "MemAvailable:";
    while (c.fgets(&line, line.len, f) != null) {
        const text = std.mem.sliceTo(&line, 0);
        if (!std.mem.startsWith(u8, text, key)) continue;
        const digits = std.mem.trim(u8, text[key.len..], " \tkB\r\n");
        const kb = std.fmt.parseInt(u64, digits, 10) catch return 0;
        return @truncate(kb *| 1024);
    }
    return 0;
}

fn availableRamWindows() usize {
    var ms: c.MEMORYSTATUSEX = undefined;
    ms.dwLength = @sizeOf(c.MEMORYSTATUSEX);
    if (c.GlobalMemoryStatusEx(&ms) == 0) return 0;
    return @truncate(ms.ullAvailPhys);
}

pub export fn cbm_system_available_ram() usize {
    return switch (os) {
        .windows => availableRamWindows(),
        .macos => availableRamMacos(),
        .netbsd, .freebsd, .openbsd => 0, // unknown rather than guessed
        else => availableRamLinux(),
    };
}

pub export fn cbm_default_worker_count(initial: bool) c_int {
    // CBM_WORKERS env override. Zero means one worker; positive values are
    // accepted in [1, CBM_WORKERS_MAX].
    // Useful inside containers where sysconf(_SC_NPROCESSORS_ONLN)
    // reports host CPUs rather than the cgroup's effective CPU quota.
    // Same precedence shape as other CBM_* env overrides:
    // explicit override > implicit detection.
    var buf: [32]u8 = undefined;
    if (c.cbm_safe_getenv("CBM_WORKERS", &buf, buf.len, null) != null) {
        var end: [*c]u8 = null;
        const n = c.strtol(&buf, &end, 10);
        const whole = end != @as([*c]u8, &buf) and end[0] == 0;
        if (whole and n == 0) return MIN_WORKERS;
        if (whole and n >= MIN_WORKERS and n <= CBM_WORKERS_MAX) return @intCast(n);
        interop.logWarn("workers.env.invalid", .{ "value", @as(interop.Str, @ptrCast(&buf)), "fallback", "sysconf" });
    }

    const info = cbm_system_info();
    if (initial) {
        // Use all cores for initial indexing — user is waiting
        return info.total_cores;
    }
    // Incremental: leave headroom for user's apps
    const workers = info.perf_cores -% 1;
    return if (workers > 0) workers else MIN_WORKERS;
}
