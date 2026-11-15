//! project_tools.zig — the check_index_coverage, index_status,
//! delete_project and get_architecture MCP tool handlers.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const mcp = @import("mcp.zig");

const CStr = mcp.CStr;
const HeapStr = mcp.HeapStr;
const Str = mcp.Str;
const Server = mcp.Server;
const MutDoc = mcp.MutDoc;
const MutVal = mcp.MutVal;
const Val = mcp.Val;
const textResult = mcp.textResult;
const orEmpty = mcp.orEmpty;
const fmtZ = mcp.fmtZ;
const eql = mcp.eql;

// ── Index coverage ───────────────────────────────────────────────
// Coverage is stamped on File node properties by the pipeline and published
// with the generation (see CBM_COVERAGE_* in pipeline.h), so every read here
// is scoped to one workspace and its active generation.

const COVERAGE_PATH_MAX = 128;
const COVERAGE_SCOPE_MAX = 32;
const COVERAGE_SCOPE_DEFAULT_LIMIT = 200;
const COVERAGE_SCOPE_MAX_LIMIT = 1000;
const COVERAGE_RANGE_MAX = 256; // matches CBM_MAX_ERROR_REGIONS
const COVERAGE_STATUS_EXAMPLES = 5;

pub const PathResult = enum { ok, outside, invalid };

/// What one File row says about extraction.
const CoverageFile = struct {
    status: Str = "no_recorded_issue", // no_recorded_issue | partial | unusable | skipped
    ranges: HeapStr = null, // partial/unusable: "12-40,88-90", ",+N" when clipped
    phase: HeapStr = null, // skipped
    reason: HeapStr = null, // skipped

    fn parse(row: *const c.cbm_zova_file_coverage_t) CoverageFile {
        if (row.properties == null) {
            // Tracked by a file hash but without a File node: nothing extracted.
            return .{
                .status = "skipped",
                .phase = mcp.dup("structure"),
                .reason = mcp.dup("tracked file has no File node"),
            };
        }
        var out: CoverageFile = .{};
        const doc = mcp.readJson(row.properties);
        defer c.yyjson_doc_free(doc);
        const root: ?*Val = if (doc) |d| c.yyjson_doc_get_root(d) else null;
        const coverage = c.yyjson_get_str(c.yyjson_obj_get(root, "coverage"));
        if (coverage != null and mcp.cEql(coverage, c.CBM_COVERAGE_PARTIAL)) {
            const ranges = c.yyjson_get_str(c.yyjson_obj_get(root, "parse_error_ranges"));
            // One range over most of the file: the ranges are no help, so the
            // file is reported as unusable rather than partial (upstream
            // a9535092). It was still indexed.
            out.status = if (c.yyjson_get_bool(c.yyjson_obj_get(root, "parse_unusable"))) "unusable" else "partial";
            out.ranges = mcp.dup(orEmpty(ranges));
        } else if (coverage != null and mcp.cEql(coverage, c.CBM_COVERAGE_MISSED)) {
            out.status = "skipped";
            out.phase = mcp.dup(orEmpty(c.yyjson_get_str(c.yyjson_obj_get(root, "coverage_phase"))));
            out.reason = mcp.dup(orEmpty(c.yyjson_get_str(c.yyjson_obj_get(root, "coverage_reason"))));
        }
        return out;
    }

    fn deinit(self: *CoverageFile) void {
        c.free(self.ranges);
        c.free(self.phase);
        c.free(self.reason);
        self.* = .{};
    }

    fn is(self: *const CoverageFile, status: []const u8) bool {
        return eql(std.mem.span(self.status), status);
    }
};

/// Normalize an untrusted repository-relative path without touching the
/// filesystem. Absolute paths, drive/UNC paths and any `..` component are
/// outside the project; control bytes are invalid. A root scope (`.`)
/// normalizes to the empty prefix.
pub fn normalizeRel(input_c: CStr, allow_root: bool, out: []u8) PathResult {
    if (input_c == null or out.len == 0) return .invalid;
    out[0] = 0;
    const input = std.mem.span(input_c);
    if (input.len == 0 and allow_root) return .ok;
    if (input.len == 0 or input.len >= out.len) return .invalid;
    if (input[0] == '/' or input[0] == '\\' or (input.len >= 2 and std.ascii.isAlphabetic(input[0]) and input[1] == ':')) {
        return .outside;
    }
    var written: usize = 0;
    var it = std.mem.tokenizeAny(u8, input, "/\\");
    while (it.next()) |part| {
        for (part) |ch| {
            if (ch < 0x20) return .invalid;
        }
        if (eql(part, ".")) continue;
        if (eql(part, "..")) return .outside;
        if (written > 0) {
            out[written] = '/';
            written += 1;
        }
        @memcpy(out[written .. written + part.len], part);
        written += part.len;
    }
    out[written] = 0;
    return if (written > 0 or allow_root) .ok else .invalid;
}

/// Same clock the pipeline records in cbm_file_hashes_v1.mtime_ns.
fn statMtimeNs(st: *const c.struct_stat) i64 {
    const ns_per_sec: i64 = 1_000_000_000;
    if (builtin.os.tag.isDarwin()) {
        return @as(i64, st.st_mtimespec.tv_sec) * ns_per_sec + @as(i64, st.st_mtimespec.tv_nsec);
    } else if (mcp.is_windows) {
        return @as(i64, st.st_mtime) * ns_per_sec;
    } else {
        return @as(i64, st.st_mtim.tv_sec) * ns_per_sec + @as(i64, st.st_mtim.tv_nsec);
    }
}

const StatResult = enum { exists, missing, outside };

/// stat root/rel; .outside when it resolves outside the root (symlink escape).
fn coverageStat(root_path: CStr, rel: CStr, st: *c.struct_stat) StatResult {
    var abs_path: [4096]u8 = undefined;
    const n = mcp.bprint(&abs_path, "{s}/{s}", .{ std.mem.span(root_path), std.mem.span(rel) });
    if (n >= abs_path.len or c.stat(&abs_path, st) != 0) return .missing;
    return if (mcp.pathWithinRoot(root_path, &abs_path)) .exists else .outside;
}

