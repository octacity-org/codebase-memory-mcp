//! result_spill.zig — see result_spill.h.
//!
//! Encoding: each record is written as its raw struct bytes (every scalar
//! field, whatever its type) followed by its string and array fields in a
//! fixed order. Decoding copies the raw bytes back and then replaces every
//! pointer field with a copy in the new result's arena. A pointer field added
//! to one of these structs must be added here too; the size assertions below
//! fail the build until it is.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const util = @import("util.zig");

const CStr = util.CStr;
const MutStr = util.MutStr;
const ix = util.ix;
const FileResult = c.CBMFileResult;
const AtomicI64 = std.atomic.Value(i64);
const is_windows = builtin.os.tag == .windows;

comptime {
    if (@sizeOf(usize) == 8) {
        const sizes = .{
            .{ c.CBMDefinition, 248 }, .{ c.CBMCall, 376 },     .{ c.CBMCallArg, 40 },
            .{ c.CBMImport, 32 },      .{ c.CBMUsage, 32 },     .{ c.CBMThrow, 16 },
            .{ c.CBMReadWrite, 24 },   .{ c.CBMTypeRef, 16 },   .{ c.CBMEnvAccess, 16 },
            .{ c.CBMTypeAssign, 24 },  .{ c.CBMStringRef, 32 }, .{ c.CBMInfraBinding, 24 },
            .{ c.CBMChannel, 32 },     .{ c.CBMImplTrait, 16 }, .{ c.CBMResolvedCall, 56 },
            .{ FileResult, 4480 },
        };
        for (sizes) |entry| {
            if (@sizeOf(entry[0]) != entry[1]) {
                @compileError("update result_spill.zig for " ++ @typeName(entry[0]));
            }
        }
    }
}

const SPILL_NULL: u32 = 0xFFFFFFFF;
const SPILL_MAGIC: u32 = 0x31535243; // "CRS1"

// ── Encoder ─────────────────────────────────────────────────────────

const SpillBuf = struct {
    data: MutStr = null,
    len: usize = 0,
    cap: usize = 0,
    failed: bool = false,
};

fn put(b: *SpillBuf, bytes: []const u8) void {
    const n = bytes.len;
    if (b.failed or n == 0) return;
    if (n > std.math.maxInt(usize) - b.len) {
        b.failed = true;
        return;
    }
    if (b.len + n > b.cap) {
        var cap: usize = if (b.cap != 0) b.cap else 4096;
        while (cap < b.len + n) cap *= 2;
        const data: MutStr = @ptrCast(c.realloc(b.data, cap));
        if (data == null) {
            b.failed = true;
            return;
        }
        b.data = data;
        b.cap = cap;
    }
    @memcpy(b.data[b.len..][0..n], bytes);
    b.len += n;
}

fn putU32(b: *SpillBuf, v: u32) void {
    put(b, std.mem.asBytes(&v));
}

fn putBytes(b: *SpillBuf, s: CStr, n: usize) void {
    if (s == null) {
        putU32(b, SPILL_NULL);
        return;
    }
    if (n >= SPILL_NULL) {
        b.failed = true;
        return;
    }
    putU32(b, @intCast(n));
    put(b, s[0..n]);
}

fn putStr(b: *SpillBuf, s: CStr) void {
    putBytes(b, s, if (s != null) c.strlen(s) else 0);
}

/// NULL-terminated string array.
fn putStrv(b: *SpillBuf, v: [*c]const CStr) void {
    if (v == null) {
        putU32(b, SPILL_NULL);
        return;
    }
    var n: u32 = 0;
    while (v[n] != null) n += 1;
    putU32(b, n);
    for (0..n) |i| putStr(b, v[i]);
}

// ── Decoder ─────────────────────────────────────────────────────────

const SpillRd = struct {
    p: [*]const u8,
    left: usize,
    arena: *c.CBMArena,
    failed: bool = false,
};

fn take(r: *SpillRd, out: []u8) void {
    const n = out.len;
    if (r.failed or n > r.left) {
        r.failed = true;
        @memset(out, 0);
        return;
    }
    @memcpy(out, r.p[0..n]);
    r.p += n;
    r.left -= n;
}

fn takeU32(r: *SpillRd) u32 {
    var v: u32 = 0;
    take(r, std.mem.asBytes(&v));
    return v;
}

