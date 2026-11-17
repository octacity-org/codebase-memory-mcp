//! main.zig — The codebase-memory-mcp program.
//!
//! Implements src/main.h. main() itself stays in main.c, which passes the
//! release version in (see that file for why).
//!
//! Modes:
//!   (default)          Run as MCP server on stdin/stdout (JSON-RPC 2.0)
//!   cli <tool> [args]  Run a single tool call and print the result
//!   --version          Print version and exit
//!   --help             Print usage and exit
//!   install, update, config, ...  The commands in cli/commands.zig
//!
//! Signal handling: SIGTERM/SIGINT trigger graceful shutdown.
//! Watcher runs in a background thread, polling for git changes.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("foundation/interop.zig");

const is_windows = builtin.os.tag == .windows;

const CStr = [*c]const u8;
const Argv = [*c][*c]u8;
const logInfo = interop.logInfo;
const logWarn = interop.logWarn;

const PARENT_WATCHDOG_STACK_SIZE = 64 * c.CBM_SZ_1K; // watchdog only polls — tiny stack suffices
const WATCHER_BASE_INTERVAL_MS = 5000;

fn strEq(a: CStr, b: CStr) bool {
    return c.strcmp(a, b) == 0;
}

// ── Globals for signal handling ────────────────────────────────────

var g_watcher: ?*c.cbm_watcher_t = null;
var g_server: ?*c.cbm_mcp_server_t = null;
var g_shutdown = std.atomic.Value(bool).init(false);

/// Idempotent shutdown: cancels the active pipeline, stops background servers,
/// and closes stdin to unblock the MCP read loop. Invoked from the signal
/// handler and from the parent-death watchdog, hence the swap guard so the
/// body runs at most once. Body is async-signal-safe (only atomic stores and
/// stop calls that themselves only set atomics).
fn requestShutdown() void {
    if (g_shutdown.swap(true, .seq_cst)) return; // already shutting down

    // Cancel any in-progress pipeline (async-signal-safe: only does an atomic store)
    if (g_server != null) {
        const p = c.cbm_mcp_server_active_pipeline(g_server);
        if (p != null) c.cbm_pipeline_cancel(p);
    }
    // Make pending index-lease and barrier waits return at once
    c.cbm_zova_coord_set_cancelled(true);

    if (g_watcher != null) c.cbm_watcher_stop(g_watcher);
    // Close stdin to unblock getline in the MCP server loop
    _ = c.fclose(interop.stdin());
}

fn posixSignalHandler(_: std.posix.SIG) callconv(.c) void {
    requestShutdown();
}

fn windowsSignalHandler(_: c_int) callconv(.c) void {
    requestShutdown();
}

/// Install platform-specific signal handlers.
fn setupSignalHandlers() void {
    if (is_windows) {
        _ = c.signal(c.SIGTERM, &windowsSignalHandler);
        _ = c.signal(c.SIGINT, &windowsSignalHandler);
    } else {
        const sa: std.posix.Sigaction = .{
            .handler = .{ .handler = posixSignalHandler },
            .mask = std.posix.sigemptyset(),
            .flags = 0,
        };
        std.posix.sigaction(.TERM, &sa, null);
        std.posix.sigaction(.INT, &sa, null);
    }
}

// ── Parent-process watchdog ────────────────────────────────────────
// parent-death watchdog — distilled from #407 (fixes #406, thanks @nvt-pankajsharma).
//
// When this stdio MCP server is launched by an agent that later dies without a
// clean SIGTERM (e.g. the editor is force-killed), the orphaned server would
// otherwise linger forever blocked on stdin. POSIX has no portable "notify on
// parent death" primitive (PR_SET_PDEATHSIG is Linux-only), so we poll getppid:
// once the parent dies the process is reparented (ppid changes, typically to 1)
// and we shut down.
//
// Windows (upstream #914, 92725a5e): job objects do not cover this case — the
// KILL_ON_JOB_CLOSE job in subprocess.zig only wraps processes CBM spawns
// itself, and Windows never propagates a client's termination to the stdio
// server it spawned. The Windows branch waits on a SYNCHRONIZE handle to the
// parent instead: the kernel signals it exactly once on termination and the
// held handle pins the process object, so PID reuse cannot fool the wait.
// Workers keep the POSIX-only guard — they run inside CBM's own kill-on-close
// job.

