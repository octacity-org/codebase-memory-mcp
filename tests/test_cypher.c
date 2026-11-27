/*
 * test_cypher.c — Tests for the Cypher query engine.
 *
 * Ported from internal/cypher/cypher_test.go (1016 LOC).
 * Covers lexer, parser, and end-to-end execution.
 */
#include "test_framework.h"
#include <cypher/cypher.h>
#include "foundation/compat_thread.h" /* cbm_thread_create: bounded query stack */
#include <store/store.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/wait.h> /* fork/waitpid crash-isolation for the projection-width guard */
#include <unistd.h>
#endif

/* ══════════════════════════════════════════════════════════════════
 *  LEXER TESTS
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_lex_simple_match) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("MATCH (n:Function)", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);

    /* MATCH ( n : Function ) EOF */
    ASSERT_GTE(r.count, 6);
    ASSERT_EQ(r.tokens[0].type, TOK_MATCH);
    ASSERT_EQ(r.tokens[1].type, TOK_LPAREN);
    ASSERT_EQ(r.tokens[2].type, TOK_IDENT);
    ASSERT_STR_EQ(r.tokens[2].text, "n");
    ASSERT_EQ(r.tokens[3].type, TOK_COLON);
    ASSERT_EQ(r.tokens[4].type, TOK_IDENT);
    ASSERT_STR_EQ(r.tokens[4].text, "Function");
    ASSERT_EQ(r.tokens[5].type, TOK_RPAREN);

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_relationship) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("-[:CALLS]->", &r);
    ASSERT_EQ(rc, 0);

    /* - [ : CALLS ] - > EOF */
    ASSERT_GTE(r.count, 7);
    ASSERT_EQ(r.tokens[0].type, TOK_DASH);
    ASSERT_EQ(r.tokens[1].type, TOK_LBRACKET);
    ASSERT_EQ(r.tokens[2].type, TOK_COLON);
    ASSERT_EQ(r.tokens[3].type, TOK_IDENT);
    ASSERT_STR_EQ(r.tokens[3].text, "CALLS");
    ASSERT_EQ(r.tokens[4].type, TOK_RBRACKET);
    ASSERT_EQ(r.tokens[5].type, TOK_DASH);
    ASSERT_EQ(r.tokens[6].type, TOK_GT);

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_string_literal) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("\"hello world\"", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 1);
    ASSERT_EQ(r.tokens[0].type, TOK_STRING);
    ASSERT_STR_EQ(r.tokens[0].text, "hello world");

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_single_quote_string) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("'hello'", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.tokens[0].type, TOK_STRING);
    ASSERT_STR_EQ(r.tokens[0].text, "hello");

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_string_overflow) {
    /* Build a string literal longer than 4096 bytes to verify we don't
     * overflow the stack buffer in lex_string_literal. */
    const int big = 5000;
    /* query: "AAAA...A"  (quotes included) */
    char *query = malloc(big + 3); /* quote + big chars + quote + NUL */
    ASSERT_NOT_NULL(query);
    query[0] = '"';
    memset(query + 1, 'A', big);
    query[big + 1] = '"';
    query[big + 2] = '\0';

    cbm_lex_result_t r = {0};
    int rc = cbm_lex(query, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_GTE(r.count, 1);
    ASSERT_EQ(r.tokens[0].type, TOK_STRING);
    /* The string should be truncated to CBM_SZ_4K - 1 (4095) characters. */
    ASSERT_EQ((int)strlen(r.tokens[0].text), 4095);

    cbm_lex_free(&r);
    free(query);
    PASS();
}

TEST(cypher_lex_number) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("42 3.14", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 2);
    ASSERT_EQ(r.tokens[0].type, TOK_NUMBER);
    ASSERT_STR_EQ(r.tokens[0].text, "42");
    ASSERT_EQ(r.tokens[1].type, TOK_NUMBER);
    ASSERT_STR_EQ(r.tokens[1].text, "3.14");

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_operators) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("= =~ >= <= ..", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 5);
    ASSERT_EQ(r.tokens[0].type, TOK_EQ);
    ASSERT_EQ(r.tokens[1].type, TOK_EQTILDE);
    ASSERT_EQ(r.tokens[2].type, TOK_GTE);
    ASSERT_EQ(r.tokens[3].type, TOK_LTE);
    ASSERT_EQ(r.tokens[4].type, TOK_DOTDOT);

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_keywords_case_insensitive) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("match WHERE Return limit", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.tokens[0].type, TOK_MATCH);
    ASSERT_EQ(r.tokens[1].type, TOK_WHERE);
    ASSERT_EQ(r.tokens[2].type, TOK_RETURN);
    ASSERT_EQ(r.tokens[3].type, TOK_LIMIT);

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_pipe_and_star) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("[:TYPE1|TYPE2*1..3]", &r);
    ASSERT_EQ(rc, 0);

    /* [ : TYPE1 | TYPE2 * 1 .. 3 ] */
    ASSERT_GTE(r.count, 9);
    ASSERT_EQ(r.tokens[3].type, TOK_PIPE);
    ASSERT_EQ(r.tokens[5].type, TOK_STAR);
    ASSERT_EQ(r.tokens[6].type, TOK_NUMBER);
    ASSERT_STR_EQ(r.tokens[6].text, "1");
    ASSERT_EQ(r.tokens[7].type, TOK_DOTDOT);
    ASSERT_EQ(r.tokens[8].type, TOK_NUMBER);
    ASSERT_STR_EQ(r.tokens[8].text, "3");

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_full_query) {
    const char *q = "MATCH (f:Function)-[:CALLS]->(g:Function) "
                    "WHERE f.name =~ \".*Order.*\" "
                    "RETURN f.name, g.name LIMIT 10";
    cbm_lex_result_t r = {0};
    int rc = cbm_lex(q, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    /* Should have many tokens; just check it doesn't crash */
    ASSERT_GT(r.count, 20);

    cbm_lex_free(&r);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PARSER TESTS
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_parse_simple_node) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(err);
    ASSERT_NOT_NULL(q);

    ASSERT_EQ(cbm_query_pattern(q).node_count, 1);
    ASSERT_EQ(cbm_query_pattern(q).rel_count, 0);
    ASSERT_STR_EQ(cbm_query_pattern(q).nodes[0].variable, "f");
    ASSERT_STR_EQ(cbm_query_pattern(q).nodes[0].label, "Function");

    cbm_query_free(q);
    PASS();
}

/* Trailing input must be an error, never a silent drop. The parser used to
 * stop at the first thing it did not understand and report success, so the
 * engine answered from the fragment it had parsed. */
TEST(cypher_parse_rejects_trailing_tokens) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name AS n BANANA SPLIT 99", &q, &err);
    ASSERT_NEQ(rc, 0);
    ASSERT_NOT_NULL(err);
    ASSERT_NULL(q);

    /* The message must name what actually stopped the parse, and must not
     * mention WITH: this query has no WITH in it anywhere. */
    ASSERT(strstr(err, "BANANA") != NULL);
    ASSERT(strstr(err, "WITH") == NULL);

    free(err);
    PASS();
}

/* Only one WITH is supported. A second one used to take the rest of the
 * query with it — the filter and the RETURN both vanished, and every row
 * came back unfiltered under the default projection. */
TEST(cypher_parse_rejects_second_with_clause) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) "
                              "OPTIONAL MATCH (a)-[:CALLS]->(f) "
                              "WITH f, count(a) AS calls "
                              "OPTIONAL MATCH (b)-[:USAGE]->(f) "
                              "WITH f, calls, count(b) AS usages "
                              "WHERE calls = 0 AND usages = 0 "
                              "RETURN f.name AS n",
                              &q, &err);
    ASSERT_NEQ(rc, 0);
    ASSERT_NOT_NULL(err);
    ASSERT_NULL(q);

    /* Here the note earns its place. The parse stops at OPTIONAL, and the
     * reason is the second WITH further along, which the reader cannot see
     * from the stopping point alone. */
    ASSERT(strstr(err, "only one WITH clause is supported") != NULL);

    free(err);
    PASS();
}

/* The guard must not reject a query that is simply finished. One WITH, a
 * WHERE after it and a RETURN is the shape the grammar does support. */
TEST(cypher_parse_accepts_single_with_clause) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) "
                              "WITH f, f.name AS n "
                              "WHERE n = 'buildTree' "
                              "RETURN n",
                              &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(err);
    ASSERT_NOT_NULL(q);

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_relationship_outbound) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function)-[:CALLS]->(g:Function)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_EQ(cbm_query_pattern(q).node_count, 2);
    ASSERT_EQ(cbm_query_pattern(q).rel_count, 1);
    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].types[0], "CALLS");
    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].direction, "outbound");
    ASSERT_EQ(cbm_query_pattern(q).rels[0].min_hops, 1);
    ASSERT_EQ(cbm_query_pattern(q).rels[0].max_hops, 1);

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_relationship_inbound) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function)<-[:CALLS]-(g:Function)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].direction, "inbound");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_relationship_any) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function)-[:CALLS]-(g:Function)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].direction, "any");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_variable_length) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function)-[:CALLS*1..3]->(g:Function)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_EQ(cbm_query_pattern(q).rels[0].min_hops, 1);
    ASSERT_EQ(cbm_query_pattern(q).rels[0].max_hops, 3);

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_variable_length_unbounded) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f)-[:CALLS*]->(g)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_EQ(cbm_query_pattern(q).rels[0].min_hops, 1);
    ASSERT_EQ(cbm_query_pattern(q).rels[0].max_hops, 0); /* 0 = unbounded */

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_multiple_edge_types) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f)-[:CALLS|HTTP_CALLS]->(g)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_EQ(cbm_query_pattern(q).rels[0].type_count, 2);
    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].types[0], "CALLS");
    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].types[1], "HTTP_CALLS");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_where_clause) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) WHERE f.name = \"Foo\"", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);
    ASSERT_NOT_NULL(q->where);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.variable, "f");
    ASSERT_STR_EQ(q->where->root->cond.property, "name");
    ASSERT_STR_EQ(q->where->root->cond.op, "=");
    ASSERT_STR_EQ(q->where->root->cond.value, "Foo");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_where_regex) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) WHERE f.name =~ \".*Order.*\"", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, "=~");
    ASSERT_STR_EQ(q->where->root->cond.value, ".*Order.*");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_where_and) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) WHERE f.name = \"A\" AND f.label = \"Function\"",
                              &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_AND);
    ASSERT_NOT_NULL(q->where->root->left);
    ASSERT_NOT_NULL(q->where->root->right);

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_return_simple) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name, f.qualified_name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->ret);
    ASSERT_EQ(q->ret->count, 2);
    ASSERT_STR_EQ(q->ret->items[0].variable, "f");
    ASSERT_STR_EQ(q->ret->items[0].property, "name");
    ASSERT_STR_EQ(q->ret->items[1].property, "qualified_name");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_return_count) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f)-[:CALLS]->(g) RETURN f.name, COUNT(g) AS cnt", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->count, 2);
    ASSERT_NOT_NULL(q->ret->items[1].func);
    ASSERT_STR_EQ(q->ret->items[1].func, "COUNT");
    ASSERT_STR_EQ(q->ret->items[1].alias, "cnt");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_return_order_limit) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f:Function) RETURN f.name ORDER BY f.name DESC LIMIT 5", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->order_key_count, 1);
    ASSERT_STR_EQ(q->ret->order_keys[0], "f.name");
    ASSERT(q->ret->order_descs[0]);
    ASSERT_EQ(q->ret->limit, 5);

    cbm_query_free(q);
    PASS();
}

/* Upstream 0a0ef516 (#1334): every ORDER BY key is parsed (per-key
 * direction) and the LIMIT that follows the key list is consumed instead of
 * silently dropped. */
TEST(cypher_parse_multikey_order_by_issue1334) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(
        "MATCH (f:Function) RETURN f.name ORDER BY f.complexity DESC, f.name ASC LIMIT 5", &q,
        &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->order_key_count, 2);
    ASSERT_STR_EQ(q->ret->order_keys[0], "f.complexity");
    ASSERT(q->ret->order_descs[0]);
    ASSERT_STR_EQ(q->ret->order_keys[1], "f.name");
    ASSERT_FALSE(q->ret->order_descs[1]);
    ASSERT_EQ(q->ret->limit, 5);

    cbm_query_free(q);
    PASS();
}

/* #1334: more keys than the modeled maximum is a loud parse error — the old
 * failure mode (ignore the remainder, drop the LIMIT) must never come back. */
TEST(cypher_parse_order_by_over_cap_rejected_issue1334) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name ORDER BY "
                              "f.a, f.b, f.c, f.d, f.e, f.f, f.g, f.h, f.i LIMIT 5",
                              &q, &err);
    ASSERT(rc != 0);
    free(err);
    PASS();
}

/* #58 (upstream PR 2002): a missing or non-numeric SKIP/LIMIT operand is a
 * parse error, not a dropped clause. The end-of-input check already rejected
 * an operand left unconsumed (`LIMIT $limit`, `LIMIT abc`); these shapes
 * parsed successfully with the clause silently dropped, so a bounded query ran
 * unbounded. */
TEST(cypher_parse_missing_skip_limit_operand_rejected) {
    static const char *const queries[] = {
        "MATCH (f:Function) RETURN f.name LIMIT $limit",
        "MATCH (f:Function) RETURN f.name SKIP $offset LIMIT 10",
        "MATCH (f:Function) RETURN f.name LIMIT abc",
        "MATCH (f:Function) RETURN f.name LIMIT",
        "MATCH (f:Function) RETURN f.name SKIP LIMIT 5",
        "MATCH (f:Function) WITH f LIMIT RETURN f.name",
        "MATCH (f:Function) RETURN f.name LIMIT UNION MATCH (g:Function) RETURN g.name",
    };
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        cbm_query_t *q = NULL;
        char *err = NULL;
        int rc = cbm_cypher_parse(queries[i], &q, &err);
        if (rc == 0) {
            printf("  accepted: %s\n", queries[i]);
            cbm_query_free(q);
        }
        ASSERT(rc != 0);
        ASSERT_NOT_NULL(err);
        free(err);
    }
    PASS();
}

/* Control: a well-formed SKIP/LIMIT still parses and still carries its values. */
TEST(cypher_parse_numeric_skip_limit_still_accepted) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name SKIP 2 LIMIT 10", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->skip, 2);
    ASSERT_EQ(q->ret->limit, 10);
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_return_distinct) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN DISTINCT f.label", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT(q->ret->distinct);

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_inline_props) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function {name: \"Foo\"})", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cbm_query_pattern(q).nodes[0].prop_count, 1);
    ASSERT_STR_EQ(cbm_query_pattern(q).nodes[0].props[0].key, "name");
    ASSERT_STR_EQ(cbm_query_pattern(q).nodes[0].props[0].value, "Foo");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_error) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("INVALID QUERY", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    free(err);
    PASS();
}

/* #150: a pattern that fails partway frees what it already parsed. Each query
 * fails at a different point; the first four leaked before the fix, and the
 * fifth reaches the same property-list failure through a second MATCH. The
 * leak itself only shows under a leak checker; without one this pins that
 * every failure path still returns a clean error. */
TEST(cypher_parse_error_frees_partial_pattern) {
    static const char *const queries[] = {
        /* named path: unsupported, fails at the first node */
        "MATCH p = (a:Class)-[:INHERITS*1..3]->(b:Class) RETURN a.name, length(p)",
        /* property list fails after one parsed entry */
        "MATCH (a:Function {name: \"x\", 7}) RETURN a",
        /* relationship cut off after its variable */
        "MATCH (a:Function)-[r:CALLS",
        /* second node's label missing, after a parsed relationship */
        "MATCH (a:Function)-[:CALLS]->(b:",
        /* a second MATCH clause's pattern fails inside its properties */
        "MATCH (a:Function) MATCH (b:Function {name: \"y\" 5}) RETURN a",
    };
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        cbm_query_t *q = NULL;
        char *err = NULL;
        int rc = cbm_cypher_parse(queries[i], &q, &err);
        ASSERT_EQ(rc, -1);
        ASSERT_NOT_NULL(err);
        free(err);
        cbm_query_free(q);
    }
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  EXECUTION TESTS (end-to-end against store)
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: set up the standard test graph.
 * Nodes: HandleOrder, ValidateOrder, SubmitOrder (Function), main (Module), LogError (Function)
 * Edges: HandleOrder→ValidateOrder (CALLS), ValidateOrder→SubmitOrder (CALLS),
 *        HandleOrder→LogError (CALLS), main→HandleOrder (DEFINES)
 */
static cbm_store_t *setup_cypher_store(void) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "HandleOrder",
                     .qualified_name = "test.HandleOrder",
                     .file_path = "handler.go",
                     .start_line = 10,
                     .end_line = 30};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "ValidateOrder",
                     .qualified_name = "test.ValidateOrder",
                     .file_path = "validate.go",
                     .start_line = 5,
                     .end_line = 15};
    cbm_node_t n3 = {.project = "test",
                     .label = "Function",
                     .name = "SubmitOrder",
                     .qualified_name = "test.SubmitOrder",
                     .file_path = "submit.go"};
    cbm_node_t n4 = {
        .project = "test", .label = "Module", .name = "main", .qualified_name = "test.main"};
    cbm_node_t n5 = {.project = "test",
                     .label = "Function",
                     .name = "LogError",
                     .qualified_name = "test.LogError",
                     .file_path = "log.go"};

    int64_t id1 = cbm_store_upsert_node(s, &n1);
    int64_t id2 = cbm_store_upsert_node(s, &n2);
    int64_t id3 = cbm_store_upsert_node(s, &n3);
    int64_t id4 = cbm_store_upsert_node(s, &n4);
    int64_t id5 = cbm_store_upsert_node(s, &n5);

    cbm_edge_t e1 = {.project = "test", .source_id = id1, .target_id = id2, .type = "CALLS"};
    cbm_edge_t e2 = {.project = "test", .source_id = id2, .target_id = id3, .type = "CALLS"};
    cbm_edge_t e3 = {.project = "test", .source_id = id1, .target_id = id5, .type = "CALLS"};
    cbm_edge_t e4 = {.project = "test", .source_id = id4, .target_id = id1, .type = "DEFINES"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);
    cbm_store_insert_edge(s, &e3);
    cbm_store_insert_edge(s, &e4);

    return s;
}

TEST(cypher_exec_match_all_functions) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function)", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4); /* HandleOrder, ValidateOrder, SubmitOrder, LogError */
    ASSERT_GT(r.col_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Upstream 886a8856: the WHERE grammar recurses once per nested '(' and once
 * per NOT with no depth counter, so tens of KB of nesting exhausted the stack
 * at parse time. Parse in a forked child so a crash surfaces as a killing
 * signal; a bounded parser must reject cleanly instead. */
TEST(cypher_deep_nesting_rejected_not_crash) {
#ifdef _WIN32
    SKIP_PLATFORM("fork crash-isolation is POSIX-only; the depth cap is platform-agnostic");
#else
    enum { NEST = 30000 };
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        char *q = malloc(NEST * 2 + 64);
        if (!q) {
            _exit(2);
        }
        int n = snprintf(q, 64, "MATCH (f:Function) WHERE ");
        for (int i = 0; i < NEST; i++) {
            q[n++] = '(';
        }
        n += snprintf(q + n, 32, "f.name = \"x\"");
        for (int i = 0; i < NEST; i++) {
            q[n++] = ')';
        }
        q[n] = '\0';
        cbm_store_t *s = setup_cypher_store();
        cbm_cypher_result_t r = {0};
        /* Any clean outcome is acceptable — success or a parse error. Only a
         * crash is a failure, and that is what the signal check below catches. */
        (void)cbm_cypher_execute(s, q, "test", 0, &r);
        cbm_cypher_result_free(&r);
        cbm_store_close(s);
        free(q);
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        char m[96];
        snprintf(m, sizeof(m), "parser killed by signal %d — unbounded recursion depth",
                 WTERMSIG(status));
        FAIL(m);
    }
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    PASS();
#endif
}

/* Upstream d0457f2b: an UNWIND literal list whose element is longer than the
 * 2KB assembly buffer overflowed the stack — snprintf reports the length it
 * WOULD have written, so blen ran past sizeof(buf) and the trailing writes
 * went out of bounds. The query text is agent-controlled via query_graph. */
TEST(cypher_parse_unwind_oversized_literal_no_overflow) {
    char query[4096];
    char big[3000];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    snprintf(query, sizeof(query), "UNWIND [\"%s\"] AS x MATCH (f) RETURN f.name", big);

    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(query, &q, &err);
    /* Must not crash and must produce a NUL-terminated, in-bounds expression. */
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->unwind_expr);
    ASSERT_STR_EQ(q->unwind_alias, "x");
    cbm_query_free(q);
    PASS();
}

/* Companion: many oversized elements accumulate blen well past the buffer,
 * which also underflowed the (size_t)(cap - blen) length passed to snprintf. */
