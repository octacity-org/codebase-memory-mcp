//! parser.zig — Cypher lexer and recursive-descent parser.
//!
//! Implements the lexing and parsing half of src/cypher/cypher.h: cbm_lex,
//! cbm_parse, cbm_cypher_parse and the functions that free their results.
//!
//! Tokens and the AST are the C structs the header declares, and every one of
//! them is malloc'd: C callers and tests read the AST directly and release it
//! with cbm_lex_free, cbm_parse_free and cbm_query_free.

const std = @import("std");
const c = @import("c");
const interop = @import("../foundation/interop.zig");

const Tok = c.cbm_token_type_t;
const Token = c.cbm_token_t;
const Expr = c.cbm_expr_t;
const Cond = c.cbm_condition_t;
const ReturnItem = c.cbm_return_item_t;
const ReturnClause = c.cbm_return_clause_t;
const Query = c.cbm_query_t;
const CStr = interop.CStr;
const mallocN = interop.mallocN;
const callocN = interop.callocN;
const reallocN = interop.reallocN;
const safeReallocN = interop.safeReallocN;
const strFree = interop.strFree;
const heapStrdup = interop.heapStrdup;
const sp = interop.sp;
const bprint = interop.bprint;
const ciEq = interop.ciEq;

const NOT_FOUND: c_int = c.CBM_NOT_FOUND;
const INIT_CAP4 = 4;
const INIT_CAP8 = 8;
const MAX_VARS = 16; // binding_t holds this many variables
const RETURN_MAX_ITEMS = 32; // projection rows are this many columns wide
const ORDER_KEYS_MAX = c.CBM_CYPHER_ORDER_KEYS_MAX;

// ── Helpers ────────────────────────────────────────────────────────

pub inline fn ix(i: c_int) usize {
    return @intCast(i);
}

/// C's (int) conversion of a long: keep the low bits.
pub inline fn narrow(v: c_long) c_int {
    return @truncate(v);
}

fn heapStrndup(s: CStr, n: usize) [*c]u8 {
    const d = mallocN(u8, n + 1);
    if (d != null) {
        @memcpy(d[0..n], s[0..n]);
        d[n] = 0;
    }
    return d;
}

fn strEql(a: CStr, b: []const u8) bool {
    return a != null and std.mem.eql(u8, std.mem.span(a), b);
}

// ══════════════════════════════════════════════════════════════════
//  LEXER
// ══════════════════════════════════════════════════════════════════

fn lexPushN(r: *c.cbm_lex_result_t, ty: Tok, start: CStr, len: usize, pos: c_int) void {
    if (r.count >= r.capacity) {
        r.capacity = if (r.capacity != 0) r.capacity *% 2 else 32;
        r.tokens = safeReallocN(Token, r.tokens, ix(r.capacity));
    }
    r.tokens[ix(r.count)] = .{ .type = ty, .text = heapStrndup(start, len), .pos = pos };
    r.count += 1;
}

fn lexPush(r: *c.cbm_lex_result_t, ty: Tok, text: CStr, pos: c_int) void {
    if (r.count >= r.capacity) {
        r.capacity = if (r.capacity != 0) r.capacity *% 2 else 32;
        r.tokens = safeReallocN(Token, r.tokens, ix(r.capacity));
    }
    r.tokens[ix(r.count)] = .{ .type = ty, .text = heapStrdup(text), .pos = pos };
    r.count += 1;
}

/// Parse a string literal (with escape handling) into the token list.
/// pos.* is the character after the opening quote; it ends past the closing one.
fn lexStringLiteral(input: CStr, len: c_int, pos: *c_int, quote: u8, out: *c.cbm_lex_result_t) void {
    const start = pos.*;
    var buf: [4096]u8 = undefined;
    var blen: usize = 0;
    const max_blen = buf.len - 1;
    while (pos.* < len and input[ix(pos.*)] != quote) {
        if (input[ix(pos.*)] == '\\' and pos.* + 1 < len) {
            pos.* += 1;
            if (blen < max_blen) {
                buf[blen] = switch (input[ix(pos.*)]) {
                    'n' => '\n',
                    't' => '\t',
                    '\\' => '\\',
                    else => |ch| ch,
                };
                blen += 1;
            }
        } else if (blen < max_blen) {
            buf[blen] = input[ix(pos.*)];
            blen += 1;
        }
        pos.* += 1;
    }
    buf[blen] = 0;
    if (pos.* < len) pos.* += 1; // skip closing quote
    lexPush(out, c.TOK_STRING, &buf, start - 1);
}

const Keyword = struct { name: []const u8, ty: Tok };

/// Keyword table (case-insensitive lookup).
const KEYWORDS = [_]Keyword{
    // Core query
    .{ .name = "MATCH", .ty = c.TOK_MATCH },
    .{ .name = "WHERE", .ty = c.TOK_WHERE },
    .{ .name = "RETURN", .ty = c.TOK_RETURN },
    .{ .name = "ORDER", .ty = c.TOK_ORDER },
    .{ .name = "BY", .ty = c.TOK_BY },
    .{ .name = "LIMIT", .ty = c.TOK_LIMIT },
    .{ .name = "AND", .ty = c.TOK_AND },
    .{ .name = "OR", .ty = c.TOK_OR },
    .{ .name = "AS", .ty = c.TOK_AS },
    .{ .name = "DISTINCT", .ty = c.TOK_DISTINCT },
    .{ .name = "COUNT", .ty = c.TOK_COUNT },
    .{ .name = "CONTAINS", .ty = c.TOK_CONTAINS },
    .{ .name = "STARTS", .ty = c.TOK_STARTS },
    .{ .name = "WITH", .ty = c.TOK_WITH },
    .{ .name = "NOT", .ty = c.TOK_NOT },
    .{ .name = "ASC", .ty = c.TOK_ASC },
    .{ .name = "DESC", .ty = c.TOK_DESC },
    // Phase 1-2: operators + expression
    .{ .name = "ENDS", .ty = c.TOK_ENDS },
    .{ .name = "IN", .ty = c.TOK_IN },
    .{ .name = "IS", .ty = c.TOK_IS },
    .{ .name = "NULL", .ty = c.TOK_NULL_KW },
    .{ .name = "XOR", .ty = c.TOK_XOR },
    // Phase 3-4: SKIP, UNION, UNWIND, aggregates
    .{ .name = "SKIP", .ty = c.TOK_SKIP },
    .{ .name = "UNION", .ty = c.TOK_UNION },
    .{ .name = "UNWIND", .ty = c.TOK_UNWIND },
    .{ .name = "SUM", .ty = c.TOK_SUM },
    .{ .name = "AVG", .ty = c.TOK_AVG },
    .{ .name = "MIN", .ty = c.TOK_MIN_KW },
    .{ .name = "MAX", .ty = c.TOK_MAX_KW },
    .{ .name = "COLLECT", .ty = c.TOK_COLLECT },
    // Phase 5: string functions + CASE
    .{ .name = "toLower", .ty = c.TOK_TOLOWER },
    .{ .name = "toUpper", .ty = c.TOK_TOUPPER },
    .{ .name = "toString", .ty = c.TOK_TOSTRING },
    .{ .name = "tolower", .ty = c.TOK_TOLOWER },
    .{ .name = "toupper", .ty = c.TOK_TOUPPER },
    .{ .name = "tostring", .ty = c.TOK_TOSTRING },
    .{ .name = "CASE", .ty = c.TOK_CASE },
    .{ .name = "WHEN", .ty = c.TOK_WHEN },
    .{ .name = "THEN", .ty = c.TOK_THEN },
    .{ .name = "ELSE", .ty = c.TOK_ELSE },
    .{ .name = "END", .ty = c.TOK_END },
    // Phase 7: OPTIONAL
    .{ .name = "OPTIONAL", .ty = c.TOK_OPTIONAL },
    // Recognized-but-unsupported write/admin keywords
    .{ .name = "CREATE", .ty = c.TOK_CREATE },
    .{ .name = "DELETE", .ty = c.TOK_DELETE },
    .{ .name = "DETACH", .ty = c.TOK_DETACH },
    .{ .name = "SET", .ty = c.TOK_SET },
    .{ .name = "REMOVE", .ty = c.TOK_REMOVE },
    .{ .name = "MERGE", .ty = c.TOK_MERGE },
    .{ .name = "YIELD", .ty = c.TOK_YIELD },
    .{ .name = "CALL", .ty = c.TOK_CALL },
    .{ .name = "ALL", .ty = c.TOK_ALL },
    .{ .name = "TRUE", .ty = c.TOK_TRUE },
    .{ .name = "FALSE", .ty = c.TOK_FALSE },
    .{ .name = "EXISTS", .ty = c.TOK_EXISTS },
    .{ .name = "MANDATORY", .ty = c.TOK_MANDATORY },
    .{ .name = "FOREACH", .ty = c.TOK_FOREACH },
    .{ .name = "ON", .ty = c.TOK_ON },
    .{ .name = "ADD", .ty = c.TOK_ADD },
    .{ .name = "CONSTRAINT", .ty = c.TOK_CONSTRAINT },
    .{ .name = "DO", .ty = c.TOK_DO },
    .{ .name = "DROP", .ty = c.TOK_DROP },
    .{ .name = "FOR", .ty = c.TOK_FOR },
    .{ .name = "FROM", .ty = c.TOK_FROM },
    .{ .name = "GRAPH", .ty = c.TOK_GRAPH },
    .{ .name = "OF", .ty = c.TOK_OF },
    .{ .name = "REQUIRE", .ty = c.TOK_REQUIRE },
    .{ .name = "SCALAR", .ty = c.TOK_SCALAR },
    .{ .name = "UNIQUE", .ty = c.TOK_UNIQUE },
};

fn keywordLookup(word: []const u8) Tok {
    for (KEYWORDS) |kw| {
        if (std.ascii.eqlIgnoreCase(word, kw.name)) return kw.ty;
    }
    return c.TOK_IDENT;
}

const TwoChar = struct { c1: u8, c2: u8, ty: Tok, text: [*:0]const u8 };

