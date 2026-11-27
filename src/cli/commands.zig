//! commands.zig — the install, uninstall, update, doctor and where commands,
//! plus their helpers: the interactive prompt, download + checksum
//! verification, and the agent-config install dispatch. Implements that part
//! of src/cli/cli.h.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const cli = @import("cli.zig");
const store = @import("store.zig");

const is_windows = builtin.os.tag == .windows;
const is_macos = builtin.os.tag.isDarwin();

const CStr = cli.CStr;
const Str = cli.Str;
const sp = cli.sp;
const eql = cli.eql;
const fmtZ = cli.fmtZ;
const bufStr = cli.bufStr;
const CLI_ERR = cli.CLI_ERR;
const CLI_TRUE = cli.CLI_TRUE;
const OCTAL_PERM = 0o755;

const BINARY_NAME = if (is_windows) "codebase-memory-mcp.exe" else "codebase-memory-mcp";
const DEFAULT_DOWNLOAD_BASE = "https://github.com/octacity-org/codebase-memory-mcp/releases/latest/download";
const HOME_ERR = "error: HOME not set (use USERPROFILE on Windows)\n";

fn out() ?*c.FILE {
    return interop.stdout();
}

fn err() ?*c.FILE {
    return interop.stderr();
}

fn stdinIsTty() bool {
    return if (is_windows) c._isatty(0) != 0 else c.isatty(0) != 0;
}

fn hasArg(argc: c_int, argv: [*c][*c]u8, names: []const []const u8) bool {
    var i: usize = 0;
    while (i < @as(usize, @intCast(@max(argc, 0)))) : (i += 1) {
        for (names) |n| if (eql(sp(argv[i]), n)) return true;
    }
    return false;
}

// ── Interactive prompt ─────────────────────────────────────────────

const AUTO_YES = 1;
const AUTO_NO = -1;

/// Global auto-answer mode: 0=interactive, 1=always yes, -1=always no.
var auto_answer: c_int = 0;

/// Test seam: force the auto-answer state so non-interactive bug-repro tests
/// can drive promptYn() deterministically (1 => yes, -1 => no, 0 => prompt).
/// Not declared in cli.h (internal); the repro runner carries an extern
/// forward declaration. Production never calls this.
export fn cbm_set_auto_answer_for_test(value: c_int) void {
    auto_answer = value;
}

fn parseAutoAnswer(argc: c_int, argv: [*c][*c]u8) void {
    var i: usize = 0;
    while (i < @as(usize, @intCast(@max(argc, 0)))) : (i += 1) {
        const a = sp(argv[i]);
        if (eql(a, "-y") or eql(a, "--yes")) auto_answer = AUTO_YES;
        if (eql(a, "-n") or eql(a, "--no")) auto_answer = AUTO_NO;
    }
}

fn promptYn(question: Str) bool {
    if (auto_answer == AUTO_YES) {
        _ = c.printf("%s (y/n): y (auto)\n", question);
        return true;
    }
    if (auto_answer == AUTO_NO) {
        _ = c.printf("%s (y/n): n (auto)\n", question);
        return false;
    }

    // Non-interactive stdin: default to "no" to avoid hanging.
    if (!is_windows and c.isatty(c.fileno(interop.stdin())) == 0) {
        _ = c.fprintf(err(), "error: interactive prompt requires a terminal. Use -y or -n flags.\n");
        return false;
    }

    _ = c.printf("%s (y/n): ", question);
    _ = c.fflush(out());

    var buf: [16]u8 = undefined;
    if (c.fgets(&buf, buf.len, interop.stdin()) == null) return false;
    return buf[0] == 'y' or buf[0] == 'Y';
}

// ── SHA-256 checksum verification ──────────────────────────────────

const SHA256_HEX_LEN = 64;
/// Minimum line length in checksums.txt: 64 hex + 2 spaces + 1 char filename.
const CHECKSUM_LINE_MIN = SHA256_HEX_LEN + 2;

/// Compute the SHA-256 of a file in-process (no external hashing tool — those
/// differ per OS, may be absent, and mis-quote paths under cmd.exe). Writes a
/// 64-char hex digest + NUL to out. Returns 0 on success. Exercised directly
/// by the self-update checksum regression test.
export fn cbm_cli_sha256_file(path: [*c]const u8, out_hex: [*c]u8, out_size: usize) c_int {
    if (out_size < SHA256_HEX_LEN + 1) return CLI_ERR;
    const fp = c.cbm_fopen(path, "rb");
    if (fp == null) return CLI_ERR;
    var ctx: c.cbm_sha256_ctx = undefined;
    c.cbm_sha256_init(&ctx);
    var buf: [1024]u8 = undefined;
    while (true) {
        const n = c.fread(&buf, 1, buf.len, fp);
        if (n == 0) break;
        c.cbm_sha256_update(&ctx, &buf, n);
    }
    const read_err = c.ferror(fp);
    _ = c.fclose(fp);
    if (read_err != 0) return CLI_ERR;
    var digest: [c.CBM_SHA256_DIGEST_LEN]u8 = undefined;
    c.cbm_sha256_final(&ctx, &digest);
    const hex = std.fmt.bytesToHex(digest, .lower);
    @memcpy(out_hex[0..hex.len], &hex);
    out_hex[SHA256_HEX_LEN] = 0;
    return 0;
}

// ── Download helper (shell-free curl via exec) ─────────────────────

fn exec(argv: []const CStr) c_int {
    var buf: [8]CStr = undefined;
    @memcpy(buf[0..argv.len], argv);
    buf[argv.len] = null;
    return c.cbm_exec_no_shell(&buf);
}

fn downloadToFile(url: CStr, dest: CStr) c_int {
    return exec(&.{ "curl", "-fSL", "--progress-bar", "-o", dest, url });
}

fn downloadToFileQuiet(url: CStr, dest: CStr) c_int {
    return exec(&.{ "curl", "-fsSL", "-o", dest, url });
}

// ── macOS ad-hoc signing ───────────────────────────────────────────

fn macosAdhocSign(binary_path: CStr) c_int {
    // Remove quarantine xattr (best effort — may not exist).
    _ = exec(&.{ "xattr", "-d", "com.apple.quarantine", binary_path });
    // Ad-hoc sign (required for arm64, harmless for x86_64).
    return exec(&.{ "codesign", "--sign", "-", "--force", binary_path });
}

// ── Kill other MCP server instances ────────────────────────────────

fn killOtherInstances() c_int {
    if (is_windows) {
        // taskkill /IM kills ALL matching processes INCLUDING self.
        // Use /FI filter to exclude our own PID.
        var pid_filter: [64]u8 = undefined;
        const filter = fmtZ(&pid_filter, "PID ne {d}", .{std.os.windows.GetCurrentProcessId()});
        _ = exec(&.{ "taskkill", "/F", "/FI", "IMAGENAME eq codebase-memory-mcp.exe", "/FI", filter });
        return 0;
    } else {
        var killed: c_int = 0;
        const self = c.getpid();
        const fp = c.cbm_popen("pgrep -x codebase-memory-mcp", "r");
        if (fp == null) return 0;
        var line: [32]u8 = undefined;
        while (c.fgets(&line, line.len, fp) != null) {
            const pid: c.pid_t = @truncate(c.strtol(&line, null, 10));
            if (pid > 0 and pid != self and c.kill(pid, c.SIGTERM) == 0) killed += 1;
        }
        _ = c.cbm_pclose(fp);
        return killed;
    }
}

/// Say why `update` failed: the step it was in, what went wrong there and,
/// when the failure came from the OS, the OS error text (os_errno 0 = none).
/// errno is what the C runtime sets on every platform for the file calls the
/// update makes, so no GetLastError translation is needed on Windows.
fn updateFailed(step: Str, detail: Str, os_errno: c_int) void {
    if (os_errno != 0) {
        _ = c.fprintf(err(), "error: update failed while %s: %s: %s\n", step, detail, c.strerror(os_errno));
    } else {
        _ = c.fprintf(err(), "error: update failed while %s: %s\n", step, detail);
    }
}

const STEP_DOWNLOAD: Str = "downloading the release archive";
const STEP_VERIFY: Str = "verifying the download";
const STEP_EXTRACT: Str = "extracting the archive";
const STEP_REPLACE: Str = "replacing the installed binary";

