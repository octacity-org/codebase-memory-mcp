//! cli.zig — CLI subcommand helpers for install, uninstall, update, version.
//!
//! Implements the configuration-file half of src/cli/cli.h: version
//! comparison, shell RC and CLI detection, file utilities, skill files, agent
//! MCP configs (JSON and TOML), instruction files, hooks and PATH setup. The
//! subcommands live in commands.zig, archive extraction in archive.zig, the
//! config store and index commands in store.zig, and tool flags in
//! tool_args.zig. All functions accept explicit paths for testability.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const is_windows = builtin.os.tag == .windows;
const is_macos = builtin.os.tag.isDarwin();

pub const Str = [*:0]const u8;
pub const CStr = [*c]const u8;
pub const sp = interop.sp;

pub const CLI_ERR: c_int = -1;
pub const CLI_TRUE: c_int = 1;

/// Directory permissions: rwxr-x---
pub const DIR_PERMS = 0o750;
pub const EXEC_PERMS = 0o755;
/// Size cap for config files read into memory.
pub const FILE_CAP: usize = 10 * 1024 * 1024;

// ── Small helpers shared by the cli files ──────────────────────────

/// snprintf-style formatting into buf; returns buf as a C string.
pub fn fmtZ(buf: []u8, comptime fmt: []const u8, args: anytype) [*:0]u8 {
    _ = interop.bprint(buf, fmt, args);
    return @ptrCast(buf.ptr);
}

/// The NUL-terminated contents of buf.
pub fn bufStr(buf: []const u8) []const u8 {
    return std.mem.sliceTo(buf, 0);
}

pub fn eql(a: []const u8, b: []const u8) bool {
    return std.mem.eql(u8, a, b);
}

/// C strcmp(a, b) == 0 for possibly-NULL a.
pub fn cEql(a: CStr, b: []const u8) bool {
    return a != null and eql(std.mem.span(a), b);
}

pub fn statPath(path: CStr, st: *c.struct_stat) bool {
    return c.stat(path, st) == 0;
}

pub fn isDirMode(st: *const c.struct_stat) bool {
    return (st.st_mode & c.S_IFMT) == c.S_IFDIR;
}

pub fn pathExists(path: CStr) bool {
    var st: c.struct_stat = undefined;
    return statPath(path, &st);
}

pub fn dirExists(path: CStr) bool {
    var st: c.struct_stat = undefined;
    return statPath(path, &st) and isDirMode(&st);
}

pub fn mkdirp(path: CStr, mode: c_int) c_int {
    return if (c.cbm_mkdir_p(path, mode)) 0 else CLI_ERR;
}

/// Create the parent directory of path (everything before the last '/').
fn mkdirParent(path: CStr) void {
    var dir: [1024]u8 = undefined;
    _ = fmtZ(&dir, "{s}", .{sp(path)});
    const cur = bufStr(&dir);
    if (std.mem.lastIndexOfScalar(u8, cur, '/')) |slash| {
        dir[slash] = 0;
        _ = mkdirp(&dir, DIR_PERMS);
    }
}

/// Allocations the cli files free themselves (never handed to C callers).
pub const gpa = std.heap.c_allocator;

// ── Version ────────────────────────────────────────────────────────

var cli_version: [*:0]const u8 = "dev";

pub export fn cbm_cli_set_version(ver: [*c]const u8) void {
    if (ver != null) cli_version = @ptrCast(ver);
}

pub export fn cbm_cli_get_version() [*c]const u8 {
    return cli_version;
}

// ── Version comparison ─────────────────────────────────────────────

fn skipVPrefix(v: [*:0]const u8) [*:0]const u8 {
    return if (v[0] == 'v' or v[0] == 'V') v + 1 else v;
}

/// Parse semver major.minor.patch, stopping at a pre-release suffix.
fn parseSemver(v_in: [*:0]const u8) [3]c_int {
    var out = [3]c_int{ 0, 0, 0 };
    var v = skipVPrefix(v_in);
    var count: usize = 0;
    while (v[0] != 0 and count < 3) {
        if (v[0] == '-') break;
        var endptr: [*c]u8 = undefined;
        const val = c.strtol(v, &endptr, 10);
        out[count] = @truncate(val);
        count += 1;
        if (endptr[0] == '.') {
            v = @ptrCast(endptr + 1);
        } else {
            break;
        }
    }
    return out;
}

fn hasPrerelease(v: [*:0]const u8) bool {
    return std.mem.indexOfScalar(u8, std.mem.span(skipVPrefix(v)), '-') != null;
}

pub export fn cbm_compare_versions(a_c: [*c]const u8, b_c: [*c]const u8) c_int {
    const a: Str = @ptrCast(a_c);
    const b: Str = @ptrCast(b_c);
    const pa = parseSemver(a);
    const pb = parseSemver(b);
    for (pa, pb) |x, y| {
        if (x != y) return x -% y;
    }
    // Same base version — non-dev beats dev.
    const a_pre = hasPrerelease(a);
    const b_pre = hasPrerelease(b);
    if (a_pre and !b_pre) return CLI_ERR;
    if (!a_pre and b_pre) return CLI_TRUE;
    return 0;
}

// ── Shell RC detection ─────────────────────────────────────────────

var shell_rc_buf: [512]u8 = undefined;

pub export fn cbm_detect_shell_rc(home_dir: [*c]const u8) [*c]const u8 {
    if (home_dir == null or home_dir[0] == 0) return "";
    const home = std.mem.span(home_dir);

    var shell_buf: [256]u8 = undefined;
    const shell_c = c.cbm_safe_getenv("SHELL", &shell_buf, shell_buf.len, "");
    const shell = if (shell_c != null) std.mem.span(shell_c) else "";
    const has = struct {
        fn f(s: []const u8, needle: []const u8) bool {
            return std.mem.indexOf(u8, s, needle) != null;
        }
    }.f;

    if (has(shell, "/zsh")) return fmtZ(&shell_rc_buf, "{s}/.zshrc", .{home});
    if (has(shell, "/bash")) {
        // Prefer .bashrc, fall back to .bash_profile.
        const rc = fmtZ(&shell_rc_buf, "{s}/.bashrc", .{home});
        if (pathExists(rc)) return rc;
        return fmtZ(&shell_rc_buf, "{s}/.bash_profile", .{home});
    }
    if (has(shell, "/fish")) return fmtZ(&shell_rc_buf, "{s}/.config/fish/config.fish", .{home});
    return fmtZ(&shell_rc_buf, "{s}/.profile", .{home});
}

// ── CLI binary detection ───────────────────────────────────────────

const PATH_DELIM = if (is_windows) ';' else ':';

/// Whether path exists and is executable. On Windows, stat() doesn't set
/// S_IXUSR — just check existence.
fn isExecutable(path: CStr) bool {
    var st: c.struct_stat = undefined;
    if (!statPath(path, &st)) return false;
    if (is_windows) return true;
    return (st.st_mode & c.S_IXUSR) != 0;
}

/// Search PATH for an executable named `name`; the full path goes to out.
fn findInPath(name: []const u8, out: []u8) bool {
    var path_copy: [4096]u8 = undefined;
    if (c.cbm_safe_getenv("PATH", &path_copy, path_copy.len, null) == null) return false;
    var it = std.mem.tokenizeScalar(u8, bufStr(&path_copy), PATH_DELIM);
    while (it.next()) |dir| {
        if (isExecutable(fmtZ(out, "{s}/{s}", .{ dir, name }))) return true;
        if (is_windows) {
            // On Windows executables carry an extension (PATHEXT). A CLI like
            // opencode is often installed as a .cmd / .ps1 / .exe shim (e.g.
            // via mise or npm), so the bare-name probe above misses it (#221).
            for ([_][]const u8{ ".exe", ".cmd", ".bat", ".ps1" }) |ext| {
                if (isExecutable(fmtZ(out, "{s}/{s}{s}", .{ dir, name, ext }))) return true;
            }
        }
    }
    return false;
}

var find_cli_buf: [512]u8 = undefined;

pub export fn cbm_find_cli(name_c: [*c]const u8, home_dir: [*c]const u8) [*c]const u8 {
    if (name_c == null or name_c[0] == 0) return "";
    const name = std.mem.span(name_c);
    if (findInPath(name, &find_cli_buf)) return &find_cli_buf;
    if (home_dir == null or home_dir[0] == 0) return "";
    const home = std.mem.span(home_dir);

    var paths: [5][512]u8 = undefined;
    _ = fmtZ(&paths[0], "/usr/local/bin/{s}", .{name});
    _ = fmtZ(&paths[1], "{s}/.npm/bin/{s}", .{ home, name });
    _ = fmtZ(&paths[2], "{s}/.local/bin/{s}", .{ home, name });
    _ = fmtZ(&paths[3], "{s}/.cargo/bin/{s}", .{ home, name });
    if (is_macos) {
        _ = fmtZ(&paths[4], "/opt/homebrew/bin/{s}", .{name});
    } else {
        paths[4][0] = 0;
    }
    for (&paths) |*p| {
        if (p[0] != 0 and isExecutable(p)) return fmtZ(&find_cli_buf, "{s}", .{bufStr(p)});
    }
    return "";
}

// ── File utilities ─────────────────────────────────────────────────

pub export fn cbm_copy_file(src: [*c]const u8, dst: [*c]const u8) c_int {
    const in = c.fopen(src, "rb") orelse return CLI_ERR;
    const out = c.fopen(dst, "wb") orelse {
        _ = c.fclose(in);
        return CLI_ERR;
    };
    var buf: [8192]u8 = undefined;
    var err = false;
    while (c.feof(in) == 0 and c.ferror(in) == 0) {
        const n = c.fread(&buf, 1, buf.len, in);
        if (n == 0) break;
        if (c.fwrite(&buf, 1, n, out) != n) {
            err = true;
            break;
        }
    }
    if (err or c.ferror(in) != 0) {
        _ = c.fclose(in);
        _ = c.fclose(out);
        return CLI_ERR;
    }
    _ = c.fclose(in);
    return if (c.fclose(out) == 0) 0 else CLI_ERR;
}

/// Whether two paths refer to the same on-disk file. Used to avoid copying
/// the running binary onto itself during install (cbm_copy_file would
/// truncate it, since it opens the destination "wb" before reading the
/// source).
pub fn sameFile(a: CStr, b: CStr) bool {
    var sa: c.struct_stat = undefined;
    var sb: c.struct_stat = undefined;
    if (!statPath(a, &sa) or !statPath(b, &sb)) return false;
    if (is_windows) {
        // st_ino is unreliable on Windows; compare normalized path strings.
        var na: [1024]u8 = undefined;
        var nb: [1024]u8 = undefined;
        _ = c.cbm_normalize_path_sep(fmtZ(&na, "{s}", .{sp(a)}));
        _ = c.cbm_normalize_path_sep(fmtZ(&nb, "{s}", .{sp(b)}));
        return eql(bufStr(&na), bufStr(&nb));
    }
    return sa.st_dev == sb.st_dev and sa.st_ino == sb.st_ino;
}