/// Filesystem metadata against the hash row published with the generation.
fn coverageFreshness(root_path: CStr, row: *const c.cbm_zova_file_coverage_t) Str {
    if (root_path == null or root_path[0] == 0) return "unavailable";
    var st: c.struct_stat = undefined;
    switch (coverageStat(root_path, row.file_path, &st)) {
        .missing => return "missing",
        .outside => return "unavailable",
        .exists => {},
    }
    if (!row.has_hash) return "not_tracked";
    return if (row.mtime_ns == statMtimeNs(&st) and row.size_bytes == @as(i64, @intCast(st.st_size))) "metadata_match" else "metadata_changed";
}

/// Discovery rules that provably exclude rel: built-in skip directories and
/// suffixes, and the root .gitignore / .cbmignore. Null when none applies;
/// nested ignore files and global excludes are not consulted.
fn coverageExclusion(rel: CStr, gitignore: ?*const c.cbm_gitignore_t, cbmignore: ?*const c.cbm_gitignore_t) ?Str {
    var prefix: [4096]u8 = undefined;
    const len = mcp.bprint(&prefix, "{s}", .{std.mem.span(rel)});
    const p = prefix[0..@min(len, prefix.len - 1)];
    var i: usize = 0;
    while (std.mem.indexOfScalarPos(u8, p, i, '/')) |slash| : (i = slash + 1) {
        prefix[slash] = 0;
        const dir = p[0..slash];
        const base = if (std.mem.lastIndexOfScalar(u8, dir, '/')) |s| prefix[s + 1 ..].ptr else prefix[0..].ptr;
        if (c.cbm_should_skip_dir(@ptrCast(base), c.CBM_MODE_FULL)) return "skipped_directory";
        if (gitignore != null and c.cbm_gitignore_matches(gitignore, &prefix, true)) return "gitignore";
        if (cbmignore != null and c.cbm_gitignore_matches(cbmignore, &prefix, true)) return "cbmignore";
        prefix[slash] = '/';
    }
    const base = c.strrchr(rel, '/');
    if (c.cbm_has_ignored_suffix(if (base != null) base + 1 else rel, c.CBM_MODE_FULL)) return "ignored_suffix";
    if (gitignore != null and c.cbm_gitignore_matches(gitignore, rel, false)) return "gitignore";
    if (cbmignore != null and c.cbm_gitignore_matches(cbmignore, rel, false)) return "cbmignore";
    return null;
}

/// strtol at s[p.*]: leading whitespace, optional sign, decimal digits. Null
/// when no digits follow; otherwise advances p past them.
fn parseLong(s: []const u8, p: *usize) ?c_long {
    var i = p.*;
    while (i < s.len and std.ascii.isWhitespace(s[i])) i += 1;
    var negative = false;
    if (i < s.len and (s[i] == '+' or s[i] == '-')) {
        negative = s[i] == '-';
        i += 1;
    }
    const start = i;
    while (i < s.len and std.ascii.isDigit(s[i])) i += 1;
    if (i == start) return null;
    p.* = i;
    const v = std.fmt.parseInt(c_long, s[start..i], 10) catch std.math.maxInt(c_long);
    return if (negative) -v else v;
}

/// Ranges as {start,end} objects. The extractor ends a clipped list with
/// ",+N" (N ranges dropped by its cap); that, or this reader's own cap, sets
/// "truncated" so a clipped list never reads as a complete one.
fn coverageAddRanges(doc: ?*MutDoc, item: ?*MutVal, ranges_c: CStr) void {
    if (ranges_c == null or ranges_c[0] == 0) return;
    const ranges = std.mem.span(ranges_c);
    const arr = c.yyjson_mut_arr(doc);
    var p: usize = 0;
    var emitted: c_int = 0;
    var dropped: c_long = 0;
    var truncated = false;
    while (p < ranges.len) {
        if (ranges[p] == '+') {
            var q = p + 1;
            if (parseLong(ranges, &q)) |v| {
                dropped = v;
                truncated = v > 0;
            }
            break;
        }
        if (emitted >= COVERAGE_RANGE_MAX) {
            truncated = true;
            break;
        }
        const start = parseLong(ranges, &p) orelse break;
        if (start <= 0) break;
        var stop = start;
        if (p < ranges.len and ranges[p] == '-') {
            var q = p + 1;
            const parsed = parseLong(ranges, &q) orelse break;
            if (parsed < start) break;
            stop = parsed;
            p = q;
        }
        const range = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_int(doc, range, "start", start);
        _ = c.yyjson_mut_obj_add_int(doc, range, "end", stop);
        _ = c.yyjson_mut_arr_add_val(arr, range);
        emitted += 1;
        if (p >= ranges.len or ranges[p] != ',') break;
        p += 1;
    }
    _ = c.yyjson_mut_obj_add_val(doc, item, "parse_error_ranges", arr);
    if (truncated) {
        _ = c.yyjson_mut_obj_add_bool(doc, item, "parse_error_ranges_truncated", true);
        if (dropped > 0) _ = c.yyjson_mut_obj_add_int(doc, item, "parse_error_ranges_dropped", dropped);
    }
}

fn coverageAddDetail(doc: ?*MutDoc, item: ?*MutVal, cf: *const CoverageFile) void {
    if (cf.ranges != null) coverageAddRanges(doc, item, cf.ranges);
    if (cf.phase != null) {
        _ = c.yyjson_mut_obj_add_strcpy(doc, item, "phase", cf.phase);
        _ = c.yyjson_mut_obj_add_strcpy(doc, item, "reason", orEmpty(cf.reason));
    }
}

fn recommendedAction(status_c: Str, freshness: ?Str) Str {
    const status = std.mem.span(status_c);
    if (eql(status, "excluded")) return "read_source_or_change_ignore_rules";
    if (eql(status, "not_indexed") or eql(status, "coverage_unavailable")) return "read_source_and_reindex";
    if (freshness == null or !eql(std.mem.span(freshness.?), "metadata_match")) return "read_source_and_reindex";
    if (eql(status, "partial")) return "read_ranges_and_verify_scope";
    if (eql(status, "skipped") or eql(status, "unusable")) return "read_source_directly";
    return "use_graph_with_best_effort_caveat";
}

