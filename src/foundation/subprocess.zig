//! subprocess.zig — cross-platform spawn + supervise + classify.
//!
//! Implements src/foundation/subprocess.h.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("interop.zig");

const Str = interop.Str;

const is_windows = builtin.os.tag == .windows;
const is_apple = builtin.os.tag.isDarwin();
const is_linux = builtin.os.tag == .linux;

/// NTSTATUS severity ERROR (top two bits set) covers the Windows crash exception
/// exit codes: 0xC0000005 (access violation), 0xC00000FD (stack overflow),
/// 0xC000001D (illegal instruction), 0xC0000094 (integer divide by zero), …
const CBM_WIN_CRASH_CODE_MIN: c_uint = 0xC0000000;

fn isFaultSignal(sig: c_int) bool {
    return sig == c.SIGSEGV or sig == c.SIGBUS or sig == c.SIGILL or
        sig == c.SIGFPE or sig == c.SIGABRT or sig == c.SIGSYS;
}

pub export fn cbm_proc_classify(exited_normally: bool, exit_code: c_int, term_signal: c_int, timed_out: bool) c.cbm_proc_outcome_t {
    if (timed_out) return c.CBM_PROC_HANG;
    if (!exited_normally) {
        // POSIX signal death.
        if (!is_windows and isFaultSignal(term_signal)) return c.CBM_PROC_CRASH;
        return c.CBM_PROC_KILLED;
    }
    // Exited with a code. A Windows NTSTATUS exception code is a crash; on POSIX
    // exit codes are 0..255 so this branch never misfires there.
    if (@as(c_uint, @bitCast(exit_code)) >= CBM_WIN_CRASH_CODE_MIN) return c.CBM_PROC_CRASH;
    return if (exit_code == 0) c.CBM_PROC_CLEAN else c.CBM_PROC_EXIT_NONZERO;
}

pub export fn cbm_proc_outcome_str(o: c.cbm_proc_outcome_t) Str {
    return switch (o) {
        c.CBM_PROC_CLEAN => "clean",
        c.CBM_PROC_EXIT_NONZERO => "exit_nonzero",
        c.CBM_PROC_CRASH => "crash",
        c.CBM_PROC_HANG => "hang",
        c.CBM_PROC_KILLED => "killed",
        c.CBM_PROC_OUTPUT_LIMIT => "output_limit",
        else => "spawn_failed",
    };
}

/// Tail newly-appended complete lines from the child log, starting at tail_pos.
/// A partial (non-newline-terminated) final line is left buffered: tail_pos is
/// not advanced past it, so it is re-read once completed. Returns true if any
/// complete line was consumed (i.e. there was progress).
fn tailLog(log_file: ?Str, tail_pos: *c_long, cb: c.cbm_proc_log_cb, ud: ?*anyopaque) bool {
    const path = log_file orelse return false;
    const lf = c.cbm_fopen(path, "r");
    if (lf == null) return false;
    var progressed = false;
    if (c.fseek(lf, tail_pos.*, c.SEEK_SET) == 0) {
        var line: [1024]u8 = undefined;
        while (true) {
            const before = c.ftell(lf);
            if (c.fgets(&line, line.len, lf) == null) break;
            const l = std.mem.len(@as(Str, @ptrCast(&line)));
            const complete = l > 0 and line[l - 1] == '\n';
            if (complete) {
                line[l - 1] = 0;
                tail_pos.* = c.ftell(lf);
                progressed = true;
                if (line[0] != 0) {
                    if (cb) |on_line| on_line(&line, ud);
                }
            } else if (l == line.len - 1) {
                // Oversized line filled the buffer without a newline — consume it
                // anyway (counts as progress) so we never stall on one long line.
                tail_pos.* = c.ftell(lf);
                progressed = true;
                if (cb) |on_line| on_line(&line, ud);
            } else {
                // Genuine partial final line — keep it buffered for next poll.
                tail_pos.* = before;
                break;
            }
        }
    }
    _ = c.fclose(lf);
    return progressed;
}

// ── Windows command-line quoting (pure; unit-tested on every platform) ───────