/// A length-prefixed byte string copied into the arena and NUL-terminated.
fn takeBytes(r: *SpillRd, out_len: ?*c_int) CStr {
    const n = takeU32(r);
    if (out_len) |ol| ol.* = 0;
    if (r.failed or n == SPILL_NULL) return null;
    if (n > r.left) {
        r.failed = true;
        return null;
    }
    const s: MutStr = @ptrCast(c.cbm_arena_alloc(r.arena, @as(usize, n) + 1));
    if (s == null) {
        r.failed = true;
        return null;
    }
    @memcpy(s[0..n], r.p[0..n]);
    s[n] = 0;
    r.p += n;
    r.left -= n;
    if (out_len) |ol| ol.* = @bitCast(n);
    return s;
}

fn takeStr(r: *SpillRd) CStr {
    return takeBytes(r, null);
}

fn takeStrv(r: *SpillRd) [*c]CStr {
    const n = takeU32(r);
    if (r.failed or n == SPILL_NULL) return null;
    // Each entry costs at least its 4-byte length.
    if (n > r.left / @sizeOf(u32)) {
        r.failed = true;
        return null;
    }
    const v: [*c]CStr = @ptrCast(@alignCast(c.cbm_arena_alloc(r.arena, (@as(usize, n) + 1) * @sizeOf(CStr))));
    if (v == null) {
        r.failed = true;
        return null;
    }
    for (0..n) |i| v[i] = takeStr(r);
    v[n] = null;
    return v;
}

/// Array header: the item count, then that many raw records. Returns arena
/// storage for the records (NULL for zero) with the count in out_count.
fn takeItems(r: *SpillRd, item_size: usize, out_count: *c_int) ?*anyopaque {
    const n = takeU32(r);
    out_count.* = 0;
    if (r.failed or n == 0) return null;
    if (n > std.math.maxInt(i32) or n > r.left / item_size) {
        r.failed = true;
        return null;
    }
    const items = c.cbm_arena_alloc(r.arena, @as(usize, n) * item_size) orelse {
        r.failed = true;
        return null;
    };
    take(r, @as([*]u8, @ptrCast(items))[0 .. @as(usize, n) * item_size]);
    out_count.* = @intCast(n);
    return items;
}

// ── Records ─────────────────────────────────────────────────────────

// Each record type lists its pointer fields once. An array is written as its
// count, then every record's raw bytes with the pointer fields nulled (so
// encoding is a pure function of the content), then each record's strings in
// field order.

const DEF_STRS = [_][]const u8{
    "name",         "qualified_name",     "label",       "file_path",    "signature",
    "return_type",  "receiver",           "docstring",   "parent_class", "route_path",
    "route_method", "structural_profile", "body_tokens", "variants",     "alias_value",
    "http_client",  "http_base_url",
};
const DEF_STRVS = [_][]const u8{ "decorators", "base_classes", "param_names", "param_types", "return_types" };

const Def = struct {
    const T = c.CBMDefinition;
    const Fingerprint = @typeInfo(@FieldType(T, "fingerprint")).pointer.child;

    fn nullFields(x: *T) void {
        inline for (DEF_STRS) |f| @field(x, f) = null;
        inline for (DEF_STRVS) |f| @field(x, f) = null;
        x.fingerprint = null;
    }

    fn enc(b: *SpillBuf, x: *const T) void {
        inline for (DEF_STRS) |f| putStr(b, @field(x, f));
        inline for (DEF_STRVS) |f| putStrv(b, @ptrCast(@field(x, f)));
        if (x.fingerprint != null and x.fingerprint_k > 0) {
            const k = ix(x.fingerprint_k);
            putU32(b, @intCast(k));
            put(b, std.mem.sliceAsBytes(x.fingerprint[0..k]));
        } else {
            putU32(b, SPILL_NULL);
        }
    }

    fn dec(r: *SpillRd, x: *T) void {
        inline for (DEF_STRS) |f| @field(x, f) = takeStr(r);
        inline for (DEF_STRVS) |f| @field(x, f) = @ptrCast(takeStrv(r));
        const k = takeU32(r);
        x.fingerprint = null;
        if (r.failed or k == SPILL_NULL) return;
        if (k > r.left / @sizeOf(Fingerprint)) {
            r.failed = true;
            return;
        }
        x.fingerprint = @ptrCast(@alignCast(c.cbm_arena_alloc(r.arena, @as(usize, k) * @sizeOf(Fingerprint))));
        if (x.fingerprint == null) {
            r.failed = true;
            return;
        }
        take(r, std.mem.sliceAsBytes(x.fingerprint[0..k]));
    }
};