/// Download checksums.txt and verify the archive integrity. Every failure is
/// reported here, naming the verification step.
/// Returns: 0 = verified OK, 1 = mismatch (FAIL), -1 = could not verify.
fn verifyDownloadChecksum(archive_path: CStr, archive_name: Str) c_int {
    var checksum_buf: [256]u8 = undefined;
    const checksum_file = fmtZ(&checksum_buf, "{s}/cbm-checksums.txt", .{sp(c.cbm_tmpdir())});

    var dl_base_buf: [512]u8 = undefined;
    const dl_base = c.cbm_safe_getenv("CBM_DOWNLOAD_URL", &dl_base_buf, dl_base_buf.len, null);
    var url_buf: [512]u8 = undefined;
    const checksum_url = if (dl_base != null and dl_base[0] != 0)
        fmtZ(&url_buf, "{s}/checksums.txt", .{sp(dl_base)})
    else
        fmtZ(&url_buf, "{s}", .{DEFAULT_DOWNLOAD_BASE ++ "/checksums.txt"});
    var detail_buf: [1024]u8 = undefined;
    const curl_rc = downloadToFileQuiet(checksum_url, checksum_file);
    if (curl_rc != 0) {
        updateFailed(STEP_VERIFY, fmtZ(&detail_buf, "could not download {s} (curl exit {d})", .{ sp(checksum_url), curl_rc }), 0);
        _ = c.cbm_unlink(checksum_file);
        return CLI_ERR;
    }

    const fp = c.fopen(checksum_file, "r");
    const open_errno = interop.errno();
    _ = c.cbm_unlink(checksum_file);
    if (fp == null) {
        updateFailed(STEP_VERIFY, fmtZ(&detail_buf, "cannot open {s}", .{sp(checksum_file)}), open_errno);
        return CLI_ERR;
    }

    var expected = std.mem.zeroes([SHA256_HEX_LEN + 1]u8);
    var line: [512]u8 = undefined;
    while (c.fgets(&line, line.len, fp) != null) {
        // Format: <64-char sha256>  <filename>\n
        const l = bufStr(&line);
        if (l.len > CHECKSUM_LINE_MIN and std.mem.indexOf(u8, l, std.mem.span(archive_name)) != null) {
            @memcpy(expected[0..SHA256_HEX_LEN], l[0..SHA256_HEX_LEN]);
            break;
        }
    }
    _ = c.fclose(fp);

    if (expected[0] == 0) {
        updateFailed(STEP_VERIFY, fmtZ(&detail_buf, "{s} is not listed in checksums.txt", .{std.mem.span(archive_name)}), 0);
        return CLI_ERR;
    }

    var actual = std.mem.zeroes([SHA256_HEX_LEN + 1]u8);
    interop.setErrno(0);
    if (cbm_cli_sha256_file(archive_path, &actual, actual.len) != 0) {
        updateFailed(STEP_VERIFY, fmtZ(&detail_buf, "cannot read {s} to compute its checksum", .{sp(archive_path)}), interop.errno());
        return CLI_ERR;
    }

    if (!eql(bufStr(&expected), bufStr(&actual))) {
        updateFailed(STEP_VERIFY, "CHECKSUM MISMATCH — downloaded binary may be compromised!", 0);
        _ = c.fprintf(err(), "  expected: %s\n", @as(CStr, &expected));
        _ = c.fprintf(err(), "  actual:   %s\n", @as(CStr, &actual));
        return CLI_TRUE;
    }

    _ = c.printf("Checksum verified: %s\n", @as(CStr, &actual));
    return 0;
}

// ── Detect OS/arch for download URL ────────────────────────────────

const detected_os: Str = if (is_windows) "windows" else if (is_macos) "darwin" else "linux";
const detected_arch: Str = if (builtin.cpu.arch == .aarch64) "arm64" else "amd64";

// ── Agent config install/refresh (shared by install + update) ──────

const AgentName = struct { flag: bool, name: Str };

/// Print detected agent names on a single line.
fn printDetectedAgents(a: *const c.cbm_detected_agents_t) void {
    const agents = [_]AgentName{
        .{ .flag = a.claude_code, .name = "Claude-Code" },
        .{ .flag = a.codex, .name = "Codex" },
        .{ .flag = a.gemini, .name = "Gemini-CLI" },
        .{ .flag = a.zed, .name = "Zed" },
        .{ .flag = a.opencode, .name = "OpenCode" },
        .{ .flag = a.antigravity, .name = "Antigravity" },
        .{ .flag = a.aider, .name = "Aider" },
        .{ .flag = a.kilocode, .name = "KiloCode" },
        .{ .flag = a.vscode, .name = "VS-Code" },
        .{ .flag = a.cursor, .name = "Cursor" },
        .{ .flag = a.openclaw, .name = "OpenClaw" },
        .{ .flag = a.kiro, .name = "Kiro" },
        .{ .flag = a.junie, .name = "Junie" },
    };
    _ = c.printf("Detected agents:");
    var any = false;
    for (agents) |ag| {
        if (ag.flag) {
            _ = c.printf(" %s", ag.name);
            any = true;
        }
    }
    if (!any) _ = c.printf(" (none)");
    _ = c.printf("\n\n");
}

// Install plan recorder (issue #388).
// When install_plan != null, the install path runs as a dry-run and each
// write site records its planned target HERE — at the same point it would
// perform the write — so the emitted plan cannot drift from actual install
// behavior (it is the same code path with mutations disabled).
const PlanEntry = struct {
    agent: [32]u8,
    kind: [32]u8, // mcp_config | instructions | skills | hook
    path: [1024]u8,
};

const Plan = std.ArrayList(PlanEntry);

var install_plan: ?*Plan = null;
var install_replace_config = false;
var install_config_conflicts: c_int = 0;

const ConfigDecision = enum { write, unchanged, conflict };

fn configContainsCommand(content: []const u8, binary_path: []const u8) bool {
    if (std.mem.indexOf(u8, content, binary_path) != null) return true;
    var escaped: std.ArrayList(u8) = .empty;
    defer escaped.deinit(cli.gpa);
    escaped.ensureTotalCapacity(cli.gpa, binary_path.len * 2) catch return false;
    for (binary_path) |ch| {
        if (ch == '\\' or ch == '"') escaped.appendAssumeCapacity('\\');
        escaped.appendAssumeCapacity(ch);
    }
    return std.mem.indexOf(u8, content, escaped.items) != null;
}

fn installConfigDecision(label: Str, config_path: CStr, binary_path: CStr) ConfigDecision {
    const content_z = cli.readFileStr(config_path, null) orelse return .write;
    defer c.free(content_z);
    const content = std.mem.span(content_z);
    const has_entry = std.mem.indexOf(u8, content, "\"codebase-memory-mcp\"") != null or
        cli.hasCodexMcpSection(content);
    if (!has_entry) return .write;
    if (configContainsCommand(content, sp(binary_path))) return .unchanged;
    if (install_replace_config) return .write;

    _ = c.fprintf(err(), "error: %s already has a codebase-memory-mcp entry pointing elsewhere:\n" ++
        "  %s\n" ++
        "Re-run with --replace-config to replace it explicitly.\n", label, config_path);
    install_config_conflicts += 1;
    return .conflict;
}

fn planRecord(agent: []const u8, kind: []const u8, path: CStr) void {
    const pl = install_plan orelse return;
    if (path == null or path[0] == 0) return;
    const e = pl.addOne(cli.gpa) catch return;
    _ = fmtZ(&e.agent, "{s}", .{agent});
    _ = fmtZ(&e.kind, "{s}", .{kind});
    _ = fmtZ(&e.path, "{s}", .{sp(path)});
}

fn installClaudeCodeConfig(home: CStr, binary_path: CStr, force: bool, dry_run: bool) void {
    var config_buf: [1024]u8 = undefined;
    const config_dir = cli.claudeConfigDir(home, &config_buf);
    var root_buf: [1024]u8 = undefined;
    const user_root = cli.claudeUserRoot(home, &root_buf);

    var skills_buf: [1024]u8 = undefined;
    const skills_dir = fmtZ(&skills_buf, "{s}/skills", .{sp(config_dir)});

    var mcp_buf: [1024]u8 = undefined;
    var legacy_buf: [1024]u8 = undefined;
    const mcp_path = fmtZ(&mcp_buf, "{s}/.mcp.json", .{sp(config_dir)});
    const legacy_mcp_path = fmtZ(&legacy_buf, "{s}/.claude.json", .{sp(user_root)});
    // Claude Code reads user-scope MCP servers from .claude.json; .mcp.json in
    // the config dir is kept for older setups. Write both.
    const mcp_paths = [_]Str{ legacy_mcp_path, mcp_path };

    // Plan mode: record the planned writes and return without mutating (#388).
    if (install_plan != null) {
        var p: [1024]u8 = undefined;
        planRecord("Claude Code", "skills", skills_dir);
        planRecord("Claude Code", "mcp_config", legacy_mcp_path);
        planRecord("Claude Code", "mcp_config", mcp_path);
        planRecord("Claude Code", "mcp_config", fmtZ(&p, "{s}/settings.json", .{sp(config_dir)}));
        planRecord("Claude Code", "hook", fmtZ(&p, "{s}/hooks/{s}", .{ sp(config_dir), cli.CMM_HOOK_GATE_SCRIPT }));
        planRecord("Claude Code", "hook", fmtZ(&p, "{s}/hooks/{s}", .{ sp(config_dir), cli.CMM_SESSION_REMINDER_SCRIPT }));
        planRecord("Claude Code", "hook", fmtZ(&p, "{s}/hooks/{s}", .{ sp(config_dir), cli.CMM_SUBAGENT_REMINDER_SCRIPT }));
        return;
    }

    _ = c.printf("Claude Code:\n");

    const skill_count = cli.cbm_install_skills(skills_dir, force, dry_run);
    _ = c.printf("  skills: %d installed\n", skill_count);

    if (cli.cbm_remove_old_monolithic_skill(skills_dir, dry_run)) {
        _ = c.printf("  removed old monolithic skill\n");
    }

    for (mcp_paths) |path| {
        const decision = installConfigDecision("Claude Code", path, binary_path);
        if (!dry_run and decision == .write) _ = cli.cbm_install_editor_mcp(binary_path, path);
        switch (decision) {
            .unchanged => _ = c.printf("  mcp: already configured (%s)\n", path),
            .write => _ = c.printf("  mcp: %s\n", path),
            .conflict => {},
        }
    }

    var settings_buf: [1024]u8 = undefined;
    const settings_path = fmtZ(&settings_buf, "{s}/settings.json", .{sp(config_dir)});
    if (!dry_run) {
        _ = cli.cbm_install_hook_gate_script(home, binary_path);
        cli.installSessionReminderScript(home);
        cli.installSubagentReminderScript(home);
        _ = cli.cbm_upsert_claude_exec_hooks(settings_path, binary_path);
    }
    _ = c.printf("  hooks: PreToolUse (Grep/Glob search-graph augmenter, non-blocking)\n");
    _ = c.printf("  hooks: SessionStart (MCP usage reminder on startup/resume/clear/compact)\n");
    _ = c.printf("  hooks: SubagentStart (MCP usage reminder for subagents)\n");

    // Migration nudge: when CLAUDE_CONFIG_DIR is set and a legacy ~/.claude tree
    // still exists, mention it so users can clean up stale artifacts.
    if (home != null and home[0] != 0) {
        var legacy_dir_buf: [1024]u8 = undefined;
        const legacy_dir = fmtZ(&legacy_dir_buf, "{s}/.claude", .{sp(home)});
        if (!eql(sp(legacy_dir), sp(config_dir)) and cli.dirExists(legacy_dir)) {
            _ = c.fprintf(err(), "  note: $CLAUDE_CONFIG_DIR=%s used; legacy %s still exists.\n" ++
                "        Remove stale {skills,hooks,settings.json,.mcp.json} there if " ++
                "no longer needed.\n", config_dir, legacy_dir);
        }
    }
}

