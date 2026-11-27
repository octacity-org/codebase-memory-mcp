//! discover.zig — Recursive directory walk with filtering.
//!
//! Implements the discovery part of src/discover/discover.h.
//!
//! Walks a repository directory tree, applying:
//!   1. Hardcoded directory skip patterns (60+ dirs like .git, node_modules)
//!   2. Hardcoded suffix filters (.pyc, .png, .wasm, etc.)
//!   3. Fast-mode additional filters (docs, examples, lock files, etc.)
//!   4. Gitignore-style pattern matching
//!   5. Language detection for accepted files
//!
//! The file and excluded-directory arrays are malloc'd: C callers free them
//! with cbm_discover_free and cbm_discover_free_excluded.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const gitignore = @import("gitignore.zig");
const language = @import("language.zig");

const is_windows = builtin.os.tag == .windows;

const Str = [*:0]const u8;
const bprint = interop.bprint;
const Gitignore = gitignore.Gitignore;
const Lang = c.CBMLanguage;
const Mode = c.cbm_index_mode_t;

const LANG_UNKNOWN: Lang = c.CBM_LANG_COUNT;
const LANG_JSON: Lang = c.CBM_LANG_JSON;
const MODE_FULL: Mode = c.CBM_MODE_FULL;

const PATH_MAX = 4096;

// ── Hardcoded always-skip directories ──────────────────────────────

const ALWAYS_SKIP_DIRS = [_][]const u8{
    // VCS
    ".git",
    ".hg",
    ".svn",
    ".worktrees",
    // IDE
    ".idea",
    ".vs",
    ".vscode",
    ".eclipse",
    ".claude",
    ".claude-worktrees",
    "Antigravity",
    // Python
    ".cache",
    ".eggs",
    ".env",
    ".mypy_cache",
    ".nox",
    ".pytest_cache",
    ".ruff_cache",
    ".tox",
    ".venv",
    "__pycache__",
    "env",
    "htmlcov",
    "site-packages",
    "venv",
    // JS/TS
    ".npm",
    ".nyc_output",
    ".pnpm-store",
    ".yarn",
    "bower_components",
    "coverage",
    "node_modules",
    ".next",
    ".nuxt",
    ".svelte-kit",
    ".angular",
    ".turbo",
    ".parcel-cache",
    ".docusaurus",
    ".expo",
    // Build artifacts
    "dist",
    "obj",
    "Pods",
    "target",
    "temp",
    "tmp",
    ".terraform",
    ".serverless",
    "bazel-bin",
    "bazel-out",
    "bazel-testlogs",
    // Language caches
    ".cargo",
    ".stack-work",
    ".dart_tool",
    "zig-cache",
    "zig-out",
    ".metals",
    ".bloop",
    ".bsp",
    ".ccls-cache",
    ".clangd",
    "elm-stuff",
    "_opam",
    ".cpcache",
    ".shadow-cljs",
    // Deploy
    ".vercel",
    ".netlify",
    "deploy",
    "deployed",
    // Misc
    ".qdrant_code_embeddings",
    ".tmp",
    "vendor",
    "vendored",
};

const FAST_SKIP_DIRS = [_][]const u8{
    "generated", "gen",           "auto-generated", "fixtures",     "testdata",    "test_data",
    "__tests__", "__mocks__",     "__snapshots__",  "__fixtures__", "__test__",    "docs",
    "doc",       "documentation", "examples",       "example",      "samples",     "sample",
    "assets",    "static",        "public",         "media",        "third_party", "thirdparty",
    "3rdparty",  "external",      "migrations",     "seeds",        "e2e",         "integration",
    "locale",    "locales",       "i18n",           "l10n",         "scripts",     "tools",
    "hack",      "bin",           "build",          "out",
};

// ── Ignored suffixes ───────────────────────────────────────────────

const ALWAYS_IGNORED_SUFFIXES = [_][]const u8{
    ".tmp",    "~",        ".pyc",  ".pyo",   ".o",   ".a",   ".so",  ".dll",
    ".class",  ".png",     ".jpg",  ".jpeg",  ".gif", ".ico", ".bmp", ".tiff",
    ".webp",   ".svg",     ".wasm", ".node",  ".exe", ".bin", ".dat", ".db",
    ".sqlite", ".sqlite3", ".woff", ".woff2", ".ttf", ".eot", ".otf",
};

const FAST_IGNORED_SUFFIXES = [_][]const u8{
    ".zip", ".tar",  ".gz",       ".bz2",  ".xz",  ".rar",    ".7z",      ".jar",
    ".war", ".ear",  ".mp3",      ".mp4",  ".avi", ".mov",    ".wav",     ".flac",
    ".ogg", ".mkv",  ".webm",     ".pdf",  ".doc", ".docx",   ".xls",     ".xlsx",
    ".ppt", ".pptx", ".odt",      ".ods",  ".map", ".min.js", ".min.css", ".pem",
    ".crt", ".key",  ".cer",      ".p12",  ".pb",  ".avro",   ".parquet", ".beam",
    ".elc", ".rlib", ".coverage", ".prof", ".out", ".patch",  ".diff",
};

// ── Fast-mode skip filenames ───────────────────────────────────────

const FAST_SKIP_FILENAMES = [_][]const u8{
    "LICENSE",        "LICENSE.txt",     "LICENSE.md",   "LICENSE-MIT",   "LICENSE-APACHE",
    "LICENCE",        "LICENCE.txt",     "LICENCE.md",   "CHANGELOG",     "CHANGELOG.md",
    "CHANGES.md",     "HISTORY",         "HISTORY.md",   "AUTHORS",       "AUTHORS.md",
    "CONTRIBUTORS",   "CONTRIBUTORS.md", "CODEOWNERS",   "go.sum",        "yarn.lock",
    "pnpm-lock.yaml", "Pipfile.lock",    "poetry.lock",  "Gemfile.lock",  "Cargo.lock",
    "mix.lock",       "flake.lock",      "pubspec.lock", "composer.lock", "package-lock.json",
    "configure",      "Makefile.in",     "config.guess", "config.sub",
};

// ── Fast-mode substring patterns ───────────────────────────────────

const FAST_PATTERNS = [_][]const u8{
    ".d.ts",      ".bundle.", ".chunk.", ".generated.",
    ".pb.go",     "_pb2.py",  ".pb2.py", "_grpc.pb.go",
    "_string.go", "mock_",    "_mock.",  "_test_helpers.",
    ".stories.",  ".spec.",   ".test.",
};

// ── Ignored JSON filenames ─────────────────────────────────────────

const IGNORED_JSON_FILES = [_][]const u8{
    "package.json",       "package-lock.json", "tsconfig.json",
    "jsconfig.json",      "composer.json",     "composer.lock",
    "yarn.lock",          "openapi.json",      "swagger.json",
    "jest.config.json",   ".eslintrc.json",    ".prettierrc.json",
    ".babelrc.json",      "tslint.json",       "angular.json",
    "firebase.json",      "renovate.json",     "lerna.json",
    "turbo.json",         ".stylelintrc.json", "pnpm-lock.json",
    "deno.json",          "biome.json",        "devcontainer.json",
    ".devcontainer.json", "launch.json",       "settings.json",
    "extensions.json",    "tasks.json",
};

// ── Helpers ────────────────────────────────────────────────────────

fn strInList(s: []const u8, list: []const []const u8) bool {
    for (list) |item| {
        if (std.mem.eql(u8, s, item)) return true;
    }
    return false;
}

/// The C string held in buf.
fn cut(buf: []const u8) []const u8 {
    return std.mem.sliceTo(buf, 0);
}

