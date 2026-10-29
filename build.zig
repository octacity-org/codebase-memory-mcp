const std = @import("std");

const Sanitizer = enum {
    none,
    address,
    thread,
};

const BuildKind = enum {
    prod,
    tests,
};

const Config = struct {
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    with_zova: bool,
    zova_root: []const u8,
    sanitize: Sanitizer,
    static_link: bool,
    target_macos_version: []const u8,
};

pub fn build(b: *std.Build) void {
    const cfg = Config{
        .target = b.standardTargetOptions(.{}),
        .optimize = b.standardOptimizeOption(.{}),
        .with_zova = b.option(bool, "with-zova", "Build with Zova C ABI support") orelse false,
        .zova_root = b.option([]const u8, "zova-root", "Path to the local Zova checkout") orelse "../zova",
        .sanitize = b.option(Sanitizer, "sanitize", "Sanitizer mode: none, address, thread") orelse .none,
        .static_link = b.option(bool, "static", "Request static system library linkage where supported") orelse false,
        .target_macos_version = b.option([]const u8, "target-macos-version", "Minimum macOS deployment target") orelse "15.0",
    };

    if (cfg.with_zova) {
        checkZovaInputs(b, cfg);
    }

    const cbm_exe = addCbmExecutable(b, cfg, .prod);
    const cbm_copy = copyArtifact(b, cbm_exe, "build/c/codebase-memory-mcp");
    const cbm_step = b.step("cbm", "Build build/c/codebase-memory-mcp");
    cbm_step.dependOn(&cbm_copy.step);

    const test_support = addTestSupportLibrary(b, cfg);
    const test_runner = addTestRunner(b, cfg, test_support);
    const test_runner_copy = copyArtifact(b, test_runner, "build/c/test-runner");
    const test_runner_step = b.step("test-runner", "Build build/c/test-runner");
    test_runner_step.dependOn(&test_runner_copy.step);

    addFocusedTestRunner(
        b,
        cfg,
        test_support,
        "zova",
        &zova_test_sources,
        "-DCBM_TEST_RUNNER_ZOVA=1",
    );
    addFocusedTestRunner(
        b,
        cfg,
        test_support,
        "mcp",
        &mcp_test_sources,
        "-DCBM_TEST_RUNNER_MCP=1",
    );
    addFocusedTestRunner(
        b,
        cfg,
        test_support,
        "pipeline",
        &pipeline_test_sources,
        "-DCBM_TEST_RUNNER_PIPELINE=1",
    );

    _ = addRunSuiteStep(b, "test", "Run the full C test runner", test_runner, null);
    _ = addRunSuiteStep(b, "test-cli", "Run focused CLI tests", test_runner, "cli");
    _ = addRunSuiteStep(b, "test-mcp", "Run focused MCP tests", test_runner, "mcp");

    addZovaSuiteStep(b, cfg, test_runner, "test-zova", "zova");
    addZovaSuiteStep(b, cfg, test_runner, "test-zova-c-sql-functions", "zova_c_sql_functions");
    addZovaSuiteStep(b, cfg, test_runner, "test-zova-bridge", "zova_bridge");

    const test_install_safety =
        b.step("test-install-safety", "Verify installer replacement and idempotency safeguards");
    const install_safety =
        b.addSystemCommand(&.{ "bash", "tests/test_install_script_safety.sh" });
    test_install_safety.dependOn(&install_safety.step);
}