/// The watchdog thread's argument: the parent's pid on POSIX, a SYNCHRONIZE
/// handle to the parent on Windows.
const ParentRef = if (is_windows) c.HANDLE else c.pid_t;

const parentWatchdogThread = if (is_windows) parentWatchdogWindows else parentWatchdogPosix;

fn parentWatchdogPosix(arg: ?*anyopaque) callconv(.c) ?*anyopaque {
    const initial_ppid = @as(*const c.pid_t, @ptrCast(@alignCast(arg))).*;
    const poll_interval_us: c_uint = 500000; // 500ms

    while (!g_shutdown.load(.seq_cst)) {
        _ = c.cbm_usleep(poll_interval_us);
        if (g_shutdown.load(.seq_cst)) break;
        // initial_ppid > 1 guards against an already-orphaned start (ppid==1),
        // where a changing ppid carries no signal.
        if (initial_ppid > 1 and c.getppid() != initial_ppid) {
            const msg = "level=warn msg=parent.exited reason=ppid_changed\n";
            _ = c.write(c.STDERR_FILENO, msg, msg.len);
            c._exit(0);
        }
    }
    return null;
}

fn parentWatchdogWindows(arg: ?*anyopaque) callconv(.c) ?*anyopaque {
    const parent_process = @as(*const c.HANDLE, @ptrCast(@alignCast(arg))).*;

    // The 500 ms timeout only re-checks g_shutdown, mirroring the POSIX poll.
    while (!g_shutdown.load(.seq_cst)) {
        const wait_status = c.WaitForSingleObject(parent_process, 500);
        if (wait_status == c.WAIT_OBJECT_0) {
            const msg = "level=warn msg=parent.exited reason=handle_signaled\n";
            _ = c._write(c._fileno(interop.stderr()), msg, msg.len);
            c._exit(0);
        }
        if (wait_status != c.WAIT_TIMEOUT) break; // handle became unwaitable — stop watching, never spin
    }
    return null;
}

/// Toolhelp is the documented way to learn one's own parent PID on Windows.
/// Returns 0 when the lookup fails ("no parent signal available").
fn windowsParentPid() c.DWORD {
    const snapshot = c.CreateToolhelp32Snapshot(c.TH32CS_SNAPPROCESS, 0);
    if (snapshot == interop.INVALID_HANDLE_VALUE) return 0;
    defer _ = c.CloseHandle(snapshot);
    var entry = std.mem.zeroes(c.PROCESSENTRY32W);
    entry.dwSize = @sizeOf(c.PROCESSENTRY32W);
    const self_pid = c.GetCurrentProcessId();
    if (c.Process32FirstW(snapshot, &entry) == 0) return 0;
    while (true) {
        if (entry.th32ProcessID == self_pid) return entry.th32ParentProcessID;
        if (c.Process32NextW(snapshot, &entry) == 0) return 0;
    }
}

/// The server's parent-death watchdog. It must not move once started: the
/// thread reads `parent` through a pointer.
const ParentWatchdog = struct {
    tid: c.cbm_thread_t = undefined,
    started: bool = false,
    parent: ParentRef = if (is_windows) null else 0,

    /// A thread-create failure (or ppid<=1) is non-fatal: the server still
    /// runs, it just won't auto-exit on parent death — same policy as the
    /// watcher thread. We deliberately do NOT exit at startup when ppid<=1
    /// (the PR's original behaviour): a legitimately-launched server can
    /// transiently show ppid==1 (early reparent races, double-fork/container
    /// launchers), and the watchdog already no-ops safely in that case via
    /// its initial_ppid>1 guard.
    fn start(self: *ParentWatchdog) void {
        if (is_windows) {
            // Same fail-open policy as POSIX ppid<=1: with no trustworthy
            // parent at startup (snapshot failed, or the parent already
            // exited) rely on stdin EOF.
            const ppid = windowsParentPid();
            self.parent = if (ppid != 0) c.OpenProcess(c.SYNCHRONIZE, c.FALSE, ppid) else null;
            if (self.parent == null) return;
        } else {
            self.parent = c.getppid();
        }
        if (c.cbm_thread_create(&self.tid, PARENT_WATCHDOG_STACK_SIZE, &parentWatchdogThread, @ptrCast(&self.parent)) == 0) {
            self.started = true;
            return;
        }
        if (is_windows) {
            _ = c.CloseHandle(self.parent);
            self.parent = null;
        }
        logWarn("parent.watchdog.unavailable", .{ "reason", "thread_create_failed" });
    }

    /// Ends the watchdog's poll loop and waits for it.
    fn stop(self: *ParentWatchdog) void {
        g_shutdown.store(true, .seq_cst);
        if (self.started) _ = c.cbm_thread_join(&self.tid);
        if (is_windows and self.parent != null) _ = c.CloseHandle(self.parent);
    }
};