TEST(cypher_parse_unwind_many_elements_no_overflow) {
    char query[8192];
    int off = snprintf(query, sizeof(query), "UNWIND [");
    for (int i = 0; i < 200; i++) {
        off += snprintf(query + off, sizeof(query) - (size_t)off, "%s\"element_value_%d\"",
                        i ? "," : "", i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, "] AS x MATCH (f) RETURN f.name");

    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(query, &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->unwind_expr);
    cbm_query_free(q);
    PASS();
}

/* Upstream b07dbe67: an OPTIONAL MATCH whose label matches zero nodes drove
 * cross_join_nodes with extra_count == 0. The old allocation
 * (bind_count * 0 + 1) reserved a single slot, but the OPTIONAL fallback then
 * wrote one binding per existing row — heap OOB once the first MATCH bound
 * more than one node. The query text is agent-controlled via query_graph. */
TEST(cypher_exec_optional_empty_label_no_overflow) {
    cbm_store_t *s = setup_cypher_store(); /* 4 Function nodes */
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(
        s, "MATCH (a:Function) OPTIONAL MATCH (b:NoSuchLabel) RETURN a.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* One row per Function, each with b left unbound. */
    ASSERT_EQ(r.row_count, 4);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #58 (upstream PR 2445): the relationship variant of the test above. An
 * OPTIONAL MATCH with a relationship whose start and terminal are both unbound,
 * and whose start matches no node, drives cross_join_with_rels with
 * extra_count == 0. Its fallback once wrote one row per binding into a buffer
 * sized for one (heap overflow under ASan); it now appends through
 * binding_out_append. */
TEST(cypher_exec_optional_empty_label_rel_no_overflow) {
    cbm_store_t *s = setup_cypher_store(); /* 4 Function nodes */
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a:Function) OPTIONAL MATCH (x:NoSuchLabel)-[:CALLS]->(y) "
                                "RETURN a.name, x.name, y.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.col_count, 3);
    /* One row per Function, each with x and y left unbound (""). */
    ASSERT_EQ(r.row_count, 4);
    const char *want[] = {"HandleOrder", "ValidateOrder", "SubmitOrder", "LogError"};
    for (int w = 0; w < 4; w++) {
        int seen = 0;
        for (int i = 0; i < r.row_count; i++) {
            if (strcmp(r.rows[i][0], want[w]) == 0) {
                seen++;
            }
        }
        ASSERT_EQ(seen, 1);
    }
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_STR_EQ(r.rows[i][1], "");
        ASSERT_STR_EQ(r.rows[i][2], "");
    }

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Arithmetic-boundary companion: the node cross-join sizes its buffer from
 * bind_count * extra_count. As a plain int that product wraps past INT_MAX to
 * a negative/garbage malloc size. cbm_cypher_cross_join_alloc now computes it
 * in size_t and rejects counts that would not fit the int binding counter or
 * overflow the byte size. Tested directly so the boundary is exercised without
 * allocating billions of bindings. */
TEST(cypher_cross_join_alloc_rejects_overflow) {
    size_t n = 0;

    /* 46341 * 46341 = 2147488281 > INT_MAX (2147483647): pre-fix the int
     * product wrapped negative -> tiny malloc -> heap OOB. Now rejected. */
    ASSERT_TRUE(cbm_cypher_cross_join_alloc(46341, 46341, false, &n) != 0);

    /* A normal join still succeeds: bind_count * extra_count + 1 slots. */
    ASSERT_EQ(cbm_cypher_cross_join_alloc(4, 3, false, &n), 0);
    ASSERT_EQ(n, (size_t)13);

    /* OPTIONAL with no extra nodes reserves one fallback row per binding + 1. */
    ASSERT_EQ(cbm_cypher_cross_join_alloc(4, 0, true, &n), 0);
    ASSERT_EQ(n, (size_t)5);

    /* Non-OPTIONAL with no extra nodes: just the sentinel slot. */
    ASSERT_EQ(cbm_cypher_cross_join_alloc(4, 0, false, &n), 0);
    ASSERT_EQ(n, (size_t)1);

    PASS();
}

/* Upstream c2a19b89: expand_pattern_rels sized its OPTIONAL-expansion output
 * as bind_cap*10 + 1 — room for the bounded expansion plus a SINGLE OPTIONAL
 * fallback row. When one source saturated the expansion and two or more later
 * sources took the OPTIONAL (no-match) path, the second fallback write ran
 * past the allocation (heap OOB). The buffer now holds max_new + bind_count
 * rows, and the LIMIT caps only the output. */
TEST(cypher_exec_optional_rel_saturated_no_overflow) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_node_t hub = {
        .project = "test", .label = "Function", .name = "hub", .qualified_name = "test.hub"};
    int64_t hub_id = cbm_store_upsert_node(s, &hub);
    for (int i = 0; i < 20; i++) {
        char nm[32];
        char qn[48];
        snprintf(nm, sizeof(nm), "leaf%02d", i);
        snprintf(qn, sizeof(qn), "test.leaf%02d", i);
        cbm_node_t leaf = {
            .project = "test", .label = "Function", .name = nm, .qualified_name = qn};
        cbm_store_upsert_node(s, &leaf);
    }

    /* Give the hub 300 CALLS edges (> max_new = 210) so its expansion saturates
     * max_new; targets are non-Function so they don't inflate bind_cap. */
    for (int i = 0; i < 300; i++) {
        char nm[32];
        char qn[48];
        snprintf(nm, sizeof(nm), "callee%d", i);
        snprintf(qn, sizeof(qn), "test.callee%d", i);
        cbm_node_t callee = {.project = "test", .label = "Var", .name = nm, .qualified_name = qn};
        int64_t cid = cbm_store_upsert_node(s, &callee);
        cbm_edge_t e = {.project = "test", .source_id = hub_id, .target_id = cid, .type = "CALLS"};
        cbm_store_insert_edge(s, &e);
    }

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (a:Function) OPTIONAL MATCH (a)-[:CALLS]->(b) RETURN a.name", "test", 5, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GT(r.row_count, 0);
    ASSERT_TRUE(r.row_count <= 5);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Companion to the saturation regression: when the expansion does NOT
 * saturate, every leaf's OPTIONAL fallback row must survive with its target
 * unbound. A `row_count > 0` check is too weak — it can pass on hub rows
 * alone — so this asserts a specific leaf appears with an empty b.name, and
 * that a real expanded hub row is present too. */
TEST(cypher_exec_optional_rel_leaf_fallback_survives) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_node_t hub = {
        .project = "test", .label = "Function", .name = "hub", .qualified_name = "test.hub"};
    int64_t hub_id = cbm_store_upsert_node(s, &hub);
    for (int i = 0; i < 3; i++) {
        char nm[32];
        char qn[48];
        snprintf(nm, sizeof(nm), "leaf%d", i);
        snprintf(qn, sizeof(qn), "test.leaf%d", i);
        cbm_node_t leaf = {
            .project = "test", .label = "Function", .name = nm, .qualified_name = qn};
        cbm_store_upsert_node(s, &leaf);
    }
    for (int i = 0; i < 2; i++) {
        char nm[32];
        char qn[48];
        snprintf(nm, sizeof(nm), "callee%d", i);
        snprintf(qn, sizeof(qn), "test.callee%d", i);
        cbm_node_t callee = {.project = "test", .label = "Var", .name = nm, .qualified_name = qn};
        int64_t cid = cbm_store_upsert_node(s, &callee);
        cbm_edge_t e = {.project = "test", .source_id = hub_id, .target_id = cid, .type = "CALLS"};
        cbm_store_insert_edge(s, &e);
    }

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (a:Function) OPTIONAL MATCH (a)-[:CALLS]->(b) RETURN a.name, b.name", "test", 0,
        &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.col_count, 2);

    bool leaf_fallback = false;
    bool hub_expanded = false;
    for (int i = 0; i < r.row_count; i++) {
        const char *a = r.rows[i][0];
        const char *b = r.rows[i][1];
        if (strcmp(a, "leaf0") == 0 && b[0] == '\0') {
            leaf_fallback = true;
        }
        if (strcmp(a, "hub") == 0 && b[0] != '\0') {
            hub_expanded = true;
        }
    }
    ASSERT_TRUE(leaf_fallback); /* the OPTIONAL no-match row survived */
    ASSERT_TRUE(hub_expanded);  /* the expansion still produced bound rows */

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Upstream 6bd11ffb (#601): an unbounded whole-graph OPTIONAL MATCH / GROUP
 * BY does O(bindings x groups) work for minutes with no wall-clock guard —
 * the 100k row ceiling never fires because no rows are produced, so
 * query_graph just hangs. With the deadline armed to trip immediately
 * (budget 0), the runaway query must abort with a clear error instead of
 * returning a (misleading, possibly partial) result. */
TEST(cypher_exec_deadline_aborts_runaway_query_issue601) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    cbm_cypher_test_set_deadline_ms(0); /* trip on the first hot-loop check */
    int rc = cbm_cypher_execute(
        s, "MATCH (a) OPTIONAL MATCH (a)-[:CALLS]->(b) RETURN a.qualified_name, count(b)", "test",
        0, &r);
    cbm_cypher_test_set_deadline_ms(-1); /* restore default before asserting (thread-local) */

    ASSERT_TRUE(rc != 0);
    ASSERT_NOT_NULL(r.error);
    ASSERT_TRUE(strstr(r.error, "time limit") != NULL);
    ASSERT_EQ(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Companion: the default (ample) budget must NOT false-positive on a normal
 * small query — it still returns its rows. */
TEST(cypher_exec_deadline_allows_normal_query_issue601) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(
        s, "MATCH (a) OPTIONAL MATCH (a)-[:CALLS]->(b) RETURN a.qualified_name, count(b)", "test",
        0, &r);

    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(r.error == NULL);
    ASSERT_GT(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* The deadline must also cover the cross joins and both ORDER BY sorts, and a
 * budget that runs out inside one of them must fail the query rather than
 * return its rows. The hook expires the deadline when that phase checks it. */
static bool cypher_expires_at(const char *site, const char *query) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    cbm_cypher_test_expire_deadline_at(site);
    int rc = cbm_cypher_execute(s, query, "test", 0, &r);
    cbm_cypher_test_expire_deadline_at(NULL);
    bool expired = rc != 0 && r.error && strstr(r.error, "time limit") && r.row_count == 0;
    if (!expired) {
        printf("\n    phase %s: rc=%d rows=%d error=%s\n", site, rc, r.row_count,
               r.error ? r.error : "(none)");
    }
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    return expired;
}

TEST(cypher_exec_deadline_covers_joins_and_sorts) {
    ASSERT_TRUE(cypher_expires_at("cross_join",
                                  "MATCH (a:Module) MATCH (b:Function) RETURN a.name, b.name"));
    ASSERT_TRUE(cypher_expires_at(
        "cross_join_rels",
        "MATCH (a:Module) MATCH (b:Function)-[:CALLS]->(c) RETURN a.name, c.name"));
    ASSERT_TRUE(cypher_expires_at("sort_bindings",
                                  "MATCH (f:Function) WITH f.name AS n ORDER BY n RETURN n"));
    ASSERT_TRUE(
        cypher_expires_at("order_by", "MATCH (f:Function) RETURN f.name ORDER BY f.name"));
    PASS();
}

/* #59: ORDER BY, DISTINCT, GROUP BY and COUNT(DISTINCT) moved from quadratic
 * scans to a merge sort and hash lookups. These differential tests check the
 * results against the engine's own unsorted, undeduplicated, ungrouped rows.
 * Fixture: 400 functions whose names repeat every 37, files every 13 and
 * start lines every 50, so sorts have ties, DISTINCT has duplicates, groups
 * are large and per-group distinct sets outgrow the linear-scan threshold. */
enum { ORDER_FIXTURE_NODES = 400 };

static cbm_store_t *setup_order_store(void) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    for (int i = 0; i < ORDER_FIXTURE_NODES; i++) {
        char name[32];
        char qn[48];
        char file[32];
        snprintf(name, sizeof(name), "fn%02d", (i * 7) % 37);
        snprintf(qn, sizeof(qn), "test.mod.fn_%03d", i);
        snprintf(file, sizeof(file), "file%02d.go", (i * 5) % 13);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = file,
                        .start_line = (i * 11) % 50 + 1,
                        .end_line = 60};
        cbm_store_upsert_node(s, &n);
    }
    return s;
}

/* How the reference sort compares one key: RETURN compares numeric columns
 * as integers, WITH compares numerically when both values parse as numbers. */
typedef enum { KEY_TEXT, KEY_INT, KEY_WITH } order_mode_t;

typedef struct {
    int col;
    bool desc;
    order_mode_t mode;
} order_key_t;

static int order_cmp(const char **a, const char **b, const order_key_t *keys, int nkeys) {
    int cmp = 0;
    for (int k = 0; k < nkeys && cmp == 0; k++) {
        const char *x = a[keys[k].col];
        const char *y = b[keys[k].col];
        char *ex = NULL;
        char *ey = NULL;
        double dx = strtod(x, &ex);
        double dy = strtod(y, &ey);
        if (keys[k].mode == KEY_INT) {
            long lx = strtol(x, NULL, 10);
            long ly = strtol(y, NULL, 10);
            cmp = (lx > ly) - (lx < ly);
        } else if (keys[k].mode == KEY_WITH && ex != x && ey != y) {
            cmp = (dx > dy) - (dx < dy);
        } else {
            cmp = strcmp(x, y);
        }
        if (keys[k].desc) {
            cmp = -cmp;
        }
    }
    return cmp;
}

static bool run_rows(cbm_store_t *s, const char *query, cbm_cypher_result_t *r) {
    memset(r, 0, sizeof(*r));
    int rc = cbm_cypher_execute(s, query, "test", 0, r);
    if (rc != 0 || r->error) {
        printf("\n    query failed: %s\n    error: %s\n", query, r->error ? r->error : "(none)");
        return false;
    }
    return true;
}

static bool row_equals(const char **a, const char **b, int cols) {
    for (int c = 0; c < cols; c++) {
        if (strcmp(a[c], b[c]) != 0) {
            return false;
        }
    }
    return true;
}

/* `sorted` must return exactly the rows of `unsorted`, in the order a stable
 * insertion sort with the given keys puts them. */
static bool sorted_like(const char *unsorted, const char *sorted, const order_key_t *keys,
                        int nkeys) {
    cbm_store_t *s = setup_order_store();
    cbm_cypher_result_t ru;
    cbm_cypher_result_t rs;
    bool ok = run_rows(s, unsorted, &ru) && run_rows(s, sorted, &rs) &&
              ru.row_count == ORDER_FIXTURE_NODES && rs.row_count == ru.row_count &&
              rs.col_count == ru.col_count;
    int *idx = malloc(ORDER_FIXTURE_NODES * sizeof(int));
    for (int i = 0; ok && i < ru.row_count; i++) {
        idx[i] = i;
        for (int j = i; j > 0 && order_cmp(ru.rows[idx[j - 1]], ru.rows[idx[j]], keys, nkeys) > 0;
             j--) {
            int t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    }
    for (int i = 0; ok && i < ru.row_count; i++) {
        if (!row_equals(rs.rows[i], ru.rows[idx[i]], ru.col_count)) {
            printf("\n    %s\n    row %d: got %s, want %s\n", sorted, i, rs.rows[i][0],
                   ru.rows[idx[i]][0]);
            ok = false;
        }
    }
    free(idx);
    cbm_cypher_result_free(&ru);
    cbm_cypher_result_free(&rs);
    cbm_store_close(s);
    return ok;
}

TEST(cypher_exec_order_by_matches_stable_sort) {
    const order_key_t by_name[] = {{0, false, KEY_TEXT}};
    ASSERT_TRUE(sorted_like("MATCH (f:Function) RETURN f.name, f.qualified_name",
                            "MATCH (f:Function) RETURN f.name, f.qualified_name ORDER BY f.name",
                            by_name, 1));
    const order_key_t by_line_desc_name[] = {{0, true, KEY_INT}, {1, false, KEY_TEXT}};
    ASSERT_TRUE(sorted_like(
        "MATCH (f:Function) RETURN f.start_line AS l, f.name AS n, f.qualified_name",
        "MATCH (f:Function) RETURN f.start_line AS l, f.name AS n, f.qualified_name "
        "ORDER BY l DESC, n",
        by_line_desc_name, 2));
    const order_key_t all_tied[] = {{0, false, KEY_TEXT}};
    ASSERT_TRUE(
        sorted_like("MATCH (f:Function) RETURN f.label, f.qualified_name",
                    "MATCH (f:Function) RETURN f.label, f.qualified_name ORDER BY f.label",
                    all_tied, 1));

    const order_key_t with_name[] = {{0, false, KEY_WITH}};
    ASSERT_TRUE(sorted_like(
        "MATCH (f:Function) WITH f.name AS n, f.qualified_name AS q RETURN n, q",
        "MATCH (f:Function) WITH f.name AS n, f.qualified_name AS q ORDER BY n RETURN n, q",
        with_name, 1));
    const order_key_t with_line_desc_name[] = {{0, true, KEY_WITH}, {1, false, KEY_WITH}};
    ASSERT_TRUE(sorted_like("MATCH (f:Function) WITH f.start_line AS l, f.name AS n, "
                            "f.qualified_name AS q RETURN l, n, q",
                            "MATCH (f:Function) WITH f.start_line AS l, f.name AS n, "
                            "f.qualified_name AS q ORDER BY l DESC, n RETURN l, n, q",
                            with_line_desc_name, 2));
    const order_key_t with_tied[] = {{0, false, KEY_WITH}};
    ASSERT_TRUE(sorted_like(
        "MATCH (f:Function) WITH f.label AS t, f.qualified_name AS q RETURN t, q",
        "MATCH (f:Function) WITH f.label AS t, f.qualified_name AS q ORDER BY t RETURN t, q",
        with_tied, 1));
    PASS();
}

/* `distinct` must return the first occurrence of every distinct row of `all`,
 * in order. */
static bool distinct_like(const char *all, const char *distinct) {
    cbm_store_t *s = setup_order_store();
    cbm_cypher_result_t ra;
    cbm_cypher_result_t rd;
    bool ok = run_rows(s, all, &ra) && run_rows(s, distinct, &rd) &&
              ra.row_count == ORDER_FIXTURE_NODES && rd.col_count == ra.col_count;
    int want = 0;
    for (int i = 0; ok && i < ra.row_count; i++) {
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = row_equals(ra.rows[i], ra.rows[j], ra.col_count);
        }
        if (seen) {
            continue;
        }
        if (want >= rd.row_count || !row_equals(rd.rows[want], ra.rows[i], ra.col_count)) {
            printf("\n    %s\n    row %d differs from first occurrence %d\n", distinct, want, i);
            ok = false;
        }
        want++;
    }
    if (ok && want != rd.row_count) {
        printf("\n    %s\n    rows: got %d, want %d\n", distinct, rd.row_count, want);
        ok = false;
    }
    cbm_cypher_result_free(&ra);
    cbm_cypher_result_free(&rd);
    cbm_store_close(s);
    return ok && want > 0 && want < ORDER_FIXTURE_NODES;
}

TEST(cypher_exec_distinct_keeps_first_occurrences) {
    ASSERT_TRUE(distinct_like("MATCH (f:Function) RETURN f.name, f.label",
                              "MATCH (f:Function) RETURN DISTINCT f.name, f.label"));
    ASSERT_TRUE(
        distinct_like("MATCH (f:Function) WITH f.file_path AS p, f.label AS t RETURN p, t",
                      "MATCH (f:Function) WITH DISTINCT f.file_path AS p, f.label AS t RETURN p, t"));
    PASS();
}

/* WITH DISTINCT compares whole values: "a|b","c" and "a","b|c" are different
 * rows even though their values joined with '|' are the same string. */
TEST(cypher_exec_with_distinct_separator_in_values) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t a = {.project = "test",
                    .label = "Function",
                    .name = "a|b",
                    .qualified_name = "test.a1",
                    .file_path = "c"};
    cbm_node_t b = {.project = "test",
                    .label = "Function",
                    .name = "a",
                    .qualified_name = "test.a2",
                    .file_path = "b|c"};
    cbm_store_upsert_node(s, &a);
    cbm_store_upsert_node(s, &b);
    cbm_cypher_result_t r = {0};
    ASSERT_EQ(cbm_cypher_execute(
                  s, "MATCH (f:Function) WITH DISTINCT f.name AS n, f.file_path AS p RETURN n, p",
                  "test", 0, &r),
              0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* `grouped` returns (key, count) per group; it must list the keys of `pairs`
 * (key, value) in first-seen order, with the number of rows per key, or with
 * the number of distinct values when `distinct`. */
static bool grouped_like(const char *pairs, const char *grouped, bool distinct) {
    cbm_store_t *s = setup_order_store();
    cbm_cypher_result_t rp;
    cbm_cypher_result_t rg;
    bool ok = run_rows(s, pairs, &rp) && run_rows(s, grouped, &rg) &&
              rp.row_count == ORDER_FIXTURE_NODES && rp.col_count == 2 && rg.col_count == 2;
    int groups = 0;
    for (int i = 0; ok && i < rp.row_count; i++) {
        bool first = true;
        for (int j = 0; j < i && first; j++) {
            first = strcmp(rp.rows[i][0], rp.rows[j][0]) != 0;
        }
        if (!first) {
            continue;
        }
        int count = 0;
        for (int j = i; j < rp.row_count; j++) {
            if (strcmp(rp.rows[j][0], rp.rows[i][0]) != 0) {
                continue;
            }
            bool counted = false;
            for (int m = i; distinct && m < j && !counted; m++) {
                counted = strcmp(rp.rows[m][0], rp.rows[i][0]) == 0 &&
                          strcmp(rp.rows[m][1], rp.rows[j][1]) == 0;
            }
            count += counted ? 0 : 1;
        }
        char want[16];
        snprintf(want, sizeof(want), "%d", count);
        if (groups >= rg.row_count || strcmp(rg.rows[groups][0], rp.rows[i][0]) != 0 ||
            strcmp(rg.rows[groups][1], want) != 0) {
            printf("\n    %s\n    group %d: got %s=%s, want %s=%s\n", grouped, groups,
                   groups < rg.row_count ? rg.rows[groups][0] : "(none)",
                   groups < rg.row_count ? rg.rows[groups][1] : "", rp.rows[i][0], want);
            ok = false;
        }
        groups++;
    }
    if (ok && groups != rg.row_count) {
        printf("\n    %s\n    groups: got %d, want %d\n", grouped, rg.row_count, groups);
        ok = false;
    }
    cbm_cypher_result_free(&rp);
    cbm_cypher_result_free(&rg);
    cbm_store_close(s);
    return ok && groups > 0;
}

TEST(cypher_exec_group_by_keeps_first_seen_groups) {
    ASSERT_TRUE(grouped_like("MATCH (f:Function) RETURN f.name, f.qualified_name",
                             "MATCH (f:Function) RETURN f.name, count(f)", false));
    ASSERT_TRUE(grouped_like("MATCH (f:Function) RETURN f.name, f.qualified_name",
                             "MATCH (f:Function) WITH f.name AS n, count(f) AS c RETURN n, c",
                             false));
    PASS();
}

TEST(cypher_exec_count_distinct_matches_reference) {
    ASSERT_TRUE(grouped_like("MATCH (f:Function) RETURN f.file_path, f.name",
                             "MATCH (f:Function) RETURN f.file_path, count(DISTINCT f.name)",
                             true));
    ASSERT_TRUE(grouped_like("MATCH (f:Function) RETURN f.label, f.name",
                             "MATCH (f:Function) RETURN f.label, count(DISTINCT f.name)", true));
    ASSERT_TRUE(grouped_like("MATCH (f:Function) RETURN f.file_path, f.name",
                             "MATCH (f:Function) WITH f.file_path AS p, "
                             "count(DISTINCT f.name) AS c RETURN p, c",
                             true));
    PASS();
}

/* Upstream 73a3c347 (#797): variable-length / repeated-variable path
 * semantics. Fixture: loopy has a SELF-LOOP as one of its outbound CALLS
 * edges plus a real 2-chain loopy->mid->leaf. Correct semantics:
 *  - a repeated node variable must unify: (a)-[:CALLS]->(a) matches ONLY
 *    the self-loop, not every edge;
 *  - relationship uniqueness within a path: the self-loop cannot be
 *    traversed twice in one trail, so no long *k..k path exists;
 *  - the engine hop cap must be an ADVERTISED clamp (result->warning), not
 *    silently indistinguishable from "no such path". */
TEST(cypher_exec_varlength_path_semantics_issue797) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t loopy = {.project = "test",
                        .label = "Function",
                        .name = "loopy",
                        .qualified_name = "test.mod.loopy",
                        .file_path = "mod.go",
                        .start_line = 1,
                        .end_line = 2};
    cbm_node_t mid = {.project = "test",
                      .label = "Function",
                      .name = "mid",
                      .qualified_name = "test.mod.mid",
                      .file_path = "mod.go",
                      .start_line = 3,
                      .end_line = 4};
    cbm_node_t leaf = {.project = "test",
                       .label = "Function",
                       .name = "leaf",
                       .qualified_name = "test.mod.leaf",
                       .file_path = "mod.go",
                       .start_line = 5,
                       .end_line = 6};
    int64_t id_loopy = cbm_store_upsert_node(s, &loopy);
    int64_t id_mid = cbm_store_upsert_node(s, &mid);
    int64_t id_leaf = cbm_store_upsert_node(s, &leaf);
    ASSERT_GT(id_loopy, 0);
    cbm_edge_t self_loop = {
        .project = "test", .source_id = id_loopy, .target_id = id_loopy, .type = "CALLS"};
    cbm_edge_t e1 = {
        .project = "test", .source_id = id_loopy, .target_id = id_mid, .type = "CALLS"};
    cbm_edge_t e2 = {.project = "test", .source_id = id_mid, .target_id = id_leaf, .type = "CALLS"};
    cbm_store_insert_edge(s, &self_loop);
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);

    /* Repeated variable must unify — only the self-loop matches. */
    cbm_cypher_result_t r1 = {0};
    ASSERT_EQ(cbm_cypher_execute(s, "MATCH (a)-[:CALLS]->(a) RETURN a.name", "test", 0, &r1), 0);
    ASSERT_EQ(r1.row_count, 1);
    cbm_cypher_result_free(&r1);

    /* *2..2 from loopy has two relationship-unique trails: the self-loop
     * followed by e1 reaches mid, and e1 followed by e2 reaches leaf.
     * Reusing the self-loop within one trail remains forbidden. */
    cbm_cypher_result_t r2 = {0};
    ASSERT_EQ(cbm_cypher_execute(s,
                                 "MATCH (a {name: \"loopy\"})-[:CALLS*2..2]->(b) "
                                 "RETURN DISTINCT b.name",
                                 "test", 0, &r2),
              0);
    ASSERT_EQ(r2.row_count, 2);
    bool saw_mid = false;
    bool saw_leaf = false;
    for (int i = 0; i < r2.row_count; i++) {
        saw_mid |= strcmp(r2.rows[i][0], "mid") == 0;
        saw_leaf |= strcmp(r2.rows[i][0], "leaf") == 0;
    }
    ASSERT_TRUE(saw_mid);
    ASSERT_TRUE(saw_leaf);
    cbm_cypher_result_free(&r2);

    /* No directed path of length 5 exists at all. */
    cbm_cypher_result_t r3 = {0};
    ASSERT_EQ(cbm_cypher_execute(s,
                                 "MATCH (a {name: \"loopy\"})-[:CALLS*5..5]->(b) "
                                 "RETURN b.name",
                                 "test", 0, &r3),
              0);
    ASSERT_EQ(r3.row_count, 0);
    cbm_cypher_result_free(&r3);

    /* A hop range beyond the engine ceiling must be an ADVERTISED clamp,
     * not silently indistinguishable from "no such path". */
    cbm_cypher_result_t r4 = {0};
    ASSERT_EQ(
        cbm_cypher_execute(s, "MATCH (a)-[:CALLS*150..150]->(b) RETURN b.name", "test", 0, &r4), 0);
    ASSERT_EQ(r4.row_count, 0);
    ASSERT_NOT_NULL(r4.warning);
    ASSERT_NOT_NULL(strstr(r4.warning, "clamped"));
    cbm_cypher_result_free(&r4);

    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_eq) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\"", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_regex) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name =~ \".*Order.*\"", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3); /* HandleOrder, ValidateOrder, SubmitOrder */

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_contains) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name CONTAINS \"Order\"", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_starts_with) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name STARTS WITH \"Handle\"", "test",
                                0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_return_properties) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "RETURN f.name, f.qualified_name, f.file_path",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_EQ(r.col_count, 3);
    /* Columns should be f.name, f.qualified_name, f.file_path */
    ASSERT_STR_EQ(r.columns[0], "f.name");
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT_STR_EQ(r.rows[0][1], "test.HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* ── Scalar / introspection functions (full-suite Tier 1) ──────── */

TEST(cypher_func_labels) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN labels(f)", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "[\"Function\"]");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* labels(n)[i] / keys(n)[i] select one list element; out of range is null. */
TEST(cypher_func_list_index) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN "
                                "labels(f)[0], labels(f)[-1] AS last, labels(f)[1], keys(f)[0]",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.columns[0], "labels(f)[0]");
    ASSERT_STR_EQ(r.rows[0][0], "Function");
    ASSERT_STR_EQ(r.rows[0][1], "Function");
    ASSERT_STR_EQ(r.rows[0][2], "");
    ASSERT_STR_EQ(r.rows[0][3], "name");
    cbm_cypher_result_free(&r);

    /* Other bracket forms still fail loudly (#373). */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN labels(f)[0..1]", "test", 0, &r);
    ASSERT_TRUE(rc != 0 || r.error != NULL);
    cbm_cypher_result_free(&r);
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name[0]", "test", 0, &r);
    ASSERT_TRUE(rc != 0 || r.error != NULL);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* WHERE compares two bound properties, not only a property and a literal. */