const InstallMcpFn = *const fn ([*c]const u8, [*c]const u8) callconv(.c) c_int;

/// Install MCP config + optional instructions for a generic agent.
fn installGenericAgentConfig(label: Str, binary_path: CStr, config_path: CStr, instr_path: CStr, dry_run: bool, install_mcp: InstallMcpFn) void {
    // Plan mode: record planned writes, mutate nothing (#388).
    if (install_plan != null) {
        planRecord(std.mem.span(label), "mcp_config", config_path);
        if (instr_path != null) planRecord(std.mem.span(label), "instructions", instr_path);
        return;
    }
    _ = c.printf("%s:\n", label);
    const decision = installConfigDecision(label, config_path, binary_path);
    if (!dry_run and decision == .write) _ = install_mcp(binary_path, config_path);
    switch (decision) {
        .unchanged => _ = c.printf("  mcp: already configured (%s)\n", config_path),
        .write => _ = c.printf("  mcp: %s\n", config_path),
        .conflict => {},
    }
    if (instr_path != null) {
        if (!dry_run) _ = cli.cbm_upsert_instructions(instr_path, cli.cbm_get_agent_instructions());
        _ = c.printf("  instructions: %s\n", instr_path);
    }
}

/// Install Gemini CLI config with hooks.
fn installGeminiConfig(home: CStr, binary_path: CStr, dry_run: bool) void {
    var cp_buf: [1024]u8 = undefined;
    var ip_buf: [1024]u8 = undefined;
    const cp = fmtZ(&cp_buf, "{s}/.gemini/settings.json", .{sp(home)});
    const ip = fmtZ(&ip_buf, "{s}/.gemini/GEMINI.md", .{sp(home)});
    installGenericAgentConfig("Gemini CLI", binary_path, cp, ip, dry_run, cli.cbm_install_editor_mcp);
    if (install_plan != null) {
        planRecord("Gemini CLI", "hook", cp); // BeforeTool + SessionStart in settings.json
        return;
    }
    if (!dry_run) {
        _ = cli.cbm_upsert_gemini_hooks(cp);
        _ = cli.cbm_upsert_gemini_session_hooks(cp);
    }
    _ = c.printf("  hooks: BeforeTool + SessionStart (codebase-memory-mcp reminder)\n");
}

/// Install MCP configs for CLI-based agents (Codex, Gemini, OpenCode,
/// Antigravity, Aider).
fn installCliAgentConfigs(agents: *const c.cbm_detected_agents_t, home: CStr, binary_path: CStr, dry_run: bool) void {
    const h = sp(home);
    var cp_buf: [1024]u8 = undefined;
    var ip_buf: [1024]u8 = undefined;
    if (agents.codex) {
        const cp = fmtZ(&cp_buf, "{s}/.codex/config.toml", .{h});
        const ip = fmtZ(&ip_buf, "{s}/.codex/AGENTS.md", .{h});
        installGenericAgentConfig("Codex CLI", binary_path, cp, ip, dry_run, cli.cbm_upsert_codex_mcp);
        // Choose the hook target: if ~/.codex/hooks.json already exists, the
        // user manages Codex hooks via the JSON representation — write the
        // SessionStart reminder there instead of config.toml. Writing both
        // makes Codex warn about loading hooks from two representations (#570).
        // config.toml remains the mcp_config target above either way.
        var hooks_buf: [1024]u8 = undefined;
        const hooks_json = fmtZ(&hooks_buf, "{s}/.codex/hooks.json", .{h});
        const use_hooks_json = c.cbm_file_exists(hooks_json);
        if (install_plan != null) {
            planRecord("Codex CLI", "hook", if (use_hooks_json) hooks_json else cp);
        } else {
            if (!dry_run) {
                if (use_hooks_json) {
                    _ = cli.cbm_upsert_gemini_session_hooks(hooks_json);
                } else {
                    _ = cli.cbm_upsert_codex_hooks(cp);
                }
            }
            _ = c.printf("  hooks: SessionStart (codebase-memory-mcp reminder)\n");
        }
    }
    if (agents.gemini) installGeminiConfig(home, binary_path, dry_run);
    if (agents.opencode) {
        const cp = cli.opencodeConfigPath(h, &cp_buf);
        const ip = fmtZ(&ip_buf, "{s}/.config/opencode/AGENTS.md", .{h});
        installGenericAgentConfig("OpenCode", binary_path, cp, ip, dry_run, cli.cbm_upsert_opencode_mcp);
    }
    if (agents.antigravity) {
        // MCP config is the SHARED Antigravity config (CLI + IDE), not a
        // per-tool file (2026 unification).
        const cp = fmtZ(&cp_buf, "{s}/.gemini/config/mcp_config.json", .{h});
        const ip = fmtZ(&ip_buf, "{s}/.gemini/antigravity-cli/AGENTS.md", .{h});
        if (!dry_run and install_plan == null) {
            var cfg_dir: [1024]u8 = undefined;
            _ = c.cbm_mkdir_p(fmtZ(&cfg_dir, "{s}/.gemini/config", .{h}), OCTAL_PERM);
        }
        installGenericAgentConfig("Antigravity", binary_path, cp, ip, dry_run, cli.cbm_upsert_antigravity_mcp);
        // Antigravity CLI is Gemini-lineage and keeps a settings.json under
        // ~/.gemini/antigravity-cli/; install the SessionStart reminder there
        // using the shared Gemini hook JSON schema.
        var sp_buf: [1024]u8 = undefined;
        const settings = fmtZ(&sp_buf, "{s}/.gemini/antigravity-cli/settings.json", .{h});
        if (install_plan != null) {
            planRecord("Antigravity", "hook", settings);
        } else {
            if (!dry_run) _ = cli.cbm_upsert_gemini_session_hooks(settings);
            _ = c.printf("  hooks: SessionStart (codebase-memory-mcp reminder)\n");
        }
    }
    if (agents.aider) {
        const ip = fmtZ(&ip_buf, "{s}/CONVENTIONS.md", .{h});
        if (install_plan != null) {
            planRecord("Aider", "instructions", ip);
        } else {
            _ = c.printf("Aider:\n");
            if (!dry_run) _ = cli.cbm_upsert_instructions(ip, cli.cbm_get_agent_instructions());
            _ = c.printf("  instructions: %s\n", ip);
        }
    }
}

/// Scan Code/User/profiles/ and install (or plan) a per-profile mcp.json for
/// each existing profile subdirectory, so VS Code profile users inherit the MCP
/// server without manual steps (#431). No-op when profiles/ is absent.
fn installVscodeProfileConfigs(code_user: []const u8, binary_path: CStr, dry_run: bool) void {
    var profiles_buf: [1024]u8 = undefined;
    const profiles_dir = fmtZ(&profiles_buf, "{s}/profiles", .{code_user});
    const d = c.cbm_opendir(profiles_dir) orelse return;
    defer c.cbm_closedir(d);
    while (c.cbm_readdir(d)) |ent| {
        const name = sp(&ent.*.name);
        if (eql(name, ".") or eql(name, "..")) continue;
        var profile_buf: [1024]u8 = undefined;
        const profile_path = fmtZ(&profile_buf, "{s}/{s}", .{ sp(profiles_dir), name });
        if (!cli.dirExists(profile_path)) continue;
        var cp_buf: [1024]u8 = undefined;
        const cp = fmtZ(&cp_buf, "{s}/mcp.json", .{sp(profile_path)});
        installGenericAgentConfig("VS Code", binary_path, cp, null, dry_run, cli.cbm_install_vscode_mcp);
    }
}

/// Zed's settings.json path.
fn zedSettingsPath(home: []const u8, buf: []u8) Str {
    if (is_macos) return fmtZ(buf, "{s}/.config/zed/settings.json", .{home});
    if (is_windows) return fmtZ(buf, "{s}/Zed/settings.json", .{sp(c.cbm_app_local_dir())});
    return fmtZ(buf, "{s}/zed/settings.json", .{sp(c.cbm_app_config_dir())});
}

