//! server.zig — the MCP request path: tool dispatch, the opt-in compact
//! output format, detect_changes, session detection with background
//! auto-indexing and update checks, the JSON-RPC request handler and the
//! stdio event loop.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const mcp = @import("mcp.zig");
const code_tools = @import("code_tools.zig");
const graph_tools = @import("graph_tools.zig");
const index_tools = @import("index_tools.zig");
const project_tools = @import("project_tools.zig");
const search_tools = @import("search_tools.zig");

const CStr = mcp.CStr;
const HeapStr = mcp.HeapStr;
const Str = mcp.Str;
const Server = mcp.Server;
const sp = mcp.sp;
const eql = mcp.eql;
const is_windows = mcp.is_windows;
const textResult = mcp.textResult;

const MCP_TIMEOUT_MS = 1000;
const MCP_DEFAULT_BFS_DEPTH = 2;
const MCP_DEFAULT_LIMIT = 10;
/// Idle store eviction timeout in seconds.
const STORE_IDLE_TIMEOUT_S = 60;
/// Largest accepted Content-Length frame body.
const MCP_MAX_FRAME_BYTES = MCP_DEFAULT_LIMIT * 1024 * 1024;

fn trimLineEnd(s: []const u8) []const u8 {
    return std.mem.trimEnd(u8, s, "\r\n");
}

/// strtol(s, NULL, 10) clamped to c_int the way `(int)strtol(...)` reads a
/// count: leading whitespace and sign accepted; an overflow reads as -1.
fn parseLeadingInt(s: []const u8) c_int {
    var i: usize = 0;
    while (i < s.len and std.ascii.isWhitespace(s[i])) i += 1;
    const neg = i < s.len and s[i] == '-';
    if (i < s.len and (s[i] == '-' or s[i] == '+')) i += 1;
    const start = i;
    while (i < s.len and std.ascii.isDigit(s[i])) i += 1;
    if (i == start) return 0;
    const v = std.fmt.parseInt(c_int, s[start..i], 10) catch return -1;
    return if (neg) -v else v;
}

// ── detect_changes ───────────────────────────────────────────────

/// Find symbols defined in a file and add them to the impacted array.
fn detectAddImpactedSymbols(store: ?*c.cbm_store_t, project: CStr, file: CStr, doc: ?*mcp.MutDoc, impacted: ?*mcp.MutVal) void {
    var nodes: [*c]c.cbm_node_t = null;
    var ncount: c_int = 0;
    _ = c.cbm_store_find_nodes_by_file(store, project, file, &nodes, &ncount);
    defer c.cbm_store_free_nodes(nodes, ncount);
    for (0..@intCast(@max(ncount, 0))) |i| {
        const n = &nodes[i];
        if (n.label == null or mcp.cEql(n.label, "File") or mcp.cEql(n.label, "Folder") or mcp.cEql(n.label, "Project")) continue;
        const item = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_strcpy(doc, item, "name", mcp.orEmpty(n.name));
        _ = c.yyjson_mut_obj_add_strcpy(doc, item, "label", n.label);
        _ = c.yyjson_mut_obj_add_strcpy(doc, item, "file", file);
        _ = c.yyjson_mut_arr_add_val(impacted, item);
    }
}

/// Characters git status --porcelain uses in its two-column status code.
fn isPorcelainCode(ch: u8) bool {
    return std.mem.indexOfScalar(u8, " MADRCU?!", ch) != null;
}

/// Run one contained git query and return its first output line, or false if the
/// command fails, prints nothing, or yields a string unsafe for shell interpolation.
fn detectGitFirstLine(root: []const u8, git_args: []const u8, out_buf: []u8) bool {
    var cmd: [2048]u8 = undefined;
    const null_dev = if (is_windows) "2>NUL" else "2>/dev/null";
    const q = if (is_windows) "\"" else "'";
    const cmd_z = mcp.fmtZ(&cmd, "git -C {s}{s}{s} {s} {s}", .{ q, root, q, git_args, null_dev });

    const fp = c.cbm_popen_git(cmd_z);
    if (fp == null) return false;

    var raw_line: [1024]u8 = undefined;
    const got = c.fgets(&raw_line, raw_line.len, fp);
    const rc = c.cbm_pclose(fp);
    if (got == null or rc != 0) return false;

    const s = std.mem.trimEnd(u8, std.mem.sliceTo(&raw_line, 0), "\r\n ");
    if (s.len == 0 or s.len >= out_buf.len) return false;
    if (s[0] == '-') return false;

    @memcpy(out_buf[0..s.len], s);
    out_buf[s.len] = 0;
    const out_c: CStr = @ptrCast(out_buf.ptr);
    if (!c.cbm_validate_shell_arg(out_c)) return false;
    if (is_windows) {
        for (s) |ch| {
            if (ch == '%' or ch == '!' or ch == '^') return false;
        }
    }
    return true;
}

/// #124: without base_branch, diff against the repository's default branch rather
/// than a literal "main" that trunk/master/develop repositories do not have.
/// Order: origin/HEAD (what the clone calls the default), local main, local master,
/// then the current branch's upstream. Fallback: "main".
fn detectDefaultBase(root_path: CStr) HeapStr {
    const root = sp(root_path);
    var buf: [1024]u8 = undefined;
    const q = if (is_windows) "\"" else "'";

    // 1. origin/HEAD (what the clone calls the default)
    if (detectGitFirstLine(root, "symbolic-ref -q --short refs/remotes/origin/HEAD", &buf)) {
        return mcp.dup(@as(CStr, @ptrCast(&buf)));
    }

    // 2. local main or master
    const local_defaults = [_]CStr{ "main", "master" };
    for (local_defaults) |branch_name| {
        var verify_arg: [256]u8 = undefined;
        _ = mcp.bprint(&verify_arg, "rev-parse -q --verify {s}refs/heads/{s}^{{commit}}{s}", .{ q, sp(branch_name), q });
        if (detectGitFirstLine(root, mcp.bufStr(&verify_arg), &buf)) {
            return mcp.dup(branch_name);
        }
    }

    // 3. current branch's upstream
    var upstream_arg: [128]u8 = undefined;
    _ = mcp.bprint(&upstream_arg, "rev-parse -q --abbrev-ref --symbolic-full-name {s}@{{upstream}}{s}", .{ q, q });
    if (detectGitFirstLine(root, mcp.bufStr(&upstream_arg), &buf)) {
        return mcp.dup(@as(CStr, @ptrCast(&buf)));
    }

    // 4. fallback to "main"
    return mcp.dup("main");
}