/// Bounded writer for the command line: reserves the final byte for a NUL
/// terminator. On overflow it sets `ovf` and stops writing.
const Cmdline = struct {
    buf: [*]u8,
    cap: usize,
    pos: usize = 0,
    ovf: bool = false,

    fn put(self: *Cmdline, ch: u8) void {
        if (self.pos + 1 >= self.cap) {
            self.ovf = true;
            return;
        }
        self.buf[self.pos] = ch;
        self.pos += 1;
    }

    fn putN(self: *Cmdline, ch: u8, n: usize) void {
        for (0..n) |_| self.put(ch);
    }

    /// Append one argv element to the command line using the Microsoft C runtime
    /// quoting rules (see MS "Parsing C Command-Line Arguments"). CreateProcess
    /// takes a SINGLE string that the child re-parses back into argv, so any
    /// element with a space, tab or double-quote must be wrapped in quotes and
    /// its embedded quotes / preceding backslashes escaped. Without this a JSON
    /// argument like {"repo_path":"C:/r"} loses its inner quotes and the child
    /// receives the invalid {repo_path:C:/r} — the Windows-only index-worker
    /// cmdline-quoting bug (the worker exited non-zero at JSON-arg parse,
    /// misattributed to the last-marked file). POSIX is unaffected: runPosix
    /// passes the argv array straight to execv.
    fn appendArg(self: *Cmdline, arg: Str, first: bool) void {
        if (!first) self.put(' ');
        self.put('"');
        var p = arg;
        while (p[0] != 0) {
            var nbs: usize = 0;
            while (p[0] == '\\') {
                nbs += 1;
                p += 1;
            }
            if (p[0] == 0) {
                // Trailing backslashes precede the closing quote: double them so
                // the quote stays a delimiter, not an escaped literal.
                self.putN('\\', nbs * 2);
                break;
            }
            if (p[0] == '"') {
                // N backslashes then a quote -> 2N+1 backslashes then an escaped quote.
                self.putN('\\', nbs * 2 + 1);
                self.put('"');
            } else {
                self.putN('\\', nbs);
                self.put(p[0]);
            }
            p += 1;
        }
        self.put('"');
    }
};

/// Build a full Windows CreateProcess command line from a NULL-terminated argv,
/// applying the MS C runtime quoting rules so the child re-parses byte-identical
/// argv. Returns true on success, false if the result would overflow `buf`.
///
/// Defined unconditionally (pure string logic, no Windows headers) so the quoting
/// contract is unit-tested on Linux/macOS CI too — even though the real spawn path
/// only runs on Windows. Shared by runWin AND the UI http_server index spawn
/// so both escape identically; a naive `"%s"` wrap silently corrupts any argument
/// containing a quote (e.g. the index JSON {"repo_path":"…"}), corrupting the
/// spawned child's argv.
pub export fn cbm_build_win_cmdline(buf: ?[*]u8, cap: usize, argv: ?[*]const ?Str) bool {
    const out = buf orelse return false;
    const args = argv orelse return false;
    if (cap == 0) return false;
    var line: Cmdline = .{ .buf = out, .cap = cap };
    var i: usize = 0;
    while (args[i]) |arg| : (i += 1) {
        line.appendArg(arg, i == 0);
        if (line.ovf) {
            out[0] = 0; // overflow: leave buf a valid (empty) string, never unterminated
            return false;
        }
    }
    out[line.pos] = 0;
    return true;
}

// Wall-clock deadline / output cap checks shared by both run loops.

fn deadlinePassed(opts: *const c.cbm_proc_opts_t, started: u64) bool {
    return opts.deadline_ms > 0 and (c.cbm_now_ms() -% started) >= @as(u64, @intCast(opts.deadline_ms));
}

fn quietTimedOut(opts: *const c.cbm_proc_opts_t, last_activity: u64) bool {
    return opts.quiet_timeout_ms > 0 and
        (c.cbm_now_ms() -% last_activity) >= @as(u64, @intCast(opts.quiet_timeout_ms));
}

fn logOverLimit(opts: *const c.cbm_proc_opts_t) bool {
    if (opts.max_log_bytes <= 0 or opts.log_file == null) return false;
    const lf = c.fopen(opts.log_file, "rb");
    if (lf == null) return false;
    const over = c.fseek(lf, 0, c.SEEK_END) == 0 and c.ftell(lf) > opts.max_log_bytes;
    _ = c.fclose(lf);
    return over;
}

