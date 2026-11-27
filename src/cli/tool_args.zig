//! tool_args.zig — `codebase-memory-mcp cli <tool>` argument handling: flags
//! to JSON arguments, the stdin policy, and per-tool --help. Implements that
//! part of src/cli/cli.h.

const std = @import("std");
const c = @import("c");
const cli = @import("cli.zig");

const CStr = cli.CStr;
const Str = cli.Str;
const sp = cli.sp;
const eql = cli.eql;
const fmtZ = cli.fmtZ;

const Val = c.yyjson_val;
const MutDoc = c.yyjson_mut_doc;
const MutVal = c.yyjson_mut_val;

/// Replace every from with to in the NUL-terminated contents of buf.
fn replaceByte(buf: []u8, from: u8, to: u8) void {
    for (buf) |*ch| {
        if (ch.* == 0) return;
        if (ch.* == from) ch.* = to;
    }
}

/// Heap-format a one-argument error message for *err_out. Caller frees.
fn heapMsg(comptime fmt: []const u8, arg: []const u8) [*c]u8 {
    var buf: [512]u8 = undefined;
    return c.cbm_strdup(fmtZ(&buf, fmt, .{arg}));
}

/// Parse a tool's input_schema (static lifetime string). Null for an unknown
/// tool or invalid JSON.
fn readSchema(schema_str: CStr) ?*c.yyjson_doc {
    if (schema_str == null) return null;
    return c.yyjson_read(schema_str, c.strlen(schema_str), 0);
}

/// True if the schema's required[] array contains key.
fn schemaRequiredHas(required: ?*Val, key: []const u8) bool {
    if (required == null or !c.yyjson_is_arr(required)) return false;
    var it: c.yyjson_arr_iter = undefined;
    _ = c.yyjson_arr_iter_init(required, &it);
    while (c.yyjson_arr_iter_next(&it)) |v| {
        if (c.yyjson_is_str(v) and eql(sp(c.yyjson_get_str(v)), key)) return true;
    }
    return false;
}

/// A property's JSON-schema "type" string (string/integer/number/boolean/
/// array). Null when the schema or property is unknown — the caller then
/// treats the value as a plain string.
fn schemaType(props: ?*Val, key: Str) ?[]const u8 {
    if (props == null or !c.yyjson_is_obj(props)) return null;
    const p = c.yyjson_obj_get(props, key);
    if (p == null or !c.yyjson_is_obj(p)) return null;
    const t = c.yyjson_obj_get(p, "type");
    return if (t != null and c.yyjson_is_str(t)) sp(c.yyjson_get_str(t)) else null;
}

/// Whether s is a complete strtol/strtod parse (non-empty, nothing left over).
fn fullParse(s: CStr, endp: [*c]u8) bool {
    return endp != null and endp != s and endp.* == 0;
}