/// Copy the running binary into the canonical install target, preserving the
/// executable bit. When src and dst are the same on-disk file the copy is
/// skipped: cbm_copy_file opens dst "wb" before reading src, so copying a
/// file onto itself would truncate it to zero. Regression surface for the
/// `install --force` binary-swap bug (#472).
pub export fn cbm_copy_binary_to_target(src: [*c]const u8, dst: [*c]const u8) c_int {
    if (sameFile(src, dst)) return 0; // already in place — nothing to copy
    if (cbm_copy_file(src, dst) != 0) return CLI_ERR;
    if (!is_windows) _ = c.chmod(dst, EXEC_PERMS);
    return 0;
}

/// Replace a binary file. Unlinks the old file first (handles read-only and
/// running binaries on Unix where unlink succeeds on open files). On all
/// platforms, the caller should tell the user to restart after update.
pub export fn cbm_replace_binary(path: [*c]const u8, data: [*c]const u8, len: c_int, mode: c_int) c_int {
    if (path == null or data == null or len <= 0) return CLI_ERR;

    // Remove the existing file. On Unix, unlink works even if the binary is
    // running (inode stays alive until the process exits). On Windows, unlink
    // fails on a running .exe — rename it aside as fallback.
    if (pathExists(path) and c.cbm_unlink(path) != 0) {
        if (!is_windows) return CLI_ERR;
        var old_path: [1024]u8 = undefined;
        _ = fmtZ(&old_path, "{s}.old", .{sp(path)});
        _ = c.cbm_unlink(&old_path);
        if (c.rename(path, &old_path) != 0) return CLI_ERR;
    }

    const f = if (is_windows) c.fopen(path, "wb") orelse return CLI_ERR else blk: {
        const fd = c.open(path, c.O_WRONLY | c.O_CREAT | c.O_TRUNC | c.O_CLOEXEC, @as(c_uint, @bitCast(mode)));
        if (fd < 0) return CLI_ERR;
        break :blk c.fdopen(fd, "wb") orelse {
            _ = c.close(fd);
            return CLI_ERR;
        };
    };
    const n: usize = @intCast(len);
    const written = c.fwrite(data, 1, n, f);
    _ = c.fclose(f);
    return if (written == n) 0 else CLI_ERR;
}

// ── Skill file content (embedded) ──────────────────────────────────
// Consolidated from 4 separate skills into 1 with progressive disclosure.
// This embedded version is the single source of truth for the CLI installer.
// Based on PR #81 by @gdilla — factual corrections applied. The tool list
// between skill_head and skill_tail is generated from the MCP registry.

const skill_head =
    "---\n" ++
    "name: codebase-memory\n" ++
    "description: \"Use the codebase knowledge graph for structural code queries. " ++
    "Triggers on: explore the codebase, understand the architecture, what functions exist, " ++
    "show me the structure, who calls this function, what does X call, trace the call chain, " ++
    "find callers of, show dependencies, impact analysis, dead code, unused functions, " ++
    "high fan-out, refactor candidates, code quality audit, graph query syntax, " ++
    "Cypher query examples, edge types, how to use search_graph.\"\n" ++
    "---\n" ++
    "\n" ++
    "# Codebase Memory — Knowledge Graph Tools\n" ++
    "\n" ++
    "Graph tools return precise structural results in ~500 tokens vs ~80K for grep.\n" ++
    "\n" ++
    "## Quick Decision Matrix\n" ++
    "\n" ++
    "| Question | Tool call |\n" ++
    "|----------|----------|\n" ++
    "| Who calls X? | `trace_path(direction=\"inbound\")` |\n" ++
    "| What does X call? | `trace_path(direction=\"outbound\")` |\n" ++
    "| Full call context | `trace_path(direction=\"both\")` |\n" ++
    "| Find by name pattern | `search_graph(name_pattern=\"...\")` |\n" ++
    "| Dead code | `search_graph(max_degree=0, exclude_entry_points=true)` |\n" ++
    "| Cross-service edges | `query_graph` with Cypher |\n" ++
    "| Impact of local changes | `detect_changes(project=...)` |\n" ++
    "| Risk-classified trace | `trace_path(risk_labels=true)` |\n" ++
    "| Text search | `search_code` or Grep |\n" ++
    "| Was this file fully parsed? | `check_index_coverage(paths=[...])` |\n" ++
    "\n" ++
    "## Projects\n" ++
    "Every indexed repository lives in one shared `cbm.zova` file; there is no per-project " ++
    "database to open. Pass the selector `list_projects` reports as `project` to every " ++
    "project-scoped tool. Colliding directory names get the shortest unique parent, e.g. " ++
    "`AnotherFolder/codebase-memory-mcp`. `search_graph(projects=[...])` searches several " ++
    "projects at once and every row names its owning project.\n" ++
    "\n" ++
    "## Exploration Workflow\n" ++
    "1. `list_projects` — check if project is indexed\n" ++
    "2. `get_graph_schema` — understand node/edge types\n" ++
    "3. `search_graph(label=\"Function\", name_pattern=\".*Pattern.*\")` — find code\n" ++
    "4. `get_code_snippet(qualified_name=\"project.path.FuncName\")` — read source\n" ++
    "\n" ++
    "## Tracing Workflow\n" ++
    "1. `search_graph(name_pattern=\".*FuncName.*\")` — discover exact name\n" ++
    "2. `trace_path(function_name=\"FuncName\", direction=\"both\", depth=3)` — trace\n" ++
    "3. `detect_changes(project=...)` — map git diff to affected symbols\n" ++
    "\n" ++
    "## Quality Analysis\n" ++
    "- Dead code: `search_graph(max_degree=0, exclude_entry_points=true)`\n" ++
    "- High fan-in/fan-out: `search_graph(min_degree=10, relationship=\"CALLS\")`, then " ++
    "`trace_path` on a hit to see which side dominates\n" ++
    "\n";

const skill_tail =
    "\n" ++
    "## Edge Types\n" ++
    "CALLS, HTTP_CALLS, ASYNC_CALLS, IMPORTS, DEFINES, DEFINES_METHOD,\n" ++
    "HANDLES, IMPLEMENTS, OVERRIDE, USAGE, CALL_REFERENCE, FILE_CHANGES_WITH,\n" ++
    "CONTAINS_FILE, CONTAINS_FOLDER, CONTAINS_PACKAGE\n" ++
    "\n" ++
    "## Cypher Examples (for query_graph)\n" ++
    "```\n" ++
    "MATCH (a)-[r:HTTP_CALLS]->(b) RETURN a.name, b.name, r.url_path, " ++
    "r.confidence LIMIT 20\n" ++
    "MATCH (f:Function) WHERE f.name =~ '.*Handler.*' RETURN f.name, f.file_path\n" ++
    "MATCH (a)-[r:CALLS]->(b) WHERE a.name = 'main' RETURN b.name\n" ++
    "```\n" ++
    "\n" ++
    "## Gotchas\n" ++
    "1. `search_graph(relationship=\"HTTP_CALLS\")` filters nodes by degree — " ++
    "use `query_graph` with Cypher to see actual edges.\n" ++
    "2. `query_graph` returns every row up to a 100k ceiling and has no offset — add " ++
    "LIMIT or `max_rows`, or page with `search_graph`.\n" ++
    "3. `trace_path` needs exact names — use `search_graph(name_pattern=...)` first.\n" ++
    "4. `direction=\"outbound\"` misses cross-service callers — use " ++
    "`direction=\"both\"`.\n" ++
    "5. `search_graph` returns 200 results per page by default — check `has_more` and use " ++
    "`offset`.\n" ++
    "6. A file the parser skipped or only partly understood can be missing from graph " ++
    "results — call `check_index_coverage` before negative or exhaustive claims.\n" ++
    "7. Discovery tools accept `format=\"compact\"` for a smaller text rendering of the same " ++
    "data.\n";

/// Old skill names — cleaned up during install to remove stale directories.
const old_skill_names = [_][]const u8{
    "codebase-memory-exploring",
    "codebase-memory-tracing",
    "codebase-memory-quality",
    "codebase-memory-reference",
};

/// snprintf-at-offset buffer: like the C `off += snprintf(buf + off, ...)`
/// idiom, `off` keeps counting past the end so the caller sees the length
/// the whole output would have had.
pub const OffsetBuf = struct {
    buf: []u8,
    off: usize = 0,

    pub fn print(self: *OffsetBuf, comptime fmt: []const u8, args: anytype) void {
        const rest = if (self.off < self.buf.len) self.buf[self.off..] else self.buf[0..0];
        self.off += interop.bprint(rest, fmt, args);
    }
};

/// Append the registry's tool names, comma-separated, wrapped at width
/// columns with each line starting with indent.
fn appendToolNames(out: *OffsetBuf, indent: []const u8, width: usize, backticks: bool) void {
    const count = c.cbm_mcp_tool_count();
    var line: usize = 0;
    var i: c_int = 0;
    while (i < count) : (i += 1) {
        var item: [256]u8 = undefined;
        const comma: []const u8 = if (i + 1 < count) "," else "";
        const name = sp(c.cbm_mcp_tool_name(i));
        if (backticks) {
            _ = fmtZ(&item, "`{s}`{s}", .{ name, comma });
        } else {
            _ = fmtZ(&item, "{s}{s}", .{ name, comma });
        }
        const len = bufStr(&item).len;
        if (line > 0 and line + 1 + len > width) {
            out.print("\n", .{});
            line = 0;
        }
        out.print("{s}{s}", .{ if (line == 0) indent else " ", bufStr(&item) });
        line += (if (line == 0) indent.len else 1) + len;
    }
    out.print("\n", .{});
}

const SKILL_TOOLS_SZ = 2048;

var skill_content: [skill_head.len + 1 + SKILL_TOOLS_SZ + skill_tail.len + 1]u8 = undefined;
var skills: [c.CBM_SKILL_COUNT]c.cbm_skill_t = std.mem.zeroes([c.CBM_SKILL_COUNT]c.cbm_skill_t);

pub export fn cbm_get_skills() [*c]const c.cbm_skill_t {
    // Built once from the MCP registry so the installed tool list cannot
    // drift from tools/list.
    if (skills[0].content == null) {
        var out: OffsetBuf = .{ .buf = &skill_content };
        out.print("{s}## {d} MCP Tools\n", .{ skill_head, c.cbm_mcp_tool_count() });
        appendToolNames(&out, "", 80, true);
        out.print("{s}", .{skill_tail});
        skills[0] = .{ .name = "codebase-memory", .content = &skill_content };
    }
    return &skills;
}

pub export fn cbm_cli_tools_help() [*c]u8 {
    var buf: [SKILL_TOOLS_SZ]u8 = undefined;
    var out: OffsetBuf = .{ .buf = &buf };
    out.print("Tools:\n", .{});
    appendToolNames(&out, "  ", 72, false);
    return interop.heapStrdup(&buf);
}