fn spawnFailed(out: *c.cbm_proc_result_t) c_int {
    out.outcome = c.CBM_PROC_SPAWN_FAILED;
    out.exit_code = -1;
    out.term_signal = 0;
    return -1;
}

/// opts->argv, or { bin, NULL } stored in `fallback` when it is NULL.
fn argvOf(opts: *const c.cbm_proc_opts_t, fallback: *[2]?Str) [*]const ?Str {
    if (opts.argv != null) return @ptrCast(opts.argv);
    fallback.* = .{ opts.bin, null };
    return fallback;
}

// ── Windows ──────────────────────────────────────────────────────

const GENERIC_WRITE: c_uint = 0x40000000;
const INVALID_FILE_ATTRIBUTES: c_uint = 0xFFFFFFFF;
const WAIT_OBJECT_0: c_uint = 0;
const INFINITE: c_uint = 0xFFFFFFFF;

fn runWin(opts: *const c.cbm_proc_opts_t, out: *c.cbm_proc_result_t) c_int {
    var default_argv: [2]?Str = undefined;
    const argv = argvOf(opts, &default_argv);

    var cmdline: [8192]u8 = undefined;
    if (!cbm_build_win_cmdline(&cmdline, cmdline.len, argv)) return spawnFailed(out);
    // Spawn via CreateProcessW with a WIDE command line. CreateProcessA would
    // re-interpret our UTF-8 cmdline bytes through the ANSI code page (CP_ACP),
    // re-mangling a non-ASCII repo path at the parent->worker boundary — so the
    // worker's own wide-argv read could never recover it (#423/#20).
    const wcmd = c.cbm_utf8_to_wide(&cmdline);
    if (wcmd == null) return spawnFailed(out);

    var hlog: c.HANDLE = interop.INVALID_HANDLE_VALUE;
    var si = std.mem.zeroes(c.STARTUPINFOW);
    si.cb = @sizeOf(c.STARTUPINFOW);
    if (opts.log_file != null) {
        // STARTF_USESTDHANDLES needs inheritable handles; a default (NULL)
        // security descriptor is not, and the child's output would be lost.
        var sa = std.mem.zeroes(c.SECURITY_ATTRIBUTES);
        sa.nLength = @sizeOf(c.SECURITY_ATTRIBUTES);
        sa.bInheritHandle = c.TRUE;
        hlog = c.CreateFileA(opts.log_file, GENERIC_WRITE, c.FILE_SHARE_READ, &sa, c.CREATE_ALWAYS, c.FILE_ATTRIBUTE_NORMAL, null);
        if (hlog != interop.INVALID_HANDLE_VALUE) {
            si.dwFlags = c.STARTF_USESTDHANDLES;
            si.hStdError = hlog;
            si.hStdOutput = hlog;
        }
    }

    var pi = std.mem.zeroes(c.PROCESS_INFORMATION);
    const ok = c.CreateProcessW(null, wcmd, null, null, c.TRUE, 0, null, null, &si, &pi);
    c.free(wcmd);
    if (hlog != interop.INVALID_HANDLE_VALUE) _ = c.CloseHandle(hlog);
    if (ok == 0) return spawnFailed(out);
    out.pid = @bitCast(@as(c_uint, @truncate(pi.dwProcessId)));

    var tail_pos: c_long = 0;
    const started = c.cbm_now_ms();
    var last_activity = started;
    var timed_out = false;
    var output_limited = false;
    while (true) {
        const w = c.WaitForSingleObject(pi.hProcess, 200);
        if (tailLog(opts.log_file, &tail_pos, opts.on_log_line, opts.log_ud)) {
            last_activity = c.cbm_now_ms();
        }
        if (w == WAIT_OBJECT_0) break;
        output_limited = logOverLimit(opts);
        timed_out = !output_limited and (quietTimedOut(opts, last_activity) or deadlinePassed(opts, started));
        if (timed_out or output_limited) {
            _ = c.TerminateProcess(pi.hProcess, 1);
            _ = c.WaitForSingleObject(pi.hProcess, INFINITE);
            break;
        }
    }

    var code: c.DWORD = 1;
    _ = c.GetExitCodeProcess(pi.hProcess, &code);
    _ = c.CloseHandle(pi.hProcess);
    _ = c.CloseHandle(pi.hThread);
    if (opts.log_file != null and opts.delete_log_on_exit) _ = c.DeleteFileA(opts.log_file);

    const exit_code: c_int = @bitCast(@as(c_uint, @truncate(code)));
    out.exit_code = exit_code;
    out.term_signal = 0;
    out.outcome = if (output_limited) c.CBM_PROC_OUTPUT_LIMIT else cbm_proc_classify(true, exit_code, 0, timed_out);
    return 0;
}