/// KiloCode's MCP settings path.
fn kilocodeSettingsPath(home: []const u8, buf: []u8) Str {
    if (is_macos) return fmtZ(buf, "{s}/Library/Application Support/Code/User/globalStorage/" ++
        "kilocode.kilo-code/settings/mcp_settings.json", .{home});
    return fmtZ(buf, "{s}/Code/User/globalStorage/kilocode.kilo-code/settings/mcp_settings.json", .{sp(c.cbm_app_config_dir())});
}

/// VS Code's Code/User directory.
fn vscodeUserDir(home: []const u8, buf: []u8) Str {
    if (is_macos) return fmtZ(buf, "{s}/Library/Application Support/Code/User", .{home});
    return fmtZ(buf, "{s}/Code/User", .{sp(c.cbm_app_config_dir())});
}

/// Install MCP configs for editor-based agents (Zed, KiloCode, VS Code,
/// OpenClaw, ...).
fn installEditorAgentConfigs(agents: *const c.cbm_detected_agents_t, home: CStr, binary_path: CStr, dry_run: bool) void {
    const h = sp(home);
    var cp_buf: [1024]u8 = undefined;
    var ip_buf: [1024]u8 = undefined;
    if (agents.zed) {
        installGenericAgentConfig("Zed", binary_path, zedSettingsPath(h, &cp_buf), null, dry_run, cli.cbm_install_zed_mcp);
    }
    if (agents.kilocode) {
        const cp = kilocodeSettingsPath(h, &cp_buf);
        const ip = fmtZ(&ip_buf, "{s}/.kilocode/rules/codebase-memory-mcp.md", .{h});
        installGenericAgentConfig("KiloCode", binary_path, cp, ip, dry_run, cli.cbm_install_editor_mcp);
    }
    if (agents.vscode) {
        var user_buf: [1024]u8 = undefined;
        const code_user = vscodeUserDir(h, &user_buf);
        const cp = fmtZ(&cp_buf, "{s}/mcp.json", .{sp(code_user)});
        installGenericAgentConfig("VS Code", binary_path, cp, null, dry_run, cli.cbm_install_vscode_mcp);
        // VS Code profiles each keep their own settings under
        // Code/User/profiles/<id>/. The default mcp.json above does NOT apply
        // to a named profile, so write/plan a per-profile mcp.json for every
        // existing profile directory (#431).
        installVscodeProfileConfigs(sp(code_user), binary_path, dry_run);
    }
    if (agents.cursor) {
        const cp = fmtZ(&cp_buf, "{s}/.cursor/mcp.json", .{h});
        installGenericAgentConfig("Cursor", binary_path, cp, null, dry_run, cli.cbm_install_editor_mcp);
    }
    if (agents.openclaw) {
        const cp = fmtZ(&cp_buf, "{s}/.openclaw/openclaw.json", .{h});
        installGenericAgentConfig("OpenClaw", binary_path, cp, null, dry_run, cli.cbm_install_openclaw_mcp);
    }
    if (agents.kiro) {
        const cp = fmtZ(&cp_buf, "{s}/.kiro/settings/mcp.json", .{h});
        if (!dry_run) _ = c.cbm_mkdir_p(fmtZ(&ip_buf, "{s}/.kiro/settings", .{h}), OCTAL_PERM);
        installGenericAgentConfig("Kiro", binary_path, cp, null, dry_run, cli.cbm_install_editor_mcp);
    }
    if (agents.junie) {
        const cp = fmtZ(&cp_buf, "{s}/.junie/mcp/mcp.json", .{h});
        if (!dry_run) _ = c.cbm_mkdir_p(fmtZ(&ip_buf, "{s}/.junie/mcp", .{h}), OCTAL_PERM);
        installGenericAgentConfig("Junie", binary_path, cp, null, dry_run, cli.cbm_upsert_junie_mcp);
    }
}

fn installAgentConfigs(home: CStr, binary_path: CStr, force: bool, dry_run: bool) void {
    const agents = cli.cbm_detect_agents(home);
    if (install_plan == null) printDetectedAgents(&agents);
    if (agents.claude_code) installClaudeCodeConfig(home, binary_path, force, dry_run);
    installCliAgentConfigs(&agents, home, binary_path, dry_run);
    installEditorAgentConfigs(&agents, home, binary_path, dry_run);
}

/// Handle pre-existing indexes during (re)install (#607).
///
/// Returns 1 to proceed with the install, 0 to abort (user declined the
/// destructive reset prompt).
///
/// Default (reset=false): PRESERVE the indexed graph. cbm.zova is NOT
/// deleted; the message tells the user the indexes are kept and that they
/// should re-index after install to pick up this version's extraction
/// improvements. The old behaviour deleted every index here while printing
/// "must be rebuilt" and never rebuilt — silent, irrecoverable data loss
/// (#607).
///
/// Opt-in (reset=true, via `install --reset-indexes`): keep the original
/// prompt-and-delete behaviour, with honest "Delete" wording.
///
/// Intentionally NOT declared in cli.h (internal helper); repro_issue607.c
/// carries an extern forward declaration.
export fn cbm_install_handle_existing_indexes(home: [*c]const u8, reset: bool, dry_run: bool) c_int {
    const index_count = projectCount(home);
    if (index_count <= 0) return 1; // nothing to handle, proceed

    if (!reset) {
        // Default: preserve. Be honest — keep the indexes, advise re-index.
        _ = c.printf("Found %d existing index(es). Keeping them. After install, " ++
            "re-index to pick up this version's improvements:\n", index_count);
        _ = store.cbm_list_indexes(home);
        _ = c.printf("\n");
        return 1; // proceed without deleting
    }

    // Opt-in reset (--reset-indexes): the original prompt-and-delete path.
    _ = c.printf("Found %d existing index(es):\n", index_count);
    _ = store.cbm_list_indexes(home);
    _ = c.printf("\n");
    if (!promptYn("Delete these indexes and continue with install?")) {
        _ = c.printf("Install cancelled.\n");
        return 0; // abort
    }
    if (!dry_run) {
        const removed = store.cbm_remove_indexes(home);
        _ = c.printf("Removed %d index(es).\n\n", removed);
    }
    return 1; // proceed
}

/// Number of indexed projects, without printing them.
fn projectCount(home: CStr) c_int {
    return store.listZovaProjects(home, false);
}

// ── Subcommand: install ────────────────────────────────────────────

extern "kernel32" fn GetModuleFileNameA(module: ?*anyopaque, filename: [*]u8, size: u32) callconv(.winapi) u32;
extern fn _NSGetExecutablePath(buf: [*]u8, bufsize: *u32) c_int;

/// The running binary's path. Falls back to ~/.local/bin/.
fn detectSelfPath(buf: []u8, home: CStr) Str {
    buf[0] = 0;
    if (is_windows) {
        _ = GetModuleFileNameA(null, buf.ptr, @intCast(buf.len));
        _ = c.cbm_normalize_path_sep(buf.ptr);
    } else if (is_macos) {
        var size: u32 = @intCast(buf.len);
        if (_NSGetExecutablePath(buf.ptr, &size) != 0) buf[0] = 0;
    } else {
        const n = c.readlink("/proc/self/exe", buf.ptr, buf.len - 1);
        if (n > 0) buf[@intCast(n)] = 0;
    }
    if (buf[0] == 0) return installTargetPath(home, buf);
    return @ptrCast(buf.ptr);
}

fn installTargetPath(home: CStr, buf: []u8) Str {
    return fmtZ(buf, "{s}/.local/bin/" ++ BINARY_NAME, .{sp(home)});
}

/// A malloc'd, NUL-terminated string builder for the C report helpers.
const StrBuf = struct {
    list: std.ArrayList(u8) = .empty,

    fn print(self: *StrBuf, comptime fmt: []const u8, args: anytype) void {
        self.list.print(cli.gpa, fmt, args) catch {};
    }

    /// The contents as a C string the caller frees with free(), or null.
    fn finish(self: *StrBuf) ?[*:0]u8 {
        const s = self.list.toOwnedSliceSentinel(cli.gpa, 0) catch {
            self.list.deinit(cli.gpa);
            return null;
        };
        return s.ptr;
    }
};

fn detectedAgentsCsv(det: *const c.cbm_detected_agents_t, buf: []u8) Str {
    const names = [_]AgentName{
        .{ .flag = det.claude_code, .name = "Claude Code" },
        .{ .flag = det.codex, .name = "Codex CLI" },
        .{ .flag = det.gemini, .name = "Gemini CLI" },
        .{ .flag = det.zed, .name = "Zed" },
        .{ .flag = det.opencode, .name = "OpenCode" },
        .{ .flag = det.antigravity, .name = "Antigravity" },
        .{ .flag = det.aider, .name = "Aider" },
        .{ .flag = det.kilocode, .name = "KiloCode" },
        .{ .flag = det.vscode, .name = "VS Code" },
        .{ .flag = det.cursor, .name = "Cursor" },
        .{ .flag = det.openclaw, .name = "OpenClaw" },
        .{ .flag = det.kiro, .name = "Kiro" },
        .{ .flag = det.junie, .name = "Junie" },
    };
    var o: cli.OffsetBuf = .{ .buf = buf };
    buf[0] = 0;
    for (names) |n| {
        if (!n.flag) continue;
        if (o.off > 0) o.print(", ", .{});
        o.print("{s}", .{std.mem.span(n.name)});
    }
    if (buf[0] == 0) return fmtZ(buf, "none", .{});
    return @ptrCast(buf.ptr);
}