pub export fn cbm_get_codex_instructions() [*c]const u8 {
    return cbm_get_agent_instructions();
}

// ── Recursive rmdir ────────────────────────────────────────────────

const RMDIR_STACK_CAP = 256;
const PathBuf = [1024]u8;

/// Scan one directory: push subdirs onto stack, unlink files.
fn rmdirScanDir(cur: []const u8, stack: *[RMDIR_STACK_CAP]PathBuf, top: *usize) void {
    var cur_z: PathBuf = undefined;
    const d = c.cbm_opendir(fmtZ(&cur_z, "{s}", .{cur})) orelse return;
    while (c.cbm_readdir(d)) |ent| {
        var child: PathBuf = undefined;
        _ = fmtZ(&child, "{s}/{s}", .{ cur, bufStr(&ent.*.name) });
        if (dirExists(&child)) {
            if (top.* < RMDIR_STACK_CAP) {
                _ = fmtZ(&stack[top.*], "{s}", .{bufStr(&child)});
                top.* += 1;
            }
        } else {
            _ = c.cbm_unlink(&child);
        }
    }
    c.cbm_closedir(d);
}

pub fn rmdirRecursive(path: CStr) c_int {
    const Bufs = struct { stack: [RMDIR_STACK_CAP]PathBuf, dirs: [RMDIR_STACK_CAP]PathBuf };
    const bufs = gpa.create(Bufs) catch return c.CBM_NOT_FOUND;
    defer gpa.destroy(bufs);
    var top: usize = 0;
    _ = fmtZ(&bufs.stack[top], "{s}", .{sp(path)});
    top += 1;

    // Post-order: collect all dirs depth-first, then rmdir in reverse.
    var dir_count: usize = 0;
    while (top > 0) {
        top -= 1;
        var cur: PathBuf = bufs.stack[top];
        if (dir_count < RMDIR_STACK_CAP) {
            bufs.dirs[dir_count] = cur;
            dir_count += 1;
        }
        rmdirScanDir(bufStr(&cur), &bufs.stack, &top);
    }
    // Remove dirs in reverse (deepest first).
    var rc: c_int = 0;
    var i = dir_count;
    while (i > 0) {
        i -= 1;
        if (c.cbm_rmdir(&bufs.dirs[i]) != 0) rc = c.CBM_NOT_FOUND;
    }
    return rc;
}

// ── Skill management ───────────────────────────────────────────────

pub export fn cbm_install_skills(skills_dir: [*c]const u8, force: bool, dry_run: bool) c_int {
    if (skills_dir == null) return 0;
    const dir = std.mem.span(skills_dir);
    var count: c_int = 0;

    // Clean up old 4-skill directories (consolidated into 1).
    for (old_skill_names) |old| {
        var old_path: PathBuf = undefined;
        if (dirExists(fmtZ(&old_path, "{s}/{s}", .{ dir, old })) and !dry_run) {
            _ = rmdirRecursive(&old_path);
        }
    }

    const list = cbm_get_skills();
    for (0..c.CBM_SKILL_COUNT) |i| {
        var skill_path: PathBuf = undefined;
        _ = fmtZ(&skill_path, "{s}/{s}", .{ dir, sp(list[i].name) });
        var file_path: PathBuf = undefined;
        _ = fmtZ(&file_path, "{s}/SKILL.md", .{bufStr(&skill_path)});

        if (!force and pathExists(&file_path)) continue;
        if (dry_run) {
            count += 1;
            continue;
        }
        if (mkdirp(&skill_path, DIR_PERMS) != 0) continue;
        const f = c.fopen(&file_path, "w") orelse continue;
        const content = list[i].content;
        _ = c.fwrite(content, 1, c.strlen(content), f);
        _ = c.fclose(f);
        count += 1;
    }
    return count;
}

pub export fn cbm_remove_skills(skills_dir: [*c]const u8, dry_run: bool) c_int {
    if (skills_dir == null) return 0;
    var count: c_int = 0;
    const list = cbm_get_skills();
    for (0..c.CBM_SKILL_COUNT) |i| {
        var skill_path: PathBuf = undefined;
        if (!pathExists(fmtZ(&skill_path, "{s}/{s}", .{ sp(skills_dir), sp(list[i].name) }))) continue;
        if (dry_run) {
            count += 1;
            continue;
        }
        if (rmdirRecursive(&skill_path) == 0) count += 1;
    }
    return count;
}

pub export fn cbm_remove_old_monolithic_skill(skills_dir: [*c]const u8, dry_run: bool) bool {
    if (skills_dir == null) return false;
    var old_path: PathBuf = undefined;
    if (!dirExists(fmtZ(&old_path, "{s}/codebase-memory-mcp", .{sp(skills_dir)}))) return false;
    if (dry_run) return true;
    return rmdirRecursive(&old_path) == 0;
}

// ── File reading and writing ───────────────────────────────────────

/// Read up to FILE_CAP bytes of path into a malloc'd, NUL-terminated buffer.
/// `allow_empty` false refuses an empty file the way the JSON reader does.
fn readFileRaw(path: CStr, allow_empty: bool, out_len: *usize) ?[*:0]u8 {
    const f = c.fopen(path, "r") orelse return null;
    _ = c.fseek(f, 0, c.SEEK_END);
    const size = c.ftell(f);
    _ = c.fseek(f, 0, c.SEEK_SET);
    if (size < 0 or (!allow_empty and size == 0) or @as(usize, @intCast(size)) > FILE_CAP) {
        _ = c.fclose(f);
        return null;
    }
    const n: usize = @intCast(size);
    const buf = interop.mallocN(u8, n + 1) orelse {
        _ = c.fclose(f);
        return null;
    };
    const nread = @min(c.fread(buf, 1, n, f), n);
    _ = c.fclose(f);
    buf[nread] = 0;
    out_len.* = nread;
    return @ptrCast(buf);
}

/// Read a whole text file into a malloc'd buffer. Returns null on error.
pub fn readFileStr(path: CStr, out_len: ?*usize) ?[*:0]u8 {
    var len: usize = 0;
    const buf = readFileRaw(path, true, &len);
    if (out_len) |p| p.* = if (buf != null) len else 0;
    return buf;
}

/// Write string to file, creating parent dirs if needed.
pub fn writeFileStr(path: CStr, content: []const u8) c_int {
    mkdirParent(path);
    const f = c.fopen(path, "w") orelse return CLI_ERR;
    const written = c.fwrite(content.ptr, 1, content.len, f);
    _ = c.fclose(f);
    return if (written == content.len) 0 else CLI_ERR;
}

// ── JSON config helpers (using yyjson) ─────────────────────────────

const MutDoc = c.yyjson_mut_doc;
const MutVal = c.yyjson_mut_val;

/// Read a JSON (or JSONC) file into a yyjson document. Returns null on error.
pub fn readJsonFile(path: CStr) ?*c.yyjson_doc {
    var len: usize = 0;
    const buf = readFileRaw(path, false, &len) orelse return null;
    defer c.free(buf);
    // Allow JSONC (comments + trailing commas) — Zed settings.json uses this format.
    const flags = c.YYJSON_READ_ALLOW_COMMENTS | c.YYJSON_READ_ALLOW_TRAILING_COMMAS;
    return c.yyjson_read(buf, len, flags);
}

/// Write a mutable yyjson document to a file with pretty printing.
fn writeJsonFile(path: CStr, doc: *MutDoc) c_int {
    mkdirParent(path);
    var len: usize = 0;
    const json = c.yyjson_mut_write(doc, c.YYJSON_WRITE_PRETTY | c.YYJSON_WRITE_ESCAPE_UNICODE, &len) orelse return CLI_ERR;
    defer c.free(json);
    const f = c.fopen(path, "w") orelse return CLI_ERR;
    const written = c.fwrite(json, 1, len, f);
    _ = c.fputc('\n', f);
    _ = c.fclose(f);
    return if (written == len) 0 else CLI_ERR;
}

/// Load an agent config for an in-place edit. A missing or empty file starts
/// a fresh object; a file that exists but does not parse is refused (null),
/// so a config the user is mid-way through editing is never replaced
/// wholesale.
fn loadJsonConfig(mdoc: *MutDoc, config_path: CStr) ?*MutVal {
    if (readJsonFile(config_path)) |doc| {
        const root = c.yyjson_val_mut_copy(mdoc, c.yyjson_doc_get_root(doc));
        c.yyjson_doc_free(doc);
        return if (c.yyjson_mut_is_obj(root)) root else null;
    }
    var st: c.struct_stat = undefined;
    if (statPath(config_path, &st) and st.st_size > 0) {
        _ = c.fprintf(interop.stderr(), "  skipped: %s exists but is not valid JSON; fix it and re-run install\n", config_path);
        return null;
    }
    return c.yyjson_mut_obj(mdoc);
}

/// The existing document of config_path as a mutable copy, for a removal.
fn loadJsonForRemoval(config_path: CStr) ?*MutDoc {
    const doc = readJsonFile(config_path) orelse return null;
    const mdoc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_val_mut_copy(mdoc, c.yyjson_doc_get_root(doc));
    c.yyjson_doc_free(doc);
    if (root == null) {
        c.yyjson_mut_doc_free(mdoc);
        return null;
    }
    c.yyjson_mut_doc_set_root(mdoc, root);
    return mdoc;
}

fn objGet(obj: ?*MutVal, key: Str) ?*MutVal {
    return c.yyjson_mut_obj_get(obj, key);
}

fn isObj(v: ?*MutVal) bool {
    return v != null and c.yyjson_mut_is_obj(v);
}

fn isArr(v: ?*MutVal) bool {
    return v != null and c.yyjson_mut_is_arr(v);
}

/// The object at key in parent, created (and added) when missing or not an
/// object.
fn getOrAddObj(mdoc: *MutDoc, parent: *MutVal, key: Str) *MutVal {
    if (objGet(parent, key)) |v| {
        if (c.yyjson_mut_is_obj(v)) return v;
    }
    const v = c.yyjson_mut_obj(mdoc);
    _ = c.yyjson_mut_obj_add_val(mdoc, parent, key, v);
    return v;
}

/// Carry keys the client or user added to our previous entry (for example
/// "enabled", "env", "timeout") into the replacement, so reinstalling only
/// refreshes the fields we own.
fn keepClientKeys(mdoc: *MutDoc, entry: *MutVal, previous: ?*MutVal) void {
    if (!isObj(previous)) return;
    var iter = c.yyjson_mut_obj_iter_with(previous);
    while (c.yyjson_mut_obj_iter_next(&iter)) |key| {
        const name = c.yyjson_mut_get_str(key);
        if (name != null and objGet(entry, name) == null) {
            _ = c.yyjson_mut_obj_add_val(mdoc, entry, name, c.yyjson_mut_val_mut_copy(mdoc, c.yyjson_mut_obj_iter_get_val(key)));
        }
    }
}

