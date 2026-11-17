//! store.zig — index listing/removal, the `zova-ops` command, the persistent
//! key-value config store (_config.db) and the `config` command. Implements
//! that part of src/cli/cli.h.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");
const cli = @import("cli.zig");

const CStr = cli.CStr;
const sp = cli.sp;
const eql = cli.eql;
const fmtZ = cli.fmtZ;
const CLI_ERR = cli.CLI_ERR;
const CLI_TRUE = cli.CLI_TRUE;

// ── Index management ───────────────────────────────────────────────

/// Whether a cache directory can be resolved: home_dir or the user's home
/// must be known.
fn haveCacheDir(home_dir: CStr) bool {
    return home_dir != null or c.cbm_get_home_dir() != null;
}

/// List the projects in the cache's cbm.zova. With print set, writes one line
/// per project. Returns the project count, 0 when there is no cbm.zova.
pub fn listZovaProjects(home_dir: CStr, print: bool) c_int {
    var path: [1024]u8 = undefined;
    if (!haveCacheDir(home_dir) or c.cbm_zova_user_database_path(&path, path.len) != 0 or
        !c.cbm_file_exists(&path))
    {
        return 0;
    }
    const catalog = c.cbm_zova_catalog_open(&path);
    var listed: c.cbm_zova_catalog_scope_t = .{};
    if (catalog == null or c.cbm_zova_catalog_list(catalog, &listed) != c.CBM_STORE_OK) {
        c.cbm_zova_catalog_close(catalog);
        return 0;
    }
    if (print) {
        var i: usize = 0;
        while (i < @as(usize, @intCast(@max(listed.count, 0)))) : (i += 1) {
            const p = listed.projects[i];
            const selector: CStr = p.selector;
            const root: CStr = p.root_path;
            _ = c.printf("  %s (%s)\n", selector, root);
        }
    }
    const count = listed.count;
    c.cbm_zova_catalog_scope_free(&listed);
    c.cbm_zova_catalog_close(catalog);
    return count;
}

pub export fn cbm_list_indexes(home_dir: [*c]const u8) c_int {
    return listZovaProjects(home_dir, true);
}

pub export fn cbm_remove_indexes(home_dir: [*c]const u8) c_int {
    const count = listZovaProjects(home_dir, false);
    var path: [1024]u8 = undefined;
    if (!haveCacheDir(home_dir) or c.cbm_zova_user_database_path(&path, path.len) != 0) {
        return 0;
    }
    var side: [1040]u8 = undefined;
    _ = c.cbm_unlink(fmtZ(&side, "{s}-wal", .{cli.bufStr(&path)}));
    _ = c.cbm_unlink(fmtZ(&side, "{s}-shm", .{cli.bufStr(&path)}));
    return if (c.cbm_unlink(&path) == 0) count else 0;
}

// ── zova-ops ───────────────────────────────────────────────────────

const Report = c.cbm_zova_operation_report_t;
const Code = c.cbm_zova_operation_code_t;
const Health = c.cbm_zova_health_class_t;

fn workspaceIdValid(value: CStr) bool {
    if (value == null) return false;
    const v = std.mem.span(value);
    if (v.len != 35 or !std.mem.startsWith(u8, v, "w1_")) return false;
    for (v[3..]) |ch| {
        if (!((ch >= '0' and ch <= '9') or (ch >= 'a' and ch <= 'f'))) return false;
    }
    return true;
}

fn healthName(health: Health) [*:0]const u8 {
    return switch (health) {
        c.CBM_ZOVA_HEALTH_OK => "healthy",
        c.CBM_ZOVA_HEALTH_WORKSPACE_REBUILD => "workspace_rebuild",
        c.CBM_ZOVA_HEALTH_WHOLE_FILE_RECOVERY => "whole_file_recovery",
        else => "unknown",
    };
}

fn orEmpty(s: CStr) CStr {
    return if (s != null) s else "";
}

const ReportPaths = struct {
    database: CStr = null,
    input: CStr = null,
    output: CStr = null,
    repo: CStr = null,
    workspace: CStr = null,
};