const CALL_STRS = [_][]const u8{ "callee_name", "enclosing_func_qn", "first_string_arg", "second_arg_name" };
const ARG_STRS = [_][]const u8{ "expr", "value", "url_value", "keyword" };

const Call = struct {
    const T = c.CBMCall;

    fn nullFields(x: *T) void {
        inline for (CALL_STRS) |f| @field(x, f) = null;
        for (&x.args) |*arg| {
            inline for (ARG_STRS) |f| @field(arg, f) = null;
        }
    }

    fn enc(b: *SpillBuf, x: *const T) void {
        inline for (CALL_STRS) |f| putStr(b, @field(x, f));
        for (&x.args) |*arg| {
            inline for (ARG_STRS) |f| putStr(b, @field(arg, f));
        }
    }

    fn dec(r: *SpillRd, x: *T) void {
        inline for (CALL_STRS) |f| @field(x, f) = takeStr(r);
        for (&x.args) |*arg| {
            inline for (ARG_STRS) |f| @field(arg, f) = takeStr(r);
        }
    }
};

/// Records whose pointer fields are only strings.
fn StringRecord(comptime Rec: type, comptime fields: []const []const u8) type {
    return struct {
        const T = Rec;

        fn nullFields(x: *T) void {
            inline for (fields) |f| @field(x, f) = null;
        }

        fn enc(b: *SpillBuf, x: *const T) void {
            inline for (fields) |f| putStr(b, @field(x, f));
        }

        fn dec(r: *SpillRd, x: *T) void {
            inline for (fields) |f| @field(x, f) = takeStr(r);
        }
    };
}

const Import = StringRecord(c.CBMImport, &.{ "local_name", "module_path", "member" });
const Usage = StringRecord(c.CBMUsage, &.{ "ref_name", "enclosing_func_qn" });
const Throw = StringRecord(c.CBMThrow, &.{ "exception_name", "enclosing_func_qn" });
const ReadWrite = StringRecord(c.CBMReadWrite, &.{ "var_name", "enclosing_func_qn" });
const TypeRef = StringRecord(c.CBMTypeRef, &.{ "type_name", "enclosing_func_qn" });
const EnvAccess = StringRecord(c.CBMEnvAccess, &.{ "env_key", "enclosing_func_qn" });
const TypeAssign = StringRecord(c.CBMTypeAssign, &.{ "var_name", "type_name", "enclosing_func_qn" });
const ImplTrait = StringRecord(c.CBMImplTrait, &.{ "trait_name", "struct_name" });
const ResolvedCall = StringRecord(c.CBMResolvedCall, &.{ "caller_qn", "callee_qn", "strategy", "reason" });
const StringRef = StringRecord(c.CBMStringRef, &.{ "value", "enclosing_func_qn", "key_path" });
const InfraBinding = StringRecord(c.CBMInfraBinding, &.{ "source_name", "target_url", "broker" });
const Channel = StringRecord(c.CBMChannel, &.{ "channel_name", "transport", "enclosing_func_qn" });

fn encodeArray(b: *SpillBuf, arr: anytype, comptime R: type) void {
    putU32(b, @bitCast(arr.count));
    const count: usize = if (arr.count > 0) ix(arr.count) else 0;
    for (0..count) |i| {
        var raw: R.T = undefined;
        @memcpy(std.mem.asBytes(&raw), std.mem.asBytes(util.at(arr.items, i)));
        R.nullFields(&raw);
        put(b, std.mem.asBytes(&raw));
    }
    for (0..count) |i| {
        R.enc(b, util.at(arr.items, i));
    }
}

fn decodeArray(r: *SpillRd, arr: anytype, comptime R: type) void {
    arr.items = @ptrCast(@alignCast(takeItems(r, @sizeOf(R.T), &arr.count)));
    arr.cap = arr.count;
    var i: usize = 0;
    while (i < arr.count and !r.failed) : (i += 1) {
        R.dec(r, util.at(arr.items, i));
    }
}

