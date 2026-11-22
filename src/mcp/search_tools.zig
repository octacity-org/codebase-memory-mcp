//! search_tools.zig — the search_code MCP tool: graph-augmented code search.
//! The indexed file list is scanned in process; an unindexed project (and a
//! Windows file_pattern search) falls back to a supervised grep / PowerShell
//! scan. Hits are grouped into their tightest containing graph node, ranked,
//! and returned with optional source or context.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const mcp = @import("mcp.zig");
const code_tools = @import("code_tools.zig");

const CStr = mcp.CStr;
const HeapStr = mcp.HeapStr;
const Str = mcp.Str;
const Server = mcp.Server;
const MutDoc = mcp.MutDoc;
const MutVal = mcp.MutVal;
const textResult = mcp.textResult;
const sp = mcp.sp;
const eql = mcp.eql;
const is_windows = mcp.is_windows;

const gpa = std.heap.c_allocator;

const MCP_DEFAULT_LIMIT = 10;
/// search_code scan bounds (upstream 31bdd4d6): a pathological pattern or a
/// huge indexed tree must not pin the request forever or fill the scratch disk.
const MCP_SEARCH_SCAN_TIMEOUT_MS = 30000;
const MCP_SEARCH_OUTPUT_MAX = 64 * 1024 * 1024;
/// No grep-level match limit — find all matches up to this, then dedup and
/// cap in our code.
const GREP_MAX_MATCHES = 500;
const MAX_MATCH_LINES = 64;

/// Replace non-ASCII bytes with '?' to guarantee valid UTF-8 JSON output.
fn sanitizeAscii(s: [*c]u8) void {
    var p = s;
    while (p[0] != 0) : (p += 1) {
        if (p[0] > 127) p[0] = '?';
    }
}

/// Intermediate grep match.
const GrepMatch = struct {
    file: [512]u8,
    line: c_int,
    content: [1024]u8,

    fn fileStr(self: *const GrepMatch) []const u8 {
        return mcp.bufStr(&self.file);
    }
};

/// Deduped result: one per containing graph node.
const SearchResult = struct {
    node_id: i64 = 0,
    node_name: [256]u8 = std.mem.zeroes([256]u8),
    qualified_name: [512]u8 = std.mem.zeroes([512]u8),
    label: [64]u8 = std.mem.zeroes([64]u8),
    file: [512]u8 = std.mem.zeroes([512]u8),
    start_line: c_int = 0,
    end_line: c_int = 0,
    in_degree: c_int = 0,
    out_degree: c_int = 0,
    score: c_int = 0,
    match_lines: [MAX_MATCH_LINES]c_int = undefined,
    match_count: usize = 0,
};

/// Score a result for ranking: project source first, vendored last, tests lowest.
fn computeSearchScore(r: *const SearchResult) c_int {
    const label = mcp.bufStr(&r.label);
    const file = mcp.bufStr(&r.file);
    var score = r.in_degree;
    if (eql(label, "Function") or eql(label, "Method")) score += 10;
    if (eql(label, "Route")) score += 15;
    if (std.mem.indexOf(u8, file, "vendored/") != null or std.mem.indexOf(u8, file, "vendor/") != null or
        std.mem.indexOf(u8, file, "node_modules/") != null) score -= 50;
    if (std.mem.indexOf(u8, file, "test") != null or std.mem.indexOf(u8, file, "spec") != null) score -= 5;
    return score;
}

/// The grep/search command. On Windows, the PowerShell Select-String script
/// (tab-delimited output), run as `powershell -Command <script>`. It holds no
/// double quote, so it stays one argument through cbm_build_win_cmdline. On
/// POSIX, a grep sh command with colon-delimited output.
fn buildGrepCmd(cmd: []u8, use_regex: bool, scoped: bool, file_pattern: CStr, tmpfile: []const u8, filelist: []const u8, root_path: []const u8) void {
    if (is_windows) {
        const sm = if (use_regex) "" else " -SimpleMatch";
        // Literal PowerShell braces are doubled for the format string.
        const each = " | ForEach-Object {{ $_.Path + [char]9 + $_.LineNumber + [char]9 + $_.Line }}";
        if (scoped) {
            const head = "$pat = Get-Content '{s}'; Get-Content '{s}' | ForEach-Object {{ Select-String -LiteralPath $_ -Pattern $pat{s} -ErrorAction SilentlyContinue }}";
            if (file_pattern != null) {
                _ = mcp.bprint(cmd, head ++ " | Where-Object {{ $_.Path -like '*{s}' }}" ++ each, .{ tmpfile, filelist, sm, sp(file_pattern) });
            } else {
                _ = mcp.bprint(cmd, head ++ each, .{ tmpfile, filelist, sm });
            }
        } else {
            const tail = " | Select-String -Pattern (Get-Content '{s}'){s} -ErrorAction SilentlyContinue" ++ each;
            if (file_pattern != null) {
                _ = mcp.bprint(cmd, "Get-ChildItem -Recurse -Path '{s}\\*' -Include '{s}' -File -ErrorAction SilentlyContinue" ++ tail, .{ root_path, sp(file_pattern), tmpfile, sm });
            } else {
                _ = mcp.bprint(cmd, "Get-ChildItem -Recurse -Path '{s}\\*' -File -ErrorAction SilentlyContinue" ++ tail, .{ root_path, tmpfile, sm });
            }
        }
        return;
    }
    const flag = if (use_regex) "-E" else "-F";
    if (scoped) {
        // -0: read NUL-separated paths from the filelist so paths containing
        // spaces stay one argument (issue #687). file_pattern was already
        // applied to that list (upstream 7a1e069b): BusyBox grep — the static
        // Alpine/musl runtime — has no GNU --include option.
        _ = mcp.bprint(cmd, "xargs -0 grep -Hn {s} -f '{s}' < '{s}' 2>/dev/null", .{ flag, tmpfile, filelist });
    } else if (file_pattern != null) {
        _ = mcp.bprint(cmd, "grep -rn {s} --include='{s}' -f '{s}' '{s}' 2>/dev/null", .{ flag, sp(file_pattern), tmpfile, root_path });
    } else {
        _ = mcp.bprint(cmd, "grep -rn {s} -f '{s}' '{s}' 2>/dev/null", .{ flag, tmpfile, root_path });
    }
}