fn printReport(action: CStr, paths: ReportPaths, has_health: bool, health: Health, code: Code, report: ?*const Report) void {
    const doc = c.yyjson_mut_doc_new(null);
    const root = c.yyjson_mut_obj(doc);
    c.yyjson_mut_doc_set_root(doc, root);
    const ok = code == c.CBM_ZOVA_OPERATION_OK or code == c.CBM_ZOVA_OPERATION_NOOP;
    _ = c.yyjson_mut_obj_add_str(doc, root, "action", orEmpty(action));
    _ = c.yyjson_mut_obj_add_bool(doc, root, "success", ok);
    _ = c.yyjson_mut_obj_add_int(doc, root, "code", code);
    _ = c.yyjson_mut_obj_add_str(doc, root, "name", c.cbm_zova_operation_code_name(code));
    const reason: CStr = if (report) |r| &r.reason else "invalid_argument";
    _ = c.yyjson_mut_obj_add_str(doc, root, "reason", reason);
    _ = c.yyjson_mut_obj_add_int(doc, root, "schema_version", if (report) |r| r.schema_version else 0);
    _ = c.yyjson_mut_obj_add_int(doc, root, "archive_version", if (report) |r| r.archive_version else 0);
    _ = c.yyjson_mut_obj_add_str(doc, root, "database_path", orEmpty(paths.database));
    _ = c.yyjson_mut_obj_add_str(doc, root, "input_path", orEmpty(paths.input));
    _ = c.yyjson_mut_obj_add_str(doc, root, "output_path", orEmpty(paths.output));
    _ = c.yyjson_mut_obj_add_str(doc, root, "repo_path", orEmpty(paths.repo));
    const ws: CStr = if (report != null and report.?.workspace_id[0] != 0) &report.?.workspace_id else orEmpty(paths.workspace);
    _ = c.yyjson_mut_obj_add_str(doc, root, "workspace_id", ws);
    _ = c.yyjson_mut_obj_add_sint(doc, root, "generation", if (report) |r| r.generation else 0);
    _ = c.yyjson_mut_obj_add_uint(doc, root, "database_bytes", if (report) |r| r.database_bytes else 0);
    _ = c.yyjson_mut_obj_add_uint(doc, root, "wal_bytes", if (report) |r| r.wal_bytes else 0);
    _ = c.yyjson_mut_obj_add_uint(doc, root, "free_bytes", if (report) |r| r.free_bytes else 0);
    _ = c.yyjson_mut_obj_add_uint(doc, root, "reclaimable_bytes", if (report) |r| r.reclaimable_bytes else 0);
    _ = c.yyjson_mut_obj_add_uint(doc, root, "page_size", if (report) |r| r.page_size else 0);
    _ = c.yyjson_mut_obj_add_uint(doc, root, "page_count", if (report) |r| r.page_count else 0);
    _ = c.yyjson_mut_obj_add_uint(doc, root, "freelist_count", if (report) |r| r.freelist_count else 0);
    _ = c.yyjson_mut_obj_add_real(doc, root, "elapsed_ms", if (report) |r| r.elapsed_ms else 0.0);
    if (has_health and code != c.CBM_ZOVA_OPERATION_BUSY) { // busy: not classified
        _ = c.yyjson_mut_obj_add_str(doc, root, "health_classification", healthName(health));
    } else {
        _ = c.yyjson_mut_obj_add_null(doc, root, "health_classification");
    }
    const json = c.yyjson_mut_write(doc, c.YYJSON_WRITE_NOFLAG, null);
    if (json != null) {
        _ = c.printf("%s\n", json);
        c.free(json);
    } else {
        _ = c.printf("{\"action\":\"\",\"success\":false,\"code\":-1," ++
            "\"name\":\"invalid\",\"reason\":\"json_failed\"}\n");
    }
    c.yyjson_mut_doc_free(doc);
}

fn usageError(action: CStr, message: [*:0]const u8) c_int {
    var report: Report = .{};
    report.code = c.CBM_ZOVA_OPERATION_INVALID;
    _ = fmtZ(&report.operation, "{s}", .{if (action != null) sp(action) else "zova_ops"});
    _ = fmtZ(&report.reason, "{s}", .{std.mem.span(message)});
    _ = c.fprintf(interop.stderr(), "error: %s\n", message);
    printReport(action, .{}, false, c.CBM_ZOVA_HEALTH_OK, c.CBM_ZOVA_OPERATION_INVALID, &report);
    return 2;
}