fn checkZovaInputs(b: *std.Build, cfg: Config) void {
    const include_path = b.pathJoin(&.{ cfg.zova_root, "include" });
    const lib_path = b.pathJoin(&.{ cfg.zova_root, "zig-out/lib/libzova_c.a" });
    const root_path = b.pathJoin(&.{ cfg.zova_root, "src/root.zig" });
    b.root.root_dir.handle.access(b.graph.io, include_path, .{}) catch {
        std.debug.panic("error: ZOVA_ROOT not found or incomplete: {s}", .{cfg.zova_root});
    };
    b.root.root_dir.handle.access(b.graph.io, lib_path, .{}) catch {
        std.debug.panic("error: Zova C static library not found: {s}\n       Build Zova first with: cd {s} && zig build", .{ lib_path, cfg.zova_root });
    };
    b.root.root_dir.handle.access(b.graph.io, root_path, .{}) catch {
        std.debug.panic("error: Zova Zig root not found: {s}", .{root_path});
    };
    const header_path = b.pathJoin(&.{ cfg.zova_root, "include/zova.h" });
    const header = b.root.root_dir.handle.readFileAlloc(b.graph.io, header_path, b.allocator, .limited(1024 * 1024)) catch {
        std.debug.panic("error: unable to read Zova C ABI header: {s}", .{header_path});
    };
    if (!std.mem.containsAtLeast(u8, header, 1, "Zova C ABI, v1.") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_database_register_function") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_vector_search_in") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_vector_search_by_id_in") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_edge_delete_many") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_vector_delete_many") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_build_fresh_keyed") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_build_fresh_prepared_keyed") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_build_fresh_prepared_keyed_with_payloads") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_edge_payload_get_many") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_graph_edge_payload_replace_many") or
        !std.mem.containsAtLeast(u8, header, 1, "zova_fresh_build_begin"))
    {
        std.debug.panic("error: a Zova 1.x C ABI header with SQL callbacks, candidate vector search, batch deletion, prepared payload graph builds, edge payload access, and fresh-build sessions is required: {s}", .{header_path});
    }
}

fn addCbmExecutable(b: *std.Build, cfg: Config, kind: BuildKind) *std.Build.Step.Compile {
    const module = createCModule(b, cfg, kind);
    addNativeSources(b, module, cfg, kind, false);
    module.addCSourceFile(.{
        .file = b.path("src/main.c"),
        .flags = flags(b, cfg, kind, .normal),
    });
    const exe = b.addExecutable(.{
        .name = "codebase-memory-mcp",
        .root_module = module,
    });
    return exe;
}

fn addTestSupportLibrary(b: *std.Build, cfg: Config) *std.Build.Step.Compile {
    const module = createCModule(b, cfg, .tests);
    addNativeSources(b, module, cfg, .tests, true);
    return b.addLibrary(.{
        .name = "cbm_test_support",
        .linkage = .static,
        .root_module = module,
    });
}

fn addTestRunner(
    b: *std.Build,
    cfg: Config,
    test_support: *std.Build.Step.Compile,
) *std.Build.Step.Compile {
    const module = createCModule(b, cfg, .tests);
    module.addCSourceFiles(.{
        .files = &all_test_sources,
        .flags = flags(b, cfg, .tests, .normal),
    });
    module.linkLibrary(test_support);
    addZovaRuntimeArchive(b, module, cfg);
    const exe = b.addExecutable(.{
        .name = "test-runner",
        .root_module = module,
    });
    return exe;
}

fn addFocusedTestRunner(
    b: *std.Build,
    cfg: Config,
    test_support: *std.Build.Step.Compile,
    group: []const u8,
    test_sources: []const []const u8,
    runner_define: []const u8,
) void {
    const module = createCModule(b, cfg, .tests);
    module.addCSourceFile(.{
        .file = b.path("tests/test_main.c"),
        .flags = flagsWithExtra(b, cfg, .tests, .normal, runner_define),
    });
    module.addCSourceFiles(.{
        .files = test_sources,
        .flags = flags(b, cfg, .tests, .normal),
    });
    module.linkLibrary(test_support);
    addZovaRuntimeArchive(b, module, cfg);
    const name = b.fmt("test-runner-{s}", .{group});
    const exe = b.addExecutable(.{
        .name = name,
        .root_module = module,
    });
    const copy = copyArtifact(b, exe, b.fmt("build/c/{s}", .{name}));
    const step = b.step(name, b.fmt("Build focused {s} test runner", .{group}));
    step.dependOn(&copy.step);
}