// ── POSIX ────────────────────────────────────────────────────────

/// Returns 1 with *out_pid set when the child was spawned; otherwise the value
/// runPosix returns (out is filled).
fn runAppleSpawn(opts: *const c.cbm_proc_opts_t, out: *c.cbm_proc_result_t, out_pid: *c.pid_t) c_int {
    var attr: c.posix_spawnattr_t = undefined;
    if (c.posix_spawnattr_init(&attr) != 0) return spawnFailed(out);
    var flags: c_short = c.POSIX_SPAWN_CLOEXEC_DEFAULT;
    if (opts.own_process_group) {
        flags |= c.POSIX_SPAWN_SETPGROUP;
        _ = c.posix_spawnattr_setpgroup(&attr, 0);
    }
    _ = c.posix_spawnattr_setflags(&attr, flags);

    var actions: c.posix_spawn_file_actions_t = undefined;
    if (c.posix_spawn_file_actions_init(&actions) != 0) {
        _ = c.posix_spawnattr_destroy(&attr);
        return spawnFailed(out);
    }

    // Stdin: explicitly open /dev/null so child never steals from stdin (e.g. MCP stdio transport)
    _ = c.posix_spawn_file_actions_addopen(&actions, c.STDIN_FILENO, "/dev/null", c.O_RDONLY, 0);

    // Redirect stdout and stderr to the log (or discard)
    const target: Str = if (opts.log_file != null) opts.log_file else "/dev/null";
    _ = c.posix_spawn_file_actions_addopen(&actions, c.STDOUT_FILENO, target, c.O_WRONLY | c.O_CREAT | c.O_TRUNC, 0o644);
    _ = c.posix_spawn_file_actions_adddup2(&actions, c.STDOUT_FILENO, c.STDERR_FILENO);

    var default_argv: [2]?Str = undefined;
    const argv = argvOf(opts, &default_argv);

    var pid: c.pid_t = -1;
    const rc = c.posix_spawnp(&pid, opts.bin, &actions, &attr, @ptrCast(argv), @ptrCast(interop.environ()));

    _ = c.posix_spawn_file_actions_destroy(&actions);
    _ = c.posix_spawnattr_destroy(&attr);

    if (rc != 0 or pid <= 0) {
        if (opts.delete_log_on_exit and opts.log_file != null) _ = c.unlink(opts.log_file);
        if (rc == c.ENOENT or rc == c.EACCES) {
            // Bogus binary path fails cleanly like execv failure (_exit 127)
            out.outcome = c.CBM_PROC_EXIT_NONZERO;
            out.exit_code = 127;
            out.term_signal = 0;
            return 0;
        }
        return spawnFailed(out);
    }
    out_pid.* = pid;
    return 1;
}

fn waitpidRetry(pid: c.pid_t, wstatus: *c_int, options: c_int) c.pid_t {
    while (true) {
        const wr = c.waitpid(pid, wstatus, options);
        if (!(wr < 0 and interop.errno() == c.EINTR)) return wr;
    }
}