TEST(cypher_where_property_vs_property) {
    cbm_store_t *s = setup_cypher_store();
    const char *base = "MATCH (a:Function)-[:CALLS]->(b:Function) WHERE ";
    struct {
        const char *where;
        int rows;
    } cases[] = {
        {"a.file_path <> b.file_path RETURN a.name", 3},
        {"a.file_path = b.file_path RETURN a.name", 0},
        {"NOT a.name = b.name RETURN a.name", 3},
        {"a.start_line < b.end_line RETURN b.name", 1},
        {"NOT b.name = \"SubmitOrder\" RETURN b.name", 2},
        {"a.name = \"HandleOrder\" AND NOT b.name = \"LogError\" RETURN b.name", 1},
    };
    char q[256];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cbm_cypher_result_t r = {0};
        snprintf(q, sizeof(q), "%s%s", base, cases[i].where);
        int rc = cbm_cypher_execute(s, q, "test", 0, &r);
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(r.row_count, cases[i].rows);
        if (i == 3) {
            ASSERT_STR_EQ(r.rows[0][0], "ValidateOrder");
        }
        cbm_cypher_result_free(&r);
    }

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.start_line < f.end_line RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_type) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function)-[r:CALLS]->(g:Function) RETURN type(r) LIMIT 1", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "CALLS");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_id) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN id(f)",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    /* id is a non-empty numeric string */
    ASSERT_TRUE(r.rows[0][0][0] >= '0' && r.rows[0][0][0] <= '9');
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_keys) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN keys(f)", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_TRUE(strstr(r.rows[0][0], "\"name\"") != NULL);
    ASSERT_TRUE(strstr(r.rows[0][0], "\"qualified_name\"") != NULL);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_properties) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN properties(f)", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_EQ(r.rows[0][0][0], '{'); /* a JSON object */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_tointeger_tofloat) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "RETURN toInteger(f.start_line), toFloat(f.start_line)",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "10"); /* start_line = 10 */
    ASSERT_STR_EQ(r.rows[0][1], "10");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_size_reverse) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"LogError\" "
                                "RETURN size(f.name), length(f.name), reverse(f.name)",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "8"); /* "LogError" has 8 chars */
    ASSERT_STR_EQ(r.rows[0][1], "8");
    ASSERT_STR_EQ(r.rows[0][2], "rorrEgoL");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_multiarg) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "RETURN substring(f.name, 0, 6), left(f.name, 6), "
                                "right(f.name, 5), replace(f.name, \"Order\", \"Req\"), "
                                "coalesce(f.missing, \"fallback\")",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "Handle");    /* substring("HandleOrder",0,6) */
    ASSERT_STR_EQ(r.rows[0][1], "Handle");    /* left(...,6) */
    ASSERT_STR_EQ(r.rows[0][2], "Order");     /* right("HandleOrder",5) */
    ASSERT_STR_EQ(r.rows[0][3], "HandleReq"); /* replace Order->Req */
    ASSERT_STR_EQ(r.rows[0][4], "fallback");  /* coalesce: f.missing empty -> literal */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Upstream 755ffe57 (#874): coalesce() in WHERE — null-safe numeric filter
 * over an optional JSON property. Nodes lacking the property fall back to
 * the literal default instead of failing to parse. */
TEST(cypher_issue874_where_coalesce_numeric) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "DeepLoop",
                     .qualified_name = "test.DeepLoop",
                     .file_path = "deep.go",
                     .properties_json = "{\"transitive_loop_depth\":5}"};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "NoMetrics",
                     .qualified_name = "test.NoMetrics",
                     .file_path = "flat.go"};
    cbm_store_upsert_node(s, &n1);
    cbm_store_upsert_node(s, &n2);

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) "
                                "WHERE coalesce(f.transitive_loop_depth, 0) >= 2 "
                                "RETURN f.qualified_name LIMIT 10",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1); /* only DeepLoop; NoMetrics coalesces to 0 */
    ASSERT_STR_EQ(r.rows[0][0], "test.DeepLoop");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #874: coalesce() in WHERE with a string fallback and first-arg-wins. */
TEST(cypher_issue874_where_coalesce_string) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    /* Missing property on every node → fallback literal matches all 4 Functions */
    int rc = cbm_cypher_execute(
        s,
        "MATCH (f:Function) WHERE coalesce(f.missing, \"fallback\") = \"fallback\" "
        "RETURN f.name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4);
    cbm_cypher_result_free(&r);

    /* Present first arg wins over the fallback */
    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE coalesce(f.name, \"zz\") = \"HandleOrder\" RETURN f.name",
        "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r2.row_count, 1);
    ASSERT_STR_EQ(r2.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r2);

    cbm_store_close(s);
    PASS();
}

/* #874: function LHS composes with NOT and AND like any other condition. */
TEST(cypher_issue874_where_coalesce_not_and) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE NOT coalesce(f.missing, \"x\") = \"x\" RETURN f.name", "test",
        0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 0); /* every node coalesces to "x" — NOT filters all */
    cbm_cypher_result_free(&r);

    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE coalesce(f.missing, \"1\") = \"1\" "
                            "AND f.name CONTAINS \"Order\" RETURN f.name",
                            "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r2.row_count, 3); /* HandleOrder, ValidateOrder, SubmitOrder */
    cbm_cypher_result_free(&r2);

    cbm_store_close(s);
    PASS();
}

/* #874: the other multi-arg scalar functions work in WHERE too. */
TEST(cypher_issue874_where_substring) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE substring(f.name, 0, 6) = \"Handle\" RETURN f.name", "test", 0,
        &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #874: an unrecognised function in WHERE must fail loudly with the
 * supported set, not the misleading "unexpected operator". */
TEST(cypher_issue874_where_unsupported_func_error) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f:Function) WHERE foo(f.name) = \"x\" RETURN f.name", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT_TRUE(strstr(err, "unsupported function 'foo'") != NULL);
    free(err);
    PASS();
}

TEST(cypher_exists_no_callers) {
    /* NOT EXISTS { (f)<-[:CALLS]-() } → functions with no CALLS caller.
     * HandleOrder has only an incoming DEFINES edge (not CALLS), so it is the
     * sole match — proving EXISTS is edge-type-specific (in_degree=1 here). */
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE NOT EXISTS { (f)<-[:CALLS]-() } RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exists_has_outgoing_calls) {
    /* EXISTS { (f)-[:CALLS]->() } → functions that call something.
     * HandleOrder (→ValidateOrder, →LogError) and ValidateOrder (→SubmitOrder). */
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE EXISTS { (f)-[:CALLS]->() } RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_calls_relationship) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "RETURN f.name, g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* HandleOrder→ValidateOrder, HandleOrder→LogError, ValidateOrder→SubmitOrder */
    ASSERT_EQ(r.row_count, 3);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_calls_with_where) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WHERE f.name = \"HandleOrder\" "
                                "RETURN f.name, g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2); /* →ValidateOrder, →LogError */

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_inbound) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)<-[:CALLS]-(g:Function) "
                                "WHERE f.name = \"ValidateOrder\" "
                                "RETURN g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1); /* HandleOrder calls ValidateOrder */
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_count) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "RETURN f.name, COUNT(g) AS cnt",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* HandleOrder→2, ValidateOrder→1 */
    ASSERT_EQ(r.row_count, 2);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #58 (upstream PR 2241): an OPTIONAL MATCH with no match keeps the row with
 * the variable unbound; COUNT of that variable must not count the row. In the
 * fixture only HandleOrder has no CALLS caller. */
static void assert_optional_caller_counts(cbm_cypher_result_t *r, int *failed) {
    for (int i = 0; i < r->row_count; i++) {
        const char *want = strcmp(r->rows[i][0], "HandleOrder") == 0 ? "0" : "1";
        if (strcmp(r->rows[i][1], want) != 0) {
            printf("  %s: count %s, want %s\n", r->rows[i][0], r->rows[i][1], want);
            *failed = 1;
        }
    }
}

TEST(cypher_exec_optional_bound_terminal_count_ignores_unbound) {
    cbm_store_t *s = setup_cypher_store();
    static const char *const queries[] = {
        "MATCH (f:Function) OPTIONAL MATCH (t)-[:CALLS]->(f) RETURN f.name, COUNT(t) AS cnt",
        "MATCH (f:Function) OPTIONAL MATCH (t)-[:CALLS]->(f) RETURN f.name, COUNT(DISTINCT t)",
        "MATCH (f:Function) OPTIONAL MATCH (t)-[r:CALLS]->(f) RETURN f.name, count(r)",
        "MATCH (f:Function) OPTIONAL MATCH (t)-[:CALLS]->(f) RETURN f.name, count(t.name)",
        "MATCH (f:Function) OPTIONAL MATCH (t)-[:CALLS]->(f) "
        "WITH f, count(t) AS n RETURN f.name, n",
        "MATCH (f:Function) OPTIONAL MATCH (t)-[:CALLS]->(f) "
        "WITH f.name AS fname, count(DISTINCT t) AS n RETURN fname, n",
    };
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
        cbm_cypher_result_t r = {0};
        int rc = cbm_cypher_execute(s, queries[q], "test", 0, &r);
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(r.row_count, 4);
        int failed = 0;
        assert_optional_caller_counts(&r, &failed);
        cbm_cypher_result_free(&r);
        if (failed) {
            printf("  query: %s\n", queries[q]);
        }
        ASSERT_EQ(failed, 0);
    }

    /* COUNT(*) still counts every row, including the one with t unbound. */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = 'HandleOrder' "
                                "OPTIONAL MATCH (t)-[:CALLS]->(f) RETURN f.name, COUNT(*)",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][1], "1");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_limit) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name LIMIT 2", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_order_by) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name ORDER BY f.name ASC", "test",
                                0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4);
    /* Alphabetical: HandleOrder, LogError, SubmitOrder, ValidateOrder */
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT_STR_EQ(r.rows[1][0], "LogError");
    ASSERT_STR_EQ(r.rows[2][0], "SubmitOrder");
    ASSERT_STR_EQ(r.rows[3][0], "ValidateOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_variable_length) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    /* HandleOrder →CALLS→ ValidateOrder →CALLS→ SubmitOrder (2 hops) */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS*1..3]->(g:Function) "
                                "WHERE f.name = \"HandleOrder\" "
                                "RETURN g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* Should find: ValidateOrder (1 hop), SubmitOrder (2 hops), LogError (1 hop) */
    ASSERT_GTE(r.row_count, 3);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Reproduce-first (#887): an EXPLICIT variable-length upper bound must still be
 * capped at the engine ceiling (cbm_cypher_max_depth(), default 10). On
 * origin/main, expand_var_length honoured an explicit `*1..N` verbatim (only the
 * unbounded `*` / `*..m` forms were capped), so `[:CALLS*1..N]` passed N straight
 * to cbm_store_bfs — an unbounded traversal (a DoS on cyclic graphs). RED before
 * the clamp: a *1..12 walk over a 13-node chain
 * returns all 12 hops (N01..N12). GREEN after: it stops at the depth-10 ceiling
 * (N01..N10); N11/N12 are never emitted. max_rows=64 keeps the binding-expansion
 * cap (bind_cap*10) well above the hop count, so DEPTH — not the binding cap — is
 * the bound under test. */
TEST(cypher_exec_var_length_explicit_bound_capped) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    /* Linear chain N00 -CALLS-> N01 -> ... -> N12 (13 nodes, one node per hop). */
    int64_t ids[13];
    for (int i = 0; i < 13; i++) {
        char name[8];
        char qn[24];
        snprintf(name, sizeof(name), "N%02d", i);
        snprintf(qn, sizeof(qn), "test.N%02d", i);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "chain.go"};
        ids[i] = cbm_store_upsert_node(s, &n);
    }
    for (int i = 0; i < 12; i++) {
        cbm_edge_t e = {
            .project = "test", .source_id = ids[i], .target_id = ids[i + 1], .type = "CALLS"};
        cbm_store_insert_edge(s, &e);
    }

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (a:Function {name: \"N00\"})-[:CALLS*1..12]->"
                                "(x:Function) RETURN x.name",
                                "test", 64, &r);
    ASSERT_EQ(rc, 0);

    /* Capped at 10 hops → exactly N01..N10; N11/N12 are beyond the ceiling. */
    ASSERT_EQ(r.row_count, 10);
    bool saw_n10 = false;
    bool saw_n11 = false;
    bool saw_n12 = false;
    for (int i = 0; i < r.row_count; i++) {
        const char *v = r.rows[i][0];
        if (v && strcmp(v, "N10") == 0) {
            saw_n10 = true;
        }
        if (v && strcmp(v, "N11") == 0) {
            saw_n11 = true;
        }
        if (v && strcmp(v, "N12") == 0) {
            saw_n12 = true;
        }
    }
    ASSERT_TRUE(saw_n10);  /* within the ceiling — proves the traversal really ran */
    ASSERT_FALSE(saw_n11); /* clamped away */
    ASSERT_FALSE(saw_n12);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_var_length_no_reuse_self_loop) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_node_t n = {.project = "test",
                    .label = "Function",
                    .name = "Recursive",
                    .qualified_name = "test.Recursive",
                    .file_path = "recursive.go"};
    int64_t id = cbm_store_upsert_node(s, &n);
    cbm_edge_t e = {.project = "test", .source_id = id, .target_id = id, .type = "CALLS"};
    cbm_store_insert_edge(s, &e);

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function {name: \"Recursive\"})-[:CALLS*2..2]"
                                "->(g:Function) RETURN g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_var_length_truncation_surfaces_warning) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    int64_t ids[18];
    for (int i = 0; i < 18; i++) {
        char name[16];
        snprintf(name, sizeof(name), "node-%d", i);
        cbm_node_t node = {.project = "test",
                           .label = "Function",
                           .name = name,
                           .qualified_name = name,
                           .file_path = "graph.c"};
        ids[i] = cbm_store_upsert_node(s, &node);
    }
    for (int source = 0; source < 17; source++) {
        for (int target = source + 1; target < 18; target++) {
            cbm_edge_t edge = {.project = "test",
                               .source_id = ids[source],
                               .target_id = ids[target],
                               .type = "CALLS"};
            cbm_store_insert_edge(s, &edge);
        }
    }

    cbm_cypher_result_t result = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (a:Function {name: \"node-0\"})-[:CALLS*10..10]->"
                                "(b:Function) RETURN b.name",
                                "test", 0, &result);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(result.warning);
    ASSERT_TRUE(strstr(result.warning, "traversal budget") != NULL);
    ASSERT_TRUE(result.row_count > 0);

    cbm_cypher_result_free(&result);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_defines_edge) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (m:Module)-[:DEFINES]->(f:Function) "
                                "RETURN m.name, f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "main");
    ASSERT_STR_EQ(r.rows[0][1], "HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_no_results) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name = \"NonExistent\"", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_numeric) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.start_line > \"8\" "
                                "RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* HandleOrder starts at 10 */
    ASSERT_GTE(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* --- Ported from cypher_test.go: TestExecuteDistinct --- */
TEST(cypher_exec_distinct) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN DISTINCT f.label", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* All 4 Function nodes share label "Function" → 1 distinct row */
    ASSERT_EQ(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* issue #238: WITH DISTINCT must deduplicate projected rows (previously the
 * DISTINCT keyword on WITH was parsed but silently ignored). */
TEST(cypher_exec_with_distinct_issue238) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    /* 4 Function nodes all share label "Function" → WITH DISTINCT collapses to
     * one row; without dedup this returned 4. */
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WITH DISTINCT f.label AS lbl RETURN lbl",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);

    /* Control: without DISTINCT, all 4 rows flow through. */
    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(s, "MATCH (f:Function) WITH f.label AS lbl RETURN lbl", "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r2.row_count, 4);
    cbm_cypher_result_free(&r2);

    cbm_store_close(s);
    PASS();
}

/* issue #241: label tests in WHERE clauses (openCypher `WHERE n:Label`) —
 * previously a parse error. */
TEST(cypher_exec_where_label_test_issue241) {
    cbm_store_t *s = setup_cypher_store();

    /* f:Function is true for all 4 Function nodes. */
    cbm_cypher_result_t r = {0};
    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE f:Function RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4);
    cbm_cypher_result_free(&r);

    /* f:Class matches none of the functions. */
    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f:Class RETURN f.name", "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r2.row_count, 0);
    cbm_cypher_result_free(&r2);

    /* Negated label test: NOT f:Class is always true. */
    cbm_cypher_result_t r3 = {0};
    rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE NOT f:Class RETURN f.name", "test", 0, &r3);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r3.row_count, 4);
    cbm_cypher_result_free(&r3);

    cbm_store_close(s);
    PASS();
}

/* issue #239: COUNT(DISTINCT x) — previously a parse error. */
TEST(cypher_exec_count_distinct_issue239) {
    cbm_store_t *s = setup_cypher_store();

    /* 4 functions all share label "Function" → COUNT(DISTINCT f.label) = 1. */
    cbm_cypher_result_t r = {0};
    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) RETURN count(DISTINCT f.label)", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "1");
    cbm_cypher_result_free(&r);

    /* Non-distinct COUNT counts all 4 occurrences. */
    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN count(f.label)", "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(r2.rows[0][0], "4");
    cbm_cypher_result_free(&r2);

    /* DISTINCT over the 4 unique function names = 4. */
    cbm_cypher_result_t r3 = {0};
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN count(DISTINCT f.name)", "test", 0, &r3);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(r3.rows[0][0], "4");
    cbm_cypher_result_free(&r3);

    cbm_store_close(s);
    PASS();
}