fn inScope(path_c: CStr, scope: []const u8) bool {
    const path = std.mem.span(path_c);
    return scope.len == 0 or (std.mem.startsWith(u8, path, scope) and (path.len == scope.len or path[scope.len] == '/'));
}

/// One exact path. row is the File row for rel, if any.
fn coverageAddPathJson(doc: ?*MutDoc, item: ?*MutVal, rel: CStr, row: ?*const c.cbm_zova_file_coverage_t, root_path: CStr, recorded: bool, gitignore: ?*const c.cbm_gitignore_t, cbmignore: ?*const c.cbm_gitignore_t) void {
    _ = c.yyjson_mut_obj_add_strcpy(doc, item, "path", rel);
    const r = row orelse {
        var st: c.struct_stat = undefined;
        const rc: StatResult = if (root_path != null) coverageStat(root_path, rel, &st) else .missing;
        const excluded = if (rc == .outside) null else coverageExclusion(rel, gitignore, cbmignore);
        const status: Str = if (rc == .outside)
            "outside_project"
        else if (excluded != null)
            "excluded"
        else if (rc == .exists)
            "not_indexed"
        else
            "missing";
        _ = c.yyjson_mut_obj_add_str(doc, item, "status", status);
        if (excluded) |e| _ = c.yyjson_mut_obj_add_str(doc, item, "reason", e);
        _ = c.yyjson_mut_obj_add_str(doc, item, "freshness", if (rc == .exists) "not_tracked" else "missing");
        _ = c.yyjson_mut_obj_add_str(doc, item, "recommended_action", if (rc == .exists or excluded != null) recommendedAction(status, null) else "verify_path");
        return;
    };
    var cf = CoverageFile.parse(r);
    defer cf.deinit();
    const status: Str = if (cf.is("no_recorded_issue") and !recorded) "coverage_unavailable" else cf.status;
    const freshness = coverageFreshness(root_path, r);
    _ = c.yyjson_mut_obj_add_str(doc, item, "status", status);
    _ = c.yyjson_mut_obj_add_str(doc, item, "freshness", freshness);
    _ = c.yyjson_mut_obj_add_str(doc, item, "recommended_action", recommendedAction(status, freshness));
    coverageAddDetail(doc, item, &cf);
}

/// Scope counts and the paginated gap list. rows is the workspace's full
/// sorted File list.
fn coverageAddScopeJson(doc: ?*MutDoc, item: ?*MutVal, scope_c: CStr, rows: []const c.cbm_zova_file_coverage_t, recorded: bool, offset: c_int, limit: c_int) void {
    const scope = std.mem.span(scope_c);
    _ = c.yyjson_mut_obj_add_strcpy(doc, item, "scope", if (scope.len > 0) scope_c else ".");
    var files: c_int = 0;
    var partial: c_int = 0;
    var unusable: c_int = 0;
    var skipped: c_int = 0;
    var gaps: c_int = 0;
    const entries = c.yyjson_mut_arr(doc);
    for (rows) |*row| {
        if (!inScope(row.file_path, scope)) continue;
        files += 1;
        var cf = CoverageFile.parse(row);
        defer cf.deinit();
        if (cf.is("no_recorded_issue")) continue;
        if (cf.is("partial")) {
            partial += 1;
        } else if (cf.is("unusable")) {
            unusable += 1;
        } else {
            skipped += 1;
        }
        if (gaps >= offset and gaps < offset + limit) {
            const entry = c.yyjson_mut_obj(doc);
            _ = c.yyjson_mut_obj_add_strcpy(doc, entry, "path", row.file_path);
            _ = c.yyjson_mut_obj_add_str(doc, entry, "status", cf.status);
            coverageAddDetail(doc, entry, &cf);
            _ = c.yyjson_mut_arr_add_val(entries, entry);
        }
        gaps += 1;
    }
    const status: Str = if (gaps > 0)
        "known_gaps"
    else if (!recorded)
        "coverage_unavailable"
    else if (files == 0)
        "no_indexed_files"
    else
        "no_recorded_issue";
    _ = c.yyjson_mut_obj_add_str(doc, item, "status", status);
    _ = c.yyjson_mut_obj_add_int(doc, item, "files", files);
    _ = c.yyjson_mut_obj_add_int(doc, item, "no_recorded_issue", files - partial - unusable - skipped);
    _ = c.yyjson_mut_obj_add_int(doc, item, "partial", partial);
    _ = c.yyjson_mut_obj_add_int(doc, item, "unusable", unusable);
    _ = c.yyjson_mut_obj_add_int(doc, item, "skipped", skipped);
    _ = c.yyjson_mut_obj_add_int(doc, item, "total_gaps", gaps);
    const end = @min(offset + limit, gaps);
    _ = c.yyjson_mut_obj_add_bool(doc, item, "has_more", end < gaps);
    if (end < gaps) _ = c.yyjson_mut_obj_add_int(doc, item, "next_offset", end);
    _ = c.yyjson_mut_obj_add_val(doc, item, "gaps", entries);
}

fn rowSlice(rows: [*c]c.cbm_zova_file_coverage_t, count: c_int) []const c.cbm_zova_file_coverage_t {
    return if (count > 0) rows[0..@intCast(count)] else &.{};
}