fn runPosix(opts: *const c.cbm_proc_opts_t, out: *c.cbm_proc_result_t) c_int {
    var pid: c.pid_t = -1;
    if (is_apple) {
        const spawned = runAppleSpawn(opts, out, &pid);
        if (spawned <= 0) return spawned;
    } else {
        pid = c.fork();
        if (pid < 0) return spawnFailed(out);
        if (pid == 0) {
            if (opts.own_process_group) _ = c.setpgid(0, 0);
            // Child: redirect stdout+stderr to the log (or discard), then exec.
            // Use open()+dup2() (async-signal-safe, no malloc) rather than freopen():
            // the parent may be multithreaded (the MCP server holds worker/watcher/http
            // threads plus mimalloc/sqlite global state), and a fork() copies
            // only the calling thread — a malloc between fork and exec could deadlock on
            // a lock another thread held at fork time. open/dup2/execv touch no heap.
            var default_argv: [2]?Str = undefined;
            const argv = argvOf(opts, &default_argv);
            const target: Str = if (opts.log_file != null) opts.log_file else "/dev/null";
            const fd = c.open(target, c.O_WRONLY | c.O_CREAT | c.O_TRUNC, @as(c_int, 0o644));
            if (fd >= 0) {
                _ = c.dup2(fd, c.STDOUT_FILENO);
                _ = c.dup2(fd, c.STDERR_FILENO);
                if (fd > c.STDERR_FILENO) _ = c.close(fd);
            }
            // Close all descriptors above stderr so children (workers, git, grep)
            // never inherit open server descriptors or lock files (#77).
            c.cbm_fd_close_above_stderr();
            _ = c.execv(opts.bin, @ptrCast(argv));
            c._exit(127); // exec failed
        }

        if (opts.own_process_group) {
            // Also set from the parent so a kill issued before the child's own
            // setpgid runs still reaches the group (EACCES after exec is fine).
            _ = c.setpgid(pid, pid);
        }
    }
    out.pid = @intCast(pid);
    const kill_target: c.pid_t = if (opts.own_process_group) -pid else pid;

    var tail_pos: c_long = 0;
    const started = c.cbm_now_ms();
    var last_activity = started;
    var timed_out = false;
    var output_limited = false;
    var wstatus: c_int = 0;
    while (true) {
        const done = waitpidRetry(pid, &wstatus, c.WNOHANG) == pid;

        if (tailLog(opts.log_file, &tail_pos, opts.on_log_line, opts.log_ud)) {
            last_activity = c.cbm_now_ms();
        }
        if (done) break;
        output_limited = logOverLimit(opts);
        timed_out = !output_limited and (quietTimedOut(opts, last_activity) or deadlinePassed(opts, started));
        if (timed_out or output_limited) {
            _ = c.kill(kill_target, c.SIGKILL);
            _ = waitpidRetry(pid, &wstatus, 0);
            break;
        }
        const ts: c.struct_timespec = .{ .tv_sec = 0, .tv_nsec = 100000000 }; // 100 ms poll
        _ = c.cbm_nanosleep(&ts, null);
    }

    if (opts.log_file != null and opts.delete_log_on_exit) _ = c.unlink(opts.log_file);

    if (output_limited) {
        out.exit_code = -1;
        out.term_signal = c.SIGKILL;
        out.outcome = c.CBM_PROC_OUTPUT_LIMIT;
    } else if (interop.wifexited(wstatus)) {
        out.exit_code = interop.wexitstatus(wstatus);
        out.term_signal = 0;
        out.outcome = cbm_proc_classify(true, out.exit_code, 0, timed_out);
    } else if (interop.wifsignaled(wstatus)) {
        out.exit_code = -1;
        out.term_signal = interop.wtermsig(wstatus);
        out.outcome = cbm_proc_classify(false, -1, out.term_signal, timed_out);
    } else {
        out.exit_code = -1;
        out.term_signal = 0;
        out.outcome = if (timed_out) c.CBM_PROC_HANG else c.CBM_PROC_KILLED;
    }
    return 0;
}

pub export fn cbm_subprocess_run(opts: ?*const c.cbm_proc_opts_t, out_in: ?*c.cbm_proc_result_t) c_int {
    var local: c.cbm_proc_result_t = undefined;
    const out = out_in orelse &local;
    out.outcome = c.CBM_PROC_SPAWN_FAILED;
    out.exit_code = -1;
    out.term_signal = 0;
    out.pid = 0;
    const o = opts orelse return -1;
    if (o.bin == null or o.bin[0] == 0) return -1;
    return if (is_windows) runWin(o, out) else runPosix(o, out);
}

// ── Own executable path ──────────────────────────────────────────