// Flags an action can require or accept. --json is always required.
const OPS_INPUT: u32 = 1 << 0;
const OPS_OUTPUT: u32 = 1 << 1;
const OPS_WORKSPACE: u32 = 1 << 2;
const OPS_CONFIRM_WORKSPACE: u32 = 1 << 3;
const OPS_REPO: u32 = 1 << 4;
const OPS_CONFIRM_REPLACE: u32 = 1 << 5;
const OPS_REPLACE: u32 = 1 << 6;

const OpsArgs = struct {
    database_path: CStr = null,
    input: CStr = null,
    output: CStr = null,
    workspace: CStr = null,
    confirmation: CStr = null,
    repo: CStr = null,
    replace: bool = false,
    health: Health = c.CBM_ZOVA_HEALTH_OK, // set by the health action
};

const OpsRun = enum {
    status,
    backup,
    restore,
    export_database,
    import_database,
    export_workspace,
    import_workspace,
    delete_workspace,
    compact,
    health,
    recover_workspace,
    migrate_format,
};

const OpsAction = struct { name: []const u8, required: u32, optional: u32, run: OpsRun };

const ops_actions = [_]OpsAction{
    .{ .name = "status", .required = 0, .optional = 0, .run = .status },
    .{ .name = "backup", .required = OPS_OUTPUT, .optional = 0, .run = .backup },
    .{ .name = "restore", .required = OPS_INPUT | OPS_CONFIRM_REPLACE, .optional = 0, .run = .restore },
    .{ .name = "export-database", .required = OPS_OUTPUT, .optional = 0, .run = .export_database },
    .{ .name = "import-database", .required = OPS_INPUT | OPS_CONFIRM_REPLACE, .optional = 0, .run = .import_database },
    .{ .name = "export-workspace", .required = OPS_WORKSPACE | OPS_OUTPUT, .optional = 0, .run = .export_workspace },
    .{ .name = "import-workspace", .required = OPS_INPUT, .optional = OPS_REPLACE, .run = .import_workspace },
    .{ .name = "delete-workspace", .required = OPS_WORKSPACE | OPS_CONFIRM_WORKSPACE, .optional = 0, .run = .delete_workspace },
    .{ .name = "compact", .required = 0, .optional = 0, .run = .compact },
    .{ .name = "health", .required = 0, .optional = OPS_WORKSPACE, .run = .health },
    .{ .name = "recover-workspace", .required = OPS_WORKSPACE | OPS_REPO, .optional = 0, .run = .recover_workspace },
    .{ .name = "migrate-format", .required = 0, .optional = 0, .run = .migrate_format },
};

fn runOp(run: OpsRun, a: *OpsArgs, r: *Report) Code {
    const db = a.database_path;
    return switch (run) {
        .status => c.cbm_zova_database_status(db, r),
        .backup => c.cbm_zova_database_backup(db, a.output, r),
        .restore => c.cbm_zova_database_restore(db, a.input, true, r),
        .export_database => c.cbm_zova_database_export(db, a.output, r),
        .import_database => c.cbm_zova_database_import(db, a.input, true, r),
        .export_workspace => c.cbm_zova_workspace_export(db, a.workspace, a.output, r),
        .import_workspace => c.cbm_zova_workspace_import(db, a.input, a.replace, r),
        .delete_workspace => c.cbm_zova_workspace_delete(db, a.workspace, a.confirmation, r),
        .compact => c.cbm_zova_database_compact(db, r),
        .health => c.cbm_zova_database_health(db, a.workspace, &a.health, r),
        .recover_workspace => c.cbm_zova_workspace_recover(db, a.workspace, a.repo, r),
        .migrate_format => c.cbm_zova_database_migrate_format(db, r),
    };
}

fn isFlag(s: CStr) bool {
    return std.mem.startsWith(u8, sp(s), "--");
}