/// Static: the detached worker watchdog reads it for the rest of the process.
var worker_initial_ppid: c.pid_t = 0;

/// #845: a supervised worker must not outlive its supervisor. If the parent
/// dies without reaping us (agent killed, supervisor crashed), an orphaned
/// worker would index on unsupervised — observed contributing to memory
/// pressure during the 2026-07-04 host panics. Reuse the parent-death
/// watchdog (safe outside server mode: on ppid change it only writes to
/// stderr and _exit(0)s — no cleanup dependencies). Detached: the worker
/// exits by returning from runCli; exit() tears the thread down. Failure
/// to start is non-fatal, same policy as the MCP-server watchdog.
fn startWorkerWatchdog() void {
    worker_initial_ppid = c.getppid();
    var tid: c.cbm_thread_t = undefined;
    if (c.cbm_thread_create(&tid, PARENT_WATCHDOG_STACK_SIZE, &parentWatchdogPosix, &worker_initial_ppid) == 0) {
        _ = c.cbm_thread_detach(&tid);
        logInfo("worker.watchdog.start", .{});
    } else {
        logWarn("worker.watchdog.unavailable", .{ "reason", "thread_create_failed" });
    }
}

// ── Watcher background thread ──────────────────────────────────────

fn watcherThread(arg: ?*anyopaque) callconv(.c) ?*anyopaque {
    _ = c.cbm_watcher_run(@ptrCast(arg), WATCHER_BASE_INTERVAL_MS);
    return null;
}

// ── Index callback for watcher ─────────────────────────────────────

fn watcherIndex(project_name: CStr, root_path: CStr, user_data: ?*anyopaque) callconv(.c) c_int {
    _ = user_data;

    // Skip indexing if shutdown is in progress (skipped, not indexed)
    if (g_shutdown.load(.seq_cst)) return 1;

    // Non-blocking: skip if another process or thread is already indexing this
    // workspace, or a database replacement is in progress. The watcher retries
    // on its next poll cycle (5-60s).
    var hold: c.cbm_zova_index_hold_t = undefined;
    const hold_status = c.cbm_mcp_index_hold_acquire(root_path, 0, &hold, null, 0);
    if (hold_status != c.CBM_ZOVA_COORD_OK) {
        logInfo("watcher.skip", .{ "project", project_name, "reason", c.cbm_mcp_index_hold_refusal_reason(hold_status, &hold) });
        return 1; // skipped: the watcher keeps the change and retries
    }
    defer c.cbm_zova_index_hold_release(&hold);

    logInfo("watcher.reindex", .{ "project", project_name, "path", root_path });

    // #832: route the re-index through the supervised worker subprocess so this
    // long-lived server process hands its RSS back to the OS on every cycle
    // instead of ratcheting (mimalloc v3 does not reclaim pages that worker
    // threads abandon at exit). The child writes the DB; the parent only needs the
    // return code and holds the index hold for the child's life.
    // Degrade to the in-process pipeline when the supervisor is off (kill switch)
    // or the spawn fails.
    if (c.cbm_index_supervisor_should_wrap()) {
        const resp = c.cbm_mcp_index_run_supervised_path(root_path);
        if (resp != null) {
            c.free(resp);
            return 0;
        }
        // resp == NULL → spawn-failure degrade → fall through to in-process.
    }

    const p = c.cbm_pipeline_new(root_path, null, c.CBM_MODE_FULL);
    if (p == null) return c.CBM_NOT_FOUND;
    defer c.cbm_pipeline_free(p);
    return c.cbm_pipeline_run(p);
}

// ── CLI mode ───────────────────────────────────────────────────────

const CLI_USAGE = "Usage: codebase-memory-mcp cli [--progress] [--json] <tool_name> [json_args]\n";