pub fn handleCheckIndexCoverage(_: *Server, args: CStr) HeapStr {
    const project = mcp.getProjectArg(args);
    var db_path: [1024]u8 = undefined;
    const repo = mcp.openRepository(&db_path, project) orelse return mcp.projectNotFoundResult(project);
    defer c.free(project);
    defer c.cbm_zova_repository_close(repo);

    const adoc = mcp.readJson(args);
    defer c.yyjson_doc_free(adoc);
    const aroot: ?*Val = if (adoc) |d| c.yyjson_doc_get_root(d) else null;
    const paths = c.yyjson_obj_get(aroot, "paths");
    const scopes = c.yyjson_obj_get(aroot, "scopes");
    const path_count: usize = if (c.yyjson_is_arr(paths)) c.yyjson_arr_size(paths) else 0;
    var scope_count: usize = if (c.yyjson_is_arr(scopes)) c.yyjson_arr_size(scopes) else 0;
    if (aroot == null or (paths != null and !c.yyjson_is_arr(paths)) or (scopes != null and !c.yyjson_is_arr(scopes)) or
        path_count > COVERAGE_PATH_MAX or scope_count > COVERAGE_SCOPE_MAX)
    {
        return textResult("paths and scopes must be arrays (max 128 paths and 32 scopes)", true);
    }
    // Neither given: summarize the whole project, as scopes ["."] would.
    const project_summary = path_count == 0 and scope_count == 0;
    if (project_summary) scope_count = 1;
    const scope_limit = std.math.clamp(mcp.intArg(args, "scope_limit", COVERAGE_SCOPE_DEFAULT_LIMIT), 1, COVERAGE_SCOPE_MAX_LIMIT);
    const scope_offset = @max(mcp.intArg(args, "scope_offset", 0), 0);

    const workspace_id = c.cbm_zova_repository_workspace_id(repo);
    var root_path: HeapStr = null;
    defer c.free(root_path);
    var generation: i64 = 0;
    var recorded = false;
    var all: [*c]c.cbm_zova_file_coverage_t = null;
    var all_count: c_int = 0;
    defer c.cbm_zova_file_coverage_free(all, all_count);
    // Scopes need the full list; exact paths read one row each. The first
    // read also fetches the root, generation and marker.
    var rc = c.cbm_zova_repository_file_coverage(repo, workspace_id, if (scope_count > 0) null else "", &all, &all_count, &root_path, &generation, &recorded);
    const all_rows = rowSlice(all, all_count);

    var gitignore: ?*c.cbm_gitignore_t = null;
    var cbmignore: ?*c.cbm_gitignore_t = null;
    defer c.cbm_gitignore_free(gitignore);
    defer c.cbm_gitignore_free(cbmignore);
    if (rc == c.CBM_STORE_OK and root_path != null and path_count > 0) {
        var ignore_path: [4096]u8 = undefined;
        gitignore = c.cbm_gitignore_load(fmtZ(&ignore_path, "{s}/.gitignore", .{std.mem.span(root_path)}));
        cbmignore = c.cbm_gitignore_load(fmtZ(&ignore_path, "{s}/.cbmignore", .{std.mem.span(root_path)}));
    }

    const doc = c.yyjson_mut_doc_new(null);
    defer c.yyjson_mut_doc_free(doc);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_strcpy(doc, root, "project", project);
    _ = c.yyjson_mut_obj_add_str(doc, root, "signal", "best_effort");
    _ = c.yyjson_mut_obj_add_sint(doc, root, "generation", generation);
    _ = c.yyjson_mut_obj_add_bool(doc, root, "coverage_recorded", recorded);

    const path_results = c.yyjson_mut_arr(doc);
    var it: c.yyjson_arr_iter = undefined;
    _ = c.yyjson_arr_iter_init(paths, &it);
    while (c.yyjson_arr_iter_next(&it)) |value| {
        if (rc != c.CBM_STORE_OK) break;
        const item = c.yyjson_mut_obj(doc);
        const input = c.yyjson_get_str(value);
        _ = c.yyjson_mut_obj_add_strcpy(doc, item, "requested_path", orEmpty(input));
        var rel: [4096]u8 = undefined;
        const normalized = normalizeRel(input, false, &rel);
        if (normalized != .ok) {
            _ = c.yyjson_mut_obj_add_str(doc, item, "status", if (normalized == .outside) "outside_project" else "invalid_path");
            _ = c.yyjson_mut_obj_add_str(doc, item, "freshness", "unavailable");
            _ = c.yyjson_mut_obj_add_str(doc, item, "recommended_action", "use_project_relative_path");
            _ = c.yyjson_mut_arr_add_val(path_results, item);
            continue;
        }
        if (scope_count > 0) {
            // The full list is sorted by path.
            const rel_s = mcp.bufStr(&rel);
            const lo = std.sort.lowerBound(c.cbm_zova_file_coverage_t, all_rows, rel_s, struct {
                fn order(key: []const u8, row: c.cbm_zova_file_coverage_t) std.math.Order {
                    return std.mem.order(u8, key, std.mem.span(row.file_path));
                }
            }.order);
            const found = lo < all_rows.len and eql(std.mem.span(all_rows[lo].file_path), rel_s);
            coverageAddPathJson(doc, item, &rel, if (found) &all_rows[lo] else null, root_path, recorded, gitignore, cbmignore);
        } else {
            var one: [*c]c.cbm_zova_file_coverage_t = null;
            var one_count: c_int = 0;
            rc = c.cbm_zova_repository_file_coverage(repo, workspace_id, &rel, &one, &one_count, null, null, null);
            if (rc == c.CBM_STORE_OK) {
                coverageAddPathJson(doc, item, &rel, if (one_count > 0) &one[0] else null, root_path, recorded, gitignore, cbmignore);
                c.cbm_zova_file_coverage_free(one, one_count);
            }
        }
        _ = c.yyjson_mut_arr_add_val(path_results, item);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "paths", path_results);

    const scope_results = c.yyjson_mut_arr(doc);
    _ = c.yyjson_arr_iter_init(scopes, &it);
    while (c.yyjson_arr_iter_next(&it)) |value| {
        if (rc != c.CBM_STORE_OK) break;
        const item = c.yyjson_mut_obj(doc);
        const input = c.yyjson_get_str(value);
        _ = c.yyjson_mut_obj_add_strcpy(doc, item, "requested_scope", orEmpty(input));
        var scope: [4096]u8 = undefined;
        const normalized = normalizeRel(input, true, &scope);
        if (normalized != .ok) {
            _ = c.yyjson_mut_obj_add_str(doc, item, "status", if (normalized == .outside) "outside_project" else "invalid_path");
        } else {
            coverageAddScopeJson(doc, item, &scope, all_rows, recorded, scope_offset, scope_limit);
        }
        _ = c.yyjson_mut_arr_add_val(scope_results, item);
    }
    if (project_summary and rc == c.CBM_STORE_OK) {
        const item = c.yyjson_mut_obj(doc);
        _ = c.yyjson_mut_obj_add_str(doc, item, "requested_scope", ".");
        coverageAddScopeJson(doc, item, "", all_rows, recorded, scope_offset, scope_limit);
        _ = c.yyjson_mut_arr_add_val(scope_results, item);
    }
    _ = c.yyjson_mut_obj_add_val(doc, root, "scopes", scope_results);
    if (scope_count > 0) {
        _ = c.yyjson_mut_obj_add_str(doc, root, "scope_note", "Scopes list indexed files only. Files excluded by ignore rules " ++
            "or unsupported languages are not enumerable; check them as " ++
            "exact paths.");
    }
    _ = c.yyjson_mut_obj_add_str(doc, root, "caveat", "Best-effort signal about extraction only. no_recorded_issue does not prove graph or " ++
        "source completeness; read flagged source and qualify claims when freshness is not " ++
        "metadata_match.");

    if (rc != c.CBM_STORE_OK) return textResult("shared cbm.zova coverage read failed", true);
    return mcp.textResultOwned(mcp.docToStr(doc), false);
}