pub export fn cbm_cmd_zova_ops(argc: c_int, argv: [*c][*c]u8) c_int {
    if (argc < 1 or argv == null or argv[0] == null or isFlag(argv[0])) {
        return usageError(null, "missing zova-ops action");
    }
    const action: CStr = argv[0];
    const op: OpsAction = for (ops_actions) |a| {
        if (eql(sp(action), a.name)) break a;
    } else return usageError(action, "unknown zova-ops action");

    var args: OpsArgs = .{};
    var json = false;
    var confirm_replace = false;
    const n: usize = @intCast(argc);
    var i: usize = 1;
    while (i < n) : (i += 1) {
        const arg = sp(argv[i]);
        if (eql(arg, "--json")) {
            if (json) return usageError(action, "duplicate --json");
            json = true;
        } else if (eql(arg, "--confirm-replace")) {
            if (confirm_replace) return usageError(action, "duplicate --confirm-replace");
            confirm_replace = true;
        } else if (eql(arg, "--replace")) {
            if (args.replace) return usageError(action, "duplicate --replace");
            args.replace = true;
        } else {
            const target: *CStr = if (eql(arg, "--input"))
                &args.input
            else if (eql(arg, "--output"))
                &args.output
            else if (eql(arg, "--workspace-id"))
                &args.workspace
            else if (eql(arg, "--confirm-workspace"))
                &args.confirmation
            else if (eql(arg, "--repo-path"))
                &args.repo
            else
                return usageError(action, "unknown zova-ops flag");
            if (target.* != null or i + 1 >= n or isFlag(argv[i + 1])) {
                return usageError(action, "missing or duplicate flag value");
            }
            i += 1;
            target.* = argv[i];
        }
    }
    if (!json) return usageError(action, "--json is required");
    if (args.workspace != null and !workspaceIdValid(args.workspace)) {
        return usageError(action, "invalid --workspace-id");
    }
    if (args.confirmation != null and !workspaceIdValid(args.confirmation)) {
        return usageError(action, "invalid --confirm-workspace");
    }

    var given: u32 = 0;
    if (args.input != null) given |= OPS_INPUT;
    if (args.output != null) given |= OPS_OUTPUT;
    if (args.workspace != null) given |= OPS_WORKSPACE;
    if (args.confirmation != null) given |= OPS_CONFIRM_WORKSPACE;
    if (args.repo != null) given |= OPS_REPO;
    if (confirm_replace) given |= OPS_CONFIRM_REPLACE;
    if (args.replace) given |= OPS_REPLACE;
    if ((given & op.required) != op.required or (given & ~(op.required | op.optional)) != 0) {
        return usageError(action, "invalid flags for zova-ops action");
    }

    var database_path = std.mem.zeroes([2048]u8);
    var report: Report = .{};
    var code: Code = c.CBM_ZOVA_OPERATION_INVALID;
    if (c.cbm_zova_workspace_registry_path(&database_path, database_path.len) != 0) {
        _ = fmtZ(&report.reason, "{s}", .{"database_path_failed"});
    } else {
        args.database_path = &database_path;
        code = runOp(op.run, &args, &report);
    }
    report.code = code;
    printReport(action, .{
        .database = &database_path,
        .input = args.input,
        .output = args.output,
        .repo = args.repo,
        .workspace = args.workspace,
    }, op.run == .health, args.health, code, &report);
    return if (code == c.CBM_ZOVA_OPERATION_OK or code == c.CBM_ZOVA_OPERATION_NOOP) 0 else 1;
}

// ── Config store (persistent key-value in _config.db) ─────────────

const Config = struct {
    db: ?*c.sqlite3,
    get_buf: [4096]u8, // buffer for cbm_config_get return values
};

fn cfgOf(cfg: ?*c.cbm_config_t) ?*Config {
    return @ptrCast(@alignCast(cfg));
}

/// SQLITE_STATIC (a null destructor): every bound string outlives the
/// statement, which is finalized before the binding functions return.
const sqlite_static: c.sqlite3_destructor_type = null;

pub export fn cbm_config_open(cache_dir: [*c]const u8) ?*c.cbm_config_t {
    if (cache_dir == null) return null;

    var dbpath: [1024]u8 = undefined;
    _ = fmtZ(&dbpath, "{s}/_config.db", .{sp(cache_dir)});

    // Ensure directory exists
    _ = cli.mkdirp(cache_dir, cli.DIR_PERMS);

    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(&dbpath, &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return null;
    }

    var err_msg: [*c]u8 = null;
    if (c.sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT)", null, null, &err_msg) != c.SQLITE_OK) {
        c.sqlite3_free(err_msg);
        _ = c.sqlite3_close(db);
        return null;
    }

    const cfg = interop.callocN(Config, 1);
    if (cfg == null) {
        _ = c.sqlite3_close(db);
        return null;
    }
    cfg.*.db = db;
    return @ptrCast(cfg);
}