/// Extract text content from MCP tool result envelope and print it.
/// MCP results: {"content":[{"type":"text","text":"..."}],"isError":...}
/// Returns 1 if the result was an error, 0 otherwise.
fn cliPrintMcpResult(result: CStr) c_int {
    @setEvalBranchQuota(10000);
    const doc = c.yyjson_read(result, c.strlen(result), 0);
    if (doc == null) {
        _ = c.printf("%s\n", result);
        return 0;
    }
    defer c.yyjson_doc_free(doc);

    const root = c.yyjson_doc_get_root(doc);
    const err_val = c.yyjson_obj_get(root, "isError");
    const is_error = err_val != null and c.yyjson_get_bool(err_val);

    var text: CStr = null;
    const content = c.yyjson_obj_get(root, "content");
    if (c.yyjson_is_arr(content) and c.yyjson_arr_size(content) > 0) {
        const tv = c.yyjson_obj_get(c.yyjson_arr_get_first(content), "text");
        if (tv != null) text = c.yyjson_get_str(tv);
    }

    if (text != null) {
        _ = c.fprintf(if (is_error) interop.stderr() else interop.stdout(), "%s\n", text);
    } else {
        _ = c.printf("%s\n", result);
    }
    return if (is_error) c.SKIP_ONE else 0;
}

/// Strip a flag from argv, returning true if found.
fn cliStripFlag(argc: *c_int, argv: Argv, flag: CStr) bool {
    const n: usize = @intCast(argc.*);
    for (0..n) |i| {
        if (!strEq(argv[i], flag)) continue;
        for (i..n - 1) |j| argv[j] = argv[j + 1];
        argc.* -= 1;
        return true;
    }
    return false;
}

/// Strip a flag AND its following value from argv, returning the value (a
/// pointer into the original argv strings, valid for the process lifetime) or
/// NULL if the flag is absent.
fn cliStripFlagValue(argc: *c_int, argv: Argv, flag: CStr) CStr {
    const n: usize = @intCast(argc.*);
    for (0..n) |i| {
        if (!strEq(argv[i], flag)) continue;
        const value: CStr = if (i + 1 < n) argv[i + 1] else null;
        const remove_count: usize = if (value != null) 2 else 1;
        for (i..n - remove_count) |j| argv[j] = argv[j + remove_count];
        argc.* -= @intCast(remove_count);
        return value;
    }
    return null;
}

/// Portable "is stdin a terminal?" — _isatty on Windows, isatty on POSIX.
fn cliStdinIsTty() bool {
    return if (is_windows) c._isatty(0) != 0 else c.isatty(0) != 0;
}

const CLI_SLURP_CHUNK = 4096;

/// Read an open stream fully into a heap, NUL-terminated string. Caller frees.
/// Returns NULL on allocation failure. Reads binary-clean (UTF-8 JSON, no
/// shell quoting needed).
fn cliSlurpStream(f: ?*c.FILE) [*c]u8 {
    var cap: usize = CLI_SLURP_CHUNK;
    var len: usize = 0;
    var buf = interop.mallocN(u8, cap);
    if (buf == null) return null;
    var tmp: [CLI_SLURP_CHUNK]u8 = undefined;
    while (true) {
        const n = c.fread(&tmp, 1, tmp.len, f);
        if (n == 0) break;
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap) cap *= 2;
            const nb = interop.reallocN(u8, buf, cap);
            if (nb == null) {
                c.free(buf);
                return null;
            }
            buf = nb;
        }
        @memcpy((buf + len)[0..n], tmp[0..n]);
        len += n;
    }
    buf[len] = 0;
    return buf;
}

/// Slurp a file path into a heap, NUL-terminated string. Caller frees.
fn cliSlurpFile(path: CStr) [*c]u8 {
    const f = c.cbm_fopen(path, "rb");
    if (f == null) return null;
    defer _ = c.fclose(f);
    return cliSlurpStream(f);
}

/// True if the first non-whitespace byte of s is '{' (raw-JSON detection).
fn cliFirstNonspaceIsBrace(s: CStr) bool {
    var i: usize = 0;
    while (s[i] == ' ' or s[i] == '\t' or s[i] == '\n' or s[i] == '\r') i += 1;
    return s[i] == '{';
}