fn handleDetectChanges(srv: *Server, args: CStr) HeapStr {
    const project = mcp.getProjectArg(args);
    var base_branch = mcp.strArg(args, "base_branch");
    defer c.free(base_branch);
    const since = mcp.strArg(args, "since");
    const scope = mcp.strArg(args, "scope");
    const depth = code_tools.clampMcpDepth(mcp.intArg(args, "depth", MCP_DEFAULT_BFS_DEPTH), "detect_changes");

    // scope: "files" = just changed files, "symbols" = files + symbols (default)
    const want_symbols = scope == null or mcp.cEql(scope, "symbols") or mcp.cEql(scope, "impact");
    c.free(scope);

    // `since` (e.g. "HEAD~10", "v0.5.0") is the documented diff base and takes
    // precedence over base_branch. Route it through base_branch so the shared
    // shell-arg validation and the existing `<base>...HEAD` (three-dot) diff
    // apply unchanged — `since` thus adopts the same merge-base semantics
    // base_branch already uses.
    if (since != null and since[0] != 0) {
        c.free(base_branch);
        base_branch = since;
    } else {
        c.free(since);
    }

    // Reject shell metacharacters, and a leading '-', in the user-supplied
    // branch name. base_branch is spliced into `git diff --name-only
    // "<base>"...HEAD`; a value starting with '-' would be read by git as an
    // option rather than a ref (e.g. `--output=<path>` writes the diff to an
    // arbitrary file). A real git ref never begins with '-'.
    if (base_branch != null) {
        if (!c.cbm_validate_shell_arg(base_branch) or base_branch[0] == '-') {
            c.free(project);
            return textResult("base_branch contains invalid characters", true);
        }
    }

    const root_path = code_tools.getProjectRoot(project);
    if (root_path == null) return mcp.projectNotFoundResult(project);
    defer c.free(project);
    defer c.free(root_path);

    if (!search_tools.validateSearchPathArg(root_path)) {
        return textResult("project path contains invalid characters", true);
    }

    // Without base_branch, diff against the repository's default branch rather
    // than a literal "main" that repositories may not have (#124).
    if (base_branch == null) {
        base_branch = detectDefaultBase(root_path);
    }

    // Get changed files via git (-C avoids cd + quoting issues on Windows).
    // Three sources are merged:
    //   1. committed changes vs base   (diff <base>...HEAD)
    //   2. unstaged tracked changes    (diff)
    //   3. untracked + staged-new files (status --porcelain) — these are
    //      invisible to `git diff` and were silently missed before, so a
    //      brand-new file never appeared until a manual re-index (#520).
    // status --porcelain prefixes each path with a 2-char code + space
    // ("?? path", "A  path"); the prefix is stripped when parsing below.
    var cmd: [2048]u8 = undefined;
    const root = sp(root_path);
    const base = sp(base_branch);
    if (is_windows) {
        _ = mcp.bprint(&cmd, "git -C \"{s}\" diff --name-only \"{s}\"...HEAD 2>NUL & " ++
            "git -C \"{s}\" diff --name-only 2>NUL & " ++
            "git --no-optional-locks -C \"{s}\" status --porcelain " ++
            "--untracked-files=normal 2>NUL", .{ root, base, root, root });
    } else {
        _ = mcp.bprint(&cmd, "{{ git -C '{s}' diff --name-only '{s}'...HEAD 2>/dev/null; " ++
            "git -C '{s}' diff --name-only 2>/dev/null; " ++
            "git --no-optional-locks -C '{s}' status --porcelain " ++
            "--untracked-files=normal 2>/dev/null; }} | sort -u", .{ root, base, root, root });
    }

    const fp = c.cbm_popen_git(&cmd);
    if (fp == null) {
        var errmsg: [256]u8 = undefined;
        return textResult(mcp.fmtZ(&errmsg, "git diff failed: cannot execute command ({s}). Check that git is installed.", .{sp(c.strerror(std.c._errno().*))}), true);
    }

    const doc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(doc);
    const root_obj = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root_obj);
    const changed = c.yyjson_mut_arr(doc);
    const impacted = c.yyjson_mut_arr(doc);

    const store = mcp.resolveStore(srv, project);

    var line: [1024]u8 = undefined;
    var file_count: c_int = 0;
    while (c.fgets(&line, line.len, fp) != null) {
        const s = trimLineEnd(mcp.bufStr(&line));
        if (s.len == 0) continue;
        line[s.len] = 0;

        // `git status --porcelain` prefixes each path with a two-character
        // status code and a space ("?? path", "A  path", " M path"). The two
        // `git diff --name-only` sources emit bare paths. Strip the porcelain
        // prefix when present so all three sources yield clean paths; for a
        // rename ("R  old -> new") keep the post-arrow destination path.
        var start: usize = 0;
        if (s.len > 2 and s[2] == ' ' and isPorcelainCode(s[0]) and isPorcelainCode(s[1])) {
            start = 3;
            if (std.mem.indexOfPos(u8, s, start, " -> ")) |arrow| start = arrow + 4;
        }
        if (start >= s.len) continue;
        const path_line: [*c]const u8 = &line[start];

        _ = c.yyjson_mut_arr_add_strcpy(doc, changed, path_line);
        file_count += 1;
        if (want_symbols and store != null) detectAddImpactedSymbols(store, project, path_line, doc, impacted);
    }
    const git_status = c.cbm_pclose(fp);

    var is_error = false;
    if (git_status != 0 and file_count == 0) {
        var hint_buf: [256]u8 = undefined;
        _ = c.yyjson_mut_obj_add_strcpy(doc, root_obj, "hint", mcp.fmtZ(&hint_buf, "git diff exited with status {d}. Check that branch '{s}' exists.", .{ git_status, base }));
        is_error = true;
    }

    _ = c.yyjson_mut_obj_add_val(doc, root_obj, "changed_files", changed);
    _ = c.yyjson_mut_obj_add_int(doc, root_obj, "changed_count", file_count);
    _ = c.yyjson_mut_obj_add_val(doc, root_obj, "impacted_symbols", impacted);
    _ = c.yyjson_mut_obj_add_int(doc, root_obj, "depth", depth);
    return mcp.textResultOwned(mcp.docToStr(doc), is_error);
}