fn createCModule(b: *std.Build, cfg: Config, kind: BuildKind) *std.Build.Module {
    const sanitize_c: ?std.zig.SanitizeC = switch (cfg.sanitize) {
        .none, .thread => null,
        .address => .full,
    };
    const sanitize_thread = if (cfg.sanitize == .thread) true else null;
    const module = b.createModule(.{
        .target = cfg.target,
        .optimize = cfg.optimize,
        .link_libc = true,
        .link_libcpp = true,
        .sanitize_c = sanitize_c,
        .sanitize_thread = sanitize_thread,
    });
    _ = kind;
    addIncludePaths(b, module, cfg);
    module.addCSourceFile(.{ .file = inputDigestSource(b), .flags = &.{} });
    module.linkSystemLibrary("m", .{ .preferred_link_mode = if (cfg.static_link) .static else .dynamic });
    module.linkSystemLibrary("pthread", .{ .preferred_link_mode = if (cfg.static_link) .static else .dynamic });
    module.linkSystemLibrary("z", .{ .preferred_link_mode = if (cfg.static_link) .static else .dynamic });
    return module;
}

/// Zig 0.17 drops a C object's header dependencies from the artifact's cache
/// manifest when that object itself comes from cache, so a later edit to a
/// header (or to a .c file pulled in by #include, like lsp/c_lsp.c) can leave
/// the binary or library stale. Every C module therefore also compiles a
/// one-line file named after a digest of the size and mtime of each C source
/// and header: any edit changes the file, the artifact manifest misses, and
/// Zig re-checks each object against its own, complete, dependency list. The
/// files are registered as configure dependencies so that build() reruns.
var input_digest_source: ?std.Build.LazyPath = null;

fn inputDigestSource(b: *std.Build) std.Build.LazyPath {
    if (input_digest_source) |path| return path;
    const io = b.graph.io;
    var sum: u64 = 0;
    for ([_][]const u8{ "src", "internal", "vendored", "tests" }) |root| {
        b.dependOnDirectoryContents(b.path(root));
        var dir = b.root.root_dir.handle.openDir(io, root, .{ .iterate = true }) catch |err|
            std.debug.panic("error: unable to open {s}: {t}", .{ root, err });
        defer dir.close(io);
        var walker = dir.walk(b.allocator) catch @panic("OOM");
        defer walker.deinit();
        while (walker.next(io) catch |err| std.debug.panic("error: unable to walk {s}: {t}", .{ root, err })) |entry| {
            const rel = b.pathJoin(&.{ root, entry.path });
            if (entry.kind == .directory) {
                b.dependOnDirectoryContents(b.path(rel));
                continue;
            }
            if (entry.kind != .file or !isCInput(entry.basename)) continue;
            const st = entry.dir.statFile(io, entry.basename, .{}) catch continue;
            b.dependOnFileMetadata(b.path(rel));
            var h = std.hash.Wyhash.init(0);
            h.update(rel);
            h.update(std.mem.asBytes(&st.size));
            h.update(std.mem.asBytes(&st.mtime.nanoseconds));
            sum +%= h.final(); // order-independent: the walk order is undefined
        }
    }
    const wf = b.addWriteFiles();
    input_digest_source = wf.add("cbm_input_digest.c", b.fmt("typedef int cbm_input_digest_{x:0>16};\n", .{sum}));
    return input_digest_source.?;
}

fn isCInput(name: []const u8) bool {
    for ([_][]const u8{ ".c", ".h", ".cc", ".cpp", ".hpp", ".inc", ".def" }) |ext| {
        if (std.mem.endsWith(u8, name, ext)) return true;
    }
    return false;
}

fn addIncludePaths(b: *std.Build, module: *std.Build.Module, cfg: Config) void {
    module.addIncludePath(b.path("src"));
    module.addIncludePath(b.path("vendored"));
    module.addIncludePath(b.path("vendored/sqlite3"));
    module.addIncludePath(b.path("vendored/mimalloc/include"));
    module.addIncludePath(b.path("internal/cbm"));
    module.addIncludePath(b.path("internal/cbm/vendored/ts_runtime/include"));
    module.addIncludePath(b.path("internal/cbm/vendored/ts_runtime/src"));
    module.addIncludePath(b.path("tests"));
    module.addIncludePath(b.path("tests/repro"));
    if (cfg.with_zova) {
        module.addIncludePath(.{ .cwd_relative = b.pathJoin(&.{ cfg.zova_root, "include" }) });
    }
}