pub export fn cbm_config_close(cfg_c: ?*c.cbm_config_t) void {
    const cfg = cfgOf(cfg_c) orelse return;
    if (cfg.db != null) _ = c.sqlite3_close(cfg.db);
    c.free(cfg);
}

/// Prepare sql and bind key (and value, when given) as parameters 1 and 2.
fn prepare(db: ?*c.sqlite3, sql: [*:0]const u8, key: CStr, value: CStr) ?*c.sqlite3_stmt {
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql, -1, &stmt, null) != c.SQLITE_OK) return null;
    _ = c.sqlite3_bind_text(stmt, 1, key, -1, sqlite_static);
    if (value != null) _ = c.sqlite3_bind_text(stmt, 2, value, -1, sqlite_static);
    return stmt;
}

pub export fn cbm_config_get(cfg_c: ?*c.cbm_config_t, key: [*c]const u8, default_val: [*c]const u8) [*c]const u8 {
    const cfg = cfgOf(cfg_c) orelse return default_val;
    if (key == null) return default_val;
    const stmt = prepare(cfg.db, "SELECT value FROM config WHERE key = ?", key, null) orelse return default_val;

    var result: CStr = default_val;
    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        const val: CStr = c.sqlite3_column_text(stmt, 0);
        if (val != null) {
            result = fmtZ(&cfg.get_buf, "{s}", .{sp(val)});
        }
    }
    _ = c.sqlite3_finalize(stmt);
    return result;
}

pub export fn cbm_config_get_bool(cfg: ?*c.cbm_config_t, key: [*c]const u8, default_val: bool) bool {
    const val = cbm_config_get(cfg, key, null);
    if (val == null) return default_val;
    const v = sp(val);
    if (eql(v, "true") or eql(v, "1") or eql(v, "on")) return true;
    if (eql(v, "false") or eql(v, "0") or eql(v, "off")) return false;
    return default_val;
}

pub export fn cbm_config_get_int(cfg: ?*c.cbm_config_t, key: [*c]const u8, default_val: c_int) c_int {
    const val = cbm_config_get(cfg, key, null);
    if (val == null) return default_val;
    var endptr: [*c]u8 = null;
    const v = c.strtol(val, &endptr, 10);
    if (endptr == val or endptr.* != 0) return default_val;
    return @truncate(v);
}

fn execKeyed(cfg_c: ?*c.cbm_config_t, sql: [*:0]const u8, key: CStr, value: CStr) c_int {
    const cfg = cfgOf(cfg_c) orelse return CLI_ERR;
    const stmt = prepare(cfg.db, sql, key, value) orelse return CLI_ERR;
    const rc: c_int = if (c.sqlite3_step(stmt) == c.SQLITE_DONE) 0 else CLI_ERR;
    _ = c.sqlite3_finalize(stmt);
    return rc;
}

pub export fn cbm_config_set(cfg: ?*c.cbm_config_t, key: [*c]const u8, value: [*c]const u8) c_int {
    if (key == null or value == null) return CLI_ERR;
    return execKeyed(cfg, "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)", key, value);
}

pub export fn cbm_config_delete(cfg: ?*c.cbm_config_t, key: [*c]const u8) c_int {
    if (key == null) return CLI_ERR;
    return execKeyed(cfg, "DELETE FROM config WHERE key = ?", key, null);
}

// ── Config CLI subcommand ──────────────────────────────────────────

// Single config-key table driving help, list, get, set, reset
// (upstream f59d24fd, #1522 bug 2). list used to print stored-or-DEFAULT
// while get printed stored-or-EMPTY, so every unset key (and every typo,
// at exit 0) read as "". Defaults mirror the runtime readers' fallbacks;
// keep them in sync.
const ConfigKey = struct { key: [*:0]const u8, default_value: [*:0]const u8, help: [*:0]const u8 };