/// index_status summary: counts over the active generation plus a few gap
/// examples. Leaves root untouched when the read fails.
pub fn addCoverageSummary(doc: ?*MutDoc, root: ?*MutVal, repo: ?*c.cbm_zova_repository_t, workspace_id: CStr) void {
    var rows: [*c]c.cbm_zova_file_coverage_t = null;
    var count: c_int = 0;
    var generation: i64 = 0;
    var recorded = false;
    if (c.cbm_zova_repository_file_coverage(repo, workspace_id, null, &rows, &count, null, &generation, &recorded) != c.CBM_STORE_OK) return;
    defer c.cbm_zova_file_coverage_free(rows, count);
    const cov = c.yyjson_mut_obj(doc);
    _ = c.yyjson_mut_obj_add_sint(doc, cov, "generation", generation);
    _ = c.yyjson_mut_obj_add_bool(doc, cov, "recorded", recorded);
    coverageAddScopeJson(doc, cov, "", rowSlice(rows, count), recorded, 0, COVERAGE_STATUS_EXAMPLES);
    _ = c.yyjson_mut_obj_remove_key(cov, "scope");
    _ = c.yyjson_mut_obj_add_str(doc, cov, "note", "Extraction completeness only. Use check_index_coverage for exact " ++
        "paths, scopes and paging.");
    _ = c.yyjson_mut_obj_add_val(doc, root, "coverage", cov);
}

/// get_code_snippet: flag a partially parsed or skipped source file. NULL
/// when the file has no recorded gap. Caller frees.
pub fn coverageSnippetNote(repo: ?*c.cbm_zova_repository_t, workspace_id: CStr, file_path: CStr) HeapStr {
    var rows: [*c]c.cbm_zova_file_coverage_t = null;
    var count: c_int = 0;
    if (file_path == null or file_path[0] == 0 or
        c.cbm_zova_repository_file_coverage(repo, workspace_id, file_path, &rows, &count, null, null, null) != c.CBM_STORE_OK)
    {
        return null;
    }
    defer c.cbm_zova_file_coverage_free(rows, count);
    if (count <= 0) return null;
    var cf = CoverageFile.parse(&rows[0..1][0]);
    defer cf.deinit();
    var buf: [1024]u8 = undefined;
    if (cf.ranges != null) {
        return mcp.dup(fmtZ(&buf, "The parser recovered from errors in this file (lines {s}); the graph may " ++
            "miss definitions or calls there. Read the source directly.", .{if (cf.ranges[0] != 0) std.mem.span(cf.ranges) else "unknown"}));
    }
    if (cf.phase != null) {
        const reason = std.mem.span(orEmpty(cf.reason));
        return mcp.dup(fmtZ(&buf, "This file was not extracted ({s}{s}{s}); the graph may miss its definitions. " ++
            "Read the source directly.", .{ std.mem.span(cf.phase), if (reason.len > 0) ": " else "", reason }));
    }
    return null;
}

// ── index_status ─────────────────────────────────────────────────

fn addWatchStatusJson(srv: *Server, doc: ?*MutDoc, root: ?*MutVal, project: CStr) void {
    const watch = c.yyjson_mut_obj(doc);
    _ = c.yyjson_mut_obj_add_val(doc, root, "watch", watch);

    if (srv.watcher == null) {
        _ = c.yyjson_mut_obj_add_bool(doc, watch, "watched", false);
        _ = c.yyjson_mut_obj_add_str(doc, watch, "reason", "watcher_disabled");
        return;
    }

    if (srv.config != null and !c.cbm_config_get_bool(srv.config, c.CBM_CONFIG_AUTO_WATCH, true)) {
        _ = c.yyjson_mut_obj_add_bool(doc, watch, "watched", false);
        _ = c.yyjson_mut_obj_add_str(doc, watch, "reason", "auto_watch_off");
        return;
    }

    var info = std.mem.zeroes(c.cbm_watcher_project_info_t);
    if (c.cbm_watcher_project_info(srv.watcher, project, &info)) {
        if (info.strategy == c.CBM_WATCHER_STRATEGY_GIT) {
            _ = c.yyjson_mut_obj_add_bool(doc, watch, "watched", true);
            _ = c.yyjson_mut_obj_add_str(doc, watch, "strategy", "git");
            return;
        } else {
            _ = c.yyjson_mut_obj_add_bool(doc, watch, "watched", false);
            _ = c.yyjson_mut_obj_add_str(doc, watch, "reason", "not_a_git_repository");
            return;
        }
    }

    _ = c.yyjson_mut_obj_add_bool(doc, watch, "watched", false);
    if (srv.session_project[0] != 0 and !mcp.cEql(&srv.session_project, project)) {
        _ = c.yyjson_mut_obj_add_str(doc, watch, "reason", "not_session_project");
    } else {
        _ = c.yyjson_mut_obj_add_str(doc, watch, "reason", "not_registered");
    }
}