const SERVER_KEY = "codebase-memory-mcp";

/// The MCP server entry shapes agents expect.
const EntryKind = enum {
    /// Cursor/Windsurf/Gemini/Antigravity/Junie: {"command": path}
    command,
    /// OpenClaw: {"enabled": true, "command": path, "args": []}
    openclaw,
    /// VS Code: {"type": "stdio", "command": path}
    vscode,
    /// OpenCode: {"enabled": true, "type": "local", "command": [path]}
    opencode,
};

/// Upsert our MCP server entry under the object path `parents` (each level
/// created when missing). Keys the client added to a previous entry are kept.
fn installJsonEntry(binary_path: CStr, config_path: CStr, parents: []const Str, kind: EntryKind) c_int {
    if (binary_path == null or config_path == null) return CLI_ERR;
    const mdoc = c.yyjson_mut_doc_new(null) orelse return CLI_ERR;
    defer c.yyjson_mut_doc_free(mdoc);
    const root = loadJsonConfig(mdoc, config_path) orelse return CLI_ERR;
    c.yyjson_mut_doc_set_root(mdoc, root);

    var servers = root;
    for (parents) |key| servers = getOrAddObj(mdoc, servers, key);

    const previous = c.yyjson_mut_obj_remove_key(servers, SERVER_KEY);
    const entry = c.yyjson_mut_obj(mdoc);
    switch (kind) {
        .command => _ = c.yyjson_mut_obj_add_str(mdoc, entry, "command", binary_path),
        .openclaw => {
            _ = c.yyjson_mut_obj_add_bool(mdoc, entry, "enabled", true);
            _ = c.yyjson_mut_obj_add_str(mdoc, entry, "command", binary_path);
            _ = c.yyjson_mut_obj_add_val(mdoc, entry, "args", c.yyjson_mut_arr(mdoc));
        },
        .vscode => {
            _ = c.yyjson_mut_obj_add_str(mdoc, entry, "type", "stdio");
            _ = c.yyjson_mut_obj_add_str(mdoc, entry, "command", binary_path);
        },
        .opencode => {
            _ = c.yyjson_mut_obj_add_bool(mdoc, entry, "enabled", true);
            _ = c.yyjson_mut_obj_add_str(mdoc, entry, "type", "local");
            const cmd_arr = c.yyjson_mut_arr(mdoc);
            _ = c.yyjson_mut_arr_add_str(mdoc, cmd_arr, binary_path);
            _ = c.yyjson_mut_obj_add_val(mdoc, entry, "command", cmd_arr);
        },
    }
    keepClientKeys(mdoc, entry, previous);
    _ = c.yyjson_mut_obj_add_val(mdoc, servers, SERVER_KEY, entry);
    return writeJsonFile(config_path, mdoc);
}

/// Remove our MCP server entry under the object path `parents`. A missing
/// path is not an error (0); an unreadable file is.
fn removeJsonEntry(config_path: CStr, parents: []const Str) c_int {
    if (config_path == null) return CLI_ERR;
    const mdoc = loadJsonForRemoval(config_path) orelse return CLI_ERR;
    defer c.yyjson_mut_doc_free(mdoc);
    var servers = c.yyjson_mut_doc_get_root(mdoc);
    for (parents) |key| {
        servers = objGet(servers, key);
        if (!isObj(servers)) return 0;
    }
    _ = c.yyjson_mut_obj_remove_key(servers, SERVER_KEY);
    return writeJsonFile(config_path, mdoc);
}

// ── Editor MCP configs ─────────────────────────────────────────────

pub export fn cbm_install_editor_mcp(binary_path: [*c]const u8, config_path: [*c]const u8) c_int {
    return installJsonEntry(binary_path, config_path, &.{"mcpServers"}, .command);
}

pub export fn cbm_remove_editor_mcp(config_path: [*c]const u8) c_int {
    return removeJsonEntry(config_path, &.{"mcpServers"});
}

pub export fn cbm_install_openclaw_mcp(binary_path: [*c]const u8, config_path: [*c]const u8) c_int {
    return installJsonEntry(binary_path, config_path, &.{ "mcp", "servers" }, .openclaw);
}

pub export fn cbm_remove_openclaw_mcp(config_path: [*c]const u8) c_int {
    return removeJsonEntry(config_path, &.{ "mcp", "servers" });
}

pub export fn cbm_install_vscode_mcp(binary_path: [*c]const u8, config_path: [*c]const u8) c_int {
    return installJsonEntry(binary_path, config_path, &.{"servers"}, .vscode);
}

pub export fn cbm_remove_vscode_mcp(config_path: [*c]const u8) c_int {
    return removeJsonEntry(config_path, &.{"servers"});
}

pub export fn cbm_install_zed_mcp(binary_path: [*c]const u8, config_path: [*c]const u8) c_int {
    return installJsonEntry(binary_path, config_path, &.{"context_servers"}, .command);
}

pub export fn cbm_remove_zed_mcp(config_path: [*c]const u8) c_int {
    return removeJsonEntry(config_path, &.{"context_servers"});
}

pub export fn cbm_upsert_opencode_mcp(binary_path: [*c]const u8, config_path: [*c]const u8) c_int {
    return installJsonEntry(binary_path, config_path, &.{"mcp"}, .opencode);
}

pub export fn cbm_remove_opencode_mcp(config_path: [*c]const u8) c_int {
    return removeJsonEntry(config_path, &.{"mcp"});
}

/// Antigravity uses the same mcpServers format as Cursor/Gemini.
pub export fn cbm_upsert_antigravity_mcp(binary_path: [*c]const u8, config_path: [*c]const u8) c_int {
    return cbm_install_editor_mcp(binary_path, config_path);
}

pub export fn cbm_remove_antigravity_mcp(config_path: [*c]const u8) c_int {
    return cbm_remove_editor_mcp(config_path);
}

/// Junie (JetBrains) uses the same mcpServers format as Cursor/Antigravity.
pub export fn cbm_upsert_junie_mcp(binary_path: [*c]const u8, config_path: [*c]const u8) c_int {
    return cbm_install_editor_mcp(binary_path, config_path);
}

pub export fn cbm_remove_junie_mcp(config_path: [*c]const u8) c_int {
    return cbm_remove_editor_mcp(config_path);
}

/// OpenCode reads opencode.json or opencode.jsonc. Target whichever exists,
/// .jsonc first, so a .jsonc user does not get a second, ignored .json file;
/// with neither present, create .json (upstream 1e6825f9).
pub fn opencodeConfigPath(home: []const u8, out: []u8) [*:0]u8 {
    const p = fmtZ(out, "{s}/.config/opencode/opencode.jsonc", .{home});
    if (c.cbm_file_exists(p)) return p;
    return fmtZ(out, "{s}/.config/opencode/opencode.json", .{home});
}

// ── Agent detection ────────────────────────────────────────────────

/// The value of $CLAUDE_CONFIG_DIR, or null when unset or empty.
fn claudeConfigEnv(buf: []u8) ?[]const u8 {
    const env = c.cbm_safe_getenv("CLAUDE_CONFIG_DIR", buf.ptr, buf.len, null);
    if (env == null or env[0] == 0) return null;
    return std.mem.span(env);
}

/// The Claude Code config dir: $CLAUDE_CONFIG_DIR, else "$home_dir/.claude".
/// Empty when neither is known.
pub fn claudeConfigDir(home_dir: CStr, out: []u8) [*:0]u8 {
    var env_buf: [1024]u8 = undefined;
    if (claudeConfigEnv(&env_buf)) |env| return fmtZ(out, "{s}", .{env});
    if (home_dir != null and home_dir[0] != 0) return fmtZ(out, "{s}/.claude", .{sp(home_dir)});
    out[0] = 0;
    return @ptrCast(out.ptr);
}

/// The parent dir of `.claude.json` (Claude Code's user config file):
/// $CLAUDE_CONFIG_DIR, else home_dir.
pub fn claudeUserRoot(home_dir: CStr, out: []u8) [*:0]u8 {
    var env_buf: [1024]u8 = undefined;
    if (claudeConfigEnv(&env_buf)) |env| return fmtZ(out, "{s}", .{env});
    if (home_dir != null and home_dir[0] != 0) return fmtZ(out, "{s}", .{sp(home_dir)});
    out[0] = 0;
    return @ptrCast(out.ptr);
}

/// The hook command string written into Claude Code's settings.json. Honors
/// $CLAUDE_CONFIG_DIR; when it is unset, keeps the tilde form so
/// settings.json stays portable across HOME values.
fn resolveHookCommand(script_name: []const u8, out: []u8) [*:0]u8 {
    var env_buf: [1024]u8 = undefined;
    if (claudeConfigEnv(&env_buf)) |env| return fmtZ(out, "{s}/hooks/{s}", .{ env, script_name });
    return fmtZ(out, "~/.claude/hooks/{s}", .{script_name});
}

/// Zed's config dir under home.
pub const ZED_CONFIG_SUBDIR = if (is_windows) "AppData/Local/Zed" else ".config/zed";
/// VS Code's User config dir under home.
pub const VSCODE_USER_SUBDIR = if (is_macos)
    "Library/Application Support/Code/User"
else if (is_windows)
    "AppData/Roaming/Code/User"
else
    ".config/Code/User";

pub export fn cbm_detect_agents(home_dir: [*c]const u8) c.cbm_detected_agents_t {
    var agents = std.mem.zeroes(c.cbm_detected_agents_t);
    if (home_dir == null or home_dir[0] == 0) return agents;
    const home = std.mem.span(home_dir);
    var path: PathBuf = undefined;

    const claude_dir = claudeConfigDir(home_dir, &path);
    agents.claude_code = claude_dir[0] != 0 and dirExists(claude_dir);
    agents.codex = dirExists(fmtZ(&path, "{s}/.codex", .{home}));
    agents.gemini = dirExists(fmtZ(&path, "{s}/.gemini", .{home}));
    agents.zed = dirExists(fmtZ(&path, "{s}/" ++ ZED_CONFIG_SUBDIR, .{home}));
    agents.opencode = cbm_find_cli("opencode", home_dir)[0] != 0;
    // Antigravity CLI (2026 unification) installs under
    // ~/.gemini/antigravity-cli/ (brain/, mcp/, settings.json), with MCP
    // config in the shared ~/.gemini/config/mcp_config.json.
    if (dirExists(fmtZ(&path, "{s}/.gemini/antigravity-cli", .{home}))) agents.antigravity = true;
    agents.aider = cbm_find_cli("aider", home_dir)[0] != 0;
    agents.kilocode = dirExists(fmtZ(&path, "{s}/" ++ VSCODE_USER_SUBDIR ++ "/globalStorage/kilocode.kilo-code", .{home}));
    agents.vscode = dirExists(fmtZ(&path, "{s}/" ++ VSCODE_USER_SUBDIR, .{home}));
    // Cursor stores its user MCP config in ~/.cursor/mcp.json on all platforms.
    agents.cursor = dirExists(fmtZ(&path, "{s}/.cursor", .{home}));
    agents.openclaw = dirExists(fmtZ(&path, "{s}/.openclaw", .{home}));
    agents.kiro = dirExists(fmtZ(&path, "{s}/.kiro", .{home}));
    agents.junie = dirExists(fmtZ(&path, "{s}/.junie", .{home}));
    return agents;
}