/* issue #373: an unsupported computed expression in WITH/RETURN (an unknown
 * function like split(...) or list indexing [..]) must FAIL LOUDLY with a clear
 * "unsupported function" error rather than silently projecting an empty column
 * (which looks like a valid-but-blank result and hides the real problem). */
/* issue #1919: a variable the WITH clause dropped must be REFUSED, not projected
 * as an empty column. `g` does not survive `WITH f.name AS caller`, so naming it
 * afterwards is a query fault. The old code answered NULL for the unbound name,
 * rendered it "", and exited clean — a column of nothing that reads as "the graph
 * holds no such data" rather than "your query named something out of scope".
 * Same principle as #373: fail loudly instead of projecting a blank column. */
TEST(cypher_rejects_projection_of_dropped_with_var_issue1919) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function)-[:CALLS]->(g) WITH f.name AS caller RETURN caller, g.name", "test",
        0, &r);
    ASSERT_TRUE(rc != 0);
    ASSERT_NOT_NULL(r.error);
    /* The message has to name the offending variable — an error that does not
     * say which name is wrong sends the reader back to guessing. */
    ASSERT_TRUE(strstr(r.error, "g") != NULL);
    ASSERT_TRUE(strstr(r.error, "scope") != NULL);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* The guard must NOT reject an OPTIONAL MATCH target that simply did not match.
 * `g` is a declared pattern variable there, so it stays in scope; it is merely
 * unbound at run time, and projecting "" for it is the documented convention.
 * This is the line between the two cases, and the reason the check reads the
 * query's declared variables rather than the run-time bindings. */
TEST(cypher_optional_match_target_still_allowed_issue1919) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) OPTIONAL MATCH (f)-[:CALLS]->(g) RETURN f.name, g.name", "test", 0,
        &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_TRUE(r.row_count > 0);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* An alias the WITH created is in scope afterwards, and a name carried through
 * unchanged is too. Both must keep working. */
TEST(cypher_with_alias_stays_in_scope_issue1919) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function)-[:CALLS]->(g) WITH f.name AS caller, g AS callee RETURN caller, callee.name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_TRUE(r.row_count > 0);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_unsupported_func_errors_issue373) {
    cbm_store_t *s = setup_cypher_store();

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WITH split(f.name)[0] AS top, count(*) AS c RETURN top, c", "test",
        0, &r);
    ASSERT_TRUE(rc != 0); /* unsupported function now fails loudly */
    ASSERT_NOT_NULL(r.error);
    ASSERT_TRUE(strstr(r.error, "unsupported") != NULL);
    ASSERT_TRUE(strstr(r.error, "split") != NULL);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

/* A recognised function still works, and an unknown one in plain RETURN errors. */
TEST(cypher_exec_unknown_func_return_errors) {
    cbm_store_t *s = setup_cypher_store();

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN nosuchfunc(f.name)", "test", 0, &r);
    ASSERT_TRUE(rc != 0);
    ASSERT_NOT_NULL(r.error);
    ASSERT_TRUE(strstr(r.error, "unsupported function") != NULL);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

/* issue #242: openCypher label alternation in MATCH — (n:A|B). */
TEST(cypher_exec_label_alternation_issue242) {
    cbm_store_t *s = setup_cypher_store();

    /* Store has 4 Function + 1 Module node → alternation seeds all 5. */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (n:Function|Module) RETURN n.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 5);
    cbm_cypher_result_free(&r);

    /* Alternation with a non-existent label still returns the existing one. */
    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(s, "MATCH (n:Function|Class) RETURN n.name", "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r2.row_count, 4);
    cbm_cypher_result_free(&r2);

    cbm_store_close(s);
    PASS();
}

/* --- Ported from cypher_test.go: TestExecuteInlinePropertyFilter --- */
TEST(cypher_exec_inline_props) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function {name: \"SubmitOrder\"}) "
                                "RETURN f.name, f.qualified_name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* --- Ported from cypher_test.go: TestParseWhereStartsWith --- */
TEST(cypher_parse_where_starts_with) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f:Function) WHERE f.name STARTS WITH \"Send\" RETURN f", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);
    ASSERT_NOT_NULL(q->where);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, "STARTS WITH");
    ASSERT_STR_EQ(q->where->root->cond.value, "Send");
    cbm_query_free(q);
    PASS();
}

/* --- Ported from cypher_test.go: TestParseWhereContains --- */
TEST(cypher_parse_where_contains) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f:Function) WHERE f.name CONTAINS \"Handler\" RETURN f", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);
    ASSERT_NOT_NULL(q->where);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, "CONTAINS");
    ASSERT_STR_EQ(q->where->root->cond.value, "Handler");
    cbm_query_free(q);
    PASS();
}

/* --- Ported from cypher_test.go: TestParseWhereNumericComparison --- */
TEST(cypher_parse_where_numeric) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) WHERE f.start_line > 10 RETURN f", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);
    ASSERT_NOT_NULL(q->where);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, ">");
    ASSERT_STR_EQ(q->where->root->cond.value, "10");
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  EDGE PROPERTY TESTS (ported from cypher_test.go Feature 2)
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: set up store with HTTP_CALLS edge having properties.
 * Creates same graph as setup_cypher_store + one HTTP_CALLS edge. */
static cbm_store_t *setup_cypher_http_store(void) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "HandleOrder",
                     .qualified_name = "test.main.HandleOrder",
                     .file_path = "main.go",
                     .start_line = 10,
                     .end_line = 30};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "ValidateOrder",
                     .qualified_name = "test.service.ValidateOrder",
                     .file_path = "service.go",
                     .start_line = 5,
                     .end_line = 20};
    cbm_node_t n3 = {.project = "test",
                     .label = "Function",
                     .name = "SubmitOrder",
                     .qualified_name = "test.service.SubmitOrder",
                     .file_path = "service.go",
                     .start_line = 25,
                     .end_line = 50};

    int64_t id1 = cbm_store_upsert_node(s, &n1);
    cbm_store_upsert_node(s, &n2);
    int64_t id3 = cbm_store_upsert_node(s, &n3);

    cbm_edge_t http = {
        .project = "test",
        .source_id = id1,
        .target_id = id3,
        .type = "HTTP_CALLS",
        .properties_json =
            "{\"url_path\":\"/api/orders\",\"confidence\":0.85,\"method\":\"POST\","
            "\"args\":[{\"i\":0,\"e\":\"a, b]\"},{\"i\":1,\"e\":\"c\"}],\"line\":7}"};
    cbm_store_insert_edge(s, &http);

    return s;
}

/* Helper: set up store with TWO HTTP_CALLS edges for filtering tests. */
static cbm_store_t *setup_cypher_multi_edge_store(void) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "testproj", "/tmp/test");

    cbm_node_t n1 = {.project = "testproj",
                     .label = "Function",
                     .name = "SendOrder",
                     .qualified_name = "testproj.caller.SendOrder",
                     .file_path = "caller/client.go"};
    cbm_node_t n2 = {.project = "testproj",
                     .label = "Function",
                     .name = "HandleOrder",
                     .qualified_name = "testproj.handler.HandleOrder",
                     .file_path = "handler/routes.go"};
    cbm_node_t n3 = {.project = "testproj",
                     .label = "Function",
                     .name = "HandleHealth",
                     .qualified_name = "testproj.handler.HandleHealth",
                     .file_path = "handler/health.go"};

    int64_t id1 = cbm_store_upsert_node(s, &n1);
    int64_t id2 = cbm_store_upsert_node(s, &n2);
    int64_t id3 = cbm_store_upsert_node(s, &n3);

    cbm_edge_t e1 = {.project = "testproj",
                     .source_id = id1,
                     .target_id = id2,
                     .type = "HTTP_CALLS",
                     .properties_json =
                         "{\"url_path\":\"/api/orders\",\"confidence\":0.85,\"method\":\"POST\"}"};
    cbm_edge_t e2 = {.project = "testproj",
                     .source_id = id1,
                     .target_id = id3,
                     .type = "HTTP_CALLS",
                     .properties_json = "{\"url_path\":\"/health\",\"confidence\":0.45}"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);

    return s;
}

/* Helper: find a column value in a cypher result row */
static const char *cypher_get_col(const cbm_cypher_result_t *r, int row, const char *col) {
    for (int c = 0; c < r->col_count; c++) {
        if (strcmp(r->columns[c], col) == 0)
            return r->rows[row][c];
    }
    return NULL;
}

/* Helper: check if any row has a column matching a value */
static bool cypher_has_row_with(const cbm_cypher_result_t *r, const char *col, const char *val) {
    int ci = -1;
    for (int c = 0; c < r->col_count; c++) {
        if (strcmp(r->columns[c], col) == 0) {
            ci = c;
            break;
        }
    }
    if (ci < 0)
        return false;
    for (int row = 0; row < r->row_count; row++) {
        if (strcmp(r->rows[row][ci], val) == 0)
            return true;
    }
    return false;
}

TEST(cypher_edge_prop_access) {
    cbm_store_t *s = setup_cypher_http_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a:Function)-[r:HTTP_CALLS]->(b:Function) "
                                "RETURN a.name, b.name, r.url_path, r.confidence, r.args, r.line",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "a.name"), "HandleOrder");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "SubmitOrder");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.url_path"), "/api/orders");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.confidence"), "0.85");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.args"),
                  "[{\"i\":0,\"e\":\"a, b]\"},{\"i\":1,\"e\":\"c\"}]");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.line"), "7");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_prop_in_where) {
    cbm_store_t *s = setup_cypher_http_store();
    cbm_cypher_result_t r = {0};

    /* confidence > 0.8 → should match */
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.confidence > 0.8 "
                                "RETURN a.name, b.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);

    /* confidence > 0.9 → should NOT match */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.confidence > 0.9 "
                            "RETURN a.name",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_type_prop) {
    cbm_store_t *s = setup_cypher_http_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (a)-[r:HTTP_CALLS]->(b) RETURN r.type", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.type"), "HTTP_CALLS");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_contains) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.url_path CONTAINS 'orders' "
                                "RETURN a.name, b.name, r.url_path",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "a.name"), "SendOrder");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleOrder");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.url_path"), "/api/orders");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_numeric_gte) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.confidence >= 0.6 "
                                "RETURN a.name, b.name, r.confidence LIMIT 20",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_bare_edge_return_exposes_properties_json) {
    /* `RETURN r` on an edge variable, with no property accessor, should
     * surface the edge's full properties JSON (or "{}"). Before the fix,
     * binding_get_virtual returned an empty string, which made bare edge
     * returns useless for callers that wanted to inspect timestamps,
     * weights, etc. without naming each property up front. */
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.method = 'POST' RETURN r",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    const char *r_val = cypher_get_col(&r, 0, "r");
    ASSERT_NOT_NULL(r_val);
    /* Expect JSON object content rather than the previous empty string. */
    ASSERT_NOT_NULL(strstr(r_val, "url_path"));
    ASSERT_NOT_NULL(strstr(r_val, "/api/orders"));

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Edge property values pass through a ring of scratch buffers before they are
 * copied into a row. The ring was process-wide, so two queries on different
 * threads overwrote each other's values: each thread here reads only its own
 * url_path, and any other value in its rows came from the other thread. */
enum { EDGE_RACE_TARGETS = 64, EDGE_RACE_ROUNDS = 3000, EDGE_RACE_THREADS = 4 };

typedef struct {
    cbm_store_t *store;
    const char *url_path;
    int wrong;
} edge_race_run_t;

static cbm_store_t *setup_edge_race_store(const char *url_path) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "race", "/tmp/race");
    cbm_node_t src = {.project = "race",
                      .label = "Function",
                      .name = "Caller",
                      .qualified_name = "race.Caller",
                      .file_path = "caller.go"};
    int64_t src_id = cbm_store_upsert_node(s, &src);
    char props[128];
    snprintf(props, sizeof(props), "{\"url_path\":\"%s\"}", url_path);
    for (int i = 0; i < EDGE_RACE_TARGETS; i++) {
        char name[32];
        char qn[48];
        snprintf(name, sizeof(name), "Target%d", i);
        snprintf(qn, sizeof(qn), "race.Target%d", i);
        cbm_node_t dst = {.project = "race",
                          .label = "Function",
                          .name = name,
                          .qualified_name = qn,
                          .file_path = "target.go"};
        cbm_edge_t e = {.project = "race",
                        .source_id = src_id,
                        .target_id = cbm_store_upsert_node(s, &dst),
                        .type = "HTTP_CALLS",
                        .properties_json = props};
        cbm_store_insert_edge(s, &e);
    }
    return s;
}

static void *edge_race_thread(void *opaque) {
    edge_race_run_t *run = opaque;
    for (int round = 0; round < EDGE_RACE_ROUNDS; round++) {
        cbm_cypher_result_t r = {0};
        if (cbm_cypher_execute(run->store,
                               "MATCH (a)-[r:HTTP_CALLS]->(b) RETURN b.name, r.url_path", "race", 0,
                               &r) != 0 ||
            r.row_count != EDGE_RACE_TARGETS) {
            run->wrong++;
        }
        for (int row = 0; row < r.row_count; row++) {
            const char *got = cypher_get_col(&r, row, "r.url_path");
            if (!got || strcmp(got, run->url_path) != 0) {
                run->wrong++;
            }
        }
        cbm_cypher_result_free(&r);
    }
    return NULL;
}

TEST(cypher_edge_prop_concurrent_queries_isolated) {
    static const char *paths[EDGE_RACE_THREADS] = {
        "/thread-a/orders",
        "/thread-b/a-longer-health-check-path",
        "/thread-c/x",
        "/thread-d/users/profile/settings",
    };
    edge_race_run_t runs[EDGE_RACE_THREADS] = {0};
    cbm_thread_t threads[EDGE_RACE_THREADS];
    int started = 0;
    for (int i = 0; i < EDGE_RACE_THREADS; i++) {
        runs[i].url_path = paths[i];
        runs[i].store = setup_edge_race_store(paths[i]);
    }
    for (int i = 0; i < EDGE_RACE_THREADS; i++) {
        if (cbm_thread_create(&threads[i], 8 * 1024 * 1024, edge_race_thread, &runs[i]) == 0) {
            started++;
        }
    }
    for (int i = 0; i < started; i++) {
        (void)cbm_thread_join(&threads[i]);
    }
    int wrong = 0;
    for (int i = 0; i < EDGE_RACE_THREADS; i++) {
        wrong += runs[i].wrong;
        cbm_store_close(runs[i].store);
    }
    ASSERT_EQ(started, EDGE_RACE_THREADS);
    ASSERT_EQ(wrong, 0);
    PASS();
}

TEST(cypher_edge_return_without_filter) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) "
                                "RETURN a.name, b.name, r.url_path, r.confidence LIMIT 20",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.row_count, 2);
    ASSERT(cypher_has_row_with(&r, "r.url_path", "/api/orders"));
    ASSERT(cypher_has_row_with(&r, "r.url_path", "/health"));

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_equals) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.method = 'POST' "
                                "RETURN a.name, b.name",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_starts_with) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.url_path STARTS WITH '/api' "
                                "RETURN a.name, b.name, r.url_path",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.url_path"), "/api/orders");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_combined_node_and_edge_filter) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a:Function)-[r:HTTP_CALLS]->(b:Function) "
                                "WHERE a.name = 'SendOrder' AND r.confidence >= 0.6 "
                                "RETURN b.name, r.url_path",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleOrder");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.url_path"), "/api/orders");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_no_match) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    /* No edge has method = 'DELETE' */
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.method = 'DELETE' "
                                "RETURN a.name",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_numeric_lt) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    /* Only health edge (0.45) should match confidence < 0.5 */
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.confidence < 0.5 "
                                "RETURN b.name, r.confidence",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleHealth");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_regex) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.url_path =~ \"/api/.*\" "
                                "RETURN b.name",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_builtin_type_filter) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    /* Untyped rel [r] — filter on r.type in WHERE */
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r]->(b) WHERE r.type = 'HTTP_CALLS' "
                                "RETURN a.name, b.name LIMIT 20",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2); /* Both HTTP_CALLS edges */

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Ported from cypher_test.go: TestApplyLimitRespectsExplicit */
TEST(cypher_apply_limit) {
    /* Create store with many nodes */
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "lim", "/tmp/lim");

    for (int i = 0; i < 50; i++) {
        char name[32], qn[64];
        snprintf(name, sizeof(name), "func%d", i);
        snprintf(qn, sizeof(qn), "lim.func%d", i);
        cbm_node_t n = {.project = "lim",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "test.go"};
        cbm_store_upsert_node(s, &n);
    }

    /* LIMIT 5 → 5 rows */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name LIMIT 5", "lim", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 5);
    cbm_cypher_result_free(&r);

    /* No LIMIT, max_rows=10 → capped at 10 */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name", "lim", 10, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 10);
    cbm_cypher_result_free(&r);

    /* LIMIT above max_rows → explicit limit wins */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name LIMIT 30", "lim", 10, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 30);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 1: SIMPLE OPERATORS
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_lex_neq_operators) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("<> !=", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 2);
    ASSERT_EQ(r.tokens[0].type, TOK_NEQ);
    ASSERT_EQ(r.tokens[1].type, TOK_NEQ);
    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_ends_keyword) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("ENDS WITH", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 2);
    ASSERT_EQ(r.tokens[0].type, TOK_ENDS);
    ASSERT_EQ(r.tokens[1].type, TOK_WITH);
    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_in_is_null) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("IN IS NULL", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 3);
    ASSERT_EQ(r.tokens[0].type, TOK_IN);
    ASSERT_EQ(r.tokens[1].type, TOK_IS);
    ASSERT_EQ(r.tokens[2].type, TOK_NULL_KW);
    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_exec_where_neq) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name <> \"HandleOrder\" RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3); /* ValidateOrder, SubmitOrder, LogError */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_neq_bang) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name != \"HandleOrder\" RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_ends_with) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name ENDS WITH \"Order\" RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* HandleOrder, ValidateOrder, SubmitOrder */
    ASSERT_EQ(r.row_count, 3);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_not) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE NOT f.name = \"HandleOrder\" RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #58 (upstream PR 2253): the early WHERE pass runs on seed rows with only
 * the first alias bound, so it must leave any OR / XOR operand over a later
 * alias to the late pass. Pruning a seed on `a.name = ...` alone would drop
 * rows the other operand keeps. */