/// buf as a C string; buf must hold a NUL.
fn cstr(buf: []const u8) Str {
    return @ptrCast(buf.ptr);
}

fn isDir(st: *const c.struct_stat) bool {
    return (st.st_mode & c.S_IFMT) == c.S_IFDIR;
}

fn isReg(st: *const c.struct_stat) bool {
    return (st.st_mode & c.S_IFMT) == c.S_IFREG;
}

// ── Git global excludes resolution ─────────────────────────────────

/// C isspace in the "C" locale.
fn trimWs(s: []const u8) []const u8 {
    return std.mem.trim(u8, s, &std.ascii.whitespace);
}

fn stripInlineComment(s: []const u8) []const u8 {
    var in_quote = false;
    var quote: u8 = 0;
    for (s, 0..) |ch, i| {
        if ((ch == '"' or ch == '\'') and (i == 0 or s[i - 1] != '\\')) {
            if (!in_quote) {
                in_quote = true;
                quote = ch;
            } else if (ch == quote) {
                in_quote = false;
            }
            continue;
        }
        if (!in_quote and (ch == '#' or ch == ';') and (i == 0 or std.ascii.isWhitespace(s[i - 1]))) {
            return s[0..i];
        }
    }
    return s;
}

fn stripMatchingQuotes(s: []const u8) []const u8 {
    if (s.len >= 2 and ((s[0] == '"' and s[s.len - 1] == '"') or
        (s[0] == '\'' and s[s.len - 1] == '\'')))
    {
        return s[1 .. s.len - 1];
    }
    return s;
}

fn hasTrailingSep(path: []const u8) bool {
    return path.len > 0 and (path[path.len - 1] == '/' or path[path.len - 1] == '\\');
}

fn pathJoin(out: []u8, base: []const u8, rel: []const u8) void {
    if (base.len == 0) {
        _ = bprint(out, "{s}", .{rel});
    } else if (rel.len == 0) {
        _ = bprint(out, "{s}", .{base});
    } else if (hasTrailingSep(base)) {
        _ = bprint(out, "{s}{s}", .{ base, rel });
    } else {
        _ = bprint(out, "{s}/{s}", .{ base, rel });
    }
    _ = c.cbm_normalize_path_sep(out.ptr);
}

fn homeDir() ?[]const u8 {
    const home: Str = c.cbm_get_home_dir() orelse return null;
    if (home[0] == 0) return null;
    return std.mem.span(home);
}

fn expandGitPath(path: []const u8, out: []u8) bool {
    if (path.len == 0) return false;
    var normalized: [PATH_MAX]u8 = undefined;
    _ = bprint(&normalized, "{s}", .{path});
    _ = c.cbm_normalize_path_sep(&normalized);
    const norm = cut(&normalized);

    if (norm[0] != '~') {
        _ = bprint(out, "{s}", .{norm});
        _ = c.cbm_normalize_path_sep(out.ptr);
        return out[0] != 0;
    }

    if (norm.len > 1 and norm[1] != '/') {
        return false; // ~user expansion is intentionally not supported.
    }

    const home = homeDir() orelse return false;
    if (norm.len == 1) {
        _ = bprint(out, "{s}", .{home});
        _ = c.cbm_normalize_path_sep(out.ptr);
    } else {
        pathJoin(out, home, norm[2..]); // after "~/"
    }
    return out[0] != 0;
}

fn readCoreExcludesFile(config_path: Str, out: []u8) bool {
    const f = c.cbm_fopen(config_path, "r") orelse return false;
    defer _ = c.fclose(f);

    var in_core = false;
    var found = false;
    var line: [PATH_MAX]u8 = undefined;
    while (c.fgets(&line, line.len, f) != null) {
        const s = trimWs(cut(&line));
        if (s.len == 0 or s[0] == '#' or s[0] == ';') continue;

        if (s[0] == '[') {
            const end = std.mem.indexOfScalar(u8, s, ']') orelse {
                in_core = false;
                continue;
            };
            in_core = std.ascii.eqlIgnoreCase(trimWs(s[1..end]), "core");
            continue;
        }

        if (!in_core) continue;

        const eq = std.mem.indexOfScalar(u8, s, '=') orelse continue;
        const key = trimWs(s[0..eq]);
        const value = stripMatchingQuotes(trimWs(stripInlineComment(trimWs(s[eq + 1 ..]))));

        if (std.ascii.eqlIgnoreCase(key, "excludesfile") and value.len > 0 and
            expandGitPath(value, out))
        {
            found = true;
        }
    }
    return found;
}

fn resolveXdgGitConfigDir(out: []u8) bool {
    var env: [PATH_MAX]u8 = undefined;
    env[0] = 0;
    if (c.cbm_safe_getenv("XDG_CONFIG_HOME", &env, env.len, null) != null and env[0] != 0) {
        _ = bprint(out, "{s}", .{cut(&env)});
        _ = c.cbm_normalize_path_sep(out.ptr);
        return true;
    }

    const home = homeDir() orelse return false;
    pathJoin(out, home, ".config");
    return out[0] != 0;
}

fn resolveGlobalExcludesPath(out: []u8) bool {
    var config_path: [PATH_MAX]u8 = undefined;

    if (homeDir()) |home| {
        pathJoin(&config_path, home, ".gitconfig");
        if (readCoreExcludesFile(cstr(&config_path), out)) return true;
    }

    var xdg_config: [PATH_MAX]u8 = undefined;
    if (resolveXdgGitConfigDir(&xdg_config)) {
        pathJoin(&config_path, cut(&xdg_config), "git/config");
        if (readCoreExcludesFile(cstr(&config_path), out)) return true;
        pathJoin(out, cut(&xdg_config), "git/ignore");
        return out[0] != 0;
    }

    return false;
}

// ── Public filter functions ────────────────────────────────────────

pub export fn cbm_should_skip_dir(dirname_c: ?Str, mode: Mode) bool {
    const dirname = std.mem.span(dirname_c orelse return false);

    if (strInList(dirname, &ALWAYS_SKIP_DIRS)) return true;

    // Fast discovery applies to both MODERATE and FAST — only FULL keeps everything.
    if (mode != MODE_FULL and strInList(dirname, &FAST_SKIP_DIRS)) return true;

    return false;
}

pub export fn cbm_has_ignored_suffix(filename_c: ?Str, mode: Mode) bool {
    const filename = std.mem.span(filename_c orelse return false);

    for (ALWAYS_IGNORED_SUFFIXES) |suffix| {
        if (std.mem.endsWith(u8, filename, suffix)) return true;
    }

    if (mode != MODE_FULL) {
        for (FAST_IGNORED_SUFFIXES) |suffix| {
            if (std.mem.endsWith(u8, filename, suffix)) return true;
        }
    }

    return false;
}

pub export fn cbm_should_skip_filename(filename_c: ?Str, mode: Mode) bool {
    const filename = std.mem.span(filename_c orelse return false);
    return mode != MODE_FULL and strInList(filename, &FAST_SKIP_FILENAMES);
}

pub export fn cbm_matches_fast_pattern(filename_c: ?Str, mode: Mode) bool {
    const filename_z = filename_c orelse return false;
    if (mode == MODE_FULL) return false;
    const filename = std.mem.span(filename_z);

    for (FAST_PATTERNS) |pattern| {
        if (std.mem.indexOf(u8, filename, pattern) != null) return true;
    }

    return false;
}

// ── Dynamic file list ──────────────────────────────────────────────

/// Paths git tracks under the discovery root, and every directory above one.
/// git applies ignore rules to untracked paths only, so .gitignore,
/// info/exclude and the global excludes file never hide a tracked path. Both
/// tables stay NULL when the root is not in a git repository or git cannot
/// run, and the ignore rules then apply exactly as before.
const Tracked = struct {
    paths: ?[*]u8 = null, // `git ls-files -z` output; holds the keys of `files`
    files: ?*c.CBMHashTable = null, // tracked path
    dirs: ?*c.CBMHashTable = null, // directory above a tracked path (owned key)
};