fn pathContainsDir(dir: []const u8) bool {
    var path_copy: [4096]u8 = undefined;
    if (dir.len == 0 or c.cbm_safe_getenv("PATH", &path_copy, path_copy.len, null) == null) return false;
    var it = std.mem.splitScalar(u8, bufStr(&path_copy), if (is_windows) ';' else ':');
    while (it.next()) |part| {
        if (eql(part, dir)) return true;
    }
    return false;
}

fn collectReportConfigPaths(home: CStr) ?[*:0]u8 {
    var sb: StrBuf = .{};
    const h = sp(home);
    sb.print("{s}/.codex/config.toml\n", .{h});
    sb.print("{s}/.codex/AGENTS.md\n", .{h});
    var zed: [1024]u8 = undefined;
    sb.print("{s}\n", .{sp(zedSettingsPath(h, &zed))});
    const rc = cli.cbm_detect_shell_rc(home);
    if (rc != null and rc[0] != 0) sb.print("{s}\n", .{sp(rc)});
    return sb.finish();
}

/// Build the agent.install.plan.v1 receipt (#388): a machine-readable list of
/// the config / instruction / hook files `install` WOULD write, produced by
/// running the real install dispatch in record-only mode (no mutation, no
/// network). Returns a heap JSON string (caller frees) or NULL.
pub export fn cbm_build_install_plan_json(home: [*c]const u8, binary_path: [*c]const u8) [*c]u8 {
    if (home == null or binary_path == null) return null;

    // Same code path as a real install, but mutations disabled and every write
    // site records into `plan` — so the receipt cannot drift from behavior.
    var plan: Plan = .empty;
    defer plan.deinit(cli.gpa);
    install_plan = &plan;
    installAgentConfigs(home, binary_path, false, true);
    install_plan = null;

    const det = cli.cbm_detect_agents(home);
    const names = [_]AgentName{
        .{ .flag = det.claude_code, .name = "claude-code" },
        .{ .flag = det.codex, .name = "codex" },
        .{ .flag = det.gemini, .name = "gemini" },
        .{ .flag = det.zed, .name = "zed" },
        .{ .flag = det.opencode, .name = "opencode" },
        .{ .flag = det.antigravity, .name = "antigravity" },
        .{ .flag = det.aider, .name = "aider" },
        .{ .flag = det.kilocode, .name = "kilocode" },
        .{ .flag = det.vscode, .name = "vscode" },
        .{ .flag = det.cursor, .name = "cursor" },
        .{ .flag = det.openclaw, .name = "openclaw" },
        .{ .flag = det.kiro, .name = "kiro" },
        .{ .flag = det.junie, .name = "junie" },
    };

    const doc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(doc);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_str(doc, root, "type", "agent.install.plan.v1");

    var target_buf: [1024]u8 = undefined;
    const binary_target = installTargetPath(home, &target_buf);
    const rc = cli.cbm_detect_shell_rc(home);
    const shell_rc: CStr = if (rc != null and rc[0] != 0) rc else "";
    _ = c.yyjson_mut_obj_add_strcpy(doc, root, "binary_target_planned", binary_target);
    _ = c.yyjson_mut_obj_add_strcpy(doc, root, "shell_rc_planned", shell_rc);

    const agents = c.yyjson_mut_arr(doc);
    for (names) |n| {
        if (n.flag) _ = c.yyjson_mut_arr_add_str(doc, agents, n.name);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "agents_detected", agents);

    const configs = c.yyjson_mut_arr(doc);
    const instrs = c.yyjson_mut_arr(doc);
    const hooks = c.yyjson_mut_arr(doc);
    for (plan.items) |*e| {
        const kind = bufStr(&e.kind);
        if (eql(kind, "mcp_config")) {
            _ = c.yyjson_mut_arr_add_strcpy(doc, configs, &e.path);
        } else if (eql(kind, "hook")) {
            const hk = c.yyjson_mut_obj(doc);
            _ = c.yyjson_mut_obj_add_strcpy(doc, hk, "agent", &e.agent);
            _ = c.yyjson_mut_obj_add_strcpy(doc, hk, "path", &e.path);
            _ = c.yyjson_mut_arr_add_val(hooks, hk);
        } else {
            _ = c.yyjson_mut_arr_add_strcpy(doc, instrs, &e.path);
        }
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "config_files_planned", configs);
    _ = c.yyjson_mut_obj_add_val(doc, root, "instruction_files_planned", instrs);
    _ = c.yyjson_mut_obj_add_val(doc, root, "hooks_planned", hooks);

    var sb: StrBuf = .{};
    for (plan.items) |*e| sb.print("{s}\n", .{bufStr(&e.path)});
    if (sb.finish()) |planned_paths| {
        defer c.free(planned_paths);
        const overview = c.cbm_cli_zig_install_plan_overview(binary_target, shell_rc, planned_paths);
        if (overview != null) {
            _ = c.yyjson_mut_obj_add_strcpy(doc, root, "human_summary", overview);
            c.cbm_cli_zig_free(overview);
        }
    }
    _ = c.yyjson_mut_obj_add_bool(doc, root, "writes_started", false);
    _ = c.yyjson_mut_obj_add_bool(doc, root, "network_after_install", false);
    _ = c.yyjson_mut_obj_add_str(doc, root, "next_safe_command", "codebase-memory-mcp install -y");

    return c.yyjson_mut_write(doc, c.YYJSON_WRITE_PRETTY, null); // malloc'd; caller frees
}

fn printInstallHelp() void {
    _ = c.printf("codebase-memory-mcp install %s\n\n", cli.cbm_cli_get_version());
    _ = c.printf("Usage:\n");
    _ = c.printf("  codebase-memory-mcp install [-y|-n] [--replace] [--replace-config] [--dry-run] [--plan]\n\n");
    _ = c.printf("Options:\n");
    _ = c.printf("  --help           Print this help and make no changes\n");
    _ = c.printf("  --plan           Print the files install would touch as JSON and make no changes\n");
    _ = c.printf("  --dry-run        Show install actions without writing files\n");
    _ = c.printf("  --replace        Replace an existing installed binary without prompting\n");
    _ = c.printf("  --force          Legacy alias for --replace; also refresh installed skills\n");
    _ = c.printf("  --replace-config Replace existing MCP entries that point to another binary\n");
    _ = c.printf("  -y, --yes        Answer yes to prompts\n");
    _ = c.printf("  -n, --no         Answer no to prompts\n");
}

/// Install/refresh all agent configs with --replace-config handling. Returns
/// false (after reporting) when conflicting entries were left unchanged.
fn installConfigsChecked(home: CStr, binary_path: CStr, force: bool, dry_run: bool, replace_config: bool) bool {
    install_replace_config = replace_config;
    install_config_conflicts = 0;
    installAgentConfigs(home, binary_path, force, dry_run);
    install_replace_config = false;
    if (install_config_conflicts != 0) {
        _ = c.fprintf(err(), "error: %d conflicting MCP configuration(s) were not changed.\n", install_config_conflicts);
        install_config_conflicts = 0;
        return false;
    }
    return true;
}