pub fn handleIndexStatus(srv: *Server, args: CStr) HeapStr {
    const project = mcp.getProjectArg(args);
    var path: [1024]u8 = undefined;
    const repo = mcp.openRepository(&path, project) orelse return mcp.projectNotFoundResult(project);
    defer c.free(project);
    var nodes: c_int = 0;
    var edges: c_int = 0;
    var info = std.mem.zeroes(c.cbm_project_t);
    defer c.cbm_project_free_fields(&info);
    const workspace_id = c.cbm_zova_repository_workspace_id(repo);
    var rc = c.cbm_zova_repository_counts(repo, workspace_id, &nodes, &edges);
    if (rc == c.CBM_STORE_OK) rc = c.cbm_zova_repository_get_project(repo, workspace_id, &info);
    if (rc != c.CBM_STORE_OK) {
        c.cbm_zova_repository_close(repo);
        return textResult("shared cbm.zova status read failed", true);
    }
    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_str(doc, root, "project", project);
    _ = c.yyjson_mut_obj_add_int(doc, root, "nodes", nodes);
    _ = c.yyjson_mut_obj_add_int(doc, root, "edges", edges);
    _ = c.yyjson_mut_obj_add_str(doc, root, "status", if (nodes > 0) "ready" else "empty");
    _ = c.yyjson_mut_obj_add_strcpy(doc, root, "root_path", orEmpty(info.root_path));
    addCoverageSummary(doc, root, repo, workspace_id);
    c.cbm_zova_repository_close(repo);
    mcp.addGitContextJson(doc, root, info.root_path);
    addWatchStatusJson(srv, doc, root, project);
    return mcp.docResult(doc);
}

// ── delete_project ───────────────────────────────────────────────

const DELETE_RETRY_MS = 50;

/// delete_project: delete the project's workspace from the shared cbm.zova.
pub fn handleDeleteProject(srv: *Server, args: CStr) HeapStr {
    const name = mcp.getProjectArg(args);
    if (name == null) return textResult("project is required", true);
    defer c.free(name);

    // Close store if it's the project being deleted
    if (srv.current_project != null and mcp.cEql(srv.current_project, name)) {
        if (srv.owns_store and srv.store != null) {
            c.cbm_store_close(srv.store);
            srv.store = null;
        }
        c.free(srv.current_project);
        srv.current_project = null;
    }

    var path = std.mem.zeroes([1024]u8);
    var status: Str = "not_found";
    var error_detail: CStr = null;
    var is_error = false;
    var report = std.mem.zeroes(c.cbm_zova_operation_report_t);

    var workspace_id = std.mem.zeroes([256]u8);
    if (mcp.openRepository(&path, name)) |repo| {
        _ = mcp.bprint(&workspace_id, "{s}", .{std.mem.span(c.cbm_zova_repository_workspace_id(repo))});
        c.cbm_zova_repository_close(repo);
    }
    // The delete takes the workspace lease without waiting; wait here
    // (bounded) for an index run of this workspace to finish.
    var code: c.cbm_zova_operation_code_t = c.CBM_ZOVA_OPERATION_NOOP;
    var wait_left_ms = c.cbm_zova_coord_default_wait_ms();
    while (workspace_id[0] != 0) : (wait_left_ms -= DELETE_RETRY_MS) {
        code = c.cbm_zova_workspace_delete(&path, &workspace_id, &workspace_id, &report);
        if (code != c.CBM_ZOVA_OPERATION_BUSY or !eql(mcp.bufStr(&report.reason), "workspace_busy") or wait_left_ms <= 0) break;
        _ = c.cbm_usleep(@as(c_uint, DELETE_RETRY_MS * 1000));
    }
    if (code == c.CBM_ZOVA_OPERATION_OK) {
        status = "deleted";
    } else if (code == c.CBM_ZOVA_OPERATION_NOOP) {
        is_error = true;
    } else {
        status = "delete_failed";
        error_detail = &report.reason;
        is_error = true;
    }

    if (srv.watcher != null) c.cbm_watcher_unwatch(srv.watcher, name);

    c.cbm_mem_collect(); // return freed pages to OS after closing database

    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    _ = c.yyjson_mut_obj_add_str(doc, root, "project", name);
    _ = c.yyjson_mut_obj_add_str(doc, root, "status", status);
    if (error_detail != null) _ = c.yyjson_mut_obj_add_str(doc, root, "error", error_detail);
    const json = mcp.docToStr(doc);
    c.yyjson_mut_doc_free(doc);
    return mcp.textResultOwned(json, is_error);
}

// ── get_architecture ─────────────────────────────────────────────

/// Canonical list of valid aspect tokens for get_architecture. Single source
/// of truth for the server-side validation (authoritative); the JSON-Schema
/// enum in the tool table is the advisory client-side mirror — update both
/// together when the aspect set changes.
const valid_aspects = [_][]const u8{
    "all",          "overview", "structure",  "dependencies", "routes",    "languages", "packages",
    "entry_points", "hotspots", "boundaries", "layers",       "file_tree", "clusters",
};

fn aspectIsValid(name: CStr) bool {
    if (name == null) return false;
    for (valid_aspects) |a| {
        if (eql(std.mem.span(name), a)) return true;
    }
    return false;
}

/// Check if an aspect is requested. No aspects = all. The array can contain
/// "all" (everything), "overview" (everything except file_tree — see
/// cbm_store_arch_aspect_in_overview), or the aspect name itself.
fn aspectWanted(aspects_arr: ?*Val, name: Str) bool {
    if (aspects_arr == null) return true; // no filter = all
    var iter: c.yyjson_arr_iter = undefined;
    _ = c.yyjson_arr_iter_init(aspects_arr, &iter);
    while (c.yyjson_arr_iter_next(&iter)) |val| {
        const s = c.yyjson_get_str(val);
        if (s == null) continue;
        const sv = std.mem.span(s);
        if (eql(sv, "all")) return true;
        if (eql(sv, "overview") and c.cbm_store_arch_aspect_in_overview(name)) return true;
        if (eql(sv, std.mem.span(name))) return true;
    }
    return false;
}

fn addStrOrEmpty(doc: ?*MutDoc, obj: ?*MutVal, key: Str, val: CStr) void {
    _ = c.yyjson_mut_obj_add_str(doc, obj, key, orEmpty(val));
}

fn countOf(n: c_int) usize {
    return @intCast(@max(n, 0));
}