fn trackedHasFile(tracked: ?*const Tracked, rel_path: Str) bool {
    const t = tracked orelse return false;
    return t.files != null and c.cbm_ht_has(t.files, rel_path);
}

fn trackedHasDir(tracked: ?*const Tracked, rel_path: Str) bool {
    const t = tracked orelse return false;
    return t.dirs != null and c.cbm_ht_has(t.dirs, rel_path);
}

const EXCLUDED_INIT_CAP = 64;
const FILES_INIT_CAP = 256;

const FileList = struct {
    files: ?[*]c.cbm_file_info_t = null,
    count: c_int = 0,
    capacity: c_int = 0,
    /// Directories skipped during the walk (rel paths), so callers can surface
    /// which subtrees were dropped (#411). strdup'd; freed by the caller via
    /// cbm_discover_free_excluded or internally when not requested.
    excluded: ?[*]?[*:0]u8 = null,
    excluded_count: c_int = 0,
    excluded_cap: c_int = 0,
    cache_tree: [PATH_MAX]u8 = @splat(0),
    /// Set when the walk could not fully materialize (e.g. a walk-stack grow
    /// failed on allocation). Callers keep whatever was discovered; the flag
    /// distinguishes a complete walk from a partially-degraded one (upstream
    /// 03dc9c91, #17).
    failed: bool = false,
    tracked: ?*const Tracked = null,
};

/// Test fault injection for the OOM paths below (same convention as
/// CBM_ZOVA_TEST_FAIL_PHASE): when CBM_DISCOVER_TEST_FAIL_PHASE is set, the
/// NTH call to a checkpoint with that phase name returns failure. The hit
/// counter is tracked PER PHASE (reset whenever the active phase name
/// changes), so a fault aimed at one checkpoint cannot be consumed by hits
/// from another.
var test_fail_active_phase: [64]u8 = @splat(0);
var test_fail_hits: c_long = 0;

fn testFailHit(comptime phase: []const u8) bool {
    const fault: Str = c.getenv("CBM_DISCOVER_TEST_FAIL_PHASE") orelse return false;
    if (!std.mem.eql(u8, std.mem.span(fault), phase)) return false;
    if (!std.mem.eql(u8, cut(&test_fail_active_phase), phase)) {
        _ = bprint(&test_fail_active_phase, "{s}", .{phase});
        test_fail_hits = 0;
    }
    const nth_raw: ?Str = c.getenv("CBM_DISCOVER_TEST_FAIL_NTH");
    var nth: c_long = if (nth_raw) |raw| c.strtol(raw, null, 10) else 1;
    if (nth <= 0) nth = 1;
    test_fail_hits += 1;
    return test_fail_hits == nth;
}

fn fileListAddExcluded(fl: *FileList, rel_path: Str) void {
    if (rel_path[0] == 0) return;
    if (fl.excluded_count >= fl.excluded_cap) {
        const new_cap: c_int = if (fl.excluded_cap != 0) fl.excluded_cap * 2 else EXCLUDED_INIT_CAP;
        if (testFailHit("excluded_grow")) {
            fl.failed = true;
            return;
        }
        const grown = c.realloc(@ptrCast(fl.excluded), @as(usize, @intCast(new_cap)) * @sizeOf(?[*:0]u8)) orelse {
            // Dropped skip-point record — the walk is no longer complete (#17).
            fl.failed = true;
            return;
        };
        fl.excluded = @ptrCast(@alignCast(grown));
        fl.excluded_cap = new_cap;
    }
    const copy: ?[*:0]u8 = c.strdup(rel_path);
    if (copy == null or testFailHit("excluded_strdup")) {
        c.free(copy);
        fl.failed = true;
        return;
    }
    fl.excluded.?[@intCast(fl.excluded_count)] = copy;
    fl.excluded_count += 1;
}

fn flAdd(fl: *FileList, abs_path: Str, rel_path: Str, lang: Lang, size: i64) void {
    if (fl.count >= fl.capacity) {
        const new_cap: c_int = if (fl.capacity != 0) fl.capacity * 2 else FILES_INIT_CAP;
        if (testFailHit("files_grow")) {
            fl.failed = true;
            return;
        }
        const grown = c.realloc(@ptrCast(fl.files), @as(usize, @intCast(new_cap)) * @sizeOf(c.cbm_file_info_t)) orelse {
            // Checked bounds growth (#17): on allocation failure the file is
            // dropped and the walk is flagged incomplete instead of silently
            // indexing a partial view as if it were the whole tree.
            fl.failed = true;
            return;
        };
        fl.files = @ptrCast(@alignCast(grown));
        fl.capacity = new_cap;
    }

    // Copy BOTH strings BEFORE the slot becomes visible: on a strdup failure
    // the entry is rolled back, so a partially initialized record with a NULL
    // path can never be passed downstream (#17).
    const path_copy: ?[*:0]u8 = c.strdup(abs_path);
    const rel_copy: ?[*:0]u8 = c.strdup(rel_path);
    if (path_copy == null or rel_copy == null or testFailHit("file_strdup")) {
        c.free(path_copy);
        c.free(rel_copy);
        fl.failed = true;
        return;
    }
    fl.files.?[@intCast(fl.count)] = .{
        .path = path_copy,
        .rel_path = rel_copy,
        .language = lang,
        .size = size,
    };
    fl.count += 1;
}

// ── Recursive walk ─────────────────────────────────────────────────

/// Compute path relative to a nested .gitignore's directory.
/// "webapp/src/foo.js" with prefix "webapp" → "src/foo.js".
fn localRelPath(rel_path: Str, local_prefix: Str) Str {
    const prefix = std.mem.span(local_prefix);
    if (prefix.len == 0) return rel_path;
    const path = std.mem.span(rel_path);
    if (std.mem.startsWith(u8, path, prefix) and path.len > prefix.len and path[prefix.len] == '/') {
        return rel_path + prefix.len + 1;
    }
    return rel_path;
}

/// One ignore file on the path from the repository root down to a walk frame.
/// Links form a root->leaf chain; every frame below the directory that owns a
/// matcher borrows a pointer to the deepest link governing it, so a directory
/// without a .gitignore of its own simply shares its parent's link. Links live
/// on the heap (the frame stack is realloc'd and popped) and are owned through
/// the `owned_next` list.
///
/// Every pattern is anchored to the directory its file came from, so each link
/// carries the offset between that directory and the walk-relative rel_path it
/// is asked about. The two offsets are mirror images and at most one is ever
/// non-empty:
///   `prefix` — the matcher's directory is at or BELOW the walk root
///              ("" for the root itself): strip it from rel_path.
///   `base`   — the matcher's directory is ABOVE the walk root (an enclosing
///              repository's ignore files, when a git-less subfolder is
///              indexed): prepend the walk root's path relative to it.
/// Both strings live in the same allocation, right after the struct, `base`
/// after `prefix`. (Upstream d3eb7e98 and 92910147.)
const Link = struct {
    gi: *const Gitignore,
    parent: ?*const Link,
    owned_next: ?*Link,
    prefix: Str,
    base: Str,
};