fn runCli(argc_in: c_int, argv: Argv) c_int {
    var argc = argc_in;
    if (argc < 1) {
        _ = c.fputs(CLI_USAGE, interop.stderr());
        return c.SKIP_ONE;
    }

    const progress = cliStripFlag(&argc, argv, "--progress");
    const raw_json = cliStripFlag(&argc, argv, "--json");

    // Supervisor worker role: when this process was spawned as a supervised index
    // worker, run indexing in-process (never re-supervise) and write the result to
    // the given file for the parent to read back. Stripped here so the tool
    // dispatch below sees only the tool name + its args.
    const index_worker = cliStripFlag(&argc, argv, "--index-worker");
    const response_out = cliStripFlagValue(&argc, argv, "--response-out");
    c.cbm_index_set_worker_role(index_worker, response_out);

    if (!is_windows) {
        if (index_worker) startWorkerWatchdog();
    }

    if (argc < 1) {
        _ = c.fputs(CLI_USAGE, interop.stderr());
        return c.SKIP_ONE;
    }

    const tool_name = argv[0];
    const rem_argc = argc - 1; // args following the tool name
    const rem_argv = argv + 1;
    const rem: usize = @intCast(rem_argc);

    // --help / -h : print per-tool help (from the tool's input_schema) and exit
    // before any server work.
    for (0..rem) |i| {
        if (strEq(rem_argv[i], "--help") or strEq(rem_argv[i], "-h")) {
            if (c.cbm_cli_print_tool_help(tool_name) != 0) {
                _ = c.fprintf(interop.stderr(), "error: unknown tool '%s'\n", tool_name);
                return c.SKIP_ONE;
            }
            return 0;
        }
    }

    // Resolve the JSON arguments. Precedence: --args-file, then raw JSON
    // (back-compat), then --flags, then piped stdin, then empty {}.
    var heap_args: [*c]u8 = null;
    defer c.free(heap_args);
    var args_json: CStr = "{}";

    var args_file_idx: ?usize = null;
    for (0..rem) |i| {
        if (strEq(rem_argv[i], "--args-file")) {
            args_file_idx = i;
            break;
        }
    }

    if (args_file_idx) |idx| {
        if (idx + 1 >= rem) {
            _ = c.fputs("error: --args-file requires a path argument\n", interop.stderr());
            return c.SKIP_ONE;
        }
        const path = rem_argv[idx + 1];
        heap_args = cliSlurpFile(path);
        if (heap_args == null) {
            _ = c.fprintf(interop.stderr(), "error: cannot read args file '%s'\n", path);
            return c.SKIP_ONE;
        }
        args_json = heap_args;
    } else if (rem >= 1 and cliFirstNonspaceIsBrace(rem_argv[0])) {
        // raw-JSON back-compat: cli <tool> '{"k":"v"}' (deprecated path). Warn on
        // STDERR only — stdout must stay clean JSON for piping.
        _ = c.fprintf(interop.stderr(), "warning: passing raw JSON to 'cli %s' is deprecated and " ++
            "will be removed in a future release; use flags (run 'cli " ++
            "%s --help'), --args-file <path>, or piped stdin.\n", tool_name, tool_name);
        args_json = rem_argv[0];
    } else if (rem >= 1 and c.strncmp(rem_argv[0], "--", 2) == 0) {
        // flag form: cli <tool> --flag value --bare-bool ...
        var err: [*c]u8 = null;
        heap_args = c.cbm_cli_build_args_json(tool_name, rem_argc, rem_argv, &err);
        if (heap_args == null) {
            const msg: CStr = if (err != null) err else "invalid arguments";
            _ = c.fprintf(interop.stderr(), "error: %s\n", msg);
            c.free(err);
            return c.SKIP_ONE;
        }
        args_json = heap_args;
    } else if (c.cbm_cli_args_from_stdin_allowed(tool_name, cliStdinIsTty())) {
        // piped stdin (UTF-8 clean, no shell quoting): cli <tool> < args.json.
        // Skipped for tools without arguments (#1359).
        heap_args = cliSlurpStream(interop.stdin());
        if (heap_args != null and heap_args[0] != 0) {
            args_json = heap_args;
        } else {
            c.free(heap_args);
            heap_args = null;
            args_json = "{}";
        }
    }

    if (progress) c.cbm_progress_sink_init(interop.stderr());
    defer if (progress) c.cbm_progress_sink_fini();

    const srv = c.cbm_mcp_server_new();
    if (srv == null) {
        _ = c.fputs("error: failed to create server\n", interop.stderr());
        return c.SKIP_ONE;
    }
    defer c.cbm_mcp_server_free(srv);

    var config_dir: [c.CBM_SZ_1K]u8 = undefined;
    var runtime_config: ?*c.cbm_config_t = null;
    if (c.cbm_get_home_dir() != null) {
        _ = interop.bprint(&config_dir, "{s}", .{interop.sp(c.cbm_resolve_cache_dir())});
        runtime_config = c.cbm_config_open(&config_dir);
    }
    defer c.cbm_config_close(runtime_config);
    c.cbm_mcp_server_set_config(srv, runtime_config);

    const result = c.cbm_mcp_handle_tool(srv, tool_name, args_json);
    var exit_code: c_int = 0;

    if (result != null) {
        // Supervised worker: hand the full result string to the parent via the
        // response file before printing (parent reads it back on a clean exit).
        const ro = c.cbm_index_worker_response_out();
        if (ro != null) {
            const rf = c.cbm_fopen(ro, "wb");
            if (rf != null) {
                _ = c.fputs(result, rf);
                _ = c.fclose(rf);
            }
        }
        if (raw_json) {
            _ = c.printf("%s\n", result);
        } else {
            exit_code = cliPrintMcpResult(result);
        }
        if (c.cbm_index_worker_active()) {
            // Supervised worker: the response is delivered (file + stdout).
            // Skip the multi-GB teardown (server/store frees) — the process
            // dies now and the OS reclaims everything wholesale; piecemeal
            // free() of a kernel-scale graph costs minutes. _Exit skips
            // atexit/LSan by design for this prod worker path.
            logInfo("index.worker.fast_exit", .{ "action", "_Exit" });
            _ = c.fflush(null);
            c._Exit(exit_code);
        }
        c.free(result);
    }
    return exit_code;
}