const SourceFlavor = enum {
    normal,
    grammar,
    cxx,
    mimalloc,
    sqlite,
    lz4,
    zstd,
};

fn flags(b: *std.Build, cfg: Config, kind: BuildKind, flavor: SourceFlavor) []const []const u8 {
    var list = std.ArrayList([]const u8).empty;
    const a = b.allocator;
    switch (flavor) {
        .normal => {
            list.appendSlice(a, &.{ "-std=c11", "-D_DEFAULT_SOURCE", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare" }) catch @panic("OOM");
            if (kind == .prod) {
                list.append(a, "-O2") catch @panic("OOM");
                list.append(a, "-DCBM_BIND_TS_ALLOCATOR=1") catch @panic("OOM");
            } else {
                list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
                list.append(a, "-Wno-unused-function") catch @panic("OOM");
            }
        },
        .grammar => {
            list.appendSlice(a, &.{ "-std=c11", "-D_DEFAULT_SOURCE", "-w", "-Iinternal/cbm", "-Iinternal/cbm/vendored/ts_runtime/include", "-Iinternal/cbm/vendored/ts_runtime/src" }) catch @panic("OOM");
            if (kind == .prod) list.append(a, "-O2") catch @panic("OOM") else list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
        },
        .cxx => {
            list.appendSlice(a, &.{ "-std=c++14", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-w", "-Iinternal/cbm/vendored" }) catch @panic("OOM");
            if (kind == .prod) list.append(a, "-O2") catch @panic("OOM") else list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
        },
        .mimalloc => {
            list.appendSlice(a, &.{ "-std=c11", "-w", "-Ivendored/mimalloc/include", "-Ivendored/mimalloc/src" }) catch @panic("OOM");
            if (kind == .prod) {
                list.appendSlice(a, &.{ "-O2", "-DMI_OVERRIDE=1" }) catch @panic("OOM");
            } else {
                list.appendSlice(a, &.{ "-g", "-O1", "-DMI_OVERRIDE=0" }) catch @panic("OOM");
            }
        },
        .sqlite => {
            list.appendSlice(a, &.{ "-std=c11", "-w", "-DSQLITE_DQS=0", "-DSQLITE_THREADSAFE=1", "-DSQLITE_ENABLE_FTS5" }) catch @panic("OOM");
            if (kind == .prod) list.append(a, "-O2") catch @panic("OOM") else list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
        },
        .lz4 => {
            list.appendSlice(a, &.{ "-std=c11", "-D_DEFAULT_SOURCE", "-w", "-Iinternal/cbm" }) catch @panic("OOM");
            if (kind == .prod) list.append(a, "-O2") catch @panic("OOM") else list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
        },
        .zstd => {
            list.appendSlice(a, &.{ "-std=c11", "-D_DEFAULT_SOURCE", "-w", "-Iinternal/cbm/vendored/zstd" }) catch @panic("OOM");
            if (kind == .prod) list.append(a, "-O2") catch @panic("OOM") else list.appendSlice(a, &.{ "-g", "-O1" }) catch @panic("OOM");
        },
    }
    list.append(a, b.fmt("-DCBM_WITH_ZOVA={d}", .{@intFromBool(cfg.with_zova)})) catch @panic("OOM");
    if (cfg.target.result.os.tag == .macos) {
        list.append(a, b.fmt("-mmacosx-version-min={s}", .{cfg.target_macos_version})) catch @panic("OOM");
    }
    switch (cfg.sanitize) {
        .none => {},
        .address => list.appendSlice(a, &.{ "-fsanitize=address,undefined", "-fno-omit-frame-pointer" }) catch @panic("OOM"),
        .thread => list.appendSlice(a, &.{ "-fsanitize=thread", "-fno-omit-frame-pointer" }) catch @panic("OOM"),
    }
    return list.toOwnedSlice(a) catch @panic("OOM");
}

fn flagsWithExtra(
    b: *std.Build,
    cfg: Config,
    kind: BuildKind,
    flavor: SourceFlavor,
    extra: []const u8,
) []const []const u8 {
    const base = flags(b, cfg, kind, flavor);
    const result = b.allocator.alloc([]const u8, base.len + 1) catch @panic("OOM");
    @memcpy(result[0..base.len], base);
    result[base.len] = extra;
    return result;
}

fn addNativeSources(
    b: *std.Build,
    module: *std.Build.Module,
    cfg: Config,
    kind: BuildKind,
    defer_zova_runtime: bool,
) void {
    module.addCSourceFile(.{ .file = b.path("vendored/mimalloc/src/static.c"), .flags = flags(b, cfg, kind, .mimalloc) });

    if (!cfg.with_zova) {
        module.addCSourceFile(.{ .file = b.path("vendored/sqlite3/sqlite3.c"), .flags = flags(b, cfg, kind, .sqlite) });
    }

    module.addCSourceFiles(.{ .files = grammarSources(b), .flags = flags(b, cfg, kind, .grammar) });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/ts_runtime.c"), .flags = flags(b, cfg, kind, .grammar) });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/lsp_all.c"), .flags = flags(b, cfg, kind, .grammar) });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/preprocessor.cpp"), .flags = flags(b, cfg, kind, .cxx), .language = .cpp });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/vendored/lz4/lz4.c"), .flags = flags(b, cfg, kind, .lz4) });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/vendored/lz4/lz4hc.c"), .flags = flags(b, cfg, kind, .lz4) });
    module.addCSourceFile(.{ .file = b.path("internal/cbm/vendored/zstd/zstd.c"), .flags = flags(b, cfg, kind, .zstd) });
    module.addAssemblyFile(b.path("vendored/nomic/code_vectors_blob.S"));

    module.addCSourceFiles(.{ .files = &native_sources, .flags = flags(b, cfg, kind, .normal) });

    if (cfg.with_zova) {
        if (!defer_zova_runtime) {
            addZovaRuntimeArchive(b, module, cfg);
        }
        const bridge = addZovaBridgeObject(b, cfg);
        module.addObject(bridge);
    }

    const cbm_zig = addCbmZigLibrary(b, cfg);
    module.linkLibrary(cbm_zig);
}