TEST(cypher_exec_where_mixed_alias_or) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s,
        "MATCH (a:Function)-[:CALLS]->(b:Function) "
        "WHERE a.name = \"NoSuchFunction\" OR NOT b.name CONTAINS \"Order\" RETURN b.name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "LogError");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_mixed_alias_xor) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc =
        cbm_cypher_execute(s,
                           "MATCH (a:Function)-[:CALLS]->(b:Function) "
                           "WHERE a.name = \"HandleOrder\" XOR b.name = \"LogError\" RETURN b.name",
                           "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "ValidateOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_in) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f) WHERE f.label IN [\"Function\", \"Module\"] RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 5); /* 4 Functions + 1 Module */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_not_in) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f) WHERE NOT f.label IN [\"Module\"] RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4); /* 4 Functions only */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_is_null) {
    /* SubmitOrder has no start_line (defaults to 0, so start_line prop = "0") */
    /* But file_path is set for all. Use a node with missing data. */
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "WithFile",
                     .qualified_name = "test.WithFile",
                     .file_path = "a.go"};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "NoFile",
                     .qualified_name = "test.NoFile",
                     .file_path = NULL};
    cbm_store_upsert_node(s, &n1);
    cbm_store_upsert_node(s, &n2);
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.file_path IS NULL RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1); /* NoFile has NULL file_path */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_is_not_null) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "WithFile",
                     .qualified_name = "test.WithFile",
                     .file_path = "a.go"};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "NoFile",
                     .qualified_name = "test.NoFile",
                     .file_path = NULL};
    cbm_store_upsert_node(s, &n1);
    cbm_store_upsert_node(s, &n2);
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.file_path IS NOT NULL RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1); /* WithFile */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_return_star) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN * LIMIT 3", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3);
    /* Should have columns: f.name, f.qualified_name, f.label, f.file_path */
    ASSERT_EQ(r.col_count, 4);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_return_star_dedups_repeated_pattern_var) {
    /* RETURN * collected its column variables from every pattern in turn and
     * never deduped, so a variable named in two patterns got its four columns
     * twice. Here f is named in the MATCH and again in the OPTIONAL MATCH, so
     * eight columns is right and twelve is the fault. */
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) OPTIONAL MATCH (f)-[:CALLS]->(g) RETURN *",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.col_count, 8);
    ASSERT_STR_EQ(r.columns[0], "f.name");
    ASSERT_STR_EQ(r.columns[4], "g.name");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_return_star_after_with_names_aliases) {
    /* RETURN * built its columns from the query pattern, never from the
     * bindings it was about to project. After a WITH the live scope is the
     * aliases the WITH made, so the old code asked for f and g, found neither,
     * and answered every value empty with no error. */
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g) "
                                "WITH f.name AS caller, g.name AS callee RETURN *",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.col_count, 2);
    ASSERT_STR_EQ(r.columns[0], "caller");
    ASSERT_STR_EQ(r.columns[1], "callee");
    /* Three CALLS edges in the fixture. */
    ASSERT_EQ(r.row_count, 3);
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_TRUE(r.rows[i][0][0] != '\0');
        ASSERT_TRUE(r.rows[i][1][0] != '\0');
    }
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_wide_with_refused_not_truncated) {
    /* Every item a WITH projects becomes one variable of the binding that
     * carries the rest of the query, and a binding holds CYP_MAX_VARS (16) of
     * them. A 20-alias WITH used to parse, drop aliases 17 to 20 inside
     * with_add_vbinding_var, and answer RETURN * with 16 columns and no error —
     * a short result the caller could not tell from a complete one. It has to
     * be refused at parse time instead. */
    char query[1024];
    int off = snprintf(query, sizeof(query), "MATCH (f:Function) WITH ");
    for (int i = 0; i < 20; i++) { /* 20 > CYP_MAX_VARS (16) */
        off += snprintf(query + off, sizeof(query) - (size_t)off, "%sf.name AS c%d", i ? ", " : "",
                        i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, " RETURN *");

    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, query, "test", 0, &r);
    ASSERT_TRUE(rc != 0); /* refused, not silently narrowed to 16 columns */
    cbm_cypher_result_free(&r);

    /* The width just under the bound still works, so the guard rejects only
     * what the binding genuinely cannot carry. */
    char ok_query[1024];
    off = snprintf(ok_query, sizeof(ok_query), "MATCH (f:Function) WITH ");
    for (int i = 0; i < 16; i++) {
        off += snprintf(ok_query + off, sizeof(ok_query) - (size_t)off, "%sf.name AS c%d",
                        i ? ", " : "", i);
    }
    snprintf(ok_query + off, sizeof(ok_query) - (size_t)off, " RETURN *");
    cbm_cypher_result_t r16 = {0};
    ASSERT_EQ(cbm_cypher_execute(s, ok_query, "test", 0, &r16), 0);
    ASSERT_EQ(r16.col_count, 16);
    cbm_cypher_result_free(&r16);
    cbm_store_close(s);
    PASS();
}

/* Build "MATCH (a0:NoSuchLabelXYZ)-[:CALLS]->(a1)-…->(aN-1)" into buf. The label
 * matches nothing, so any query built on it is instant and needs no fixture. */
static void build_node_chain(char *buf, size_t buf_sz, int nodes) {
    int off = snprintf(buf, buf_sz, "MATCH (a0:NoSuchLabelXYZ)");
    for (int i = 1; i < nodes; i++) {
        off += snprintf(buf + off, buf_sz - (size_t)off, "-[:CALLS]->(a%d)", i);
    }
}

TEST(cypher_wide_pattern_refused) {
    /* A binding holds CYP_MAX_VARS (16) node variables, and binding_set drops
     * the 17th without a word. The query then answers a column of empty strings
     * for every name it could not bind, which reads as "the graph holds no such
     * data". Refuse the query instead of answering it wrong. */
    cbm_store_t *s = setup_cypher_store();
    char query[2048];

    build_node_chain(query, sizeof(query), 20); /* 20 > CYP_MAX_VARS */
    strncat(query, " RETURN a0.name", sizeof(query) - strlen(query) - 1);
    cbm_cypher_result_t wide = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &wide) != 0);
    ASSERT_NOT_NULL(wide.error);
    ASSERT_TRUE(strstr(wide.error, "node") != NULL); /* says which limit was passed */
    cbm_cypher_result_free(&wide);

    /* The width right at the bound still runs, so the guard refuses only what a
     * binding genuinely cannot hold. */
    build_node_chain(query, sizeof(query), 16);
    strncat(query, " RETURN a0.name", sizeof(query) - strlen(query) - 1);
    cbm_cypher_result_t ok = {0};
    ASSERT_EQ(cbm_cypher_execute(s, query, "test", 0, &ok), 0);
    cbm_cypher_result_free(&ok);

    cbm_store_close(s);
    PASS();
}

TEST(cypher_wide_edge_pattern_refused) {
    /* Same shape on the edge table, where binding_set_edge stops at
     * CYP_MAX_EDGE_VARS (8). Only NAMED relationships take a slot. */
    cbm_store_t *s = setup_cypher_store();
    char query[2048];
    int off = snprintf(query, sizeof(query), "MATCH (a0:NoSuchLabelXYZ)");
    for (int i = 1; i <= 9; i++) { /* 9 > CYP_MAX_EDGE_VARS */
        off += snprintf(query + off, sizeof(query) - (size_t)off, "-[r%d:CALLS]->(a%d)", i, i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, " RETURN a0.name");
    cbm_cypher_result_t wide = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &wide) != 0);
    ASSERT_NOT_NULL(wide.error);
    ASSERT_TRUE(strstr(wide.error, "edge") != NULL);
    cbm_cypher_result_free(&wide);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_unnamed_head_takes_a_slot) {
    /* The head of the first pattern is bound whether the query names it or not:
     * execute_single falls back to the synthetic name "_n0". So an unnamed head
     * plus CYP_MAX_VARS (16) named nodes needs 17 slots and only 16 exist. Before
     * the fix, the capacity check counted names alone, let this query through,
     * and binding_set dropped the 16th name without a word — a0..a14 answered and
     * a15 came back empty. Refuse it instead. */
    cbm_store_t *s = setup_cypher_store();
    char query[2048];
    int off = snprintf(query, sizeof(query), "MATCH (:NoSuchLabelXYZ)");
    for (int i = 0; i < 16; i++) { /* 16 named + the unnamed head = 17 */
        off += snprintf(query + off, sizeof(query) - (size_t)off, "-[:CALLS]->(a%d)", i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, " RETURN a0.name");
    cbm_cypher_result_t wide = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &wide) != 0);
    ASSERT_NOT_NULL(wide.error);
    ASSERT_TRUE(strstr(wide.error, "node") != NULL);
    cbm_cypher_result_free(&wide);

    /* One name fewer fits exactly, so the guard still refuses only what a
     * binding genuinely cannot hold. */
    off = snprintf(query, sizeof(query), "MATCH (:NoSuchLabelXYZ)");
    for (int i = 0; i < 15; i++) {
        off += snprintf(query + off, sizeof(query) - (size_t)off, "-[:CALLS]->(a%d)", i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, " RETURN a0.name");
    cbm_cypher_result_t ok = {0};
    ASSERT_EQ(cbm_cypher_execute(s, query, "test", 0, &ok), 0);
    cbm_cypher_result_free(&ok);

    cbm_store_close(s);
    PASS();
}

TEST(cypher_scope_check_survives_wide_pattern) {
    /* Regression test for #1995. check_projection_scope models declared names in
     * a fixed array and used to skip the check entirely when a query declared
     * more than it held. So the same out-of-scope name was refused on a narrow
     * query and quietly accepted on a wide one. Both must now be refused. */
    cbm_store_t *s = setup_cypher_store();
    char query[4096];

    build_node_chain(query, sizeof(query), 10);
    strncat(query, " RETURN zzz.name", sizeof(query) - strlen(query) - 1);
    cbm_cypher_result_t narrow = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &narrow) != 0);
    ASSERT_NOT_NULL(narrow.error);
    ASSERT_TRUE(strstr(narrow.error, "zzz") != NULL);
    cbm_cypher_result_free(&narrow);

    /* 35 declared names — this one used to answer a zzz.name column of nothing. */
    build_node_chain(query, sizeof(query), 35);
    strncat(query, " RETURN zzz.name", sizeof(query) - strlen(query) - 1);
    cbm_cypher_result_t wide = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &wide) != 0);
    ASSERT_NOT_NULL(wide.error);
    cbm_cypher_result_free(&wide);

    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_neq) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) WHERE f.name <> \"X\"", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->where);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, "<>");
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_in) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f) WHERE f.label IN [\"Function\", \"Module\"]", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, "IN");
    ASSERT_EQ(q->where->root->cond.in_value_count, 2);
    ASSERT_STR_EQ(q->where->root->cond.in_values[0], "Function");
    ASSERT_STR_EQ(q->where->root->cond.in_values[1], "Module");
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_is_null) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f) WHERE f.file_path IS NULL", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_STR_EQ(q->where->root->cond.op, "IS NULL");
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 2: EXPRESSION TREE
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_where_or) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s,
        "MATCH (f:Function) WHERE f.name = \"HandleOrder\" OR f.name = \"LogError\" RETURN f.name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_complex_bool) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* (name CONTAINS "Order" OR name = "LogError") AND label = "Function" */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f) WHERE (f.name CONTAINS \"Order\" OR f.name = "
                                "\"LogError\") AND f.label = \"Function\" "
                                "RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4); /* HandleOrder, ValidateOrder, SubmitOrder, LogError */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_xor) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* name CONTAINS "Handle" XOR name CONTAINS "Order" → XOR = true when exactly one is true
     * HandleOrder: both true → false
     * ValidateOrder: false, true → true
     * SubmitOrder: false, true → true
     * LogError: false, false → false */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name CONTAINS \"Handle\" XOR f.name "
                                "CONTAINS \"Order\" RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2); /* ValidateOrder, SubmitOrder */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_not_prefix) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE NOT (f.name CONTAINS \"Order\") RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1); /* LogError */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_expr_tree_and_or) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f) WHERE f.a = \"1\" AND f.b = \"2\" OR f.c = \"3\"", &q, &err);
    ASSERT_EQ(rc, 0);
    /* Precedence: AND binds tighter than OR → root is OR */
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_OR);
    ASSERT_EQ(q->where->root->left->type, EXPR_AND);
    ASSERT_EQ(q->where->root->right->type, EXPR_CONDITION);
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_expr_tree_nested) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f) WHERE (f.a = \"1\" OR f.b = \"2\") AND f.c = \"3\"", &q, &err);
    ASSERT_EQ(rc, 0);
    /* Parens override precedence: root is AND, left is OR */
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_AND);
    ASSERT_EQ(q->where->root->left->type, EXPR_OR);
    ASSERT_EQ(q->where->root->right->type, EXPR_CONDITION);
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 3: UNSUPPORTED KEYWORD ERRORS
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_error_create) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("CREATE (n:Node {name: \"X\"})", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT(strstr(err, "CREATE") != NULL);
    free(err);
    PASS();
}

TEST(cypher_error_delete) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("DELETE n", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT(strstr(err, "DELETE") != NULL);
    free(err);
    PASS();
}

TEST(cypher_error_set) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("SET n.name = \"X\"", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT(strstr(err, "SET") != NULL);
    free(err);
    PASS();
}

TEST(cypher_error_merge) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MERGE (n:Node)", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT(strstr(err, "MERGE") != NULL);
    free(err);
    PASS();
}

TEST(cypher_error_call) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("CALL db.labels()", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT(strstr(err, "CALL") != NULL);
    free(err);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 4: SKIP + GENERALIZED AGGREGATION
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_skip) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name ORDER BY f.name ASC SKIP 2",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* 4 functions ordered: HandleOrder, LogError, SubmitOrder, ValidateOrder → skip 2 = 2 */
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "SubmitOrder");
    ASSERT_STR_EQ(r.rows[1][0], "ValidateOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_skip_limit) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) RETURN f.name ORDER BY f.name ASC SKIP 1 LIMIT 2", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "LogError");
    ASSERT_STR_EQ(r.rows[1][0], "SubmitOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Regression for #1334: a LIMIT must survive a multi-key ORDER BY. The parser
 * used to stop at the first sort key, leaving ", key2 ... LIMIT n" unconsumed —
 * the whole result set came back instead of the limited one. */
TEST(cypher_exec_multikey_order_by_keeps_limit_issue1334) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* start_lines: HandleOrder=10, ValidateOrder=5, SubmitOrder=0, LogError=0 */
    int rc = cbm_cypher_execute(
        s,
        "MATCH (f:Function) RETURN f.name, f.start_line "
        "ORDER BY f.start_line DESC, f.name ASC LIMIT 2",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT_STR_EQ(r.rows[1][0], "ValidateOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #1334: the secondary key must actually break ties, per-key direction. */
TEST(cypher_exec_multikey_order_by_tiebreak_issue1334) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* start_line ASC puts the two 0-line functions first; name DESC breaks the
     * tie: SubmitOrder before LogError. */
    int rc = cbm_cypher_execute(
        s,
        "MATCH (f:Function) RETURN f.name, f.start_line "
        "ORDER BY f.start_line ASC, f.name DESC LIMIT 2",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "SubmitOrder");
    ASSERT_STR_EQ(r.rows[1][0], "LogError");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #1334: the WITH-clause pipeline has the same multi-key ORDER BY contract. */
TEST(cypher_exec_with_multikey_order_by_keeps_limit_issue1334) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WITH f.name AS n, f.start_line AS sl "
                                "ORDER BY sl DESC, n ASC LIMIT 2 RETURN n",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT_STR_EQ(r.rows[1][0], "ValidateOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_sum) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* start_lines: HandleOrder=10, ValidateOrder=5, SubmitOrder=0, LogError=0 → sum=15 */
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN SUM(f.start_line) AS total", "test",
                                0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "15");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_avg) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* start_lines: 10, 5, 0, 0 → avg = 3.75 */
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN AVG(f.start_line) AS avg_line",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "3.75");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_min) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* Among functions with nonzero: HandleOrder=10, ValidateOrder=5 → but MIN is 0 from others */
    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) RETURN MIN(f.start_line) AS mn", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "0");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_max) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) RETURN MAX(f.start_line) AS mx", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "10");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_collect) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WHERE f.name = \"HandleOrder\" "
                                "RETURN f.name, COLLECT(g.name) AS callees",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    /* Should be a JSON array like ["ValidateOrder","LogError"] */
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT(strstr(r.rows[0][1], "ValidateOrder") != NULL);
    ASSERT(strstr(r.rows[0][1], "LogError") != NULL);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_count_star) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN COUNT(*) AS n", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "4");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_skip) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f) RETURN f.name SKIP 5 LIMIT 10", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->ret);
    ASSERT_EQ(q->ret->skip, 5);
    ASSERT_EQ(q->ret->limit, 10);
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_sum_avg) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f) RETURN SUM(f.x) AS s, AVG(f.y) AS a", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->count, 2);
    ASSERT_STR_EQ(q->ret->items[0].func, "SUM");
    ASSERT_STR_EQ(q->ret->items[0].alias, "s");
    ASSERT_STR_EQ(q->ret->items[1].func, "AVG");
    ASSERT_STR_EQ(q->ret->items[1].alias, "a");
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_collect) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f)-[:CALLS]->(g) RETURN f.name, COLLECT(g.name) AS names", &q,
                              &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->count, 2);
    ASSERT_STR_EQ(q->ret->items[1].func, "COLLECT");
    ASSERT_STR_EQ(q->ret->items[1].alias, "names");
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 5: STRING FUNCTIONS + CASE
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_tolower) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN toLower(f.name) AS lower_name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "handleorder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_toupper) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN toUpper(f.name) AS upper_name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HANDLEORDER");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_tostring) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN toString(f.start_line) AS sl",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "10");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_case) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s,
        "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
        "RETURN CASE WHEN f.start_line > \"5\" THEN \"high\" ELSE \"low\" END AS pos",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "high");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_tolower) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f) RETURN toLower(f.name) AS n", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(q->ret->items[0].func, "toLower");
    ASSERT_STR_EQ(q->ret->items[0].alias, "n");
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_case) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(
        "MATCH (f) RETURN CASE WHEN f.x = \"1\" THEN \"a\" ELSE \"b\" END AS val", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->ret->items[0].kase);
    ASSERT_EQ(q->ret->items[0].kase->branch_count, 1);
    ASSERT_STR_EQ(q->ret->items[0].kase->branches[0].then_val, "a");
    ASSERT_STR_EQ(q->ret->items[0].kase->else_val, "b");
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 6: WITH CLAUSE
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_with_rename) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "WITH f.name AS fname RETURN fname",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #57: a node carried IN SCOPE through WITH must project every property
 * exactly as the same node does without the WITH. The carried var used to be a
 * name-only stub whose qualified_name slot held the variable name and whose
 * start_line was 0, so f.qualified_name returned "f" and f.start_line "0". */
static int64_t issue57_id_of(cbm_store_t *s, const char *qn) {
    cbm_node_t n = {0};
    int64_t id = 0;
    if (cbm_store_find_node_by_qn(s, "test", qn, &n) == CBM_STORE_OK) {
        id = n.id;
        cbm_node_free_fields(&n);
    }
    return id;
}

static cbm_store_t *setup_issue57_store(void) {
    cbm_store_t *s = setup_cypher_store();
    cbm_node_t twins[] = {
        {.project = "test",
         .label = "Function",
         .name = "ProjectionTwin",
         .qualified_name = "test.ProjectionA",
         .file_path = "projection_a.go",
         .start_line = 30,
         .end_line = 40,
         .properties_json = "{\"marker\":\"alpha\"}"},
        {.project = "test",
         .label = "Function",
         .name = "ProjectionTwin",
         .qualified_name = "test.ProjectionB",
         .file_path = "projection_b.go",
         .start_line = 10,
         .end_line = 20,
         .properties_json = "{\"marker\":\"beta\"}"},
        {.project = "test",
         .label = "Function",
         .name = "ProjectionTwin",
         .qualified_name = "test.ProjectionC",
         .file_path = "projection_c.go",
         .start_line = 20,
         .end_line = 30,
         .properties_json = "{\"marker\":\"gamma\"}"},
    };
    int64_t ids[3];
    for (int i = 0; i < 3; i++) {
        ids[i] = cbm_store_upsert_node(s, &twins[i]);
    }
    /* Callers: A has 2 (HandleOrder, ValidateOrder), B has 1 (HandleOrder), C none. */
    int64_t handle = issue57_id_of(s, "test.HandleOrder");
    int64_t validate = issue57_id_of(s, "test.ValidateOrder");
    cbm_edge_t e1 = {.project = "test", .source_id = handle, .target_id = ids[0], .type = "CALLS"};
    cbm_edge_t e2 = {
        .project = "test", .source_id = validate, .target_id = ids[0], .type = "CALLS"};
    cbm_edge_t e3 = {.project = "test", .source_id = handle, .target_id = ids[1], .type = "CALLS"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);
    cbm_store_insert_edge(s, &e3);
    return s;
}