/// Append a typed value to the output object under key. For array-typed
/// properties, repeated flags accumulate into a single JSON array.
fn addTyped(out: *MutDoc, obj: *MutVal, key: Str, type_: ?[]const u8, value: CStr, have_value: bool) void {
    const v: CStr = if (have_value) value else "";
    const t = type_ orelse "";
    if (eql(t, "array")) {
        var arr = c.yyjson_mut_obj_get(obj, key);
        if (arr == null or !c.yyjson_mut_is_arr(arr)) {
            arr = c.yyjson_mut_arr(out);
            _ = c.yyjson_mut_obj_add(obj, c.yyjson_mut_strcpy(out, key), arr);
        }
        if (have_value and v[0] == '[') {
            const lit = c.yyjson_read(v, std.mem.len(v), 0);
            if (lit != null) {
                defer c.yyjson_doc_free(lit);
                const lit_root = c.yyjson_doc_get_root(lit);
                if (lit_root != null and c.yyjson_is_arr(lit_root)) {
                    var it: c.yyjson_arr_iter = undefined;
                    _ = c.yyjson_arr_iter_init(lit_root, &it);
                    while (c.yyjson_arr_iter_next(&it)) |elem| {
                        if (c.yyjson_is_str(elem)) {
                            _ = c.yyjson_mut_arr_add_strcpy(out, arr, c.yyjson_get_str(elem));
                        } else {
                            _ = c.yyjson_mut_arr_add_val(arr, c.yyjson_val_mut_copy(out, elem));
                        }
                    }
                    return;
                }
            }
        }
        _ = c.yyjson_mut_arr_add_strcpy(out, arr, v);
        return;
    }

    const vv: [*c]MutVal = if (eql(t, "boolean")) blk: {
        const b = !have_value or eql(sp(value), "true") or eql(sp(value), "1") or eql(sp(value), "yes");
        break :blk c.yyjson_mut_bool(out, b);
    } else if (eql(t, "integer")) blk: {
        var endp: [*c]u8 = null;
        const n = c.strtol(v, &endp, 10);
        break :blk if (fullParse(v, endp)) c.yyjson_mut_int(out, n) else c.yyjson_mut_strcpy(out, v);
    } else if (eql(t, "number")) blk: {
        var endp: [*c]u8 = null;
        const d = c.strtod(v, &endp);
        break :blk if (fullParse(v, endp)) c.yyjson_mut_real(out, d) else c.yyjson_mut_strcpy(out, v);
    } else c.yyjson_mut_strcpy(out, v); // string or unknown type
    _ = c.yyjson_mut_obj_add(obj, c.yyjson_mut_strcpy(out, key), vv);
}

pub export fn cbm_cli_args_from_stdin_allowed(tool_name: [*c]const u8, stdin_is_tty: bool) bool {
    if (stdin_is_tty) return false;
    // Reading stdin waits for EOF, and a caller that never closes an inherited
    // pipe never sends one (upstream 9ca108fc, #1359). Only read it for a tool
    // that requires an argument (top-level or in a oneOf/anyOf branch): a tool
    // whose arguments are all optional (list_projects) runs bare and takes its
    // options from flags or --args-file, and an unknown tool is rejected by name.
    const schema_str = c.cbm_mcp_tool_input_schema(tool_name);
    if (schema_str == null) return false;
    const doc = readSchema(schema_str) orelse return true; // close the channel only on positive evidence
    defer c.yyjson_doc_free(doc);
    const root = c.yyjson_doc_get_root(doc);
    if (c.yyjson_arr_size(c.yyjson_obj_get(root, "required")) > 0) return true;
    for ([_]Str{ "oneOf", "anyOf" }) |name| {
        var it: c.yyjson_arr_iter = undefined;
        if (!c.yyjson_arr_iter_init(c.yyjson_obj_get(root, name), &it)) continue;
        while (c.yyjson_arr_iter_next(&it)) |branch| {
            if (c.yyjson_arr_size(c.yyjson_obj_get(branch, "required")) > 0) return true;
        }
    }
    return false;
}