const TWO_CHAR_TOKENS = [_]TwoChar{
    .{ .c1 = '!', .c2 = '=', .ty = c.TOK_NEQ, .text = "!=" },
    .{ .c1 = '<', .c2 = '>', .ty = c.TOK_NEQ, .text = "<>" },
    .{ .c1 = '=', .c2 = '~', .ty = c.TOK_EQTILDE, .text = "=~" },
    .{ .c1 = '>', .c2 = '=', .ty = c.TOK_GTE, .text = ">=" },
    .{ .c1 = '<', .c2 = '=', .ty = c.TOK_LTE, .text = "<=" },
    .{ .c1 = '.', .c2 = '.', .ty = c.TOK_DOTDOT, .text = ".." },
};

/// Match a two-character token at i. Returns true and advances i if matched.
fn lexTryTwoChar(input: CStr, len: c_int, i: *c_int, out: *c.cbm_lex_result_t) bool {
    const ch = input[ix(i.*)];
    if (i.* + 1 >= len) return false;
    const ch2 = input[ix(i.* + 1)];
    for (TWO_CHAR_TOKENS) |pair| {
        if (ch == pair.c1 and ch2 == pair.c2) {
            lexPush(out, pair.ty, pair.text, i.*);
            i.* += 2;
            return true;
        }
    }
    return false;
}

/// Single-character token type, or TOK_EOF if ch is not one.
fn lexSingleChar(ch: u8) Tok {
    return switch (ch) {
        '(' => c.TOK_LPAREN,
        ')' => c.TOK_RPAREN,
        '[' => c.TOK_LBRACKET,
        ']' => c.TOK_RBRACKET,
        '-' => c.TOK_DASH,
        '>' => c.TOK_GT,
        '<' => c.TOK_LT,
        ':' => c.TOK_COLON,
        '.' => c.TOK_DOT,
        '{' => c.TOK_LBRACE,
        '}' => c.TOK_RBRACE,
        '*' => c.TOK_STAR,
        ',' => c.TOK_COMMA,
        '=' => c.TOK_EQ,
        '|' => c.TOK_PIPE,
        else => c.TOK_EOF,
    };
}

/// Lex an identifier or keyword at i. Returns true if matched.
fn lexTryIdent(input: CStr, len: c_int, i: *c_int, out: *c.cbm_lex_result_t) bool {
    const ch = input[ix(i.*)];
    if (!std.ascii.isAlphabetic(ch) and ch != '_') return false;
    const start = i.*;
    while (i.* < len and (std.ascii.isAlphanumeric(input[ix(i.*)]) or input[ix(i.*)] == '_')) {
        i.* += 1;
    }
    // Keywords are looked up in the first 255 bytes, like the C word buffer.
    const wlen = @min(ix(i.* - start), 255);
    const ty = keywordLookup(input[ix(start) .. ix(start) + wlen]);
    lexPushN(out, ty, input + ix(start), ix(i.* - start), start);
    return true;
}

/// Lex a number at i. Returns true if matched.
fn lexTryNumber(input: CStr, len: c_int, i: *c_int, out: *c.cbm_lex_result_t) bool {
    const ch = input[ix(i.*)];
    if (!std.ascii.isDigit(ch) and
        !(ch == '.' and i.* + 1 < len and std.ascii.isDigit(input[ix(i.* + 1)])))
    {
        return false;
    }
    const start = i.*;
    while (i.* < len and (std.ascii.isDigit(input[ix(i.*)]) or
        (input[ix(i.*)] == '.' and i.* + 1 < len and input[ix(i.* + 1)] != '.')))
    {
        i.* += 1;
    }
    lexPushN(out, c.TOK_NUMBER, input + ix(start), ix(i.* - start), start);
    return true;
}

/// Skip whitespace and comments. Returns true if something was skipped.
fn lexSkipWhitespaceComments(input: CStr, len: c_int, i: *c_int) bool {
    if (std.ascii.isWhitespace(input[ix(i.*)])) {
        i.* += 1;
        return true;
    }
    if (i.* + 1 < len and input[ix(i.*)] == '/' and input[ix(i.* + 1)] == '/') {
        while (i.* < len and input[ix(i.*)] != '\n') i.* += 1;
        return true;
    }
    // SQL-style -- single-line comment
    if (i.* + 1 < len and input[ix(i.*)] == '-' and input[ix(i.* + 1)] == '-') {
        while (i.* < len and input[ix(i.*)] != '\n') i.* += 1;
        return true;
    }
    if (i.* + 1 < len and input[ix(i.*)] == '/' and input[ix(i.* + 1)] == '*') {
        i.* += 2;
        while (i.* + 1 < len and !(input[ix(i.*)] == '*' and input[ix(i.* + 1)] == '/')) {
            i.* += 1;
        }
        if (i.* + 1 < len) i.* += 2;
        return true;
    }
    return false;
}

pub export fn cbm_lex(input: [*c]const u8, out_c: [*c]c.cbm_lex_result_t) c_int {
    const out: *c.cbm_lex_result_t = out_c;
    out.* = .{};
    if (input == null) return NOT_FOUND;

    const len: c_int = @bitCast(@as(u32, @truncate(c.strlen(input))));
    var i: c_int = 0;

    while (i < len) {
        if (lexSkipWhitespaceComments(input, len, &i)) continue;

        const ch = input[ix(i)];

        // String literals
        if (ch == '"' or ch == '\'') {
            i += 1;
            lexStringLiteral(input, len, &i, ch, out);
            continue;
        }

        // Numbers — stop before ".." (DOTDOT operator)
        if (lexTryNumber(input, len, &i, out)) continue;

        // Identifiers / keywords
        if (lexTryIdent(input, len, &i, out)) continue;

        // Two-character tokens
        if (lexTryTwoChar(input, len, &i, out)) continue;

        // Single-character tokens
        const stype = lexSingleChar(ch);
        if (stype != c.TOK_EOF) {
            const buf = [2]u8{ ch, 0 };
            lexPush(out, stype, &buf, i);
            i += 1;
            continue;
        }

        // Unknown character — skip
        i += 1;
    }

    lexPush(out, c.TOK_EOF, "", i);
    return 0;
}

pub export fn cbm_lex_free(r_c: [*c]c.cbm_lex_result_t) void {
    if (r_c == null) return;
    const r: *c.cbm_lex_result_t = r_c;
    var i: c_int = 0;
    while (i < r.count) : (i += 1) strFree(&(r.tokens + ix(i)).*.text);
    c.free(@ptrCast(r.tokens));
    c.free(@ptrCast(r.@"error"));
    r.* = .{};
}

// ══════════════════════════════════════════════════════════════════
//  PARSER
// ══════════════════════════════════════════════════════════════════

/// The WHERE grammar descends once per nested '(' and once per NOT, so parse
/// depth follows the query text rather than anything bounded (upstream
/// 886a8856). 256 is far past any hand-written or generated query while
/// leaving the stack untouched.
const MAX_PARSE_DEPTH = 256;

const Parser = struct {
    tokens: [*c]const Token,
    count: c_int,
    pos: c_int = 0,
    depth: c_int = 0, // current recursive-descent depth; see MAX_PARSE_DEPTH
    err: [512]u8 = @splat(0),

    fn setError(p: *Parser, comptime fmt: []const u8, args: anytype) void {
        _ = bprint(&p.err, fmt, args);
    }

    fn hasError(p: *const Parser) bool {
        return p.err[0] != 0;
    }
};

/// Enter one level of recursive descent. Returns false when the cap is hit,
/// in which case the caller must return null without recursing.
fn depthEnter(p: *Parser) bool {
    if (p.depth >= MAX_PARSE_DEPTH) {
        p.setError("expression nested deeper than {d} levels", .{MAX_PARSE_DEPTH});
        return false;
    }
    p.depth += 1;
    return true;
}

fn depthLeave(p: *Parser) void {
    p.depth -= 1;
}

fn peek(p: *Parser) *const Token {
    if (p.pos >= p.count) return (p.tokens + ix(p.count - 1)); // EOF
    return (p.tokens + ix(p.pos));
}

fn advance(p: *Parser) *const Token {
    if (p.pos >= p.count) return (p.tokens + ix(p.count - 1));
    const t: *const Token = p.tokens + ix(p.pos);
    p.pos += 1;
    return t;
}

fn check(p: *Parser, ty: Tok) bool {
    return peek(p).type == ty;
}

fn match(p: *Parser, ty: Tok) bool {
    if (check(p, ty)) {
        _ = advance(p);
        return true;
    }
    return false;
}

fn expect(p: *Parser, ty: Tok) ?*const Token {
    if (check(p, ty)) return advance(p);
    p.setError("expected token type {d}, got {d} at pos {d}", .{ ty, peek(p).type, peek(p).pos });
    return null;
}

/// Type of the token after the current one, or null at the end.
fn nextType(p: *Parser) ?Tok {
    if (p.pos + 1 >= p.count) return null;
    return p.tokens[ix(p.pos + 1)].type;
}

/// Parse inline properties: {key: "value", ...}
fn parseProps(p: *Parser, out: *[*c]c.cbm_prop_filter_t, count: *c_int) c_int {
    out.* = null;
    count.* = 0;
    if (!match(p, c.TOK_LBRACE)) return 0;

    var cap: c_int = INIT_CAP4;
    var n: c_int = 0;
    var arr = mallocN(c.cbm_prop_filter_t, ix(cap));
    if (arr == null) return NOT_FOUND;

    while (!check(p, c.TOK_RBRACE) and !check(p, c.TOK_EOF)) {
        // Every failure frees the entries already parsed, not only the array.
        const key = expect(p, c.TOK_IDENT) orelse {
            propsFree(arr, n);
            return NOT_FOUND;
        };
        if (expect(p, c.TOK_COLON) == null) {
            propsFree(arr, n);
            return NOT_FOUND;
        }
        const val = expect(p, c.TOK_STRING) orelse {
            propsFree(arr, n);
            return NOT_FOUND;
        };

        if (n >= cap) {
            const new_cap = cap * 2;
            const tmp = reallocN(c.cbm_prop_filter_t, arr, ix(new_cap));
            if (tmp == null) {
                propsFree(arr, n);
                return NOT_FOUND;
            }
            arr = tmp;
            cap = new_cap;
        }
        var new_key: CStr = heapStrdup(key.text);
        var new_val: CStr = heapStrdup(val.text);
        if (new_key == null or new_val == null) {
            strFree(&new_key);
            strFree(&new_val);
            propsFree(arr, n);
            return NOT_FOUND;
        }
        arr[ix(n)] = .{ .key = new_key, .value = new_val };
        n += 1;

        _ = match(p, c.TOK_COMMA); // optional comma
    }
    _ = expect(p, c.TOK_RBRACE);

    out.* = arr;
    count.* = n;
    return 0;
}