/// Scalar result fields, written as one block so a decode restores them in
/// one step.
const SpillScalars = extern struct {
    has_error: bool,
    parse_incomplete: bool,
    parse_unusable: bool,
    is_test_file: bool,
    error_region_count: i32,
    imports_count: i32,
    cached_lang: i32,
};

pub export fn cbm_result_spill_encode(result: *const FileResult, out_len: *usize) MutStr {
    var b: SpillBuf = .{};
    putU32(&b, SPILL_MAGIC);
    encodeArray(&b, &result.defs, Def);
    encodeArray(&b, &result.calls, Call);
    encodeArray(&b, &result.imports, Import);
    encodeArray(&b, &result.usages, Usage);
    encodeArray(&b, &result.throws, Throw);
    encodeArray(&b, &result.rw, ReadWrite);
    encodeArray(&b, &result.type_refs, TypeRef);
    encodeArray(&b, &result.env_accesses, EnvAccess);
    encodeArray(&b, &result.type_assigns, TypeAssign);
    encodeArray(&b, &result.impl_traits, ImplTrait);
    encodeArray(&b, &result.resolved_calls, ResolvedCall);
    encodeArray(&b, &result.string_refs, StringRef);
    encodeArray(&b, &result.infra_bindings, InfraBinding);
    encodeArray(&b, &result.channels, Channel);
    putStr(&b, result.module_qn);
    putStr(&b, result.namespace_name);
    putStrv(&b, @ptrCast(result.exports));
    putStrv(&b, @ptrCast(result.constants));
    putStrv(&b, @ptrCast(result.global_vars));
    putStrv(&b, @ptrCast(result.macros));
    putStr(&b, result.error_msg);
    putStr(&b, result.error_ranges);
    const scalars: SpillScalars = .{
        .has_error = result.has_error,
        .parse_incomplete = result.parse_incomplete,
        .parse_unusable = result.parse_unusable,
        .is_test_file = result.is_test_file,
        .error_region_count = result.error_region_count,
        .imports_count = result.imports_count,
        .cached_lang = @bitCast(result.cached_lang),
    };
    put(&b, std.mem.asBytes(&scalars));
    putBytes(&b, result.source, if (result.source != null) ix(result.source_len) else 0);
    if (b.failed) {
        c.free(b.data);
        return null;
    }
    out_len.* = b.len;
    return b.data;
}

pub export fn cbm_result_spill_decode(data: [*]const u8, len: usize) ?*FileResult {
    const result: *FileResult = @as(?*FileResult, interop.callocN(FileResult, 1)) orelse return null;
    c.cbm_arena_init(&result.arena);
    var r: SpillRd = .{ .p = data, .left = len, .arena = &result.arena };
    if (takeU32(&r) != SPILL_MAGIC) r.failed = true;
    decodeArray(&r, &result.defs, Def);
    decodeArray(&r, &result.calls, Call);
    decodeArray(&r, &result.imports, Import);
    decodeArray(&r, &result.usages, Usage);
    decodeArray(&r, &result.throws, Throw);
    decodeArray(&r, &result.rw, ReadWrite);
    decodeArray(&r, &result.type_refs, TypeRef);
    decodeArray(&r, &result.env_accesses, EnvAccess);
    decodeArray(&r, &result.type_assigns, TypeAssign);
    decodeArray(&r, &result.impl_traits, ImplTrait);
    decodeArray(&r, &result.resolved_calls, ResolvedCall);
    decodeArray(&r, &result.string_refs, StringRef);
    decodeArray(&r, &result.infra_bindings, InfraBinding);
    decodeArray(&r, &result.channels, Channel);
    result.module_qn = takeStr(&r);
    result.namespace_name = takeStr(&r);
    result.exports = @ptrCast(takeStrv(&r));
    result.constants = @ptrCast(takeStrv(&r));
    result.global_vars = @ptrCast(takeStrv(&r));
    result.macros = @ptrCast(takeStrv(&r));
    result.error_msg = takeStr(&r);
    result.error_ranges = takeStr(&r);
    var scalars: SpillScalars = undefined;
    take(&r, std.mem.asBytes(&scalars));
    result.has_error = scalars.has_error;
    result.parse_incomplete = scalars.parse_incomplete;
    result.parse_unusable = scalars.parse_unusable;
    result.is_test_file = scalars.is_test_file;
    result.error_region_count = scalars.error_region_count;
    result.imports_count = scalars.imports_count;
    result.cached_lang = @bitCast(scalars.cached_lang);
    result.source = takeBytes(&r, &result.source_len);
    if (r.failed or r.left != 0) {
        c.cbm_free_result(result);
        return null;
    }
    return result;
}