fn addZovaRuntimeArchive(b: *std.Build, module: *std.Build.Module, cfg: Config) void {
    if (!cfg.with_zova) return;
    module.addObjectFile(.{
        .cwd_relative = b.pathJoin(&.{ cfg.zova_root, "zig-out/lib/libzova_c.a" }),
    });
}

fn addCbmZigLibrary(b: *std.Build, cfg: Config) *std.Build.Step.Compile {
    // Zig modules implement C headers; they see the C declarations through a
    // translate-c module of src/cbm_zig_c.h (Zig has no @cImport).
    const c_decls = b.addTranslateC(.{
        .root_source_file = b.path("src/cbm_zig_c.h"),
        .target = cfg.target,
        .optimize = .ReleaseFast,
    });
    c_decls.defineCMacro("_DEFAULT_SOURCE", null);
    c_decls.defineCMacro("_GNU_SOURCE", null);
    for ([_][]const u8{
        "src",
        "vendored",
        "vendored/sqlite3",
        "vendored/mimalloc/include",
        "internal/cbm",
        "internal/cbm/vendored/ts_runtime/include",
    }) |dir| c_decls.addIncludePath(b.path(dir));

    const module = b.createModule(.{
        .root_source_file = b.path("src/cbm_zig.zig"),
        .target = cfg.target,
        .optimize = .ReleaseFast,
        .link_libc = true,
    });
    module.addImport("c", c_decls.createModule());
    const lib = b.addLibrary(.{
        .name = "cbm_zig",
        .linkage = .static,
        .root_module = module,
    });
    return lib;
}