fn linkNew(gi: *const Gitignore, prefix: []const u8, base: []const u8, parent: ?*const Link, owned_links: *?*Link) ?*Link {
    const mem: [*]u8 = @ptrCast(c.malloc(@sizeOf(Link) + prefix.len + base.len + 2) orelse return null);
    const prefix_copy = mem + @sizeOf(Link);
    @memcpy(prefix_copy[0..prefix.len], prefix);
    prefix_copy[prefix.len] = 0;
    const base_copy = prefix_copy + prefix.len + 1;
    @memcpy(base_copy[0..base.len], base);
    base_copy[base.len] = 0;
    const link: *Link = @ptrCast(@alignCast(mem));
    link.* = .{
        .gi = gi,
        .parent = parent,
        .owned_next = owned_links.*,
        .prefix = @ptrCast(prefix_copy),
        .base = @ptrCast(base_copy),
    };
    owned_links.* = link;
    return link;
}

fn linksFree(first: ?*Link) void {
    var link = first;
    while (link) |l| {
        link = l.owned_next;
        c.free(l);
    }
}

/// Verdict of every ignore file between the repository root and the directory
/// being walked, with git's precedence: the deepest file that has an opinion
/// wins (a lower-level file takes precedence over every higher-level one), and
/// within one file the last matching pattern wins. Returns >0 ignored, <0
/// re-included by a negation, 0 when no file mentions the path. Cost is one
/// match per ignore file on the path.
fn gitignoreChainResult(first: ?*const Link, rel_path: Str, is_dir: bool) c_int {
    var based: [PATH_MAX]u8 = undefined;
    var link = first;
    while (link) |l| : (link = l.parent) {
        var probe = localRelPath(rel_path, l.prefix);
        if (l.base[0] != 0) {
            // Matcher from a directory above the walk root: ask it about the
            // path it would see, the one its own rooted patterns are anchored
            // against.
            const written = bprint(&based, "{s}/{s}", .{ std.mem.span(l.base), std.mem.span(probe) });
            if (written >= based.len) {
                continue; // longer than this matcher can address: no opinion
            }
            probe = cstr(&based);
        }
        const verdict = gitignore.matchResult(l.gi, probe, is_dir);
        if (verdict != 0) return verdict;
    }
    return 0;
}

/// Non-negatable safety core: built-in skip dirs that a .cbmignore negation
/// can NEVER un-skip. A repo-committed .cbmignore must not be able to defeat
/// OOM/safety skips: .git holds VCS internals (and the info/exclude sources,
/// #489), node_modules explodes discovery, and the worktree-internal dirs
/// (.worktrees / .claude-worktrees, the worktree entries in ALWAYS_SKIP_DIRS)
/// contain parallel checkouts of the same repo whose indexing would duplicate
/// the whole codebase (#802).
const SAFETY_CORE_DIRS = [_][]const u8{ ".git", "node_modules", ".worktrees", ".claude-worktrees" };

/// The shared cache directory holds every indexed workspace's database and the
/// CBM internal files. A custom CBM_CACHE_DIR may legitimately sit inside a
/// repository — tests do it routinely, and nothing forbids it in production —
/// and walking into it would pull other workspaces' databases and cache state
/// into this project's file list. "cache" is deliberately not on the built-in
/// skip list (it is a common source directory name), so the walk prunes the
/// configured cache tree by absolute path instead.
///
/// This is the narrow remedy for the boundary concern in #17 ("never traverse
/// the shared CBM cache/database as repository input"): refusing any root that
/// contains the cache was considered and rejected as too blunt — not walking
/// the cache is what the boundary actually asks for. (Upstream 7814dacf.)
fn dirIsCacheTree(abs_path: []const u8, cache: []const u8) bool {
    if (cache.len == 0 or abs_path.len == 0) return false;
    var n = cache.len;
    while (n > 1 and (cache[n - 1] == '/' or cache[n - 1] == '\\')) n -= 1;
    if (!std.mem.startsWith(u8, abs_path, cache[0..n])) return false;
    // Boundary-aware so "<cache>x" is not treated as living under "<cache>".
    return abs_path.len == n or abs_path[n] == '/' or abs_path[n] == '\\';
}

/// What the ignore rules say about a directory. .git_ignored comes from git's
/// own rules (.gitignore files, info/exclude, the global excludes file), which
/// yield to tracked content; built-in skips and .cbmignore never do.
const DirVerdict = enum { keep, skip, git_ignored };

fn optsMode(opts: ?*const c.cbm_discover_opts_t) Mode {
    return if (opts) |o| o.mode else MODE_FULL;
}

fn directoryVerdict(
    entry_name: Str,
    rel_path: Str,
    opts: ?*const c.cbm_discover_opts_t,
    ignore_chain: ?*const Link,
    global_gi: ?*const Gitignore,
    cbmignore: ?*const Gitignore,
) DirVerdict {
    if (cbm_should_skip_dir(entry_name, optsMode(opts))) {
        // #500: a .cbmignore negation (e.g. "!obj/") whose rule is the last
        // match for this dir un-skips a built-in skip-list dir — except the
        // non-negatable safety core. Fall through so .gitignore/global/local
        // rules still apply to the un-skipped dir.
        const unskipped = cbmignore != null and
            !strInList(std.mem.span(entry_name), &SAFETY_CORE_DIRS) and
            gitignore.matchResult(cbmignore, rel_path, true) < 0;
        if (!unskipped) return .skip;
    }
    const git_ignored = gitignoreChainResult(ignore_chain, rel_path, true) > 0;
    var global_ignored = global_gi != null and gitignore.cbm_gitignore_matches(global_gi, rel_path, true);
    if (cbmignore != null) {
        const cbm_result = gitignore.matchResult(cbmignore, rel_path, true);
        if (cbm_result > 0) return .skip;
        if (cbm_result < 0) {
            global_ignored = false; // a .cbmignore negation un-ignores a global match
        }
    }
    return if (git_ignored or global_ignored) .git_ignored else .keep;
}

/// Check if a regular file should be skipped (filters + gitignore + size).
/// git's own rules never hide a tracked file; inside a git-ignored directory
/// (entered only for its tracked content) every untracked file is ignored, as
/// in git.
fn shouldSkipFile(
    entry_name: Str,
    rel_path: Str,
    opts: ?*const c.cbm_discover_opts_t,
    ignore_chain: ?*const Link,
    global_gi: ?*const Gitignore,
    cbmignore: ?*const Gitignore,
    file_size: i64,
    tracked: bool,
    in_ignored_dir: bool,
) bool {
    const mode = optsMode(opts);
    if (cbm_has_ignored_suffix(entry_name, mode)) return true;
    if (cbm_should_skip_filename(entry_name, mode)) return true;
    if (cbm_matches_fast_pattern(entry_name, mode)) return true;
    if (!tracked and (in_ignored_dir or gitignoreChainResult(ignore_chain, rel_path, false) > 0)) {
        return true;
    }
    var global_ignored = !tracked and global_gi != null and
        gitignore.cbm_gitignore_matches(global_gi, rel_path, false);
    if (cbmignore != null) {
        const cbm_result = gitignore.matchResult(cbmignore, rel_path, false);
        if (cbm_result > 0) return true;
        if (cbm_result < 0 and global_ignored) global_ignored = false;
    }
    if (opts) |o| {
        if (o.max_file_size > 0 and file_size > o.max_file_size) return true;
    }
    return global_ignored;
}