/* The reporter's exact shape: aggregate WITH carrying the node beside count(). */
TEST(cypher_issue57_with_agg_carried_node_props) {
    cbm_store_t *s = setup_issue57_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function {qualified_name: 'test.ProjectionA'}) "
                                "OPTIONAL MATCH (caller)-[r:CALLS]->(f) "
                                "WITH f, count(r) AS refs "
                                "RETURN f.qualified_name, f.file_path, f.start_line, "
                                "f.end_line, f.label, f.marker, f.name, refs",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "test.ProjectionA");
    ASSERT_STR_EQ(r.rows[0][1], "projection_a.go");
    ASSERT_STR_EQ(r.rows[0][2], "30");
    ASSERT_STR_EQ(r.rows[0][3], "40");
    ASSERT_STR_EQ(r.rows[0][4], "Function");
    ASSERT_STR_EQ(r.rows[0][5], "alpha");
    ASSERT_STR_EQ(r.rows[0][6], "ProjectionTwin");
    ASSERT_STR_EQ(r.rows[0][7], "2");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Aggregate WITH groups by node IDENTITY, not display name: three same-named
 * functions stay three rows, each with its own properties and caller count
 * (they used to collapse into one row with one node's props and summed refs). */
TEST(cypher_issue57_with_agg_groups_by_node_identity) {
    cbm_store_t *s = setup_issue57_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function {name: 'ProjectionTwin'}) "
                                "OPTIONAL MATCH (caller)-[r:CALLS]->(f) "
                                "WITH f, count(r) AS refs "
                                "RETURN f.qualified_name, f.start_line, refs "
                                "ORDER BY f.qualified_name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3);
    ASSERT_STR_EQ(r.rows[0][0], "test.ProjectionA");
    ASSERT_STR_EQ(r.rows[0][1], "30");
    ASSERT_STR_EQ(r.rows[0][2], "2");
    ASSERT_STR_EQ(r.rows[1][0], "test.ProjectionB");
    ASSERT_STR_EQ(r.rows[1][1], "10");
    ASSERT_STR_EQ(r.rows[1][2], "1");
    ASSERT_STR_EQ(r.rows[2][0], "test.ProjectionC");
    ASSERT_STR_EQ(r.rows[2][1], "20");
    ASSERT_STR_EQ(r.rows[2][2], "0"); /* caller-less: r is unbound (#58) */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Simple (non-aggregate) WITH: a bare node stays a node (renamed or not);
 * scalar items stay scalars; OPTIONAL null stays null; post-WITH WHERE and
 * WITH ... ORDER BY see the real node properties. */
TEST(cypher_issue57_with_simple_carried_node_props) {
    cbm_store_t *s = setup_issue57_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.qualified_name = 'test.ProjectionA' "
                                "WITH f "
                                "RETURN f.name, f.qualified_name, f.file_path, f.label, "
                                "f.start_line, f.marker",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "ProjectionTwin");
    ASSERT_STR_EQ(r.rows[0][1], "test.ProjectionA");
    ASSERT_STR_EQ(r.rows[0][2], "projection_a.go");
    ASSERT_STR_EQ(r.rows[0][3], "Function");
    ASSERT_STR_EQ(r.rows[0][4], "30");
    ASSERT_STR_EQ(r.rows[0][5], "alpha");
    cbm_cypher_result_free(&r);

    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE f.qualified_name = 'test.ProjectionA' "
                            "WITH f AS g, f.name AS n, labels(f) AS ls "
                            "RETURN g.qualified_name, g.file_path, n, ls, n.file_path, "
                            "ls.file_path",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "test.ProjectionA");
    ASSERT_STR_EQ(r.rows[0][1], "projection_a.go");
    ASSERT_STR_EQ(r.rows[0][2], "ProjectionTwin");
    ASSERT_STR_EQ(r.rows[0][3], "[\"Function\"]");
    ASSERT_STR_EQ(r.rows[0][4], "");
    ASSERT_STR_EQ(r.rows[0][5], "");
    cbm_cypher_result_free(&r);

    /* OPTIONAL null carried through WITH stays null (no fabricated node). */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE f.name = 'LogError' "
                            "OPTIONAL MATCH (f)-[:CALLS]->(g:Function) "
                            "WITH g AS projected "
                            "RETURN projected, projected.file_path, projected.qualified_name",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "");
    ASSERT_STR_EQ(r.rows[0][1], "");
    ASSERT_STR_EQ(r.rows[0][2], "");
    cbm_cypher_result_free(&r);

    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WITH f AS g "
                            "WHERE g.file_path = 'projection_b.go' "
                            "RETURN g.qualified_name",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "test.ProjectionB");
    cbm_cypher_result_free(&r);

    /* A(30), B(10), C(20): ascending then SKIP 1 LIMIT 1 selects C. */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE f.name = 'ProjectionTwin' "
                            "WITH f AS g ORDER BY g.start_line ASC SKIP 1 LIMIT 1 "
                            "RETURN g.qualified_name, g.start_line",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "test.ProjectionC");
    ASSERT_STR_EQ(r.rows[0][1], "20");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #57: id(), keys() and labels() read the carried node, and a scalar WITH
 * alias exposes no fields of its own: its name used to sit in qualified_name,
 * so nm.qualified_name returned "nm" and keys(nm) listed "qualified_name". */
TEST(cypher_issue57_with_carried_id_and_scalar_alias) {
    cbm_store_t *s = setup_issue57_store();
    char want_id[32];
    snprintf(want_id, sizeof(want_id), "%lld", (long long)issue57_id_of(s, "test.ProjectionB"));
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.qualified_name = 'test.ProjectionB' "
                                "WITH f RETURN id(f), labels(f), f.end_line",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], want_id);
    ASSERT_STR_EQ(r.rows[0][1], "[\"Function\"]");
    ASSERT_STR_EQ(r.rows[0][2], "20");
    cbm_cypher_result_free(&r);

    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE f.qualified_name = 'test.ProjectionB' "
                            "WITH f.name AS nm RETURN nm, nm.qualified_name, keys(nm)",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "ProjectionTwin");
    ASSERT_STR_EQ(r.rows[0][1], "");
    ASSERT_NULL(strstr(r.rows[0][2], "qualified_name"));
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #57: a function item grouped beside an aggregate stays a scalar. The group
 * used to tag it with the node's id, so l.file_path re-fetched f's file. */
TEST(cypher_issue57_with_agg_func_item_not_a_node) {
    cbm_store_t *s = setup_issue57_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function {name: 'ProjectionTwin'}) "
                                "WITH labels(f) AS l, count(*) AS c "
                                "RETURN l, l.file_path, c",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "[\"Function\"]");
    ASSERT_STR_EQ(r.rows[0][1], "");
    ASSERT_STR_EQ(r.rows[0][2], "3");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #57: WITH ... ORDER BY on a carried node's JSON string property sorts more
 * rows than node_prop has rotating buffers (8). Each value is copied before
 * the sort; keeping the buffer pointers would compare overwritten strings. */
TEST(cypher_issue57_with_order_by_carried_prop_many_rows) {
    cbm_store_t *s = setup_cypher_store();
    static const int lines[] = {107, 103, 111, 101, 109, 105, 112, 102, 110, 104, 108, 106};
    enum { N = (int)(sizeof(lines) / sizeof(lines[0])) };
    for (int i = 0; i < N; i++) {
        char qn[64];
        char props[64];
        snprintf(qn, sizeof(qn), "test.OrderTwin%d", i);
        snprintf(props, sizeof(props), "{\"rank\":\"r%d\"}", lines[i]);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = "OrderTwin",
                        .qualified_name = qn,
                        .file_path = "order.go",
                        .start_line = lines[i],
                        .end_line = lines[i] + 1,
                        .properties_json = props};
        cbm_store_upsert_node(s, &n);
    }
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = 'OrderTwin' "
                                "WITH f AS g ORDER BY g.rank DESC "
                                "RETURN g.rank, g.start_line",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, N);
    for (int i = 0; i < N; i++) {
        char want[16];
        snprintf(want, sizeof(want), "r%d", 112 - i);
        ASSERT_STR_EQ(r.rows[i][0], want);
        snprintf(want, sizeof(want), "%d", 112 - i);
        ASSERT_STR_EQ(r.rows[i][1], want);
    }
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_with_count) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WITH f.name AS caller, COUNT(g) AS cnt "
                                "RETURN caller, cnt ORDER BY cnt DESC",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.row_count, 1);
    /* HandleOrder calls 2 (ValidateOrder, LogError), ValidateOrder calls 1 (SubmitOrder) */
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT_STR_EQ(r.rows[0][1], "2");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Regression: a bare node group-var carried through WITH aggregation must project
 * its real properties (not blank). Pre-fix, the carried var held only the node
 * name, so RETURN g.file_path returned "". */
TEST(cypher_exec_with_node_groupvar_prop) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WHERE g.name = \"ValidateOrder\" "
                                "WITH g, COUNT(*) AS c "
                                "RETURN g.file_path, g.name, c",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "validate.go"); /* was "" before the fix */
    ASSERT_STR_EQ(r.rows[0][1], "ValidateOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_with_where) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WITH f.name AS caller, COUNT(g) AS cnt "
                                "WHERE cnt > \"1\" "
                                "RETURN caller, cnt",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* Only HandleOrder has cnt > 1 (cnt=2) */
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_with_orderby_limit) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WITH f.name AS caller, COUNT(g) AS cnt "
                                "ORDER BY cnt DESC LIMIT 1 "
                                "RETURN caller, cnt",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_with) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(
        "MATCH (f)-[:CALLS]->(g) WITH f.name AS caller, COUNT(g) AS cnt RETURN caller, cnt", &q,
        &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->with_clause);
    ASSERT_EQ(q->with_clause->count, 2);
    ASSERT_STR_EQ(q->with_clause->items[0].alias, "caller");
    ASSERT_STR_EQ(q->with_clause->items[1].func, "COUNT");
    ASSERT_NOT_NULL(q->ret);
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_with_where) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f)-[:CALLS]->(g) WITH f.name AS caller, COUNT(g) AS cnt "
                              "WHERE cnt > \"1\" RETURN caller",
                              &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->with_clause);
    ASSERT_NOT_NULL(q->post_with_where);
    ASSERT_NOT_NULL(q->post_with_where->root);
    ASSERT_NOT_NULL(q->ret);
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 7: OPTIONAL MATCH + MULTIPLE MATCH
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_optional_match_no_result) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* LogError has no CALLS outbound edges → OPTIONAL MATCH keeps binding with empty target */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"LogError\" "
                                "OPTIONAL MATCH (f)-[:CALLS]->(g:Function) "
                                "RETURN f.name, g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "LogError");
    /* g.name should be empty since OPTIONAL MATCH found nothing */
    ASSERT_STR_EQ(r.rows[0][1], "");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_optional_match_has_result) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "OPTIONAL MATCH (f)-[:CALLS]->(g:Function) "
                                "RETURN f.name, g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2); /* ValidateOrder, LogError */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_multi_match) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* Two MATCH clauses: first finds a module, second finds functions */
    int rc =
        cbm_cypher_execute(s,
                           "MATCH (m:Module) MATCH (f:Function) WHERE f.name CONTAINS \"Order\" "
                           "RETURN m.name, f.name",
                           "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* 1 module × 3 *Order functions = 3 */
    ASSERT_EQ(r.row_count, 3);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_optional_match) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(
        "MATCH (f:Function) OPTIONAL MATCH (f)-[:CALLS]->(g) RETURN f.name, g.name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->pattern_count, 2);
    ASSERT(!q->pattern_optional[0]);
    ASSERT(q->pattern_optional[1]);
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_multi_match) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (a:Module) MATCH (b:Function) RETURN a.name, b.name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->pattern_count, 2);
    ASSERT(!q->pattern_optional[0]);
    ASSERT(!q->pattern_optional[1]);
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 8: UNION / UNION ALL
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_union) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN f.name "
                                "UNION "
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* UNION deduplicates → 1 row */
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_union_all) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN f.name "
                                "UNION ALL "
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* UNION ALL keeps duplicates → 2 rows */
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_union) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f) RETURN f.name UNION ALL MATCH (g) RETURN g.name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT(q->union_all);
    ASSERT_NOT_NULL(q->union_next);
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 9: UNWIND
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_parse_unwind) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("UNWIND [\"a\", \"b\", \"c\"] AS x MATCH (f) RETURN f.name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->unwind_expr);
    ASSERT_STR_EQ(q->unwind_alias, "x");
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_unwind_var) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("UNWIND items AS item MATCH (f) RETURN f.name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(q->unwind_expr, "items");
    ASSERT_STR_EQ(q->unwind_alias, "item");
    cbm_query_free(q);
    PASS();
}

/* ── Issue #389 group: Cypher feature reproductions ─────────────────
 * Each asserts the CORRECT behavior; a failure reproduces the bug. */

/* #240: labels() function */
TEST(cypher_issue240_labels_function) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (n:Module) RETURN labels(n) AS lbl", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #237: DISTINCT applied before ORDER BY + LIMIT */
TEST(cypher_issue237_distinct_order_limit) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) RETURN DISTINCT f.label AS l ORDER BY l LIMIT 10", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Upstream 6c7fe78e (#873): duplicate projected rows must be deduped before
 * ORDER BY + LIMIT. Pre-fix, LIMIT truncated first: 4 Function rows ordered
 * first, LIMIT 2 kept two identical rows, DISTINCT collapsed them to 1. */
TEST(cypher_issue873_distinct_order_limit_dedupes_before_limit) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (n) RETURN DISTINCT n.label AS label ORDER BY label LIMIT 2", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "Function");
    ASSERT_STR_EQ(r.rows[1][0], "Module");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #873: the projection fast path must not truncate rows before DISTINCT for
 * a simple DISTINCT ... LIMIT with no ORDER BY. */
TEST(cypher_issue873_distinct_limit_dedupes_before_limit) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (n) RETURN DISTINCT n.label AS label LIMIT 2", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #873: SKIP is applied after DISTINCT and ORDER BY, not before dedupe. */
TEST(cypher_issue873_distinct_order_skip_limit_dedupes_before_skip) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (n) RETURN DISTINCT n.label AS label ORDER BY label SKIP 1 LIMIT 1", "test",
        0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "Module");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #58 (upstream PR 2511): RETURN DISTINCT deduplicated only after the engine
 * row cap, so a value first seen past the cap was lost. The cap must count
 * distinct rows. Fixture: 20 CALLS edges, then one HAS_BRANCH edge from the
 * last-inserted node; max_rows=5 puts the rare type far past the raw-row cap. */
TEST(cypher_return_distinct_counts_distinct_rows_against_cap) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_upsert_project(s, "test", "/tmp/test"), CBM_STORE_OK);

    int64_t ids[22];
    for (int i = 0; i < 22; i++) {
        char name[32];
        char qn[64];
        snprintf(name, sizeof(name), "fn_%02d", i);
        snprintf(qn, sizeof(qn), "test.%s", name);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "a.py"};
        ids[i] = cbm_store_upsert_node(s, &n);
        ASSERT_GT(ids[i], 0);
    }
    for (int i = 0; i < 20; i++) {
        cbm_edge_t e = {
            .project = "test", .source_id = ids[i], .target_id = ids[i + 1], .type = "CALLS"};
        cbm_store_insert_edge(s, &e);
    }
    cbm_edge_t rare = {
        .project = "test", .source_id = ids[21], .target_id = ids[0], .type = "HAS_BRANCH"};
    cbm_store_insert_edge(s, &rare);

    /* Plain DISTINCT and RETURN DISTINCT * after WITH: both types. */
    static const char *const both[] = {
        "MATCH ()-[r]->() RETURN DISTINCT type(r) AS t",
        "MATCH ()-[r]->() WITH type(r) AS t RETURN DISTINCT *",
    };
    for (size_t q = 0; q < sizeof(both) / sizeof(both[0]); q++) {
        cbm_cypher_result_t r = {0};
        int rc = cbm_cypher_execute(s, both[q], "test", 5, &r);
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(r.row_count, 2);
        bool saw_calls = false;
        bool saw_branch = false;
        for (int i = 0; i < r.row_count; i++) {
            saw_calls = saw_calls || strcmp(r.rows[i][0], "CALLS") == 0;
            saw_branch = saw_branch || strcmp(r.rows[i][0], "HAS_BRANCH") == 0;
        }
        ASSERT_TRUE(saw_calls);
        ASSERT_TRUE(saw_branch);
        cbm_cypher_result_free(&r);
    }

    /* DISTINCT + ORDER BY shares the cap. */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH ()-[r]->() RETURN DISTINCT type(r) AS t ORDER BY t DESC",
                                "test", 5, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "HAS_BRANCH");
    ASSERT_STR_EQ(r.rows[1][0], "CALLS");
    cbm_cypher_result_free(&r);

    /* DISTINCT + ORDER BY + SKIP/LIMIT: SKIP counts distinct rows. */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(
        s, "MATCH ()-[r]->() RETURN DISTINCT type(r) AS t ORDER BY t SKIP 1 LIMIT 1", "test", 5,
        &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HAS_BRANCH");
    cbm_cypher_result_free(&r);

    /* An explicit LIMIT on DISTINCT counts distinct rows too. */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH ()-[r]->() RETURN DISTINCT type(r) AS t LIMIT 2", "test", 5,
                            &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);

    /* More distinct rows than the cap: still bounded by it. */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH (a)-[r]->(b) RETURN DISTINCT b.name", "test", 5, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 5);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

/* Upstream 48fa9cbc: a string property must not end at an escaped quote, and
 * an array property keeps its internal commas. */
TEST(cypher_exec_prop_composite_and_escaped_values) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t n = {.project = "test",
                    .label = "Method",
                    .name = "findAll",
                    .qualified_name = "test.C.findAll",
                    .file_path = "c.ts",
                    .properties_json = "{\"decorators\":[\"@Roles('OWNER', 'ADMIN')\",\"@Get()\"],"
                                       "\"signature\":\"(sep: \\\"a,b\\\") => void\",\"lines\":3}"};
    cbm_store_upsert_node(s, &n);
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (m:Method) RETURN m.decorators, m.signature, m.lines", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "[\"@Roles('OWNER', 'ADMIN')\",\"@Get()\"]");
    ASSERT_STR_EQ(r.rows[0][1], "(sep: \\\"a,b\\\") => void");
    ASSERT_STR_EQ(r.rows[0][2], "3");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Upstream b43bdddc (#1111): type(r) grouped with count(*) must return the actual relationship type,
 * not the row count. ret_agg_build_key/ret_agg_emit_row classified aggregate vs.
 * scalar columns with a bare `item->func` truthy check, so type(r) (a non-aggregate
 * function, func != NULL) was misrouted into the aggregate-value branch and
 * formatted via format_agg_value's default case, silently substituting the row
 * count for the relationship type. */
TEST(cypher_issue1111_return_type_count_group) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (a)-[r]->(b) RETURN type(r) AS t, count(*) AS n ORDER BY n DESC", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "CALLS");
    ASSERT_STR_EQ(r.rows[0][1], "3");
    ASSERT_STR_EQ(r.rows[1][0], "DEFINES");
    ASSERT_STR_EQ(r.rows[1][1], "1");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #1111, WITH variant: the same misrouting in with_agg_build_key/with_agg_accumulate/
 * execute_with_aggregate's per-column func check. */
TEST(cypher_issue1111_with_type_count_group) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s,
        "MATCH (a)-[r]->(b) WITH type(r) AS t, count(*) AS n RETURN t, n ORDER BY n DESC",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "CALLS");
    ASSERT_STR_EQ(r.rows[0][1], "3");
    ASSERT_STR_EQ(r.rows[1][0], "DEFINES");
    ASSERT_STR_EQ(r.rows[1][1], "1");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}


/* Upstream ec29cd59: a repeated variable-length node variable unifies with its
 * earlier binding; the fixture has no CALLS cycle, so nothing matches. */
TEST(cypher_exec_variable_length_repeated_node_var_unifies) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function)-[:CALLS*1..2]->(f:Function) RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 0);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Upstream 27d9831a. Reproduce-first: after the expansion budget is exhausted, OPTIONAL MATCH must
 * not FABRICATE a "no match" row for a source that genuinely has matches.
 *
 * process_edges / expand_var_length used to carry the budget in the LOOP
 * condition (`ei < edge_count && *new_count < max_new`), so once new_count hit
 * max_new they stopped iterating entirely and never incremented match_count —
 * even though neighbours existed. expand_pattern_rels' ungated fallback then saw
 * match_count == 0 and emitted an unbound row. `WHERE b IS NULL` reads that as
 * "nothing points here", so a dead-code query reported LIVE code as dead.
 *
 * Shape: A saturates the budget, B genuinely has no callees, C has 5. Only B may
 * appear with an unbound b. Asserting on C specifically is what discriminates —
 * the pre-fix code emits C with an empty b, and a `row_count` check would not
 * notice. Deterministic: insertion order fixes the scan order (rowid), and every
 * count is exact. */
TEST(cypher_exec_optional_saturated_does_not_fabricate_no_match) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    /* 3 Function nodes → scan_count = 3; max_rows = 3 → bind_cap = 3 and
     * max_new = 30. A alone exceeds that, so B and C are reached with the
     * budget already spent — the regime that produced the fabrication. */
    cbm_node_t a = {.project = "test", .label = "Function", .name = "A", .qualified_name = "test.A"};
    cbm_node_t b = {.project = "test", .label = "Function", .name = "B", .qualified_name = "test.B"};
    cbm_node_t c = {.project = "test", .label = "Function", .name = "C", .qualified_name = "test.C"};
    int64_t a_id = cbm_store_upsert_node(s, &a);
    (void)cbm_store_upsert_node(s, &b); /* B: no outgoing CALLS at all */
    int64_t c_id = cbm_store_upsert_node(s, &c);
    ASSERT_GT(a_id, 0);
    ASSERT_GT(c_id, 0);

    /* Callees are label Var so they do not inflate scan_count/bind_cap. */
    for (int i = 0; i < 40; i++) {
        char nm[32];
        char qn[48];
        snprintf(nm, sizeof(nm), "acallee%d", i);
        snprintf(qn, sizeof(qn), "test.acallee%d", i);
        cbm_node_t t = {.project = "test", .label = "Var", .name = nm, .qualified_name = qn};
        int64_t tid = cbm_store_upsert_node(s, &t);
        cbm_edge_t e = {.project = "test", .source_id = a_id, .target_id = tid, .type = "CALLS"};
        cbm_store_insert_edge(s, &e);
    }
    for (int i = 0; i < 5; i++) {
        char nm[32];
        char qn[48];
        snprintf(nm, sizeof(nm), "ccallee%d", i);
        snprintf(qn, sizeof(qn), "test.ccallee%d", i);
        cbm_node_t t = {.project = "test", .label = "Var", .name = nm, .qualified_name = qn};
        int64_t tid = cbm_store_upsert_node(s, &t);
        cbm_edge_t e = {.project = "test", .source_id = c_id, .target_id = tid, .type = "CALLS"};
        cbm_store_insert_edge(s, &e);
    }

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) OPTIONAL MATCH (f)-[:CALLS]->(g) WHERE g IS NULL RETURN f.name",
        "test", 3, &r);
    ASSERT_EQ(rc, 0);

    /* Positive control: B genuinely has no callees, so the query must still find
     * it. Without this a "C is absent" assertion could pass on an empty result. */
    bool saw_b = false;
    bool saw_c = false;
    for (int i = 0; i < r.row_count; i++) {
        const char *name = r.rows[i][0];
        if (name && strcmp(name, "B") == 0) {
            saw_b = true;
        }
        if (name && strcmp(name, "C") == 0) {
            saw_c = true;
        }
    }
    ASSERT_TRUE(saw_b);
    /* The discriminator: C has 5 callees, so claiming it has none is a
     * fabrication. Pre-fix this is exactly what the saturated path emitted. */
    ASSERT_FALSE(saw_c);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Upstream 068690ca. Sibling of the leaf-fallback test, but for the BOUND-TERMINAL expansion path
 * (expand_from_bound_terminal): the OPTIONAL start var is unbound and the
 * terminal is bound, e.g. `MATCH (f) OPTIONAL MATCH (c)-[:CALLS]->(f)`. That
 * function sized its hop buffer bind_count*10 + 1 and gated the OPTIONAL
 * fallback on `new_count < max_new`, so once one terminal's expansion saturated
 * the buffer, every LATER terminal's no-match row was silently dropped — the
 * rows `WHERE c IS NULL` is meant to surface. Not an overflow (the guard kept
 * the write in bounds) but real data loss. Lossless sizing (bind_count*10 +
 * bind_count, fallback ungated) preserves them. */