// ── Compact output ───────────────────────────────────────────────
// Opt-in format:"compact" for discovery tools. The JSON result is rendered as
// indented "key: value" lines; an array of flat objects becomes a table with
// one header of column names and one space-separated row per element, which
// is where most of the saving comes from. Strings are quoted (JSON escapes)
// whenever a bare word would be ambiguous, null prints as null and a cell an
// element lacks prints as "-", so the rendering is lossless. The compact
// result carries no structuredContent copy.

/// A growing malloc'd text buffer. Allocation failure drops the write, as the
/// C buffer did.
const CompactBuf = struct {
    buf: HeapStr = null,
    len: usize = 0,
    cap: usize = 0,

    fn put(b: *CompactBuf, s: []const u8) void {
        if (b.len + s.len + 1 > b.cap) {
            var cap = if (b.cap != 0) b.cap else 4096;
            while (b.len + s.len + 1 > cap) cap *= 2;
            const grown = interop.reallocN(u8, b.buf, cap);
            if (grown == null) return;
            b.buf = grown;
            b.cap = cap;
        }
        @memcpy(b.buf[b.len..][0..s.len], s);
        b.len += s.len;
        b.buf[b.len] = 0;
    }

    fn indent(b: *CompactBuf, depth: usize) void {
        for (0..depth) |_| b.put("  ");
    }
};

const Iter = struct {
    /// Elements of a yyjson array.
    fn arr(v: ?*mcp.Val) c.yyjson_arr_iter {
        var it: c.yyjson_arr_iter = undefined;
        _ = c.yyjson_arr_iter_init(v, &it);
        return it;
    }

    /// Keys of a yyjson object; the value is yyjson_obj_iter_get_val(key).
    fn obj(v: ?*mcp.Val) c.yyjson_obj_iter {
        var it: c.yyjson_obj_iter = undefined;
        _ = c.yyjson_obj_iter_init(v, &it);
        return it;
    }
};

fn compactBareOk(s_c: CStr) bool {
    const s = std.mem.span(s_c);
    if (s.len == 0 or eql(s, "-") or eql(s, "null") or eql(s, "true") or eql(s, "false")) return false;
    var end: [*c]u8 = null;
    _ = c.strtod(s_c, &end);
    if (end != null and end[0] == 0) return false; // would read back as a number
    for (s) |ch| {
        if (ch <= ' ' or ch == '"' or ch == 0x7f) return false;
    }
    return true;
}

fn compactScalar(b: *CompactBuf, v: ?*mcp.Val) void {
    if (c.yyjson_is_str(v) and compactBareOk(c.yyjson_get_str(v))) {
        b.put(std.mem.span(c.yyjson_get_str(v)));
        return;
    }
    const text = c.yyjson_val_write(v, 0, null);
    if (text != null) {
        b.put(std.mem.span(text));
        c.free(text);
    }
}

fn compactIsScalar(v: ?*mcp.Val) bool {
    return !c.yyjson_is_obj(v) and !c.yyjson_is_arr(v);
}

/// An array whose elements are all objects with only scalar values.
fn compactIsTable(arr: ?*mcp.Val) bool {
    if (c.yyjson_arr_size(arr) == 0) return false;
    var it = Iter.arr(arr);
    while (c.yyjson_arr_iter_next(&it)) |el| {
        if (!c.yyjson_is_obj(el)) return false;
        var kit = Iter.obj(el);
        while (c.yyjson_obj_iter_next(&kit)) |key| {
            if (!compactIsScalar(c.yyjson_obj_iter_get_val(key))) return false;
        }
    }
    return true;
}

fn compactTable(b: *CompactBuf, arr: ?*mcp.Val, depth: usize) void {
    const max_cols = 64;
    var cols: [max_cols]CStr = undefined;
    var ncols: usize = 0;
    var it = Iter.arr(arr);
    while (c.yyjson_arr_iter_next(&it)) |el| {
        var kit = Iter.obj(el);
        while (c.yyjson_obj_iter_next(&kit)) |key| {
            const name = c.yyjson_get_str(key);
            const seen = for (cols[0..ncols]) |col| {
                if (mcp.cEql(col, name)) break true;
            } else false;
            if (!seen and ncols < max_cols) {
                cols[ncols] = name;
                ncols += 1;
            }
        }
    }
    b.put("  (");
    for (cols[0..ncols], 0..) |col, i| {
        if (i > 0) b.put(" ");
        b.put(std.mem.span(col));
    }
    b.put(")\n");
    it = Iter.arr(arr);
    while (c.yyjson_arr_iter_next(&it)) |el| {
        b.indent(depth + 1);
        for (cols[0..ncols], 0..) |col, i| {
            if (i > 0) b.put(" ");
            const cell = c.yyjson_obj_get(el, col);
            if (cell != null) compactScalar(b, cell) else b.put("-");
        }
        b.put("\n");
    }
}