/// Detect language for a file, handling .m/.cls/.frm disambiguation and JSON filtering.
fn detectFileLanguage(entry_name: Str, abs_path: Str) Lang {
    var lang = language.cbm_language_for_filename(entry_name);
    if (lang == LANG_UNKNOWN) {
        // Filename/extension detection failed: fall back to a conservative
        // shebang probe so extensionless scripts get indexed (#1199). Filename
        // detection stays authoritative — this runs only when it returns
        // unknown.
        return language.cbm_language_from_shebang(abs_path);
    }
    const name = std.mem.span(entry_name);
    if (std.mem.lastIndexOfScalar(u8, name, '.')) |dot| {
        const ext = name[dot..];
        // Special: .m files need content-based disambiguation
        if (std.mem.eql(u8, ext, ".m")) lang = language.cbm_disambiguate_m(abs_path);
        // Special: .cls / .frm are shared with VB6 class modules and forms
        if (std.mem.eql(u8, ext, ".cls")) lang = language.cbm_disambiguate_cls(abs_path);
        if (std.mem.eql(u8, ext, ".frm")) lang = language.cbm_disambiguate_frm(abs_path);
        if (std.mem.eql(u8, ext, ".res")) lang = language.cbm_disambiguate_res(abs_path);
    }
    // Check ignored JSON files
    if (lang == LANG_JSON and strInList(name, &IGNORED_JSON_FILES)) return LANG_UNKNOWN;
    return lang;
}

/// Process a single regular file entry during directory walk.
fn walkDirProcessFile(
    abs_path: Str,
    rel_path: Str,
    name: Str,
    opts: ?*const c.cbm_discover_opts_t,
    ignore_chain: ?*const Link,
    global_gi: ?*const Gitignore,
    cbmignore: ?*const Gitignore,
    in_ignored_dir: bool,
    size: i64,
    out: *FileList,
) void {
    if (shouldSkipFile(name, rel_path, opts, ignore_chain, global_gi, cbmignore, size, trackedHasFile(out.tracked, rel_path), in_ignored_dir)) {
        return;
    }
    const lang = detectFileLanguage(name, abs_path);
    if (lang == LANG_UNKNOWN) return;
    flAdd(out, abs_path, rel_path, lang, size);
}

const Frame = struct {
    dir: [PATH_MAX]u8,
    prefix: [PATH_MAX]u8,
    ignore_chain: ?*const Link, // deepest .gitignore governing this dir
    git_ignored: bool, // ignored by git's rules, entered only for its tracked content
};

/// Initial capacity only — the stack grows on demand. A single directory can
/// hold more pending sibling frames than any fixed cap (dotnet/runtime has 855
/// subdirs in one JIT regression dir), so a hard cap here means whole-repo
/// discovery failure, not a depth guard (upstream 03dc9c91, #17).
const WALK_STACK_CAP = 512;

const WalkStack = struct {
    frames: [*]Frame,
    top: usize,
    cap: usize,
};

/// Try to load a nested .gitignore from this directory. Returns owned pointer or
/// NULL. Every directory below the root is probed, whether or not an ancestor
/// already contributed a matcher: git stacks ALL .gitignore files on a path.
/// The root's own .gitignore is loaded by cbm_discover_ex (merged with
/// info/exclude).
fn tryLoadNestedGitignore(frame: *const Frame) ?*Gitignore {
    if (frame.prefix[0] == 0) return null;
    var gi_path: [PATH_MAX]u8 = undefined;
    _ = bprint(&gi_path, "{s}/.gitignore", .{cut(&frame.dir)});
    var gi_st: c.struct_stat = undefined;
    if (c.cbm_stat_utf8(&gi_path, &gi_st) == 0 and isReg(&gi_st)) {
        return gitignore.cbm_gitignore_load(cstr(&gi_path));
    }
    return null;
}

/// Push a subdirectory onto the walk stack, inheriting local gitignore context.
/// Grows the stack geometrically; the caller's `parent` must not point into the
/// stack array (walkDir pops into a local copy). On growth failure the subtree
/// is dropped and the walk is flagged incomplete (#17, upstream 03dc9c91).
fn walkPushSubdir(ws: *WalkStack, abs_path: []const u8, rel_path: []const u8, parent: *const Frame, git_ignored: bool, out: *FileList) void {
    if (ws.top >= ws.cap) {
        const new_cap = ws.cap * 2;
        if (testFailHit("stack_grow")) {
            out.failed = true;
            return;
        }
        const grown = c.realloc(@ptrCast(ws.frames), new_cap * @sizeOf(Frame)) orelse {
            out.failed = true;
            return;
        };
        ws.frames = @ptrCast(@alignCast(grown));
        ws.cap = new_cap;
    }
    const slot = &ws.frames[ws.top];
    const directory_length = bprint(&slot.dir, "{s}", .{abs_path});
    const prefix_length = bprint(&slot.prefix, "{s}", .{rel_path});
    if (directory_length == 0 or directory_length >= PATH_MAX or prefix_length >= PATH_MAX) {
        out.failed = true;
        return;
    }
    slot.ignore_chain = parent.ignore_chain;
    slot.git_ignored = git_ignored;
    ws.top += 1;
}

/// Build abs/rel paths and process one directory entry.
fn walkDirProcessEntry(
    entry: *const c.cbm_dirent_t,
    frame: *const Frame,
    opts: ?*const c.cbm_discover_opts_t,
    global_gi: ?*const Gitignore,
    cbmignore: ?*const Gitignore,
    ws: *WalkStack,
    out: *FileList,
) void {
    const name = cut(&entry.name);
    var abs_path: [PATH_MAX]u8 = undefined;
    var rel_path: [PATH_MAX]u8 = undefined;
    _ = bprint(&abs_path, "{s}/{s}", .{ cut(&frame.dir), name });
    if (frame.prefix[0] != 0) {
        _ = bprint(&rel_path, "{s}/{s}", .{ cut(&frame.prefix), name });
    } else {
        _ = bprint(&rel_path, "{s}", .{name});
    }

    var st: c.struct_stat = undefined;
    if (c.cbm_stat_no_follow(&abs_path, &st) != 0) return;

    if (isDir(&st)) {
        var verdict: DirVerdict = if (dirIsCacheTree(cut(&abs_path), cut(&out.cache_tree)))
            .skip
        else
            directoryVerdict(cstr(&entry.name), cstr(&rel_path), opts, frame.ignore_chain, global_gi, cbmignore);
        if (verdict == .keep and frame.git_ignored) {
            verdict = .git_ignored; // below a git-ignored directory, as in git
        }
        if (verdict == .keep or
            (verdict == .git_ignored and trackedHasDir(out.tracked, cstr(&rel_path))))
        {
            walkPushSubdir(ws, cut(&abs_path), cut(&rel_path), frame, verdict == .git_ignored, out);
        } else {
            // Record the excluded subtree root so callers can report it (#411).
            fileListAddExcluded(out, cstr(&rel_path));
        }
    } else if (isReg(&st)) {
        walkDirProcessFile(cstr(&abs_path), cstr(&rel_path), cstr(&entry.name), opts, frame.ignore_chain, global_gi, cbmignore, frame.git_ignored, @intCast(st.st_size), out);
    }
}

const GI_OWNED_INIT_CAP = 16;

/// Matchers owned by the walk or by the enclosing-repository chain.
const OwnedGitignores = struct {
    items: ?[*]*Gitignore = null,
    count: usize = 0,
    cap: usize = 0,

    /// Append a matcher; false (array unchanged) when it cannot grow.
    fn append(self: *OwnedGitignores, gi: *Gitignore) bool {
        if (self.count == self.cap) {
            const new_cap = if (self.cap != 0) self.cap * 2 else GI_OWNED_INIT_CAP;
            const grown = if (testFailHit("gi_owned_grow"))
                null
            else
                c.realloc(@ptrCast(self.items), new_cap * @sizeOf(*Gitignore));
            self.items = @ptrCast(@alignCast(grown orelse return false));
            self.cap = new_cap;
        }
        self.items.?[self.count] = gi;
        self.count += 1;
        return true;
    }

    fn free(self: *OwnedGitignores) void {
        if (self.items) |items| {
            for (items[0..self.count]) |gi| gitignore.cbm_gitignore_free(gi);
        }
        c.free(@ptrCast(self.items));
    }
};