// ── Help ───────────────────────────────────────────────────────────

fn printHelp(version: CStr) void {
    _ = c.printf("codebase-memory-mcp %s\n\n", version);
    _ = c.fputs("Usage:\n" ++
        "  codebase-memory-mcp              Run MCP server on stdio\n" ++
        "  codebase-memory-mcp cli <tool> [json]  Run a single tool\n" ++
        "  codebase-memory-mcp install [-y|-n] [--force] [--dry-run] [--plan]\n" ++
        "  codebase-memory-mcp uninstall [-y|-n] [--dry-run]\n" ++
        "  codebase-memory-mcp update [-y|-n]\n" ++
        "  codebase-memory-mcp config <list|get|set|reset>\n" ++
        "  codebase-memory-mcp doctor      Show install/cache/agent status\n" ++
        "  codebase-memory-mcp where       Show storage and config paths\n" ++
        "  codebase-memory-mcp zova-ops <action> --json\n" ++
        "  codebase-memory-mcp --version    Print version\n" ++
        "  codebase-memory-mcp --help       Print this help\n" ++
        "\nSupported agents (auto-detected):\n" ++
        "  Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode,\n" ++
        "  Antigravity, Aider, KiloCode, Kiro\n", interop.stdout());
    const tools = c.cbm_cli_tools_help();
    if (tools != null) {
        _ = c.printf("\n%s", tools);
        c.free(tools);
    }
}

// ── Main ───────────────────────────────────────────────────────────

fn initMemoryBudget() void {
    c.cbm_mem_init(c.cbm_mem_ram_fraction_for_total(c.cbm_system_info().total_ram));
}