pub export fn cbm_cmd_install(argc: c_int, argv: [*c][*c]u8) c_int {
    if (hasArg(argc, argv, &.{ "--help", "-h" })) {
        printInstallHelp();
        return 0;
    }
    parseAutoAnswer(argc, argv);
    const dry_run = hasArg(argc, argv, &.{"--dry-run"});
    const force = hasArg(argc, argv, &.{"--force"});
    var replace_binary = force or hasArg(argc, argv, &.{"--replace"});
    const replace_config = hasArg(argc, argv, &.{"--replace-config"});
    const plan = hasArg(argc, argv, &.{"--plan"});

    const home = c.cbm_get_home_dir();
    if (home == null) {
        _ = c.fprintf(err(), HOME_ERR);
        return CLI_TRUE;
    }

    var target_buf: [1024]u8 = undefined;
    const bin_target = installTargetPath(home, &target_buf);

    // --plan: emit the machine-readable install receipt and exit WITHOUT
    // mutating anything (no config writes, no index deletion, no network) so
    // an agent can inspect exactly what install would touch first (#388).
    if (plan) {
        const json = cbm_build_install_plan_json(home, bin_target);
        if (json == null) {
            _ = c.fprintf(err(), "error: failed to build install plan\n");
            return CLI_TRUE;
        }
        _ = c.printf("%s\n", json);
        c.free(json);
        return 0;
    }

    _ = c.printf("codebase-memory-mcp install %s\n\n", cli.cbm_cli_get_version());

    // Step 1: Kill running MCP server instances so agents pick up new config.
    if (!dry_run) {
        const killed = killOtherInstances();
        if (killed > 0) _ = c.printf("Stopped %d running MCP server instance(s).\n\n", killed);
    }

    // Step 1c: Place the running binary at the canonical install target.
    // Previously install only re-signed whatever was already at the target, so
    // `install --force` from a freshly built binary silently kept the OLD file
    // — operators ran stale code believing they had upgraded (#472). Copy the
    // running binary to ~/.local/bin (unless we ARE that file), then sign it.
    var self_buf = std.mem.zeroes([1024]u8);
    const self_path = detectSelfPath(&self_buf, home);

    if (!cli.sameFile(self_path, bin_target)) {
        const target_exists = cli.pathExists(bin_target);
        var do_copy = !target_exists or replace_binary;
        if (target_exists and !replace_binary) {
            _ = c.printf("A different binary already exists at:\n  %s\n", bin_target);
            if (promptYn("Replace it with the binary you ran install from?")) {
                do_copy = true;
                replace_binary = true; // user approved replacement for this run
            } else {
                _ = c.printf("Keeping existing binary; configs will point at it.\n\n");
            }
        }
        if (do_copy) {
            if (dry_run) {
                _ = c.printf("Would install binary -> %s\n\n", bin_target);
            } else {
                var dir_buf: [1024]u8 = undefined;
                _ = c.cbm_mkdir_p(fmtZ(&dir_buf, "{s}/.local/bin", .{sp(home)}), OCTAL_PERM);
                if (cli.cbm_copy_binary_to_target(self_path, bin_target) != 0) {
                    _ = c.fprintf(err(), "error: failed to copy binary to %s\n", bin_target);
                    return CLI_TRUE;
                }
                _ = c.printf("Installed binary -> %s\n\n", bin_target);
            }
        }
    }

    // Step 1d: macOS ad-hoc signing of the installed binary. A freshly
    // clang-built arm64 binary is linker-signed (flags=0x20002) and gets
    // Killed:9 when spawned by an MCP host; re-signing ad-hoc (flags=0x2)
    // makes it launchable. Sign the target, not whatever the operator ran.
    if (is_macos and !dry_run and cli.pathExists(bin_target) and macosAdhocSign(bin_target) != 0) {
        _ = c.fprintf(err(), "warning: ad-hoc signing failed — binary may not run on macOS arm64\n");
    }

    // Step 3: Install/refresh all agent configs, pointing at the install target.
    if (!installConfigsChecked(home, bin_target, force, dry_run, replace_config)) return CLI_TRUE;

    // Step 4: Ensure PATH.
    var dir_buf: [1024]u8 = undefined;
    const bin_dir = fmtZ(&dir_buf, "{s}/.local/bin", .{sp(home)});
    const rc = cli.cbm_detect_shell_rc(home);
    if (rc[0] != 0) {
        const path_rc = cli.cbm_ensure_path(bin_dir, rc, dry_run);
        if (path_rc == 0) {
            _ = c.printf("\nAdded %s to PATH in %s\n", bin_dir, rc);
        } else if (path_rc == CLI_TRUE) {
            _ = c.printf("\nPATH already includes %s\n", bin_dir);
        }
    }

    _ = c.printf("\nInstall complete. Restart your shell or run:\n");
    _ = c.printf("  source %s\n", rc);
    if (dry_run) _ = c.printf("\n(dry-run — no files were modified)\n");
    return 0;
}

// ── Subcommands: doctor, where ─────────────────────────────────────

fn reportPathsCommon(terse: bool) c_int {
    const home = c.cbm_get_home_dir();
    if (home == null) {
        _ = c.fprintf(err(), HOME_ERR);
        return CLI_TRUE;
    }
    var running_buf = std.mem.zeroes([1024]u8);
    const running = detectSelfPath(&running_buf, home);
    var installed_buf: [1024]u8 = undefined;
    const installed = installTargetPath(home, &installed_buf);

    const cache_c = c.cbm_resolve_cache_dir();
    const cache_dir: CStr = if (cache_c != null) cache_c else "";
    var db_buf: [1024]u8 = undefined;
    const database_path = fmtZ(&db_buf, "{s}{s}cbm.zova", .{ sp(cache_dir), if (cache_dir[0] != 0) "/" else "" });
    const config_paths = collectReportConfigPaths(home) orelse {
        _ = c.fprintf(err(), "error: failed to build report\n");
        return CLI_TRUE;
    };
    defer c.free(config_paths);

    const report = if (terse)
        c.cbm_cli_zig_where_report(running, installed, cache_dir, database_path, config_paths)
    else blk: {
        const det = cli.cbm_detect_agents(home);
        var agents_buf: [512]u8 = undefined;
        const agents = detectedAgentsCsv(&det, &agents_buf);
        var bin_buf: [1024]u8 = undefined;
        const bin_dir = fmtZ(&bin_buf, "{s}/.local/bin", .{sp(home)});
        break :blk c.cbm_cli_zig_doctor_report(home, running, installed, cache_dir, database_path, config_paths, agents, @intFromBool(pathContainsDir(sp(bin_dir))));
    };
    if (report == null) {
        _ = c.fprintf(err(), "error: failed to build report\n");
        return CLI_TRUE;
    }
    _ = c.printf("%s", report);
    c.cbm_cli_zig_free(report);
    return 0;
}

pub export fn cbm_cmd_doctor(argc: c_int, argv: [*c][*c]u8) c_int {
    if (hasArg(argc, argv, &.{ "--help", "-h" })) {
        _ = c.printf("Usage: codebase-memory-mcp doctor\n\n");
        _ = c.printf("Reports binary path, shared Zova database, detected agents,\n");
        _ = c.printf("config paths, and PATH status. Makes no changes.\n");
        return 0;
    }
    return reportPathsCommon(false);
}

pub export fn cbm_cmd_where(argc: c_int, argv: [*c][*c]u8) c_int {
    if (hasArg(argc, argv, &.{ "--help", "-h" })) {
        _ = c.printf("Usage: codebase-memory-mcp where\n\n");
        _ = c.printf("Prints cache, binary, shared Zova database, and config paths. Makes no changes.\n");
        return 0;
    }
    return reportPathsCommon(true);
}

// ── Subcommand: uninstall ──────────────────────────────────────────

/// Remove Claude Code agent configs.
fn uninstallClaudeCode(home: CStr, dry_run: bool) void {
    var config_buf: [1024]u8 = undefined;
    const config_dir = sp(cli.claudeConfigDir(home, &config_buf));
    var root_buf: [1024]u8 = undefined;
    const user_root = sp(cli.claudeUserRoot(home, &root_buf));

    var p: [1024]u8 = undefined;
    const removed = cli.cbm_remove_skills(fmtZ(&p, "{s}/skills", .{config_dir}), dry_run);
    _ = c.printf("Claude Code: removed %d skill(s)\n", removed);

    if (!dry_run) _ = cli.cbm_remove_editor_mcp(fmtZ(&p, "{s}/.mcp.json", .{config_dir}));
    _ = c.printf("  removed MCP config entry\n");

    if (!dry_run) _ = cli.cbm_remove_editor_mcp(fmtZ(&p, "{s}/.claude.json", .{user_root}));

    if (!dry_run) {
        const settings_path = fmtZ(&p, "{s}/settings.json", .{config_dir});
        _ = cli.cbm_remove_claude_hooks(settings_path);
        _ = cli.removeSessionHooks(settings_path);
        _ = cli.cbm_remove_claude_subagent_hooks(settings_path);
    }
    _ = c.printf("  removed PreToolUse + SessionStart + SubagentStart hooks\n");
}

const RemoveMcpFn = *const fn ([*c]const u8) callconv(.c) c_int;

/// Remove MCP + instructions for a generic agent.
fn uninstallAgentMcpInstr(name: Str, config_path: CStr, instr_path: CStr, dry_run: bool, remove_fn: RemoveMcpFn) void {
    if (!dry_run) _ = remove_fn(config_path);
    _ = c.printf("%s: removed MCP config entry\n", name);
    if (instr_path != null) {
        if (!dry_run) _ = cli.cbm_remove_instructions(instr_path);
        _ = c.printf("  removed instructions\n");
    }
}

/// Remove CLI agent configs (Codex, Gemini, OpenCode, Antigravity, Aider).
fn uninstallCliAgents(agents: *const c.cbm_detected_agents_t, home: CStr, dry_run: bool) void {
    const h = sp(home);
    var cp_buf: [1024]u8 = undefined;
    var ip_buf: [1024]u8 = undefined;
    if (agents.codex) {
        const cp = fmtZ(&cp_buf, "{s}/.codex/config.toml", .{h});
        const ip = fmtZ(&ip_buf, "{s}/.codex/AGENTS.md", .{h});
        uninstallAgentMcpInstr("Codex CLI", cp, ip, dry_run, cli.cbm_remove_codex_mcp);
        if (!dry_run) _ = cli.cbm_remove_codex_hooks(cp);
    }
    if (agents.gemini) {
        const cp = fmtZ(&cp_buf, "{s}/.gemini/settings.json", .{h});
        const ip = fmtZ(&ip_buf, "{s}/.gemini/GEMINI.md", .{h});
        if (!dry_run) {
            _ = cli.cbm_remove_editor_mcp(cp);
            _ = cli.cbm_remove_gemini_hooks(cp);
            _ = cli.cbm_remove_gemini_session_hooks(cp);
            _ = cli.cbm_remove_instructions(ip);
        }
        _ = c.printf("Gemini CLI: removed MCP config + hooks + instructions\n");
    }
    if (agents.opencode) {
        const cp = cli.opencodeConfigPath(h, &cp_buf);
        const ip = fmtZ(&ip_buf, "{s}/.config/opencode/AGENTS.md", .{h});
        uninstallAgentMcpInstr("OpenCode", cp, ip, dry_run, cli.cbm_remove_opencode_mcp);
    }
    if (agents.antigravity) {
        const cp = fmtZ(&cp_buf, "{s}/.gemini/config/mcp_config.json", .{h});
        const ip = fmtZ(&ip_buf, "{s}/.gemini/antigravity-cli/AGENTS.md", .{h});
        uninstallAgentMcpInstr("Antigravity", cp, ip, dry_run, cli.cbm_remove_antigravity_mcp);
        if (!dry_run) {
            _ = cli.cbm_remove_gemini_session_hooks(fmtZ(&cp_buf, "{s}/.gemini/antigravity-cli/settings.json", .{h}));
        }
    }
    if (agents.aider) {
        if (!dry_run) _ = cli.cbm_remove_instructions(fmtZ(&ip_buf, "{s}/CONVENTIONS.md", .{h}));
        _ = c.printf("Aider: removed instructions\n");
    }
}