// ── Store ───────────────────────────────────────────────────────────

const SpillSlot = extern struct {
    offset: i64, // -1: empty
    len: i64,
    infra_bindings: c_int,
    string_refs: c_int,
};

const Spill = struct {
    path: [1024]u8,
    file: ?*c.FILE, // opened on the first park
    open_failed: bool,
    unlinked: bool, // already removed while open (POSIX); close must not retry
    end: i64,
    mu: c.cbm_mutex_t,
    slots: [*]SpillSlot,
    slot_count: c_int,
    parked: AtomicI64,
    bytes: AtomicI64,
    loads: AtomicI64,
    load_failures: AtomicI64,
};

extern fn _getpid() c_int;
extern fn _fseeki64(file: ?*c.FILE, offset: i64, whence: c_int) c_int;

fn currentPid() c_int {
    return if (is_windows) _getpid() else c.getpid();
}

fn spillFseek(file: ?*c.FILE, offset: i64, whence: c_int) c_int {
    return if (is_windows) _fseeki64(file, offset, whence) else c.fseeko(file, offset, whence);
}

var sequence: std.atomic.Value(c_int) = .init(0);

/// Below this much free space spilling does not start: filling the disk breaks
/// everything else on the machine, which is worse than the memory pressure the
/// spill would relieve.
const SPILL_FREE_FLOOR: u64 = 10 * 1024 * 1024 * 1024;

/// Test seam: a nonzero value replaces the free space read from the disk.
var free_bytes_pin: std.atomic.Value(u64) = .init(0);

pub export fn cbm_result_spill_pin_free_bytes_for_tests(bytes: u64) u64 {
    return free_bytes_pin.swap(bytes, .seq_cst);
}

pub export fn cbm_result_spill_free_floor_bytes() u64 {
    return SPILL_FREE_FLOOR;
}

/// Bytes available to this user on the filesystem holding `dir`; 0 when the
/// platform cannot say.
fn spillFreeBytes(dir: CStr) u64 {
    const pinned = free_bytes_pin.load(.seq_cst);
    if (pinned > 0) return pinned;
    if (is_windows) {
        var available = std.mem.zeroes(c.ULARGE_INTEGER);
        if (c.GetDiskFreeSpaceExA(dir, &available, null, null) == 0) return 0;
        return available.QuadPart;
    }
    var filesystem = std.mem.zeroes(c.struct_statvfs);
    if (c.statvfs(dir, &filesystem) != 0) return 0;
    const fragment_size: u64 = if (filesystem.f_frsize != 0) filesystem.f_frsize else filesystem.f_bsize;
    return @as(u64, filesystem.f_bavail) *| fragment_size;
}

/// Remove spill files left by runs that are gone. Only files this module names
/// (spill-<pid>-<n>.bin) are touched. A run killed outright runs no cleanup;
/// on Windows its file survives, and on POSIX so do files written before the
/// unlink-at-open below existed. Deleting is the liveness test on Windows: an
/// open file refuses, so a concurrent run keeps its spill. On POSIX a
/// concurrent run still reads its file through the handle it holds.
fn spillSweepOrphans(dir: CStr) void {
    const d = c.cbm_opendir(dir) orelse return;
    var removed: c_int = 0;
    while (c.cbm_readdir(d)) |entry| {
        const name = std.mem.sliceTo(&entry.*.name, 0);
        if (!std.mem.startsWith(u8, name, "spill-") or !std.mem.endsWith(u8, name, ".bin")) continue;
        var path: [1024]u8 = undefined;
        const n = c.snprintf(&path, path.len, "%s/%s", dir, @as(CStr, &entry.*.name));
        if (n < 0 or @as(usize, @intCast(n)) >= path.len) continue;
        if (c.cbm_unlink(&path) == 0) removed += 1;
    }
    c.cbm_closedir(d);
    if (removed > 0) {
        var count: [16]u8 = undefined;
        _ = c.snprintf(&count, count.len, "%d", removed);
        interop.logInfo("mem.spill.orphans_removed", .{ @as(CStr, "count"), @as(CStr, &count), @as(CStr, "dir"), dir });
    }
}