fn propsFree(arr: [*c]c.cbm_prop_filter_t, n: c_int) void {
    var i: c_int = 0;
    while (i < n) : (i += 1) {
        strFree(&(arr + ix(i)).*.key);
        strFree(&(arr + ix(i)).*.value);
    }
    c.free(@ptrCast(arr));
}

/// Parse node: (variable:Label {props})
fn parseNode(p: *Parser, out: *c.cbm_node_pattern_t) c_int {
    out.* = .{};
    if (expect(p, c.TOK_LPAREN) == null) return NOT_FOUND;

    // Optional variable
    if (check(p, c.TOK_IDENT)) out.variable = heapStrdup(advance(p).text);

    // Optional :Label, with openCypher label alternation :A|B|C (#242).
    // Stored as a single "A|B|C" string; the matcher splits on '|'.
    if (match(p, c.TOK_COLON)) {
        const label = expect(p, c.TOK_IDENT) orelse return NOT_FOUND;
        var lbuf: [256]u8 = undefined;
        var ll = bprint(&lbuf, "{s}", .{sp(label.text)});
        while (match(p, c.TOK_PIPE)) {
            const alt = expect(p, c.TOK_IDENT) orelse return NOT_FOUND;
            const room: []u8 = if (ll < lbuf.len) lbuf[ll..] else lbuf[0..0];
            ll += bprint(room, "|{s}", .{sp(alt.text)});
            if (ll >= lbuf.len) break; // buffer full
        }
        out.label = heapStrdup(&lbuf);
    }

    // Optional {props}
    if (check(p, c.TOK_LBRACE)) {
        if (parseProps(p, &out.props, &out.prop_count) < 0) return NOT_FOUND;
    }

    if (expect(p, c.TOK_RPAREN) == null) return NOT_FOUND;
    return 0;
}

fn tokenInt(t: *const Token) c_int {
    return narrow(c.strtol(t.text, null, 10));
}

/// Parse *min..max hop range after the star token has been consumed.
fn parseHopRange(p: *Parser, min_hops: *c_int, max_hops: *c_int) void {
    if (check(p, c.TOK_NUMBER)) {
        const val = tokenInt(peek(p));
        _ = advance(p);
        if (match(p, c.TOK_DOTDOT)) {
            min_hops.* = val;
            max_hops.* = if (check(p, c.TOK_NUMBER)) tokenInt(advance(p)) else 0;
        } else {
            // *N means 1..N
            min_hops.* = 1;
            max_hops.* = val;
        }
    } else if (match(p, c.TOK_DOTDOT)) {
        min_hops.* = 1;
        max_hops.* = if (check(p, c.TOK_NUMBER)) tokenInt(advance(p)) else 0;
    } else {
        // * alone = unbounded
        min_hops.* = 1;
        max_hops.* = 0;
    }
}

fn strListFree(list: [*c]CStr, n: c_int) void {
    var i: c_int = 0;
    while (i < n) : (i += 1) strFree((list + ix(i)));
    c.free(@ptrCast(list));
}

/// Parse relationship type list after ':' inside brackets. Returns -1 on error.
fn parseRelTypes(p: *Parser, out: *c.cbm_rel_pattern_t) c_int {
    var cap: c_int = INIT_CAP4;
    var n: c_int = 0;
    var types = mallocN(CStr, ix(cap));
    if (types == null) return NOT_FOUND;

    var t = expect(p, c.TOK_IDENT) orelse {
        c.free(@ptrCast(types));
        return NOT_FOUND;
    };
    const first_type = heapStrdup(t.text);
    if (first_type == null) {
        c.free(@ptrCast(types));
        return NOT_FOUND;
    }
    types[ix(n)] = first_type;
    n += 1;

    while (match(p, c.TOK_PIPE)) {
        t = expect(p, c.TOK_IDENT) orelse {
            strListFree(types, n);
            return NOT_FOUND;
        };
        if (n >= cap) {
            const new_cap = cap * 2;
            const tmp = reallocN(CStr, types, ix(new_cap));
            if (tmp == null) {
                strListFree(types, n);
                return NOT_FOUND;
            }
            types = tmp;
            cap = new_cap;
        }
        const next_type = heapStrdup(t.text);
        if (next_type == null) {
            strListFree(types, n);
            return NOT_FOUND;
        }
        types[ix(n)] = next_type;
        n += 1;
    }

    out.types = types;
    out.type_count = n;
    return 0;
}

/// Parse bracket content of a relationship: [var:TYPE*hops]
fn parseRelBracket(p: *Parser, out: *c.cbm_rel_pattern_t) c_int {
    // Optional variable
    if (check(p, c.TOK_IDENT) and !check(p, c.TOK_COLON)) {
        out.variable = heapStrdup(advance(p).text);
    }
    // Optional :Types
    if (match(p, c.TOK_COLON)) {
        if (parseRelTypes(p, out) < 0) return NOT_FOUND;
    }
    // Optional *hop_range
    if (match(p, c.TOK_STAR)) parseHopRange(p, &out.min_hops, &out.max_hops);
    if (expect(p, c.TOK_RBRACKET) == null) return NOT_FOUND;
    return 0;
}

/// Parse relationship: -[:TYPE|TYPE2*min..max]-> or <-[...]-
fn parseRel(p: *Parser, out: *c.cbm_rel_pattern_t) c_int {
    out.* = .{ .min_hops = 1, .max_hops = 1 };

    // Check for leading < (inbound)
    const leading_lt = match(p, c.TOK_LT);
    if (expect(p, c.TOK_DASH) == null) return NOT_FOUND;

    // Optional bracket content
    if (match(p, c.TOK_LBRACKET)) {
        if (parseRelBracket(p, out) < 0) return NOT_FOUND;
    }

    if (expect(p, c.TOK_DASH) == null) return NOT_FOUND;

    // Check for trailing > (outbound)
    const trailing_gt = match(p, c.TOK_GT);

    out.direction = heapStrdup(if (leading_lt and !trailing_gt)
        "inbound"
    else if (!leading_lt and trailing_gt)
        "outbound"
    else
        "any");
    return 0;
}

// ── Expression tree helpers ────────────────────────────────────────

fn funcArgsFree(args: [*c]c.cbm_func_arg_t, count: c_int) void {
    var i: c_int = 0;
    while (i < count) : (i += 1) {
        strFree(&(args + ix(i)).*.variable);
        strFree(&(args + ix(i)).*.property);
        strFree(&(args + ix(i)).*.literal);
    }
    c.free(@ptrCast(args));
}

/// Free the heap fields of one condition.
fn condFree(cond: *Cond) void {
    strFree(&cond.variable);
    strFree(&cond.property);
    strFree(&cond.op);
    strFree(&cond.value);
    strFree(&cond.value_variable);
    strFree(&cond.value_property);
    var i: c_int = 0;
    while (i < cond.in_value_count) : (i += 1) strFree((cond.in_values + ix(i)));
    c.free(@ptrCast(cond.in_values));
    strFree(&cond.func);
}

pub fn exprFree(e: [*c]Expr) void {
    const STACK = 128;
    var stack: [STACK][*c]Expr = undefined;
    var top: usize = 0;
    if (e != null) {
        stack[top] = e;
        top += 1;
    }
    while (top > 0) {
        top -= 1;
        const cur = stack[top];
        if (cur.*.type == c.EXPR_CONDITION) {
            condFree(&cur.*.cond);
            var i: c_int = 0;
            while (i < cur.*.cond.arg_count) : (i += 1) {
                strFree(&(cur.*.cond.args + ix(i)).*.variable);
                strFree(&(cur.*.cond.args + ix(i)).*.property);
                strFree(&(cur.*.cond.args + ix(i)).*.literal);
            }
            c.free(@ptrCast(cur.*.cond.args));
        }
        if (cur.*.right != null) {
            if (top < STACK) {
                stack[top] = cur.*.right;
                top += 1;
            } else {
                exprFree(cur.*.right); // recurse when the stack overflows
            }
        }
        if (cur.*.left != null) {
            if (top < STACK) {
                stack[top] = cur.*.left;
                top += 1;
            } else {
                exprFree(cur.*.left); // recurse when the stack overflows
            }
        }
        c.free(@ptrCast(cur));
    }
}

fn exprLeaf(cond: Cond) [*c]Expr {
    const e = callocN(Expr, 1);
    e.*.type = c.EXPR_CONDITION;
    e.*.cond = cond;
    return e;
}

fn exprBinary(ty: c.cbm_expr_type_t, left: [*c]Expr, right: [*c]Expr) [*c]Expr {
    const e = callocN(Expr, 1);
    e.*.type = ty;
    e.*.left = left;
    e.*.right = right;
    return e;
}

fn exprNot(child: [*c]Expr) [*c]Expr {
    const e = callocN(Expr, 1);
    e.*.type = c.EXPR_NOT;
    e.*.left = child;
    return e;
}

// ── Unsupported keyword detection ─────────────────────────────────