/// snprintf(out, outsz, "%s", path): true when path is non-empty and fit.
fn copyPath(out: ?[*]u8, outsz: usize, path: ?Str) bool {
    const dst = out orelse return false;
    const src = std.mem.span(path orelse return false);
    if (outsz == 0 or src.len == 0) return false;
    const n = @min(src.len, outsz - 1);
    @memcpy(dst[0..n], src[0..n]);
    dst[n] = 0;
    return src.len < outsz;
}

fn isExecutableFile(path: ?Str) bool {
    const p = path orelse return false;
    var st: c.struct_stat = undefined;
    return c.stat(p, &st) == 0 and (st.st_mode & c.S_IFMT) == c.S_IFREG and c.access(p, c.X_OK) == 0;
}

fn resolveFromPath(name: ?Str, out: ?[*]u8, outsz: usize) bool {
    const exe = std.mem.span(name orelse return false);
    const path_env: ?Str = c.getenv("PATH");
    const path = std.mem.span(path_env orelse return false);
    if (exe.len == 0 or std.mem.indexOfScalar(u8, exe, '/') != null or path.len == 0) return false;

    var cur = path;
    while (cur.len != 0) {
        const colon = std.mem.indexOfScalar(u8, cur, ':');
        const dir = cur[0 .. colon orelse cur.len];
        if (dir.len > 0 and dir.len < 900) {
            var candidate: [1024]u8 = undefined;
            const n = interop.bprint(&candidate, "{s}/{s}", .{ dir, exe });
            if (n < candidate.len and isExecutableFile(@ptrCast(&candidate))) {
                return copyPath(out, outsz, @ptrCast(&candidate));
            }
        }
        cur = cur[(colon orelse break) + 1 ..];
    }
    return false;
}

fn resolveSelfExecutable(out: ?[*]u8, outsz: usize) bool {
    var buf: [1024]u8 = undefined;
    if (is_windows) {
        const n = c.GetModuleFileNameA(null, &buf, buf.len);
        if (n > 0 and n < buf.len) return copyPath(out, outsz, @ptrCast(&buf));
        return false;
    } else if (is_apple) {
        var sz: u32 = buf.len;
        if (c._NSGetExecutablePath(&buf, &sz) == 0 and buf[0] != 0) {
            return copyPath(out, outsz, @ptrCast(&buf));
        }
        return false;
    } else {
        const len = c.readlink("/proc/self/exe", &buf, buf.len - 1);
        if (len > 0) {
            buf[@intCast(len)] = 0;
            return copyPath(out, outsz, @ptrCast(&buf));
        }
        return false;
    }
}

pub export fn cbm_resolve_binary_path(argv0: ?Str, out: ?[*]u8, outsz: usize) bool {
    const dst = out orelse return false;
    if (outsz == 0) return false;
    dst[0] = 0;

    if (is_windows) {
        if (argv0) |a0| {
            if (a0[0] != 0) {
                const attrs = c.GetFileAttributesA(a0);
                if (attrs != INVALID_FILE_ATTRIBUTES and (attrs & c.FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    return copyPath(dst, outsz, a0);
                }
            }
        }
        if (resolveSelfExecutable(dst, outsz)) return true;
    } else {
        if (argv0) |a0| {
            if (std.mem.indexOfScalar(u8, std.mem.span(a0), '/') != null and isExecutableFile(a0)) {
                return copyPath(dst, outsz, a0);
            }
        }
        if (resolveFromPath(argv0, dst, outsz)) return true;

        // #1204 (upstream 11bbf0d8, 5b96b067): never hand back a self-resolved
        // path that is not executable. After an installer's atomic rename-over the
        // image path no longer exists (Linux readlink reads "<path> (deleted)"),
        // and spawning it fails with ENOENT.
        if (resolveSelfExecutable(dst, outsz)) {
            if (isExecutableFile(@ptrCast(dst))) return true;
            if (is_linux) {
                // The /proc/self/exe magic link still executes the in-memory old
                // build, the only image whose build fingerprint the worker accepts.
                return copyPath(dst, outsz, "/proc/self/exe");
            }
            // No magic link: fail closed, and the supervisor degrades to
            // in-process indexing instead of spawning a missing binary.
            dst[0] = 0;
            return false;
        }
    }
    return copyPath(dst, outsz, argv0);
}