const config_keys = [_]ConfigKey{
    .{ .key = c.CBM_CONFIG_AUTO_INDEX, .default_value = "false", .help = "Enable auto-indexing on MCP session start" },
    .{ .key = c.CBM_CONFIG_AUTO_INDEX_LIMIT, .default_value = "50000", .help = "Max files for auto-indexing new projects" },
    .{ .key = c.CBM_CONFIG_AUTO_WATCH, .default_value = "true", .help = "Register background git watcher on session connect" },
    .{ .key = c.CBM_CONFIG_INDEX_MAX_FILES, .default_value = "0", .help = "Max discovered files for index_repository (0 = unlimited)" },
    .{ .key = c.CBM_CONFIG_INDEX_MAX_SOURCE_MB, .default_value = "0", .help = "Max discovered source size in MiB for index_repository (0 = unlimited)" },
};

fn configKeyLookup(key: CStr) ?*const ConfigKey {
    if (key == null) return null;
    for (&config_keys) |*def| {
        if (eql(std.mem.span(def.key), sp(key))) return def;
    }
    return null;
}

fn printUnknownKey(key: CStr) void {
    const err = interop.stderr();
    _ = c.fprintf(err, "error: unknown config key: %s\nKnown keys:", orEmpty(key));
    for (config_keys) |def| _ = c.fprintf(err, " %s", def.key);
    _ = c.fprintf(err, "\n");
}

pub export fn cbm_cmd_config(argc: c_int, argv: [*c][*c]u8) c_int {
    if (argc == 0) {
        _ = c.printf("Usage: codebase-memory-mcp config <command> [args]\n\n");
        _ = c.printf("Commands:\n");
        _ = c.printf("  list             Show all config values\n");
        _ = c.printf("  get <key>        Get a config value\n");
        _ = c.printf("  set <key> <val>  Set a config value\n");
        _ = c.printf("  reset <key>      Reset a key to default\n\n");
        _ = c.printf("Config keys:\n");
        for (config_keys) |def| {
            _ = c.printf("  %-25s  default=%-10s  %s\n", def.key, def.default_value, def.help);
        }
        return 0;
    }

    const err = interop.stderr();
    if (c.cbm_get_home_dir() == null) {
        _ = c.fprintf(err, "error: HOME not set (use USERPROFILE on Windows)\n");
        return CLI_TRUE;
    }

    var cache_dir: [1024]u8 = undefined;
    _ = fmtZ(&cache_dir, "{s}", .{sp(c.cbm_resolve_cache_dir())});

    const cfg = cbm_config_open(&cache_dir) orelse {
        _ = c.fprintf(err, "error: cannot open config database\n");
        return CLI_TRUE;
    };

    var rc: c_int = 0;
    const cmd = sp(argv[0]);
    const key: CStr = if (argc >= 2) argv[1] else null;
    if (eql(cmd, "list") or eql(cmd, "ls")) {
        _ = c.printf("Configuration:\n");
        for (config_keys) |def| {
            _ = c.printf("  %-25s = %-10s\n", def.key, cbm_config_get(cfg, def.key, def.default_value));
        }
    } else if (eql(cmd, "get")) {
        if (argc < 2) {
            _ = c.fprintf(err, "Usage: config get <key>\n");
            rc = CLI_TRUE;
        } else if (configKeyLookup(key)) |def| {
            _ = c.printf("%s\n", cbm_config_get(cfg, def.key, def.default_value));
        } else {
            printUnknownKey(key);
            rc = CLI_TRUE;
        }
    } else if (eql(cmd, "set")) {
        if (argc < 3) {
            _ = c.fprintf(err, "Usage: config set <key> <value>\n");
            rc = CLI_TRUE;
        } else if (configKeyLookup(key) == null) {
            printUnknownKey(key);
            rc = CLI_TRUE;
        } else if (cbm_config_set(cfg, key, argv[2]) == 0) {
            _ = c.printf("%s = %s\n", key, argv[2]);
        } else {
            _ = c.fprintf(err, "error: failed to set %s\n", key);
            rc = CLI_TRUE;
        }
    } else if (eql(cmd, "reset")) {
        if (argc < 2) {
            _ = c.fprintf(err, "Usage: config reset <key>\n");
            rc = CLI_TRUE;
        } else if (configKeyLookup(key) == null) {
            printUnknownKey(key);
            rc = CLI_TRUE;
        } else {
            _ = cbm_config_delete(cfg, key);
            _ = c.printf("%s reset to default\n", key);
        }
    } else {
        _ = c.fprintf(err, "Unknown config command: %s\n", argv[0]);
        rc = CLI_TRUE;
    }

    cbm_config_close(cfg);
    return rc;
}