fn unsupportedClauseError(ty: Tok) ?[]const u8 {
    return switch (ty) {
        c.TOK_CREATE => "unsupported Cypher feature: CREATE clause (write operations not supported)",
        c.TOK_DELETE => "unsupported Cypher feature: DELETE clause (write operations not supported)",
        c.TOK_DETACH => "unsupported Cypher feature: DETACH DELETE (write operations not supported)",
        c.TOK_SET => "unsupported Cypher feature: SET clause (write operations not supported)",
        c.TOK_REMOVE => "unsupported Cypher feature: REMOVE clause (write operations not supported)",
        c.TOK_MERGE => "unsupported Cypher feature: MERGE clause (write operations not supported)",
        c.TOK_YIELD => "unsupported Cypher feature: YIELD clause",
        c.TOK_CALL => "unsupported Cypher feature: CALL clause (stored procedures not supported)",
        c.TOK_FOREACH => "unsupported Cypher feature: FOREACH clause",
        c.TOK_MANDATORY => "unsupported Cypher feature: MANDATORY MATCH",
        c.TOK_DROP => "unsupported Cypher feature: DROP (schema operations not supported)",
        c.TOK_CONSTRAINT => "unsupported Cypher feature: CONSTRAINT (schema operations not supported)",
        else => null,
    };
}

// ── Recursive descent WHERE parser ────────────────────────────────

/// Free the fields of a partially-parsed multi-arg function item.
fn funcItemFieldsFree(item: *ReturnItem) void {
    strFree(&item.variable);
    strFree(&item.property);
    strFree(&item.func);
    funcArgsFree(item.args, item.arg_count);
    item.args = null;
    item.arg_count = 0;
}

/// Free the function-call fields of a WHERE condition (#874).
fn condFuncFieldsFree(cond: *Cond) void {
    strFree(&cond.func);
    funcArgsFree(cond.args, cond.arg_count);
    cond.args = null;
    cond.arg_count = 0;
}

/// Free the variable, property and op of a condition whose parse failed.
fn condLhsOpFree(cond: *Cond) void {
    strFree(&cond.variable);
    strFree(&cond.property);
    strFree(&cond.op);
}

/// Parse IN [val, val, ...]. Returns a leaf, or null on error.
fn parseInList(p: *Parser, cond: *Cond) [*c]Expr {
    _ = advance(p);
    cond.op = heapStrdup("IN");
    if (cond.op == null) {
        strFree(&cond.variable);
        strFree(&cond.property);
        return null;
    }
    if (expect(p, c.TOK_LBRACKET) == null) {
        condLhsOpFree(cond);
        return null;
    }
    var vcap: c_int = INIT_CAP8;
    var vn: c_int = 0;
    var vals = mallocN(CStr, ix(vcap));
    if (vals == null) {
        condLhsOpFree(cond);
        return null;
    }
    while (!check(p, c.TOK_RBRACKET) and !check(p, c.TOK_EOF)) {
        if (vn > 0) _ = match(p, c.TOK_COMMA);
        if (!check(p, c.TOK_STRING) and !check(p, c.TOK_NUMBER)) break;
        if (vn >= vcap) {
            const new_vcap = vcap * 2;
            const tmp = reallocN(CStr, vals, ix(new_vcap));
            if (tmp == null) {
                strListFree(vals, vn);
                condLhsOpFree(cond);
                return null;
            }
            vals = tmp;
            vcap = new_vcap;
        }
        const new_val = heapStrdup(advance(p).text);
        if (new_val == null) {
            strListFree(vals, vn);
            condLhsOpFree(cond);
            return null;
        }
        vals[ix(vn)] = new_val;
        vn += 1;
    }
    _ = expect(p, c.TOK_RBRACKET);
    cond.in_values = vals;
    cond.in_value_count = vn;
    return exprLeaf(cond.*);
}

/// Parse a comparison operator. Returns a heap-allocated op string or null.
fn parseComparisonOp(p: *Parser) [*c]u8 {
    if (match(p, c.TOK_EQ)) return heapStrdup("=");
    if (match(p, c.TOK_NEQ)) return heapStrdup("<>");
    if (match(p, c.TOK_EQTILDE)) return heapStrdup("=~");
    if (match(p, c.TOK_GTE)) return heapStrdup(">=");
    if (match(p, c.TOK_LTE)) return heapStrdup("<=");
    if (match(p, c.TOK_GT)) return heapStrdup(">");
    if (match(p, c.TOK_LT)) return heapStrdup("<");
    if (check(p, c.TOK_CONTAINS)) {
        _ = advance(p);
        return heapStrdup("CONTAINS");
    }
    if (check(p, c.TOK_STARTS)) {
        _ = advance(p);
        _ = expect(p, c.TOK_WITH);
        return heapStrdup("STARTS WITH");
    }
    if (check(p, c.TOK_ENDS)) {
        _ = advance(p);
        _ = expect(p, c.TOK_WITH);
        return heapStrdup("ENDS WITH");
    }
    return null;
}

/// Free the heap fields of a standalone node pattern (not owned by a pattern).
fn freeOneNodePattern(n: *c.cbm_node_pattern_t) void {
    strFree(&n.variable);
    strFree(&n.label);
    var j: c_int = 0;
    while (j < n.prop_count) : (j += 1) {
        strFree(&(n.props + ix(j)).*.key);
        strFree(&(n.props + ix(j)).*.value);
    }
    c.free(@ptrCast(n.props));
    n.* = .{};
}

/// Free the heap fields of a standalone relationship pattern.
fn freeOneRelPattern(r: *c.cbm_rel_pattern_t) void {
    strFree(&r.variable);
    var j: c_int = 0;
    while (j < r.type_count) : (j += 1) strFree((r.types + ix(j)));
    c.free(@ptrCast(r.types));
    strFree(&r.direction);
    r.* = .{};
}

/// Parse a bounded EXISTS predicate: EXISTS { (anchor)-[:TYPE]->() } — a
/// single-hop, edge-type-specific existence check anchored on a bound variable
/// (e.g. WHERE NOT EXISTS { (f)<-[:CALLS]-() } finds functions with no callers).
/// Multi-hop / nested-WHERE EXISTS is intentionally unsupported.
fn parseExistsPredicate(p: *Parser, negated: bool) [*c]Expr {
    _ = advance(p); // EXISTS
    if (!match(p, c.TOK_LBRACE)) {
        p.setError("expected '{{' after EXISTS at pos {d}", .{peek(p).pos});
        return null;
    }
    var anchor: c.cbm_node_pattern_t = .{};
    var rel: c.cbm_rel_pattern_t = .{};
    var far: c.cbm_node_pattern_t = .{};
    if (parseNode(p, &anchor) < 0 or parseRel(p, &rel) < 0 or parseNode(p, &far) < 0) {
        freeOneNodePattern(&anchor);
        freeOneRelPattern(&rel);
        freeOneNodePattern(&far);
        p.setError("unsupported EXISTS pattern — only the single-hop form " ++
            "'(var)-[:TYPE]->()' is supported", .{});
        return null;
    }
    _ = expect(p, c.TOK_RBRACE);

    var cond: Cond = .{};
    cond.negated = negated;
    cond.op = heapStrdup("EXISTS");
    cond.variable = heapStrdup(if (anchor.variable != null) anchor.variable else "");
    cond.value = if (rel.type_count > 0 and rel.types[0] != null) heapStrdup(rel.types[0]) else null;
    cond.exists_dir = if (strEql(rel.direction, "inbound")) 1 else if (strEql(rel.direction, "any")) 2 else 0;
    freeOneNodePattern(&anchor);
    freeOneRelPattern(&rel);
    freeOneNodePattern(&far);
    return exprLeaf(cond);
}

/// Parse the operator + value tail shared by every condition subject
/// (var[.prop] and multi-arg functions like coalesce(...)): IS [NOT] NULL,
/// IN [...], or a comparison operator with a literal value.
fn parseConditionOp(p: *Parser, cond: *Cond) [*c]Expr {
    // IS NULL / IS NOT NULL
    if (check(p, c.TOK_IS)) {
        _ = advance(p);
        if (match(p, c.TOK_NOT)) {
            cond.op = heapStrdup("IS NOT NULL");
            _ = expect(p, c.TOK_NULL_KW);
        } else {
            _ = expect(p, c.TOK_NULL_KW);
            cond.op = heapStrdup("IS NULL");
        }
        return exprLeaf(cond.*);
    }

    // IN [...]
    if (check(p, c.TOK_IN)) {
        const e = parseInList(p, cond);
        if (e == null) condFuncFieldsFree(cond);
        return e;
    }

    // Standard operators
    cond.op = parseComparisonOp(p);
    if (cond.op == null) {
        p.setError("unexpected operator at pos {d}", .{peek(p).pos});
        condFuncFieldsFree(cond);
        strFree(&cond.variable);
        strFree(&cond.property);
        return null;
    }

    // Value
    if (check(p, c.TOK_STRING) or check(p, c.TOK_NUMBER)) {
        cond.value = heapStrdup(advance(p).text);
    } else if (check(p, c.TOK_TRUE)) {
        _ = advance(p);
        cond.value = heapStrdup("true");
    } else if (check(p, c.TOK_FALSE)) {
        _ = advance(p);
        cond.value = heapStrdup("false");
    } else if (check(p, c.TOK_IDENT)) {
        // Another bound variable or property: a.file_path <> b.file_path
        cond.value_variable = heapStrdup(advance(p).text);
        if (match(p, c.TOK_DOT)) {
            const prop = expect(p, c.TOK_IDENT) orelse {
                condFuncFieldsFree(cond);
                condLhsOpFree(cond);
                strFree(&cond.value_variable);
                return null;
            };
            cond.value_property = heapStrdup(prop.text);
        }
    } else {
        p.setError("expected value at pos {d}", .{peek(p).pos});
        condFuncFieldsFree(cond);
        condLhsOpFree(cond);
        return null;
    }

    return exprLeaf(cond.*);
}

/// parseConditionLhs result: the label-test form is a complete condition.
const COND_LHS_COMPLETE = 1;