fn addZovaBridgeObject(b: *std.Build, cfg: Config) *std.Build.Step.Compile {
    const zova_module = b.createModule(.{
        .root_source_file = .{ .cwd_relative = b.pathJoin(&.{ cfg.zova_root, "src/root.zig" }) },
        .target = cfg.target,
        .optimize = .ReleaseFast,
    });
    // Zova 1.x resolves its SQLite bindings and build options as named imports.
    zova_module.addImport("sqlite_c", b.addTranslateC(.{
        .root_source_file = b.path("vendored/sqlite3/sqlite3.h"),
        .target = cfg.target,
        .optimize = .ReleaseFast,
    }).createModule());
    const zova_build_options = b.addOptions();
    zova_build_options.addOption(bool, "enable_dynamic_extensions", false);
    zova_module.addOptions("zova_build_options", zova_build_options);
    const bridge_module = b.createModule(.{
        .root_source_file = b.path("src/zova/cbm_zova_bridge.zig"),
        .target = cfg.target,
        .optimize = .ReleaseFast,
        .link_libc = true,
    });
    bridge_module.addImport("zova", zova_module);
    bridge_module.addIncludePath(b.path("src"));
    bridge_module.addIncludePath(b.path("vendored/sqlite3"));
    return b.addObject(.{
        .name = "cbm_zova_bridge",
        .root_module = bridge_module,
    });
}

fn copyArtifact(b: *std.Build, artifact: *std.Build.Step.Compile, dest: []const u8) *std.Build.Step.Run {
    const dir = std.fs.path.dirname(dest).?;
    const mkdir = b.addSystemCommand(&.{ "mkdir", "-p", dir });
    const cp = b.addSystemCommand(&.{"cp"});
    cp.step.dependOn(&mkdir.step);
    cp.addArtifactArg(artifact);
    cp.addArg(dest);
    return cp;
}

fn addRunSuiteStep(
    b: *std.Build,
    name: []const u8,
    desc: []const u8,
    runner: *std.Build.Step.Compile,
    suite: ?[]const u8,
) *std.Build.Step {
    const run = b.addRunArtifact(runner);
    if (suite) |s| run.addArg(s);
    const step = b.step(name, desc);
    step.dependOn(&run.step);
    return step;
}

fn addZovaSuiteStep(
    b: *std.Build,
    cfg: Config,
    runner: *std.Build.Step.Compile,
    name: []const u8,
    suite: []const u8,
) void {
    const step = b.step(name, "Run focused Zova test suite");
    if (!cfg.with_zova) {
        const fail = b.addFail(b.fmt("{s} requires -Dwith-zova=true", .{name}));
        step.dependOn(&fail.step);
        return;
    }
    const run = b.addRunArtifact(runner);
    run.addArg(suite);
    step.dependOn(&run.step);
}

fn grammarSources(b: *std.Build) []const []const u8 {
    var dir = b.root.root_dir.handle.openDir(b.graph.io, "internal/cbm", .{ .iterate = true }) catch @panic("unable to open internal/cbm");
    defer dir.close(b.graph.io);
    var list = std.ArrayList([]const u8).empty;
    var it = dir.iterate();
    while (it.next(b.graph.io) catch @panic("unable to iterate internal/cbm")) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.startsWith(u8, entry.name, "grammar_")) continue;
        if (!std.mem.endsWith(u8, entry.name, ".c")) continue;
        list.append(b.allocator, b.pathJoin(&.{ "internal/cbm", entry.name })) catch @panic("OOM");
    }
    return list.toOwnedSlice(b.allocator) catch @panic("OOM");
}