// ── Shared agent instructions content ──────────────────────────────

const agent_instructions_content =
    "# Codebase Knowledge Graph (codebase-memory-mcp)\n" ++
    "\n" ++
    "This project uses codebase-memory-mcp to maintain a knowledge graph of the codebase.\n" ++
    "ALWAYS prefer MCP graph tools over grep/glob/file-search for code discovery.\n" ++
    "Run `list_projects` to discover indexed project selectors. If the target repository is not " ++
    "indexed, run `index_repository` first.\n" ++
    "All indexed projects live in one shared `cbm.zova` file; there is no per-project database " ++
    "to open. Pass a selector from `list_projects` as `project` to project-scoped tools.\n" ++
    "\n" ++
    "## Priority Order\n" ++
    "1. `search_graph` — find functions, classes, routes, variables by pattern\n" ++
    "2. `trace_path` — trace who calls a function or what it calls\n" ++
    "3. `get_code_snippet` — read specific function/class source code\n" ++
    "4. `query_graph` — run Cypher queries for complex patterns\n" ++
    "5. `get_architecture` — high-level project summary\n" ++
    "\n" ++
    "## When to fall back to grep/glob\n" ++
    "- Searching for string literals, error messages, config values\n" ++
    "- Searching non-code files (Dockerfiles, shell scripts, configs)\n" ++
    "- When MCP tools return insufficient results\n" ++
    "- When `check_index_coverage` reports a file as partial or skipped; check coverage before " ++
    "negative or exhaustive claims\n" ++
    "\n" ++
    "## Cross-repository search\n" ++
    "- Use `search_graph(projects=[\"project-a\",\"project-b\"], query=\"...\")` to search " ++
    "selected repositories in one request.\n" ++
    "- Use `search_graph(projects=[\"*\"], query=\"...\")` to search every ready, healthy " ++
    "workspace.\n" ++
    "- Use selectors exactly as reported by `list_projects`. Names normally use the repository " ++
    "directory, while collisions include the shortest unique parent, for example " ++
    "`AnotherFolder/codebase-memory-mcp`.\n" ++
    "- Every cross-repository result includes its owning project. Pass the returned `project` " ++
    "and `qualified_name` to `get_code_snippet`.\n" ++
    "- `query_graph` and `trace_path` remain single-project operations.\n" ++
    "\n" ++
    "## Examples\n" ++
    "- Find a handler: `search_graph(name_pattern=\".*OrderHandler.*\")`\n" ++
    "- Who calls it: `trace_path(function_name=\"OrderHandler\", direction=\"inbound\")`\n" ++
    "- Read source: `get_code_snippet(qualified_name=\"pkg/orders.OrderHandler\")`\n";

pub export fn cbm_get_agent_instructions() [*c]const u8 {
    return agent_instructions_content;
}

// ── Instructions file upsert ───────────────────────────────────────

const CMM_MARKER_START = "<!-- codebase-memory-mcp:start -->";
const CMM_MARKER_END = "<!-- codebase-memory-mcp:end -->";

/// Concatenate parts into a temporary buffer and write it to path.
fn writeParts(path: CStr, parts: []const []const u8) c_int {
    const joined = std.mem.concat(gpa, u8, parts) catch return CLI_ERR;
    defer gpa.free(joined);
    return writeFileStr(path, joined);
}

/// The span of the first START..END marker block in text: start index and the
/// index just past END (plus one trailing newline).
fn findMarkedBlock(text: []const u8, start_marker: []const u8, end_marker: []const u8) ?struct { start: usize, end: usize } {
    const start = std.mem.indexOf(u8, text, start_marker) orelse return null;
    const end_at = std.mem.indexOfPos(u8, text, start, end_marker) orelse return null;
    var end = end_at + end_marker.len;
    if (end < text.len and text[end] == '\n') end += 1;
    return .{ .start = start, .end = end };
}

pub export fn cbm_upsert_instructions(path: [*c]const u8, content: [*c]const u8) c_int {
    if (path == null or content == null) return CLI_ERR;
    const body = std.mem.span(content);
    const section = std.mem.concat(gpa, u8, &.{ CMM_MARKER_START, "\n", body, CMM_MARKER_END, "\n" }) catch return CLI_ERR;
    defer gpa.free(section);

    var existing_len: usize = 0;
    // File doesn't exist — create with just the section.
    const existing_buf = readFileStr(path, &existing_len) orelse return writeFileStr(path, section);
    defer c.free(existing_buf);
    const existing = std.mem.span(existing_buf);

    if (findMarkedBlock(existing, CMM_MARKER_START, CMM_MARKER_END)) |blk| {
        // Replace between markers (including the markers themselves).
        return writeParts(path, &.{ existing[0..blk.start], section, existing[blk.end..] });
    }
    // Append section.
    if (existing_len + 1 + section.len > FILE_CAP) return CLI_ERR; // 10 MB safety cap
    return writeParts(path, &.{ existing_buf[0..existing_len], "\n", section });
}

pub export fn cbm_remove_instructions(path: [*c]const u8) c_int {
    if (path == null) return CLI_ERR;
    const content_buf = readFileStr(path, null) orelse return CLI_TRUE;
    defer c.free(content_buf);
    const content = std.mem.span(content_buf);
    const blk = findMarkedBlock(content, CMM_MARKER_START, CMM_MARKER_END) orelse return CLI_TRUE; // not found
    // Also remove a leading newline before the start marker if present.
    var start = blk.start;
    if (start > 0 and content[start - 1] == '\n') start -= 1;
    return writeParts(path, &.{ content[0..start], content[blk.end..] });
}

// ── Codex MCP config (TOML) ────────────────────────────────────────

const CODEX_CMM_SECTION = "[mcp_servers.codebase-memory-mcp]";

fn isCodexMcpHeaderLine(line: []const u8) bool {
    var p: usize = 0;
    while (p < line.len and (line[p] == ' ' or line[p] == '\t')) : (p += 1) {}
    if (p >= line.len or line[p] != '[') return false;
    p += 1;
    if (p < line.len and line[p] == '[') return false; // not [[array of tables]]

    // First key component: "mcp_servers"
    while (p < line.len and (line[p] == ' ' or line[p] == '\t')) : (p += 1) {}
    if (p >= line.len) return false;

    if (line[p] == '"' or line[p] == '\'') {
        const quote = line[p];
        p += 1;
        const key_start = p;
        while (p < line.len and line[p] != quote) : (p += 1) {}
        if (p >= line.len) return false;
        if (!eql(line[key_start..p], "mcp_servers")) return false;
        p += 1;
    } else {
        const key_start = p;
        while (p < line.len and line[p] != '.' and line[p] != ' ' and line[p] != '\t' and line[p] != ']') : (p += 1) {}
        if (!eql(line[key_start..p], "mcp_servers")) return false;
    }

    // Dot separator
    while (p < line.len and (line[p] == ' ' or line[p] == '\t')) : (p += 1) {}
    if (p >= line.len or line[p] != '.') return false;
    p += 1;

    // Second key component: "codebase-memory-mcp"
    while (p < line.len and (line[p] == ' ' or line[p] == '\t')) : (p += 1) {}
    if (p >= line.len) return false;

    if (line[p] == '"' or line[p] == '\'') {
        const quote = line[p];
        p += 1;
        const key_start = p;
        while (p < line.len and line[p] != quote) : (p += 1) {}
        if (p >= line.len) return false;
        if (!eql(line[key_start..p], "codebase-memory-mcp")) return false;
        p += 1;
    } else {
        const key_start = p;
        while (p < line.len and line[p] != ' ' and line[p] != '\t' and line[p] != ']') : (p += 1) {}
        if (!eql(line[key_start..p], "codebase-memory-mcp")) return false;
    }

    // Closing ']'
    while (p < line.len and (line[p] == ' ' or line[p] == '\t')) : (p += 1) {}
    if (p >= line.len or line[p] != ']') return false;
    p += 1;
    if (p < line.len and line[p] == ']') return false; // not ']]'

    // Rest of line: optional whitespace or comments
    while (p < line.len) : (p += 1) {
        if (line[p] == '#') break;
        if (line[p] != ' ' and line[p] != '\t' and line[p] != '\r') return false;
    }
    return true;
}

const CodexSectionSpan = struct {
    start: usize,
    header_end: usize,
    end: usize,
};

/// The span of our Codex TOML section: from after its header to the next "[" line
/// (exclusive) or EOF. header_end is the newline position of the header line.
fn codexSectionEnd(content: []const u8, header_end: usize) usize {
    const next = std.mem.indexOfPos(u8, content, header_end, "\n[") orelse return content.len;
    return next + 1; // keep the newline before the next section
}

fn findNextCodexMcpSection(content: []const u8, from: usize) ?CodexSectionSpan {
    var line_start: usize = from;
    while (line_start < content.len) {
        const line_end = std.mem.indexOfScalarPos(u8, content, line_start, '\n') orelse content.len;
        const line = content[line_start..line_end];
        if (isCodexMcpHeaderLine(line)) {
            const header_end = line_end;
            const end = codexSectionEnd(content, header_end);
            return .{
                .start = line_start,
                .header_end = header_end,
                .end = end,
            };
        }
        if (line_end == content.len) break;
        line_start = line_end + 1;
    }
    return null;
}

pub fn hasCodexMcpSection(content: []const u8) bool {
    return findNextCodexMcpSection(content, 0) != null;
}

pub export fn cbm_upsert_codex_mcp(binary_path: [*c]const u8, config_path: [*c]const u8) c_int {
    if (binary_path == null or config_path == null) return CLI_ERR;

    // Codex passes only the names in env_vars into a stdio MCP subprocess;
    // without CBM_CACHE_DIR a Codex-spawned server would open the default
    // cache's cbm.zova instead of the configured one. Listed unconditionally:
    // env_vars forwards a name only when it is set (upstream 63f26f4c).
    var section_buf: [1024]u8 = undefined;
    const section = std.mem.span(fmtZ(&section_buf, "{s}\ncommand = \"{s}\"\nenv_vars = [\"CBM_CACHE_DIR\"]\n", .{ CODEX_CMM_SECTION, sp(binary_path) }));

    var len: usize = 0;
    // No file — create fresh.
    const content_buf = readFileStr(config_path, &len) orelse return writeFileStr(config_path, section);
    defer c.free(content_buf);
    const content = std.mem.span(content_buf);

    if (findNextCodexMcpSection(content, 0) == null) {
        return writeParts(config_path, &.{ content, "\n", section });
    }

    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    var cursor: usize = 0;
    var first = true;
    while (findNextCodexMcpSection(content, cursor)) |span| {
        if (first) {
            out.appendSlice(gpa, content[cursor..span.start]) catch return CLI_ERR;
            out.appendSlice(gpa, section) catch return CLI_ERR;
            cursor = span.end;
            if (cursor < content.len and content[cursor] == '[') {
                out.appendSlice(gpa, "\n") catch return CLI_ERR;
            }
            first = false;
        } else {
            var at = span.start;
            if (at > cursor and content[at - 1] == '\n') {
                at -= 1;
                if (at > cursor and content[at - 1] == '\r') at -= 1;
            }
            out.appendSlice(gpa, content[cursor..at]) catch return CLI_ERR;
            cursor = span.end;
        }
    }
    if (cursor < content.len) {
        out.appendSlice(gpa, content[cursor..]) catch return CLI_ERR;
    }
    return writeFileStr(config_path, out.items);
}