/// Parse the left-hand side of a WHERE condition into cond.
/// Returns NOT_FOUND on error, 0 when an operator/value should follow, and
/// COND_LHS_COMPLETE when the condition is already complete (label test).
fn parseConditionLhs(p: *Parser, cond: *Cond) c_int {
    if (isMultiargFuncCall(p)) {
        // Multi-arg scalar function LHS: coalesce(f.depth, 0) >= 2 (#874).
        // Reuse the RETURN-item parser, then move ownership into the condition.
        var fitem: ReturnItem = .{};
        if (parseMultiargFuncItem(p, &fitem) < 0) {
            funcItemFieldsFree(&fitem);
            return NOT_FOUND;
        }
        cond.variable = fitem.variable;
        cond.property = fitem.property;
        cond.func = fitem.func;
        cond.args = fitem.args;
        cond.arg_count = fitem.arg_count;
        return 0;
    }

    if (check(p, c.TOK_IDENT) and nextType(p) == c.TOK_LPAREN) {
        // Unrecognised function call in WHERE — fail loudly with the supported
        // set instead of the misleading "unexpected operator" (#874).
        p.setError("unsupported function '{s}' in WHERE (supported: coalesce, substring, replace, " ++
            "left, right)", .{sp(peek(p).text)});
        return NOT_FOUND;
    }

    const v = expect(p, c.TOK_IDENT) orelse return NOT_FOUND;

    // Label test: WHERE n:Label (openCypher, #241). Modelled as a leaf with
    // op="HAS_LABEL" and value=Label, evaluated against the bound node's label.
    if (check(p, c.TOK_COLON)) {
        _ = advance(p);
        const lbl = expect(p, c.TOK_IDENT) orelse return NOT_FOUND;
        cond.variable = heapStrdup(v.text);
        cond.op = heapStrdup("HAS_LABEL");
        cond.value = heapStrdup(lbl.text);
        return COND_LHS_COMPLETE;
    }

    if (match(p, c.TOK_DOT)) {
        const prop = expect(p, c.TOK_IDENT) orelse return NOT_FOUND;
        cond.variable = heapStrdup(v.text);
        cond.property = heapStrdup(prop.text);
    } else {
        // No dot: bare alias (e.g. post-WITH variable like "cnt")
        cond.variable = heapStrdup(v.text);
        cond.property = null;
    }
    return 0;
}

fn parseConditionExpr(p: *Parser) [*c]Expr {
    // NOT prefix at condition level (e.g. NOT n.name CONTAINS "x")
    const negated = match(p, c.TOK_NOT);

    // EXISTS { pattern } predicate (anchored single-hop existence).
    if (check(p, c.TOK_EXISTS)) return parseExistsPredicate(p, negated);

    var cond: Cond = .{};
    cond.negated = negated;

    const lhs_rc = parseConditionLhs(p, &cond);
    if (lhs_rc < 0) return null;
    // HAS_LABEL leaf — complete condition, no operator follows
    if (lhs_rc > 0) return exprLeaf(cond);

    return parseConditionOp(p, &cond);
}

/// Atom: ( expr ) | condition
fn parseAtomExpr(p: *Parser) [*c]Expr {
    if (match(p, c.TOK_LPAREN)) {
        if (!depthEnter(p)) return null;
        const e = parseOrExpr(p);
        depthLeave(p);
        _ = expect(p, c.TOK_RPAREN);
        return e;
    }
    return parseConditionExpr(p);
}

/// NOT: NOT atom | atom
fn parseNotExpr(p: *Parser) [*c]Expr {
    if (match(p, c.TOK_NOT)) {
        if (!depthEnter(p)) return null;
        const child = parseNotExpr(p);
        depthLeave(p);
        return if (child != null) exprNot(child) else null;
    }
    return parseAtomExpr(p);
}

/// A chain `a OP b OP c ...` of one associative operator is linked along the
/// RIGHT child: OP(a, OP(b, OP(c, ...))). The chain's length is bounded only by
/// the message size, so a left-deep fold would hand every walker (evaluate,
/// seed-selectivity, free) a tree as deep as the chain is long; linked this
/// way each walker follows the chain in a loop and only recurses into the
/// operands, whose nesting the parse-depth cap bounds. Operand order and the
/// left-to-right short-circuit are the same as in the nested form. `next` is
/// the operand just parsed; `more` says whether another OP follows it.
fn exprChainLink(tail: *[*c]Expr, ty: c.cbm_expr_type_t, next: [*c]Expr, more: bool) void {
    if (more) {
        const node = exprBinary(ty, next, null);
        tail.*.*.right = node;
        tail.* = node;
    } else {
        tail.*.*.right = next;
    }
}

/// One level of the AND / XOR / OR chains: operand (OP operand)*.
fn parseChain(p: *Parser, comptime op: Tok, comptime ty: c.cbm_expr_type_t, comptime operand: fn (*Parser) [*c]Expr) [*c]Expr {
    const first = operand(p);
    if (first == null or !check(p, op)) return first;
    const head = exprBinary(ty, first, null);
    var tail = head;
    while (match(p, op)) {
        const next = operand(p);
        if (next == null) {
            exprFree(head);
            return null;
        }
        exprChainLink(&tail, ty, next, check(p, op));
    }
    return head;
}

/// AND: not (AND not)*
fn parseAndExpr(p: *Parser) [*c]Expr {
    return parseChain(p, c.TOK_AND, c.EXPR_AND, parseNotExpr);
}

/// XOR: and (XOR and)*
fn parseXorExpr(p: *Parser) [*c]Expr {
    return parseChain(p, c.TOK_XOR, c.EXPR_XOR, parseAndExpr);
}

/// OR: xor (OR xor)*
fn parseOrExpr(p: *Parser) [*c]Expr {
    return parseChain(p, c.TOK_OR, c.EXPR_OR, parseXorExpr);
}

/// Parse WHERE clause — builds the expression tree.
fn parseWhere(p: *Parser, out: *[*c]c.cbm_where_clause_t) c_int {
    if (!match(p, c.TOK_WHERE)) {
        out.* = null;
        return 0;
    }

    const w = callocN(c.cbm_where_clause_t, 1);
    w.*.root = parseOrExpr(p);
    if (w.*.root == null and p.hasError()) {
        c.free(@ptrCast(w));
        return NOT_FOUND;
    }

    out.* = w;
    return 0;
}

fn isAggregateTok(t: Tok) bool {
    return t == c.TOK_COUNT or t == c.TOK_SUM or t == c.TOK_AVG or t == c.TOK_MIN_KW or
        t == c.TOK_MAX_KW or t == c.TOK_COLLECT;
}

fn isStringFuncTok(t: Tok) bool {
    return t == c.TOK_TOLOWER or t == c.TOK_TOUPPER or t == c.TOK_TOSTRING;
}

fn aggFuncName(t: Tok) [*:0]const u8 {
    return switch (t) {
        c.TOK_COUNT => "COUNT",
        c.TOK_SUM => "SUM",
        c.TOK_AVG => "AVG",
        c.TOK_MIN_KW => "MIN",
        c.TOK_MAX_KW => "MAX",
        c.TOK_COLLECT => "COLLECT",
        else => "COUNT",
    };
}

fn strFuncName(t: Tok) [*:0]const u8 {
    return switch (t) {
        c.TOK_TOLOWER => "toLower",
        c.TOK_TOUPPER => "toUpper",
        c.TOK_TOSTRING => "toString",
        else => "",
    };
}

/// Parse a value literal: string, number, ident[.prop], true, false.
/// Returns a heap-allocated string, or null.
fn parseValueLiteral(p: *Parser) [*c]u8 {
    if (check(p, c.TOK_STRING) or check(p, c.TOK_NUMBER)) return heapStrdup(advance(p).text);
    if (check(p, c.TOK_IDENT)) {
        var buf: [256]u8 = undefined;
        const v = advance(p);
        if (match(p, c.TOK_DOT)) {
            const pr = expect(p, c.TOK_IDENT);
            _ = bprint(&buf, "{s}.{s}", .{ sp(v.text), if (pr) |t| sp(t.text) else "" });
        } else {
            _ = bprint(&buf, "{s}", .{sp(v.text)});
        }
        return heapStrdup(&buf);
    }
    if (check(p, c.TOK_TRUE)) {
        _ = advance(p);
        return heapStrdup("true");
    }
    if (check(p, c.TOK_FALSE)) {
        _ = advance(p);
        return heapStrdup("false");
    }
    return null;
}

/// Parse CASE WHEN ... THEN ... [ELSE ...] END (CASE already consumed).
fn parseCaseExpr(p: *Parser) [*c]c.cbm_case_expr_t {
    const kase = callocN(c.cbm_case_expr_t, 1);
    if (kase == null) return null;
    var bcap: c_int = INIT_CAP4;
    kase.*.branches = mallocN(c.cbm_case_branch_t, ix(bcap));
    if (kase.*.branches == null) {
        c.free(@ptrCast(kase));
        return null;
    }

    while (check(p, c.TOK_WHEN)) {
        _ = advance(p);
        const when = parseOrExpr(p);
        if (expect(p, c.TOK_THEN) == null) {
            exprFree(when);
            break;
        }
        var then_val: CStr = parseValueLiteral(p);
        if (kase.*.branch_count >= bcap) {
            const new_bcap = bcap * 2;
            const tmp = reallocN(c.cbm_case_branch_t, kase.*.branches, ix(new_bcap));
            if (tmp == null) {
                exprFree(when);
                strFree(&then_val);
                var i: c_int = 0;
                while (i < kase.*.branch_count) : (i += 1) {
                    exprFree(kase.*.branches[ix(i)].when_expr);
                    strFree(&(kase.*.branches + ix(i)).*.then_val);
                }
                c.free(@ptrCast(kase.*.branches));
                c.free(@ptrCast(kase));
                return null;
            }
            kase.*.branches = tmp;
            bcap = new_bcap;
        }
        kase.*.branches[ix(kase.*.branch_count)] = .{ .when_expr = when, .then_val = then_val };
        kase.*.branch_count += 1;
    }

    if (match(p, c.TOK_ELSE)) kase.*.else_val = parseValueLiteral(p);
    _ = expect(p, c.TOK_END);
    return kase;
}

/// Canonical name for a single-argument scalar / entity-introspection function
/// invoked by identifier — labels/type/id/keys/properties and the numeric/bool
/// casts toInteger/toFloat/toBoolean — or null if unrecognised
/// (case-insensitive). toLower/toUpper/toString are separate keyword tokens
/// handled elsewhere.
fn scalarFuncCanonical(s: CStr) ?[*:0]const u8 {
    const names = [_][*:0]const u8{
        "labels", "type",   "id",   "keys",  "properties", "toInteger", "toFloat", "toBoolean",
        "size",   "length", "trim", "ltrim", "rtrim",      "reverse",
    };
    for (names) |name| {
        if (ciEq(s, name)) return name;
    }
    return null;
}