fn walkDir(
    dir_path: Str,
    rel_prefix: Str,
    opts: ?*const c.cbm_discover_opts_t,
    root_gi: ?*const Gitignore,
    ancestors: ?*const Link,
    global_gi: ?*const Gitignore,
    cbmignore: ?*const Gitignore,
    out: *FileList,
) void {
    const frames = if (testFailHit("stack_calloc")) null else c.calloc(WALK_STACK_CAP, @sizeOf(Frame));
    var ws: WalkStack = .{
        .frames = @ptrCast(@alignCast(frames orelse {
            // Nothing was walked at all — flag so the caller does not mistake an
            // empty result for an empty tree (#17).
            out.failed = true;
            return;
        })),
        .top = 0,
        .cap = WALK_STACK_CAP,
    };
    // Collect all owned gitignores and chain links — freed at the end because
    // child frames on the stack hold borrowed pointers to them.
    var owned_gis: OwnedGitignores = .{};
    var owned_links: ?*Link = null;

    const initial_directory_length = bprint(&ws.frames[0].dir, "{s}", .{std.mem.span(dir_path)});
    const initial_prefix_length = bprint(&ws.frames[0].prefix, "{s}", .{std.mem.span(rel_prefix)});
    if (initial_directory_length == 0 or initial_directory_length >= PATH_MAX or
        initial_prefix_length >= PATH_MAX)
    {
        out.failed = true;
        c.free(@ptrCast(ws.frames));
        return;
    }
    // `ancestors` is the deepest link of the enclosing repository's chain (or
    // NULL); the walk root's own matcher sits below it so it still wins.
    ws.frames[0].ignore_chain = ancestors;
    if (root_gi) |gi| {
        ws.frames[0].ignore_chain = linkNew(gi, std.mem.span(rel_prefix), "", ancestors, &owned_links) orelse {
            out.failed = true;
            c.free(@ptrCast(ws.frames));
            return;
        };
    }
    ws.top += 1;

    while (ws.top > 0) {
        ws.top -= 1;
        var frame = ws.frames[ws.top];

        if (tryLoadNestedGitignore(&frame)) |loaded| {
            if (!owned_gis.append(loaded)) {
                // Walking on without these rules would index files they
                // exclude, so drop the subtree and flag the walk (#17).
                gitignore.cbm_gitignore_free(loaded);
                out.failed = true;
                continue;
            }
            // owned_gis owns `loaded` from here on, even if the link fails.
            frame.ignore_chain = linkNew(loaded, cut(&frame.prefix), "", frame.ignore_chain, &owned_links) orelse {
                out.failed = true;
                continue;
            };
        }

        const d = c.cbm_opendir(&frame.dir) orelse continue;
        while (true) {
            const entry: *const c.cbm_dirent_t = c.cbm_readdir(d) orelse break;
            walkDirProcessEntry(entry, &frame, opts, global_gi, cbmignore, &ws, out);
        }
        c.cbm_closedir(d);
    }
    owned_gis.free();
    linksFree(owned_links);
    c.free(@ptrCast(ws.frames));
}

// ── Public API ─────────────────────────────────────────────────────

fn discoverPathIsAbsolute(path: []const u8) bool {
    if (path.len == 0) return false;
    if (path[0] == '/' or path[0] == '\\') return true;
    return is_windows and std.ascii.isAlphabetic(path[0]) and path.len > 1 and path[1] == ':';
}

/// Resolve the shared "common" git directory for repo_path.
/// Handles three layouts:
///   1. <repo>/.git is a directory    - ordinary repo; common_dir == <repo>/.git
///   2. <repo>/.git is a regular file - linked worktree gitlink "gitdir: <path>";
///      the common dir is read from <git_dir>/commondir (git stores info/exclude +
///      config there, shared across worktrees). Falls back to git_dir when no
///      commondir file exists.
///   3. neither - not a git repo.
/// Returns true when a git dir was resolved. Fixes the worktree case where
/// .git/info/exclude and core.excludesfile were silently dropped because the old
/// check required .git to be a directory (issue #489 only covered ordinary repos).
fn resolveGitCommonDir(repo_path: []const u8, common_dir: []u8) bool {
    var dot_git: [PATH_MAX]u8 = undefined;
    _ = bprint(&dot_git, "{s}/.git", .{repo_path});
    var st: c.struct_stat = undefined;
    if (c.cbm_stat_utf8(&dot_git, &st) != 0) return false;
    if (isDir(&st)) {
        _ = bprint(common_dir, "{s}", .{cut(&dot_git)});
        _ = c.cbm_normalize_path_sep(common_dir.ptr);
        return true;
    }
    if (!isReg(&st)) return false;

    // Linked worktree: parse "gitdir: <path>" from the gitlink file.
    // cbm_fopen (not raw fopen) so non-ASCII repo paths open on Windows.
    const f = c.cbm_fopen(&dot_git, "r") orelse return false;
    var git_dir: [PATH_MAX]u8 = undefined;
    var got_git_dir = false;
    var line: [PATH_MAX]u8 = undefined;
    while (c.fgets(&line, line.len, f) != null) {
        const gs = trimWs(cut(&line));
        if (std.mem.startsWith(u8, gs, "gitdir:")) {
            const val = trimWs(gs["gitdir:".len..]);
            if (val.len > 0) {
                if (discoverPathIsAbsolute(val)) {
                    _ = bprint(&git_dir, "{s}", .{val});
                    _ = c.cbm_normalize_path_sep(&git_dir);
                } else {
                    pathJoin(&git_dir, repo_path, val);
                }
                got_git_dir = true;
            }
            break;
        }
    }
    _ = c.fclose(f);
    if (!got_git_dir) return false;

    // The shared dir holding info/exclude + config is named in <git_dir>/commondir
    // (typically a relative path like "../.."). Absent in single-worktree gitdirs.
    var commondir_path: [PATH_MAX]u8 = undefined;
    pathJoin(&commondir_path, cut(&git_dir), "commondir");
    if (c.cbm_fopen(&commondir_path, "r")) |cf| {
        var cbuf: [PATH_MAX]u8 = undefined;
        var resolved = false;
        if (c.fgets(&cbuf, cbuf.len, cf) != null) {
            const cs = trimWs(cut(&cbuf));
            if (cs.len > 0) {
                if (discoverPathIsAbsolute(cs)) {
                    _ = bprint(common_dir, "{s}", .{cs});
                    _ = c.cbm_normalize_path_sep(common_dir.ptr);
                } else {
                    pathJoin(common_dir, cut(&git_dir), cs);
                }
                resolved = true;
            }
        }
        _ = c.fclose(cf);
        if (resolved) return true;
    }

    _ = bprint(common_dir, "{s}", .{cut(&git_dir)});
    _ = c.cbm_normalize_path_sep(common_dir.ptr);
    return true;
}

/// When repo_path itself carries no .git (resolveGitCommonDir already
/// returned false for it), walk upward looking for an enclosing repository, as
/// `git` itself does when run from a subfolder. Bounded by the filesystem or
/// drive root: dir is truncated at each iteration, so the loop cannot run more
/// times than repo_path is characters long. Returns true and fills
/// ancestor_root (for the enclosing repository's own .gitignore files) plus
/// common_dir (for its info/exclude and config). (Upstream 92910147.)
fn resolveEnclosingGitRoot(repo_path: []const u8, ancestor_root: []u8, common_dir: []u8) bool {
    var dir: [PATH_MAX]u8 = undefined;
    _ = bprint(&dir, "{s}", .{repo_path});
    _ = c.cbm_normalize_path_sep(&dir);

    while (true) {
        const current = cut(&dir);
        const slash = std.mem.lastIndexOfScalar(u8, current, '/') orelse return false;
        // Only the root separator (POSIX "/") or a bare drive prefix
        // (Windows "C:/") left: nothing above this to check.
        if (slash == 0 or current[slash - 1] == ':') return false;
        dir[slash] = 0;
        if (resolveGitCommonDir(dir[0..slash], common_dir)) {
            _ = bprint(ancestor_root, "{s}", .{dir[0..slash]});
            return true;
        }
    }
}