TEST(cypher_exec_bound_terminal_optional_fallback_survives) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    /* 2 Function nodes (hub + leaf) → bind_count = 2, max_new = 20. The hub has
     * 21 incoming CALLS edges (> max_new), so its expansion saturates the write
     * buffer; the leaf has none, so it must still yield its OPTIONAL no-match
     * row. Under the original "+ SKIP_ONE" sizing that row was dropped once the
     * buffer filled; the lossless sizing preserves it regardless of the order in
     * which the (unordered) label scan visits the two terminals. */
    cbm_node_t hub = {
        .project = "test", .label = "Function", .name = "hub", .qualified_name = "test.hub"};
    int64_t hub_id = cbm_store_upsert_node(s, &hub);
    cbm_node_t leaf = {
        .project = "test", .label = "Function", .name = "leaf", .qualified_name = "test.leaf"};
    cbm_store_upsert_node(s, &leaf);

    /* Callers are non-Function so they do not inflate bind_count. Each CALLS the
     * hub (source = caller, target = hub), so from the bound terminal `hub` the
     * expansion binds the unbound start `c` to each caller. */
    for (int i = 0; i < 21; i++) {
        char nm[32];
        char qn[48];
        snprintf(nm, sizeof(nm), "caller%02d", i);
        snprintf(qn, sizeof(qn), "test.caller%02d", i);
        cbm_node_t caller = {.project = "test", .label = "Var", .name = nm, .qualified_name = qn};
        int64_t cid = cbm_store_upsert_node(s, &caller);
        cbm_edge_t e = {.project = "test", .source_id = cid, .target_id = hub_id, .type = "CALLS"};
        cbm_store_insert_edge(s, &e);
    }

    /* max_rows 0 → the 100000 result ceiling, so the output LIMIT does not hide
     * the leaf row; we are testing the hop buffer, not the output cap. */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) OPTIONAL MATCH (c)-[:CALLS]->(f) RETURN f.name, c.name", "test", 0,
        &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.col_count, 2);

    /* The leaf terminal has no incoming CALLS edge, so it must yield one row with
     * the start var `c` unbound (""). Before the fix this row was dropped once the
     * hub saturated the buffer. Also confirm the hub still expanded to bound rows. */
    bool leaf_fallback = false;
    bool hub_expanded = false;
    for (int i = 0; i < r.row_count; i++) {
        const char *f = r.rows[i][0];
        const char *c = r.rows[i][1];
        if (strcmp(f, "leaf") == 0 && c[0] == '\0') {
            leaf_fallback = true;
        }
        if (strcmp(f, "hub") == 0 && c[0] != '\0') {
            hub_expanded = true;
        }
    }
    ASSERT_TRUE(leaf_fallback); /* the bound-terminal OPTIONAL no-match row survived */
    ASSERT_TRUE(hub_expanded);  /* the expansion still produced bound rows */

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Discriminating companion to the test above: preserving OPTIONAL no-match rows
 * under saturation must NOT be bought by fabricating them. If match detection is
 * gated on the same ceiling as the write, then once one terminal fills the
 * buffer, another terminal that genuinely HAS callers is never scanned, its
 * match_count stays 0, and the fallback invents an unbound "dead code" row for it
 * — reporting live code as dead, which is worse than dropping a row.
 *
 * The construction is deliberately order-independent: BOTH hubs have enough
 * callers to saturate the buffer on their own, so whichever the scan visits
 * second is guaranteed to be processed after saturation. Under the gated variant
 * that second hub is fabricated as dead; the assertion "no hub with callers is
 * dead" then fails no matter which order `find_nodes_by_label` returns (its query
 * has no ORDER BY, so the test must not depend on one). The fix makes it pass. */
TEST(cypher_exec_bound_terminal_saturation_no_false_deadcode) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    /* 3 Function terminals → bind_count = 3, max_new = 30. */
    cbm_node_t hubA = {
        .project = "test", .label = "Function", .name = "hubA", .qualified_name = "test.hubA"};
    int64_t hubA_id = cbm_store_upsert_node(s, &hubA);
    cbm_node_t hubB = {
        .project = "test", .label = "Function", .name = "hubB", .qualified_name = "test.hubB"};
    int64_t hubB_id = cbm_store_upsert_node(s, &hubB);
    cbm_node_t leaf = {
        .project = "test", .label = "Function", .name = "leaf", .qualified_name = "test.leaf"};
    cbm_store_upsert_node(s, &leaf);

    /* BOTH hubs get 35 callers (> max_new = 30), so either one saturates the write
     * buffer by itself; leaf gets none. Callers are non-Function so they don't
     * inflate bind_count. */
    for (int i = 0; i < 70; i++) {
        char nm[32];
        char qn[48];
        snprintf(nm, sizeof(nm), "caller%02d", i);
        snprintf(qn, sizeof(qn), "test.caller%02d", i);
        cbm_node_t caller = {.project = "test", .label = "Var", .name = nm, .qualified_name = qn};
        int64_t cid = cbm_store_upsert_node(s, &caller);
        int64_t tgt = i < 35 ? hubA_id : hubB_id; /* 35 -> hubA, 35 -> hubB */
        cbm_edge_t e = {.project = "test", .source_id = cid, .target_id = tgt, .type = "CALLS"};
        cbm_store_insert_edge(s, &e);
    }

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) OPTIONAL MATCH (c)-[:CALLS]->(f) RETURN f.name, c.name", "test", 0,
        &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.col_count, 2);

    bool hub_expanded = false;      /* sanity: the buffer really did fill from a hub */
    bool hub_false_deadcode = false; /* the bug: a hub with callers invented as dead */
    bool leaf_deadcode = false;     /* the lossless property: genuine dead code kept */
    for (int i = 0; i < r.row_count; i++) {
        const char *f = r.rows[i][0];
        const char *c = r.rows[i][1];
        bool is_hub = strcmp(f, "hubA") == 0 || strcmp(f, "hubB") == 0;
        if (is_hub && c[0] != '\0') {
            hub_expanded = true;
        }
        if (is_hub && c[0] == '\0') {
            hub_false_deadcode = true;
        }
        if (strcmp(f, "leaf") == 0 && c[0] == '\0') {
            leaf_deadcode = true;
        }
    }
    ASSERT_TRUE(hub_expanded);
    ASSERT_FALSE(hub_false_deadcode); /* live code with callers must never appear as dead */
    ASSERT_TRUE(leaf_deadcode);       /* genuine no-match row still survives saturation */

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #252: toInteger() */
TEST(cypher_issue252_tointeger) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN toInteger(f.start_line) AS ln",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #305: count(*) + AS alias */
TEST(cypher_issue305_count_star_alias) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (n) RETURN count(*) AS total", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Regression: projecting several computed/JSON properties in one row must yield
 * DISTINCT values. node_prop previously returned a single shared static buffer,
 * so every such column aliased the last property read — and because the search
 * key is matched in the JSON, `loop_depth` must not be confused with its suffix
 * `transitive_loop_depth`. Exercises the bottleneck metrics end-to-end. */
TEST(cypher_multi_prop_projection_no_alias) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t n = {.project = "test",
                    .label = "Function",
                    .name = "Hot",
                    .qualified_name = "test.Hot",
                    .file_path = "hot.go",
                    .start_line = 10,
                    .end_line = 42,
                    .properties_json = "{\"complexity\":3,\"cognitive\":7,\"loop_count\":2,"
                                       "\"loop_depth\":1,\"self_recursive\":false,"
                                       "\"transitive_loop_depth\":5,\"recursive\":true}"};
    cbm_store_upsert_node(s, &n);

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) RETURN f.loop_depth, f.transitive_loop_depth, "
                                "f.cognitive, f.complexity, f.start_line, f.end_line",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_EQ(r.col_count, 6);
    ASSERT_STR_EQ(r.rows[0][0], "1");  /* loop_depth — NOT the suffix transitive_loop_depth */
    ASSERT_STR_EQ(r.rows[0][1], "5");  /* transitive_loop_depth */
    ASSERT_STR_EQ(r.rows[0][2], "7");  /* cognitive */
    ASSERT_STR_EQ(r.rows[0][3], "3");  /* complexity */
    ASSERT_STR_EQ(r.rows[0][4], "10"); /* start_line (computed) */
    ASSERT_STR_EQ(r.rows[0][5], "42"); /* end_line (computed) — distinct from start_line */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Result projection writes into fixed-width per-row stack arrays
 * (vals[CBM_SZ_32] / func_bufs[CBM_SZ_32][…] in execute_return_simple and its
 * siblings), indexed by the parsed RETURN item count. The parser must bound
 * that count to the array width; an over-wide RETURN has to be rejected, not
 * allowed to run and write past the arrays. Drive a >32-column RETURN in a
 * forked child so a stack overwrite (ASan abort, or a raw segfault) shows up
 * as a killing signal instead of taking down the whole runner; the bounded
 * path returns an ordinary error and the child exits cleanly. */
TEST(cypher_wide_return_projection_bounded) {
#ifdef _WIN32
    SKIP_PLATFORM("fork crash-isolation is POSIX-only; the parse-time bound is platform-agnostic");
#else
    char query[4096];
    int off = snprintf(query, sizeof(query), "MATCH (f:Function) RETURN ");
    for (int i = 0; i < 48; i++) { /* 48 > CBM_SZ_32 (32) fixed columns */
        off += snprintf(query + off, sizeof(query) - (size_t)off, "%sf.p%d", i ? ", " : "", i);
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        cbm_store_t *s = setup_cypher_store();
        cbm_cypher_result_t r = {0};
        (void)cbm_cypher_execute(s, query, "test", 0, &r);
        cbm_cypher_result_free(&r);
        cbm_store_close(s);
        _exit(0); /* reached only when execution did NOT overflow */
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "wide RETURN killed by signal %d — projection stack overflow",
                 WTERMSIG(status));
        FAIL(msg);
    }
    ASSERT_TRUE(WIFEXITED(status));
    PASS();
#endif
}

TEST(cypher_classify_native_relationship) {
    cbm_query_t *q = NULL;
    char *error = NULL;
    ASSERT_EQ(cbm_cypher_parse("MATCH (a)-[:CALLS]->(b) RETURN a,b", &q, &error), 0);
    const char *reason = NULL;
    ASSERT_EQ(cbm_cypher_classify_plan(q, &reason), CBM_CYPHER_ROUTE_NATIVE_GRAPH);
    ASSERT_NOT_NULL(reason);
    cbm_query_free(q);
    free(error);
    PASS();
}

TEST(cypher_classify_rich_query_in_database) {
    cbm_query_t *q = NULL;
    char *error = NULL;
    ASSERT_EQ(cbm_cypher_parse("MATCH (n:Function) WHERE n.name =~ '.*run.*' RETURN n.name ORDER BY n.name LIMIT 2", &q, &error), 0);
    const char *reason = NULL;
    ASSERT_EQ(cbm_cypher_classify_plan(q, &reason), CBM_CYPHER_ROUTE_IN_DATABASE_COMPAT);
    ASSERT_NOT_NULL(reason);
    cbm_query_free(q);
    free(error);
    PASS();
}

TEST(cypher_classify_null_plan_unsupported) {
    const char *reason = NULL;
    ASSERT_EQ(cbm_cypher_classify_plan(NULL, &reason), CBM_CYPHER_ROUTE_UNSUPPORTED);
    ASSERT_NOT_NULL(reason);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  LONG OPERATOR CHAINS AND MANY UNION BRANCHES
 *
 * A WHERE chain `a OR b OR c ...` and a UNION chain are bounded only by the
 * message size, and an agent that lists a few thousand names in one query is
 * ordinary input. The parser used to fold a chain into a tree as deep as the
 * chain is long and to parse every UNION branch through a nested call, so each
 * walker (evaluation, the early WHERE pass, parsing) recursed once per operand
 * or branch (#58). These tests run their query on a 2 MiB thread, which the
 * recursive forms overflow at these sizes, inside a forked child on POSIX, so
 * an overflow is reported as the signal that killed the child instead of
 * ending the test runner. The stack also holds the thread's static TLS block
 * (about 0.8 MB in the test runner), so it cannot shrink much further: the
 * 256 KiB query worker stack upstream tests with cannot be created here.
 * ══════════════════════════════════════════════════════════════════ */

enum { CYPHER_WORKER_STACK_BYTES = 2 * 1024 * 1024 };

typedef struct {
    int (*check)(void *);
    void *arg;
    int result;
} worker_stack_run_t;

static void *worker_stack_thread(void *opaque) {
    worker_stack_run_t *run = opaque;
    run->result = run->check(run->arg);
    return NULL;
}

/* Runs check(arg) on a CYPHER_WORKER_STACK_BYTES thread; returns its result. */
static int run_on_worker_stack_thread(worker_stack_run_t *run) {
    cbm_thread_t t;
    if (cbm_thread_create(&t, CYPHER_WORKER_STACK_BYTES, worker_stack_thread, run) != 0) {
        FAIL("could not create the worker-stack thread");
    }
    (void)cbm_thread_join(&t);
    return run->result;
}

/* Runs check(arg) on a bounded stack. POSIX: on the CYPHER_WORKER_STACK_BYTES
 * thread of a forked child, whose exit status is the check's result; a killing
 * signal is reported as such. Windows has no fork, and CreateThread's size
 * argument sets the committed stack while the reserve stays the image default,
 * so the thread runs in-process there. */
static int run_on_worker_stack(int (*check)(void *), void *arg) {
    worker_stack_run_t run = {.check = check, .arg = arg, .result = 1};
#ifdef _WIN32
    return run_on_worker_stack_thread(&run);
#else
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        int code = run_on_worker_stack_thread(&run);
        fflush(NULL);
        _exit(code == 0 ? 0 : 1);
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        char m[96];
        snprintf(m, sizeof(m), "query thread killed by signal %d on a %d KiB stack",
                 WTERMSIG(status), CYPHER_WORKER_STACK_BYTES / 1024);
        FAIL(m);
    }
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
#endif
}

/* The fixture graph of setup_cypher_store, restated here so the expected rows
 * are computed by the test and not read back from the engine. */
static const char *const cypher_fixture_functions[] = {"HandleOrder", "ValidateOrder",
                                                       "SubmitOrder", "LogError"};
enum { CYPHER_FIXTURE_FUNCTION_COUNT = 4 };
static const char *const cypher_fixture_calls[][2] = {{"HandleOrder", "ValidateOrder"},
                                                      {"ValidateOrder", "SubmitOrder"},
                                                      {"HandleOrder", "LogError"}};
enum { CYPHER_FIXTURE_CALL_COUNT = 3 };
static const char *const cypher_fixture_nodes[] = {"HandleOrder", "ValidateOrder", "SubmitOrder",
                                                   "main", "LogError"};
enum { CYPHER_FIXTURE_NODE_COUNT = 5 };

enum { CYPHER_CHAIN_OPERANDS = 50000 };

/* Operand i of a generated chain compares f.name with this value. Two fixture
 * names sit deep in the chain (the middle and the very last operand); every
 * other operand names a function that does not exist. */
static const char *chain_operand_value(int i, char *buf, size_t n) {
    if (i == CYPHER_CHAIN_OPERANDS / 2) {
        return "ValidateOrder";
    }
    if (i == CYPHER_CHAIN_OPERANDS - 1) {
        return "LogError";
    }
    snprintf(buf, n, "no_such_function_%d", i);
    return buf;
}

/* The test's own reading of the chain: does any operand name `name`? */
static bool chain_names(const char *name) {
    for (int i = 0; i < CYPHER_CHAIN_OPERANDS; i++) {
        char buf[48];
        if (strcmp(chain_operand_value(i, buf, sizeof(buf)), name) == 0) {
            return true;
        }
    }
    return false;
}

/* head + "f.name <op> \"v0\"" + join + "f.name <op> \"v1\"" + ... + tail */
static char *chain_query(const char *head, const char *op, const char *join, const char *tail) {
    size_t cap = 256 + (size_t)CYPHER_CHAIN_OPERANDS * 64;
    char *q = malloc(cap);
    if (!q) {
        return NULL;
    }
    size_t len = (size_t)snprintf(q, cap, "%s", head);
    for (int i = 0; i < CYPHER_CHAIN_OPERANDS && len < cap; i++) {
        char buf[48];
        const char *v = chain_operand_value(i, buf, sizeof(buf));
        len += (size_t)snprintf(q + len, cap - len, "%sf.name %s \"%s\"", i ? join : "", op, v);
    }
    if (len >= cap) {
        free(q);
        return NULL;
    }
    snprintf(q + len, cap - len, "%s", tail);
    return q;
}

typedef struct {
    const char *head;
    const char *op;
    const char *join;
    const char *tail;
    bool admits_named; /* true: a row passes when the chain names f; false: when it does not */
    int expected_rows;
} chain_case_t;

/* Runs the chain query and checks every row against the test's own reading of
 * the chain, then checks that the engine still answers afterwards. */
static int chain_check(void *arg) {
    const chain_case_t *c = arg;
    char *query = chain_query(c->head, c->op, c->join, c->tail);
    ASSERT_NOT_NULL(query);

    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, query, "test", 0, &r);
    free(query);
    if (rc != 0) {
        printf("  query error: %s\n", r.error ? r.error : "(none)");
    }
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, c->expected_rows);
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_EQ(chain_names(r.rows[i][0]), c->admits_named);
    }
    cbm_cypher_result_free(&r);

    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, CYPHER_FIXTURE_FUNCTION_COUNT);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    return 0;
}

/* 50,000 operands joined by OR over a single node pattern: the rows are the
 * Functions the chain names. The expected count comes from the fixture list
 * and chain_names, not from the engine. */
TEST(cypher_long_or_chain_evaluates) {
    chain_case_t c = {.head = "MATCH (f:Function) WHERE ",
                      .op = "=",
                      .join = " OR ",
                      .tail = " RETURN f.name",
                      .admits_named = true,
                      .expected_rows = 0};
    for (int i = 0; i < CYPHER_FIXTURE_FUNCTION_COUNT; i++) {
        c.expected_rows += chain_names(cypher_fixture_functions[i]) ? 1 : 0;
    }
    ASSERT_EQ(c.expected_rows, 2); /* the generator plants ValidateOrder and LogError */
    int rc = run_on_worker_stack(chain_check, &c);
    if (rc != 0) {
        return rc;
    }
    PASS();
}

/* 50,000 operands joined by AND over a single-hop pattern, so the early WHERE
 * pass on the first alias sees the chain as well as the evaluator: the rows are
 * the CALLS edges whose caller the chain does not name. */
TEST(cypher_long_and_chain_evaluates) {
    chain_case_t c = {.head = "MATCH (f:Function)-[:CALLS]->(g:Function) WHERE ",
                      .op = "<>",
                      .join = " AND ",
                      .tail = " RETURN f.name, g.name",
                      .admits_named = false,
                      .expected_rows = 0};
    for (int i = 0; i < CYPHER_FIXTURE_CALL_COUNT; i++) {
        c.expected_rows += chain_names(cypher_fixture_calls[i][0]) ? 0 : 1;
    }
    ASSERT_EQ(c.expected_rows, 2); /* HandleOrder's two calls; ValidateOrder's is named */
    int rc = run_on_worker_stack(chain_check, &c);
    if (rc != 0) {
        return rc;
    }
    PASS();
}

static int cmp_cstr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Runs `query` and checks that its first column holds exactly the names in
 * `expected`, given in strcmp order; the rows are sorted the same way so the
 * engine's row order does not matter. */
static int assert_name_set(cbm_store_t *s, const char *query, const char *const *expected, int n) {
    enum { NAME_SET_MAX = 8 };
    ASSERT_TRUE(n <= NAME_SET_MAX);
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, query, "test", 0, &r);
    if (rc != 0) {
        printf("  query error: %s\n", r.error ? r.error : "(none)");
    }
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, n);
    const char *names[NAME_SET_MAX];
    for (int i = 0; i < n; i++) {
        names[i] = r.rows[i][0];
    }
    qsort(names, (size_t)n, sizeof(names[0]), cmp_cstr);
    for (int i = 0; i < n; i++) {
        ASSERT_STR_EQ(names[i], expected[i]);
    }
    cbm_cypher_result_free(&r);
    return 0;
}

/* Every spelling in `forms` is one WHERE expression; all must yield `expected`. */
static int where_forms_agree(cbm_store_t *s, const char *const *forms, int form_count,
                             const char *const *expected, int n) {
    for (int f = 0; f < form_count; f++) {
        char query[512];
        snprintf(query, sizeof(query), "MATCH (n) WHERE %s RETURN n.name", forms[f]);
        int rc = assert_name_set(s, query, expected, n);
        if (rc != 0) {
            printf("  form: %s\n", forms[f]);
            return rc;
        }
    }
    return 0;
}

/* The early WHERE pass (execute_single) binds only the first MATCH alias, so
 * an operand on the second alias is UNKNOWN there and must neither decide nor
 * be dropped from a chain: three and more operands put it on a chain link and
 * not only on the chain's head. The rows are the full evaluation's; an early
 * pass that read UNKNOWN as false, or let a later operand overwrite it, would
 * prune the HandleOrder seed and lose them. */
TEST(cypher_chain_with_unbound_alias_keeps_seed) {
    cbm_store_t *s = setup_cypher_store();

    /* OR, UNKNOWN first: only HandleOrder -> LogError has a target without "Order". */
    static const char *const log_error[] = {"LogError"};
    ASSERT_EQ(assert_name_set(s,
                              "MATCH (a:Function)-[:CALLS]->(b:Function) WHERE NOT b.name CONTAINS "
                              "\"Order\" OR a.name = \"NoSuchA\" OR a.name = \"NoSuchB\" "
                              "RETURN b.name",
                              log_error, 1),
              0);

    /* XOR, UNKNOWN in the middle: F ^ T ^ F holds for HandleOrder -> LogError only. */
    ASSERT_EQ(assert_name_set(s,
                              "MATCH (a:Function)-[:CALLS]->(b:Function) WHERE a.name = "
                              "\"NoSuchA\" XOR b.name = \"LogError\" XOR a.name = \"NoSuchB\" "
                              "RETURN b.name",
                              log_error, 1),
              0);

    /* AND, UNKNOWN first and a definite operand last: HandleOrder's two calls. */
    static const char *const handle_order_calls[] = {"LogError", "ValidateOrder"};
    ASSERT_EQ(assert_name_set(s,
                              "MATCH (a:Function)-[:CALLS]->(b:Function) WHERE b.name <> "
                              "\"SubmitOrder\" AND a.name <> \"NoSuchA\" AND a.name = "
                              "\"HandleOrder\" RETURN b.name",
                              handle_order_calls, 2),
              0);

    cbm_store_close(s);
    PASS();
}