fn compactValue(b: *CompactBuf, key: CStr, v: ?*mcp.Val, depth: usize) void {
    b.indent(depth);
    if (key != null) b.put(std.mem.span(key));
    if (compactIsScalar(v)) {
        if (key != null) b.put(": ");
        compactScalar(b, v);
        b.put("\n");
        return;
    }
    if (c.yyjson_is_obj(v)) {
        b.put(if (key != null) ":\n" else "{}\n");
        var kit = Iter.obj(v);
        while (c.yyjson_obj_iter_next(&kit)) |k| {
            compactValue(b, c.yyjson_get_str(k), c.yyjson_obj_iter_get_val(k), depth + 1);
        }
        return;
    }
    var count: [32]u8 = undefined;
    b.put(mcp.bufStr(mcp.fmtZ(&count, "[{d}]", .{c.yyjson_arr_size(v)})[0..count.len]));
    if (compactIsTable(v)) {
        compactTable(b, v, depth);
        return;
    }
    var all_scalar = true;
    var it = Iter.arr(v);
    while (c.yyjson_arr_iter_next(&it)) |el| all_scalar = all_scalar and compactIsScalar(el);
    if (all_scalar) {
        b.put(":");
        it = Iter.arr(v);
        while (c.yyjson_arr_iter_next(&it)) |el| {
            b.put(" ");
            compactScalar(b, el);
        }
        b.put("\n");
        return;
    }
    b.put(":\n");
    it = Iter.arr(v);
    while (c.yyjson_arr_iter_next(&it)) |el| {
        if (c.yyjson_is_obj(el)) {
            b.indent(depth + 1);
            b.put("-\n");
            var kit = Iter.obj(el);
            while (c.yyjson_obj_iter_next(&kit)) |ck| {
                compactValue(b, c.yyjson_get_str(ck), c.yyjson_obj_iter_get_val(ck), depth + 2);
            }
        } else {
            compactValue(b, "-", el, depth + 1);
        }
    }
}

/// Render a JSON object as compact text. NULL when json is not an object.
pub export fn cbm_mcp_render_compact(json: [*c]const u8) [*c]u8 {
    const doc = mcp.readJson(json);
    defer c.yyjson_doc_free(doc);
    const root = if (doc) |d| c.yyjson_doc_get_root(d) else null;
    if (!c.yyjson_is_obj(root)) return null;
    var b: CompactBuf = .{};
    var kit = Iter.obj(root);
    while (c.yyjson_obj_iter_next(&kit)) |k| {
        compactValue(&b, c.yyjson_get_str(k), c.yyjson_obj_iter_get_val(k), 0);
    }
    return if (b.buf != null) b.buf else mcp.dup("");
}

/// Tools whose schema advertises format:"compact".
fn toolSupportsCompact(tool_name: CStr) bool {
    const names = [_]Str{ "search_graph", "query_graph", "trace_path", "trace_call_path", "search_code", "get_architecture", "list_projects", "check_index_coverage", "get_file_outline" };
    for (names) |name| {
        if (mcp.cEql(tool_name, name)) return true;
    }
    return false;
}

/// Replace a successful JSON tool result with its compact rendering. Results
/// that are errors or not JSON objects pass through unchanged.
fn compactToolResult(result: HeapStr) HeapStr {
    const doc = mcp.readJson(result);
    const root = if (doc) |d| c.yyjson_doc_get_root(d) else null;
    const content = c.yyjson_arr_get_first(c.yyjson_obj_get(root, "content"));
    const text = c.yyjson_get_str(c.yyjson_obj_get(content, "text"));
    const compact = if (c.yyjson_get_bool(c.yyjson_obj_get(root, "isError")) or text == null) null else cbm_mcp_render_compact(text);
    c.yyjson_doc_free(doc);
    if (compact == null) return result;
    defer c.free(compact);

    const mdoc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(mdoc);
    const mroot = c.yyjson_mut_obj(mdoc);
    c.yyjson_mut_doc_set_root(mdoc, mroot);
    const items = c.yyjson_mut_arr(mdoc);
    const item = c.yyjson_mut_obj(mdoc);
    _ = c.yyjson_mut_obj_add_str(mdoc, item, "type", "text");
    _ = c.yyjson_mut_obj_add_str(mdoc, item, "text", compact);
    _ = c.yyjson_mut_arr_add_val(items, item);
    _ = c.yyjson_mut_obj_add_val(mdoc, mroot, "content", items);
    _ = c.yyjson_mut_obj_add_bool(mdoc, mroot, "isError", false);
    const out = mcp.docToStr(mdoc);
    c.free(result);
    return out;
}

// ── Tool dispatch ────────────────────────────────────────────────

pub export fn cbm_mcp_handle_tool(srv_c: ?*c.cbm_mcp_server_t, tool_name: [*c]const u8, args_json: [*c]const u8) [*c]u8 {
    if (tool_name == null) return textResult("missing tool name", true);
    const srv = mcp.srvOf(srv_c).?;
    var compact = false;
    const format = if (args_json != null) mcp.strArg(args_json, "format") else null;
    defer c.free(format);
    if (format != null and toolSupportsCompact(tool_name)) {
        if (mcp.cEql(format, "compact")) {
            compact = true;
        } else if (!mcp.cEql(format, "json")) {
            return textResult("format must be \"json\" or \"compact\"", true);
        }
    }
    const result = dispatchTool(srv, tool_name, args_json);
    return if (compact) compactToolResult(result) else result;
}

const Handler = *const fn (*Server, CStr) HeapStr;

const tool_handlers = [_]struct { name: Str, handler: Handler }{
    .{ .name = "list_projects", .handler = mcp.handleListProjects },
    .{ .name = "compare_graphs", .handler = graph_tools.handleCompareGraphs },
    .{ .name = "get_graph_schema", .handler = graph_tools.handleGetGraphSchema },
    .{ .name = "search_graph", .handler = graph_tools.handleSearchGraph },
    .{ .name = "query_graph", .handler = graph_tools.handleQueryGraph },
    .{ .name = "index_status", .handler = project_tools.handleIndexStatus },
    .{ .name = "check_index_coverage", .handler = project_tools.handleCheckIndexCoverage },
    .{ .name = "delete_project", .handler = project_tools.handleDeleteProject },
    .{ .name = "trace_path", .handler = code_tools.handleTraceCallPath },
    .{ .name = "trace_call_path", .handler = code_tools.handleTraceCallPath },
    .{ .name = "get_architecture", .handler = project_tools.handleGetArchitecture },
    // Pipeline-dependent tools
    .{ .name = "index_repository", .handler = index_tools.handleIndexRepository },
    .{ .name = "get_code_snippet", .handler = code_tools.handleGetCodeSnippet },
    .{ .name = "get_file_outline", .handler = code_tools.handleGetFileOutline },
    .{ .name = "search_code", .handler = search_tools.handleSearchCode },
    .{ .name = "detect_changes", .handler = handleDetectChanges },
};