pub fn handleGetArchitecture(srv: *Server, args: CStr) HeapStr {
    // The yyjson inline helpers count toward the branch quota.
    @setEvalBranchQuota(10000);
    const project = mcp.getProjectArg(args);
    const scope_path = mcp.strArg(args, "path");
    defer c.free(scope_path);
    const store = mcp.resolveStore(srv, project) orelse return mcp.projectNotFoundResult(project);
    defer c.free(project);

    const not_indexed = mcp.verifyProjectIndexed(store, project);
    if (not_indexed != null) return not_indexed;

    // Parse aspects array from args; the doc stays alive while aspects_strs
    // points into it.
    var aspects_doc: ?*c.yyjson_doc = null;
    var aspects_arr: ?*Val = null;
    if (mcp.readJson(args)) |args_doc| {
        const aval = c.yyjson_obj_get(c.yyjson_doc_get_root(args_doc), "aspects");
        if (c.yyjson_is_arr(aval)) {
            aspects_doc = args_doc;
            aspects_arr = aval;
        } else {
            c.yyjson_doc_free(args_doc);
        }
    }
    defer if (aspects_doc) |d| c.yyjson_doc_free(d);

    var aspects_strs: [16]CStr = undefined;
    var aspects_strs_count: c_int = 0;
    if (aspects_arr != null) {
        var it: c.yyjson_arr_iter = undefined;
        _ = c.yyjson_arr_iter_init(aspects_arr, &it);
        while (c.yyjson_arr_iter_next(&it)) |aspect_val| {
            const s = c.yyjson_get_str(aspect_val);
            if (s != null and aspects_strs_count < aspects_strs.len) {
                aspects_strs[@intCast(aspects_strs_count)] = s;
                aspects_strs_count += 1;
            }
        }
    }

    // Server-side validation: reject unknown aspect tokens with an isError
    // result listing the valid values. The JSON-Schema enum is advisory —
    // many MCP clients do not validate arguments against tool schemas — so
    // without this check a typo degraded to a silent near-empty payload.
    for (aspects_strs[0..countOf(aspects_strs_count)]) |aspect| {
        if (!aspectIsValid(aspect)) {
            var valid_list: [256]u8 = undefined;
            var off: usize = 0;
            for (valid_aspects, 0..) |a, j| {
                off += mcp.bprint(valid_list[off..], "{s}{s}", .{ if (j > 0) ", " else "", a });
                if (off >= valid_list.len) break;
            }
            var msg: [512]u8 = undefined;
            return textResult(fmtZ(&msg, "Unknown aspect '{s}'. Valid: {s}.", .{ std.mem.span(aspect), mcp.bufStr(&valid_list) }), true);
        }
    }

    var schema = std.mem.zeroes(c.cbm_schema_info_t);
    defer c.cbm_store_schema_free(&schema);
    // Counts-only: this handler renders label/type counts but never property
    // keys, and full key discovery json_each-scans every row (seconds-to-
    // minutes on multi-million-node graphs).
    const schema_rc = c.cbm_store_get_schema_counts_scoped(store, project, scope_path, &schema);

    var arch = std.mem.zeroes(c.cbm_architecture_info_t);
    defer c.cbm_store_architecture_free(&arch);
    const arch_rc = c.cbm_store_get_architecture(store, project, scope_path, if (aspects_strs_count > 0) @ptrCast(&aspects_strs) else null, aspects_strs_count, &arch);

    const node_count = c.cbm_store_count_nodes_scoped(store, project, scope_path);
    const edge_count = c.cbm_store_count_edges_scoped(store, project, scope_path);
    if (schema_rc != c.CBM_STORE_OK or arch_rc != c.CBM_STORE_OK or node_count < 0 or edge_count < 0) {
        return textResult("failed to read the project architecture", true);
    }
    var norm_path: [512]u8 = undefined;
    const path_scoped = c.cbm_store_normalize_arch_path(scope_path, &norm_path, norm_path.len);

    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);

    _ = c.yyjson_mut_obj_add_str(doc, root, "project", project);
    if (path_scoped) {
        _ = c.yyjson_mut_obj_add_str(doc, root, "path", &norm_path);
        _ = c.yyjson_mut_obj_add_int(doc, root, "root_total_nodes", c.cbm_store_count_nodes(store, project));
        _ = c.yyjson_mut_obj_add_int(doc, root, "root_total_edges", c.cbm_store_count_edges(store, project));
        _ = c.yyjson_mut_obj_add_int(doc, root, "scoped_total_nodes", node_count);
        _ = c.yyjson_mut_obj_add_int(doc, root, "scoped_total_edges", edge_count);
    }
    _ = c.yyjson_mut_obj_add_int(doc, root, "total_nodes", node_count);
    _ = c.yyjson_mut_obj_add_int(doc, root, "total_edges", edge_count);

    // Node label summary
    if (aspectWanted(aspects_arr, "structure")) {
        const labels = c.yyjson_mut_arr(doc);
        for (schema.node_labels[0..countOf(schema.node_label_count)]) |nl| {
            const item = c.yyjson_mut_obj(doc);
            _ = c.yyjson_mut_obj_add_str(doc, item, "label", nl.label);
            _ = c.yyjson_mut_obj_add_int(doc, item, "count", nl.count);
            _ = c.yyjson_mut_arr_add_val(labels, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "node_labels", labels);
    }

    // Edge type summary
    if (aspectWanted(aspects_arr, "dependencies")) {
        const types = c.yyjson_mut_arr(doc);
        for (schema.edge_types[0..countOf(schema.edge_type_count)]) |et| {
            const item = c.yyjson_mut_obj(doc);
            _ = c.yyjson_mut_obj_add_str(doc, item, "type", et.type);
            _ = c.yyjson_mut_obj_add_int(doc, item, "count", et.count);
            _ = c.yyjson_mut_arr_add_val(types, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "edge_types", types);
    }

    // Relationship patterns
    if (aspectWanted(aspects_arr, "routes") and schema.rel_pattern_count > 0) {
        const pats = c.yyjson_mut_arr(doc);
        for (schema.rel_patterns[0..countOf(schema.rel_pattern_count)]) |pat| _ = c.yyjson_mut_arr_add_str(doc, pats, pat);
        _ = c.yyjson_mut_obj_add_val(doc, root, "relationship_patterns", pats);
    }

    if (arch.language_count > 0) {
        const langs = c.yyjson_mut_arr(doc);
        for (arch.languages[0..countOf(arch.language_count)]) |l| {
            const item = c.yyjson_mut_obj(doc);
            addStrOrEmpty(doc, item, "language", l.language);
            _ = c.yyjson_mut_obj_add_int(doc, item, "file_count", l.file_count);
            _ = c.yyjson_mut_arr_add_val(langs, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "languages", langs);
    }

    if (arch.package_count > 0) {
        const pkgs = c.yyjson_mut_arr(doc);
        for (arch.packages[0..countOf(arch.package_count)]) |p| {
            const item = c.yyjson_mut_obj(doc);
            addStrOrEmpty(doc, item, "name", p.name);
            _ = c.yyjson_mut_obj_add_int(doc, item, "node_count", p.node_count);
            _ = c.yyjson_mut_obj_add_int(doc, item, "fan_in", p.fan_in);
            _ = c.yyjson_mut_obj_add_int(doc, item, "fan_out", p.fan_out);
            _ = c.yyjson_mut_arr_add_val(pkgs, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "packages", pkgs);
    }

    if (arch.entry_point_count > 0) {
        const eps = c.yyjson_mut_arr(doc);
        for (arch.entry_points[0..countOf(arch.entry_point_count)]) |e| {
            const item = c.yyjson_mut_obj(doc);
            addStrOrEmpty(doc, item, "name", e.name);
            addStrOrEmpty(doc, item, "qualified_name", e.qualified_name);
            addStrOrEmpty(doc, item, "file", e.file);
            _ = c.yyjson_mut_arr_add_val(eps, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "entry_points", eps);
    }

    if (arch.route_count > 0) {
        const routes = c.yyjson_mut_arr(doc);
        for (arch.routes[0..countOf(arch.route_count)]) |r| {
            const item = c.yyjson_mut_obj(doc);
            addStrOrEmpty(doc, item, "method", r.method);
            addStrOrEmpty(doc, item, "path", r.path);
            addStrOrEmpty(doc, item, "handler", r.handler);
            _ = c.yyjson_mut_arr_add_val(routes, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "routes", routes);
    }

    if (arch.hotspot_count > 0) {
        const hotspots = c.yyjson_mut_arr(doc);
        for (arch.hotspots[0..countOf(arch.hotspot_count)]) |h| {
            const item = c.yyjson_mut_obj(doc);
            addStrOrEmpty(doc, item, "name", h.name);
            addStrOrEmpty(doc, item, "qualified_name", h.qualified_name);
            _ = c.yyjson_mut_obj_add_int(doc, item, "fan_in", h.fan_in);
            _ = c.yyjson_mut_arr_add_val(hotspots, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "hotspots", hotspots);
    }

    // Cross-package boundaries
    if (arch.boundary_count > 0) {
        const boundaries = c.yyjson_mut_arr(doc);
        for (arch.boundaries[0..countOf(arch.boundary_count)]) |b| {
            const item = c.yyjson_mut_obj(doc);
            addStrOrEmpty(doc, item, "from", b.from);
            addStrOrEmpty(doc, item, "to", b.to);
            _ = c.yyjson_mut_obj_add_int(doc, item, "call_count", b.call_count);
            _ = c.yyjson_mut_arr_add_val(boundaries, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "boundaries", boundaries);
    }

    // Cross-service links (HTTP/async between services)
    if (arch.service_count > 0) {
        const services = c.yyjson_mut_arr(doc);
        for (arch.services[0..countOf(arch.service_count)]) |s| {
            const item = c.yyjson_mut_obj(doc);
            addStrOrEmpty(doc, item, "from", s.from);
            addStrOrEmpty(doc, item, "to", s.to);
            addStrOrEmpty(doc, item, "type", s.type);
            _ = c.yyjson_mut_obj_add_int(doc, item, "count", s.count);
            _ = c.yyjson_mut_arr_add_val(services, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "services", services);
    }

    if (arch.layer_count > 0) {
        const layers = c.yyjson_mut_arr(doc);
        for (arch.layers[0..countOf(arch.layer_count)]) |l| {
            const item = c.yyjson_mut_obj(doc);
            addStrOrEmpty(doc, item, "name", l.name);
            addStrOrEmpty(doc, item, "layer", l.layer);
            addStrOrEmpty(doc, item, "reason", l.reason);
            _ = c.yyjson_mut_arr_add_val(layers, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "layers", layers);
    }

    // Clusters (community detection)
    if (arch.cluster_count > 0) {
        const clusters = c.yyjson_mut_arr(doc);
        for (arch.clusters[0..countOf(arch.cluster_count)]) |*cl| {
            const item = c.yyjson_mut_obj(doc);
            _ = c.yyjson_mut_obj_add_int(doc, item, "id", cl.id);
            addStrOrEmpty(doc, item, "label", cl.label);
            _ = c.yyjson_mut_obj_add_int(doc, item, "members", cl.members);
            _ = c.yyjson_mut_obj_add_real(doc, item, "cohesion", cl.cohesion);
            const lists = .{
                .{ "top_nodes", cl.top_nodes, cl.top_node_count },
                .{ "packages", cl.packages, cl.package_count },
                .{ "edge_types", cl.edge_types, cl.edge_type_count },
            };
            inline for (lists) |l| {
                const arr = c.yyjson_mut_arr(doc);
                for (0..countOf(l[2])) |j| _ = c.yyjson_mut_arr_add_str(doc, arr, orEmpty(l[1][j]));
                _ = c.yyjson_mut_obj_add_val(doc, item, l[0], arr);
            }
            _ = c.yyjson_mut_arr_add_val(clusters, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "clusters", clusters);
    }

    if (arch.file_tree_count > 0) {
        const file_tree = c.yyjson_mut_arr(doc);
        for (arch.file_tree[0..countOf(arch.file_tree_count)]) |f| {
            const item = c.yyjson_mut_obj(doc);
            addStrOrEmpty(doc, item, "path", f.path);
            addStrOrEmpty(doc, item, "type", f.type);
            _ = c.yyjson_mut_obj_add_int(doc, item, "children", f.children);
            _ = c.yyjson_mut_arr_add_val(file_tree, item);
        }
        _ = c.yyjson_mut_obj_add_val(doc, root, "file_tree", file_tree);
    }

    // The doc borrows schema/arch strings: serialize before the deferred frees.
    return mcp.docResult(doc);
}