/// Try to handle a subcommand (cli/install/uninstall/update/config/--version/--help).
/// Returns -1 if no subcommand matched, otherwise the exit code.
fn handleSubcommand(argc: c_int, argv: Argv, version: CStr) c_int {
    const n: usize = @intCast(argc);
    // First scan: global flags
    var i: usize = 1;
    while (i < n) : (i += 1) {
        if (strEq(argv[i], "--profile")) c.cbm_profile_enable();
    }
    i = 1;
    while (i < n) : (i += 1) {
        const arg = argv[i];
        const sub_argc = argc - @as(c_int, @intCast(i)) - 1;
        const sub_argv = argv + (i + 1);
        if (strEq(arg, "--version")) {
            _ = c.printf("codebase-memory-mcp %s\n", version);
            return 0;
        }
        if (strEq(arg, "--help") or strEq(arg, "-h")) {
            printHelp(version);
            return 0;
        }
        if (strEq(arg, "cli")) {
            initMemoryBudget();
            return runCli(sub_argc, sub_argv);
        }
        if (strEq(arg, "hook-augment")) {
            initMemoryBudget();
            return c.cbm_cmd_hook_augment();
        }
        if (strEq(arg, "install")) return c.cbm_cmd_install(sub_argc, sub_argv);
        if (strEq(arg, "uninstall")) return c.cbm_cmd_uninstall(sub_argc, sub_argv);
        if (strEq(arg, "update")) return c.cbm_cmd_update(sub_argc, sub_argv);
        if (strEq(arg, "config")) return c.cbm_cmd_config(sub_argc, sub_argv);
        if (strEq(arg, "doctor")) return c.cbm_cmd_doctor(sub_argc, sub_argv);
        if (strEq(arg, "where")) return c.cbm_cmd_where(sub_argc, sub_argv);
        if (strEq(arg, "zova-ops")) return c.cbm_cmd_zova_ops(sub_argc, sub_argv);
    }
    return c.CBM_NOT_FOUND;
}

/// On Windows the CRT hands main() an argv encoded in the active ANSI code
/// page, so a non-ASCII CLI argument (e.g. a repo path like café_日本語_repo) is
/// mangled before the program ever sees it — the documented
/// `cli index_repository "<json>"` then fails with "repo_path is required"
/// (#423/#20). Rebuild argv from the wide command line (GetCommandLineW →
/// CommandLineToArgvW) and convert each element to UTF-8 so the rest of the
/// program receives the same UTF-8 bytes it gets on POSIX. Returns a
/// NULL-terminated argv and sets out_argc, or NULL on any failure (caller then
/// keeps the original narrow argv). The returned block lives for the whole
/// process (argv must stay valid until exit), so it is intentionally never
/// freed.
fn windowsUtf8Argv(out_argc: *c_int) Argv {
    var wargc: c_int = 0;
    const wargv = c.CommandLineToArgvW(c.GetCommandLineW(), &wargc);
    if (wargv == null) return null;
    defer _ = c.LocalFree(@ptrCast(wargv));
    if (wargc <= 0) return null;
    const n: usize = @intCast(wargc);
    const u8argv = interop.callocN([*c]u8, n + 1); // NULL-terminated
    if (u8argv == null) return null;
    for (0..n) |i| {
        u8argv[i] = c.cbm_wide_to_utf8(wargv[i]);
        if (u8argv[i] == null) {
            for (0..i) |j| c.free(u8argv[j]);
            c.free(@ptrCast(u8argv));
            return null;
        }
    }
    out_argc.* = wargc;
    return u8argv;
}

/// A cbm.zova written by an older release uses an older Zova storage format,
/// which this binary refuses to open. Convert it once before the server touches
/// it; the conversion writes a new verified file and then swaps it in.
fn migrateZovaFormatAtStartup() void {
    var path_buf: [c.CBM_SZ_1K]u8 = undefined;
    const path: CStr = &path_buf;
    var report = std.mem.zeroes(c.cbm_zova_operation_report_t);
    if (c.cbm_zova_user_database_path(&path_buf, path_buf.len) != 0 or !c.cbm_file_exists(path) or
        c.cbm_zova_database_format_status(path) != c.CBM_ZOVA_DATABASE_MIGRATION_REQUIRED)
    {
        return;
    }
    logInfo("zova.migrate_format", .{ "path", path, "status", "started" });
    const code = c.cbm_zova_database_migrate_format(path, &report);
    if (code == c.CBM_ZOVA_OPERATION_OK or code == c.CBM_ZOVA_OPERATION_NOOP) {
        logInfo("zova.migrate_format", .{ "path", path, "status", "ok" });
    } else {
        const reason: CStr = &report.reason;
        const fix = "codebase-memory-mcp zova-ops migrate-format --json";
        interop.log(c.CBM_LOG_ERROR, "zova.migrate_format", .{ "path", path, "status", reason, "fix", fix });
    }
}