/* A chain and the same expression written out with parentheses, in both
 * associations, must agree row for row. The operands include a property the
 * `main` node does not have: file_path reads as the empty string there, so
 * `IS NULL` holds and `<>` holds, and a missing value takes part in AND, OR and
 * XOR like any other operand instead of voiding the chain. */
TEST(cypher_mixed_chain_matches_nested_form) {
    cbm_store_t *s = setup_cypher_store();

    static const char *const or_forms[] = {
        "n.file_path = \"handler.go\" OR n.file_path IS NULL OR n.name = \"LogError\"",
        "(n.file_path = \"handler.go\" OR n.file_path IS NULL) OR n.name = \"LogError\"",
        "n.file_path = \"handler.go\" OR (n.file_path IS NULL OR n.name = \"LogError\")"};
    static const char *const or_rows[] = {"HandleOrder", "LogError", "main"};
    ASSERT_EQ(where_forms_agree(s, or_forms, 3, or_rows, 3), 0);

    static const char *const and_forms[] = {
        "n.file_path <> \"handler.go\" AND n.file_path <> \"log.go\" AND n.name <> \"SubmitOrder\"",
        "(n.file_path <> \"handler.go\" AND n.file_path <> \"log.go\") AND n.name <> "
        "\"SubmitOrder\"",
        "n.file_path <> \"handler.go\" AND (n.file_path <> \"log.go\" AND n.name <> "
        "\"SubmitOrder\")"};
    static const char *const and_rows[] = {"ValidateOrder", "main"};
    ASSERT_EQ(where_forms_agree(s, and_forms, 3, and_rows, 2), 0);

    /* Parity: main is T^T^F^F, LogError F^F^T^F, HandleOrder F^F^F^T. */
    static const char *const xor_forms[] = {
        "n.name = \"main\" XOR n.file_path IS NULL XOR n.name = \"LogError\" XOR "
        "n.file_path = \"handler.go\"",
        "((n.name = \"main\" XOR n.file_path IS NULL) XOR n.name = \"LogError\") XOR "
        "n.file_path = \"handler.go\"",
        "n.name = \"main\" XOR (n.file_path IS NULL XOR (n.name = \"LogError\" XOR "
        "n.file_path = \"handler.go\"))"};
    static const char *const xor_rows[] = {"HandleOrder", "LogError"};
    ASSERT_EQ(where_forms_agree(s, xor_forms, 3, xor_rows, 2), 0);

    /* Mixed precedence (NOT > AND > XOR > OR) with a NOT over a missing value:
     * main by name, LogError by (F AND T) XOR T, SubmitOrder by NOT (F). */
    static const char *const mixed_forms[] = {
        "n.name = \"main\" OR n.file_path IS NULL AND n.name = \"LogError\" XOR "
        "n.file_path = \"log.go\" OR NOT n.file_path <> \"submit.go\"",
        "n.name = \"main\" OR ((n.file_path IS NULL AND n.name = \"LogError\") XOR "
        "n.file_path = \"log.go\") OR (NOT (n.file_path <> \"submit.go\"))",
        "(n.name = \"main\" OR ((n.file_path IS NULL AND n.name = \"LogError\") XOR "
        "n.file_path = \"log.go\")) OR (NOT (n.file_path <> \"submit.go\"))"};
    static const char *const mixed_rows[] = {"LogError", "SubmitOrder", "main"};
    ASSERT_EQ(where_forms_agree(s, mixed_forms, 3, mixed_rows, 3), 0);

    cbm_store_close(s);
    PASS();
}

/* Branch i selects fixture node i % 5 by name; `join` is " UNION " or
 * " UNION ALL ". */
static char *union_query(int branches, const char *join) {
    size_t cap = 64 + (size_t)branches * 80;
    char *q = malloc(cap);
    if (!q) {
        return NULL;
    }
    size_t len = 0;
    for (int i = 0; i < branches && len < cap; i++) {
        len +=
            (size_t)snprintf(q + len, cap - len, "%sMATCH (n) WHERE n.name = \"%s\" RETURN n.name",
                             i ? join : "", cypher_fixture_nodes[i % CYPHER_FIXTURE_NODE_COUNT]);
    }
    if (len >= cap) {
        free(q);
        return NULL;
    }
    return q;
}

enum { CYPHER_UNION_MANY_BRANCHES = 20000 };
enum { CYPHER_UNION_DEADLINE_MS = 10 * 60 * 1000 };

/* UNION ALL keeps every branch's row in branch order. */
static int union_many_check(void *arg) {
    (void)arg;
    char *query = union_query(CYPHER_UNION_MANY_BRANCHES, " UNION ALL ");
    ASSERT_NOT_NULL(query);

    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* This exercises stack depth and every result, independently of how
     * slowly instrumentation runs. Dedicated deadline tests retain expiry. */
    cbm_cypher_test_set_deadline_ms(CYPHER_UNION_DEADLINE_MS);
    /* Request exactly the output this fixture needs: the default 100k ceiling
     * also reserves 100k binding slots separately for every UNION branch. */
    int rc = cbm_cypher_execute(s, query, "test", CYPHER_UNION_MANY_BRANCHES, &r);
    cbm_cypher_test_set_deadline_ms(-1);
    free(query);
    if (rc != 0) {
        printf("  query error: %s\n", r.error ? r.error : "(none)");
    }
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, CYPHER_UNION_MANY_BRANCHES);
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_STR_EQ(r.rows[i][0], cypher_fixture_nodes[i % CYPHER_FIXTURE_NODE_COUNT]);
    }
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    return 0;
}

/* 20,000 UNION ALL branches parse, run and free on the worker stack, and
 * every branch contributes its row. */
TEST(cypher_many_union_branches) {
    int rc = run_on_worker_stack(union_many_check, NULL);
    if (rc != 0) {
        return rc;
    }
    PASS();
}

/* 50 branches: UNION ALL yields one row per branch in branch order, UNION
 * keeps the first row of each name, and a UNION with nothing after it is still
 * a clean parse error. */
TEST(cypher_fifty_union_branches) {
    enum { BRANCHES = 50 };
    cbm_store_t *s = setup_cypher_store();
    char *all = union_query(BRANCHES, " UNION ALL ");
    char *distinct = union_query(BRANCHES, " UNION ");
    ASSERT_NOT_NULL(all);
    ASSERT_NOT_NULL(distinct);

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, all, "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, BRANCHES);
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_STR_EQ(r.rows[i][0], cypher_fixture_nodes[i % CYPHER_FIXTURE_NODE_COUNT]);
    }
    cbm_cypher_result_free(&r);

    rc = cbm_cypher_execute(s, distinct, "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, CYPHER_FIXTURE_NODE_COUNT);
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_STR_EQ(r.rows[i][0], cypher_fixture_nodes[i]);
    }
    cbm_cypher_result_free(&r);
    free(all);
    free(distinct);

    rc = cbm_cypher_execute(s, "MATCH (n) RETURN n.name UNION", "test", 0, &r);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(r.error);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════ */

SUITE(cypher) {
    RUN_TEST(cypher_classify_native_relationship);
    RUN_TEST(cypher_classify_rich_query_in_database);
    RUN_TEST(cypher_classify_null_plan_unsupported);
    /* Lexer */
    RUN_TEST(cypher_lex_simple_match);
    RUN_TEST(cypher_lex_relationship);
    RUN_TEST(cypher_lex_string_literal);
    RUN_TEST(cypher_lex_single_quote_string);
    RUN_TEST(cypher_lex_string_overflow);
    RUN_TEST(cypher_lex_number);
    RUN_TEST(cypher_lex_operators);
    RUN_TEST(cypher_lex_keywords_case_insensitive);
    RUN_TEST(cypher_lex_pipe_and_star);
    RUN_TEST(cypher_lex_full_query);
    /* Parser */
    RUN_TEST(cypher_parse_simple_node);
    RUN_TEST(cypher_parse_rejects_trailing_tokens);
    RUN_TEST(cypher_parse_rejects_second_with_clause);
    RUN_TEST(cypher_parse_accepts_single_with_clause);
    RUN_TEST(cypher_parse_relationship_outbound);
    RUN_TEST(cypher_parse_relationship_inbound);
    RUN_TEST(cypher_parse_relationship_any);
    RUN_TEST(cypher_parse_variable_length);
    RUN_TEST(cypher_parse_variable_length_unbounded);
    RUN_TEST(cypher_parse_multiple_edge_types);
    RUN_TEST(cypher_parse_where_clause);
    RUN_TEST(cypher_parse_where_regex);
    RUN_TEST(cypher_parse_where_and);
    RUN_TEST(cypher_parse_return_simple);
    RUN_TEST(cypher_parse_return_count);
    RUN_TEST(cypher_parse_return_order_limit);
    RUN_TEST(cypher_parse_multikey_order_by_issue1334);
    RUN_TEST(cypher_parse_order_by_over_cap_rejected_issue1334);
    RUN_TEST(cypher_parse_missing_skip_limit_operand_rejected);
    RUN_TEST(cypher_parse_numeric_skip_limit_still_accepted);
    RUN_TEST(cypher_parse_return_distinct);
    RUN_TEST(cypher_parse_inline_props);
    RUN_TEST(cypher_parse_error);
    RUN_TEST(cypher_parse_error_frees_partial_pattern);
    /* Execution */
    RUN_TEST(cypher_exec_deadline_aborts_runaway_query_issue601);
    RUN_TEST(cypher_exec_deadline_allows_normal_query_issue601);
    RUN_TEST(cypher_exec_deadline_covers_joins_and_sorts);
    RUN_TEST(cypher_exec_order_by_matches_stable_sort);
    RUN_TEST(cypher_exec_distinct_keeps_first_occurrences);
    RUN_TEST(cypher_exec_with_distinct_separator_in_values);
    RUN_TEST(cypher_exec_group_by_keeps_first_seen_groups);
    RUN_TEST(cypher_exec_count_distinct_matches_reference);
    RUN_TEST(cypher_deep_nesting_rejected_not_crash);
    RUN_TEST(cypher_parse_unwind_oversized_literal_no_overflow);
    RUN_TEST(cypher_parse_unwind_many_elements_no_overflow);
    RUN_TEST(cypher_exec_optional_empty_label_no_overflow);
    RUN_TEST(cypher_exec_optional_empty_label_rel_no_overflow);
    RUN_TEST(cypher_cross_join_alloc_rejects_overflow);
    RUN_TEST(cypher_exec_optional_rel_saturated_no_overflow);
    RUN_TEST(cypher_exec_optional_rel_leaf_fallback_survives);
    RUN_TEST(cypher_exec_optional_saturated_does_not_fabricate_no_match);
    RUN_TEST(cypher_exec_bound_terminal_optional_fallback_survives);
    RUN_TEST(cypher_exec_bound_terminal_saturation_no_false_deadcode);
    RUN_TEST(cypher_exec_match_all_functions);
    RUN_TEST(cypher_exec_varlength_path_semantics_issue797);
    RUN_TEST(cypher_issue240_labels_function);
    RUN_TEST(cypher_issue237_distinct_order_limit);
    RUN_TEST(cypher_issue873_distinct_order_limit_dedupes_before_limit);
    RUN_TEST(cypher_issue873_distinct_limit_dedupes_before_limit);
    RUN_TEST(cypher_issue873_distinct_order_skip_limit_dedupes_before_skip);
    RUN_TEST(cypher_return_distinct_counts_distinct_rows_against_cap);
    RUN_TEST(cypher_exec_prop_composite_and_escaped_values);
    RUN_TEST(cypher_issue1111_return_type_count_group);
    RUN_TEST(cypher_issue1111_with_type_count_group);
    RUN_TEST(cypher_exec_variable_length_repeated_node_var_unifies);
    RUN_TEST(cypher_issue252_tointeger);
    RUN_TEST(cypher_issue305_count_star_alias);
    RUN_TEST(cypher_exec_where_eq);
    RUN_TEST(cypher_exec_where_regex);
    RUN_TEST(cypher_exec_where_contains);
    RUN_TEST(cypher_exec_where_starts_with);
    RUN_TEST(cypher_exec_return_properties);
    RUN_TEST(cypher_func_labels);
    RUN_TEST(cypher_func_list_index);
    RUN_TEST(cypher_where_property_vs_property);
    RUN_TEST(cypher_func_type);
    RUN_TEST(cypher_func_id);
    RUN_TEST(cypher_func_keys);
    RUN_TEST(cypher_func_properties);
    RUN_TEST(cypher_func_tointeger_tofloat);
    RUN_TEST(cypher_func_size_reverse);
    RUN_TEST(cypher_func_multiarg);
    RUN_TEST(cypher_issue874_where_coalesce_numeric);
    RUN_TEST(cypher_issue874_where_coalesce_string);
    RUN_TEST(cypher_issue874_where_coalesce_not_and);
    RUN_TEST(cypher_issue874_where_substring);
    RUN_TEST(cypher_issue874_where_unsupported_func_error);
    RUN_TEST(cypher_multi_prop_projection_no_alias);
    RUN_TEST(cypher_exists_no_callers);
    RUN_TEST(cypher_exists_has_outgoing_calls);
    RUN_TEST(cypher_exec_calls_relationship);
    RUN_TEST(cypher_exec_calls_with_where);
    RUN_TEST(cypher_exec_inbound);
    RUN_TEST(cypher_exec_count);
    RUN_TEST(cypher_exec_optional_bound_terminal_count_ignores_unbound);
    RUN_TEST(cypher_exec_limit);
    RUN_TEST(cypher_exec_order_by);
    RUN_TEST(cypher_exec_variable_length);
    RUN_TEST(cypher_exec_var_length_explicit_bound_capped);
    RUN_TEST(cypher_exec_var_length_no_reuse_self_loop);
    RUN_TEST(cypher_exec_var_length_truncation_surfaces_warning);
    RUN_TEST(cypher_exec_defines_edge);
    RUN_TEST(cypher_exec_no_results);
    RUN_TEST(cypher_exec_where_numeric);
    /* Go test ports */
    RUN_TEST(cypher_exec_distinct);
    RUN_TEST(cypher_exec_with_distinct_issue238);
    RUN_TEST(cypher_exec_where_label_test_issue241);
    RUN_TEST(cypher_exec_label_alternation_issue242);
    RUN_TEST(cypher_exec_count_distinct_issue239);
    RUN_TEST(cypher_exec_unsupported_func_errors_issue373);
    RUN_TEST(cypher_rejects_projection_of_dropped_with_var_issue1919);
    RUN_TEST(cypher_optional_match_target_still_allowed_issue1919);
    RUN_TEST(cypher_with_alias_stays_in_scope_issue1919);
    RUN_TEST(cypher_exec_unknown_func_return_errors);
    RUN_TEST(cypher_exec_inline_props);
    RUN_TEST(cypher_parse_where_starts_with);
    RUN_TEST(cypher_parse_where_contains);
    RUN_TEST(cypher_parse_where_numeric);
    /* Edge property tests (ported from cypher_test.go Feature 2) */
    RUN_TEST(cypher_edge_prop_access);
    RUN_TEST(cypher_edge_prop_in_where);
    RUN_TEST(cypher_edge_type_prop);
    RUN_TEST(cypher_edge_filter_contains);
    RUN_TEST(cypher_edge_filter_numeric_gte);
    RUN_TEST(cypher_bare_edge_return_exposes_properties_json);
    RUN_TEST(cypher_edge_prop_concurrent_queries_isolated);
    RUN_TEST(cypher_edge_return_without_filter);
    RUN_TEST(cypher_edge_filter_equals);
    RUN_TEST(cypher_edge_filter_starts_with);
    RUN_TEST(cypher_edge_combined_node_and_edge_filter);
    RUN_TEST(cypher_edge_filter_no_match);
    RUN_TEST(cypher_edge_filter_numeric_lt);
    RUN_TEST(cypher_edge_filter_regex);
    RUN_TEST(cypher_edge_builtin_type_filter);
    RUN_TEST(cypher_apply_limit);
    /* Phase 1: Simple operators */
    RUN_TEST(cypher_lex_neq_operators);
    RUN_TEST(cypher_lex_ends_keyword);
    RUN_TEST(cypher_lex_in_is_null);
    RUN_TEST(cypher_exec_where_neq);
    RUN_TEST(cypher_exec_where_neq_bang);
    RUN_TEST(cypher_exec_where_ends_with);
    RUN_TEST(cypher_exec_where_not);
    RUN_TEST(cypher_exec_where_mixed_alias_or);
    RUN_TEST(cypher_exec_where_mixed_alias_xor);
    RUN_TEST(cypher_exec_where_in);
    RUN_TEST(cypher_exec_where_not_in);
    RUN_TEST(cypher_exec_where_is_null);
    RUN_TEST(cypher_exec_where_is_not_null);
    RUN_TEST(cypher_exec_return_star);
    RUN_TEST(cypher_return_star_dedups_repeated_pattern_var);
    RUN_TEST(cypher_return_star_after_with_names_aliases);
    RUN_TEST(cypher_wide_with_refused_not_truncated);
    RUN_TEST(cypher_wide_pattern_refused);
    RUN_TEST(cypher_wide_edge_pattern_refused);
    RUN_TEST(cypher_unnamed_head_takes_a_slot);
    RUN_TEST(cypher_scope_check_survives_wide_pattern);
    RUN_TEST(cypher_parse_neq);
    RUN_TEST(cypher_parse_in);
    RUN_TEST(cypher_parse_is_null);
    /* Phase 2: Expression tree */
    RUN_TEST(cypher_exec_where_or);
    RUN_TEST(cypher_exec_where_complex_bool);
    RUN_TEST(cypher_exec_where_xor);
    RUN_TEST(cypher_exec_where_not_prefix);
    RUN_TEST(cypher_parse_expr_tree_and_or);
    RUN_TEST(cypher_parse_expr_tree_nested);
    /* Phase 3: Unsupported keyword errors */
    RUN_TEST(cypher_error_create);
    RUN_TEST(cypher_error_delete);
    RUN_TEST(cypher_error_set);
    RUN_TEST(cypher_error_merge);
    RUN_TEST(cypher_error_call);
    /* Phase 4: SKIP + aggregation */
    RUN_TEST(cypher_exec_skip);
    RUN_TEST(cypher_exec_skip_limit);
    RUN_TEST(cypher_exec_multikey_order_by_keeps_limit_issue1334);
    RUN_TEST(cypher_exec_multikey_order_by_tiebreak_issue1334);
    RUN_TEST(cypher_exec_with_multikey_order_by_keeps_limit_issue1334);
    RUN_TEST(cypher_exec_sum);
    RUN_TEST(cypher_exec_avg);
    RUN_TEST(cypher_exec_min);
    RUN_TEST(cypher_exec_max);
    RUN_TEST(cypher_exec_collect);
    RUN_TEST(cypher_exec_count_star);
    RUN_TEST(cypher_parse_skip);
    RUN_TEST(cypher_parse_sum_avg);
    RUN_TEST(cypher_parse_collect);
    /* Phase 5: String functions + CASE */
    RUN_TEST(cypher_exec_tolower);
    RUN_TEST(cypher_exec_toupper);
    RUN_TEST(cypher_exec_tostring);
    RUN_TEST(cypher_exec_case);
    RUN_TEST(cypher_parse_tolower);
    RUN_TEST(cypher_parse_case);
    /* Phase 6: WITH clause */
    RUN_TEST(cypher_exec_with_rename);
    RUN_TEST(cypher_issue57_with_agg_carried_node_props);
    RUN_TEST(cypher_issue57_with_agg_groups_by_node_identity);
    RUN_TEST(cypher_issue57_with_simple_carried_node_props);
    RUN_TEST(cypher_issue57_with_carried_id_and_scalar_alias);
    RUN_TEST(cypher_issue57_with_agg_func_item_not_a_node);
    RUN_TEST(cypher_issue57_with_order_by_carried_prop_many_rows);
    RUN_TEST(cypher_exec_with_count);
    RUN_TEST(cypher_exec_with_node_groupvar_prop);
    RUN_TEST(cypher_exec_with_where);
    RUN_TEST(cypher_exec_with_orderby_limit);
    RUN_TEST(cypher_parse_with);
    RUN_TEST(cypher_parse_with_where);
    /* Phase 7: OPTIONAL MATCH + multiple MATCH */
    RUN_TEST(cypher_exec_optional_match_no_result);
    RUN_TEST(cypher_exec_optional_match_has_result);
    RUN_TEST(cypher_exec_multi_match);
    RUN_TEST(cypher_parse_optional_match);
    RUN_TEST(cypher_parse_multi_match);
    /* Phase 8: UNION */
    RUN_TEST(cypher_exec_union);
    RUN_TEST(cypher_exec_union_all);
    RUN_TEST(cypher_parse_union);
    /* Phase 9: UNWIND */
    RUN_TEST(cypher_parse_unwind);
    RUN_TEST(cypher_parse_unwind_var);
    RUN_TEST(cypher_wide_return_projection_bounded);
    /* Long operator chains and many UNION branches (#58) */
    RUN_TEST(cypher_long_or_chain_evaluates);
    RUN_TEST(cypher_long_and_chain_evaluates);
    RUN_TEST(cypher_mixed_chain_matches_nested_form);
    RUN_TEST(cypher_chain_with_unbound_alias_keeps_seed);
    RUN_TEST(cypher_many_union_branches);
    RUN_TEST(cypher_fifty_union_branches);
}