/// Remove editor agent configs (Zed, KiloCode, VS Code, OpenClaw, ...).
fn uninstallEditorAgents(agents: *const c.cbm_detected_agents_t, home: CStr, dry_run: bool) void {
    const h = sp(home);
    var cp_buf: [1024]u8 = undefined;
    var ip_buf: [1024]u8 = undefined;
    if (agents.zed) {
        uninstallAgentMcpInstr("Zed", zedSettingsPath(h, &cp_buf), null, dry_run, cli.cbm_remove_zed_mcp);
    }
    if (agents.kilocode) {
        const cp = kilocodeSettingsPath(h, &cp_buf);
        const ip = fmtZ(&ip_buf, "{s}/.kilocode/rules/codebase-memory-mcp.md", .{h});
        uninstallAgentMcpInstr("KiloCode", cp, ip, dry_run, cli.cbm_remove_editor_mcp);
    }
    if (agents.vscode) {
        var user_buf: [1024]u8 = undefined;
        const cp = fmtZ(&cp_buf, "{s}/mcp.json", .{sp(vscodeUserDir(h, &user_buf))});
        uninstallAgentMcpInstr("VS Code", cp, null, dry_run, cli.cbm_remove_vscode_mcp);
    }
    if (agents.cursor) {
        uninstallAgentMcpInstr("Cursor", fmtZ(&cp_buf, "{s}/.cursor/mcp.json", .{h}), null, dry_run, cli.cbm_remove_editor_mcp);
    }
    if (agents.openclaw) {
        uninstallAgentMcpInstr("OpenClaw", fmtZ(&cp_buf, "{s}/.openclaw/openclaw.json", .{h}), null, dry_run, cli.cbm_remove_openclaw_mcp);
    }
    if (agents.kiro) {
        uninstallAgentMcpInstr("Kiro", fmtZ(&cp_buf, "{s}/.kiro/settings/mcp.json", .{h}), null, dry_run, cli.cbm_remove_editor_mcp);
    }
    if (agents.junie) {
        uninstallAgentMcpInstr("Junie", fmtZ(&cp_buf, "{s}/.junie/mcp/mcp.json", .{h}), null, dry_run, cli.cbm_remove_junie_mcp);
    }
}

pub export fn cbm_cmd_uninstall(argc: c_int, argv: [*c][*c]u8) c_int {
    if (hasArg(argc, argv, &.{ "--help", "-h" })) {
        _ = c.printf(
            \\Usage: codebase-memory-mcp uninstall [options]
            \\
            \\Removes the codebase-memory-mcp binary and its agent configurations.
            \\THIS IS DESTRUCTIVE. Project indexes are kept by default. An interactive
            \\terminal is asked separately; the default answer is to keep them.
            \\
            \\Options:
            \\  --dry-run          Show what would be removed, change nothing
            \\  --delete-indexes   Also delete every project index (overrides --no)
            \\  -y, --yes          Do not prompt; indexes are kept unless explicitly deleted
            \\  -n, --no           Decline prompts; --delete-indexes still takes precedence
            \\  -h, --help         Show this help and exit
            \\
            \\Run with --dry-run first if you are unsure.
            \\
        );
        return 0;
    }

    parseAutoAnswer(argc, argv);
    const dry_run = hasArg(argc, argv, &.{"--dry-run"});
    const delete_indexes_requested = hasArg(argc, argv, &.{"--delete-indexes"});

    const home = c.cbm_get_home_dir();
    if (home == null) {
        _ = c.fprintf(err(), HOME_ERR);
        return CLI_TRUE;
    }

    _ = c.printf("codebase-memory-mcp uninstall\n\n");

    const agents = cli.cbm_detect_agents(home);
    if (agents.claude_code) uninstallClaudeCode(home, dry_run);
    uninstallCliAgents(&agents, home, dry_run);
    uninstallEditorAgents(&agents, home, dry_run);

    // Step 2: Remove indexes.
    const index_count = projectCount(home);
    if (index_count > 0) {
        _ = c.printf("\nFound %d index(es):\n", index_count);
        _ = store.cbm_list_indexes(home);
        var delete_indexes = delete_indexes_requested;
        if (!delete_indexes_requested) {
            const can_ask = auto_answer == 0 and stdinIsTty();
            delete_indexes = can_ask and promptYn("Delete these indexes? (default: no)");
        }

        if (!delete_indexes) {
            const cache_c = c.cbm_resolve_cache_dir();
            const cache_dir: [*c]const u8 = if (cache_c) |dir| dir else "the cache directory";
            _ = c.printf(
                "Indexes kept in %s. To remove them, uninstall with --delete-indexes or delete the files there.\n",
                cache_dir,
            );
        } else if (dry_run) {
            _ = c.printf("(dry-run — %d index(es) would be deleted)\n", index_count);
        } else {
            const idx_removed = store.cbm_remove_indexes(home);
            _ = c.printf("Removed %d index(es).\n", idx_removed);
        }
    }

    // Step 3: Remove binary.
    var bin_buf: [1024]u8 = undefined;
    const bin_path = installTargetPath(home, &bin_buf);
    if (cli.pathExists(bin_path)) {
        if (!dry_run) _ = c.cbm_unlink(bin_path);
        _ = c.printf("Removed %s\n", bin_path);
    }
    if (!is_windows) {
        var hash_buf: [1024]u8 = undefined;
        const hash_record = fmtZ(&hash_buf, "{s}.sha256", .{sp(bin_path)});
        if (cli.pathExists(hash_record)) {
            if (!dry_run) _ = c.cbm_unlink(hash_record);
            _ = c.printf("Removed %s\n", hash_record);
        }
    }

    _ = c.printf("\nUninstall complete.\n");
    if (dry_run) _ = c.printf("(dry-run — no files were modified)\n");
    return 0;
}

// ── Subcommand: update ─────────────────────────────────────────────

/// Read archive from disk, extract binary (tar.gz or zip), write to bin_dest.
/// Returns 0 on success, 1 on failure after reporting the failing step and
/// its OS error. Cleans up tmp_archive.
fn extractAndInstallBinary(tmp_archive: Str, ext: []const u8, bin_dest: Str) c_int {
    var detail_buf: [1280]u8 = undefined;
    const f = c.fopen(tmp_archive, "rb");
    if (f == null) {
        updateFailed(STEP_EXTRACT, fmtZ(&detail_buf, "cannot open {s}", .{sp(tmp_archive)}), interop.errno());
        return CLI_TRUE;
    }
    _ = c.fseek(f, 0, c.SEEK_END);
    const fsize = c.ftell(f);
    const size_errno = interop.errno();
    _ = c.fseek(f, 0, c.SEEK_SET);

    if (fsize < 0) {
        _ = c.fclose(f);
        _ = c.cbm_unlink(tmp_archive);
        updateFailed(STEP_EXTRACT, fmtZ(&detail_buf, "cannot size {s}", .{sp(tmp_archive)}), size_errno);
        return CLI_TRUE;
    }
    const data = interop.mallocN(u8, @intCast(fsize));
    if (data == null) {
        _ = c.fclose(f);
        _ = c.cbm_unlink(tmp_archive);
        updateFailed(STEP_EXTRACT, fmtZ(&detail_buf, "out of memory reading {s} ({d} bytes)", .{ sp(tmp_archive), fsize }), 0);
        return CLI_TRUE;
    }
    interop.setErrno(0);
    const got = c.fread(data, 1, @intCast(fsize), f);
    const read_errno = interop.errno();
    _ = c.fclose(f);
    if (got != @as(usize, @intCast(fsize))) {
        c.free(data);
        _ = c.cbm_unlink(tmp_archive);
        updateFailed(STEP_EXTRACT, fmtZ(&detail_buf, "read {d} of {d} bytes of {s}", .{ got, fsize, sp(tmp_archive) }), read_errno);
        return CLI_TRUE;
    }

    var bin_len: c_int = 0;
    const archive = @import("archive.zig");
    const len: c_int = @truncate(fsize);
    const bin_data = if (eql(ext, "tar.gz"))
        archive.cbm_extract_binary_from_targz(data, len, &bin_len)
    else
        archive.cbm_extract_binary_from_zip(data, len, &bin_len);
    c.free(data);
    _ = c.cbm_unlink(tmp_archive);

    if (bin_data == null or bin_len <= 0) {
        updateFailed(STEP_EXTRACT, "the archive is damaged or does not contain the codebase-memory-mcp binary", 0);
        c.free(bin_data);
        return CLI_TRUE;
    }
    defer c.free(bin_data);

    interop.setErrno(0);
    if (cli.cbm_replace_binary(bin_dest, bin_data, bin_len, OCTAL_PERM) != 0) {
        updateFailed(STEP_REPLACE, fmtZ(&detail_buf, "cannot write {s}", .{sp(bin_dest)}), interop.errno());
        return CLI_TRUE;
    }
    return 0;
}