/// Deduplicated file list from search results + raw matches.
fn buildDedupFilesArray(doc: ?*MutDoc, sr: []const SearchResult, raw: []const GrepMatch) ?*MutVal {
    const max_seen = 512;
    const files_arr = c.yyjson_mut_arr(doc);
    var seen: [max_seen][]const u8 = undefined;
    var seen_count: usize = 0;
    const Add = struct {
        fn add(d: ?*MutDoc, arr: ?*MutVal, s: *[max_seen][]const u8, n: *usize, file: [*c]const u8) void {
            const f = std.mem.span(file);
            for (s[0..n.*]) |x| {
                if (eql(x, f)) return;
            }
            if (n.* >= max_seen) return;
            s[n.*] = f;
            n.* += 1;
            _ = c.yyjson_mut_arr_add_str(d, arr, file);
        }
    };
    for (sr) |*r| Add.add(doc, files_arr, &seen, &seen_count, &r.file);
    for (raw) |*r| {
        if (seen_count >= max_seen) break;
        Add.add(doc, files_arr, &seen, &seen_count, &r.file);
    }
    return files_arr;
}

const SearchMode = enum(c_int) { compact = 0, full = 1, files = 2 };

/// Attach source or context lines to a search result JSON item.
fn attachResultSource(doc: ?*MutDoc, item: ?*MutVal, r: *const SearchResult, mode: SearchMode, context_lines: c_int, root_path: CStr) void {
    if (r.start_line <= 0 or r.end_line <= 0) return;
    var abs_path: [1024]u8 = undefined;
    _ = mcp.bprint(&abs_path, "{s}/{s}", .{ sp(root_path), mcp.bufStr(&r.file) });

    // Containment: a search result whose indexed path resolves outside the
    // project root (a `..` segment, or a symlink/junction that discovery
    // followed) must not be read back into the response.
    if (!mcp.pathWithinRoot(root_path, &abs_path)) return;

    if (mode == .full) {
        const source = code_tools.readFileLines(&abs_path, r.start_line, r.end_line);
        if (source != null) {
            sanitizeAscii(source);
            _ = c.yyjson_mut_obj_add_strcpy(doc, item, "source", source);
            c.free(source);
        }
    } else if (context_lines > 0 and r.match_count > 0) {
        const ctx_start = @max(r.match_lines[0] - context_lines, 1);
        const ctx_end = r.match_lines[r.match_count - 1] + context_lines;
        const ctx = code_tools.readFileLines(&abs_path, ctx_start, ctx_end);
        if (ctx != null) {
            sanitizeAscii(ctx);
            _ = c.yyjson_mut_obj_add_strcpy(doc, item, "context", ctx);
            _ = c.yyjson_mut_obj_add_int(doc, item, "context_start", ctx_start);
            c.free(ctx);
        }
    }
}

/// Directory distribution object from search results (top-level dir → count).
fn buildDirDistribution(doc: ?*MutDoc, sr: []const SearchResult) ?*MutVal {
    const max_dirs = 64;
    const dirs = c.yyjson_mut_obj(doc);
    var names: [max_dirs][128]u8 = undefined;
    var counts: [max_dirs]c_int = undefined;
    var dir_n: usize = 0;
    for (sr) |*r| {
        const file = mcp.bufStr(&r.file);
        var top: [128]u8 = undefined;
        const top_s = if (std.mem.indexOfScalar(u8, file, '/')) |slash| file[0..@min(slash + 1, top.len - 1)] else file;
        _ = mcp.bprint(&top, "{s}", .{top_s});
        const found = for (names[0..dir_n], 0..) |*n, d| {
            if (eql(mcp.bufStr(n), mcp.bufStr(&top))) break d;
        } else null;
        if (found) |d| {
            counts[d] += 1;
        } else if (dir_n < max_dirs) {
            names[dir_n] = top;
            counts[dir_n] = 1;
            dir_n += 1;
        }
    }
    for (names[0..dir_n], counts[0..dir_n]) |*n, cnt| {
        _ = c.yyjson_mut_obj_add(dirs, c.yyjson_mut_strcpy(doc, n), c.yyjson_mut_int(doc, cnt));
    }
    return dirs;
}