const native_sources = [_][]const u8{
    "src/foundation/arena.c",
    "src/foundation/hash_table.c",
    "src/foundation/str_intern.c",
    "src/foundation/log.c",
    "src/foundation/str_util.c",
    "src/foundation/platform.c",
    "src/foundation/system_info.c",
    "src/foundation/slab_alloc.c",
    "src/foundation/yaml.c",
    "src/foundation/compat.c",
    "src/foundation/compat_thread.c",
    "src/foundation/compat_fs.c",
    "src/foundation/file_lock.c",
    "src/foundation/compat_regex.c",
    "src/foundation/mem.c",
    "src/foundation/diagnostics.c",
    "src/foundation/profile.c",
    "src/foundation/dump_verify.c",
    "src/foundation/limits.c",
    "src/foundation/subprocess.c",
    "src/foundation/sha256.c",
    "src/store/store.c",
    "src/zova/cbm_zova.c",
    "src/zova/cbm_zova_publish_model.c",
    "src/zova/cbm_zova_edge_payload.c",
    "src/zova/cbm_zova_delta.c",
    "src/zova/cbm_zova_operations.c",
    "src/zova/cbm_zova_v5_snapshot.c",
    "src/zova/cbm_zova_writer_gate.c",
    "src/zova/cbm_zova_coordination.c",
    "src/zova/cbm_zova_route.c",
    "src/zova/cbm_zova_repository.c",
    "src/zova/cbm_zova_bridge_stub.c",
    "src/zova/cbm_zova_plugin.c",
    "src/cypher/cypher.c",
    "src/mcp/mcp.c",
    "src/mcp/index_supervisor.c",
    "src/discover/language.c",
    "src/discover/userconfig.c",
    "src/discover/gitignore.c",
    "src/discover/discover.c",
    "src/graph_buffer/graph_buffer.c",
    "src/pipeline/fqn.c",
    "src/pipeline/path_alias.c",
    "src/pipeline/laravel_routing.c",
    "src/pipeline/registry.c",
    "src/pipeline/pipeline.c",
    "src/pipeline/pipeline_incremental.c",
    "src/pipeline/worker_pool.c",
    "src/pipeline/pass_parallel.c",
    "src/pipeline/result_spill.c",
    "src/pipeline/pass_definitions.c",
    "src/pipeline/pass_calls.c",
    "src/pipeline/pass_lsp_cross.c",
    "src/pipeline/pass_usages.c",
    "src/pipeline/pass_semantic.c",
    "src/pipeline/pass_tests.c",
    "src/pipeline/pass_githistory.c",
    "src/pipeline/pass_gitdiff.c",
    "src/pipeline/pass_configures.c",
    "src/pipeline/pass_configlink.c",
    "src/pipeline/pass_route_nodes.c",
    "src/pipeline/pass_enrichment.c",
    "src/pipeline/pass_envscan.c",
    "src/pipeline/pass_compile_commands.c",
    "src/pipeline/pass_infrascan.c",
    "src/pipeline/pass_k8s.c",
    "src/pipeline/pass_similarity.c",
    "src/pipeline/pass_semantic_edges.c",
    "src/pipeline/pass_complexity.c",
    "src/pipeline/pass_importance.c",
    "src/pipeline/pass_pkgmap.c",
    "src/semantic/semantic.c",
    "src/semantic/ast_profile.c",
    "src/semantic/rotsq.c",
    "src/cli/cli.c",
    "src/cli/progress_sink.c",
    "src/cli/hook_augment.c",
    "vendored/yyjson/yyjson.c",
    "internal/cbm/cbm.c",
    "internal/cbm/extract_defs.c",
    "internal/cbm/extract_calls.c",
    "internal/cbm/extract_imports.c",
    "internal/cbm/extract_usages.c",
    "internal/cbm/extract_unified.c",
    "internal/cbm/extract_semantic.c",
    "internal/cbm/extract_type_refs.c",
    "internal/cbm/extract_type_assigns.c",
    "internal/cbm/extract_env_accesses.c",
    "internal/cbm/extract_channels.c",
    "internal/cbm/extract_k8s.c",
    "internal/cbm/helpers.c",
    "internal/cbm/lang_specs.c",
    "internal/cbm/service_patterns.c",
    "internal/cbm/ac.c",
    "internal/cbm/lz4_store.c",
    "internal/cbm/zstd_store.c",
};

const zova_test_sources = [_][]const u8{
    "tests/test_zova.c",
    "tests/test_zova_operations.c",
    "tests/test_zova_c_sql_functions.c",
    "tests/test_zova_bridge.c",
    "tests/test_zova_plugin.c",
    "tests/test_incremental.c",
};

const mcp_test_sources = [_][]const u8{
    "tests/test_mcp.c",
};