fn dispatchTool(srv: *Server, tool_name: CStr, args_json: CStr) HeapStr {
    for (tool_handlers) |t| {
        if (mcp.cEql(tool_name, t.name)) return t.handler(srv, args_json);
    }
    var msg: [256]u8 = undefined;
    return textResult(mcp.fmtZ(&msg, "unknown tool: {s}", .{sp(tool_name)}), true);
}

// ── Session detection + auto-index ───────────────────────────────

extern fn _getcwd(buf: [*]u8, size: c_int) ?[*:0]u8;

fn getCwd(buf: []u8) bool {
    if (is_windows) return _getcwd(buf.ptr, @intCast(buf.len)) != null;
    return c.getcwd(buf.ptr, buf.len) != null;
}

/// Detect session root from CWD.
fn detectSession(srv: *Server) void {
    if (srv.session_detected) return;
    srv.session_detected = true;

    var cwd: [1024]u8 = undefined;
    if (getCwd(&cwd)) {
        const home = c.cbm_get_home_dir();
        // Skip useless roots: / and $HOME
        if (!mcp.cEql(&cwd, "/") and (home == null or !mcp.cEql(&cwd, home))) {
            _ = mcp.bprint(&srv.session_root, "{s}", .{mcp.bufStr(&cwd)});
            interop.logInfo("session.root.cwd", .{ "path", @as(CStr, &cwd) });
        }
    }

    // Derive project name from path — must match cbm_project_name_from_path
    // used by the pipeline, otherwise session queries look for a project
    // that doesn't match the indexed project name.
    if (srv.session_root[0] != 0) {
        const pname = c.cbm_project_name_from_path(&srv.session_root);
        if (pname != null) {
            _ = mcp.bprint(&srv.session_project, "{s}", .{sp(pname)});
            c.free(pname);
        }
    }
}

/// auto_watch config: gates background watcher registration (default on).
/// Multi-project users can contain a session to its own project with
/// `config set auto_watch false`.
fn autoWatchEnabled(srv: *Server) bool {
    if (srv.config == null) return true;
    return c.cbm_config_get_bool(srv.config, c.CBM_CONFIG_AUTO_WATCH, true);
}

/// Register the session project with the background watcher for ongoing
/// change detection — unless auto_watch is disabled.
fn registerWatcherIfEnabled(srv: *Server) void {
    if (srv.watcher == null or srv.session_project[0] == 0 or srv.session_root[0] == 0) return;
    if (!autoWatchEnabled(srv)) {
        interop.logInfo("watcher.register.skipped", .{ "reason", "auto_watch_off", "project", @as(CStr, &srv.session_project) });
        return;
    }
    c.cbm_watcher_watch(srv.watcher, &srv.session_project, &srv.session_root);
}

/// Background auto-index thread function.
fn autoindexThread(arg: ?*anyopaque) callconv(.c) ?*anyopaque {
    const srv: *Server = @ptrCast(@alignCast(arg));
    interop.logInfo("autoindex.start", .{ "project", @as(CStr, &srv.session_project), "path", @as(CStr, &srv.session_root) });

    // Background work does not wait: if another process is already indexing
    // this workspace, its result is the one we would produce.
    var hold: c.cbm_zova_index_hold_t = undefined;
    const hold_status = index_tools.cbm_mcp_index_hold_acquire(&srv.session_root, 0, &hold, null, 0);
    if (hold_status != c.CBM_ZOVA_COORD_OK) {
        interop.logInfo("autoindex.skip", .{ "reason", index_tools.cbm_mcp_index_hold_refusal_reason(hold_status, &hold), "project", @as(CStr, &srv.session_project) });
        if (hold_status == c.CBM_ZOVA_COORD_BUSY) registerWatcherIfEnabled(srv);
        return null;
    }

    // #832: prefer the supervised worker subprocess. Indexing the whole
    // session in this long-lived server thread ratchets RSS (mimalloc v3 does
    // not reclaim the pages worker threads abandon at exit); running it in a
    // child that exits hands 100% of that memory back to the OS every cycle.
    // Degrade to the in-process pipeline below when the supervisor is off
    // (kill switch) or the spawn fails.
    if (c.cbm_index_supervisor_should_wrap()) {
        const resp = index_tools.indexRunSupervisedPath(srv, &srv.session_root);
        if (resp != null) {
            c.free(resp);
            interop.logInfo("autoindex.done", .{ "project", @as(CStr, &srv.session_project), "mode", "supervised" });
            // Register with watcher — gated on auto_watch (#849), same as the
            // in-process branch below.
            c.cbm_zova_index_hold_release(&hold);
            registerWatcherIfEnabled(srv);
            return null;
        }
        // resp == NULL → spawn-failure degrade → fall through to in-process.
    }

    const p = c.cbm_pipeline_new(&srv.session_root, null, c.CBM_MODE_FULL);
    if (p == null) {
        c.cbm_zova_index_hold_release(&hold);
        interop.logWarn("autoindex.err", .{ "msg", "pipeline_create_failed" });
        return null;
    }
    const rc = c.cbm_pipeline_run(p);
    c.cbm_zova_index_hold_release(&hold);
    c.cbm_pipeline_free(p);
    c.cbm_mem_collect(); // return mimalloc pages to OS after indexing (in-process only)

    if (rc == 0) {
        interop.logInfo("autoindex.done", .{ "project", @as(CStr, &srv.session_project) });
        registerWatcherIfEnabled(srv);
    } else {
        interop.logWarn("autoindex.err", .{ "msg", "pipeline_run_failed" });
    }
    return null;
}

/// Default file limit for auto-indexing new projects.
const DEFAULT_AUTO_INDEX_LIMIT = 50000;