/// True for single-argument functions that transform a scalar string value
/// (vs. entity-introspection funcs that act on the bound node/edge).
pub fn isScalarValueFunc(f: CStr) bool {
    const names = [_][]const u8{
        "toLower", "toUpper", "toString", "toInteger", "toFloat", "toBoolean",
        "size",    "length",  "trim",     "ltrim",     "rtrim",   "reverse",
    };
    if (f == null) return false;
    for (names) |name| {
        if (strEql(f, name)) return true;
    }
    return false;
}

/// Parse var[.prop] into item.variable and item.property. Returns -1 on error.
fn parseVarDotProp(p: *Parser, item: *ReturnItem) c_int {
    const v = expect(p, c.TOK_IDENT) orelse return NOT_FOUND;
    item.variable = heapStrdup(v.text);
    if (match(p, c.TOK_DOT)) {
        if (expect(p, c.TOK_IDENT)) |prop| item.property = heapStrdup(prop.text);
    }
    return 0;
}

/// True if the cursor is at `IDENT(` where IDENT is a supported scalar function.
fn isNamedFuncCall(p: *Parser) bool {
    if (!check(p, c.TOK_IDENT) or nextType(p) != c.TOK_LPAREN) return false;
    return scalarFuncCanonical(peek(p).text) != null;
}

/// Parse a single-argument scalar / introspection call: labels(n), type(r),
/// id(n), keys(n), properties(n), toInteger(n.start_line), ...
fn parseNamedFuncItem(p: *Parser, item: *ReturnItem) c_int {
    const canon = scalarFuncCanonical(peek(p).text);
    _ = advance(p); // the function name
    _ = expect(p, c.TOK_LPAREN);
    if (parseVarDotProp(p, item) < 0) return NOT_FOUND;
    _ = expect(p, c.TOK_RPAREN);
    item.func = heapStrdup(canon);
    return 0;
}

/// Canonical name for a multi-argument scalar function, or null.
fn multiargFuncCanonical(s: CStr) ?[*:0]const u8 {
    const names = [_][*:0]const u8{ "coalesce", "substring", "replace", "left", "right" };
    for (names) |name| {
        if (ciEq(s, name)) return name;
    }
    return null;
}

fn isMultiargFuncCall(p: *Parser) bool {
    if (!check(p, c.TOK_IDENT) or nextType(p) != c.TOK_LPAREN) return false;
    return multiargFuncCanonical(peek(p).text) != null;
}

/// Parse one function argument: a string/number literal or a var[.prop].
fn parseFuncArg(p: *Parser, arg: *c.cbm_func_arg_t) c_int {
    arg.* = .{};
    if (check(p, c.TOK_STRING) or check(p, c.TOK_NUMBER)) {
        arg.literal = heapStrdup(peek(p).text);
        _ = advance(p);
        return 0;
    }
    const v = expect(p, c.TOK_IDENT) orelse return NOT_FOUND;
    arg.variable = heapStrdup(v.text);
    if (match(p, c.TOK_DOT)) {
        if (expect(p, c.TOK_IDENT)) |prop| arg.property = heapStrdup(prop.text);
    }
    return 0;
}

/// Parse a multi-argument scalar call: coalesce(a, b, ...), substring(s, i[, n]),
/// replace(s, from, to), left(s, n), right(s, n).
fn parseMultiargFuncItem(p: *Parser, item: *ReturnItem) c_int {
    const canon = multiargFuncCanonical(peek(p).text);
    _ = advance(p); // function name
    _ = expect(p, c.TOK_LPAREN);
    var cap: c_int = INIT_CAP4;
    item.args = mallocN(c.cbm_func_arg_t, ix(cap));
    item.arg_count = 0;
    while (!check(p, c.TOK_RPAREN) and !check(p, c.TOK_EOF)) {
        if (item.arg_count > 0 and !match(p, c.TOK_COMMA)) break;
        if (item.arg_count >= cap) {
            cap *= 2;
            item.args = safeReallocN(c.cbm_func_arg_t, item.args, ix(cap));
        }
        if (parseFuncArg(p, (item.args + ix(item.arg_count))) < 0) return NOT_FOUND;
        item.arg_count += 1;
    }
    _ = expect(p, c.TOK_RPAREN);
    item.func = heapStrdup(canon);
    // Surface the first variable arg as variable/property for column naming.
    if (item.arg_count > 0 and item.args[0].variable != null) {
        item.variable = heapStrdup(item.args[0].variable);
        if (item.args[0].property != null) item.property = heapStrdup(item.args[0].property);
    }
    return 0;
}

/// Parse aggregate function call: COUNT(var.prop)
fn parseAggregateItem(p: *Parser, item: *ReturnItem) c_int {
    const ft = peek(p).type;
    _ = advance(p);
    _ = expect(p, c.TOK_LPAREN);
    // Optional DISTINCT inside the call: COUNT(DISTINCT x) (#239).
    item.distinct = match(p, c.TOK_DISTINCT);
    if (match(p, c.TOK_STAR)) {
        item.variable = heapStrdup("*");
    } else if (parseVarDotProp(p, item) < 0) {
        return NOT_FOUND;
    }
    _ = expect(p, c.TOK_RPAREN);
    item.func = heapStrdup(aggFuncName(ft));
    return 0;
}

/// Parse string function call: toLower(var.prop)
fn parseStringFuncItem(p: *Parser, item: *ReturnItem) c_int {
    const ft = peek(p).type;
    _ = advance(p);
    _ = expect(p, c.TOK_LPAREN);
    if (parseVarDotProp(p, item) < 0) return NOT_FOUND;
    _ = expect(p, c.TOK_RPAREN);
    item.func = heapStrdup(strFuncName(ft));
    return 0;
}

/// Parse a single RETURN/WITH item (aggregate, string func, CASE, or plain
/// var.prop). Returns 0 on success, -1 on error.
fn parseReturnItem(p: *Parser, item: *ReturnItem) c_int {
    item.* = .{};
    var rc: c_int = 0;
    if (check(p, c.TOK_CASE)) {
        _ = advance(p);
        item.kase = parseCaseExpr(p);
        item.variable = heapStrdup("CASE");
    } else if (isAggregateTok(peek(p).type)) {
        rc = parseAggregateItem(p, item);
    } else if (isStringFuncTok(peek(p).type)) {
        rc = parseStringFuncItem(p, item);
    } else if (isMultiargFuncCall(p)) {
        rc = parseMultiargFuncItem(p, item);
    } else if (isNamedFuncCall(p)) {
        rc = parseNamedFuncItem(p, item);
    } else {
        rc = parseVarDotProp(p, item);
    }
    if (rc < 0) return NOT_FOUND;
    // labels(n)[i], keys(n)[i]: one element of a list-valued function.
    if ((strEql(item.func, "labels") or strEql(item.func, "keys")) and match(p, c.TOK_LBRACKET)) {
        const negative = match(p, c.TOK_DASH);
        const num = expect(p, c.TOK_NUMBER);
        if (num == null or std.mem.indexOfScalar(u8, sp(num.?.text), '.') != null or
            expect(p, c.TOK_RBRACKET) == null)
        {
            p.setError("unsupported expression: only an integer index '[n]' may follow {s}()", .{sp(item.func)});
            return NOT_FOUND;
        }
        item.has_index = true;
        item.index = @as(c_int, if (negative) -1 else 1) *% c.atoi(num.?.text);
    }
    // A bare identifier followed by '(' is a function we don't recognise
    // (recognised aggregates / string funcs / scalar funcs are handled above),
    // and '[' begins list indexing/slicing we don't support. Rather than
    // silently projecting an empty column — which looks like a valid but blank
    // result and hides the real problem — fail loudly with a clear message so
    // the caller knows the query used an unsupported feature (#373).
    if (item.func == null and item.kase == null and
        (check(p, c.TOK_LPAREN) or check(p, c.TOK_LBRACKET)))
    {
        if (check(p, c.TOK_LPAREN)) {
            p.setError("unsupported function '{s}' (supported: count, sum, avg, min, max, collect, " ++
                "toLower, toUpper, toString, toInteger, toFloat, toBoolean, size, length, " ++
                "trim, ltrim, rtrim, reverse, labels, type, id, keys, properties)", .{if (item.variable != null) sp(item.variable) else "?"});
        } else {
            p.setError("unsupported expression: list indexing/slicing '[...]' is not supported", .{});
        }
        strFree(&item.variable);
        strFree(&item.property);
        return NOT_FOUND;
    }
    // Optional AS alias
    if (match(p, c.TOK_AS)) {
        if (expect(p, c.TOK_IDENT)) |alias| item.alias = heapStrdup(alias.text);
    }
    return 0;
}

/// Parse an aggregate call in ORDER BY into buf.
fn parseOrderByAgg(p: *Parser, buf: []u8) void {
    const func = aggFuncName(peek(p).type);
    _ = advance(p);
    _ = expect(p, c.TOK_LPAREN);
    if (match(p, c.TOK_STAR)) {
        _ = bprint(buf, "{s}(*)", .{func});
    } else {
        const v = expect(p, c.TOK_IDENT);
        _ = bprint(buf, "{s}({s})", .{ func, if (v) |t| sp(t.text) else "" });
    }
    _ = expect(p, c.TOK_RPAREN);
}

/// Parse var[.prop] for ORDER BY into buf.
fn parseOrderByVar(p: *Parser, buf: []u8) void {
    const v = expect(p, c.TOK_IDENT) orelse return;
    _ = bprint(buf, "{s}", .{sp(v.text)});
    if (match(p, c.TOK_DOT)) {
        if (expect(p, c.TOK_IDENT)) |prop| _ = bprint(buf, "{s}.{s}", .{ sp(v.text), sp(prop.text) });
    }
}

/// Parse one ORDER BY key expression into buf.
fn parseOrderByExpr(p: *Parser, buf: []u8) void {
    buf[0] = 0;
    if (isAggregateTok(peek(p).type)) {
        parseOrderByAgg(p, buf);
    } else {
        parseOrderByVar(p, buf);
    }
}