const pipeline_test_sources = [_][]const u8{
    "tests/test_graph_buffer.c",
    "tests/test_registry.c",
    "tests/test_pipeline.c",
    "tests/test_index_resilience.c",
    "tests/test_call_reference.c",
    "tests/test_fqn.c",
    "tests/test_route_canon.c",
    "tests/test_path_alias.c",
    "tests/test_configlink.c",
    "tests/test_infrascan.c",
    "tests/test_worker_pool.c",
    "tests/test_parallel.c",
    "tests/test_incremental.c",
    "tests/test_importance.c",
};

const all_test_sources = [_][]const u8{
    "tests/test_main.c",
    "tests/test_arena.c",
    "tests/test_hash_table.c",
    "tests/test_dyn_array.c",
    "tests/test_str_intern.c",
    "tests/test_log.c",
    "tests/test_str_util.c",
    "tests/test_platform.c",
    "tests/test_dump_verify.c",
    "tests/test_subprocess.c",
    "tests/test_extraction.c",
    "tests/test_parse_coverage.c",
    "tests/test_extraction_inheritance.c",
    "tests/test_extraction_imports.c",
    "tests/test_grammar_regression.c",
    "tests/test_grammar_labels.c",
    "tests/test_grammar_imports.c",
    "tests/test_ac.c",
    "tests/test_store_nodes.c",
    "tests/test_store_edges.c",
    "tests/test_store_search.c",
    "tests/test_store_arch.c",
    "tests/test_cypher.c",
    "tests/test_mcp.c",
    "tests/test_language.c",
    "tests/test_userconfig.c",
    "tests/test_gitignore.c",
    "tests/test_git_context.c",
    "tests/test_discover.c",
    "tests/test_graph_buffer.c",
    "tests/test_registry.c",
    "tests/test_pipeline.c",
    "tests/test_importance.c",
    "tests/test_fqn.c",
    "tests/test_route_canon.c",
    "tests/test_path_alias.c",
    "tests/test_configlink.c",
    "tests/test_infrascan.c",
    "tests/test_worker_pool.c",
    "tests/test_parallel.c",
    "tests/test_index_resilience.c",
    "tests/test_call_reference.c",
    "tests/test_watcher.c",
    "tests/test_lz4.c",
    "tests/test_zstd.c",
    "tests/test_go_lsp.c",
    "tests/test_c_lsp.c",
    "tests/test_php_lsp.c",
    "tests/test_cs_lsp.c",
    "tests/test_cs_lsp_bench.c",
    "tests/test_scope.c",
    "tests/test_type_rep.c",
    "tests/test_py_lsp.c",
    "tests/test_py_lsp_bench.c",
    "tests/test_py_lsp_stress.c",
    "tests/test_py_lsp_scale.c",
    "tests/test_ts_lsp.c",
    "tests/test_java_lsp.c",
    "tests/test_java_lsp_coverage.c",
    "tests/test_kotlin_lsp.c",
    "tests/test_rust_lsp.c",
    "tests/test_zig_lsp.c",
    "tests/test_cli.c",
    "tests/test_mem.c",
    "tests/test_security.c",
    "tests/test_yaml.c",
    "tests/test_semantic.c",
    "tests/test_ast_profile.c",
    "tests/test_slab_alloc.c",
    "tests/test_simhash.c",
    "tests/test_stack_overflow.c",
    "tests/test_zova.c",
    "tests/test_zova_operations.c",
    "tests/test_zova_c_sql_functions.c",
    "tests/test_zova_bridge.c",
    "tests/test_zova_plugin.c",
    "tests/test_integration.c",
    "tests/test_incremental.c",
    "tests/test_lang_contract.c",
    "tests/test_edge_imports.c",
    "tests/test_edge_structural.c",
    "tests/test_lsp_resolution_probe.c",
    "tests/test_node_creation_probe.c",
    "tests/test_edge_types_probe.c",
    "tests/test_convergence_probe.c",
    "tests/test_matrix_known_classes.c",
    "tests/test_matrix_new_constructs.c",
    "tests/test_grammar_probe_a.c",
    "tests/test_grammar_probe_b.c",
    "tests/test_grammar_probe_c.c",
    "tests/test_grammar_probe_d.c",
    "tests/test_grammar_probe_e.c",
    "tests/test_grammar_probe_f.c",
    "tests/test_grammar_probe_g.c",
};