/// Start auto-indexing if configured and project not yet indexed.
fn maybeAutoIndex(srv: *Server) void {
    if (srv.session_root[0] == 0) return; // no session root detected

    // Check if the project is already indexed in the shared cbm.zova
    var zova_path: [1024]u8 = undefined;
    const repo = if (c.cbm_zova_user_database_path(&zova_path, zova_path.len) == 0)
        c.cbm_zova_repository_open(&zova_path, &srv.session_project)
    else
        null;
    if (repo != null) {
        c.cbm_zova_repository_close(repo);
        // Already indexed → register watcher for change detection
        interop.logInfo("autoindex.skip", .{ "reason", "already_indexed", "project", @as(CStr, &srv.session_project) });
        registerWatcherIfEnabled(srv);
        return;
    }

    var auto_index = false;
    var file_limit: c_int = DEFAULT_AUTO_INDEX_LIMIT;
    if (srv.config != null) {
        auto_index = c.cbm_config_get_bool(srv.config, c.CBM_CONFIG_AUTO_INDEX, false);
        file_limit = c.cbm_config_get_int(srv.config, c.CBM_CONFIG_AUTO_INDEX_LIMIT, DEFAULT_AUTO_INDEX_LIMIT);
    }
    if (!auto_index) {
        interop.logInfo("autoindex.skip", .{ "reason", "disabled", "hint", "run: codebase-memory-mcp config set auto_index true" });
        return;
    }

    // Quick file count check to avoid OOM on massive repos
    if (!c.cbm_validate_shell_arg(&srv.session_root)) {
        interop.logWarn("autoindex.skip", .{ "reason", "path contains shell metacharacters" });
        return;
    }
    var cmd: [1024]u8 = undefined;
    const fp = c.cbm_popen_git(mcp.fmtZ(&cmd, "git -C '{s}' ls-files 2>/dev/null | wc -l", .{mcp.bufStr(&srv.session_root)}));
    if (fp != null) {
        var line: [64]u8 = undefined;
        if (c.fgets(&line, line.len, fp) != null) {
            const count = parseLeadingInt(mcp.bufStr(&line));
            if (count > file_limit) {
                interop.logWarn("autoindex.skip", .{ "reason", "too_many_files", "files", @as(CStr, &line), "limit", c.CBM_CONFIG_AUTO_INDEX_LIMIT });
                _ = c.cbm_pclose(fp);
                return;
            }
        }
        _ = c.cbm_pclose(fp);
    }

    // Launch auto-index in background
    if (c.cbm_thread_create(&srv.autoindex_tid, 0, autoindexThread, srv) == 0) {
        srv.autoindex_active = true;
    }
}

// ── Background update check ──────────────────────────────────────

const UPDATE_CHECK_URL = "https://api.github.com/repos/octacity-org/codebase-memory-mcp/releases/latest";