/// The enclosing repository's ignore files, kept OUT of the indexed directory's
/// own matcher. Merging them in would re-anchor every rooted pattern onto the
/// indexed subfolder: "/secret.py" at the enclosing root would start hiding
/// <subfolder>/secret.py, and "pkg/scratch/" would stop hiding pkg/scratch.
/// Each matcher instead keeps its own directory as its anchor, via the link's
/// `base`.
const AncestorIgnores = struct {
    owned: OwnedGitignores = .{},
    chain: ?*const Link = null, // deepest ancestor link, or NULL
    links: ?*Link = null, // owner list backing `chain`

    fn free(self: *AncestorIgnores) void {
        self.owned.free();
        linksFree(self.links);
    }

    /// Add one ancestor directory's matcher to the chain, deepest last so that
    /// the deepest file with an opinion wins (git's shallow-to-deep rule,
    /// negations included). Takes ownership of `gi`.
    fn push(self: *AncestorIgnores, gi: *Gitignore, base: []const u8) bool {
        if (!self.owned.append(gi)) {
            gitignore.cbm_gitignore_free(gi);
            return false;
        }
        // owned owns `gi` from here on, even if the link fails.
        self.chain = linkNew(gi, "", base, self.chain, &self.links) orelse return false;
        return true;
    }

    /// Load the .gitignore of EVERY directory from the enclosing repository root
    /// (inclusive) down to the indexed directory (exclusive): git consults all
    /// of them, not just the root's. `root_extra` is that repository's
    /// <common>/info/exclude, which git anchors at its root like the root
    /// .gitignore; ownership is taken. `leaf` must be a canonical path under
    /// `root`, both with '/' separators. Returns false only on allocation
    /// failure.
    fn build(self: *AncestorIgnores, root: []const u8, leaf: []const u8, root_extra_in: ?*Gitignore) bool {
        var root_extra = root_extra_in;
        if (!std.mem.startsWith(u8, leaf, root)) {
            gitignore.cbm_gitignore_free(root_extra);
            return true;
        }
        var base = leaf[root.len..];
        while (base.len > 0 and base[0] == '/') base = base[1..];

        var dir: [PATH_MAX]u8 = undefined;
        var dir_len = bprint(&dir, "{s}", .{root});
        if (dir_len >= dir.len or base.len == 0) {
            gitignore.cbm_gitignore_free(root_extra);
            return true;
        }

        while (true) {
            var gi_path: [PATH_MAX]u8 = undefined;
            pathJoin(&gi_path, dir[0..dir_len], ".gitignore");
            var gi = gitignore.cbm_gitignore_load(cstr(&gi_path));
            if (root_extra) |extra| {
                // First iteration: `dir` is the enclosing repository root, the
                // one directory info/exclude shares an anchor with. Merged
                // after the .gitignore patterns so it overrides them, the same
                // precedence the two already have for an ordinary repository
                // root.
                if (gi) |g| {
                    _ = gitignore.cbm_gitignore_merge(g, extra);
                    gitignore.cbm_gitignore_free(extra);
                } else {
                    gi = extra;
                }
                root_extra = null;
            }
            if (gi) |g| {
                if (!self.push(g, base)) return false;
            }
            const slash = std.mem.indexOfScalar(u8, base, '/') orelse {
                return true; // the next directory down IS the indexed one
            };
            const written = bprint(dir[dir_len..], "/{s}", .{base[0..slash]});
            if (written >= dir.len - dir_len) {
                return true; // deeper directories are unaddressable: stop here
            }
            dir_len += written;
            base = base[slash + 1 ..];
        }
    }
};

fn trackedFreeKey(key: [*c]const u8, value: ?*anyopaque, userdata: ?*anyopaque) callconv(.c) void {
    _ = value;
    _ = userdata;
    c.free(@ptrCast(@constCast(key)));
}

fn trackedFree(tracked: *Tracked) void {
    if (tracked.dirs) |dirs| c.cbm_ht_foreach(dirs, trackedFreeKey, null);
    c.cbm_ht_free(tracked.dirs);
    c.cbm_ht_free(tracked.files);
    c.free(tracked.paths);
    tracked.* = .{};
}

/// Record every directory above `path` (its '/'-separated prefixes).
fn trackedAddDirs(dirs: *c.CBMHashTable, path: []const u8) bool {
    var prefix: [PATH_MAX]u8 = undefined;
    var from: usize = 0;
    while (std.mem.indexOfScalarPos(u8, path, from, '/')) |slash| {
        from = slash + 1;
        if (slash == 0 or slash >= prefix.len) continue;
        @memcpy(prefix[0..slash], path[0..slash]);
        prefix[slash] = 0;
        if (c.cbm_ht_has(dirs, &prefix)) continue;
        const key = c.strdup(&prefix) orelse return false;
        _ = c.cbm_ht_set(dirs, key, key);
    }
    return true;
}

/// Load what git tracks under repo_path (`git ls-files -z`, paths relative to
/// it); leaves `tracked` empty on any failure, so the ignore rules then apply
/// exactly as they did without it. (Upstream a90076a3.)
fn trackedLoad(tracked: *Tracked, repo_path: Str) void {
    tracked.* = .{};
    var length: usize = 0;
    tracked.paths = c.cbm_git_ls_files(repo_path, &length);
    const paths = tracked.paths orelse return;
    if (length == 0) {
        trackedFree(tracked);
        return;
    }
    tracked.files = c.cbm_ht_create(1024);
    tracked.dirs = c.cbm_ht_create(256);
    const files = tracked.files orelse return trackedFree(tracked);
    const dirs = tracked.dirs orelse return trackedFree(tracked);
    var at: usize = 0;
    while (at < length) {
        const path: [*:0]u8 = @ptrCast(paths + at);
        const n = std.mem.len(path);
        if (n > 0) {
            _ = c.cbm_ht_set(files, path, path);
            if (!trackedAddDirs(dirs, path[0..n])) {
                trackedFree(tracked);
                return;
            }
        }
        at += n + 1;
    }
}

pub export fn cbm_discover(repo_path: ?Str, opts: ?*const c.cbm_discover_opts_t, out: ?*?[*]c.cbm_file_info_t, count: ?*c_int) c_int {
    return cbm_discover_ex(repo_path, opts, out, count, null, null, null);
}