pub export fn cbm_result_spill_open(dir: CStr, slots: c_int) ?*Spill {
    if (dir == null or slots <= 0) return null;
    const sp: ?*Spill = interop.callocN(Spill, 1);
    const table: ?[*]SpillSlot = interop.callocN(SpillSlot, ix(slots));
    if (sp == null or table == null) {
        c.free(sp);
        c.free(@ptrCast(table));
        return null;
    }
    const n = c.snprintf(
        &sp.?.path,
        sp.?.path.len,
        "%s/spill-%d-%d.bin",
        dir,
        currentPid(),
        sequence.fetchAdd(1, .seq_cst),
    );
    if (n < 0 or @as(usize, @intCast(n)) >= sp.?.path.len) {
        c.free(sp);
        c.free(@ptrCast(table));
        return null;
    }
    spillSweepOrphans(dir);
    for (table.?[0..ix(slots)]) |*slot| slot.offset = -1;
    sp.?.slots = table.?;
    sp.?.slot_count = slots;
    c.cbm_mutex_init(&sp.?.mu);
    return sp;
}

/// Under sp.mu. Creates <dir> and the file on first use.
fn spillEnsureFile(sp: *Spill) bool {
    if (sp.file != null or sp.open_failed) return sp.file != null;
    var dir: [1024]u8 = undefined;
    _ = c.snprintf(&dir, dir.len, "%s", @as(CStr, &sp.path));
    const slash: MutStr = c.strrchr(&dir, '/');
    if (slash != null) {
        slash[0] = 0;
        _ = c.cbm_mkdir_p(&dir, 0o700);
    }
    // Ask the disk before trading memory for it. Refusing leaves the caller in
    // the memory path it was already in (back-pressure, then its own budget
    // decision): the same outcome as a disk that fills mid-run, minus the
    // filled disk. Unknown free space (0) does not refuse.
    const free_bytes = spillFreeBytes(&dir);
    if (free_bytes > 0 and free_bytes < SPILL_FREE_FLOOR) {
        sp.open_failed = true;
        var free_mb: [32]u8 = undefined;
        _ = c.snprintf(&free_mb, free_mb.len, "%llu", @as(c_ulonglong, free_bytes / (1024 * 1024)));
        interop.logWarn("mem.spill.refused", .{ @as(CStr, "reason"), @as(CStr, "low_disk"), @as(CStr, "free_mb"), @as(CStr, &free_mb) });
        return false;
    }
    sp.file = c.cbm_fopen(&sp.path, "w+b");
    if (sp.file == null) {
        sp.open_failed = true;
        interop.logWarn("mem.spill.open_failed", .{ @as(CStr, "path"), @as(CStr, &sp.path) });
        return false;
    }
    // Unlink while the file is open: the data stays reachable through this
    // handle and the space comes back the moment the process ends, whether by
    // a clean exit, an abort or a SIGKILL. Windows refuses to unlink an open
    // file, so it keeps the unlink at close and relies on the sweep at open.
    if (!is_windows and c.cbm_unlink(&sp.path) == 0) sp.unlinked = true;
    return true;
}

pub export fn cbm_result_spill_park(sp_c: ?*Spill, slot: c_int, result_c: ?*FileResult) bool {
    const sp = sp_c orelse return false;
    const result = result_c orelse return false;
    if (slot < 0 or slot >= sp.slot_count) return false;
    var len: usize = 0;
    const data = cbm_result_spill_encode(result, &len);
    if (data == null) return false;
    var ok = false;
    c.cbm_mutex_lock(&sp.mu);
    if (sp.slots[ix(slot)].offset < 0 and spillEnsureFile(sp) and
        spillFseek(sp.file, sp.end, c.SEEK_SET) == 0 and c.fwrite(data, 1, len, sp.file) == len)
    {
        sp.slots[ix(slot)] = .{
            .offset = sp.end,
            .len = @intCast(len),
            .infra_bindings = result.infra_bindings.count,
            .string_refs = result.string_refs.count,
        };
        sp.end += @intCast(len);
        ok = true;
    }
    c.cbm_mutex_unlock(&sp.mu);
    c.free(data);
    if (ok) {
        _ = sp.parked.fetchAdd(1, .seq_cst);
        _ = sp.bytes.fetchAdd(@intCast(len), .seq_cst);
        c.cbm_free_result(result);
    }
    return ok;
}