fn updateCheckThread(arg: ?*anyopaque) callconv(.c) ?*anyopaque {
    const srv: *Server = @ptrCast(@alignCast(arg));
    defer srv.update_checked = true;

    // Use curl with 5s timeout to fetch latest release tag
    const fp = c.cbm_popen("curl -sf --max-time 5 -H 'Accept: application/vnd.github+json' " ++
        "'" ++ UPDATE_CHECK_URL ++ "' 2>/dev/null", "r");
    if (fp == null) return null;

    var buf: [4096]u8 = undefined;
    var total: usize = 0;
    while (total < buf.len - 1) {
        const n = c.fread(&buf[total], 1, buf.len - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    buf[total] = 0;
    _ = c.cbm_pclose(fp);

    // Parse tag_name from JSON response
    const doc = c.yyjson_read(&buf, total, 0) orelse return null;
    defer c.yyjson_doc_free(doc);
    const tag_str = c.yyjson_get_str(c.yyjson_obj_get(c.yyjson_doc_get_root(doc), "tag_name"));
    if (tag_str != null) {
        const current = c.cbm_cli_get_version();
        if (c.cbm_compare_versions(tag_str, current) > 0) {
            _ = mcp.bprint(&srv.update_notice, "Update available: {s} -> {s} -- run: codebase-memory-mcp update  |  " ++
                "Enjoying codebase-memory-mcp? Please leave a star: " ++
                "https://github.com/DeusData/codebase-memory-mcp", .{ sp(current), sp(tag_str) });
            interop.logInfo("update.available", .{ "current", current, "latest", tag_str });
        }
    }
    return null;
}

fn startUpdateCheck(srv: *Server) void {
    if (srv.update_checked) return;
    srv.update_checked = true; // prevent double-launch
    if (c.cbm_thread_create(&srv.update_tid, 0, updateCheckThread, srv) == 0) {
        srv.update_thread_active = true;
    }
}

/// Prepend update notice to a tool result, then clear it (one-shot).
fn injectUpdateNotice(srv: *Server, result_json: HeapStr) HeapStr {
    if (srv.update_notice[0] == 0) return result_json;

    // Parse existing result, prepend notice text, rebuild
    const doc = mcp.readJson(result_json) orelse return result_json;
    const mdoc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(mdoc);
    const root = c.yyjson_val_mut_copy(mdoc, c.yyjson_doc_get_root(doc));
    c.yyjson_doc_free(doc);
    if (root == null) return result_json;
    c.yyjson_mut_doc_set_root(mdoc, root);

    // Find the "content" array
    const content = c.yyjson_mut_obj_get(root, "content");
    if (content != null and c.yyjson_mut_is_arr(content)) {
        // Prepend a text content item with the update notice
        const notice_item = c.yyjson_mut_obj(mdoc);
        _ = c.yyjson_mut_obj_add_str(mdoc, notice_item, "type", "text");
        _ = c.yyjson_mut_obj_add_str(mdoc, notice_item, "text", &srv.update_notice);
        _ = c.yyjson_mut_arr_prepend(content, notice_item);
    }

    const new_json = mcp.docToStr(mdoc);
    if (new_json == null) return result_json;
    c.free(result_json);
    srv.update_notice[0] = 0; // clear — one-shot
    return new_json;
}

// ── Server request handler ───────────────────────────────────────

/// Microseconds elapsed since t0_ns (cbm_now_ns, monotonic).
fn elapsedUs(t0_ns: u64) c_longlong {
    return @intCast((c.cbm_now_ns() -% t0_ns) / 1000);
}

pub export fn cbm_mcp_server_handle(srv_c: ?*c.cbm_mcp_server_t, line: [*c]const u8) [*c]u8 {
    const srv = mcp.srvOf(srv_c).?;
    var req = std.mem.zeroes(c.cbm_jsonrpc_request_t);
    if (mcp.cbm_jsonrpc_parse(line, &req) < 0) {
        return mcp.cbm_jsonrpc_format_error(0, mcp.JSONRPC_PARSE_ERROR, "Parse error");
    }
    defer mcp.cbm_jsonrpc_request_free(&req);
    const method = req.method;
    const is = struct {
        fn f(m: CStr, name: Str) bool {
            return m != null and mcp.cEql(m, name);
        }
    }.f;

    // Notifications (no id) → handle cancellation, then no response
    if (!req.has_id) {
        if (is(method, "notifications/cancelled")) {
            if (srv.active_pipeline != null and
                mcp.cbm_mcp_cancel_request_matches(req.params_raw, srv.active_request_id, srv.active_request_id_str))
            {
                c.cbm_pipeline_cancel(srv.active_pipeline);
                interop.logInfo("mcp.cancelled", .{ "match", "true" });
            }
        }
        return null;
    }

    const req_t0 = c.cbm_now_ns();
    var result_json: HeapStr = null;
    var request_logged = false;

    if (is(method, "initialize")) {
        result_json = mcp.cbm_mcp_initialize_response(req.params_raw);
        startUpdateCheck(srv);
        detectSession(srv);
        maybeAutoIndex(srv);
    } else if (is(method, "ping")) {
        result_json = mcp.dup("{}");
    } else if (is(method, "tools/list")) {
        result_json = mcp.toolsListPage(req.params_raw);
    } else if (is(method, "tools/call")) {
        const tool_name = if (req.params_raw != null) mcp.cbm_mcp_get_tool_name(req.params_raw) else null;
        defer c.free(tool_name);
        const tool_args = if (req.params_raw != null) mcp.cbm_mcp_get_arguments(req.params_raw) else mcp.dup("{}");
        defer c.free(tool_args);
        srv.active_request_id = req.id;
        c.free(srv.active_request_id_str);
        srv.active_request_id_str = if (req.id_str != null) mcp.dup(req.id_str) else null;

        const t0 = c.cbm_now_ns();
        result_json = cbm_mcp_handle_tool(srv_c, tool_name, tool_args);
        srv.active_request_id = mcp.CBM_NOT_FOUND;
        c.free(srv.active_request_id_str);
        srv.active_request_id_str = null;
        const is_err = result_json != null and c.strstr(result_json, "\"isError\":true") != null;
        c.cbm_diag_record_query(elapsedUs(t0), is_err);
        c.cbm_log_mcp_request(method, tool_name, is_err, elapsedUs(req_t0));
        request_logged = true;

        result_json = injectUpdateNotice(srv, result_json);
    } else {
        // Echo the original id (string or numeric, issue #253) on the error.
        var err_obj: [160]u8 = undefined;
        var err_resp = std.mem.zeroes(c.cbm_jsonrpc_response_t);
        err_resp.id = req.id;
        err_resp.id_str = req.id_str;
        err_resp.error_json = mcp.fmtZ(&err_obj, "{{\"code\":{d},\"message\":\"Method not found\"}}", .{mcp.JSONRPC_METHOD_NOT_FOUND});
        const err = mcp.cbm_jsonrpc_format_response(&err_resp);
        c.cbm_log_mcp_request(method, null, true, elapsedUs(req_t0));
        return err;
    }

    if (!request_logged) c.cbm_log_mcp_request(method, null, false, elapsedUs(req_t0));

    var resp = std.mem.zeroes(c.cbm_jsonrpc_response_t);
    resp.id = req.id;
    resp.id_str = req.id_str;
    resp.result_json = result_json;
    const out = mcp.cbm_jsonrpc_format_response(&resp);
    c.free(result_json);
    return out;
}

/// Write one response, framed or newline-delimited, and flush.
fn writeResponse(out: ?*c.FILE, resp: HeapStr, framed: bool) void {
    const body = std.mem.span(resp);
    if (framed) {
        var header: [64]u8 = undefined;
        _ = c.fputs(mcp.fmtZ(&header, "Content-Length: {d}\r\n\r\n", .{body.len}), out);
        _ = c.fwrite(body.ptr, 1, body.len, out);
    } else {
        _ = c.fwrite(body.ptr, 1, body.len, out);
        _ = c.fputc('\n', out);
    }
    _ = c.fflush(out);
}

/// Handle a Content-Length-framed message (LSP-style transport).
/// Reads headers, body, processes request, writes framed response.
fn handleContentLengthFrame(srv_c: ?*c.cbm_mcp_server_t, in: ?*c.FILE, out: ?*c.FILE, line: *[*c]u8, cap: *usize, content_len: usize) void {
    // Skip blank line(s) between header and body
    while (c.cbm_getline(line, cap, in) > 0) {
        if (trimLineEnd(std.mem.span(line.*)).len == 0) break;
    }

    const body = interop.mallocN(u8, content_len + 1);
    if (body == null) return;
    const nread = c.fread(body, 1, content_len, in);
    body[nread] = 0;

    const resp = cbm_mcp_server_handle(srv_c, body);
    c.free(body);
    if (resp != null) {
        writeResponse(out, resp, true);
        c.free(resp);
    }
}

const PollResult = enum { ready, timeout, eof };

/// Unix 3-phase poll: non-blocking fd check, FILE* buffer peek, blocking
/// poll. A timeout evicts idle stores.
fn pollForInputUnix(srv_c: ?*c.cbm_mcp_server_t, fd: c_int, in: ?*c.FILE) PollResult {
    var pfd = std.mem.zeroes(c.struct_pollfd);
    pfd.fd = fd;
    pfd.events = c.POLLIN;
    var pr = c.poll(&pfd, 1, 0); // Phase 1: non-blocking
    if (pr < 0) return .eof;
    if (pr > 0) return .ready;

    // Phase 2: peek FILE* buffer
    const saved_flags = c.fcntl(fd, c.F_GETFL);
    if (saved_flags >= 0) {
        _ = c.fcntl(fd, c.F_SETFL, saved_flags | c.O_NONBLOCK);
        const ch = c.fgetc(in);
        _ = c.fcntl(fd, c.F_SETFL, saved_flags);
        if (ch != c.EOF) {
            _ = c.ungetc(ch, in);
            return .ready;
        }
        if (c.feof(in) != 0) return .eof; // true EOF
        c.clearerr(in);
    }
    // Phase 3 (or fcntl failed): blocking poll, bounded to a SHORT interval
    // (not the full idle timeout). macOS poll()/select() do NOT report
    // POLLIN/POLLHUP when a FIFO's last writer closes — only read() returns 0
    // there (verified). A 60s poll would therefore leave the server blocked up
    // to a full idle timeout after stdin EOF (a client that closes the pipe
    // would appear to hang). Waking every MCP_TIMEOUT_MS lets the Phase-2
    // read() above detect the EOF within ~1s. Idle-store eviction (threshold
    // STORE_IDLE_TIMEOUT_S) is idempotent, so checking it on each short tick
    // is harmless.
    pr = c.poll(&pfd, 1, MCP_TIMEOUT_MS);
    if (pr < 0) return .eof;
    if (pr == 0) {
        mcp.cbm_mcp_server_evict_idle(srv_c, STORE_IDLE_TIMEOUT_S);
        return .timeout;
    }
    return .ready;
}

extern fn _get_osfhandle(fd: c_int) isize;
extern "kernel32" fn WaitForSingleObject(handle: ?*anyopaque, ms: u32) callconv(.winapi) u32;
const WAIT_TIMEOUT: u32 = 0x102;
const WAIT_FAILED: u32 = 0xFFFFFFFF;

/// Windows: WaitForSingleObject on the stdin handle.
fn pollForInputWindows(srv_c: ?*c.cbm_mcp_server_t, fd: c_int) PollResult {
    const handle: ?*anyopaque = @ptrFromInt(@as(usize, @bitCast(_get_osfhandle(fd))));
    const wr = WaitForSingleObject(handle, STORE_IDLE_TIMEOUT_S * MCP_TIMEOUT_MS);
    if (wr == WAIT_FAILED) return .eof;
    if (wr == WAIT_TIMEOUT) {
        mcp.cbm_mcp_server_evict_idle(srv_c, STORE_IDLE_TIMEOUT_S);
        return .timeout;
    }
    return .ready;
}

// ── Event loop ───────────────────────────────────────────────────

pub export fn cbm_mcp_server_run(srv_c: ?*c.cbm_mcp_server_t, in: ?*c.FILE, out: ?*c.FILE) c_int {
    var line: [*c]u8 = null;
    defer c.free(line);
    var cap: usize = 0;
    const fd = c.cbm_fileno(in);

    while (true) {
        // Poll with idle timeout so we can evict unused stores between
        // requests.
        //
        // IMPORTANT: poll() operates on the raw fd, but getline() reads from a
        // buffered FILE*. When a client sends multiple messages in rapid
        // succession, the first getline() call may drain ALL kernel data into
        // libc's internal FILE* buffer. Subsequent poll() calls then see an
        // empty kernel fd and block for STORE_IDLE_TIMEOUT_S seconds even
        // though the next messages are already in the FILE* buffer.
        //
        // Fix (Unix): use a three-phase approach —
        //   Phase 1: non-blocking poll (timeout=0) to check the kernel fd.
        //   Phase 2: if Phase 1 returns 0, peek the FILE* buffer via fgetc/
        //            ungetc to detect data buffered by a prior getline() call.
        //            The fd is temporarily set O_NONBLOCK so fgetc() returns
        //            immediately (EAGAIN → EOF + ferror) instead of blocking
        //            when the FILE* buffer is empty, which would otherwise
        //            bypass the Phase 3 idle eviction timeout.
        //   Phase 3: only if both phases confirm no data, do blocking poll.
        const pr = if (is_windows) pollForInputWindows(srv_c, fd) else pollForInputUnix(srv_c, fd, in);
        switch (pr) {
            .eof => break,
            .timeout => continue, // idle stores evicted
            .ready => {},
        }

        if (c.cbm_getline(&line, &cap, in) <= 0) break;

        // Trim trailing newline/CR
        const len = trimLineEnd(std.mem.span(line)).len;
        line[len] = 0;
        if (len == 0) continue;

        // Content-Length framing (LSP-style transport)
        const prefix = "Content-Length:";
        if (std.mem.startsWith(u8, line[0..len], prefix)) {
            const content_len = parseLeadingInt(line[prefix.len..len]);
            if (content_len > 0 and content_len <= MCP_MAX_FRAME_BYTES) {
                handleContentLengthFrame(srv_c, in, out, &line, &cap, @intCast(content_len));
            }
            continue;
        }

        const resp = cbm_mcp_server_handle(srv_c, line);
        if (resp != null) {
            writeResponse(out, resp, false);
            c.free(resp);
        }
    }
    return 0;
}

// ── cbm_parse_file_uri ───────────────────────────────────────────

pub export fn cbm_parse_file_uri(uri: [*c]const u8, out_path: [*c]u8, out_size: c_int) bool {
    if (uri == null or out_path == null or out_size <= 0) {
        if (out_path != null and out_size > 0) out_path[0] = 0;
        return false;
    }
    const out = out_path[0..@intCast(out_size)];

    // Must start with file://
    const prefix = "file://";
    const u = std.mem.span(uri);
    if (!std.mem.startsWith(u8, u, prefix)) {
        out[0] = 0;
        return false;
    }
    var path = u[prefix.len..];

    // On Windows, file:///C:/path → /C:/path. Strip leading / before drive letter.
    if (path.len >= 3 and path[0] == '/' and std.ascii.isAlphabetic(path[1]) and path[2] == ':') path = path[1..];

    _ = mcp.bprint(out, "{s}", .{path});
    return true;
}