/// Default mode: the MCP server on stdio.
fn runServer(version: CStr) c_int {
    // parent-death watchdog — distilled from #407 (fixes #406). Start it early so
    // an orphaned server exits even if it dies before reaching the MCP loop.
    var watchdog: ParentWatchdog = .{};
    watchdog.start();

    initMemoryBudget();
    logInfo("server.start", .{ "version", version });
    migrateZovaFormatAtStartup();
    _ = c.cbm_diag_start(); // starts if CBM_DIAGNOSTICS=1

    setupSignalHandlers();

    // Open config store for runtime settings
    var config_dir: [c.CBM_SZ_1K]u8 = undefined;
    var runtime_config: ?*c.cbm_config_t = null;
    if (c.cbm_get_home_dir() != null) {
        _ = interop.bprint(&config_dir, "{s}", .{interop.sp(c.cbm_resolve_cache_dir())});
        runtime_config = c.cbm_config_open(&config_dir);
    }

    // Create MCP server
    g_server = c.cbm_mcp_server_new();
    if (g_server == null) {
        interop.log(c.CBM_LOG_ERROR, "server.err", .{ "msg", "failed to create server" });
        c.cbm_config_close(runtime_config);
        watchdog.stop();
        return c.SKIP_ONE;
    }

    // Create and start watcher in background thread
    const watch_store = c.cbm_store_open_memory();
    g_watcher = c.cbm_watcher_new(watch_store, &watcherIndex, null);

    // Wire watcher + config into MCP server for session auto-index
    c.cbm_mcp_server_set_watcher(g_server, g_watcher);
    c.cbm_mcp_server_set_config(g_server, runtime_config);
    var watcher_tid: c.cbm_thread_t = undefined;
    var watcher_started = false;
    if (g_watcher != null) {
        watcher_started = c.cbm_thread_create(&watcher_tid, 0, &watcherThread, g_watcher) == 0;
    }

    // Run MCP event loop (blocks until EOF or signal)
    const rc = c.cbm_mcp_server_run(g_server, interop.stdin(), interop.stdout());

    // Shutdown
    logInfo("server.shutdown", .{});
    watchdog.stop(); // also unblocks its poll loop

    if (watcher_started) {
        c.cbm_watcher_stop(g_watcher);
        _ = c.cbm_thread_join(&watcher_tid);
    }
    c.cbm_watcher_free(g_watcher);
    c.cbm_store_close(watch_store);
    c.cbm_mcp_server_free(g_server);
    c.cbm_config_close(runtime_config);

    g_watcher = null;
    g_server = null;
    c.cbm_diag_stop();

    return rc;
}

pub export fn cbm_main(argc_in: c_int, argv_in: Argv, version: CStr) c_int {
    // Defense-in-depth: bind tree-sitter and sqlite3 to mimalloc so a
    // correct binary does not rely on the fragile MI_OVERRIDE symbol override
    // (#424). MUST be the VERY FIRST statement: SQLITE_CONFIG_MALLOC has to run
    // before the first sqlite3_open* (cbm_mcp_server_new → cbm_store_open_memory
    // below opens sqlite early), else sqlite3_config returns SQLITE_MISUSE and
    // the bind is silently ignored. No-op in the test build.
    c.cbm_alloc_init();
    var argc = argc_in;
    var argv = argv_in;
    if (is_windows) {
        // Replace the ANSI-code-page argv the CRT handed us with a UTF-8 argv
        // rebuilt from the wide command line, so non-ASCII CLI arguments survive
        // (#423/#20). Falls back to the original argv if the wide rebuild fails.
        // Done after cbm_alloc_init (which must stay the very first statement)
        // but before argv is first read below.
        var win_argc: c_int = 0;
        const win_argv = windowsUtf8Argv(&win_argc);
        if (win_argv != null) {
            argc = win_argc;
            argv = win_argv;
        }
    }
    // #845: mark this process as the REAL binary so the index supervisor may
    // wrap index_repository in a worker subprocess. Must run before any
    // subcommand dispatch so MCP-server, CLI, and HTTP paths are all covered.
    // Embedders of cbm_mcp_handle_tool (test binaries) never mark themselves,
    // so they index in-process instead of re-invoking themselves as
    // `<self> cli --index-worker …` (recursive suite re-runs / spawn chains).
    c.cbm_index_supervisor_mark_host();
    c.cbm_cli_set_version(version);
    c.cbm_profile_init(); // reads CBM_PROFILE env var, gates all prof macros
    // CBM_LOG_LEVEL support — distilled from #414 (closes #413). Apply before
    // the first log statement so the configured level governs all output.
    c.cbm_log_init_from_env();
    const subcmd = handleSubcommand(argc, argv, version);
    if (subcmd >= 0) return subcmd;

    return runServer(version);
}