pub export fn cbm_result_spill_has(sp_c: ?*const Spill, slot: c_int) bool {
    // A slot is written before its result leaves the cache and read only by
    // later phases, so no lock is needed here.
    const sp = sp_c orelse return false;
    return slot >= 0 and slot < sp.slot_count and sp.slots[ix(slot)].offset >= 0;
}

pub export fn cbm_result_spill_infra_binding_count(sp: ?*const Spill, slot: c_int) c_int {
    return if (cbm_result_spill_has(sp, slot)) sp.?.slots[ix(slot)].infra_bindings else 0;
}

pub export fn cbm_result_spill_string_ref_count(sp: ?*const Spill, slot: c_int) c_int {
    return if (cbm_result_spill_has(sp, slot)) sp.?.slots[ix(slot)].string_refs else 0;
}

pub export fn cbm_result_spill_load(sp_c: ?*Spill, slot: c_int) ?*FileResult {
    if (!cbm_result_spill_has(sp_c, slot)) return null;
    const sp = sp_c.?;
    const entry = sp.slots[ix(slot)];
    const entry_len: usize = @intCast(entry.len);
    const data: MutStr = @ptrCast(c.malloc(entry_len));
    if (data == null) return null;
    c.cbm_mutex_lock(&sp.mu);
    const read = c.fflush(sp.file) == 0 and spillFseek(sp.file, entry.offset, c.SEEK_SET) == 0 and
        c.fread(data, 1, entry_len, sp.file) == entry_len;
    c.cbm_mutex_unlock(&sp.mu);
    const result = if (read) cbm_result_spill_decode(data, entry_len) else null;
    c.free(data);
    if (result != null) {
        _ = sp.loads.fetchAdd(1, .seq_cst);
    } else {
        _ = sp.load_failures.fetchAdd(1, .seq_cst);
        interop.log(c.CBM_LOG_ERROR, "mem.spill.load_failed", .{ @as(CStr, "path"), @as(CStr, &sp.path) });
    }
    return result;
}

pub export fn cbm_result_spill_load_failures(sp: ?*const Spill) i64 {
    return if (sp) |s| s.load_failures.load(.seq_cst) else 0;
}

pub export fn cbm_result_spill_stats(sp: ?*const Spill, parked: *i64, bytes: *i64, loads: *i64) void {
    parked.* = if (sp) |s| s.parked.load(.seq_cst) else 0;
    bytes.* = if (sp) |s| s.bytes.load(.seq_cst) else 0;
    loads.* = if (sp) |s| s.loads.load(.seq_cst) else 0;
}

pub export fn cbm_result_spill_close(sp_c: ?*Spill) void {
    const sp = sp_c orelse return;
    if (sp.file != null) {
        _ = c.fclose(sp.file);
        if (!sp.unlinked) _ = c.cbm_unlink(&sp.path);
    }
    c.cbm_mutex_destroy(&sp.mu);
    c.free(@ptrCast(sp.slots));
    c.free(sp);
}

// ── Cache access ────────────────────────────────────────────────────

pub export fn cbm_result_cache_acquire(cache: [*]?*FileResult, sp: ?*Spill, i: c_int) ?*FileResult {
    if (cache[ix(i)]) |cached| return cached;
    return cbm_result_spill_load(sp, i);
}

pub export fn cbm_result_cache_release(cache: [*]?*FileResult, i: c_int, result: ?*FileResult) void {
    if (result != null and result != cache[ix(i)]) {
        c.cbm_free_result(result);
    }
}

pub export fn cbm_result_cache_present(cache: [*]const ?*FileResult, sp: ?*const Spill, i: c_int) bool {
    return cache[ix(i)] != null or cbm_result_spill_has(sp, i);
}