/// Parse the full comma-separated ORDER BY key list (upstream 0a0ef516/#1334).
/// Consuming only the first key left ", key2 ... LIMIT n" unparsed, which
/// silently dropped the LIMIT and flooded the caller with the whole result
/// set. Returns 0 on success, NOT_FOUND when the key list exceeds the
/// modeled maximum.
fn parseOrderByClause(p: *Parser, r: *ReturnClause) c_int {
    _ = expect(p, c.TOK_BY);
    while (true) {
        if (r.order_key_count >= ORDER_KEYS_MAX) return NOT_FOUND;
        var order_buf: [256]u8 = undefined;
        parseOrderByExpr(p, &order_buf);
        var desc = false;
        if (match(p, c.TOK_ASC)) {
            desc = false;
        } else if (match(p, c.TOK_DESC)) {
            desc = true;
        }
        r.order_keys[ix(r.order_key_count)] = heapStrdup(&order_buf);
        r.order_descs[ix(r.order_key_count)] = desc;
        r.order_key_count += 1;
        if (!match(p, c.TOK_COMMA)) break;
    }
    return 0;
}

/// Parse a RETURN or WITH clause (shared logic).
fn parseReturnOrWith(p: *Parser, out: *[*c]ReturnClause, is_with: bool) c_int {
    const tok: Tok = if (is_with) c.TOK_WITH else c.TOK_RETURN;
    if (!match(p, tok)) {
        out.* = null;
        return 0;
    }

    const r = callocN(ReturnClause, 1);
    // -1 = no LIMIT clause (return all). An explicit `LIMIT 0` parses to 0 below
    // and must return 0 rows — distinguishing the two requires a sentinel, since
    // calloc zeroes limit and `limit > 0` would treat LIMIT 0 as "no limit".
    r.*.limit = -1;
    var cap: c_int = INIT_CAP8;
    r.*.items = mallocN(ReturnItem, ix(cap));

    r.*.distinct = match(p, c.TOK_DISTINCT);

    // RETURN * skips straight to ORDER BY / SKIP / LIMIT.
    if (!is_with and match(p, c.TOK_STAR)) {
        r.*.star = true;
    } else {
        while (true) {
            if (r.*.count > 0 and !match(p, c.TOK_COMMA)) break;

            var item: ReturnItem = .{};
            if (parseReturnItem(p, &item) < 0) {
                freeReturnClause(r);
                return NOT_FOUND;
            }

            if (r.*.count >= cap) {
                cap *= 2;
                r.*.items = safeReallocN(ReturnItem, r.*.items, ix(cap));
            }
            r.*.items[ix(r.*.count)] = item;
            r.*.count += 1;
            if (!check(p, c.TOK_COMMA)) break;
        }

        // Projection is materialized per row into fixed-width stack arrays sized
        // at RETURN_MAX_ITEMS columns (execute_return_simple and its siblings).
        // Bound the parsed item count to that width so an over-wide RETURN is
        // rejected here instead of writing past those arrays downstream.
        //
        // WITH is bounded tighter, by MAX_VARS. Every item a WITH projects
        // becomes one variable of the binding that carries the rest of the
        // query, and binding_t holds exactly MAX_VARS variables. A wider WITH
        // used to parse, then lose every alias past the 16th in
        // with_add_vbinding_var and answer with silently blank columns. Refuse
        // it here, the same way an over-wide RETURN is refused, so the caller
        // sees an error instead of a short or empty result.
        if (r.*.count > @as(c_int, if (is_with) MAX_VARS else RETURN_MAX_ITEMS)) {
            freeReturnClause(r);
            return NOT_FOUND;
        }
    }

    // Optional ORDER BY
    if (match(p, c.TOK_ORDER)) {
        if (parseOrderByClause(p, r) < 0) {
            freeReturnClause(r);
            return NOT_FOUND;
        }
    }

    // Optional SKIP. A missing or non-numeric operand is a parse error:
    // expect() has already filled p.err, and dropping the clause instead
    // would leave skip at its "none" default while the query still reports
    // success (#58).
    if (match(p, c.TOK_SKIP)) {
        const num = expect(p, c.TOK_NUMBER) orelse {
            freeReturnClause(r);
            return NOT_FOUND;
        };
        r.*.skip = tokenInt(num);
    }

    // Optional LIMIT. Same rule: a dropped LIMIT would hand the caller the
    // whole result set for a bounded query.
    if (match(p, c.TOK_LIMIT)) {
        const num = expect(p, c.TOK_NUMBER) orelse {
            freeReturnClause(r);
            return NOT_FOUND;
        };
        r.*.limit = tokenInt(num);
    }

    out.* = r;
    return 0;
}

/// Parse a single MATCH pattern into pat. On failure pat is freed and zeroed:
/// the callers count a pattern only after it parsed, so cbm_query_free would
/// never release a partial one (upstream 37a3d329).
fn parseMatchPattern(p: *Parser, pat: *c.cbm_pattern_t) c_int {
    pat.* = .{};
    var node_cap: c_int = INIT_CAP4;
    var rel_cap: c_int = INIT_CAP4;
    pat.nodes = mallocN(c.cbm_node_pattern_t, ix(node_cap));
    pat.rels = callocN(c.cbm_rel_pattern_t, ix(rel_cap));

    // A failed parseNode / parseRel leaves its element zeroed except for what
    // it allocated, so the element is counted before the check and
    // failMatchPattern frees its partial state with the rest.
    pat.node_count = 1;
    if (parseNode(p, pat.nodes) < 0) return failMatchPattern(pat);

    while (check(p, c.TOK_DASH) or check(p, c.TOK_LT)) {
        if (pat.rel_count >= rel_cap) {
            rel_cap *= 2;
            pat.rels = safeReallocN(c.cbm_rel_pattern_t, pat.rels, ix(rel_cap));
        }
        pat.rel_count += 1;
        if (parseRel(p, (pat.rels + ix(pat.rel_count - 1))) < 0) return failMatchPattern(pat);

        if (pat.node_count >= node_cap) {
            node_cap *= 2;
            pat.nodes = safeReallocN(c.cbm_node_pattern_t, pat.nodes, ix(node_cap));
        }
        pat.node_count += 1;
        if (parseNode(p, (pat.nodes + ix(pat.node_count - 1))) < 0) return failMatchPattern(pat);
    }
    return 0;
}

fn failMatchPattern(pat: *c.cbm_pattern_t) c_int {
    freePattern(pat);
    pat.* = .{};
    return NOT_FOUND;
}

/// Parse an UNWIND [...] AS var clause into q.
fn parseUnwindClause(p: *Parser, q: *Query) void {
    _ = advance(p);
    if (check(p, c.TOK_LBRACKET)) {
        // Literal list: [1, 2, 3] — collected as a JSON array string
        _ = advance(p);
        var buf: [2048]u8 = undefined;
        buf[0] = '[';
        var blen: usize = 1;
        // snprintf returns the length it WOULD have written, so a single
        // oversized token can push blen past the buffer (upstream d0457f2b).
        // Clamp after every write and guard the raw stores.
        const cap = buf.len;
        while (!check(p, c.TOK_RBRACKET) and !check(p, c.TOK_EOF)) {
            if (blen > 1 and blen < cap - 1) {
                buf[blen] = ',';
                blen += 1;
            }
            if (check(p, c.TOK_STRING)) {
                blen += bprint(buf[blen..], "\"{s}\"", .{sp(peek(p).text)});
                _ = advance(p);
            } else if (check(p, c.TOK_NUMBER)) {
                blen += bprint(buf[blen..], "{s}", .{sp(peek(p).text)});
                _ = advance(p);
            } else {
                _ = advance(p);
            }
            if (blen >= cap) blen = cap - 1;
            _ = match(p, c.TOK_COMMA);
        }
        _ = expect(p, c.TOK_RBRACKET);
        if (blen < cap - 1) {
            buf[blen] = ']';
            blen += 1;
        }
        buf[blen] = 0;
        q.unwind_expr = heapStrdup(&buf);
    } else if (check(p, c.TOK_IDENT)) {
        q.unwind_expr = heapStrdup(advance(p).text);
    }
    _ = expect(p, c.TOK_AS);
    if (expect(p, c.TOK_IDENT)) |alias| q.unwind_alias = heapStrdup(alias.text);
}

/// Parse a chain of MATCH / OPTIONAL MATCH patterns into q.
/// Returns -1 on error (fills p.err).
fn parseMatchChain(p: *Parser, q: *Query, pat_cap: *c_int) c_int {
    while (check(p, c.TOK_MATCH) or check(p, c.TOK_OPTIONAL)) {
        var opt = false;
        if (check(p, c.TOK_OPTIONAL)) {
            _ = advance(p);
            opt = true;
        }
        if (expect(p, c.TOK_MATCH) == null) break;
        if (q.pattern_count >= pat_cap.*) {
            pat_cap.* *= 2;
            q.patterns = safeReallocN(c.cbm_pattern_t, q.patterns, ix(pat_cap.*));
            q.pattern_optional = safeReallocN(bool, q.pattern_optional, ix(pat_cap.*));
        }
        if (parseMatchPattern(p, (q.patterns + ix(q.pattern_count))) < 0) return NOT_FOUND;
        q.pattern_optional[ix(q.pattern_count)] = opt;
        q.pattern_count += 1;
    }
    return 0;
}

/// Parse post-WHERE clauses: additional MATCH, WITH, RETURN. A UNION keyword
/// is left for cbm_parse, which reads the branches in a loop.
fn parsePostWhere(p: *Parser, q: *Query, pat_cap: *c_int) c_int {
    // More MATCH / OPTIONAL MATCH after WHERE
    if (parseMatchChain(p, q, pat_cap) < 0) return NOT_FOUND;
    // Unsupported keywords
    if (unsupportedClauseError(peek(p).type)) |unsup| {
        p.setError("{s}", .{unsup});
        return NOT_FOUND;
    }
    // Optional WITH clause (standalone, not STARTS WITH)
    if (check(p, c.TOK_WITH) and (p.pos < 2 or p.tokens[ix(p.pos - 1)].type != c.TOK_STARTS)) {
        if (parseReturnOrWith(p, &q.with_clause, true) < 0) return NOT_FOUND;
        if (parseWhere(p, &q.post_with_where) < 0) return NOT_FOUND;
    }
    // Optional RETURN
    if (parseReturnOrWith(p, &q.ret, false) < 0) return NOT_FOUND;
    return 0;
}