pub export fn cbm_remove_codex_mcp(config_path: [*c]const u8) c_int {
    if (config_path == null) return CLI_ERR;
    const content_buf = readFileStr(config_path, null) orelse return CLI_TRUE;
    defer c.free(content_buf);
    const content = std.mem.span(content_buf);
    if (findNextCodexMcpSection(content, 0) == null) return CLI_TRUE;

    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    var cursor: usize = 0;
    while (findNextCodexMcpSection(content, cursor)) |span| {
        var at = span.start;
        if (at > cursor and content[at - 1] == '\n') {
            at -= 1;
            if (at > cursor and content[at - 1] == '\r') at -= 1;
        }
        out.appendSlice(gpa, content[cursor..at]) catch return CLI_ERR;
        cursor = span.end;
    }
    if (cursor < content.len) {
        out.appendSlice(gpa, content[cursor..]) catch return CLI_ERR;
    }
    return writeFileStr(config_path, out.items);
}

// ── SessionStart reminder hook (Codex / Gemini / Antigravity) ──────
// Same methodology as the Claude Code SessionStart hook: a non-blocking
// lifecycle hook whose stdout is injected as session context, reminding the
// agent to use codebase-memory-mcp graph tools first. The command is written
// so it is valid both inside a TOML single-quoted literal (Codex config.toml)
// and a JSON string (Gemini settings.json) — i.e. it contains NO single
// quotes and NO newlines. (issues #330 + Gemini/Antigravity parity)
const CMM_SESSION_REMINDER_CMD =
    "echo \"Code discovery: prefer codebase-memory-mcp (search_graph, trace_path, " ++
    "get_code_snippet, query_graph, search_code) over grep/file-read; run " ++
    "index_repository first if the project is not indexed.\"";

// Sentinel-delimited block so upsert/remove are robust to the nested TOML
// array-of-tables (which both start with '[').
const CODEX_HOOK_BEGIN = "# >>> codebase-memory-mcp SessionStart >>>";
const CODEX_HOOK_END = "# <<< codebase-memory-mcp SessionStart <<<";

/// Strip every managed Codex SessionStart block and any stray marker lines.
/// Returns the remaining text (caller frees with gpa), or null when unchanged.
fn codexHookStrip(content: []const u8) ?[]u8 {
    var out: std.ArrayList(u8) = .empty;
    var changed = false;
    var in_block = false;
    var rest = content;
    while (rest.len > 0) {
        const line_len = if (std.mem.indexOfScalar(u8, rest, '\n')) |nl| nl + 1 else rest.len;
        const line = rest[0..line_len];
        rest = rest[line_len..];
        if (std.mem.indexOf(u8, line, CODEX_HOOK_BEGIN) != null) {
            in_block = true;
            changed = true;
        } else if (std.mem.indexOf(u8, line, CODEX_HOOK_END) != null) {
            in_block = false;
            changed = true;
        } else if (in_block) {
            changed = true;
        } else {
            out.appendSlice(gpa, line) catch {
                out.deinit(gpa);
                return null;
            };
        }
    }
    if (!changed) {
        out.deinit(gpa);
        return null;
    }
    return out.toOwnedSlice(gpa) catch null;
}

/// Install/update the Codex SessionStart reminder hook in config.toml.
pub export fn cbm_upsert_codex_hooks(config_path: [*c]const u8) c_int {
    if (config_path == null) return CLI_ERR;
    const block = "\n" ++ CODEX_HOOK_BEGIN ++ "\n" ++
        "[[hooks.SessionStart]]\n" ++
        "matcher = \"startup|resume|clear|compact\"\n\n" ++
        "[[hooks.SessionStart.hooks]]\n" ++
        "type = \"command\"\n" ++
        "command = '" ++ CMM_SESSION_REMINDER_CMD ++ "'\n" ++ CODEX_HOOK_END ++ "\n";

    const content_buf = readFileStr(config_path, null) orelse return writeFileStr(config_path, block[1..]); // skip leading newline
    defer c.free(content_buf);
    const content = std.mem.span(content_buf);
    const stripped = codexHookStrip(content);
    defer if (stripped) |s| gpa.free(s);
    return writeParts(config_path, &.{ stripped orelse content, block });
}

pub export fn cbm_remove_codex_hooks(config_path: [*c]const u8) c_int {
    if (config_path == null) return CLI_ERR;
    const content_buf = readFileStr(config_path, null) orelse return CLI_TRUE;
    defer c.free(content_buf);
    const stripped = codexHookStrip(std.mem.span(content_buf)) orelse return CLI_TRUE; // nothing to remove
    defer gpa.free(stripped);
    return writeFileStr(config_path, stripped);
}

// ── Claude Code pre-tool hooks ─────────────────────────────────────

/// Matcher intentionally excludes Read: gating Read breaks Claude Code's
/// read-before-edit invariant (issue #362). The hook is a non-blocking
/// augmenter, never a gate.
const CMM_HOOK_MATCHER = "Grep|Glob";
/// Script basenames; the full command path is resolved at install time via
/// resolveHookCommand so $CLAUDE_CONFIG_DIR is honored. Extensionless bash
/// shims under %USERPROFILE%\.claude\hooks open the "How do you want to open
/// this file?" dialog when an editor scans the hooks dir, and cannot run
/// without bash, so Windows installs .cmd scripts and removes the
/// extensionless twin (upstream b1502bf3).
const SCRIPT_EXT = if (is_windows) ".cmd" else "";
pub const CMM_HOOK_GATE_SCRIPT_LEGACY = "cbm-code-discovery-gate";
pub const CMM_HOOK_GATE_SCRIPT = CMM_HOOK_GATE_SCRIPT_LEGACY ++ SCRIPT_EXT;
pub const CMM_SESSION_REMINDER_SCRIPT_LEGACY = "cbm-session-reminder";
pub const CMM_SESSION_REMINDER_SCRIPT = CMM_SESSION_REMINDER_SCRIPT_LEGACY ++ SCRIPT_EXT;
pub const CMM_SUBAGENT_REMINDER_SCRIPT_LEGACY = "cbm-subagent-reminder";
pub const CMM_SUBAGENT_REMINDER_SCRIPT = CMM_SUBAGENT_REMINDER_SCRIPT_LEGACY ++ SCRIPT_EXT;
/// Hard backstop in settings.json; the binary also self-bounds with an
/// in-process deadline well under this.
const CMM_HOOK_TIMEOUT_SEC = 5;
/// Claude hooks run the binary directly (no shell) as `<binary>
/// hook-augment`; hook-augment dispatches on the payload's hook_event_name
/// (upstream 7d20ff93).
const CMM_HOOK_EXEC_ARG = "hook-augment";

/// Old matcher values from previous versions — recognized during upgrade so
/// upsert/remove can clean them up before inserting the current matcher.
const cmm_claude_old_matchers = [_][]const u8{ "Grep|Glob|Read|Search", "Grep|Glob|Read" };
const cmm_gemini_old_matchers = [_][]const u8{"google_search|read_file|grep_search"};

/// True when a hook object is our exec form: "args" is exactly
/// [CMM_HOOK_EXEC_ARG].
fn isCmmExecHook(hook: ?*MutVal) bool {
    const args = if (isObj(hook)) objGet(hook, "args") else null;
    if (!isArr(args) or c.yyjson_mut_arr_size(args) != 1) return false;
    const arg = c.yyjson_mut_arr_get(args, 0);
    return arg != null and c.yyjson_mut_is_str(arg) and cEql(c.yyjson_mut_get_str(arg), CMM_HOOK_EXEC_ARG);
}

/// Whether a hook array entry is ours (current matcher or a known old one).
/// When require_command_substr is set, the matcher match is not sufficient:
/// the entry must ALSO carry a hooks[].command containing that substring, or
/// be our exec form. This disambiguates our entry from a user's own hook that
/// happens to share the same matcher (notably "*", which a user is likely to
/// pick for a catch-all hook), so upsert/remove never clobber a foreign
/// entry. null keeps matcher-only matching for callers whose matcher is
/// already CMM-specific (e.g. "startup").
fn isCmmHookEntry(entry: ?*MutVal, matcher_str: []const u8, old_matchers: []const []const u8, require_command_substr: ?[]const u8) bool {
    const matcher = objGet(entry, "matcher");
    if (matcher == null or !c.yyjson_mut_is_str(matcher)) return false;
    const val_c = c.yyjson_mut_get_str(matcher);
    if (val_c == null) return false;
    const val = std.mem.span(val_c);
    var matcher_ok = eql(val, matcher_str);
    for (old_matchers) |old| {
        if (eql(val, old)) matcher_ok = true;
    }
    if (!matcher_ok) return false;
    const substr = require_command_substr orelse return true;
    const hooks = objGet(entry, "hooks");
    if (!isArr(hooks)) return false;
    var iter: c.yyjson_mut_arr_iter = undefined;
    _ = c.yyjson_mut_arr_iter_init(hooks, &iter);
    while (c.yyjson_mut_arr_iter_next(&iter)) |h| {
        if (isCmmExecHook(h)) return true;
        const cmd = objGet(h, "command");
        if (cmd != null and c.yyjson_mut_is_str(cmd)) {
            const cs = c.yyjson_mut_get_str(cmd);
            if (cs != null and std.mem.indexOf(u8, std.mem.span(cs), substr) != null) return true;
        }
    }
    return false;
}

/// Remove the first entry of event_arr that isCmmHookEntry claims.
fn removeCmmHookEntry(event_arr: *MutVal, matcher_str: []const u8, old_matchers: []const []const u8, require_command_substr: ?[]const u8) void {
    var iter: c.yyjson_mut_arr_iter = undefined;
    _ = c.yyjson_mut_arr_iter_init(event_arr, &iter);
    var idx: usize = 0;
    while (c.yyjson_mut_arr_iter_next(&iter)) |item| : (idx += 1) {
        if (isCmmHookEntry(item, matcher_str, old_matchers, require_command_substr)) {
            _ = c.yyjson_mut_arr_remove(event_arr, idx);
            return;
        }
    }
}