pub export fn cbm_cli_build_args_json(tool_name: [*c]const u8, argc: c_int, argv: [*c][*c]u8, err_out: [*c][*c]u8) [*c]u8 {
    if (err_out != null) err_out.* = null;

    // The tool's input_schema (NULL for an unknown tool — then every value is
    // treated as a string). Static lifetime; do not free.
    const schema_doc = readSchema(c.cbm_mcp_tool_input_schema(tool_name));
    defer if (schema_doc) |d| c.yyjson_doc_free(d);
    const props: ?*Val = if (schema_doc) |d| c.yyjson_obj_get(c.yyjson_doc_get_root(d), "properties") else null;

    const out = c.yyjson_mut_doc_new(null) orelse return null;
    defer c.yyjson_mut_doc_free(out);
    const obj = c.yyjson_mut_obj(out);
    c.yyjson_mut_doc_set_root(out, obj);

    const n: usize = @intCast(@max(argc, 0));
    var i: usize = 0;
    while (i < n) : (i += 1) {
        const arg = sp(argv[i]);
        if (eql(arg, "--")) break; // end of flag parsing
        if (!std.mem.startsWith(u8, arg, "--")) {
            if (err_out != null) err_out.* = heapMsg("unexpected argument '{s}' (expected --flag value)", arg);
            return null;
        }

        const body = arg[2..]; // skip leading "--"
        var key: [256]u8 = undefined;
        var value: CStr = null;
        var have_value = false;
        if (std.mem.indexOfScalar(u8, body, '=')) |eq| {
            // --key=value : split on the FIRST '='; value may contain '='/spaces.
            _ = fmtZ(&key, "{s}", .{body[0..eq]});
            value = argv[i] + 2 + eq + 1;
            have_value = true;
        } else {
            _ = fmtZ(&key, "{s}", .{body});
            // Consume the next token as the value unless it is itself a flag
            // (then this is a bare boolean/string flag).
            if (i + 1 < n and !std.mem.startsWith(u8, sp(argv[i + 1]), "--")) {
                value = argv[i + 1];
                have_value = true;
                i += 1;
            }
        }

        replaceByte(&key, '-', '_');
        const key_z: Str = @ptrCast(&key);
        const type_ = schemaType(props, key_z);
        if (type_ != null and eql(type_.?, "array") and !have_value) {
            if (err_out != null) err_out.* = heapMsg("flag --{s} requires a value", cli.bufStr(&key));
            return null;
        }
        addTyped(out, obj, key_z, type_, value, have_value);
    }

    var len: usize = 0;
    return c.yyjson_mut_write(out, 0, &len); // malloc'd; caller frees
}

pub export fn cbm_cli_print_tool_help(tool_name: [*c]const u8) c_int {
    const schema_str = c.cbm_mcp_tool_input_schema(tool_name);
    if (schema_str == null) return cli.CLI_ERR;

    const doc = readSchema(schema_str);
    defer if (doc) |d| c.yyjson_doc_free(d);
    const root: ?*Val = if (doc) |d| c.yyjson_doc_get_root(d) else null;
    const props: ?*Val = if (root != null) c.yyjson_obj_get(root, "properties") else null;
    const required: ?*Val = if (root != null) c.yyjson_obj_get(root, "required") else null;

    _ = c.printf("Usage:\n");
    _ = c.printf("  codebase-memory-mcp cli %s --flag value [--flag2 value2 ...]\n", tool_name);
    _ = c.printf("  codebase-memory-mcp cli %s --args-file <path-to-json>\n", tool_name);
    _ = c.printf("  echo '<json>' | codebase-memory-mcp cli %s\n", tool_name);
    _ = c.printf("  codebase-memory-mcp cli %s '<raw-json-args>'\n", tool_name);

    _ = c.printf("\nFlags:\n");
    if (props != null and c.yyjson_is_obj(props)) {
        var iter: c.yyjson_obj_iter = undefined;
        _ = c.yyjson_obj_iter_init(props, &iter);
        while (c.yyjson_obj_iter_next(&iter)) |pkey| {
            const pval = c.yyjson_obj_iter_get_val(pkey);
            const name: CStr = c.yyjson_get_str(pkey);
            if (name == null) continue;
            var type_: CStr = "string";
            var desc: CStr = "";
            if (c.yyjson_is_obj(pval)) {
                const t = c.yyjson_obj_get(pval, "type");
                if (t != null and c.yyjson_is_str(t)) type_ = c.yyjson_get_str(t);
                const d = c.yyjson_obj_get(pval, "description");
                if (d != null and c.yyjson_is_str(d)) desc = c.yyjson_get_str(d);
            }
            var flag: [256]u8 = undefined;
            _ = fmtZ(&flag, "{s}", .{sp(name)});
            replaceByte(&flag, '_', '-');
            const req: CStr = if (schemaRequiredHas(required, sp(name))) " [required]" else "";
            _ = c.printf("  --%s <%s>%s", @as(CStr, @ptrCast(&flag)), type_, req);
            if (desc[0] != 0) _ = c.printf("  %s", desc);
            _ = c.printf("\n");
        }
    }
    return 0;
}