/// Phase 4: assemble JSON output from search results.
fn assembleSearchOutput(sr: []const SearchResult, raw: []const GrepMatch, gm_count: usize, limit: c_int, mode: SearchMode, context_lines: c_int, root_path: CStr, warn_literal_pipe: bool, elapsed_ms: u64) HeapStr {
    const search_slow_ms = 5000;
    const doc = c.yyjson_mut_doc_new(null);
    const root_obj = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root_obj);

    const output = sr[0..@min(sr.len, @as(usize, @intCast(@max(limit, 0))))];
    if (mode == .files) {
        _ = c.yyjson_mut_obj_add_val(doc, root_obj, "files", buildDedupFilesArray(doc, output, raw));
    } else {
        const results_arr = c.yyjson_mut_arr(doc);
        for (output) |*r| {
            const item = c.yyjson_mut_obj(doc);
            _ = c.yyjson_mut_obj_add_str(doc, item, "node", &r.node_name);
            _ = c.yyjson_mut_obj_add_str(doc, item, "qualified_name", &r.qualified_name);
            _ = c.yyjson_mut_obj_add_str(doc, item, "label", &r.label);
            _ = c.yyjson_mut_obj_add_str(doc, item, "file", &r.file);
            _ = c.yyjson_mut_obj_add_int(doc, item, "start_line", r.start_line);
            _ = c.yyjson_mut_obj_add_int(doc, item, "end_line", r.end_line);
            _ = c.yyjson_mut_obj_add_int(doc, item, "in_degree", r.in_degree);
            _ = c.yyjson_mut_obj_add_int(doc, item, "out_degree", r.out_degree);
            const ml = c.yyjson_mut_arr(doc);
            for (r.match_lines[0..r.match_count]) |line| _ = c.yyjson_mut_arr_add_int(doc, ml, line);
            _ = c.yyjson_mut_obj_add_val(doc, item, "match_lines", ml);
            attachResultSource(doc, item, r, mode, context_lines, root_path);
            _ = c.yyjson_mut_arr_add_val(results_arr, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root_obj, "results", results_arr);

        const max_raw = 20;
        const raw_arr = c.yyjson_mut_arr(doc);
        for (raw[0..@min(raw.len, max_raw)]) |*m| {
            const item = c.yyjson_mut_obj(doc);
            _ = c.yyjson_mut_obj_add_str(doc, item, "file", &m.file);
            _ = c.yyjson_mut_obj_add_int(doc, item, "line", m.line);
            _ = c.yyjson_mut_obj_add_str(doc, item, "content", &m.content);
            _ = c.yyjson_mut_arr_add_val(raw_arr, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root_obj, "raw_matches", raw_arr);
    }

    _ = c.yyjson_mut_obj_add_val(doc, root_obj, "directories", buildDirDistribution(doc, sr));

    // Summary stats
    _ = c.yyjson_mut_obj_add_int(doc, root_obj, "total_grep_matches", @intCast(gm_count));
    _ = c.yyjson_mut_obj_add_int(doc, root_obj, "total_results", @intCast(sr.len));
    _ = c.yyjson_mut_obj_add_int(doc, root_obj, "raw_match_count", @intCast(raw.len));
    const elapsed: c_int = @truncate(@as(i64, @bitCast(elapsed_ms)));
    _ = c.yyjson_mut_obj_add_int(doc, root_obj, "elapsed_ms", elapsed);
    if (sr.len > 0 and gm_count > 0) {
        var ratio: [32]u8 = undefined;
        const r = @as(f64, @floatFromInt(gm_count)) / @as(f64, @floatFromInt(sr.len + raw.len));
        _ = c.yyjson_mut_obj_add_strcpy(doc, root_obj, "dedup_ratio", mcp.fmtZ(&ratio, "{d:.1}x", .{r}));
    }

    // Warnings: surface common foot-guns instead of leaving them silent.
    const warnings = c.yyjson_mut_arr(doc);
    if (warn_literal_pipe) {
        _ = c.yyjson_mut_arr_add_strcpy(doc, warnings, "pattern contains '|' but regex=false, so it is matched literally (not as " ++
            "alternation). Pass regex=true for 'foo|bar' to mean 'foo OR bar'.");
    }
    if (elapsed_ms >= search_slow_ms) {
        var slow: [128]u8 = undefined;
        _ = c.yyjson_mut_arr_add_strcpy(doc, warnings, mcp.fmtZ(&slow, "search took {d}ms (>{d}s); narrow file_pattern/path_filter or use a more " ++
            "specific pattern", .{ elapsed, search_slow_ms / 1000 }));
        var ems: [32]u8 = undefined;
        interop.logWarn("search.slow", .{ "elapsed_ms", @as(CStr, mcp.fmtZ(&ems, "{d}", .{elapsed})) });
    }
    if (c.yyjson_mut_arr_size(warnings) > 0) _ = c.yyjson_mut_obj_add_val(doc, root_obj, "warnings", warnings);

    const json = mcp.docToStr(doc);
    if (json != null) sanitizeAscii(json);
    c.yyjson_mut_doc_free(doc);
    return mcp.textResultOwned(json, false);
}

/// path with the root prefix (and its following '/') removed.
fn stripRootPrefix(path: []const u8, root: []const u8) []const u8 {
    if (!std.mem.startsWith(u8, path, root)) return path;
    const rest = path[root.len..];
    return if (rest.len > 0 and rest[0] == '/') rest[1..] else rest;
}

/// strtol(s, NULL, 10) for a line number field.
fn parseLineNumber(s: []const u8) c_int {
    var i: usize = 0;
    while (i < s.len and std.ascii.isWhitespace(s[i])) i += 1;
    const neg = i < s.len and s[i] == '-';
    if (i < s.len and (s[i] == '-' or s[i] == '+')) i += 1;
    const start = i;
    while (i < s.len and std.ascii.isDigit(s[i])) i += 1;
    const v = std.fmt.parseInt(c_int, s[start..i], 10) catch return if (i == start) 0 else std.math.maxInt(c_int);
    return if (neg) -v else v;
}

fn initMatch(m: *GrepMatch, file: []const u8, line: c_int, content: []const u8) void {
    _ = mcp.bprint(&m.file, "{s}", .{file});
    if (is_windows) _ = c.cbm_normalize_path_sep(&m.file);
    m.line = line;
    _ = mcp.bprint(&m.content, "{s}", .{content});
    sanitizeAscii(&m.content);
}

/// Parse grep output ("file:line:content", tab-delimited on Windows), apply
/// the path filter, and collect up to grep_limit matches.
fn collectGrepMatches(fp: ?*c.FILE, root_path: []const u8, path_regex: ?*const c.cbm_regex_t, grep_limit: usize, out: *std.ArrayList(GrepMatch)) void {
    var line: [2048]u8 = undefined;
    // A match line longer than the buffer arrives in several chunks. Parse
    // only the first chunk of each line, so a continuation is never read as
    // a new match.
    var at_line_start = true;
    // PowerShell output uses tab as delimiter (paths may contain colons on
    // Windows, e.g. C:\dir\file). Unix grep uses colon.
    const sep: u8 = if (is_windows) '\t' else ':';
    while (out.items.len < grep_limit and c.fgets(&line, line.len, fp) != null) {
        var s = mcp.bufStr(&line);
        const continuation = !at_line_start;
        at_line_start = s.len > 0 and s[s.len - 1] == '\n';
        if (continuation) continue;
        s = std.mem.trimEnd(u8, s, "\r\n");
        if (s.len == 0) continue;
        const sep1 = std.mem.indexOfScalar(u8, s, sep) orelse continue;
        const sep2 = std.mem.indexOfScalarPos(u8, s, sep1 + 1, sep) orelse continue;
        line[sep1] = 0;
        if (is_windows) _ = c.cbm_normalize_path_sep(&line);
        const file = stripRootPrefix(s[0..sep1], root_path);
        if (path_regex) |re| {
            var file_z: [2048]u8 = undefined;
            if (c.cbm_regexec(re, mcp.fmtZ(&file_z, "{s}", .{file}), 0, null, 0) != c.CBM_REG_OK) continue;
        }
        const m = out.addOne(gpa) catch return;
        initMatch(m, file, parseLineNumber(s[sep1 + 1 .. sep2]), s[sep2 + 1 ..]);
    }
}

// ── In-process content scan ──────────────────────────────────────
// The scoped search_code path scans the indexed file list here instead of
// spawning `xargs grep` (POSIX) or a PowerShell Select-String pipeline
// (Windows), and fills GrepMatch directly, stopping at the match limit:
//   - patterns: the pattern split on '\n', as `grep -f` reads its file; an
//     empty pattern matches every line;
//   - literal mode is a substring match (grep -F); regex mode runs each line
//     through cbm_regex, the platform's POSIX ERE engine (grep -E), with the
//     GNU escapes (\b \< \w \s) grep accepts;
//   - a NUL byte in the first 32 KiB marks the file binary and yields no
//     hit, as grep prints no lines for it; after a later NUL nothing more is
//     reported, as GNU grep stops line output there;
//   - a listed file that cannot be read is skipped, as grep's error exit was
//     ignored.

/// A NUL-terminated heap copy of s, freed with gpa.free.
fn dupeZ(s: []const u8) ![:0]u8 {
    const out = try gpa.allocSentinel(u8, s.len, 0);
    @memcpy(out, s);
    return out;
}

const SEARCH_SCAN_BINARY_PROBE = 32768;
const SEARCH_SCAN_CHECK_EVERY = 4096; // lines between deadline checks
const SEARCH_SCAN_READ_CHUNK = 64 * 1024;

const ScanRc = enum(c_int) { ok = 0, deadline = 1, failed = 2 };

const ScanPatterns = struct {
    items: std.ArrayList([:0]u8) = .empty,
    regexes: std.ArrayList(c.cbm_regex_t) = .empty, // regex mode: one per item, unused for empty ones
    use_regex: bool = false,
    any_empty: bool = false, // an empty pattern: every line matches

    fn deinit(self: *ScanPatterns) void {
        for (self.regexes.items, 0..) |*re, i| {
            if (self.items.items[i].len > 0) c.cbm_regfree(re);
        }
        for (self.items.items) |item| gpa.free(item);
        self.regexes.deinit(gpa);
        self.items.deinit(gpa);
    }

    fn parse(pattern: []const u8, use_regex: bool) ?ScanPatterns {
        var self: ScanPatterns = .{ .use_regex = use_regex };
        var it = std.mem.splitScalar(u8, pattern, '\n');
        while (it.next()) |part| {
            const item = dupeZ(part) catch {
                self.deinit();
                return null;
            };
            self.items.append(gpa, item) catch {
                gpa.free(item);
                self.deinit();
                return null;
            };
            self.any_empty = self.any_empty or part.len == 0;
        }
        if (use_regex) {
            for (self.items.items) |item| {
                var re = std.mem.zeroes(c.cbm_regex_t);
                if (item.len > 0 and c.cbm_regcomp(&re, item.ptr, c.CBM_REG_EXTENDED | c.CBM_REG_ENHANCED | c.CBM_REG_NOSUB) != c.CBM_REG_OK) {
                    self.deinit();
                    return null;
                }
                self.regexes.append(gpa, re) catch {
                    if (item.len > 0) c.cbm_regfree(&re);
                    self.deinit();
                    return null;
                };
            }
        }
        return self;
    }

    /// line is NUL-terminated at line.len.
    fn lineMatches(self: *const ScanPatterns, line: [:0]const u8) bool {
        if (self.any_empty) return true;
        for (self.items.items, 0..) |item, i| {
            const hit = if (self.use_regex)
                c.cbm_regexec(&self.regexes.items[i], line.ptr, 0, null, 0) == c.CBM_REG_OK
            else
                std.mem.indexOf(u8, line, item) != null;
            if (hit) return true;
        }
        return false;
    }
};

const ScanHits = struct {
    gm: std.ArrayList(GrepMatch) = .empty,
    limit: usize,
    root_path: []const u8,

    /// Append one hit the way collectGrepMatches stores a grep record: the
    /// path relative to the root, and the line without trailing CRs,
    /// truncated to the content buffer.
    fn add(self: *ScanHits, path: []const u8, line_number: usize, line: []const u8) bool {
        const m = self.gm.addOne(gpa) catch return false;
        initMatch(m, stripRootPrefix(path, self.root_path), @intCast(@min(line_number, std.math.maxInt(c_int))), std.mem.trimEnd(u8, line, "\r"));
        return true;
    }
};

/// Scan one file. buffer.len is the file length; one spare byte follows it.
/// Stops early once hits reach their limit.
fn scanBuffer(hits: *ScanHits, patterns: *const ScanPatterns, path: []const u8, buffer_in: []u8, deadline_ms: u64) ScanRc {
    var buffer = buffer_in;
    if (std.mem.indexOfScalar(u8, buffer[0..@min(buffer.len, SEARCH_SCAN_BINARY_PROBE)], 0) != null) return .ok;
    if (std.mem.indexOfScalar(u8, buffer, 0)) |nul| {
        // Lines up to the last complete line before the NUL.
        const keep = if (std.mem.lastIndexOfScalar(u8, buffer[0..nul], '\n')) |nl| nl + 1 else 0;
        buffer = buffer[0..keep];
    }
    const length = buffer.len;
    buffer.ptr[length] = 0;
    var line_number: usize = 1;
    var since_check: usize = 0;
    const single_literal = !patterns.use_regex and !patterns.any_empty and patterns.items.items.len == 1;
    var pos: usize = 0;
    while (pos < length and hits.gm.items.len < hits.limit) {
        since_check += 1;
        if (since_check >= SEARCH_SCAN_CHECK_EVERY) {
            since_check = 0;
            if (c.cbm_now_ms() >= deadline_ms) return .deadline;
        }
        var line_start = pos;
        if (single_literal) {
            // Jump to the next occurrence instead of visiting every line.
            const hit = std.mem.indexOfPos(u8, buffer, pos, patterns.items.items[0]) orelse break;
            line_start = if (std.mem.lastIndexOfScalar(u8, buffer[pos..hit], '\n')) |nl| pos + nl + 1 else pos;
            line_number += std.mem.count(u8, buffer[pos..line_start], "\n");
        }
        const line_end = std.mem.indexOfScalarPos(u8, buffer, line_start, '\n') orelse length;
        // Windows matched Select-String's Line, which has no trailing CR, so
        // `foo$` matched a CRLF line there; POSIX grep matches with the CR.
        var match_end = line_end;
        if (is_windows and match_end > line_start and buffer[match_end - 1] == '\r') match_end -= 1;
        const saved = buffer.ptr[match_end];
        buffer.ptr[match_end] = 0;
        const matched = single_literal or patterns.lineMatches(buffer.ptr[line_start..match_end :0]);
        buffer.ptr[match_end] = saved;
        if (matched and !hits.add(path, line_number, buffer[line_start..line_end])) return .failed;
        pos = line_end + 1;
        line_number += 1;
    }
    return .ok;
}

/// Read a whole file into buf (reused across files), leaving one spare byte
/// after the contents.
fn scanReadFile(path: CStr, buf: *std.ArrayList(u8)) bool {
    const fp = c.cbm_fopen(path, "rb");
    if (fp == null) return false;
    defer _ = c.fclose(fp);
    buf.clearRetainingCapacity();
    while (true) {
        if (buf.items.len + SEARCH_SCAN_READ_CHUNK + 1 > std.math.maxInt(c_int)) return false;
        buf.ensureUnusedCapacity(gpa, SEARCH_SCAN_READ_CHUNK + 1) catch return false;
        const want = buf.capacity - buf.items.len - 1;
        const got = c.fread(buf.items.ptr + buf.items.len, 1, want, fp);
        buf.items.len += got;
        if (got < want) return c.ferror(fp) == 0;
    }
}

/// Scan every file in filelist_path (the writeScopedFilelist format) and
/// collect up to limit hits in file-list order. .deadline once cbm_now_ms()
/// reaches deadline_ms; .failed on a bad regex, unreadable list or
/// allocation failure. Hits are kept only on .ok.
fn scanInProcess(filelist_path: CStr, root_path: CStr, pattern: CStr, use_regex: bool, limit: usize, deadline_ms: u64, hits: *ScanHits) ScanRc {
    // NUL-separated for `xargs -0` on POSIX; newline-separated for
    // PowerShell's Get-Content on Windows.
    const record_separator: u8 = if (is_windows) '\n' else 0;
    hits.* = .{ .limit = limit, .root_path = std.mem.span(root_path) };
    var patterns = ScanPatterns.parse(std.mem.span(pattern), use_regex) orelse return .failed;
    defer patterns.deinit();
    var list: std.ArrayList(u8) = .empty;
    defer list.deinit(gpa);
    if (!scanReadFile(filelist_path, &list)) return .failed;
    var buffer: std.ArrayList(u8) = .empty;
    defer buffer.deinit(gpa);
    var rc: ScanRc = .ok;
    var records = std.mem.splitScalar(u8, list.items, record_separator);
    while (records.next()) |path| {
        if (rc != .ok or hits.gm.items.len >= limit) break;
        if (path.len == 0) continue;
        if (c.cbm_now_ms() >= deadline_ms) {
            rc = .deadline;
            break;
        }
        const path_z = dupeZ(path) catch {
            rc = .failed;
            break;
        };
        defer gpa.free(path_z);
        if (!scanReadFile(path_z.ptr, &buffer)) continue;
        rc = scanBuffer(hits, &patterns, path, buffer.items.ptr[0..buffer.items.len], deadline_ms);
    }
    if (rc != .ok) {
        hits.gm.deinit(gpa);
        hits.gm = .empty;
    }
    return rc;
}

pub export fn cbm_mcp_search_scan_for_testing(filelist_path: [*c]const u8, root_path: [*c]const u8, pattern: [*c]const u8, use_regex: bool, limit: c_int, deadline_ms: u64, out: [*c]u8, out_size: usize) c_int {
    var hits: ScanHits = undefined;
    const rc = scanInProcess(filelist_path, root_path, pattern, use_regex, @intCast(@max(limit, 0)), deadline_ms, &hits);
    defer hits.gm.deinit(gpa);
    if (out_size > 0) out[0] = 0;
    var used: usize = 0;
    for (hits.gm.items) |*m| {
        if (used >= out_size) break;
        used += mcp.bprint(out[used..out_size], "{s}:{d}:{s}\n", .{ m.fileStr(), m.line, mcp.bufStr(&m.content) });
    }
    return @backingInt(rc);
}

// ── Grouping hits into graph nodes ───────────────────────────────

/// The tightest node containing a line, or null.
fn findTightestNode(nodes: []const c.cbm_node_t, line: c_int) ?usize {
    var best: ?usize = null;
    var best_span: c_int = 999999;
    for (nodes, 0..) |*n, j| {
        if (n.start_line <= line and n.end_line >= line) {
            const span = n.end_line - n.start_line;
            if (span < best_span) {
                best = j;
                best_span = span;
            }
        }
    }
    return best;
}

/// Add a grep hit to the search result set (merge into existing or create new).
fn addToSearchResults(sr: *std.ArrayList(SearchResult), n: *const c.cbm_node_t, line: c_int) void {
    for (sr.items) |*r| {
        if (r.node_id == n.id) {
            if (r.match_count < MAX_MATCH_LINES) {
                r.match_lines[r.match_count] = line;
                r.match_count += 1;
            }
            return;
        }
    }
    const r = sr.addOne(gpa) catch return;
    r.* = .{ .node_id = n.id, .start_line = n.start_line, .end_line = n.end_line, .match_count = 1 };
    _ = mcp.bprint(&r.node_name, "{s}", .{sp(mcp.orEmpty(n.name))});
    _ = mcp.bprint(&r.qualified_name, "{s}", .{sp(mcp.orEmpty(n.qualified_name))});
    _ = mcp.bprint(&r.label, "{s}", .{sp(mcp.orEmpty(n.label))});
    _ = mcp.bprint(&r.file, "{s}", .{sp(mcp.orEmpty(n.file_path))});
    r.match_lines[0] = line;
}

fn grepMatchLess(_: void, a: GrepMatch, b: GrepMatch) bool {
    return std.mem.order(u8, a.fileStr(), b.fileStr()) == .lt;
}

/// Classify all grep matches file-by-file into search results (a hit inside
/// a graph node) and raw hits (no containing node).
fn classifyAllGrepHits(gm: []GrepMatch, store: ?*c.cbm_store_t, project: CStr, sr: *std.ArrayList(SearchResult), raw: *std.ArrayList(GrepMatch)) void {
    std.sort.block(GrepMatch, gm, {}, grepMatchLess);
    var i: usize = 0;
    while (i < gm.len) {
        const file_start = i;
        while (i < gm.len and eql(gm[i].fileStr(), gm[file_start].fileStr())) i += 1;
        var file_nodes: [*c]c.cbm_node_t = null;
        var file_node_count: c_int = 0;
        if (store != null) _ = c.cbm_store_find_nodes_by_file(store, project, &gm[file_start].file, &file_nodes, &file_node_count);
        defer c.cbm_store_free_nodes(file_nodes, file_node_count);
        const nodes: []const c.cbm_node_t = if (file_nodes != null and file_node_count > 0) file_nodes[0..@intCast(file_node_count)] else &.{};
        for (gm[file_start..i]) |*hit| {
            if (findTightestNode(nodes, hit.line)) |best| {
                addToSearchResults(sr, &nodes[best], hit.line);
            } else {
                raw.append(gpa, hit.*) catch {};
            }
        }
    }
}

/// Write the indexed file list for a scoped scan. Returns true if scoped.
/// path_filter (and on POSIX file_pattern, as a basename glob) is applied
/// here — before the scan — with the same predicate the post-grep filter
/// uses, so prefiltering only skips files whose hits would be dropped anyway.
/// written receives the number of records written (0 = the filter excluded
/// every indexed file).
fn writeScopedFilelist(srv: *Server, project: CStr, root_path: CStr, filelist: CStr, path_regex: ?*const c.cbm_regex_t, file_pattern: CStr, written: *c_int) bool {
    written.* = 0;
    const pre_store = mcp.resolveStore(srv, project) orelse return false;
    var indexed_files: [*c][*c]u8 = null;
    var indexed_count: c_int = 0;
    const rc = c.cbm_store_list_files(pre_store, project, &indexed_files, &indexed_count);
    defer {
        for (0..@intCast(@max(indexed_count, 0))) |fi| c.free(indexed_files[fi]);
        c.free(@ptrCast(indexed_files));
    }
    if (rc != c.CBM_STORE_OK or indexed_count == 0) return false;
    const fl = c.cbm_fopen(filelist, "wb");
    if (fl == null) return false;
    defer _ = c.fclose(fl);
    const root = std.mem.span(root_path);
    for (indexed_files[0..@intCast(indexed_count)]) |file| {
        // A source path never legitimately contains a newline or carriage
        // return. Those bytes are the record separator on the Windows
        // filelist, so a crafted indexed path could inject an extra entry
        // into the scan set. Skip such paths entirely.
        if (c.strpbrk(file, "\r\n") != null) continue;
        if (path_regex) |re| {
            if (is_windows) _ = c.cbm_normalize_path_sep(file);
            if (c.cbm_regexec(re, file, 0, null, 0) != c.CBM_REG_OK) continue;
        }
        if (!is_windows and file_pattern != null) {
            // grep --include semantics: a basename glob. Windows keeps its
            // PowerShell -like filter.
            const base = c.strrchr(file, '/');
            if (c.fnmatch(file_pattern, if (base != null) base + 1 else file, 0) != 0) continue;
        }
        // "<root>/<file>" written piece-by-piece (no fixed-size buffer). The
        // record separator is NUL for `xargs -0` on Unix (handles spaces in
        // paths) and newline for PowerShell Get-Content on Windows (NUL bytes
        // break it).
        const f = std.mem.span(file);
        _ = c.fwrite(root.ptr, 1, root.len, fl);
        _ = c.fputc('/', fl);
        _ = c.fwrite(f.ptr, 1, f.len, fl);
        _ = c.fputc(if (is_windows) '\n' else 0, fl);
        written.* += 1;
    }
    return true;
}

fn parseSearchMode(mode_str: CStr) SearchMode {
    if (mode_str == null) return .compact;
    if (mcp.cEql(mode_str, "full")) return .full;
    if (mcp.cEql(mode_str, "files")) return .files;
    return .compact;
}

/// Search paths and globs are ALWAYS single-quoted (POSIX sh) or quoted
/// (Windows PowerShell) on the command line, which neutralises '&' — a very
/// common character in real paths (R&D, "Foo & Bar", OneDrive). Accept '&'
/// while still rejecting every metacharacter that could break out of the
/// quoting (#272).
pub fn validateSearchPathArg(s: CStr) bool {
    if (s == null) return false;
    for (std.mem.span(s)) |ch| {
        switch (ch) {
            '\'', '"', ';', '|', '$', '`', '<', '>', '\n', '\r' => return false,
            '\\' => if (!is_windows) return false,
            else => {},
        }
    }
    return true;
}

/// Write the pattern to the temp file for grep -f.
fn writePatternFile(tmpfile: CStr, pattern: CStr) bool {
    const tf = c.cbm_fopen(tmpfile, "w");
    if (tf == null) return false;
    _ = c.fputs(pattern, tf);
    _ = c.fputc('\n', tf);
    _ = c.fclose(tf);
    return true;
}

/// If pattern contains whitespace and is not already a regex, a regex that
/// matches all words in order: "foo bar baz" → "foo.*bar.*baz". This avoids
/// requiring the exact phrase as a contiguous substring. NULL on allocation
/// failure.
fn multiWordRegex(pattern: CStr) HeapStr {
    const src = std.mem.span(pattern);
    const out = interop.mallocN(u8, src.len * 3 + 1);
    if (out == null) return null;
    var dst: usize = 0;
    var in_space = false;
    for (src) |ch| {
        if (ch == ' ' or ch == '\t') {
            if (!in_space) {
                out[dst] = '.';
                out[dst + 1] = '*';
                dst += 2;
                in_space = true;
            }
        } else {
            // Escape regex metacharacters from user input.
            if (std.mem.indexOfScalar(u8, "\\^$.|?*+()[]{}", ch) != null) {
                out[dst] = '\\';
                dst += 1;
            }
            out[dst] = ch;
            dst += 1;
            in_space = false;
        }
    }
    out[dst] = 0;
    return out;
}

const GrepOutcome = union(enum) { ok, err: Str, err_owned: HeapStr };

/// Run the grep / PowerShell scan supervised: a wall-clock deadline and an
/// output cap, with the kill taking the whole sh/xargs/grep group. Either
/// bound fails the request closed rather than returning a silently partial
/// match set. grep's own exit status stays ignored (1 = no match, 2 =
/// unreadable file).
fn runGrepScan(tmpfile: []const u8, filelist: []const u8, scoped: bool, use_regex: bool, pattern: CStr, file_pattern: CStr, root_path: CStr, path_regex: ?*const c.cbm_regex_t, out: *std.ArrayList(GrepMatch)) GrepOutcome {
    var tmpfile_z: [256]u8 = undefined;
    _ = mcp.bprint(&tmpfile_z, "{s}", .{tmpfile});
    if (!writePatternFile(&tmpfile_z, pattern)) {
        var errmsg: [256]u8 = undefined;
        return .{ .err_owned = mcp.dup(mcp.fmtZ(&errmsg, "search failed: cannot create temp file ({s})", .{sp(c.strerror(std.c._errno().*))})) };
    }
    defer _ = c.cbm_unlink(&tmpfile_z);
    var cmd: [4096]u8 = undefined;
    buildGrepCmd(&cmd, use_regex, scoped, file_pattern, tmpfile, filelist, std.mem.span(root_path));

    var outfile: [264]u8 = undefined;
    _ = mcp.bprint(&outfile, "{s}.out", .{tmpfile});
    defer _ = c.cbm_unlink(&outfile);
    // On Windows the scan is one PowerShell process with no children, so the
    // TerminateProcess kill covers it.
    const scan_argv = if (is_windows) [_]CStr{ "powershell", "-Command", &cmd, null } else [_]CStr{ "/bin/sh", "-c", &cmd, null };
    var scan_opts = std.mem.zeroes(c.cbm_proc_opts_t);
    scan_opts.bin = scan_argv[0];
    scan_opts.argv = &scan_argv;
    scan_opts.log_file = &outfile;
    scan_opts.deadline_ms = MCP_SEARCH_SCAN_TIMEOUT_MS;
    scan_opts.max_log_bytes = MCP_SEARCH_OUTPUT_MAX;
    scan_opts.own_process_group = true;
    var scan_result: c.cbm_proc_result_t = undefined;
    const scan_rc = c.cbm_subprocess_run(&scan_opts, &scan_result);
    var scan_err: ?Str = null;
    if (scan_rc != 0) {
        scan_err = "search failed";
    } else if (scan_result.outcome == c.CBM_PROC_HANG) {
        scan_err = "search_code scan exceeded its execution deadline";
    } else if (scan_result.outcome == c.CBM_PROC_OUTPUT_LIMIT) {
        scan_err = std.fmt.comptimePrint("search failed: output exceeded the {d}-byte safety limit", .{MCP_SEARCH_OUTPUT_MAX});
    } else if (scan_result.outcome == c.CBM_PROC_CRASH or scan_result.outcome == c.CBM_PROC_KILLED) {
        scan_err = "search failed: the scan command did not complete";
    }
    if (scan_err) |e| return .{ .err = e };
    const fp = c.cbm_fopen(&outfile, "r");
    if (fp == null) return .{ .err = "search failed" };
    defer _ = c.fclose(fp);
    collectGrepMatches(fp, std.mem.span(root_path), path_regex, GREP_MAX_MATCHES, out);
    return .ok;
}

pub fn handleSearchCode(srv: *Server, args: CStr) HeapStr {
    var pattern = mcp.strArg(args, "pattern");
    defer c.free(pattern);
    const project = mcp.getProjectArg(args);
    defer c.free(project);
    const file_pattern = mcp.strArg(args, "file_pattern");
    defer c.free(file_pattern);
    const limit = mcp.intArg(args, "limit", MCP_DEFAULT_LIMIT);
    const context_lines = mcp.intArg(args, "context", 0);
    var use_regex = mcp.boolArg(args, "regex");
    const search_t0 = c.cbm_now_ms();
    // In literal (non-regex) mode a '|' is matched as a byte, not alternation
    // — a common silent 0-match trap; flagged in the result warnings (#282).
    const pat_has_pipe = pattern != null and c.strchr(pattern, '|') != null;

    const mode_str = mcp.strArg(args, "mode");
    const mode = parseSearchMode(mode_str);
    c.free(mode_str);

    var path_regex_storage = std.mem.zeroes(c.cbm_regex_t);
    const path_filter = mcp.strArg(args, "path_filter");
    defer c.free(path_filter);
    const has_path_filter = path_filter != null and path_filter[0] != 0;
    // An invalid path_filter must not be dropped: the search would then run
    // unfiltered and return results the caller asked to exclude.
    if (has_path_filter and
        c.cbm_regcomp(&path_regex_storage, path_filter, c.CBM_REG_EXTENDED | c.CBM_REG_NOSUB) != c.CBM_REG_OK)
    {
        var message: [512]u8 = undefined;
        return textResult(mcp.fmtZ(&message, "invalid path_filter regex '{s}': check for unbalanced (), [], or {{}}", .{sp(path_filter)}), true);
    }
    defer if (has_path_filter) c.cbm_regfree(&path_regex_storage);
    const path_regex: ?*const c.cbm_regex_t = if (has_path_filter) &path_regex_storage else null;

    if (pattern == null) return textResult("pattern is required", true);
    if (project == null) return mcp.textResultOwned(mcp.buildProjectListError("project is required"), true);

    const root_path = code_tools.getProjectRoot(project);
    if (root_path == null) return mcp.textResultOwned(mcp.buildProjectListError("project not found or not indexed"), true);
    defer c.free(root_path);

    if (!validateSearchPathArg(root_path) or (file_pattern != null and !validateSearchPathArg(file_pattern))) {
        return textResult("path or file_pattern contains invalid characters", true);
    }

    // issue #283: when regex=true, a syntactically invalid pattern makes the
    // scan fail, which would otherwise read as an empty result set —
    // indistinguishable from a legitimate no-match. Validate up front.
    if (use_regex) {
        var probe = std.mem.zeroes(c.cbm_regex_t);
        if (c.cbm_regcomp(&probe, pattern, c.CBM_REG_EXTENDED | c.CBM_REG_ENHANCED | c.CBM_REG_NOSUB) != c.CBM_REG_OK) {
            return textResult("invalid regex pattern (regex=true): check for unbalanced (), [], or {}", true);
        }
        c.cbm_regfree(&probe);
    }

    // Phase 0.5: multi-word → regex conversion.
    if (!use_regex and c.strchr(pattern, ' ') != null) {
        const regex_pat = multiWordRegex(pattern);
        if (regex_pat != null) {
            c.free(pattern);
            pattern = regex_pat;
            use_regex = true;
        }
    }

    // Phase 1: scan. Scope it to indexed files only — avoids scanning
    // vendored/generated code. Falls back to a recursive grep if no indexed
    // files are found (project not fully indexed).
    var tmpfile_buf: [256]u8 = undefined;
    const tmpfile = mcp.bufStr(mcp.fmtZ(&tmpfile_buf, "{s}/cbm_search_{d}.pat", .{ sp(c.cbm_tmpdir()), getpid() })[0..tmpfile_buf.len]);
    var filelist_buf: [256]u8 = undefined;
    const filelist = mcp.fmtZ(&filelist_buf, "{s}.files", .{tmpfile});
    var scoped_written: c_int = 0;
    const scoped = writeScopedFilelist(srv, project, root_path, filelist, path_regex, file_pattern, &scoped_written);
    defer if (scoped) {
        _ = c.cbm_unlink(filelist);
    };

    // The indexed file list is scanned in process. On Windows the list does
    // not apply file_pattern (the PowerShell -like filter does), so that case
    // keeps the subprocess.
    const scan_in_process = scoped and !(is_windows and file_pattern != null);

    var gm: std.ArrayList(GrepMatch) = .empty;
    defer gm.deinit(gpa);
    if (scoped and scoped_written == 0) {
        // The path_filter (or POSIX file_pattern) excluded every indexed
        // file — nothing to scan.
    } else if (scan_in_process) {
        var hits: ScanHits = undefined;
        const scan_rc = scanInProcess(filelist, root_path, pattern, use_regex, GREP_MAX_MATCHES, c.cbm_now_ms() + MCP_SEARCH_SCAN_TIMEOUT_MS, &hits);
        gm = hits.gm;
        if (scan_rc != .ok) {
            return textResult(if (scan_rc == .deadline) "search_code scan exceeded its execution deadline" else "search failed", true);
        }
    } else {
        switch (runGrepScan(tmpfile, mcp.bufStr(&filelist_buf), scoped, use_regex, pattern, file_pattern, root_path, path_regex, &gm)) {
            .ok => {},
            .err => |e| return textResult(e, true),
            .err_owned => |e| return mcp.textResultOwned(e, true),
        }
    }

    // Phase 2+3: block expansion + graph ranking. One query per unique file
    // for nodes, one batch query for all degrees.
    const store = mcp.resolveStore(srv, project);
    var sr: std.ArrayList(SearchResult) = .empty;
    defer sr.deinit(gpa);
    var raw: std.ArrayList(GrepMatch) = .empty;
    defer raw.deinit(gpa);
    classifyAllGrepHits(gm.items, store, project, &sr, &raw);

    // Phase 3: batch degree query — ONE query for all results instead of 2×N.
    if (store != null and sr.items.len > 0) batchDegrees(store, sr.items);

    // Compute scores and sort (descending; ties keep classification order).
    for (sr.items) |*r| r.score = computeSearchScore(r);
    std.sort.block(SearchResult, sr.items, {}, struct {
        fn more(_: void, a: SearchResult, b: SearchResult) bool {
            return a.score > b.score;
        }
    }.more);

    // Phase 4: context assembly.
    return assembleSearchOutput(sr.items, raw.items, gm.items.len, limit, mode, context_lines, root_path, pat_has_pipe and !use_regex, c.cbm_now_ms() - search_t0);
}

fn batchDegrees(store: ?*c.cbm_store_t, sr: []SearchResult) void {
    const ids = gpa.alloc(i64, sr.len) catch return;
    defer gpa.free(ids);
    const in_degs = gpa.alloc(c_int, sr.len) catch return;
    defer gpa.free(in_degs);
    const out_degs = gpa.alloc(c_int, sr.len) catch return;
    defer gpa.free(out_degs);
    for (sr, ids) |r, *id| id.* = r.node_id;
    if (c.cbm_store_batch_count_degrees(store, ids.ptr, @intCast(sr.len), "CALLS", in_degs.ptr, out_degs.ptr) == c.CBM_STORE_OK) {
        for (sr, in_degs, out_degs) |*r, in_d, out_d| {
            r.in_degree = in_d;
            r.out_degree = out_d;
        }
    }
}

fn getpid() c_int {
    if (is_windows) return @intCast(std.os.windows.GetCurrentProcessId());
    return @intCast(c.getpid());
}