const HooksUpsertArgs = struct {
    settings_path: CStr,
    hook_event: Str,
    matcher_str: Str,
    command_str: CStr,
    /// non-null: exec form, adds "args":[command_arg]
    command_arg: ?Str = null,
    old_matchers: []const []const u8 = &.{},
    /// >0 adds "timeout" to the hook entry
    timeout_sec: c_int = 0,
    /// non-null: also require this in the entry command to claim ownership
    match_command_substr: ?[]const u8 = null,
};

/// Generic hook upsert for both Claude Code and Gemini CLI.
fn upsertHooksJson(args: HooksUpsertArgs) c_int {
    if (args.settings_path == null) return CLI_ERR;
    const mdoc = c.yyjson_mut_doc_new(null) orelse return CLI_ERR;
    defer c.yyjson_mut_doc_free(mdoc);

    const root = if (readJsonFile(args.settings_path)) |doc| blk: {
        const r = c.yyjson_val_mut_copy(mdoc, c.yyjson_doc_get_root(doc));
        c.yyjson_doc_free(doc);
        break :blk r;
    } else c.yyjson_mut_obj(mdoc);
    if (root == null) return CLI_ERR;
    c.yyjson_mut_doc_set_root(mdoc, root);

    const hooks = getOrAddObj(mdoc, root.?, "hooks");
    // Get or create the hook event array (e.g. PreToolUse / BeforeTool).
    var event_arr = objGet(hooks, args.hook_event);
    if (!isArr(event_arr)) {
        event_arr = c.yyjson_mut_arr(mdoc);
        _ = c.yyjson_mut_obj_add_val(mdoc, hooks, args.hook_event, event_arr);
    }
    removeCmmHookEntry(event_arr.?, std.mem.span(args.matcher_str), args.old_matchers, args.match_command_substr);

    const entry = c.yyjson_mut_obj(mdoc);
    _ = c.yyjson_mut_obj_add_str(mdoc, entry, "matcher", args.matcher_str);
    const hooks_arr = c.yyjson_mut_arr(mdoc);
    const hook_obj = c.yyjson_mut_obj(mdoc);
    _ = c.yyjson_mut_obj_add_str(mdoc, hook_obj, "type", "command");
    _ = c.yyjson_mut_obj_add_str(mdoc, hook_obj, "command", args.command_str);
    if (args.command_arg) |arg| {
        const command_args = c.yyjson_mut_arr(mdoc);
        _ = c.yyjson_mut_arr_add_str(mdoc, command_args, arg);
        _ = c.yyjson_mut_obj_add_val(mdoc, hook_obj, "args", command_args);
    }
    if (args.timeout_sec > 0) _ = c.yyjson_mut_obj_add_int(mdoc, hook_obj, "timeout", args.timeout_sec);
    _ = c.yyjson_mut_arr_append(hooks_arr, hook_obj);
    _ = c.yyjson_mut_obj_add_val(mdoc, entry, "hooks", hooks_arr);
    _ = c.yyjson_mut_arr_append(event_arr, entry);

    return writeJsonFile(args.settings_path, mdoc);
}

const HooksRemoveArgs = struct {
    settings_path: CStr,
    hook_event: Str,
    matcher_str: []const u8,
    old_matchers: []const []const u8 = &.{},
    match_command_substr: ?[]const u8 = null,
};

/// Generic hook remove for both Claude Code and Gemini CLI.
fn removeHooksJson(args: HooksRemoveArgs) c_int {
    if (args.settings_path == null) return CLI_ERR;
    const mdoc = loadJsonForRemoval(args.settings_path) orelse return CLI_ERR;
    defer c.yyjson_mut_doc_free(mdoc);
    const hooks = objGet(c.yyjson_mut_doc_get_root(mdoc), "hooks") orelse return 0;
    const event_arr = objGet(hooks, args.hook_event);
    if (!isArr(event_arr)) return 0;
    removeCmmHookEntry(event_arr.?, args.matcher_str, args.old_matchers, args.match_command_substr);
    // Prune the event key once its array is empty, so removing our hook leaves
    // no stale "<Event>": [] cruft behind.
    if (c.yyjson_mut_arr_size(event_arr) == 0) _ = c.yyjson_mut_obj_remove_key(hooks, args.hook_event);
    return writeJsonFile(args.settings_path, mdoc);
}

/// Claude hook command: the exec form `<binary_path> hook-augment` when a
/// binary is known (install), else the shell-form script path. Sets arg to
/// the exec argument, or null for the shell form.
fn claudeHookCommand(binary_path: CStr, script: []const u8, buf: []u8, arg: *?Str) CStr {
    if (binary_path != null and binary_path[0] != 0) {
        arg.* = CMM_HOOK_EXEC_ARG;
        return binary_path;
    }
    arg.* = null;
    return resolveHookCommand(script, buf);
}

fn upsertClaudeHooksWithBinary(settings_path: CStr, binary_path: CStr) c_int {
    var buf: [1024]u8 = undefined;
    var arg: ?Str = null;
    const command = claudeHookCommand(binary_path, CMM_HOOK_GATE_SCRIPT, &buf, &arg);
    return upsertHooksJson(.{
        .settings_path = settings_path,
        .hook_event = "PreToolUse",
        .matcher_str = CMM_HOOK_MATCHER,
        .command_str = command,
        .command_arg = arg,
        .old_matchers = &cmm_claude_old_matchers,
        .timeout_sec = CMM_HOOK_TIMEOUT_SEC,
    });
}

pub export fn cbm_upsert_claude_hooks(settings_path: [*c]const u8) c_int {
    return upsertClaudeHooksWithBinary(settings_path, null);
}

pub export fn cbm_remove_claude_hooks(settings_path: [*c]const u8) c_int {
    return removeHooksJson(.{
        .settings_path = settings_path,
        .hook_event = "PreToolUse",
        .matcher_str = CMM_HOOK_MATCHER,
        .old_matchers = &cmm_claude_old_matchers,
    });
}

/// The Claude hooks dir (created), or null when no config dir is known. On
/// Windows the pre-.cmd extensionless twin of `legacy_name` is removed; POSIX
/// keeps the extensionless name, where legacy == current.
fn prepareHooksDir(home: CStr, legacy_name: []const u8, out: *PathBuf) ?[]const u8 {
    var config_dir: PathBuf = undefined;
    if (claudeConfigDir(home, &config_dir)[0] == 0) return null;
    const hooks_dir = std.mem.span(fmtZ(out, "{s}/hooks", .{bufStr(&config_dir)}));
    _ = c.cbm_mkdir_p(out, EXEC_PERMS);
    if (is_windows) {
        var legacy_path: PathBuf = undefined;
        _ = c.cbm_unlink(fmtZ(&legacy_path, "{s}/{s}", .{ hooks_dir, legacy_name }));
    }
    return hooks_dir;
}

/// Write a hook script and make it executable. fchmod before close avoids a
/// TOCTOU race (CodeQL cpp/toctou-race-condition).
fn writeHookScript(script_path: CStr, text: []const u8) void {
    const f = c.fopen(script_path, "w") orelse return;
    _ = c.fwrite(text.ptr, 1, text.len, f);
    if (!is_windows) _ = c.fchmod(c.fileno(f), EXEC_PERMS);
    _ = c.fclose(f);
    if (is_windows) _ = c.chmod(script_path, EXEC_PERMS);
}

/// Install the search-augmenter shim to ~/.claude/hooks/. The shim is a thin
/// wrapper that delegates to `<binary> hook-augment`, which adds graph
/// context to Grep/Glob calls. It NEVER blocks a tool call: a
/// missing/old/hung binary results in a silent exit 0 (issue #362/#288). The
/// legacy filename `cbm-code-discovery-gate` is retained so existing
/// settings.json entries and uninstall keep working with zero migration.
pub export fn cbm_install_hook_gate_script(home: [*c]const u8, binary_path: [*c]const u8) void {
    if (home == null or binary_path == null) return;
    const bin = std.mem.span(binary_path);
    // Defensive: refuse to embed a binary path containing a double-quote,
    // which would break the BIN="..." shell quoting in the generated shim.
    if (std.mem.indexOfScalar(u8, bin, '"') != null) return;
    var hooks_buf: PathBuf = undefined;
    const hooks_dir = prepareHooksDir(home, CMM_HOOK_GATE_SCRIPT_LEGACY, &hooks_buf) orelse return;
    var script_path: PathBuf = undefined;
    _ = fmtZ(&script_path, "{s}/" ++ CMM_HOOK_GATE_SCRIPT, .{hooks_dir});

    const text = if (is_windows)
        std.fmt.allocPrint(gpa, "@echo off\r\n" ++
            "REM codebase-memory-mcp search augmenter (Claude Code PreToolUse).\r\n" ++
            "REM Never blocks a tool call - it only adds graph context.\r\n" ++
            "REM Any failure is silent (exit 0, no output).\r\n" ++
            "if not exist \"{s}\" exit /b 0\r\n" ++
            "\"{s}\" hook-augment 2>NUL\r\n" ++
            "exit /b 0\r\n", .{ bin, bin })
    else
        std.fmt.allocPrint(gpa, "#!/usr/bin/env bash\n" ++
            "# codebase-memory-mcp search augmenter (Claude Code PreToolUse).\n" ++
            "# NOTE: the legacy filename is kept for zero-migration upgrades.\n" ++
            "# Despite the name this NEVER blocks a tool call - it only adds\n" ++
            "# graph context. Any failure is silent (exit 0, no output).\n" ++
            "BIN=\"{s}\"\n" ++
            "[ -x \"$BIN\" ] || exit 0\n" ++
            "\"$BIN\" hook-augment 2>/dev/null\n" ++
            "exit 0\n", .{bin});
    const t = text catch return;
    defer gpa.free(t);
    writeHookScript(&script_path, t);
}