/// A stage that failed without writing a message gets the stage's own.
fn parseErrorDefault(p: *Parser, msg: []const u8) void {
    if (!p.hasError()) p.setError("{s}", .{msg});
}

/// One MATCH ... RETURN block: the whole query, or one branch of a UNION. On
/// failure p.err holds the message and out stays null.
fn parseQueryBlock(p: *Parser, out: *[*c]Query) c_int {
    out.* = null;
    // Each block starts with a clean message, as it did when every UNION
    // branch had a parser of its own.
    p.err[0] = 0;

    // Unsupported leading keywords
    if (unsupportedClauseError(peek(p).type)) |unsup| {
        p.setError("{s}", .{unsup});
        return NOT_FOUND;
    }

    const q = callocN(Query, 1);

    if (check(p, c.TOK_UNWIND)) parseUnwindClause(p, q);

    var first_optional = false;
    if (check(p, c.TOK_OPTIONAL)) {
        _ = advance(p);
        first_optional = true;
    }
    if (expect(p, c.TOK_MATCH) == null) {
        parseErrorDefault(p, "expected MATCH");
        cbm_query_free(q);
        return NOT_FOUND;
    }

    var pat_cap: c_int = INIT_CAP4;
    q.*.patterns = mallocN(c.cbm_pattern_t, ix(pat_cap));
    q.*.pattern_optional = mallocN(bool, ix(pat_cap));

    if (parseMatchPattern(p, q.*.patterns) < 0) {
        parseErrorDefault(p, "failed to parse pattern");
        cbm_query_free(q);
        return NOT_FOUND;
    }
    q.*.pattern_optional[0] = first_optional;
    q.*.pattern_count = 1;

    if (parseMatchChain(p, q, &pat_cap) < 0) {
        parseErrorDefault(p, "failed to parse additional pattern");
        cbm_query_free(q);
        return NOT_FOUND;
    }

    if (parseWhere(p, &q.*.where) < 0) {
        parseErrorDefault(p, "failed to parse WHERE");
        cbm_query_free(q);
        return NOT_FOUND;
    }

    if (parsePostWhere(p, q, &pat_cap) < 0) {
        parseErrorDefault(p, "failed to parse query");
        cbm_query_free(q);
        return NOT_FOUND;
    }

    out.* = q;
    return 0;
}

pub export fn cbm_parse(tokens: [*c]const c.cbm_token_t, token_count: c_int, out_c: [*c]c.cbm_parse_result_t) c_int {
    const out: *c.cbm_parse_result_t = out_c;
    out.* = .{};
    var p: Parser = .{ .tokens = tokens, .count = token_count };

    // The branches of a UNION are read one after another by this one parser
    // and linked as they come, so the number of branches is a loop count, not
    // a recursion depth (#58).
    var q: [*c]Query = null;
    var tail: [*c]Query = null;
    while (true) {
        var block: [*c]Query = null;
        if (parseQueryBlock(&p, &block) < 0) {
            out.@"error" = heapStrdup(&p.err);
            cbm_query_free(q);
            return NOT_FOUND;
        }
        if (tail != null) {
            tail.*.union_next = block;
        } else {
            q = block;
        }
        tail = block;
        // UNION [ALL]: the ALL flag sits on the branch before the keyword.
        if (!check(&p, c.TOK_UNION)) break;
        _ = advance(&p);
        tail.*.union_all = match(&p, c.TOK_ALL);
    }

    // Every token must be consumed. The grammar accepts at most one WITH and
    // treats RETURN as optional, so a query with a second WITH stage — or any
    // typo after RETURN — used to stop parsing there and succeed anyway. The
    // dropped tail took the filter and the RETURN with it, and the engine
    // answered from the fragment it had parsed, using its default projection.
    // That reported success and returned wrong rows, which is worse than a
    // refusal because nothing tells the caller to look. Refuse instead.
    if (peek(&p).type != c.TOK_EOF) {
        // Only mention the one-WITH limit when a standalone WITH really is
        // sitting in the part we could not read. Saying it every time points
        // a reader at WITH when the problem is a typo. A WITH straight after
        // STARTS is the STARTS WITH operator rather than a clause, the same
        // guard parsePostWhere uses.
        var hint: []const u8 = "";
        var i = p.pos;
        while (i < p.count) : (i += 1) {
            if (p.tokens[ix(i)].type == c.TOK_WITH and
                (i == 0 or p.tokens[ix(i - 1)].type != c.TOK_STARTS))
            {
                hint = " Note that only one WITH clause is supported.";
                break;
            }
        }
        const text = peek(&p).text;
        p.setError("unexpected input at pos {d} ('{s}') — the query was not fully " ++
            "parsed.{s}", .{ peek(&p).pos, if (text != null) sp(text) else "", hint });
        out.@"error" = heapStrdup(&p.err);
        cbm_query_free(q);
        return NOT_FOUND;
    }

    out.query = q;
    return 0;
}

pub export fn cbm_parse_free(r_c: [*c]c.cbm_parse_result_t) void {
    if (r_c == null) return;
    const r: *c.cbm_parse_result_t = r_c;
    cbm_query_free(r.query);
    c.free(@ptrCast(r.@"error"));
    r.* = .{};
}

// ── Query free ─────────────────────────────────────────────────────

fn freePattern(pat: *c.cbm_pattern_t) void {
    var i: c_int = 0;
    while (i < pat.node_count) : (i += 1) {
        const n: *c.cbm_node_pattern_t = pat.nodes + ix(i);
        strFree(&n.variable);
        strFree(&n.label);
        var j: c_int = 0;
        while (j < n.prop_count) : (j += 1) {
            strFree(&(n.props + ix(j)).*.key);
            strFree(&(n.props + ix(j)).*.value);
        }
        c.free(@ptrCast(n.props));
    }
    c.free(@ptrCast(pat.nodes));
    i = 0;
    while (i < pat.rel_count) : (i += 1) {
        const r: *c.cbm_rel_pattern_t = pat.rels + ix(i);
        strFree(&r.variable);
        var j: c_int = 0;
        while (j < r.type_count) : (j += 1) strFree((r.types + ix(j)));
        c.free(@ptrCast(r.types));
        strFree(&r.direction);
    }
    c.free(@ptrCast(pat.rels));
}

fn freeWhere(w: [*c]c.cbm_where_clause_t) void {
    if (w == null) return;
    exprFree(w.*.root);
    var i: c_int = 0;
    while (i < w.*.count) : (i += 1) {
        const cond: *Cond = w.*.conditions + ix(i);
        condFree(cond);
        funcArgsFree(cond.args, cond.arg_count);
    }
    c.free(@ptrCast(w.*.conditions));
    strFree(&w.*.op);
    c.free(@ptrCast(w));
}

fn freeCaseExpr(k: [*c]c.cbm_case_expr_t) void {
    if (k == null) return;
    var i: c_int = 0;
    while (i < k.*.branch_count) : (i += 1) {
        exprFree(k.*.branches[ix(i)].when_expr);
        strFree(&(k.*.branches + ix(i)).*.then_val);
    }
    c.free(@ptrCast(k.*.branches));
    strFree(&k.*.else_val);
    c.free(@ptrCast(k));
}

fn freeReturnClause(r: [*c]ReturnClause) void {
    if (r == null) return;
    var i: c_int = 0;
    while (i < r.*.count) : (i += 1) {
        const item: *ReturnItem = r.*.items + ix(i);
        strFree(&item.variable);
        strFree(&item.property);
        strFree(&item.alias);
        strFree(&item.func);
        freeCaseExpr(item.kase);
        var j: c_int = 0;
        while (j < item.arg_count) : (j += 1) {
            strFree(&(item.args + ix(j)).*.variable);
            strFree(&(item.args + ix(j)).*.property);
            strFree(&(item.args + ix(j)).*.literal);
        }
        c.free(@ptrCast(item.args));
    }
    c.free(@ptrCast(r.*.items));
    var k: c_int = 0;
    while (k < r.*.order_key_count) : (k += 1) strFree(&r.*.order_keys[ix(k)]);
    c.free(@ptrCast(r));
}

pub export fn cbm_query_free(q_c: [*c]c.cbm_query_t) void {
    var q = q_c;
    while (q != null) {
        const next = q.*.union_next;
        var i: c_int = 0;
        while (i < q.*.pattern_count) : (i += 1) freePattern((q.*.patterns + ix(i)));
        c.free(@ptrCast(q.*.patterns));
        c.free(@ptrCast(q.*.pattern_optional));
        freeWhere(q.*.where);
        freeWhere(q.*.post_with_where);
        freeReturnClause(q.*.with_clause);
        freeReturnClause(q.*.ret);
        strFree(&q.*.unwind_expr);
        strFree(&q.*.unwind_alias);
        c.free(@ptrCast(q));
        q = next;
    }
}

// ── Convenience: lex + parse ───────────────────────────────────────

pub export fn cbm_cypher_parse(query: [*c]const u8, out: [*c][*c]c.cbm_query_t, err: [*c][*c]u8) c_int {
    out.* = null;
    err.* = null;

    var lr: c.cbm_lex_result_t = .{};
    if (cbm_lex(query, &lr) < 0 or lr.@"error" != null) {
        err.* = heapStrdup(if (lr.@"error" != null) lr.@"error" else "lex error");
        cbm_lex_free(&lr);
        return NOT_FOUND;
    }

    var pr: c.cbm_parse_result_t = .{};
    if (cbm_parse(lr.tokens, lr.count, &pr) < 0) {
        err.* = heapStrdup(if (pr.@"error" != null) pr.@"error" else "parse error");
        cbm_parse_free(&pr);
        cbm_lex_free(&lr);
        return NOT_FOUND;
    }

    out.* = pr.query;
    pr.query = null;
    cbm_parse_free(&pr);
    cbm_lex_free(&lr);
    return 0;
}