/// Test seam for the extract and replace steps of `update`, which the command
/// itself reaches only after stopping every running server. Not declared in
/// cli.h; the test carries an extern forward declaration.
export fn cbm_update_extract_and_install_for_test(archive_path: [*c]const u8, ext: [*c]const u8, bin_dest: [*c]const u8) c_int {
    return extractAndInstallBinary(archive_path, sp(ext), bin_dest);
}

/// Linux ships a fully-static "-portable" build; the standard linux binary
/// dynamically links glibc 2.38+ and fails on older distros. macOS/Windows
/// have no such variant. Keep in sync with install.sh / install.js / pypi
/// _cli.py.
const portable_suffix = if (builtin.os.tag == .linux or (!is_windows and !is_macos)) "-portable" else "";

/// Build the download URL for the update command.
fn buildUpdateUrl(buf: []u8, ext: []const u8) Str {
    var base_buf: [512]u8 = undefined;
    const base = c.cbm_safe_getenv("CBM_DOWNLOAD_URL", &base_buf, base_buf.len, null);
    const base_url = if (base != null and base[0] != 0) sp(base) else DEFAULT_DOWNLOAD_BASE;
    return fmtZ(buf, "{s}/codebase-memory-mcp-{s}-{s}{s}.{s}", .{ base_url, std.mem.span(detected_os), std.mem.span(detected_arch), portable_suffix, ext });
}

/// Download, verify checksum, kill old instances, and install binary.
/// Returns 0 on success.
fn downloadVerifyInstall(url: Str, ext: []const u8, bin_dest: Str) c_int {
    var tmp_buf: [256]u8 = undefined;
    const tmp_archive = fmtZ(&tmp_buf, "{s}/cbm-update.{s}", .{ sp(c.cbm_tmpdir()), ext });

    const rc = downloadToFile(url, tmp_archive);
    if (rc != 0) {
        // curl -S has already printed its own reason on the line above.
        var detail_buf: [1024]u8 = undefined;
        updateFailed(STEP_DOWNLOAD, fmtZ(&detail_buf, "could not download {s} to {s} (curl exit {d})", .{ sp(url), sp(tmp_archive), rc }), 0);
        _ = c.cbm_unlink(tmp_archive);
        return CLI_TRUE;
    }

    // Must match buildUpdateUrl: linux uses the static "-portable" asset.
    var name_buf: [256]u8 = undefined;
    const archive_name = fmtZ(&name_buf, "codebase-memory-mcp-{s}-{s}{s}.{s}", .{ std.mem.span(detected_os), std.mem.span(detected_arch), portable_suffix, ext });
    // Fail closed: install only a positively-verified download. A mismatch,
    // a missing checksum entry, or an unavailable hash tool (crc != 0) all
    // abort rather than install an unverified binary.
    if (verifyDownloadChecksum(tmp_archive, archive_name) != 0) {
        _ = c.fprintf(err(), "error: refusing to install an unverified download\n");
        _ = c.cbm_unlink(tmp_archive);
        return CLI_TRUE;
    }

    const killed = killOtherInstances();
    if (killed > 0) _ = c.printf("Stopped %d running MCP server instance(s).\n", killed);

    return if (extractAndInstallBinary(tmp_archive, ext, bin_dest) != 0) CLI_TRUE else 0;
}

/// Case-insensitive prefix match.
fn prefixIcase(s: []const u8, prefix: []const u8) bool {
    return s.len >= prefix.len and std.ascii.eqlIgnoreCase(s[0..prefix.len], prefix);
}

/// Fetch latest release tag from GitHub via redirect header into buf (e.g.
/// "v0.5.7"). Null on failure.
fn fetchLatestTag(buf: []u8) ?Str {
    const fp = c.cbm_popen("curl -sfI https://github.com/octacity-org/codebase-memory-mcp/releases/latest 2>/dev/null", "r");
    if (fp == null) return null;
    defer _ = c.cbm_pclose(fp);
    var line: [512]u8 = undefined;
    while (c.fgets(&line, line.len, fp) != null) {
        const l = bufStr(&line);
        if (!prefixIcase(l, "location:")) continue;
        const slash = std.mem.lastIndexOfScalar(u8, l, '/') orelse return null;
        const tag = std.mem.trimEnd(u8, l[slash + 1 ..], "\r\n ");
        if (tag.len == 0) return null;
        return fmtZ(buf, "{s}", .{tag});
    }
    return null;
}

/// Check if current version is already latest. Returns true to skip update.
fn checkAlreadyLatest() bool {
    var dl_env = std.mem.zeroes([256]u8);
    _ = c.cbm_safe_getenv("CBM_DOWNLOAD_URL", &dl_env, dl_env.len, null);
    if (dl_env[0] != 0) return false; // testing override — always update
    var tag_buf: [512]u8 = undefined;
    const latest = fetchLatestTag(&tag_buf) orelse {
        _ = c.fprintf(err(), "warning: could not check latest version (network unavailable?). " ++
            "Proceeding with update.\n");
        return false;
    };
    const current = cli.cbm_cli_get_version();
    const cmp = cli.cbm_compare_versions(latest, current);
    if (cmp <= 0) {
        if (cmp < 0) {
            _ = c.printf("Already up to date (%s, ahead of latest %s).\n", current, latest);
        } else {
            _ = c.printf("Already up to date (%s).\n", current);
        }
        return true;
    }
    _ = c.printf("Update available: %s -> %s\n", current, latest);
    return false;
}

pub export fn cbm_cmd_update(argc: c_int, argv: [*c][*c]u8) c_int {
    parseAutoAnswer(argc, argv);
    const dry_run = hasArg(argc, argv, &.{"--dry-run"});
    const force = hasArg(argc, argv, &.{"--force"});
    const replace_config = hasArg(argc, argv, &.{"--replace-config"});

    const home = c.cbm_get_home_dir();
    if (home == null) {
        _ = c.fprintf(err(), HOME_ERR);
        return CLI_TRUE;
    }

    _ = c.printf("codebase-memory-mcp update (current: %s)\n\n", cli.cbm_cli_get_version());

    // Version check — skip download if already on latest (not in dry-run).
    if (!force and !dry_run and checkAlreadyLatest()) return 0;

    // Updating the executable never deletes the shared Zova database; schema
    // compatibility is handled by the runtime.

    const ext = if (is_windows) "zip" else "tar.gz";
    var url_buf: [512]u8 = undefined;
    const url = buildUpdateUrl(&url_buf, ext);

    _ = c.printf(if (dry_run) "\nWould download binary for %s/%s ...\n" else "\nDownloading binary for %s/%s ...\n", detected_os, detected_arch);
    _ = c.printf("  %s\n", url);

    if (dry_run) {
        _ = c.printf("\n(dry-run — skipping download, extraction, and binary replacement)\n");
        _ = c.printf("  target: %s/.local/bin/codebase-memory-mcp\n", home);
        _ = c.printf("  os/arch: %s/%s\n", detected_os, detected_arch);
        _ = c.printf("\nUpdate dry-run complete.\n");
        return 0;
    }

    // Step 4-5: Download, verify, and install binary.
    var dest_buf: [1024]u8 = undefined;
    const bin_dest = installTargetPath(home, &dest_buf);
    var dir_buf: [1024]u8 = undefined;
    const bin_dir = fmtZ(&dir_buf, "{s}/.local/bin", .{sp(home)});
    interop.setErrno(0);
    if (!c.cbm_mkdir_p(bin_dir, OCTAL_PERM) and !cli.pathExists(bin_dir)) {
        var detail_buf: [1280]u8 = undefined;
        updateFailed("creating the install directory", fmtZ(&detail_buf, "cannot create {s}", .{sp(bin_dir)}), interop.errno());
        return CLI_TRUE;
    }

    if (downloadVerifyInstall(url, ext, bin_dest) != 0) return CLI_TRUE;

    // Step 5b: macOS ad-hoc signing (required for arm64, harmless for x86_64).
    if (is_macos and macosAdhocSign(bin_dest) != 0) {
        _ = c.fprintf(err(), "warning: ad-hoc signing failed — binary may not run on macOS arm64\n");
    }

    // Step 6: Refresh all agent configs (skills, MCP entries, hooks).
    _ = c.printf("Refreshing agent configurations...\n");
    if (!installConfigsChecked(home, bin_dest, true, false, replace_config)) {
        updateFailed("refreshing agent configurations", "the binary was replaced, but the configuration errors above remain", 0);
        return CLI_TRUE;
    }

    // Step 7: Verify new version (exec directly, no shell interpretation).
    _ = c.printf("\nUpdate complete. Verifying:\n");
    _ = exec(&.{ bin_dest, "--version" });

    _ = c.printf("\nAll project indexes were cleared. They will be rebuilt\n");
    _ = c.printf("automatically when you next use the MCP server.\n");
    _ = c.printf("\nPlease restart your MCP client to use the new binary.\n");
    return 0;
}