/// SessionStart hook: remind agent to use MCP tools on every context reset.
pub fn installSessionReminderScript(home: CStr) void {
    if (home == null) return;
    var hooks_buf: PathBuf = undefined;
    const hooks_dir = prepareHooksDir(home, CMM_SESSION_REMINDER_SCRIPT_LEGACY, &hooks_buf) orelse return;
    var script_path: PathBuf = undefined;
    _ = fmtZ(&script_path, "{s}/" ++ CMM_SESSION_REMINDER_SCRIPT, .{hooks_dir});
    const text = if (is_windows)
        // cmd variant: one echo per line; `|` must be caret-escaped.
        "@echo off\r\n" ++
            "REM SessionStart hook: remind agent to use codebase-memory-mcp tools.\r\n" ++
            "echo CRITICAL - Code Discovery Protocol:\r\n" ++
            "echo 1. ALWAYS use codebase-memory-mcp tools FIRST for ANY code exploration:\r\n" ++
            "echo    - search_graph(name_pattern/label/qn_pattern) to find functions/classes/routes\r\n" ++
            "echo    - trace_path(function_name, mode=calls^|data_flow^|cross_service) for call " ++
            "chains\r\n" ++
            "echo    - get_code_snippet(qualified_name) for exact symbol source (precise ranges)\r\n" ++
            "echo    - query_graph(query) for complex Cypher patterns\r\n" ++
            "echo    - get_architecture(aspects) for project structure\r\n" ++
            "echo    - search_code(pattern) for text search (graph-augmented grep)\r\n" ++
            "echo 2. Use Grep/Glob/Read freely for text, configs, non-code files, and\r\n" ++
            "echo    always Read a file before editing it.\r\n" ++
            "echo 3. If a project is not indexed yet, run index_repository FIRST.\r\n"
    else
        "#!/usr/bin/env bash\n" ++
            "# SessionStart hook: remind agent to use codebase-memory-mcp tools.\n" ++
            "# Installed by codebase-memory-mcp. Fires on startup/resume/clear/compact.\n" ++
            "cat << 'REMINDER'\n" ++
            "CRITICAL - Code Discovery Protocol:\n" ++
            "1. ALWAYS use codebase-memory-mcp tools FIRST for ANY code exploration:\n" ++
            "   - search_graph(name_pattern/label/qn_pattern) to find functions/classes/routes\n" ++
            "   - trace_path(function_name, mode=calls|data_flow|cross_service) for call chains\n" ++
            "   - get_code_snippet(qualified_name) for exact symbol source (precise ranges)\n" ++
            "   - query_graph(query) for complex Cypher patterns\n" ++
            "   - get_architecture(aspects) for project structure\n" ++
            "   - search_code(pattern) for text search (graph-augmented grep)\n" ++
            "2. Use Grep/Glob/Read freely for text, configs, non-code files, and\n" ++
            "   always Read a file before editing it.\n" ++
            "3. If a project is not indexed yet, run index_repository FIRST.\n" ++
            "REMINDER\n";
    writeHookScript(&script_path, text);
}

const session_matchers = [_]Str{ "startup", "resume", "clear", "compact" };

pub fn upsertSessionHooks(settings_path: CStr, binary_path: CStr) c_int {
    var buf: [1024]u8 = undefined;
    var arg: ?Str = null;
    const command = claudeHookCommand(binary_path, CMM_SESSION_REMINDER_SCRIPT, &buf, &arg);
    var rc: c_int = 0;
    for (session_matchers) |m| {
        if (upsertHooksJson(.{ .settings_path = settings_path, .hook_event = "SessionStart", .matcher_str = m, .command_str = command, .command_arg = arg }) != 0) rc = CLI_ERR;
    }
    return rc;
}

pub fn removeSessionHooks(settings_path: CStr) c_int {
    var rc: c_int = 0;
    for (session_matchers) |m| {
        if (removeHooksJson(.{ .settings_path = settings_path, .hook_event = "SessionStart", .matcher_str = std.mem.span(m) }) != 0) rc = CLI_ERR;
    }
    return rc;
}

/// SubagentStart hook: subagents spawned via the Agent tool do NOT fire
/// SessionStart, so the SessionStart reminder never reaches them. This hook
/// is their equivalent. Unlike SessionStart (where plain stdout is injected as
/// context), SubagentStart injects context only via a JSON object on stdout.
/// The text is a leaner variant of the SessionStart protocol: it omits the
/// "run index_repository first" step, since the parent session has already
/// indexed the project. Matcher "*" fires for every agent type.
pub fn installSubagentReminderScript(home: CStr) void {
    if (home == null) return;
    var hooks_buf: PathBuf = undefined;
    const hooks_dir = prepareHooksDir(home, CMM_SUBAGENT_REMINDER_SCRIPT_LEGACY, &hooks_buf) orelse return;
    var script_path: PathBuf = undefined;
    _ = fmtZ(&script_path, "{s}/" ++ CMM_SUBAGENT_REMINDER_SCRIPT, .{hooks_dir});
    // The additionalContext value is a single line with no embedded quotes,
    // backslashes, or newlines, so the JSON below is valid as written — no
    // runtime escaping (and no python3/jq dependency) is required.
    const json = "{\"hookSpecificOutput\":{\"hookEventName\":\"SubagentStart\"," ++
        "\"additionalContext\":\"Code discovery: prefer codebase-memory-mcp tools " ++
        "(search_graph, trace_path, get_code_snippet, query_graph, get_architecture, " ++
        "search_code) over grep/file-read for navigating code. Use Grep/Glob/Read for " ++
        "text, configs, and non-code files.\"}}";
    const text = if (is_windows)
        // cmd variant: one echo with the JSON verbatim (no cmd metacharacters).
        "@echo off\r\n" ++
            "REM SubagentStart hook: tell subagents to use codebase-memory-mcp tools.\r\n" ++
            "echo " ++ json ++ "\r\n"
    else
        "#!/usr/bin/env bash\n" ++
            "# SubagentStart hook: tell subagents to use codebase-memory-mcp tools.\n" ++
            "# Installed by codebase-memory-mcp. Fires when any subagent is spawned.\n" ++
            "# SubagentStart injects context via JSON additionalContext, not plain stdout.\n" ++
            "cat << 'REMINDER'\n" ++
            json ++ "\n" ++
            "REMINDER\n";
    writeHookScript(&script_path, text);
}

fn upsertClaudeSubagentHooksWithBinary(settings_path: CStr, binary_path: CStr) c_int {
    var buf: [1024]u8 = undefined;
    var arg: ?Str = null;
    const command = claudeHookCommand(binary_path, CMM_SUBAGENT_REMINDER_SCRIPT, &buf, &arg);
    // matcher "*" is the natural choice a user would also pick for their own
    // catch-all SubagentStart hook, so claim ownership by command too — never
    // clobber or remove a foreign "*" entry.
    return upsertHooksJson(.{
        .settings_path = settings_path,
        .hook_event = "SubagentStart",
        .matcher_str = "*",
        .command_str = command,
        .command_arg = arg,
        .match_command_substr = CMM_SUBAGENT_REMINDER_SCRIPT_LEGACY,
    });
}

pub export fn cbm_upsert_claude_subagent_hooks(settings_path: [*c]const u8) c_int {
    return upsertClaudeSubagentHooksWithBinary(settings_path, null);
}

pub export fn cbm_upsert_claude_exec_hooks(settings_path: [*c]const u8, binary_path: [*c]const u8) c_int {
    if (binary_path == null or binary_path[0] == 0) return CLI_ERR;
    var rc: c_int = 0;
    if (upsertClaudeHooksWithBinary(settings_path, binary_path) != 0) rc = CLI_ERR;
    if (upsertSessionHooks(settings_path, binary_path) != 0) rc = CLI_ERR;
    if (upsertClaudeSubagentHooksWithBinary(settings_path, binary_path) != 0) rc = CLI_ERR;
    return rc;
}

pub export fn cbm_remove_claude_subagent_hooks(settings_path: [*c]const u8) c_int {
    return removeHooksJson(.{
        .settings_path = settings_path,
        .hook_event = "SubagentStart",
        .matcher_str = "*",
        .match_command_substr = CMM_SUBAGENT_REMINDER_SCRIPT_LEGACY,
    });
}

/// Matcher excludes read_file for consistency with the Claude fix: the hook
/// is an advisory reminder, not a gate over the agent's file reads.
const GEMINI_HOOK_MATCHER = "google_search|grep_search";
const GEMINI_HOOK_COMMAND =
    "echo 'Reminder: prefer codebase-memory-mcp search_graph/trace_path/" ++
    "get_code_snippet over grep/file search for code discovery.' >&2";

pub export fn cbm_upsert_gemini_hooks(settings_path: [*c]const u8) c_int {
    return upsertHooksJson(.{
        .settings_path = settings_path,
        .hook_event = "BeforeTool",
        .matcher_str = GEMINI_HOOK_MATCHER,
        .command_str = GEMINI_HOOK_COMMAND,
        .old_matchers = &cmm_gemini_old_matchers,
    });
}

pub export fn cbm_remove_gemini_hooks(settings_path: [*c]const u8) c_int {
    return removeHooksJson(.{
        .settings_path = settings_path,
        .hook_event = "BeforeTool",
        .matcher_str = GEMINI_HOOK_MATCHER,
        .old_matchers = &cmm_gemini_old_matchers,
    });
}

/// Gemini CLI / Antigravity SessionStart reminder. settings.json uses the same
/// hooks.<Event>[].hooks[] JSON shape as Claude, so it reuses
/// upsertHooksJson. The SessionStart matcher is advisory in Gemini (it does
/// not filter lifecycle sources), so a single "startup" entry fires on
/// startup/resume/clear. The command's stdout is injected as session context.
pub export fn cbm_upsert_gemini_session_hooks(settings_path: [*c]const u8) c_int {
    return upsertHooksJson(.{
        .settings_path = settings_path,
        .hook_event = "SessionStart",
        .matcher_str = "startup",
        .command_str = CMM_SESSION_REMINDER_CMD,
    });
}

pub export fn cbm_remove_gemini_session_hooks(settings_path: [*c]const u8) c_int {
    return removeHooksJson(.{
        .settings_path = settings_path,
        .hook_event = "SessionStart",
        .matcher_str = "startup",
    });
}

// ── PATH management ────────────────────────────────────────────────

pub export fn cbm_ensure_path(bin_dir: [*c]const u8, rc_file: [*c]const u8, dry_run: bool) c_int {
    if (bin_dir == null or rc_file == null) return CLI_ERR;

    // fish uses a different syntax than POSIX shells: `export PATH="...:$PATH"`
    // is a syntax error in fish and breaks config.fish (#319). When the target
    // is a fish config, emit the fish-native `fish_add_path` (idempotent,
    // prepends only if absent) instead.
    const is_fish = std.mem.endsWith(u8, std.mem.span(rc_file), ".fish");
    var line_buf: [1024]u8 = undefined;
    const line = std.mem.span(if (is_fish)
        fmtZ(&line_buf, "fish_add_path {s}", .{sp(bin_dir)})
    else
        fmtZ(&line_buf, "export PATH=\"{s}:$PATH\"", .{sp(bin_dir)}));

    // Check if already present in rc file.
    if (c.fopen(rc_file, "r")) |f| {
        var buf: [2048]u8 = undefined;
        while (c.fgets(&buf, buf.len, f) != null) {
            if (std.mem.indexOf(u8, bufStr(&buf), line) != null) {
                _ = c.fclose(f);
                return CLI_TRUE; // already present
            }
        }
        _ = c.fclose(f);
    }
    if (dry_run) return 0;

    const f = c.fopen(rc_file, "a") orelse return CLI_ERR;
    _ = c.fprintf(f, "\n# Added by codebase-memory-mcp install\n%s\n", line.ptr);
    _ = c.fclose(f);
    return 0;
}