pub export fn cbm_discover_ex(
    repo_path_c: ?Str,
    opts: ?*const c.cbm_discover_opts_t,
    out_c: ?*?[*]c.cbm_file_info_t,
    count_c: ?*c_int,
    excluded_out: ?*?[*]?[*:0]u8,
    excluded_count_out: ?*c_int,
    degraded_out: ?*bool,
) c_int {
    if (excluded_out) |p| p.* = null;
    if (excluded_count_out) |p| p.* = 0;
    if (degraded_out) |p| p.* = false;
    const repo_path_z = repo_path_c orelse return c.CBM_NOT_FOUND;
    const out = out_c orelse return c.CBM_NOT_FOUND;
    const count = count_c orelse return c.CBM_NOT_FOUND;
    const repo_path = std.mem.span(repo_path_z);

    out.* = null;
    count.* = 0;

    // Verify directory exists
    var st: c.struct_stat = undefined;
    if (c.cbm_stat_utf8(repo_path_z, &st) != 0 or !isDir(&st)) return c.CBM_NOT_FOUND;

    // Load gitignore sources for ordinary repos AND linked worktrees.
    // Sources merged in order (later patterns win on conflict):
    //   1. <repo>/.gitignore     — committed exclusions
    //   2. <common>/info/exclude — per-clone exclusions, not committed
    // <common> is the git common dir, resolved via resolveGitCommonDir() so a
    // worktree (where .git is a gitlink file) reads the shared info/exclude/config
    // just like a normal checkout. Both are folded into a single matcher so all
    // downstream call paths remain unchanged. Fixes issue #489: OOM on repos whose
    // worktrees are excluded only via .git/info/exclude (e.g. Sandcastle).
    //
    // An ENCLOSING repository's ignore files (indexing a git-less subfolder)
    // are deliberately NOT folded in here: their patterns are anchored to
    // their own directories, so they stay separate matchers on the chain with
    // a `base` offset. See AncestorIgnores.build().
    var gi_path: [PATH_MAX]u8 = undefined;
    var gi_stat: c.struct_stat = undefined;
    // Resolve the git common dir, transparently following a worktree gitlink so the
    // .git/info/exclude and core.excludesfile sources are honoured inside linked
    // worktrees too (where .git is a file pointing at the shared dir, not a directory).
    var git_common_dir: [PATH_MAX]u8 = undefined;
    var is_git_repo = resolveGitCommonDir(repo_path, &git_common_dir);
    var has_git_config = false;
    // repo_path itself is not a git repo root: walk upward for an enclosing
    // one, as `git` does when run from a subfolder (upstream 92910147). The
    // walk is lexical, so repo_path is canonicalized first; otherwise a
    // relative and an absolute spelling of one directory would discover
    // different files.
    var ancestors: AncestorIgnores = .{};
    var enclosing_root: [PATH_MAX]u8 = undefined;
    var canonical_repo: [PATH_MAX]u8 = undefined;
    var has_enclosing_repo = false;
    if (!is_git_repo) {
        if (!c.cbm_canonical_path(repo_path_z, &canonical_repo, canonical_repo.len)) {
            _ = bprint(&canonical_repo, "{s}", .{repo_path});
        }
        _ = c.cbm_normalize_path_sep(&canonical_repo);
        has_enclosing_repo = resolveEnclosingGitRoot(cut(&canonical_repo), &enclosing_root, &git_common_dir);
        is_git_repo = has_enclosing_repo;
    }
    // Always honour the .gitignore at the indexed-directory root, even when the
    // directory is not a git repo root (e.g. indexing a sub-package directly).
    // Fixes issue #510: a root .gitignore was silently ignored without .git/.
    _ = bprint(&gi_path, "{s}/.gitignore", .{repo_path});
    var root_gi = gitignore.cbm_gitignore_load(cstr(&gi_path));
    if (is_git_repo) {
        pathJoin(&gi_path, cut(&git_common_dir), "config");
        has_git_config = c.cbm_stat_utf8(&gi_path, &gi_stat) == 0 and isReg(&gi_stat);

        var exc_path: [PATH_MAX]u8 = undefined;
        pathJoin(&exc_path, cut(&git_common_dir), "info/exclude");
        const git_exclude = gitignore.cbm_gitignore_load(cstr(&exc_path));
        if (has_enclosing_repo) {
            // git_common_dir belongs to the ENCLOSING repository, so its
            // info/exclude is anchored at that repository's root, not at the
            // indexed subfolder. Ownership passes to the ancestor chain.
            if (!ancestors.build(cut(&enclosing_root), cut(&canonical_repo), git_exclude)) {
                ancestors.free();
                gitignore.cbm_gitignore_free(root_gi);
                return c.CBM_NOT_FOUND;
            }
        } else if (git_exclude) |exclude| {
            if (root_gi) |gi| {
                // On allocation failure the merge is atomic (dst unchanged), so
                // the .gitignore patterns still apply; the exclude patterns are
                // simply skipped — same as if .git/info/exclude were absent.
                _ = gitignore.cbm_gitignore_merge(gi, exclude);
                gitignore.cbm_gitignore_free(exclude);
            } else {
                root_gi = exclude;
            }
        }
    }

    var global_gi: ?*Gitignore = null;
    if (has_git_config and resolveGlobalExcludesPath(&gi_path)) {
        global_gi = gitignore.cbm_gitignore_load(cstr(&gi_path));
    }

    // Load cbmignore if specified or exists at repo root
    var cbmignore: ?*Gitignore = null;
    if (opts != null and opts.?.ignore_file != null) {
        cbmignore = gitignore.cbm_gitignore_load(opts.?.ignore_file);
    } else {
        _ = bprint(&gi_path, "{s}/.cbmignore", .{repo_path});
        cbmignore = gitignore.cbm_gitignore_load(cstr(&gi_path));
    }

    // Walk
    var fl: FileList = .{};
    const cache: ?Str = c.cbm_resolve_cache_dir();
    if (cache != null and cache.?[0] != 0) {
        if (!c.cbm_canonical_path(cache, &fl.cache_tree, fl.cache_tree.len)) {
            _ = bprint(&fl.cache_tree, "{s}", .{std.mem.span(cache.?)});
        }
        if (!is_windows) {
            // The walk builds paths from repo_path as given. When that path goes
            // through a symlink (macOS /tmp -> /private/tmp), re-express a cache
            // that lives inside the repository in the walk's own spelling.
            var root_real: [PATH_MAX]u8 = undefined;
            if (c.cbm_canonical_path(repo_path_z, &root_real, root_real.len) and
                dirIsCacheTree(cut(&fl.cache_tree), cut(&root_real)))
            {
                var inside: [PATH_MAX]u8 = undefined;
                _ = bprint(&inside, "{s}", .{cut(fl.cache_tree[cut(&root_real).len..])});
                _ = bprint(&fl.cache_tree, "{s}{s}", .{ repo_path, cut(&inside) });
            }
        }
    }
    var tracked: Tracked = .{};
    if (is_git_repo) {
        trackedLoad(&tracked, repo_path_z);
        fl.tracked = &tracked;
    }
    walkDir(repo_path_z, "", opts, root_gi, ancestors.chain, global_gi, cbmignore, &fl);

    // Cleanup
    trackedFree(&tracked);
    ancestors.free();
    gitignore.cbm_gitignore_free(root_gi);
    gitignore.cbm_gitignore_free(global_gi);
    gitignore.cbm_gitignore_free(cbmignore);

    out.* = fl.files;
    count.* = fl.count;
    if (degraded_out) |p| p.* = fl.failed;

    // Hand the excluded-dir list to the caller, or free it if not requested.
    if (excluded_out) |p| {
        p.* = fl.excluded;
        if (excluded_count_out) |n| n.* = fl.excluded_count;
    } else {
        cbm_discover_free_excluded(fl.excluded, fl.excluded_count);
    }
    return 0;
}

pub export fn cbm_discover_free(files_c: ?[*]c.cbm_file_info_t, count: c_int) void {
    const files = files_c orelse return;
    for (files[0..@intCast(@max(count, 0))]) |fi| {
        c.free(fi.path);
        c.free(fi.rel_path);
    }
    c.free(@ptrCast(files));
}

pub export fn cbm_discover_free_excluded(excluded_c: ?[*]?[*:0]u8, count: c_int) void {
    const excluded = excluded_c orelse return;
    for (excluded[0..@intCast(@max(count, 0))]) |dir| c.free(dir);
    c.free(@ptrCast(excluded));
}
