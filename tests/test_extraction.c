/*
 * test_extraction.c — Regression tests for the extraction module.
 *
 * Port of internal/cbm/regression_test.go (1282 LOC, ~80 test cases).
 * Exercises cbm_extract_file() on code snippets across 30+ languages,
 * verifying definitions, calls, and imports are correctly extracted.
 */
#include "test_framework.h"
#include "cbm.h"
#include "foundation/constants.h"     /* CBM_SZ_* */
#include "../src/foundation/compat.h" /* cbm_clock_gettime (wide-flat scaling guard) */
#include <time.h>

/* ── Helpers ───────────────────────────────────────────────────── */

/* Check if any definition with the given label has the given name. */
static int has_def(CBMFileResult *r, const char *label, const char *name) {
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, label) == 0 && strcmp(r->defs.items[i].name, name) == 0)
            return 1;
    }
    return 0;
}

/* Check if any definition has the given name (any label). */
static int has_def_any(CBMFileResult *r, const char *name) {
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].name, name) == 0)
            return 1;
    }
    return 0;
}

/* Check if any call to the given callee exists. */
static int has_call(CBMFileResult *r, const char *callee) {
    for (int i = 0; i < r->calls.count; i++) {
        if (strstr(r->calls.items[i].callee_name, callee) != NULL)
            return 1;
    }
    return 0;
}

/* Check if any import with the given module path exists. */
static int __attribute__((unused)) has_import(CBMFileResult *r, const char *path_substr) {
    for (int i = 0; i < r->imports.count; i++) {
        if (r->imports.items[i].module_path &&
            strstr(r->imports.items[i].module_path, path_substr) != NULL)
            return 1;
    }
    return 0;
}

/* Count definitions with a given label. */
static int count_defs_with_label(CBMFileResult *r, const char *label) {
    int count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, label) == 0)
            count++;
    }
    return count;
}

/* Count definitions with a given label and name. */
static int count_defs_named(CBMFileResult *r, const char *label, const char *name) {
    int count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, label) == 0 &&
            strcmp(r->defs.items[i].name, name) == 0) {
            count++;
        }
    }
    return count;
}

/* Check if a definition with the given qualified name exists. */
static int has_def_qn(CBMFileResult *r, const char *qn) {
    for (int i = 0; i < r->defs.count; i++) {
        if (r->defs.items[i].qualified_name && strcmp(r->defs.items[i].qualified_name, qn) == 0)
            return 1;
    }
    return 0;
}

/* Convenience: extract, assert no error, return result. Caller frees. */
static CBMFileResult *extract(const char *src, CBMLanguage lang, const char *proj,
                              const char *path) {
    CBMFileResult *r = cbm_extract_file(src, (int)strlen(src), lang, proj, path, 0, NULL, NULL);
    return r;
}

/* ═══════════════════════════════════════════════════════════════════
 * Group A: OOP Languages
 * ═══════════════════════════════════════════════════════════════════ */

/* --- Parse coverage: unrecovered ERROR regions are reported with line ranges --- */
TEST(extract_parse_incomplete_reports_error_ranges) {
    CBMFileResult *clean = extract("int ok(void) { return 1; }\n", CBM_LANG_C, "t", "ok.c");
    ASSERT_NOT_NULL(clean);
    ASSERT_FALSE(clean->parse_incomplete);
    ASSERT_NULL(clean->error_ranges);
    ASSERT_EQ(clean->error_region_count, 0);
    cbm_free_result(clean);

    CBMFileResult *r = extract("int ok(void) { return 1; }\n"
                               "\n"
                               "int broken( { @@ ### $$ ;;\n"
                               "}}} )) ((\n",
                               CBM_LANG_C, "t", "broken.c");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_TRUE(r->parse_incomplete);
    ASSERT_NOT_NULL(r->error_ranges);
    ASSERT(r->error_region_count > 0);
    /* Line 1 parses cleanly; the reported damage starts at or after line 3. */
    ASSERT(r->error_ranges[0] != '1' || r->error_ranges[1] != '-');
    cbm_free_result(r);
    PASS();
}

/* --- Parse budget is thread CPU time, not wall clock (upstream 52701625) --- */
TEST(extract_parse_budget_survives_wall_stall) {
    /* The hook reads the wall clock budget + 1 s ahead for this file, as if the
     * worker had been descheduled: a wall-only budget would drop the file. */
    enum { LINES = 2000 };
    size_t cap = (size_t)LINES * 40;
    char *src = malloc(cap);
    ASSERT_NOT_NULL(src);
    size_t off = 0;
    for (int i = 0; i < LINES; i++) {
        off += (size_t)snprintf(src + off, cap - off, "def f%d(x):\n    return x + %d\n", i, i);
    }
    cbm_setenv("CBM_TEST_WALL_STALL_ON", "stalled.py", 1);
    CBMFileResult *r = cbm_extract_file(src, (int)off, CBM_LANG_PYTHON, "t", "pkg/stalled.py",
                                        5000000, NULL, NULL);
    cbm_unsetenv("CBM_TEST_WALL_STALL_ON");
    free(src);
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 2000);
    cbm_free_result(r);
    PASS();
}

TEST(extract_parse_budget_still_trips_on_cpu) {
    /* A large parse against a 1 us CPU budget must still stop, and say why. */
    enum { LINES = 50000 };
    size_t cap = (size_t)LINES * 40;
    char *src = malloc(cap);
    ASSERT_NOT_NULL(src);
    size_t off = 0;
    for (int i = 0; i < LINES; i++) {
        off += (size_t)snprintf(src + off, cap - off, "def f%d(x):\n    return x + %d\n", i, i);
    }
    CBMFileResult *r =
        cbm_extract_file(src, (int)off, CBM_LANG_PYTHON, "t", "big.py", 1, NULL, NULL);
    free(src);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(r->has_error);
    ASSERT_STR_EQ(r->error_msg, "parse timeout");
    cbm_free_result(r);
    PASS();
}

/* --- R: box::use imports (#218) + module$fn calls (#219) --- */
TEST(extract_r_box_use_imports_issue218) {
    CBMFileResult *r = extract("box::use(\n"
                               "  shiny[moduleServer, NS],\n"
                               "  app/logic/validation[validate_input],\n"
                               ")\n"
                               "library(dplyr)\n"
                               "source(\"helpers.R\")\n",
                               CBM_LANG_R, "t", "app.R");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* box::use specs → one IMPORTS edge per module (symbol list stripped). */
    ASSERT(has_import(r, "shiny"));
    ASSERT(has_import(r, "app/logic/validation"));
    /* base-R imports work too. */
    ASSERT(has_import(r, "dplyr"));
    ASSERT(has_import(r, "helpers"));
    cbm_free_result(r);
    PASS();
}

TEST(extract_r_dollar_call_issue219) {
    CBMFileResult *r = extract("validation$validate_input(x)\n", CBM_LANG_R, "t", "app.R");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* module$fn() now produces a CALLS edge (was silently dropped). */
    ASSERT(has_call(r, "validation.validate_input"));
    cbm_free_result(r);
    PASS();
}

/* --- TS: object-literal arrow methods from a factory (Zustand, #341) --- */
TEST(extract_ts_factory_object_methods_issue341) {
    CBMFileResult *r = extract("export function createItemActions(set, get) {\n"
                               "  return {\n"
                               "    addItem: (type, id) => { return 1; },\n"
                               "    moveItem: (id, target) => { return 2; },\n"
                               "    deleteItem: (id) => { return 3; },\n"
                               "  };\n"
                               "}\n",
                               CBM_LANG_TYPESCRIPT, "t", "item-actions.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* The factory itself + each returned arrow method are Function nodes. */
    ASSERT(has_def_any(r, "createItemActions"));
    ASSERT(has_def_any(r, "addItem"));
    ASSERT(has_def_any(r, "moveItem"));
    ASSERT(has_def_any(r, "deleteItem"));
    cbm_free_result(r);
    PASS();
}

/* #2010, split out of #1997: AST traversal stacks were allocated from
 * result->arena, which the parallel pass holds for every file until the whole
 * result cache is freed, so a one-file scratch structure was retained for the
 * length of the index. cbm_extract_channels runs for every file and dispatches
 * TypeScript to extract_channels_js, whose two walks take a 4096-entry TSNode
 * stack each, and the ES import walk takes a 512-entry one:
 * 2 * 4096 * 32 + 512 * 32 = 278528 bytes charged to the arena of a one-line
 * file.
 *
 * The bound is derived, not tuned. Measured on this source, total_alloc was
 * 365984 before the scratch arena and is 87456 after, exactly that difference.
 * Of the 87456 that remain, 7680 is the defs item array at GROW_ARRAY's
 * starting capacity of 32 times sizeof(CBMDefinition) 240, and the other 79776
 * is everything else this file's extraction interns; none of it is traversal
 * scratch. So the bound sits above 87456 with room and a factor of four below
 * 365984.
 *
 * It is a byte budget, not a proof of lifetime; that is
 * extract_traversal_stacks_come_from_ctx_scratch_issue2010 in test_mem.c. */
TEST(traversal_stack_not_in_result_arena_issue2010) {
    CBMFileResult *r = extract("export const x = 1;\n", CBM_LANG_TYPESCRIPT, "t", "a.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_TRUE(has_def_any(r, "x"));
    /* Read the field rather than cbm_arena_total(): this file sees
     * internal/cbm/arena.h, which declares a subset of the API. test_mem.c
     * includes foundation/arena.h ahead of cbm.h and can call the accessor. */
    ASSERT_LT(r->arena.total_alloc, (size_t)CBM_SZ_128 * CBM_SZ_1K);
    cbm_free_result(r);
    PASS();
}

/* Not a scratch test. The C and C++ preprocessed second pass builds its own
 * extraction context (pp_ctx in cbm_extract_file_ex), and that context carries
 * ctx->scratch so every context in the file is uniform, but nothing reads it
 * there: pp_ctx reaches only cbm_extract_unified and cbm_run_c_lsp, and neither
 * extract_unified.c nor anything under internal/cbm/lsp/ includes
 * extract_node_stack.h, so no traversal stack is built on that path today.
 * This guards the macro-expansion path itself, which had no assertion on a call
 * that exists only after expansion. */
TEST(extract_c_macro_hidden_call_survives_preprocessed_pass_issue2010) {
    CBMFileResult *r = extract("void target(void) {}\n"
                               "#define INVOKE() target()\n"
                               "void caller(void) { INVOKE(); }\n",
                               CBM_LANG_C, "t", "macro_call.c");
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(has_call(r, "target"));
    cbm_free_result(r);
    PASS();
}

/* --- C/C++ preprocessor macros become Macro nodes (#375) --- */
TEST(extract_c_macros_issue375) {
    CBMFileResult *r = extract("#define SIMPLE_MACRO 1\n"
                               "#define FN_MACRO(x) (2 * (x))\n"
                               "#define EMPTY_MACRO\n"
                               "int main(void) { return FN_MACRO(SIMPLE_MACRO); }\n",
                               CBM_LANG_C, "p", "macros.c");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Macro", "SIMPLE_MACRO"));
    ASSERT(has_def(r, "Macro", "FN_MACRO"));
    ASSERT(has_def(r, "Macro", "EMPTY_MACRO"));
    ASSERT(has_def(r, "Function", "main")); /* macros don't displace function defs */
    cbm_free_result(r);
    PASS();
}

TEST(extract_cpp_macros_issue375) {
    CBMFileResult *r = extract("#define MAX(a, b) ((a) > (b) ? (a) : (b))\n"
                               "#define PI 3.14159\n"
                               "namespace n {\n"
                               "int f() { return MAX(1, 2); }\n"
                               "}\n",
                               CBM_LANG_CPP, "p", "macros.cpp");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Macro", "MAX"));
    ASSERT(has_def(r, "Macro", "PI"));
    cbm_free_result(r);
    PASS();
}

/* --- GDScript: AST -> graph visitor (Godot, #186) --- */
TEST(extract_gdscript_issue186) {
    CBMFileResult *r = extract("extends Node\n"
                               "class_name Player\n"
                               "\n"
                               "var health = 100\n"
                               "\n"
                               "func _ready():\n"
                               "    take_damage(10)\n"
                               "\n"
                               "func take_damage(amount):\n"
                               "    health -= amount\n"
                               "\n"
                               "class Inner:\n"
                               "    func helper():\n"
                               "        pass\n",
                               CBM_LANG_GDSCRIPT, "game", "player.gd");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "_ready"));
    ASSERT(has_def(r, "Function", "take_damage"));
    ASSERT(has_def(r, "Class", "Inner"));
    ASSERT(has_call(r, "take_damage"));
    cbm_free_result(r);
    PASS();
}

/* --- PowerShell: AST -> graph visitor (#35) --- */
TEST(extract_powershell_issue35) {
    CBMFileResult *r = extract("function Get-Greeting {\n"
                               "    param($Name)\n"
                               "    Write-Output \"Hello $Name\"\n"
                               "}\n"
                               "\n"
                               "function Set-Config {\n"
                               "    Get-Greeting -Name 'World'\n"
                               "}\n",
                               CBM_LANG_POWERSHELL, "ops", "greet.ps1");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(count_defs_with_label(r, "Function") >= 2);
    cbm_free_result(r);
    PASS();
}

/* --- Luau: AST -> graph visitor (Roblox, #39) --- */
TEST(extract_luau_issue39) {
    CBMFileResult *r = extract("local function add(a, b)\n"
                               "    return a + b\n"
                               "end\n"
                               "\n"
                               "function multiply(a, b)\n"
                               "    return add(a, a) * b\n"
                               "end\n",
                               CBM_LANG_LUAU, "game", "math.luau");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(count_defs_with_label(r, "Function") >= 2);
    cbm_free_result(r);
    PASS();
}

/* --- QML: AST -> graph visitor (Qt, #42) --- */
TEST(extract_qml_issue42) {
    CBMFileResult *r = extract("import QtQuick 2.15\n"
                               "\n"
                               "Rectangle {\n"
                               "    id: root\n"
                               "    width: 100\n"
                               "    property int counter: 0\n"
                               "    signal clicked(int value)\n"
                               "\n"
                               "    function increment() {\n"
                               "        counter += 1\n"
                               "        compute(counter)\n"
                               "    }\n"
                               "\n"
                               "    function compute(n) {\n"
                               "        return n * 2\n"
                               "    }\n"
                               "}\n",
                               CBM_LANG_QML, "app", "Main.qml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "increment"));
    ASSERT(has_def(r, "Function", "compute"));
    ASSERT(has_call(r, "compute"));
    cbm_free_result(r);
    PASS();
}

/* --- CFML script dialect: .cfc components (Lucee/ColdFusion, #38) --- */
TEST(extract_cfscript_issue38) {
    CBMFileResult *r = extract("component {\n"
                               "    public function getUser(numeric id) {\n"
                               "        return loadUser(id);\n"
                               "    }\n"
                               "    function loadUser(id) {\n"
                               "        return id * 2;\n"
                               "    }\n"
                               "}\n",
                               CBM_LANG_CFSCRIPT, "app", "User.cfc");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(count_defs_with_label(r, "Function") >= 2);
    ASSERT(has_def(r, "Function", "getUser"));
    ASSERT(has_def(r, "Function", "loadUser"));
    ASSERT(has_call(r, "loadUser"));
    cbm_free_result(r);
    PASS();
}

/* --- CFML tag dialect: .cfm templates with <cffunction> (#38) --- */
TEST(extract_cfml_tag_issue38) {
    CBMFileResult *r = extract("<cffunction name=\"greet\" returntype=\"string\">\n"
                               "    <cfargument name=\"who\" type=\"string\">\n"
                               "    <cfreturn \"Hello \" & arguments.who>\n"
                               "</cffunction>\n",
                               CBM_LANG_CFML, "app", "index.cfm");
    ASSERT_NOT_NULL(r);
    ASSERT(has_def(r, "Function", "greet"));
    cbm_free_result(r);
    PASS();
}

/* --- Helm / Go template: named templates + include calls (#338) --- */
TEST(extract_helm_templates_issue338) {
    CBMFileResult *r = extract("{{- define \"chart.fullname\" -}}\n"
                               "{{- .Release.Name -}}\n"
                               "{{- end -}}\n"
                               "\n"
                               "{{- define \"chart.labels\" -}}\n"
                               "app: {{ include \"chart.fullname\" . }}\n"
                               "chart: {{ template \"chart.fullname\" . }}\n"
                               "{{- end -}}\n",
                               CBM_LANG_GOTEMPLATE, "chart", "templates/_helpers.tpl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* define -> Function nodes */
    ASSERT(has_def(r, "Function", "chart.fullname"));
    ASSERT(has_def(r, "Function", "chart.labels"));
    /* include / template -> CALLS to the named template (not to "include") */
    ASSERT(has_call(r, "chart.fullname"));
    cbm_free_result(r);
    PASS();
}

/* --- Helm values.yaml: top-level keys only, no leaf flood (#338) --- */
TEST(extract_helm_values_toplevel_issue338) {
    CBMFileResult *r = extract("image:\n"
                               "  repository: nginx\n"
                               "  tag: latest\n"
                               "replicaCount: 3\n"
                               "service:\n"
                               "  port: 80\n",
                               CBM_LANG_YAML, "chart", "values.yaml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Variable", "image"));
    ASSERT(has_def(r, "Variable", "replicaCount"));
    ASSERT(has_def(r, "Variable", "service"));
    /* Nested leaf keys must NOT explode into separate nodes. */
    ASSERT(!has_def(r, "Variable", "repository"));
    ASSERT(!has_def(r, "Variable", "tag"));
    ASSERT(!has_def(r, "Variable", "port"));
    ASSERT_EQ(count_defs_with_label(r, "Variable"), 3);
    cbm_free_result(r);
    PASS();
}

/* --- Java --- */
TEST(java_class) {
    CBMFileResult *r = extract(
        "public class Animal { private String name; public String getName() { return name; } }",
        CBM_LANG_JAVA, "t", "Animal.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Animal"));
    cbm_free_result(r);
    PASS();
}

TEST(java_method) {
    CBMFileResult *r = extract(
        "public class Svc { public void doWork() {} public int compute(int x) { return x; } }",
        CBM_LANG_JAVA, "t", "Svc.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Method", "doWork"));
    ASSERT(has_def(r, "Method", "compute"));
    cbm_free_result(r);
    PASS();
}

TEST(java_interface) {
    CBMFileResult *r =
        extract("public interface Repository { void save(Object o); Object findById(long id); }",
                CBM_LANG_JAVA, "t", "Repo.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def_any(r, "Repository"));
    cbm_free_result(r);
    PASS();
}

/* Regression for #1234: Java interface/enum methods were emitted as both a
 * Method node (correct, via extract_class_methods) and a duplicate Function
 * node (incorrect, via walk_defs). Prevention in push_class_body_children
 * (gated to Java) recognizes interface_body and enum_body as class body
 * containers, stopping the fallback path from re-walking method_declaration
 * children as top-level functions. */
TEST(java_interface_no_duplicate_function_issue1234) {
    CBMFileResult *r =
        extract("public interface MarketplaceService {\n"
                "    ReservationDTO createReservation(Authentication auth, RequestDTO req);\n"
                "    void cancelReservation(long id);\n"
                "}\n",
                CBM_LANG_JAVA, "t", "MarketplaceService.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    ASSERT(has_def(r, "Interface", "MarketplaceService"));
    ASSERT(has_def(r, "Method", "createReservation"));
    ASSERT(has_def(r, "Method", "cancelReservation"));
    ASSERT_EQ(count_defs_with_label(r, "Function"), 0);

    cbm_free_result(r);
    PASS();
}

TEST(java_enum_dedup_preserves_calls_issue1234) {
    CBMFileResult *r =
        extract("package app;\n\nenum Day {\n"
                "    MON, TUE, WED, THU, FRI, SAT, SUN;\n\n"
                "    public boolean isWeekend() { return this == SAT || this == SUN; }\n"
                "    public String label() { return name().toLowerCase(); }\n}\n\n"
                "class DayUtil {\n"
                "    static String describe(Day d) {\n"
                "        return d.label() + (d.isWeekend() ? \"(rest)\" : \"(work)\");\n    }\n}\n",
                CBM_LANG_JAVA, "t", "Day.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    ASSERT(has_def(r, "Enum", "Day"));
    ASSERT(has_def(r, "Method", "isWeekend"));
    ASSERT(has_def(r, "Method", "label"));
    ASSERT(has_def(r, "Class", "DayUtil"));
    ASSERT(has_def(r, "Method", "describe"));
    ASSERT_EQ(count_defs_with_label(r, "Function"), 0);

    cbm_free_result(r);
    PASS();
}

/* Regression for #91: Scala, Solidity and Ruby class bodies were not
 * recognized by push_class_body_children, so every method was emitted as a
 * Method and again as a free Function. The rest of the body must still be
 * walked: Scala vals and Ruby constants are only found by the walk. */
TEST(scala_class_no_duplicate_function_issue91) {
    CBMFileResult *r = extract("class A {\n"
                               "  val limit: Int = 3\n"
                               "  def foo(): Int = 1\n"
                               "  object Inner {\n"
                               "    def innerFoo(): Int = 2\n"
                               "  }\n"
                               "}\n"
                               "trait T {\n"
                               "  def tFoo(): Int = 5\n"
                               "}\n",
                               CBM_LANG_SCALA, "t", "A.scala");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    ASSERT_EQ(count_defs_named(r, "Method", "foo"), 1);
    ASSERT_EQ(count_defs_named(r, "Method", "innerFoo"), 1);
    ASSERT_EQ(count_defs_named(r, "Method", "tFoo"), 1);
    ASSERT_EQ(count_defs_with_label(r, "Function"), 0);
    ASSERT(has_def(r, "Variable", "limit"));

    cbm_free_result(r);
    PASS();
}

TEST(solidity_contract_no_duplicate_function_issue91) {
    CBMFileResult *r =
        extract("pragma solidity ^0.8.0;\n"
                "contract B {\n"
                "    uint256 public total;\n"
                "    modifier onlyOwner() { _; }\n"
                "    function foo() public pure returns (uint) { return 1; }\n"
                "}\n"
                "interface IB {\n"
                "    function bar() external;\n"
                "}\n"
                "library LB {\n"
                "    function baz() internal pure returns (uint) { return 2; }\n"
                "}\n",
                CBM_LANG_SOLIDITY, "t", "B.sol");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    ASSERT_EQ(count_defs_named(r, "Method", "foo"), 1);
    ASSERT_EQ(count_defs_named(r, "Method", "onlyOwner"), 1);
    ASSERT_EQ(count_defs_named(r, "Method", "bar"), 1);
    ASSERT_EQ(count_defs_named(r, "Method", "baz"), 1);
    ASSERT_EQ(count_defs_with_label(r, "Function"), 0);

    cbm_free_result(r);
    PASS();
}

TEST(ruby_class_no_duplicate_function_issue91) {
    CBMFileResult *r = extract("class C\n"
                               "  LIMIT = 3\n"
                               "  def foo\n"
                               "    1\n"
                               "  end\n"
                               "  def self.sfoo\n"
                               "    2\n"
                               "  end\n"
                               "  class << self\n"
                               "    def cfoo\n"
                               "      3\n"
                               "    end\n"
                               "  end\n"
                               "end\n",
                               CBM_LANG_RUBY, "t", "c.rb");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    ASSERT_EQ(count_defs_named(r, "Method", "foo"), 1);
    ASSERT_EQ(count_defs_named(r, "Function", "foo"), 0);
    ASSERT_EQ(count_defs_named(r, "Method", "sfoo"), 1);
    ASSERT_EQ(count_defs_named(r, "Function", "sfoo"), 0);
    /* Not a direct body member, so extract_class_methods never sees it: the
     * walk must keep reaching it. */
    ASSERT(has_def_any(r, "cfoo"));
    ASSERT(has_def(r, "Variable", "LIMIT"));

    cbm_free_result(r);
    PASS();
}

/* Regression for #279: a Java class declaring both `extends` and
 * `implements` must produce one INHERITS edge per base — the extends parent
 * AND every implements interface — with bare type names (not the keyword
 * text "extends Bar" / "implements Baz, Qux"). Before the fix:
 *   1) the field loop returned on the first match → only the superclass
 *      was emitted, the interfaces were dropped.
 *   2) the emitted name was the full field text including the keyword. */
TEST(java_class_extends_and_implements) {
    CBMFileResult *r = extract("public class DefaultLinkTool extends DefaultDiagramTool implements "
                               "ILinkTool, Closeable { }",
                               CBM_LANG_JAVA, "t", "DefaultLinkTool.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    /* Find the class def and inspect its base_classes list. */
    CBMDefinition *cls = NULL;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, "Class") == 0 &&
            strcmp(r->defs.items[i].name, "DefaultLinkTool") == 0) {
            cls = &r->defs.items[i];
            break;
        }
    }
    ASSERT_NOT_NULL(cls);
    ASSERT_NOT_NULL(cls->base_classes);

    bool saw_super = false;
    bool saw_iface_a = false;
    bool saw_iface_b = false;
    for (const char **b = cls->base_classes; *b; b++) {
        /* The keyword-text bug would surface as "extends ..." or
         * "implements ..." literally inside one of the entries. */
        ASSERT_NULL(strstr(*b, "extends"));
        ASSERT_NULL(strstr(*b, "implements"));
        if (strcmp(*b, "DefaultDiagramTool") == 0)
            saw_super = true;
        if (strcmp(*b, "ILinkTool") == 0)
            saw_iface_a = true;
        if (strcmp(*b, "Closeable") == 0)
            saw_iface_b = true;
    }
    ASSERT_TRUE(saw_super);
    ASSERT_TRUE(saw_iface_a);
    ASSERT_TRUE(saw_iface_b);

    cbm_free_result(r);
    PASS();
}

/* REPRODUCTION (RED until fixed) — Python `class Animal(Base):` must extract the
 * BARE base name "Base", but extract_base_classes captures the whole
 * `superclasses` argument_list text "(Base)" instead: collect_bases_from_field
 * (internal/cbm/extract_defs.c) matches only type_identifier / generic_type /
 * qualified_name / scoped_type_identifier / user_type, while tree-sitter-python
 * uses a plain `identifier` node for the base — so no child matches and the
 * raw-field fallback grabs the argument_list text "(Base)" (parens included).
 * DOWNSTREAM SYMPTOM: that malformed name never resolves to the Base class node,
 * so EVERY Python subclass yields ZERO INHERITS edges (observed in the P6
 * graph-contract suite). Fix: have collect_bases_from_field accept `identifier`
 * (or strip the argument_list parens). This test stays RED as the regression
 * guard / reproduction until the fix lands — see CLAUDE.md "Bug Fixing —
 * Reproduce-First". */
TEST(python_class_base_extracted_bare) {
    CBMFileResult *r = extract("class Base:\n    pass\n\n\nclass Animal(Base):\n    pass\n",
                               CBM_LANG_PYTHON, "t", "models.py");
    ASSERT_NOT_NULL(r);

    CBMDefinition *cls = NULL;
    for (int i = 0; i < r->defs.count; i++) {
        if (r->defs.items[i].label && strcmp(r->defs.items[i].label, "Class") == 0 &&
            r->defs.items[i].name && strcmp(r->defs.items[i].name, "Animal") == 0) {
            cls = &r->defs.items[i];
            break;
        }
    }
    ASSERT_NOT_NULL(cls);
    ASSERT_NOT_NULL(cls->base_classes); /* a subclass must record at least one base */

    bool saw_bare_base = false;
    for (const char **b = cls->base_classes; *b; b++) {
        if (strcmp(*b, "Base") == 0) {
            saw_bare_base = true;
        }
    }
    /* CURRENTLY FAILS: base_classes holds "(Base)" (argument_list text), not the
     * bare "Base" needed for INHERITS resolution. */
    ASSERT_TRUE(saw_bare_base);

    cbm_free_result(r);
    PASS();
}

/* --- PHP --- */
TEST(php_class) {
    CBMFileResult *r = extract("<?php\nclass User { public string $name; public function "
                               "getName(): string { return $this->name; } }",
                               CBM_LANG_PHP, "t", "User.php");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "User"));
    ASSERT(has_def(r, "Method", "getName"));
    cbm_free_result(r);
    PASS();
}

TEST(php_function) {
    CBMFileResult *r =
        extract("<?php\nfunction greet(string $name): string { return 'Hello ' . $name; }",
                CBM_LANG_PHP, "t", "helpers.php");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "greet"));
    cbm_free_result(r);
    PASS();
}

/* --- Ruby --- */
TEST(ruby_class) {
    CBMFileResult *r = extract("class Animal\n  def initialize(name)\n    @name = name\n  end\n  "
                               "def speak\n    puts @name\n  end\nend\n",
                               CBM_LANG_RUBY, "t", "animal.rb");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Animal"));
    ASSERT(has_def(r, "Method", "speak"));
    cbm_free_result(r);
    PASS();
}

TEST(ruby_module) {
    CBMFileResult *r = extract("module Greetable\n  def greet\n    \"Hello\"\n  end\nend\n",
                               CBM_LANG_RUBY, "t", "greetable.rb");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def_any(r, "Greetable"));
    cbm_free_result(r);
    PASS();
}

/* --- C# --- */
TEST(csharp_class) {
    CBMFileResult *r = extract("namespace App { public class Service { public void Run() {} public "
                               "int Compute(int x) => x * 2; } }",
                               CBM_LANG_CSHARP, "t", "Service.cs");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Service"));
    ASSERT(has_def(r, "Method", "Run"));
    cbm_free_result(r);
    PASS();
}

TEST(csharp_interface) {
    CBMFileResult *r = extract("public interface IService { void Execute(); string GetStatus(); }",
                               CBM_LANG_CSHARP, "t", "IService.cs");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def_any(r, "IService"));
    cbm_free_result(r);
    PASS();
}

/* --- Swift --- */
TEST(swift_class) {
    CBMFileResult *r = extract("class Vehicle {\n    var speed: Int = 0\n    func accelerate() { "
                               "speed += 10 }\n    func stop() { speed = 0 }\n}\n",
                               CBM_LANG_SWIFT, "t", "Vehicle.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Vehicle"));
    ASSERT(has_def(r, "Method", "accelerate"));
    cbm_free_result(r);
    PASS();
}

TEST(swift_protocol) {
    /* A protocol requirement is a bodyless func inside a protocol body. Swift
     * codebases are heavily protocol-driven, so the requirement is very often
     * the declaration a reader is actually looking for — before this it was
     * absent from the graph entirely. */
    CBMFileResult *r = extract("protocol StudyRunning {\n    func generate() -> String\n}\n",
                               CBM_LANG_SWIFT, "t", "StudyRunning.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Interface", "StudyRunning"));
    ASSERT(has_def(r, "Method", "generate"));
    cbm_free_result(r);
    PASS();
}

/* --- Kotlin --- */
TEST(kotlin_function) {
    CBMFileResult *r = extract("fun greet(name: String): String = \"Hello $name\"\nfun main() { "
                               "println(greet(\"World\")) }\n",
                               CBM_LANG_KOTLIN, "t", "main.kt");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "greet"));
    ASSERT(has_def(r, "Function", "main"));
    cbm_free_result(r);
    PASS();
}

TEST(kotlin_class) {
    CBMFileResult *r =
        extract("class User(val name: String) {\n    fun display(): String = \"User: $name\"\n}\n",
                CBM_LANG_KOTLIN, "t", "User.kt");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "User"));
    cbm_free_result(r);
    PASS();
}

/* --- Scala --- */
TEST(scala_function) {
    CBMFileResult *r =
        extract("object Main {\n  def greet(name: String): String = s\"Hello $name\"\n  def "
                "main(args: Array[String]): Unit = println(greet(\"World\"))\n}\n",
                CBM_LANG_SCALA, "t", "Main.scala");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Method", "greet"));
    cbm_free_result(r);
    PASS();
}

TEST(scala_class) {
    CBMFileResult *r =
        extract("class Animal(val name: String) {\n  def speak(): String = s\"I am $name\"\n}\n",
                CBM_LANG_SCALA, "t", "Animal.scala");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Animal"));
    cbm_free_result(r);
    PASS();
}

/* --- Dart --- */
TEST(dart_class) {
    CBMFileResult *r = extract("class Animal {\n  String name;\n  Animal(this.name);\n  String "
                               "speak() => 'I am $name';\n}\n",
                               CBM_LANG_DART, "t", "animal.dart");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Animal"));
    ASSERT(has_def(r, "Method", "speak"));
    cbm_free_result(r);
    PASS();
}

/* --- Dart extension members (#1457) --- */
TEST(dart_extension_members) {
    CBMFileResult *r =
        extract("class Holder {\n  final int value;\n  const Holder(this.value);\n}\n\n"
                "extension HolderMath on Holder {\n  int doubled() => value + value;\n"
                "  int tripled() => value + value + value;\n"
                "  String describeHolder() => 'holder';\n}\n\n"
                "int topLevelHelper(int x) => x + 1;\n",
                CBM_LANG_DART, "t", "a.dart");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Holder"));
    ASSERT(has_def(r, "Function", "topLevelHelper"));
    ASSERT(has_def(r, "Class", "HolderMath"));
    ASSERT(has_def(r, "Method", "doubled"));
    ASSERT(has_def(r, "Method", "tripled"));
    ASSERT(has_def(r, "Method", "describeHolder"));
    /* Members hang off the extension (DEFINES_METHOD source), not the file. */
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (d->name && strcmp(d->name, "doubled") == 0) {
            ASSERT_NOT_NULL(d->parent_class);
            ASSERT_STR_EQ(d->parent_class, "t.a.HolderMath");
        }
    }
    cbm_free_result(r);

    /* Unnamed extension: its members are still indexed. */
    r = extract("extension on String {\n  bool isShout() => this == toUpperCase();\n}\n",
                CBM_LANG_DART, "t", "b.dart");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def_any(r, "isShout"));
    cbm_free_result(r);
    PASS();
}

/* --- Groovy --- */
TEST(groovy_class) {
    CBMFileResult *r =
        extract("class Greeter {\n    String name\n    String greet() { \"Hello, $name\" }\n    "
                "static void main(args) { println new Greeter(name:'World').greet() }\n}\n",
                CBM_LANG_GROOVY, "t", "Greeter.groovy");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Greeter"));
    ASSERT(has_def(r, "Method", "greet"));
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group B: Systems Languages
 * ═══════════════════════════════════════════════════════════════════ */

/* --- Rust --- */
TEST(rust_function) {
    CBMFileResult *r =
        extract("fn main() { println!(\"Hello\"); }\npub fn add(a: i32, b: i32) -> i32 { a + b }\n",
                CBM_LANG_RUST, "t", "main.rs");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "main"));
    ASSERT(has_def(r, "Function", "add"));
    cbm_free_result(r);
    PASS();
}

TEST(rust_struct) {
    CBMFileResult *r = extract("pub struct Point { pub x: f64, pub y: f64 }\nimpl Point { pub fn "
                               "new(x: f64, y: f64) -> Self { Point { x, y } } }\n",
                               CBM_LANG_RUST, "t", "point.rs");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Struct", "Point"));
    ASSERT(has_def(r, "Method", "new"));
    cbm_free_result(r);
    PASS();
}

/* --- Go --- */
TEST(go_function) {
    CBMFileResult *r = extract("package main\nfunc Greet(name string) string { return \"Hello, \" "
                               "+ name }\nfunc main() { Greet(\"World\") }\n",
                               CBM_LANG_GO, "t", "main.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "Greet"));
    ASSERT(has_def(r, "Function", "main"));
    cbm_free_result(r);
    PASS();
}

TEST(go_struct) {
    CBMFileResult *r = extract("package main\ntype Server struct { Host string; Port int }\nfunc "
                               "(s *Server) Start() error { return nil }\n",
                               CBM_LANG_GO, "t", "server.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Struct", "Server"));
    ASSERT(has_def(r, "Method", "Start"));
    cbm_free_result(r);
    PASS();
}

TEST(go_interface) {
    CBMFileResult *r =
        extract("package main\ntype Handler interface { ServeHTTP() error; Close() }\n",
                CBM_LANG_GO, "t", "handler.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def_any(r, "Handler"));
    cbm_free_result(r);
    PASS();
}

/* --- Zig --- */
TEST(zig_function) {
    CBMFileResult *r =
        extract("const std = @import(\"std\");\npub fn add(a: i32, b: i32) i32 { return a + b; }\n",
                CBM_LANG_ZIG, "t", "main.zig");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "add"));
    cbm_free_result(r);
    PASS();
}

/* --- C --- */
TEST(c_function) {
    CBMFileResult *r =
        extract("int add(int a, int b) { return a + b; }\nvoid greet() { printf(\"Hello\"); }\n",
                CBM_LANG_C, "t", "math.c");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "add"));
    ASSERT(has_def(r, "Function", "greet"));
    cbm_free_result(r);
    PASS();
}

TEST(c_struct) {
    CBMFileResult *r =
        extract("struct Point { int x; int y; };\nvoid init_point(struct Point *p) { p->x = 0; }\n",
                CBM_LANG_C, "t", "point.c");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "init_point"));
    cbm_free_result(r);
    PASS();
}

/* Linux arch/x86/kernel/espfix_64.c reaches this expression without the
 * generated kernel configuration includes. Undefined identifiers become 0
 * during preprocessing, so the derived shift count is negative. Extraction
 * must reject/fold that expression without executing an invalid C++ shift. */
TEST(c_preprocessor_invalid_shift_does_not_trap) {
    CBMFileResult *r = extract(
        "#define ESPFIX_PAGE_SPACE (1UL << (P4D_SHIFT-PAGE_SHIFT-16))\n"
        "#define ESPFIX_MAX_CPUS (ESPFIX_STACKS_PER_PAGE * ESPFIX_PAGE_SPACE)\n"
        "#if CONFIG_NR_CPUS > ESPFIX_MAX_CPUS\n"
        "#error too many CPUs\n"
        "#endif\n"
        "void init_espfix(void) {}\n",
        CBM_LANG_C, "linux", "arch/x86/kernel/espfix_64.c");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "init_espfix"));
    cbm_free_result(r);
    PASS();
}

/* --- C++ --- */
TEST(cpp_class) {
    CBMFileResult *r = extract(
        "class Widget {\npublic:\n    void draw() {}\n    int width() const { return 0; }\n};\n",
        CBM_LANG_CPP, "t", "widget.cpp");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Widget"));
    ASSERT(has_def(r, "Method", "draw"));
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group C: Scripting / Dynamic Languages
 * ═══════════════════════════════════════════════════════════════════ */

/* --- Python --- */
TEST(python_function) {
    CBMFileResult *r = extract(
        "def greet(name):\n    return f\"Hello {name}\"\n\ndef main():\n    greet(\"World\")\n",
        CBM_LANG_PYTHON, "t", "main.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "greet"));
    ASSERT(has_def(r, "Function", "main"));
    cbm_free_result(r);
    PASS();
}

TEST(python_class) {
    CBMFileResult *r =
        extract("class Dog:\n    def __init__(self, name):\n        self.name = name\n    def "
                "speak(self):\n        return f\"Woof from {self.name}\"\n",
                CBM_LANG_PYTHON, "t", "dog.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Dog"));
    ASSERT(has_def(r, "Method", "speak"));
    cbm_free_result(r);
    PASS();
}

/* --- JavaScript --- */
TEST(js_function) {
    CBMFileResult *r =
        extract("function greet(name) { return `Hello ${name}`; }\nconst add = (a, b) => a + b;\n",
                CBM_LANG_JAVASCRIPT, "t", "util.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "greet"));
    cbm_free_result(r);
    PASS();
}

TEST(js_class) {
    CBMFileResult *r =
        extract("class Counter {\n  constructor() { this.count = 0; }\n  increment() { "
                "this.count++; }\n  get value() { return this.count; }\n}\n",
                CBM_LANG_JAVASCRIPT, "t", "counter.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Counter"));
    ASSERT(has_def(r, "Method", "increment"));
    cbm_free_result(r);
    PASS();
}

/* --- TypeScript --- */
TEST(ts_function) {
    CBMFileResult *r = extract("export function greet(name: string): string { return `Hello "
                               "${name}`; }\nfunction helper(): void {}\n",
                               CBM_LANG_TYPESCRIPT, "t", "util.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "greet"));
    cbm_free_result(r);
    PASS();
}

TEST(ts_class) {
    CBMFileResult *r =
        extract("class Service {\n  private name: string;\n  constructor(name: string) { this.name "
                "= name; }\n  getName(): string { return this.name; }\n}\n",
                CBM_LANG_TYPESCRIPT, "t", "service.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Service"));
    cbm_free_result(r);
    PASS();
}

/* --- Lua --- */
TEST(lua_function) {
    CBMFileResult *r = extract(
        "function greet(name)\n  return \"Hello \" .. name\nend\nlocal function helper() end\n",
        CBM_LANG_LUA, "t", "main.lua");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "greet"));
    cbm_free_result(r);
    PASS();
}

/* --- Bash --- */
TEST(bash_function) {
    CBMFileResult *r =
        extract("greet() {\n  echo \"Hello $1\"\n}\nmain() {\n  greet \"World\"\n}\n",
                CBM_LANG_BASH, "t", "script.sh");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "greet"));
    cbm_free_result(r);
    PASS();
}

/* --- Perl --- */
TEST(perl_function) {
    CBMFileResult *r = extract("sub greet {\n    my ($name) = @_;\n    return \"Hello "
                               "$name\";\n}\nsub main { greet(\"World\"); }\n",
                               CBM_LANG_PERL, "t", "main.pl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "greet"));
    cbm_free_result(r);
    PASS();
}

/* --- R --- */
TEST(r_function) {
    CBMFileResult *r = extract("add <- function(x, y) x + y\nmultiply <- function(x, y) x * y\n",
                               CBM_LANG_R, "t", "math.R");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "add"));
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group D: Functional Languages
 * ═══════════════════════════════════════════════════════════════════ */

/* --- Elixir --- */
TEST(elixir_function) {
    CBMFileResult *r = extract("defmodule Greeter do\n  def greet(name), do: \"Hello #{name}\"\n  "
                               "defp helper, do: nil\nend\n",
                               CBM_LANG_ELIXIR, "t", "greeter.ex");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "greet"));
    cbm_free_result(r);
    PASS();
}

/* tree-sitter-elixir gives a call's arguments node no field name, so the
 * generic `arguments` field lookup returns null and first_string_arg was never
 * populated for any Elixir call — Phoenix route paths, service URLs and config
 * keys all key off it. */
TEST(elixir_call_string_argument) {
    CBMFileResult *r = extract("defmodule Sample do\n"
                               "  def run do\n"
                               "    get(\"/wallets\", WalletController)\n"
                               "  end\n"
                               "end\n",
                               CBM_LANG_ELIXIR, "t", "sample.ex");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int seen = 0;
    for (int i = 0; i < r->calls.count; i++) {
        if (strcmp(r->calls.items[i].callee_name, "get") != 0) {
            continue;
        }
        seen = 1;
        ASSERT_NOT_NULL(r->calls.items[i].first_string_arg);
        ASSERT_STR_EQ("/wallets", r->calls.items[i].first_string_arg);
    }
    ASSERT_EQ(1, seen);
    cbm_free_result(r);
    PASS();
}

/* --- Haskell --- */
TEST(haskell_function) {
    CBMFileResult *r = extract("add :: Int -> Int -> Int\nadd x y = x + y\n\nmultiply :: Int -> "
                               "Int -> Int\nmultiply x y = x * y\n",
                               CBM_LANG_HASKELL, "t", "Math.hs");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "add"));
    cbm_free_result(r);
    PASS();
}

/* --- OCaml --- */
TEST(ocaml_function) {
    CBMFileResult *r =
        extract("let add x y = x + y\nlet multiply x y = x * y\n", CBM_LANG_OCAML, "t", "math.ml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "add"));
    cbm_free_result(r);
    PASS();
}

/* --- Erlang --- */
TEST(erlang_function) {
    CBMFileResult *r = extract(
        "-module(math).\n-export([add/2]).\nadd(X, Y) -> X + Y.\nmultiply(X, Y) -> X * Y.\n",
        CBM_LANG_ERLANG, "t", "math.erl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "add"));
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group E: Markup / Config / Helper Languages
 * ═══════════════════════════════════════════════════════════════════ */

/* --- YAML --- */
TEST(yaml_variables) {
    CBMFileResult *r =
        extract("name: myapp\nversion: 1.0\ndatabase:\n  host: localhost\n  port: 5432\n",
                CBM_LANG_YAML, "t", "config.yml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* YAML should extract top-level keys as variables */
    ASSERT_GT(r->defs.count, 0);
    cbm_free_result(r);
    PASS();
}

/* --- HCL --- */
TEST(hcl_blocks) {
    CBMFileResult *r = extract("resource \"aws_instance\" \"web\" {\n  ami = \"abc-123\"\n  "
                               "instance_type = \"t2.micro\"\n}\n"
                               "variable \"region\" {\n  default = \"us-east-1\"\n}\n",
                               CBM_LANG_HCL, "t", "main.tf");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->defs.count, 0);
    /* Block labels are folded into the name so blocks are distinguishable (#337). */
    ASSERT(has_def(r, "Class", "resource.aws_instance.web"));
    ASSERT(has_def(r, "Class", "variable.region"));
    cbm_free_result(r);
    PASS();
}

/* --- SQL --- */
TEST(sql_create_table) {
    CBMFileResult *r = extract("CREATE TABLE users (\n  id INTEGER PRIMARY KEY,\n  name TEXT NOT "
                               "NULL\n);\nCREATE VIEW active_users AS SELECT * FROM users;\n",
                               CBM_LANG_SQL, "t", "schema.sql");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

/* --- Dockerfile --- */
TEST(dockerfile_stages) {
    CBMFileResult *r = extract(
        "FROM node:18 AS builder\nRUN npm install\nFROM node:18-slim\nCOPY --from=builder /app .\n",
        CBM_LANG_DOCKERFILE, "t", "Dockerfile");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group F: Scientific / Math Languages
 * ═══════════════════════════════════════════════════════════════════ */

/* --- MATLAB --- */
TEST(matlab_function) {
    CBMFileResult *r =
        extract("function y = square(x)\n  y = x.^2;\nend\n", CBM_LANG_MATLAB, "t", "square.m");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "square"));
    cbm_free_result(r);
    PASS();
}

/* --- Lean 4 --- */
TEST(lean_function) {
    CBMFileResult *r =
        extract("def add (x y : Nat) : Nat := x + y\n", CBM_LANG_LEAN, "t", "Math.lean");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "add"));
    cbm_free_result(r);
    PASS();
}

/* --- FORM --- */
TEST(form_procedure) {
    CBMFileResult *r = extract("#procedure doSomething\n  id x = y;\n#endprocedure\n",
                               CBM_LANG_FORM, "t", "test.frm");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "doSomething"));
    cbm_free_result(r);
    PASS();
}

/* --- Wolfram --- */
TEST(wolfram_function) {
    CBMFileResult *r =
        extract("square[x_] := x^2\nadd[x_, y_] := x + y\n", CBM_LANG_WOLFRAM, "t", "math.wl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "square"));
    ASSERT(has_def(r, "Function", "add"));
    cbm_free_result(r);
    PASS();
}

/* --- Magma --- */
TEST(magma_function) {
    CBMFileResult *r = extract("function Factorial(n)\n  if n le 1 then\n    return 1;\n  end "
                               "if;\n  return n * Factorial(n - 1);\nend function;\n",
                               CBM_LANG_MAGMA, "t", "test.m");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "Factorial"));
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group G: v0.5 Expansion Languages
 * ═══════════════════════════════════════════════════════════════════ */

/* --- F# --- */
TEST(fsharp_function) {
    /* Go test only asserts >=1 def — F# name extraction is incomplete */
    CBMFileResult *r = extract("module Greeter\nlet greet name = sprintf \"Hello %s\" name\n",
                               CBM_LANG_FSHARP, "t", "Greeter.fs");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Julia --- */
TEST(julia_function) {
    CBMFileResult *r = extract("function add(x, y)\n    x + y\nend\nadd2(x, y) = x + y\n",
                               CBM_LANG_JULIA, "t", "math.jl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "add"));
    cbm_free_result(r);
    PASS();
}

/* --- Elm --- */
TEST(elm_function) {
    CBMFileResult *r =
        extract("add x y = x + y\nmultiply x y = x * y\n", CBM_LANG_ELM, "t", "Math.elm");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "add"));
    cbm_free_result(r);
    PASS();
}

/* --- Nix --- */
TEST(nix_function) {
    CBMFileResult *r =
        extract("{ pkgs ? import <nixpkgs> {} }:\nlet\n  hello = pkgs.writeShellScriptBin "
                "\"hello\" ''echo hello'';\nin { inherit hello; }\n",
                CBM_LANG_NIX, "t", "default.nix");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

/* --- Fortran --- */
TEST(fortran_function) {
    /* Fortran subroutine name extraction is incomplete — just verify no crash */
    CBMFileResult *r = extract("subroutine greet(name)\n  character(*), intent(in) :: name\n  "
                               "print *, 'Hello ', name\nend subroutine\n",
                               CBM_LANG_FORTRAN, "t", "greet.f90");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group A2: Missing OOP / Systems variants
 * ═══════════════════════════════════════════════════════════════════ */

/* --- Swift struct --- */
TEST(swift_struct) {
    CBMFileResult *r = extract("struct Point {\n    var x: Double\n    var y: Double\n    func "
                               "distance() -> Double { return (x*x + y*y).squareRoot() }\n}\n",
                               CBM_LANG_SWIFT, "t", "Point.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Method", "distance"));
    cbm_free_result(r);
    PASS();
}

/* --- Swift calls (port of PR #47 Go tests) --- */
TEST(swift_simple_call) {
    CBMFileResult *r = extract("func main() { greet() }\nfunc greet() { print(\"hello\") }\n",
                               CBM_LANG_SWIFT, "t", "main.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_call(r, "greet"));
    cbm_free_result(r);
    PASS();
}

TEST(swift_method_call) {
    CBMFileResult *r =
        extract("class Foo {\n    func bar() { baz.run() }\n}\n", CBM_LANG_SWIFT, "t", "Foo.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_call(r, "baz.run"));
    cbm_free_result(r);
    PASS();
}

TEST(swift_constructor_call) {
    CBMFileResult *r =
        extract("func create() { let x = MyClass() }\n", CBM_LANG_SWIFT, "t", "create.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_call(r, "MyClass"));
    cbm_free_result(r);
    PASS();
}

TEST(swift_chained_call) {
    CBMFileResult *r = extract("func setup() { AlarmScheduler.shared.startKeepAlive() }\n",
                               CBM_LANG_SWIFT, "t", "setup.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(r->calls.count > 0);
    cbm_free_result(r);
    PASS();
}

TEST(swift_force_unwrap) {
    CBMFileResult *r = extract(
        "func load() {\n"
        "    let url = URL(string: \"https://example.com\")!\n"
        "    let data = \"payload\".data(using: .utf8)!\n"
        "    print(url, data)\n"
        "}\n",
        CBM_LANG_SWIFT, "t", "ForceUnwrap.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

TEST(swift_try_bang) {
    CBMFileResult *r = extract(
        "func decode() throws -> String { return \"ok\" }\n"
        "func load() { let value = try! decode(); print(value) }\n",
        CBM_LANG_SWIFT, "t", "TryBang.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

/* --- Objective-C --- */
TEST(objc_interface) {
    CBMFileResult *r =
        extract("@interface Animal : NSObject\n- (NSString *)name;\n- (void)speak;\n@end\n",
                CBM_LANG_OBJC, "t", "Animal.h");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

TEST(objc_implementation) {
    CBMFileResult *r = extract("@implementation Animal\n- (NSString *)name { return @\"Animal\"; "
                               "}\n- (void)speak { NSLog(@\"...\"); }\n@end\n",
                               CBM_LANG_OBJC, "t", "Animal.m");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Dart top-level function --- */
TEST(dart_top_level_function) {
    CBMFileResult *r = extract(
        "void main() {\n  print('Hello');\n}\nString greet(String name) => 'Hello $name';\n",
        CBM_LANG_DART, "t", "main.dart");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "main"));
    ASSERT(has_def(r, "Function", "greet"));
    cbm_free_result(r);
    PASS();
}

/* --- Rust enum --- */
TEST(rust_enum) {
    CBMFileResult *r =
        extract("pub enum Direction { North, South, East, West }\n", CBM_LANG_RUST, "t", "dir.rs");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Zig struct --- */
TEST(zig_struct) {
    CBMFileResult *r = extract("const Point = struct { x: f32, y: f32, pub fn dist(self: Point) "
                               "f32 { return self.x + self.y; } };\n",
                               CBM_LANG_ZIG, "t", "point.zig");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Zig containers scope their members ---
 * `const Database = struct {...}` names the struct through its
 * variable_declaration. Its functions are Methods qualified by the struct, so
 * two structs' `deinit` stay distinct nodes, and the declaration is not also a
 * Variable that shadows the Struct. */
/* A re-export inside a struct is a container-qualified alias Variable;
 * `const Self = @This()` and nested containers are not. */
/* A container chosen by a compile-time `if` is named by its declaration: both
 * branches are the Struct, and their methods are its Methods. */
TEST(zig_comptime_if_container_is_named) {
    CBMFileResult *r = extract("const builtin = @import(\"builtin\");\n"
                               "const AbiMutex = if (builtin.os.tag == .emscripten) struct {\n"
                               "    pub fn lock(_: *@This()) void {}\n"
                               "} else struct {\n"
                               "    state: u32 = 0,\n"
                               "    pub fn lock(self: *AbiMutex) void { _ = self; }\n"
                               "};\n"
                               "pub const Handle = struct {\n"
                               "    mutex: AbiMutex = .{},\n"
                               "    fn run(h: *Handle) void { h.mutex.lock(); }\n"
                               "};\n",
                               CBM_LANG_ZIG, "t", "src/h.zig");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int structs = 0;
    int locks = 0;
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (strcmp(d->qualified_name, "t.src.h.AbiMutex") == 0 && strcmp(d->label, "Struct") == 0) {
            structs++;
        } else if (strcmp(d->qualified_name, "t.src.h.AbiMutex.lock") == 0) {
            ASSERT_STR_EQ(d->label, "Method");
            locks++;
        }
    }
    ASSERT_EQ(structs, 2);
    ASSERT_EQ(locks, 2);
    ASSERT_EQ(count_defs_named(r, "Variable", "AbiMutex"), 0);
    ASSERT_FALSE(has_def_qn(r, "t.src.h.lock"));
    ASSERT(has_def_qn(r, "t.src.h.AbiMutex.state"));
    cbm_free_result(r);
    PASS();
}

/* A type function names the containers it returns: their members are
 * `<fn>.<member>` and hang off the Function, which stands for the type (no
 * Struct def shares its QN). Calls inside them belong to those methods. */
TEST(zig_type_function_containers_are_named) {
    CBMFileResult *r = extract("pub fn Pool(comptime T: type) type {\n"
                               "    return struct {\n"
                               "        items: []T,\n"
                               "        pub fn first(self: *@This()) *T { return &self.items[0]; }\n"
                               "        pub fn grow(self: *@This()) void { self.first(); }\n"
                               "    };\n"
                               "}\n"
                               "fn Ops(comptime kind: u8) type {\n"
                               "    if (kind == 1) return struct { fn copy() void {} };\n"
                               "    return switch (kind) {\n"
                               "        0 => struct { fn copy() void {} },\n"
                               "        else => enum { a, b },\n"
                               "    };\n"
                               "}\n",
                               CBM_LANG_ZIG, "t", "src/p.zig");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_defs_named(r, "Function", "Pool"), 1);
    ASSERT_EQ(count_defs_named(r, "Struct", "Pool"), 0);
    int copies = 0;
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (strcmp(d->qualified_name, "t.src.p.Pool.first") == 0) {
            ASSERT_STR_EQ(d->label, "Method");
            ASSERT_STR_EQ(d->parent_class, "t.src.p.Pool");
        } else if (strcmp(d->qualified_name, "t.src.p.Ops.copy") == 0) {
            copies++;
        }
    }
    ASSERT_EQ(copies, 2);
    ASSERT(has_def_qn(r, "t.src.p.Pool.items"));
    ASSERT_EQ(count_defs_named(r, "Enum", "Ops"), 0);
    ASSERT_FALSE(has_def_qn(r, "t.src.p.first"));
    int grow_calls = 0;
    for (int i = 0; i < r->calls.count; i++) {
        const CBMCall *c = &r->calls.items[i];
        if (strcmp(c->callee_name, "self.first") == 0 || strcmp(c->callee_name, "first") == 0) {
            ASSERT_STR_EQ(c->enclosing_func_qn, "t.src.p.Pool.grow");
            grow_calls++;
        }
    }
    ASSERT_EQ(grow_calls, 1);
    cbm_free_result(r);
    PASS();
}

TEST(zig_container_aliases_are_scoped) {
    CBMFileResult *r = extract("const impl = @import(\"db.zig\");\n"
                               "pub const api = struct {\n"
                               "    const Self = @This();\n"
                               "    pub const Db = impl.Database;\n"
                               "    pub const sqlite = @import(\"sqlite.zig\");\n"
                               "    const Local = struct {};\n"
                               "    const limit: u32 = 4;\n"
                               "};\n",
                               CBM_LANG_ZIG, "t", "src/root.zig");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *db = NULL;
    const CBMDefinition *sq = NULL;
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (strcmp(d->qualified_name, "t.src.root.api.Db") == 0) {
            db = d;
        } else if (strcmp(d->qualified_name, "t.src.root.api.sqlite") == 0) {
            sq = d;
        }
    }
    ASSERT_NOT_NULL(db);
    ASSERT_STR_EQ(db->label, "Variable");
    ASSERT_STR_EQ(db->parent_class, "t.src.root.api");
    ASSERT_STR_EQ(db->alias_value, "impl.Database");
    ASSERT_NOT_NULL(sq);
    ASSERT_STR_EQ(sq->alias_value, "@import(\"sqlite.zig\")");
    ASSERT_EQ(count_defs_named(r, "Variable", "Self"), 0);
    ASSERT_EQ(count_defs_named(r, "Variable", "Local"), 0);
    ASSERT_EQ(count_defs_named(r, "Variable", "limit"), 0);
    ASSERT_EQ(count_defs_named(r, "Variable", "Db"), 1);
    cbm_free_result(r);
    PASS();
}

TEST(zig_struct_members_are_scoped) {
    CBMFileResult *r = extract("const std = @import(\"std\");\n"
                               "pub const Database = struct {\n"
                               "    handle: i32,\n"
                               "    const Self = @This();\n"
                               "    pub fn open() !Database { return .{ .handle = 0 }; }\n"
                               "    pub fn deinit(self: *Self) void { self.close(); }\n"
                               "    fn close(self: *Self) void { _ = self; }\n"
                               "    pub const Inner = struct {\n"
                               "        pub fn deinit(self: *Inner) void { _ = self; }\n"
                               "    };\n"
                               "    test \"inside\" { helper(); }\n"
                               "};\n"
                               "const Kind = enum { a, b, pub fn name(self: Kind) u8 { _ = self; "
                               "return 0; } };\n"
                               "fn helper() void {}\n",
                               CBM_LANG_ZIG, "t", "src/db.zig");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *db = NULL;
    const CBMDefinition *deinit = NULL;
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (strcmp(d->qualified_name, "t.src.db.Database") == 0) {
            db = d;
        } else if (strcmp(d->qualified_name, "t.src.db.Database.deinit") == 0) {
            deinit = d;
        }
    }
    ASSERT_NOT_NULL(db);
    ASSERT_STR_EQ(db->label, "Struct");
    ASSERT_NOT_NULL(deinit);
    ASSERT_STR_EQ(deinit->label, "Method");
    ASSERT_STR_EQ(deinit->parent_class, "t.src.db.Database");
    ASSERT(has_def_qn(r, "t.src.db.Database.open"));
    ASSERT(has_def_qn(r, "t.src.db.Database.close"));
    ASSERT(has_def_qn(r, "t.src.db.Database.handle"));
    ASSERT(has_def_qn(r, "t.src.db.Database.Inner"));
    ASSERT(has_def_qn(r, "t.src.db.Database.Inner.deinit"));
    ASSERT(has_def_qn(r, "t.src.db.Kind"));
    ASSERT(has_def_qn(r, "t.src.db.Kind.name"));
    ASSERT(has_def_qn(r, "t.src.db.helper"));
    ASSERT(has_def(r, "Function", "inside"));
    /* Members are not re-extracted as file-level functions. */
    ASSERT_FALSE(has_def_qn(r, "t.src.db.deinit"));
    ASSERT_FALSE(has_def_qn(r, "t.src.db.open"));
    /* A container declaration is the Struct, not also a Variable. */
    ASSERT_EQ(count_defs_named(r, "Variable", "Database"), 0);
    ASSERT_EQ(count_defs_named(r, "Variable", "Inner"), 0);
    ASSERT_EQ(count_defs_named(r, "Variable", "Kind"), 0);
    ASSERT_EQ(count_defs_named(r, "Variable", "std"), 1);
    /* An alias keeps its value text for the resolver, not as a return type. */
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (strcmp(d->label, "Variable") == 0 && strcmp(d->name, "std") == 0) {
            ASSERT_NULL(d->return_type);
            ASSERT_STR_EQ(d->alias_value, "@import(\"std\")");
        }
    }
    /* A call inside a method is scoped to the struct-qualified method. */
    bool close_in_deinit = false;
    bool helper_in_test = false;
    for (int i = 0; i < r->calls.count; i++) {
        const CBMCall *c = &r->calls.items[i];
        if (strstr(c->callee_name, "close") && c->enclosing_func_qn &&
            strcmp(c->enclosing_func_qn, "t.src.db.Database.deinit") == 0) {
            close_in_deinit = true;
        }
        /* A test inside a struct is a file-level Function; its calls agree. */
        if (strcmp(c->callee_name, "helper") == 0 && c->enclosing_func_qn &&
            strcmp(c->enclosing_func_qn, "t.src.db.inside") == 0) {
            helper_in_test = true;
        }
    }
    ASSERT(close_in_deinit);
    ASSERT(helper_in_test);
    ASSERT(has_def_qn(r, "t.src.db.inside"));
    cbm_free_result(r);
    PASS();
}

/* --- Zig imports are keyed by their bound name; value receivers are flagged ---
 * `const db = @import("db.zig")` binds "db" (not the "zig" a '.'-split gave), so
 * the import map can match `db.helper()`. A member call on a value receiver is
 * is_method (weak short-name matches must not bind it); a TitleCase type-name
 * receiver is not. */
TEST(zig_import_names_and_member_receivers) {
    CBMFileResult *r = extract("const std = @import(\"std\");\n"
                               "const db = @import(\"db.zig\");\n"
                               "const Statement = @import(\"sql/stmt.zig\").Statement;\n"
                               "fn run(tmp: anytype) void {\n"
                               "    tmp.cleanup();\n"
                               "    db.helper();\n"
                               "    Statement.prepare();\n"
                               "    db.Database.open();\n"
                               "    run(tmp);\n"
                               "}\n",
                               CBM_LANG_ZIG, "t", "src/main.zig");
    ASSERT_NOT_NULL(r);
    const char *want[][2] = {{"std", "std"}, {"db", "db.zig"}, {"stmt", "sql/stmt.zig"}};
    for (int w = 0; w < 3; w++) {
        bool found = false;
        for (int i = 0; i < r->imports.count; i++) {
            const CBMImport *imp = &r->imports.items[i];
            if (imp->local_name && strcmp(imp->local_name, want[w][0]) == 0 &&
                strcmp(imp->module_path, want[w][1]) == 0) {
                found = true;
            }
        }
        ASSERT(found);
    }
    int checked = 0;
    for (int i = 0; i < r->calls.count; i++) {
        const CBMCall *c = &r->calls.items[i];
        if (strcmp(c->callee_name, "tmp.cleanup") == 0 || strcmp(c->callee_name, "db.helper") == 0) {
            ASSERT_TRUE(c->is_method);
            checked++;
        } else if (strcmp(c->callee_name, "Statement.prepare") == 0 ||
                   strcmp(c->callee_name, "db.Database.open") == 0 ||
                   strcmp(c->callee_name, "run") == 0) {
            ASSERT_FALSE(c->is_method);
            checked++;
        }
    }
    ASSERT_EQ(checked, 5);
    cbm_free_result(r);
    PASS();
}

/* --- C++ function (standalone) --- */
TEST(cpp_function) {
    CBMFileResult *r = extract("#include <string>\nstd::string greet(const std::string& name) { "
                               "return \"Hello \" + name; }\nint main() { return 0; }\n",
                               CBM_LANG_CPP, "t", "main.cpp");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- C++ out-of-line method definitions (#428) ---
 * A .cpp defining methods of a class declared elsewhere (not in this TU).
 * Pre-fix these were recorded as free Functions (label "Function", no
 * parent_class); they must be Methods linked to their enclosing class via
 * parent_class. (The helper also descends nested scopes `ns::Class::method` to
 * the immediate class, but tree-sitter-cpp parses a synthetic doubly-qualified
 * out-of-line def in isolation as an ERROR node, so that path is exercised by
 * real codebases rather than this isolated unit fixture.) */
TEST(cpp_out_of_line_method_issue428) {
    CBMFileResult *r = extract("void Foo::bar() {}\n"
                               "int Foo::baz() { return 0; }\n",
                               CBM_LANG_CPP, "t", "foo.cpp");
    ASSERT_NOT_NULL(r);
    ASSERT(has_def(r, "Method", "bar"));
    ASSERT(has_def(r, "Method", "baz"));
    ASSERT(!has_def(r, "Function", "bar")); /* not a free function */
    /* parent_class links to the enclosing class QN */
    int checked = 0;
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (strcmp(d->name, "bar") == 0 && strcmp(d->label, "Method") == 0) {
            ASSERT_NOT_NULL(d->parent_class);
            ASSERT(strstr(d->parent_class, "Foo") != NULL);
            checked = 1;
        }
    }
    ASSERT(checked);
    cbm_free_result(r);
    PASS();
}

/* --- COBOL paragraph --- */
TEST(cobol_paragraph) {
    CBMFileResult *r =
        extract("IDENTIFICATION DIVISION.\nPROGRAM-ID. HELLO.\nPROCEDURE DIVISION.\n    "
                "DISPLAY-GREETING.\n        DISPLAY 'HELLO WORLD'.\n        STOP RUN.\n",
                CBM_LANG_COBOL, "t", "hello.cbl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Verilog module --- */
TEST(verilog_module) {
    CBMFileResult *r =
        extract("module adder(input a, input b, output sum);\n  assign sum = a + b;\nendmodule\n",
                CBM_LANG_VERILOG, "t", "adder.v");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- CUDA kernel --- */
TEST(cuda_kernel) {
    CBMFileResult *r = extract("__global__ void vectorAdd(float *a, float *b, float *c, int n) {\n "
                               "   int i = blockIdx.x * blockDim.x + threadIdx.x;\n    if (i < n) "
                               "c[i] = a[i] + b[i];\n}\nint main() { return 0; }\n",
                               CBM_LANG_CUDA, "t", "vector.cu");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Python decorator --- */
TEST(python_decorator) {
    CBMFileResult *r = extract("class Router:\n    @staticmethod\n    def route(path: str):\n      "
                               "  def decorator(func): return func\n        return decorator\n",
                               CBM_LANG_PYTHON, "t", "router.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "Router"));
    cbm_free_result(r);
    PASS();
}

/* --- TypeScript interface --- */
TEST(ts_interface) {
    CBMFileResult *r = extract("export interface Repository<T> { findById(id: number): T; "
                               "save(entity: T): void; delete(id: number): void; }\n",
                               CBM_LANG_TYPESCRIPT, "t", "repo.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- TSX component --- */
TEST(tsx_component) {
    CBMFileResult *r = extract(
        "import React from 'react';\ninterface Props { name: string; }\nexport function Greeting({ "
        "name }: Props) {\n    return <div>Hello {name}</div>;\n}\nexport default Greeting;\n",
        CBM_LANG_TSX, "t", "Greeting.tsx");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "Greeting"));
    cbm_free_result(r);
    PASS();
}

/* --- Lua table method --- */
TEST(lua_table_method) {
    CBMFileResult *r =
        extract("local M = {}\nfunction M.create(name)\n    return { name = name }\nend\nfunction "
                "M.greet(self)\n    return 'Hi ' .. self.name\nend\nreturn M\n",
                CBM_LANG_LUA, "t", "module.lua");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* Should extract at least one Function from Lua table method */
    int fn_count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, "Function") == 0)
            fn_count++;
    }
    ASSERT_GTE(fn_count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Emacs Lisp defun --- */
TEST(emacs_lisp_defun) {
    CBMFileResult *r = extract("(defun greet (name)\n  (message \"Hello %s\" name))\n(defun main "
                               "()\n  (greet \"World\"))\n",
                               CBM_LANG_EMACSLISP, "t", "init.el");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "greet"));
    ASSERT(has_def(r, "Function", "main"));
    cbm_free_result(r);
    PASS();
}

/* --- Emacs Lisp defvar --- */
TEST(emacs_lisp_defvar) {
    CBMFileResult *r = extract("(defvar my-count 0 \"A counter.\")\n(defcustom my-name \"World\" "
                               "\"The name.\"\n  :type 'string)\n",
                               CBM_LANG_EMACSLISP, "t", "vars.el");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Haskell data type --- */
TEST(haskell_data_type) {
    CBMFileResult *r =
        extract("data Shape = Circle Double | Rectangle Double Double\narea :: Shape -> "
                "Double\narea (Circle r) = pi * r * r\narea (Rectangle w h) = w * h\n",
                CBM_LANG_HASKELL, "t", "Shape.hs");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Clojure function (known limitation: defn produces list_lit) --- */
TEST(clojure_function) {
    CBMFileResult *r = extract("(ns greeter.core)\n(defn greet [name]\n  (str \"Hello \" "
                               "name))\n(defn -main [& args]\n  (println (greet \"World\")))\n",
                               CBM_LANG_CLOJURE, "t", "core.clj");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* Clojure uses list_lit for all forms — no function defs extracted (known limitation) */
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group E2: Missing Config / Markup Languages
 * ═══════════════════════════════════════════════════════════════════ */

/* --- HTML elements --- */
TEST(html_elements) {
    CBMFileResult *r = extract(
        "<!DOCTYPE "
        "html><html><head><title>Test</title></head><body><h1>Hello</h1><p>World</p></body></html>",
        CBM_LANG_HTML, "t", "index.html");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- SQL function (CREATE FUNCTION) --- */
TEST(sql_function) {
    CBMFileResult *r = extract("CREATE FUNCTION get_user_count() RETURNS INTEGER AS $$ SELECT "
                               "COUNT(*) FROM users; $$ LANGUAGE SQL;\n",
                               CBM_LANG_SQL, "t", "funcs.sql");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Meson project --- */
TEST(meson_project) {
    CBMFileResult *r = extract(
        "project('myapp', 'c', version: '1.0.0')\nexecutable('myapp', 'main.c', install: true)\n",
        CBM_LANG_MESON, "t", "meson.build");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- CSS rules --- */
TEST(css_rules) {
    CBMFileResult *r = extract(
        ".container { display: flex; width: 100%; }\n.button { background: #007bff; color: white; "
        "border: none; }\n@media (max-width: 768px) { .container { flex-direction: column; } }\n",
        CBM_LANG_CSS, "t", "styles.css");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- SCSS rules --- */
TEST(scss_rules) {
    CBMFileResult *r = extract("$primary: #007bff;\n.container {\n  width: 100%;\n  .button {\n    "
                               "background: $primary;\n    &:hover { opacity: 0.8; }\n  }\n}\n",
                               CBM_LANG_SCSS, "t", "styles.scss");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- TOML basic --- */
TEST(toml_basic) {
    CBMFileResult *r = extract("[server]\nhost = \"localhost\"\nport = 8080\n\n[database]\nurl = "
                               "\"postgres://localhost/db\"\nmax_connections = 10\n",
                               CBM_LANG_TOML, "t", "config.toml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Class", "server"));
    ASSERT(has_def(r, "Class", "database"));
    ASSERT(has_def(r, "Variable", "host"));
    ASSERT(has_def(r, "Variable", "port"));
    cbm_free_result(r);
    PASS();
}

/* --- CMake function --- */
TEST(cmake_function) {
    CBMFileResult *r = extract(
        "cmake_minimum_required(VERSION 3.16)\nproject(MyApp VERSION 1.0)\nadd_executable(myapp "
        "main.cpp)\ntarget_compile_features(myapp PRIVATE cxx_std_17)\n",
        CBM_LANG_CMAKE, "t", "CMakeLists.txt");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- JSON object --- */
TEST(json_object) {
    CBMFileResult *r = extract("{\"name\": \"myapp\", \"version\": \"1.0.0\", \"scripts\": "
                               "{\"build\": \"go build\", \"test\": \"go test ./...\"}}",
                               CBM_LANG_JSON, "t", "config.json");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Variable", "name"));
    ASSERT(has_def(r, "Variable", "version"));
    cbm_free_result(r);
    PASS();
}

/* --- Protobuf message --- */
TEST(protobuf_message) {
    CBMFileResult *r = extract(
        "syntax = \"proto3\";\npackage user;\nmessage User { int64 id = 1; string name = 2; string "
        "email = 3; }\nservice UserService { rpc GetUser(User) returns (User); }\n",
        CBM_LANG_PROTOBUF, "t", "user.proto");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- GraphQL type --- */
TEST(graphql_type) {
    CBMFileResult *r = extract("type User {\n  id: ID!\n  name: String!\n  email: String!\n}\ntype "
                               "Query {\n  user(id: ID!): User\n  users: [User!]!\n}\n",
                               CBM_LANG_GRAPHQL, "t", "schema.graphql");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Svelte component --- */
TEST(svelte_component) {
    CBMFileResult *r = extract("<script>\n  let name = 'World';\n  function greet() {\n    return "
                               "`Hello ${name}`;\n  }\n</script>\n<h1>{greet()}</h1>\n",
                               CBM_LANG_SVELTE, "t", "App.svelte");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- Vue component --- */
TEST(vue_component) {
    CBMFileResult *r =
        extract("<template><div>{{ message }}</div></template>\n<script>\nexport default {\n  "
                "name: 'App',\n  data() { return { message: 'Hello World' }; },\n  methods: { "
                "greet() { return this.message; } }\n};\n</script>\n",
                CBM_LANG_VUE, "t", "App.vue");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- GLSL shader --- */
TEST(glsl_shader) {
    CBMFileResult *r = extract(
        "#version 330 core\nvoid main() {\n    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n}\nvec3 "
        "transform(vec3 pos, mat4 mvp) {\n    return (mvp * vec4(pos, 1.0)).xyz;\n}\n",
        CBM_LANG_GLSL, "t", "vertex.glsl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* --- VimScript function --- */
TEST(vimscript_function) {
    CBMFileResult *r = extract("function! SayHello()\n  echo 'Hello'\nendfunction\n",
                               CBM_LANG_VIMSCRIPT, "t", "plugin.vim");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* VimScript extraction may or may not produce named functions */
    int fn_count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, "Function") == 0)
            fn_count++;
    }
    if (fn_count > 0) {
        ASSERT(has_def(r, "Function", "SayHello"));
    }
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group H: Scientific / Math — extended tests
 * ═══════════════════════════════════════════════════════════════════ */

/* --- MATLAB parse (simple expression) --- */
TEST(matlab_parse) {
    CBMFileResult *r = extract("x = 1;\ny = x + 2;\n", CBM_LANG_MATLAB, "t", "simple.matlab");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

/* --- MATLAB call --- */
TEST(matlab_call) {
    CBMFileResult *r = extract("function y = foo(x)\n  y = inv(x);\n  disp hello\nend\n",
                               CBM_LANG_MATLAB, "t", "foo.matlab");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->calls.count, 0);
    ASSERT(has_call(r, "inv"));
    ASSERT(has_call(r, "disp"));
    cbm_free_result(r);
    PASS();
}

/* --- Lean parse (theorem) --- */
TEST(lean_parse) {
    CBMFileResult *r = extract("theorem add_comm (a b : Nat) : a + b = b + a := by omega\n",
                               CBM_LANG_LEAN, "t", "Comm.lean");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

/* --- Lean call (recursive fib) --- */
TEST(lean_call) {
    CBMFileResult *r = extract("def fib : Nat \xe2\x86\x92 Nat\n  | 0 => 1\n  | 1 => 1\n  | n + 2 "
                               "=> fib (n + 1) + fib n\n",
                               CBM_LANG_LEAN, "t", "Fib.lean");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->calls.count, 0);
    ASSERT(has_call(r, "fib"));
    cbm_free_result(r);
    PASS();
}

/* --- Lean type annotation not call --- */
TEST(lean_type_annotation_not_call) {
    CBMFileResult *r = extract(
        "def listLen (xs : List Nat) : Nat := 0\ndef greet : IO Unit := IO.println \"hi\"\n",
        CBM_LANG_LEAN, "t", "Types.lean");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* "List" in binder type position should NOT be extracted as a call */
    for (int i = 0; i < r->calls.count; i++) {
        ASSERT_FALSE(strcmp(r->calls.items[i].callee_name, "List") == 0);
    }
    /* IO.println in the body should be present */
    int found_println = 0;
    for (int i = 0; i < r->calls.count; i++) {
        if (strstr(r->calls.items[i].callee_name, "println") != NULL) {
            found_println = 1;
        }
    }
    ASSERT_TRUE(found_println);
    cbm_free_result(r);
    PASS();
}

/* --- FORM parse (simple expression) --- */
TEST(form_parse) {
    CBMFileResult *r = extract("Symbols x, y;\nLocal F = x + y;\nPrint;\n.end\n", CBM_LANG_FORM,
                               "t", "example.frm");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

/* --- FORM call (#call) --- */
TEST(form_call) {
    CBMFileResult *r = extract("#procedure myproc(x)\n  id `x' = 0;\n#endprocedure\n#procedure "
                               "caller()\n  #call myproc(1)\n#endprocedure\n",
                               CBM_LANG_FORM, "t", "calc.frm");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->calls.count, 0);
    ASSERT(has_call(r, "myproc"));
    cbm_free_result(r);
    PASS();
}

/* --- Magma procedure --- */
TEST(magma_procedure) {
    CBMFileResult *r = extract("procedure PrintHello()\n  print \"Hello\";\nend procedure;\n",
                               CBM_LANG_MAGMA, "t", "hello.mag");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int fn_count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, "Function") == 0)
            fn_count++;
    }
    if (fn_count > 0) {
        ASSERT(has_def(r, "Function", "PrintHello"));
    }
    cbm_free_result(r);
    PASS();
}

/* --- Magma parse (simple) --- */
TEST(magma_parse) {
    CBMFileResult *r = extract("x := 42;\ny := x + 1;\n", CBM_LANG_MAGMA, "t", "simple.mag");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

/* --- Magma import (load) --- */
TEST(magma_import) {
    CBMFileResult *r = extract("load \"utils.mag\";\nload \"lib/helpers.mag\";\n", CBM_LANG_MAGMA,
                               "t", "main.mag");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->imports.count, 2);
    cbm_free_result(r);
    PASS();
}

/* --- Magma call --- */
TEST(magma_call) {
    CBMFileResult *r = extract("function Foo(x)\n  y := Bar(x);\n  return y;\nend function;\n",
                               CBM_LANG_MAGMA, "t", "calls.mag");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->calls.count, 0);
    ASSERT(has_call(r, "Bar"));
    cbm_free_result(r);
    PASS();
}

/* --- Magma disambiguation (.m file as Magma) --- */
TEST(magma_disambiguation) {
    CBMFileResult *r = extract("function Factorial(n)\n  if n le 1 then\n    return 1;\n  end "
                               "if;\n  return n * Factorial(n - 1);\nend function;\n",
                               CBM_LANG_MAGMA, "t", "test.m");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int fn_count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, "Function") == 0)
            fn_count++;
    }
    ASSERT_GTE(fn_count, 1);
    if (fn_count > 0) {
        ASSERT(has_def(r, "Function", "Factorial"));
    }
    cbm_free_result(r);
    PASS();
}

/* --- Wolfram function (both := and =) --- */
TEST(wolfram_function_extended) {
    CBMFileResult *r = extract("f[x_] := x^2\ng[x_] = x + 1\n", CBM_LANG_WOLFRAM, "t", "funcs.wl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int fn_count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, "Function") == 0)
            fn_count++;
    }
    ASSERT_GTE(fn_count, 2);
    ASSERT(has_def(r, "Function", "f"));
    ASSERT(has_def(r, "Function", "g"));
    cbm_free_result(r);
    PASS();
}

/* --- Wolfram call --- */
TEST(wolfram_call) {
    CBMFileResult *r = extract("f[x_] := g[x] + h[x]\n", CBM_LANG_WOLFRAM, "t", "calls.wl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->calls.count, 0);
    ASSERT(has_call(r, "g"));
    ASSERT(has_call(r, "h"));
    /* "f" should NOT appear as a call (it's the definition LHS) */
    for (int i = 0; i < r->calls.count; i++) {
        ASSERT_FALSE(strcmp(r->calls.items[i].callee_name, "f") == 0);
    }
    cbm_free_result(r);
    PASS();
}

/* --- Wolfram caller attribution --- */
TEST(wolfram_caller_attribution) {
    CBMFileResult *r = extract("f[x_] := g[x] + h[x]\n", CBM_LANG_WOLFRAM, "t", "caller.wl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->calls.count, 0);
    /* Calls inside f[] should have f as enclosing function, not the module path */
    for (int i = 0; i < r->calls.count; i++) {
        if (strcmp(r->calls.items[i].callee_name, "g") == 0 ||
            strcmp(r->calls.items[i].callee_name, "h") == 0) {
            /* enclosing_func_qn must NOT be empty or the file path */
            ASSERT_NOT_NULL(r->calls.items[i].enclosing_func_qn);
            ASSERT_FALSE(strcmp(r->calls.items[i].enclosing_func_qn, "") == 0);
            ASSERT_FALSE(strcmp(r->calls.items[i].enclosing_func_qn, "t.caller") == 0);
        }
    }
    cbm_free_result(r);
    PASS();
}

/* Issue #438: a C function_definition has no `name` field — the name lives in the
 * declarator chain. Calls inside a C function must be attributed to the enclosing
 * function, not the module. Pre-fix, enclosing_func_qn fell back to the module QN. */
TEST(c_caller_attribution) {
    CBMFileResult *r = extract("int helper(int x) { return x; }\n"
                               "int caller(void) { return helper(1); }\n",
                               CBM_LANG_C, "t", "main.c");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->calls.count, 0);
    int saw_helper = 0;
    for (int i = 0; i < r->calls.count; i++) {
        if (strcmp(r->calls.items[i].callee_name, "helper") == 0) {
            saw_helper = 1;
            /* enclosing_func_qn must be the function, NOT empty and NOT the module QN. */
            ASSERT_NOT_NULL(r->calls.items[i].enclosing_func_qn);
            ASSERT_FALSE(strcmp(r->calls.items[i].enclosing_func_qn, "") == 0);
            ASSERT_FALSE(strcmp(r->calls.items[i].enclosing_func_qn, "t.main") == 0);
        }
    }
    ASSERT(saw_helper);
    cbm_free_result(r);
    PASS();
}

/* adc8304 (the dedup refactor bundled into #463) re-pointed the C/C++ enclosing-
 * function resolver at the canonical declarator walker: qualified names (Foo::bar)
 * now resolve via resolve_qualified_name(), and `type_identifier` was dropped from
 * the terminal-name set. These guard that out-of-line C++ method / ctor / dtor
 * definitions still attribute their inner calls to the enclosing function, not the
 * module — i.e. that the dedup did not reintroduce the #438 regression on the
 * qualified-declarator path. Module QN for "m.cpp" under prefix "t" is "t.m". */
TEST(cpp_out_of_line_method_caller_attribution) {
    CBMFileResult *r = extract("struct Foo { void bar(); };\n"
                               "int helper(int x) { return x; }\n"
                               "void Foo::bar() { helper(1); }\n",
                               CBM_LANG_CPP, "t", "m.cpp");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->calls.count, 0);
    int saw_helper = 0;
    for (int i = 0; i < r->calls.count; i++) {
        if (strcmp(r->calls.items[i].callee_name, "helper") == 0) {
            saw_helper = 1;
            /* enclosing must be the out-of-line method, NOT empty and NOT the module. */
            ASSERT_NOT_NULL(r->calls.items[i].enclosing_func_qn);
            ASSERT_FALSE(strcmp(r->calls.items[i].enclosing_func_qn, "") == 0);
            ASSERT_FALSE(strcmp(r->calls.items[i].enclosing_func_qn, "t.m") == 0);
        }
    }
    ASSERT(saw_helper);
    cbm_free_result(r);
    PASS();
}

/* Out-of-line constructor (Foo::Foo) and destructor (Foo::~Foo) exercise the
 * identifier and destructor_name branches of resolve_qualified_name(). A call
 * inside either must attribute to that special member, not the module. */
TEST(cpp_out_of_line_ctor_dtor_caller_attribution) {
    CBMFileResult *r = extract("struct Foo { Foo(); ~Foo(); };\n"
                               "int helper(int x) { return x; }\n"
                               "Foo::Foo() { helper(1); }\n"
                               "Foo::~Foo() { helper(2); }\n",
                               CBM_LANG_CPP, "t", "m.cpp");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->calls.count, 0);
    int helper_calls = 0;
    for (int i = 0; i < r->calls.count; i++) {
        if (strcmp(r->calls.items[i].callee_name, "helper") == 0) {
            helper_calls++;
            ASSERT_NOT_NULL(r->calls.items[i].enclosing_func_qn);
            ASSERT_FALSE(strcmp(r->calls.items[i].enclosing_func_qn, "") == 0);
            ASSERT_FALSE(strcmp(r->calls.items[i].enclosing_func_qn, "t.m") == 0);
        }
    }
    ASSERT(helper_calls >= 1);
    cbm_free_result(r);
    PASS();
}

/* --- Wolfram parse (simple assignment) --- */
TEST(wolfram_parse) {
    CBMFileResult *r = extract("x = 42;\ny = x + 1;\n", CBM_LANG_WOLFRAM, "t", "simple.wl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

/* --- Wolfram import --- */
TEST(wolfram_import) {
    CBMFileResult *r =
        extract("<< \"utils.wl\"\nNeeds[\"Package`\"]\n", CBM_LANG_WOLFRAM, "t", "main.wl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->imports.count, 0);
    cbm_free_result(r);
    PASS();
}

/* --- Wolfram nested def --- */
TEST(wolfram_nested_def) {
    CBMFileResult *r = extract("main[x_] := Module[{localF}, localF[t_] := t + 1; localF[x]]\n",
                               CBM_LANG_WOLFRAM, "t", "nested.wl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "main"));
    ASSERT(has_def(r, "Function", "localF"));
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group I: cbm_test.go ports
 * ═══════════════════════════════════════════════════════════════════ */

TEST(python_docstring) {
    CBMFileResult *r = extract(
        "def compute(x, y):\n    \"\"\"Compute the sum of x and y.\"\"\"\n    return x + y\n",
        CBM_LANG_PYTHON, "test", "test.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "compute"));
    /* Check docstring is present */
    int found = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].name, "compute") == 0) {
            found = 1;
            ASSERT_NOT_NULL(r->defs.items[i].docstring);
            ASSERT_TRUE(strlen(r->defs.items[i].docstring) > 0);
        }
    }
    ASSERT_TRUE(found);
    cbm_free_result(r);
    PASS();
}

TEST(c_docstring_truncation_preserves_utf8_boundary) {
    char source[1024];
    size_t pos = 0;
    const char prefix[] = "/* ";
    const char box_drawing[] = "\xe2\x94\x80";
    const char suffix[] = " */\nint utf8_boundary_target(void) { return 0; }\n";

    memcpy(source + pos, prefix, sizeof(prefix) - 1);
    pos += sizeof(prefix) - 1;
    for (int i = 0; i < 200; i++) {
        memcpy(source + pos, box_drawing, sizeof(box_drawing) - 1);
        pos += sizeof(box_drawing) - 1;
    }
    memcpy(source + pos, suffix, sizeof(suffix));

    CBMFileResult *r = extract(source, CBM_LANG_C, "test", "test.c");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "utf8_boundary_target"));

    const char *docstring = NULL;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].name, "utf8_boundary_target") == 0) {
            docstring = r->defs.items[i].docstring;
            break;
        }
    }
    ASSERT_NOT_NULL(docstring);
    ASSERT_EQ((int)strlen(docstring), 498);
    ASSERT_EQ((unsigned char)docstring[495], 0xe2);
    ASSERT_EQ((unsigned char)docstring[496], 0x94);
    ASSERT_EQ((unsigned char)docstring[497], 0x80);

    cbm_free_result(r);
    PASS();
}

TEST(go_function_extraction) {
    CBMFileResult *r =
        extract("package main\n\n// Greet returns a greeting.\nfunc Greet(name string) string "
                "{\n\treturn \"Hello, \" + name\n}\n\nfunc main() {\n\tGreet(\"world\")\n}\n",
                CBM_LANG_GO, "test", "main.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "Greet"));
    ASSERT(has_def(r, "Function", "main"));
    ASSERT(has_call(r, "Greet"));
    cbm_free_result(r);
    PASS();
}

TEST(js_arrow_function) {
    CBMFileResult *r = extract("const greet = (name) => {\n  return \"Hello \" + "
                               "name;\n};\n\nconst result = greet(\"world\");\n",
                               CBM_LANG_JAVASCRIPT, "test", "app.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group J: language_failures_test.go ports
 * ═══════════════════════════════════════════════════════════════════ */

/* CommonLisp — defun extraction (known limitation: grammar produces list_lit) */
TEST(commonlisp_defun) {
    CBMFileResult *r =
        extract("(defun hello () \"world\")\n", CBM_LANG_COMMONLISP, "test", "hello.lisp");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* Known limitation: CommonLisp grammar produces list_lit, not defun nodes.
     * Function extraction returns 0 — this test documents the limitation. */
    cbm_free_result(r);
    PASS();
}

TEST(commonlisp_multiple_functions) {
    CBMFileResult *r = extract("(defun add (a b) (+ a b))\n(defun mul (a b) (* a b))\n",
                               CBM_LANG_COMMONLISP, "test", "math.lisp");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

TEST(commonlisp_defmacro) {
    CBMFileResult *r =
        extract("(defmacro when2 (condition &body body)\n  `(if ,condition (progn ,@body)))\n",
                CBM_LANG_COMMONLISP, "test", "macros.lisp");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    cbm_free_result(r);
    PASS();
}

TEST(makefile_rule_as_function) {
    CBMFileResult *r = extract("all:\n\t@echo hello\n", CBM_LANG_MAKEFILE, "test", "Makefile");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "all"));
    cbm_free_result(r);
    PASS();
}

TEST(makefile_multiple_targets) {
    CBMFileResult *r = extract("all: main.o\n\tgcc -o all main.o\n\nbuild:\n\tgo build ./...\n",
                               CBM_LANG_MAKEFILE, "test", "Makefile");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_def(r, "Function", "all"));
    ASSERT(has_def(r, "Function", "build"));
    cbm_free_result(r);
    PASS();
}

TEST(makefile_variable_extraction) {
    CBMFileResult *r =
        extract("CC := gcc\nCFLAGS := -Wall\n", CBM_LANG_MAKEFILE, "test", "Makefile");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* Variable extraction may or may not work depending on Makefile grammar support */
    cbm_free_result(r);
    PASS();
}

TEST(vimscript_function_extraction) {
    CBMFileResult *r = extract("function! SayHello()\n  echo 'Hello'\nendfunction\n",
                               CBM_LANG_VIMSCRIPT, "test", "plugin.vim");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* VimScript function extraction may or may not produce named functions */
    int fn_count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, "Function") == 0)
            fn_count++;
    }
    if (fn_count > 0) {
        ASSERT(has_def(r, "Function", "SayHello"));
    }
    cbm_free_result(r);
    PASS();
}

TEST(vimscript_function_without_bang) {
    CBMFileResult *r = extract("function MyFunc(arg)\n  return arg\nendfunction\n",
                               CBM_LANG_VIMSCRIPT, "test", "plugin.vim");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int fn_count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, "Function") == 0)
            fn_count++;
    }
    if (fn_count > 0) {
        ASSERT(has_def(r, "Function", "MyFunc"));
    }
    cbm_free_result(r);
    PASS();
}

TEST(julia_function_extraction) {
    CBMFileResult *r = extract("function hello()\n  println(\"Hello, World!\")\nend\n",
                               CBM_LANG_JULIA, "test", "hello.jl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int fn_count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, "Function") == 0)
            fn_count++;
    }
    if (fn_count > 0) {
        ASSERT(has_def(r, "Function", "hello"));
    }
    cbm_free_result(r);
    PASS();
}

TEST(julia_function_with_args) {
    CBMFileResult *r = extract("function add(a::Int, b::Int)::Int\n  return a + b\nend\n",
                               CBM_LANG_JULIA, "test", "math.jl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int fn_count = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].label, "Function") == 0)
            fn_count++;
    }
    if (fn_count > 0) {
        ASSERT(has_def(r, "Function", "add"));
    }
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Cross-cutting: Calls + Imports
 * ═══════════════════════════════════════════════════════════════════ */

TEST(python_calls) {
    CBMFileResult *r =
        extract("import os\ndef main():\n    os.path.exists('/tmp')\n    print('hello')\n",
                CBM_LANG_PYTHON, "t", "main.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* Python unified extraction produces calls — verify at least some exist */
    ASSERT_GT(r->calls.count, 0);
    cbm_free_result(r);
    PASS();
}

TEST(python_iris_classMethodValue) {
    CBMFileResult *r =
        extract("import iris\n"
                "iris_obj = iris.cls('%Library.ObjectScript')\n"
                "def call_bfs(n):\n"
                "    return iris_obj.classMethodValue('Graph.KG.TraversalBFS', 'BFSFastJson', n)\n",
                CBM_LANG_PYTHON, "t", "store.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_call(r, "Graph.KG.TraversalBFS.BFSFastJson"));
    cbm_free_result(r);
    PASS();
}

TEST(go_calls) {
    CBMFileResult *r =
        extract("package main\nimport \"fmt\"\nfunc main() { fmt.Println(\"hello\") }\n",
                CBM_LANG_GO, "t", "main.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_call(r, "fmt.Println"));
    cbm_free_result(r);
    PASS();
}

TEST(python_imports) {
    CBMFileResult *r =
        extract("import os\nfrom sys import argv\nfrom collections import defaultdict\n",
                CBM_LANG_PYTHON, "t", "main.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->imports.count, 0);
    cbm_free_result(r);
    PASS();
}

TEST(js_imports) {
    CBMFileResult *r = extract("import React from 'react';\nimport { useState } from "
                               "'react';\nconst fs = require('fs');\n",
                               CBM_LANG_JAVASCRIPT, "t", "app.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->imports.count, 0);
    cbm_free_result(r);
    PASS();
}

TEST(go_imports) {
    CBMFileResult *r =
        extract("package main\n\nimport \"fmt\"\nimport (\n    \"os\"\n    net \"net/http\"\n)\n",
                CBM_LANG_GO, "t", "main.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->imports.count, 0);
    ASSERT(has_import(r, "fmt"));
    cbm_free_result(r);
    PASS();
}
/* cgo's `import "C"` is a pseudo-package, not a real import: keeping it lets the
 * import resolver name-match "C" onto an arbitrary project symbol called C. The
 * real imports of the same file must survive. */
TEST(go_cgo_pseudo_import_dropped) {
    CBMFileResult *r = extract("package m\n\n/*\nstatic int helper(void) { return 1; }\n*/\n"
                               "import \"C\"\n\nimport \"fmt\"\n\n"
                               "func Run() { fmt.Println(C.helper()) }\n",
                               CBM_LANG_GO, "t", "cgo.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_import(r, "fmt"));
    for (int i = 0; i < r->imports.count; i++) {
        ASSERT_NOT_NULL(r->imports.items[i].module_path);
        ASSERT_TRUE(strcmp(r->imports.items[i].module_path, "C") != 0);
    }
    cbm_free_result(r);
    PASS();
}
TEST(java_imports) {
    CBMFileResult *r = extract(
        "import java.util.List;\nimport java.util.ArrayList;\nimport static java.lang.Math.PI;\n"
        "public class Foo {}\n",
        CBM_LANG_JAVA, "t", "Foo.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->imports.count, 0);
    ASSERT(has_import(r, "java.util.List"));
    cbm_free_result(r);
    PASS();
}

TEST(rust_imports) {
    CBMFileResult *r = extract(
        "use std::collections::HashMap;\nuse std::io::{self, Write};\nuse serde::Serialize;\n"
        "fn main() {}\n",
        CBM_LANG_RUST, "t", "main.rs");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->imports.count, 0);
    ASSERT(has_import(r, "std::collections::HashMap"));
    cbm_free_result(r);
    PASS();
}

TEST(c_imports) {
    CBMFileResult *r = extract("#include <stdio.h>\n#include <stdlib.h>\n#include "
                               "\"mylib.h\"\n\nint main() { return 0; }\n",
                               CBM_LANG_C, "t", "main.c");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->imports.count, 0);
    ASSERT(has_import(r, "stdio.h"));
    cbm_free_result(r);
    PASS();
}

TEST(ruby_imports) {
    CBMFileResult *r = extract(
        "require 'json'\nrequire 'net/http'\nrequire_relative 'helpers'\n\nclass Foo; end\n",
        CBM_LANG_RUBY, "t", "app.rb");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->imports.count, 0);
    ASSERT(has_import(r, "json"));
    cbm_free_result(r);
    PASS();
}

TEST(lua_imports) {
    CBMFileResult *r = extract("local json = require(\"dkjson\")\nlocal http = "
                               "require(\"socket.http\")\n\nlocal function greet() end\n",
                               CBM_LANG_LUA, "t", "main.lua");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GT(r->imports.count, 0);
    ASSERT(has_import(r, "dkjson"));
    cbm_free_result(r);
    PASS();
}

TEST(import_stress_go) {
    /* Stress test: 5,000 single-line Go imports.
     * Verifies O(N) behaviour — would hang indefinitely with the O(N²) loop. */
    const int N = 5000;
    /* Each line: import "pkg/NNNNN"\n  = ~20 chars; total ~100KB */
    int buf_size = N * 24 + 64;
    char *src = malloc((size_t)buf_size);
    ASSERT_NOT_NULL(src);

    int pos = 0;
    pos += snprintf(src + pos, (size_t)(buf_size - pos), "package stress\n");
    for (int k = 0; k < N; k++) {
        pos += snprintf(src + pos, (size_t)(buf_size - pos), "import \"pkg/%05d\"\n", k);
    }

    CBMFileResult *r = extract(src, CBM_LANG_GO, "t", "stress.go");
    free(src);
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(r->imports.count, N);
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Embedded-language import extraction
 * Host grammars (Svelte, Vue, HTML, Astro) keep <script> bodies as
 * raw_text — the embedded-imports walker re-parses each block with the
 * JS grammar so the standard ES import extractor sees real
 * import_statement nodes.
 * ═══════════════════════════════════════════════════════════════════ */

TEST(svelte_imports_basic) {
    /* Default import + named imports + namespace import */
    CBMFileResult *r = extract("<script>\n"
                               "import Foo from './Foo.svelte';\n"
                               "import { bar, baz } from '../lib/utils';\n"
                               "import * as helpers from './helpers';\n"
                               "export let value = 42;\n"
                               "</script>\n"
                               "<h1>Hello {value}</h1>\n",
                               CBM_LANG_SVELTE, "t", "Comp.svelte");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->imports.count, 3);
    ASSERT(has_import(r, "Foo.svelte"));
    ASSERT(has_import(r, "lib/utils"));
    ASSERT(has_import(r, "helpers"));
    cbm_free_result(r);
    PASS();
}

TEST(svelte_imports_no_script) {
    /* .svelte with no <script> block must not crash, 0 imports */
    CBMFileResult *r = extract("<h1>Static page</h1>\n"
                               "<p>No script here.</p>\n",
                               CBM_LANG_SVELTE, "t", "Static.svelte");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(r->imports.count, 0);
    cbm_free_result(r);
    PASS();
}

TEST(vue_imports_basic) {
    /* Vue SFC: same document→script_element→raw_text AST structure */
    CBMFileResult *r = extract("<template><div>{{ msg }}</div></template>\n"
                               "<script>\n"
                               "import MyComp from './MyComp.vue';\n"
                               "import { ref } from 'vue';\n"
                               "export default { name: 'App' };\n"
                               "</script>\n",
                               CBM_LANG_VUE, "t", "App.vue");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->imports.count, 2);
    ASSERT(has_import(r, "MyComp.vue"));
    ASSERT(has_import(r, "vue"));
    cbm_free_result(r);
    PASS();
}

TEST(html_imports_basic) {
    /* Plain HTML with inline ES module imports — same generic walker. */
    CBMFileResult *r = extract("<!DOCTYPE html><html><head>\n"
                               "<script type=\"module\">\n"
                               "import { renderApp } from './app.js';\n"
                               "import * as utils from './utils.js';\n"
                               "renderApp();\n"
                               "</script>\n"
                               "</head><body></body></html>\n",
                               CBM_LANG_HTML, "t", "index.html");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(r->imports.count, 2);
    ASSERT(has_import(r, "app.js"));
    ASSERT(has_import(r, "utils.js"));
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * config_extraction_test.go ports (25 tests)
 * ═══════════════════════════════════════════════════════════════════ */

/* --- TOML (8 tests) --- */

TEST(toml_basic_table_and_pair) {
    CBMFileResult *r = extract("[database]\nhost = \"localhost\"\nport = 5432\n", CBM_LANG_TOML,
                               "t", "config.toml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_defs_with_label(r, "Class"), 1);
    ASSERT(has_def(r, "Class", "database"));
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 2);
    ASSERT(has_def(r, "Variable", "host"));
    ASSERT(has_def(r, "Variable", "port"));
    cbm_free_result(r);
    PASS();
}

TEST(toml_nested_table) {
    CBMFileResult *r = extract("[server.http]\nport = 8080\n", CBM_LANG_TOML, "t", "config.toml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Class"), 1);
    cbm_free_result(r);
    PASS();
}

TEST(toml_table_array_element) {
    CBMFileResult *r = extract("[[servers]]\nname = \"alpha\"\n[[servers]]\nname = \"beta\"\n",
                               CBM_LANG_TOML, "t", "config.toml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Class"), 2);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 2);
    cbm_free_result(r);
    PASS();
}

TEST(toml_dotted_key) {
    CBMFileResult *r =
        extract("database.host = \"localhost\"\n", CBM_LANG_TOML, "t", "config.toml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 1);
    cbm_free_result(r);
    PASS();
}

TEST(toml_quoted_key) {
    CBMFileResult *r = extract("\"unusual-key\" = \"value\"\n", CBM_LANG_TOML, "t", "config.toml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 1);
    cbm_free_result(r);
    PASS();
}

TEST(toml_empty_table) {
    CBMFileResult *r = extract("[empty]\n", CBM_LANG_TOML, "t", "config.toml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_defs_with_label(r, "Class"), 1);
    ASSERT(has_def(r, "Class", "empty"));
    ASSERT_EQ(count_defs_with_label(r, "Variable"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(toml_comments_only) {
    CBMFileResult *r =
        extract("# just a comment\n# another comment\n", CBM_LANG_TOML, "t", "config.toml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_defs_with_label(r, "Class"), 0);
    ASSERT_EQ(count_defs_with_label(r, "Variable"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(toml_boolean_and_integer_values) {
    CBMFileResult *r =
        extract("enabled = true\ncount = 42\nname = \"test\"\n", CBM_LANG_TOML, "t", "config.toml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 3);
    cbm_free_result(r);
    PASS();
}

/* --- INI (4 tests) --- */

TEST(ini_basic_section_and_setting) {
    CBMFileResult *r =
        extract("[database]\nhost = localhost\nport = 5432\n", CBM_LANG_INI, "t", "config.ini");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Class"), 1);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 2);
    cbm_free_result(r);
    PASS();
}

TEST(ini_multiple_sections) {
    CBMFileResult *r = extract("[section1]\nkey1 = val1\n[section2]\nkey2 = val2\n", CBM_LANG_INI,
                               "t", "config.ini");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Class"), 2);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 2);
    cbm_free_result(r);
    PASS();
}

TEST(ini_global_keys) {
    CBMFileResult *r = extract("key1 = value1\nkey2 = value2\n", CBM_LANG_INI, "t", "config.ini");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_defs_with_label(r, "Class"), 0);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 2);
    cbm_free_result(r);
    PASS();
}

TEST(ini_comments) {
    CBMFileResult *r = extract("; comment\n# another comment\n[section]\nkey = val\n", CBM_LANG_INI,
                               "t", "config.ini");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Class"), 1);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 1);
    cbm_free_result(r);
    PASS();
}

/* --- JSON (5 tests) --- */

TEST(json_basic_pair) {
    CBMFileResult *r =
        extract("{\"host\": \"localhost\", \"port\": 5432}", CBM_LANG_JSON, "t", "config.json");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 2);
    ASSERT(has_def(r, "Variable", "host"));
    ASSERT(has_def(r, "Variable", "port"));
    cbm_free_result(r);
    PASS();
}

TEST(json_nested_object) {
    CBMFileResult *r = extract("{\"database\": {\"host\": \"localhost\", \"port\": 5432}}",
                               CBM_LANG_JSON, "t", "config.json");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 3);
    cbm_free_result(r);
    PASS();
}

TEST(json_empty_object) {
    CBMFileResult *r = extract("{}", CBM_LANG_JSON, "t", "config.json");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_defs_with_label(r, "Variable"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(json_boolean_null_values) {
    CBMFileResult *r = extract("{\"enabled\": true, \"value\": null, \"name\": \"test\"}",
                               CBM_LANG_JSON, "t", "config.json");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 3);
    cbm_free_result(r);
    PASS();
}

TEST(json_package_json_deps) {
    CBMFileResult *r =
        extract("{\"name\":\"pkg\",\"dependencies\":{\"express\":\"^4.0\",\"lodash\":\"^4.17\"}}",
                CBM_LANG_JSON, "t", "package.json");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Variable"), 4);
    ASSERT(has_def(r, "Variable", "name"));
    ASSERT(has_def(r, "Variable", "dependencies"));
    ASSERT(has_def(r, "Variable", "express"));
    ASSERT(has_def(r, "Variable", "lodash"));
    cbm_free_result(r);
    PASS();
}

/* --- XML (4 tests) --- */

TEST(xml_basic_element) {
    CBMFileResult *r = extract(
        "<?xml version=\"1.0\"?><config><database><host>localhost</host></database></config>",
        CBM_LANG_XML, "t", "config.xml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Class"), 3);
    ASSERT(has_def(r, "Class", "config"));
    ASSERT(has_def(r, "Class", "database"));
    ASSERT(has_def(r, "Class", "host"));
    cbm_free_result(r);
    PASS();
}

TEST(xml_self_closing_tag) {
    CBMFileResult *r =
        extract("<?xml version=\"1.0\"?><config><feature enabled=\"true\"/></config>", CBM_LANG_XML,
                "t", "config.xml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Class"), 2);
    cbm_free_result(r);
    PASS();
}

TEST(xml_empty_document) {
    CBMFileResult *r = extract("<?xml version=\"1.0\"?><root/>", CBM_LANG_XML, "t", "config.xml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Class"), 1);
    cbm_free_result(r);
    PASS();
}

TEST(xml_multiple_children) {
    CBMFileResult *r =
        extract("<?xml version=\"1.0\"?><servers><server/><server/><server/></servers>",
                CBM_LANG_XML, "t", "config.xml");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Class"), 4); /* servers + 3x server */
    cbm_free_result(r);
    PASS();
}

/* --- Markdown (4 tests) --- */

TEST(markdown_atx_headings) {
    CBMFileResult *r =
        extract("# Title\n## Section\n### Subsection\n", CBM_LANG_MARKDOWN, "t", "README.md");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Section"), 3);
    ASSERT_EQ(count_defs_with_label(r, "Class"), 0); /* Markdown: Section, not Class */
    cbm_free_result(r);
    PASS();
}

TEST(markdown_setext_headings) {
    CBMFileResult *r =
        extract("Title\n=====\nSection\n------\n", CBM_LANG_MARKDOWN, "t", "README.md");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Section"), 2);
    cbm_free_result(r);
    PASS();
}

TEST(markdown_heading_content) {
    CBMFileResult *r = extract("# Installation Guide\n## Prerequisites\n## Setup\n",
                               CBM_LANG_MARKDOWN, "t", "README.md");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_GTE(count_defs_with_label(r, "Section"), 3);
    ASSERT(has_def(r, "Section", "Installation Guide"));
    ASSERT(has_def(r, "Section", "Prerequisites"));
    ASSERT(has_def(r, "Section", "Setup"));
    cbm_free_result(r);
    PASS();
}

TEST(markdown_no_headings) {
    CBMFileResult *r =
        extract("Just a paragraph\n\nAnother paragraph\n", CBM_LANG_MARKDOWN, "t", "README.md");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_defs_with_label(r, "Section"), 0);
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Python __init__.py Module QN collision regression
 * ═══════════════════════════════════════════════════════════════════ */

TEST(python_init_module_qn_not_collide_with_folder) {
    /* Bug: __init__.py Module QN was identical to the Folder QN for the
     * same directory, causing the Folder node to be overwritten when the
     * Module was upserted. The Module QN must contain "__init__" to
     * distinguish it from the Folder QN. */
    CBMFileResult *r = extract("class Config:\n    DEBUG = True\n\ndef setup():\n    pass\n",
                               CBM_LANG_PYTHON, "proj", "mypackage/__init__.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    /* Module node must exist */
    ASSERT_GTE(r->defs.count, 1);
    ASSERT_STR_EQ(r->defs.items[0].label, "Module");

    /* Module QN must contain __init__ (not be stripped to just "proj.mypackage") */
    ASSERT_NOT_NULL(r->module_qn);
    ASSERT_NOT_NULL(strstr(r->module_qn, "__init__"));

    /* But symbols inside __init__.py should NOT have __init__ in their QN */
    int found_config = 0;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].name, "Config") == 0) {
            ASSERT_NOT_NULL(r->defs.items[i].qualified_name);
            /* Should be "proj.mypackage.Config", NOT "proj.mypackage.__init__.Config" */
            ASSERT_STR_EQ(r->defs.items[i].qualified_name, "proj.mypackage.Config");
            found_config = 1;
        }
    }
    ASSERT_EQ(found_config, 1);

    cbm_free_result(r);
    PASS();
}

TEST(python_init_nested_module_qn) {
    /* Deeply nested __init__.py — same collision must not happen */
    CBMFileResult *r = extract("def greet():\n    return 'hello'\n", CBM_LANG_PYTHON, "proj",
                               "docker-images/cloud-runs/bq-sync-api/__init__.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_NOT_NULL(r->module_qn);
    /* Must contain __init__ to not collide with Folder QN */
    ASSERT_NOT_NULL(strstr(r->module_qn, "__init__"));
    cbm_free_result(r);
    PASS();
}

TEST(js_index_module_qn_not_collide_with_folder) {
    /* Same bug for JS/TS index.ts files */
    CBMFileResult *r = extract("export function App() { return null; }\n", CBM_LANG_TYPESCRIPT,
                               "proj", "src/components/index.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_NOT_NULL(r->module_qn);
    /* Must contain "index" to not collide with Folder QN */
    ASSERT_NOT_NULL(strstr(r->module_qn, "index"));
    cbm_free_result(r);
    PASS();
}

TEST(python_regular_module_qn_unchanged) {
    /* Non-__init__.py Python files should be unaffected */
    CBMFileResult *r =
        extract("def helper():\n    pass\n", CBM_LANG_PYTHON, "proj", "mypackage/utils.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_NOT_NULL(r->module_qn);
    /* Regular module QN should not contain __init__ or index */
    ASSERT_STR_EQ(r->module_qn, "proj.mypackage.utils");
    cbm_free_result(r);
    PASS();
}

/* Find a definition by name; returns the item or NULL. */
static const CBMDefinition *find_def_by_name(CBMFileResult *r, const char *name) {
    for (int i = 0; i < r->defs.count; i++) {
        if (r->defs.items[i].name && strcmp(r->defs.items[i].name, name) == 0) {
            return &r->defs.items[i];
        }
    }
    return NULL;
}

static int decorators_contain(const CBMDefinition *d, const char *needle) {
    if (!d || !d->decorators) {
        return 0;
    }
    for (int i = 0; d->decorators[i]; i++) {
        if (strstr(d->decorators[i], needle)) {
            return 1;
        }
    }
    return 0;
}

/* #1692: each C# `[Attr]` bracket group is its own attribute_list sibling, and
 * a field lookup only ever returned the first, dropping every later attribute. */
TEST(extract_csharp_stacked_attributes_issue1692) {
    CBMFileResult *r = extract("public class AttrController {\n"
                               "    [Foo]\n"
                               "    [Bar(\"x\")]\n"
                               "    [Baz]\n"
                               "    public void StackedAttrsMethod() { }\n"
                               "}\n",
                               CBM_LANG_CSHARP, "t", "StackedAttrs.cs");
    ASSERT_NOT_NULL(r);
    const CBMDefinition *d = find_def_by_name(r, "StackedAttrsMethod");
    ASSERT_NOT_NULL(d);
    ASSERT_TRUE(decorators_contain(d, "Foo"));
    ASSERT_TRUE(decorators_contain(d, "Bar"));
    ASSERT_TRUE(decorators_contain(d, "Baz"));
    cbm_free_result(r);
    PASS();
}

static const CBMCall *find_call_by_callee(CBMFileResult *r, const char *callee);

/* Issue #1009: URL-builder helper pattern — a function returning a URL-shaped
 * literal, consumed as client(buildPath(id)). The builder's URL is recorded in
 * the per-file constant map and resolved at the call site, for both return
 * statements and arrow expression bodies. */
TEST(extract_ts_url_builder_issue1009) {
    CBMFileResult *r = extract("function thingDetail(id: string): string {\n"
                               "  return `/api/v1/things/${id}/detail`;\n"
                               "}\n"
                               "const arrowPath = (id: string) => `/api/v1/arrows/${id}`;\n"
                               "export function useThing(id: string) {\n"
                               "  return apiGet(thingDetail(id));\n"
                               "}\n"
                               "export function useArrow(id: string) {\n"
                               "  return apiFetch(arrowPath(id));\n"
                               "}\n",
                               CBM_LANG_TYPESCRIPT, "t", "builders.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMCall *c1 = find_call_by_callee(r, "apiGet");
    ASSERT_NOT_NULL(c1);
    ASSERT_NOT_NULL(c1->first_string_arg);
    ASSERT_STR_EQ(c1->first_string_arg, "/api/v1/things/{}/detail");
    const CBMCall *c2 = find_call_by_callee(r, "apiFetch");
    ASSERT_NOT_NULL(c2);
    ASSERT_NOT_NULL(c2->first_string_arg);
    ASSERT_STR_EQ(c2->first_string_arg, "/api/v1/arrows/{}");
    cbm_free_result(r);
    PASS();
}

/* A route registration names its middleware before its handler, and every
 * framework here puts the handler last. The handler scan took the FIRST
 * argument that looked like a function reference, so a named middleware won
 * and the HANDLES edge pointed at the middleware instead of the handler. */
TEST(extract_ts_route_handler_after_named_middleware) {
    CBMFileResult *r = extract("function requireAuth(req: any, res: any, next: any) { next(); }\n"
                               "function rateLimit(req: any, res: any, next: any) { next(); }\n"
                               "function listUsers(req: any, res: any) { res.json([]); }\n"
                               "routerGet(\"/users\", requireAuth, rateLimit, listUsers);\n",
                               CBM_LANG_TYPESCRIPT, "t", "routes.ts");
    ASSERT_NOT_NULL(r);
    const CBMCall *c = find_call_by_callee(r, "routerGet");
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(c->first_string_arg);
    ASSERT_STR_EQ(c->first_string_arg, "/users");
    ASSERT_NOT_NULL(c->second_arg_name);
    ASSERT_STR_EQ(c->second_arg_name, "listUsers");
    cbm_free_result(r);
    PASS();
}

/* The same route with its middleware written inline. An arrow function is not
 * one of the kinds the handler scan accepts, so three of them pushed the real
 * handler past the scan bound and no handler came back at all. */
TEST(extract_ts_route_handler_after_inline_middleware) {
    CBMFileResult *r = extract("function listOrders(req: any, res: any) { res.json([]); }\n"
                               "routerGet(\"/orders\",\n"
                               "  (req: any, res: any, next: any) => { next(); },\n"
                               "  (req: any, res: any, next: any) => { next(); },\n"
                               "  (req: any, res: any, next: any) => { next(); },\n"
                               "  listOrders);\n",
                               CBM_LANG_TYPESCRIPT, "t", "orders.ts");
    ASSERT_NOT_NULL(r);
    const CBMCall *c = find_call_by_callee(r, "routerGet");
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(c->first_string_arg);
    ASSERT_STR_EQ(c->first_string_arg, "/orders");
    ASSERT_NOT_NULL(c->second_arg_name);
    ASSERT_STR_EQ(c->second_arg_name, "listOrders");
    cbm_free_result(r);
    PASS();
}

/* Issue #1009 (composed builders): a builder whose template inlines an earlier
 * builder's call plus a query string: `return \`${basePath(id)}?${params}\``.
 * The known-substitution is inlined and the query string is truncated, so the
 * resolved URL joins the server route exactly. */
TEST(extract_ts_url_builder_composed_issue1009) {
    CBMFileResult *r = extract("function activityPath(id: string): string {\n"
                               "  return `/api/v1/team-members/${id}/activity`;\n"
                               "}\n"
                               "function buildPath(id: string, cursor: string): string {\n"
                               "  const params = new URLSearchParams();\n"
                               "  params.set('cursor', cursor);\n"
                               "  return `${activityPath(id)}?${params.toString()}`;\n"
                               "}\n"
                               "export function useActivity(id: string, cursor: string) {\n"
                               "  return apiGet(buildPath(id, cursor));\n"
                               "}\n",
                               CBM_LANG_TYPESCRIPT, "t", "composed.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMCall *c = find_call_by_callee(r, "apiGet");
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(c->first_string_arg);
    ASSERT_STR_EQ(c->first_string_arg, "/api/v1/team-members/{}/activity");
    cbm_free_result(r);
    PASS();
}

static bool any_call_arg_resolves_to(CBMFileResult *r, const char *url) {
    for (int i = 0; i < r->calls.count; i++) {
        const CBMCall *c = &r->calls.items[i];
        if (c->first_string_arg && strcmp(c->first_string_arg, url) == 0) {
            return true;
        }
        for (int a = 0; a < c->arg_count; a++) {
            if (c->args[a].value && strcmp(c->args[a].value, url) == 0) {
                return true;
            }
        }
    }
    return false;
}

TEST(extract_c_url_builder_gated_issue1009) {
    CBMFileResult *r = extract("static const char *cfg_path(void) {\n"
                               "  return \"/etc/myapp/conf.d\";\n"
                               "}\n"
                               "void init(void) {\n"
                               "  parse_config(cfg_path());\n"
                               "}\n",
                               CBM_LANG_C, "t", "conf.c");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_FALSE(any_call_arg_resolves_to(r, "/etc/myapp/conf.d"));
    cbm_free_result(r);
    PASS();
}

/* One literal return next to a computed one would attribute the literal to every
 * call site, including those taking the computed branch. A builder whose returns
 * are not all URL literals is declined. */
TEST(extract_ts_url_builder_mixed_returns_issue1009) {
    CBMFileResult *r = extract("function pathFor(kind: string): string {\n"
                               "  if (kind === 'user') return '/api/users';\n"
                               "  return computePath(kind);\n"
                               "}\n"
                               "export function load(kind: string) {\n"
                               "  return apiGet(pathFor(kind));\n"
                               "}\n",
                               CBM_LANG_TYPESCRIPT, "t", "mixed.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_FALSE(any_call_arg_resolves_to(r, "/api/users"));
    cbm_free_result(r);
    PASS();
}

/* Two different literal URLs make the call site's route unknowable, so the
 * builder is tombstoned and lookups miss. */
TEST(extract_ts_url_builder_ambiguous_issue1009) {
    CBMFileResult *r = extract("function pathFor(kind: string): string {\n"
                               "  if (kind === 'user') return '/api/users';\n"
                               "  return '/api/teams';\n"
                               "}\n"
                               "export function load(kind: string) {\n"
                               "  return apiGet(pathFor(kind));\n"
                               "}\n",
                               CBM_LANG_TYPESCRIPT, "t", "ambiguous.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_FALSE(any_call_arg_resolves_to(r, "/api/users"));
    ASSERT_FALSE(any_call_arg_resolves_to(r, "/api/teams"));
    cbm_free_result(r);
    PASS();
}

/* Handing a builder to a callback position builds no URL. Only a call to it
 * resolves, so the mapping function does not inherit an HTTP_CALLS edge to the
 * route. */
TEST(extract_ts_url_builder_reference_issue1009) {
    CBMFileResult *r = extract("function thingPath(id: string): string {\n"
                               "  return `/api/v1/things/${id}`;\n"
                               "}\n"
                               "export function loadAll(ids: string[]) {\n"
                               "  return ids.map(thingPath);\n"
                               "}\n",
                               CBM_LANG_TYPESCRIPT, "t", "reference.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_FALSE(any_call_arg_resolves_to(r, "/api/v1/things/{}"));
    cbm_free_result(r);
    PASS();
}

/* A helper returning an ordinary string is not a URL builder, so its call site
 * gets no resolved string argument. */
TEST(extract_ts_url_builder_non_url_issue1009) {
    CBMFileResult *r = extract("function label(): string {\n"
                               "  return 'plain text';\n"
                               "}\n"
                               "export function render() {\n"
                               "  return send(label());\n"
                               "}\n",
                               CBM_LANG_TYPESCRIPT, "t", "label.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMCall *c = find_call_by_callee(r, "send");
    ASSERT_NOT_NULL(c);
    ASSERT_NULL(c->first_string_arg);
    ASSERT_FALSE(any_call_arg_resolves_to(r, "plain text"));
    cbm_free_result(r);
    PASS();
}

/* Issue #382: Java Method nodes had empty decorators / signature. */
TEST(extract_java_method_annotations_issue382) {
    CBMFileResult *r = extract("public class C {\n"
                               "  @GetMapping(\"/x\")\n"
                               "  public String cmd(String c) { return c; }\n"
                               "}\n",
                               CBM_LANG_JAVA, "t", "C.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *m = find_def_by_name(r, "cmd");
    ASSERT_NOT_NULL(m);
    ASSERT(decorators_contain(m, "GetMapping"));
    ASSERT_NOT_NULL(m->signature);
    ASSERT(m->signature[0] != '\0');
    cbm_free_result(r);
    PASS();
}

/* Find an in-body call by its raw callee text; returns the call or NULL. */
static const CBMCall *find_call_by_callee(CBMFileResult *r, const char *callee) {
    for (int i = 0; i < r->calls.count; i++) {
        if (r->calls.items[i].callee_name && strcmp(r->calls.items[i].callee_name, callee) == 0) {
            return &r->calls.items[i];
        }
    }
    return NULL;
}

/* #1892: the Swift grammar declares no "arguments" field, so the generic field
 * lookup read nothing and every Swift call lost its arguments. Without the URL
 * the service-pattern table cannot raise an HTTP_CALLS edge or a Route node,
 * even though Alamofire/Moya/URLSession are already listed in it. */
TEST(swift_call_string_arg_issue1892) {
    CBMFileResult *r =
        extract("func listWidgets() { AF.request(\"https://example.com/api/v1/widgets\") }\n",
                CBM_LANG_SWIFT, "t", "Client.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMCall *c = find_call_by_callee(r, "AF.request");
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(c->first_string_arg);
    ASSERT_STR_EQ(c->first_string_arg, "https://example.com/api/v1/widgets");
    cbm_free_result(r);
    PASS();
}

/* Swift labels its arguments, and each one sits in a value_argument node that
 * leads with the label. Reading the first child alone would return `with`
 * rather than the path. */
TEST(swift_labeled_call_string_arg_issue1892) {
    CBMFileResult *r =
        extract("func fetch() { URLSession.shared.dataTask(with: \"/api/v1/widgets/1\") }\n",
                CBM_LANG_SWIFT, "t", "Fetch.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMCall *c = find_call_by_callee(r, "URLSession.shared.dataTask");
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(c->first_string_arg);
    ASSERT_STR_EQ(c->first_string_arg, "/api/v1/widgets/1");
    cbm_free_result(r);
    PASS();
}

/* Swift has no URL literal, so real code builds one and force-unwraps it. The
 * string then sits two levels below the argument list. */
TEST(swift_nested_url_constructor_issue1892) {
    CBMFileResult *r = extract("func fetch() { URLSession.shared.dataTask(with: URL(string: "
                               "\"https://example.com/api/v1/widgets\")!) }\n",
                               CBM_LANG_SWIFT, "t", "Fetch.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMCall *c = find_call_by_callee(r, "URLSession.shared.dataTask");
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(c->first_string_arg);
    ASSERT_STR_EQ(c->first_string_arg, "https://example.com/api/v1/widgets");
    cbm_free_result(r);
    PASS();
}

/* Without the trailing \"!\" the constructor is not wrapped in a
 * postfix_expression, so this covers the other shape. */
TEST(swift_nested_url_no_bang_issue1892) {
    CBMFileResult *r =
        extract("func fetch() { client.send(to: URLRequest(url: \"/api/v1/widgets/1\")) }\n",
                CBM_LANG_SWIFT, "t", "Send.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMCall *c = find_call_by_callee(r, "client.send");
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(c->first_string_arg);
    ASSERT_STR_EQ(c->first_string_arg, "/api/v1/widgets/1");
    cbm_free_result(r);
    PASS();
}

/* A constructor that is not one of the three URL types keeps its own meaning:
 * the outer call must not borrow the inner call's string. */
TEST(swift_non_url_constructor_untouched_issue1892) {
    CBMFileResult *r = extract("func f() { log.write(to: Formatter(pattern: \"%s-%d\")) }\n",
                               CBM_LANG_SWIFT, "t", "Log.swift");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMCall *c = find_call_by_callee(r, "log.write");
    ASSERT_NOT_NULL(c);
    ASSERT_NULL(c->first_string_arg);
    cbm_free_result(r);
    PASS();
}


/* Reproduce-first: Java module QN must derive from the CONTAINING DIRECTORY, not
 * the filename stem, so a top-level class `Outer` in `Outer.java` is `t.Outer`,
 * NOT the doubled `t.Outer.Outer`. The nested method def QN must also equal the
 * QN the textual calls-enclosing path records for an in-body call (the
 * lsp_resolve join keys on exact caller_qn == enclosing_func_qn equality). */
TEST(extract_java_no_double_class_qn) {
    CBMFileResult *r = extract("class Outer {\n"
                               "    int helper(int x) { return x + 2; }\n"
                               "    class Inner {\n"
                               "        int run(int v) { return helper(v); }\n"
                               "    }\n"
                               "}\n",
                               CBM_LANG_JAVA, "t", "Outer.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    /* Module QN is the directory (root) → just the project. */
    ASSERT_NOT_NULL(r->module_qn);
    ASSERT_STR_EQ(r->module_qn, "t");

    /* No def QN anywhere may double the top-level class name. */
    for (int i = 0; i < r->defs.count; i++) {
        const char *qn = r->defs.items[i].qualified_name;
        if (qn) {
            ASSERT_EQ(strstr(qn, "Outer.Outer"), NULL);
        }
    }

    /* The nested class and its method carry the single-form QN. */
    const CBMDefinition *outer = find_def_by_name(r, "Outer");
    ASSERT_NOT_NULL(outer);
    ASSERT_STR_EQ(outer->qualified_name, "t.Outer");

    const CBMDefinition *run = find_def_by_name(r, "run");
    ASSERT_NOT_NULL(run);
    ASSERT_STR_EQ(run->qualified_name, "t.Outer.Inner.run");

    /* The in-body call to helper() must be attributed to the SAME QN as the
     * method def — this is the equality the LSP cross-resolution join relies on
     * for nested classes (the lsp_outer_dispatch repro). */
    const CBMCall *call = find_call_by_callee(r, "helper");
    ASSERT_NOT_NULL(call);
    ASSERT_NOT_NULL(call->enclosing_func_qn);
    ASSERT_STR_EQ(call->enclosing_func_qn, run->qualified_name);

    cbm_free_result(r);
    PASS();
}

/* Reproduce-first: Go module QN must derive from the CONTAINING DIRECTORY
 * (package), not the filename stem, so a type/method in `myapp/db/conn.go`
 * belongs to module `proj.myapp.db` and is NOT polluted with the `.conn.`
 * filename segment. */
TEST(extract_go_no_filename_in_module_qn) {
    CBMFileResult *r = extract("package db\n\n"
                               "type Conn struct{}\n\n"
                               "func (c *Conn) Query() {}\n",
                               CBM_LANG_GO, "proj", "myapp/db/conn.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    /* Module is the directory `myapp/db`, NOT `myapp/db/conn`. */
    ASSERT_NOT_NULL(r->module_qn);
    ASSERT_STR_EQ(r->module_qn, "proj.myapp.db");

    /* The type and method QNs must not contain the filename segment `.conn.`. */
    const CBMDefinition *conn = find_def_by_name(r, "Conn");
    ASSERT_NOT_NULL(conn);
    ASSERT_STR_EQ(conn->qualified_name, "proj.myapp.db.Conn");

    /* Go method nodes keep a FLAT QN (module + name) with a separate
     * parent_class link to the receiver type — the QN must carry the
     * directory-based module and NOT the `.conn.` filename segment. */
    const CBMDefinition *query = find_def_by_name(r, "Query");
    ASSERT_NOT_NULL(query);
    ASSERT_STR_EQ(query->qualified_name, "proj.myapp.db.Query");
    ASSERT_EQ(strstr(query->qualified_name, ".conn."), NULL);
    /* The method's parent_class must match the type node QN (for DEFINES_METHOD). */
    ASSERT_NOT_NULL(query->parent_class);
    ASSERT_STR_EQ(query->parent_class, "proj.myapp.db.Conn");

    cbm_free_result(r);
    PASS();
}

/* Issue #213: large TS files were indexed as a File node with zero children. */
TEST(extract_large_ts_has_functions_issue213) {
    enum { NFUNCS = 4000 };
    size_t cap = (size_t)NFUNCS * 80 + 64;
    char *src = (char *)malloc(cap);
    ASSERT_NOT_NULL(src);
    size_t off = 0;
    for (int i = 0; i < NFUNCS; i++) {
        off +=
            (size_t)snprintf(src + off, cap - off,
                             "export function fn%d(a: number): number { return a + %d; }\n", i, i);
    }
    CBMFileResult *r =
        cbm_extract_file(src, (int)off, CBM_LANG_TYPESCRIPT, "t", "big.ts", 0, NULL, NULL);
    ASSERT_NOT_NULL(r);
    int fns = count_defs_with_label(r, "Function");
    ASSERT_GT(fns, 0); /* must not silently produce zero children */
    cbm_free_result(r);
    free(src);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group: per-function complexity metrics (Tier A — local AST metrics)
 *
 * cbm_compute_complexity stamps each Function/Method with cyclomatic,
 * cognitive, loop_count and loop_depth in the same tree-sitter walk that
 * extracts the definition. loop_depth (max nested-loop depth) is the
 * polynomial-degree proxy used as a queryable bottleneck signal.
 * ═══════════════════════════════════════════════════════════════════ */

/* Return the first definition with the given name, or NULL. */
static const CBMDefinition *find_def(CBMFileResult *r, const char *name) {
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].name, name) == 0)
            return &r->defs.items[i];
    }
    return NULL;
}

TEST(complexity_nested_loops_depth) {
    CBMFileResult *r = extract("package p\n"
                               "func deepLoops() {\n"
                               "    for i := 0; i < 10; i++ {\n"
                               "        for j := 0; j < 10; j++ {\n"
                               "            for k := 0; k < 10; k++ {\n"
                               "                doWork()\n"
                               "            }\n"
                               "        }\n"
                               "    }\n"
                               "}\n",
                               CBM_LANG_GO, "t", "deep.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "deepLoops");
    ASSERT_NOT_NULL(d);
    ASSERT_EQ(d->loop_depth, 3); /* three nested for-loops */
    ASSERT_EQ(d->loop_count, 3);
    cbm_free_result(r);
    PASS();
}

TEST(complexity_loop_with_branch) {
    CBMFileResult *r = extract("package p\n"
                               "func single() {\n"
                               "    for i := 0; i < 10; i++ {\n"
                               "        if i > 5 {\n"
                               "            doWork()\n"
                               "        }\n"
                               "    }\n"
                               "}\n",
                               CBM_LANG_GO, "t", "single.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "single");
    ASSERT_NOT_NULL(d);
    ASSERT_EQ(d->loop_depth, 1);
    ASSERT_EQ(d->loop_count, 1);
    /* the nested `if` contributes a branch, so cyclomatic > 1 and the
     * nesting-weighted cognitive score is non-zero. */
    ASSERT_GT(d->complexity, 1);
    ASSERT_GT(d->cognitive, 0);
    cbm_free_result(r);
    PASS();
}

TEST(complexity_flat_no_loops) {
    CBMFileResult *r = extract("package p\n"
                               "func flat() {\n"
                               "    doWork()\n"
                               "    doMore()\n"
                               "}\n",
                               CBM_LANG_GO, "t", "flat.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "flat");
    ASSERT_NOT_NULL(d);
    ASSERT_EQ(d->loop_depth, 0);
    ASSERT_EQ(d->loop_count, 0);
    cbm_free_result(r);
    PASS();
}

/* A linear-scan call (contains) inside a loop → hidden O(n^2) signal. */
TEST(complexity_linear_scan_in_loop) {
    CBMFileResult *r = extract("package p\n"
                               "func scanInLoop(xs []int, t int) bool {\n"
                               "    for i := 0; i < len(xs); i++ {\n"
                               "        if contains(xs, t) {\n"
                               "            return true\n"
                               "        }\n"
                               "    }\n"
                               "    return false\n"
                               "}\n",
                               CBM_LANG_GO, "t", "scan.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "scanInLoop");
    ASSERT_NOT_NULL(d);
    ASSERT_GT(d->linear_scan_in_loop, 0); /* contains() called inside the for-loop */
    cbm_free_result(r);
    PASS();
}

/* Self-call inside a loop, not guarded by any conditional → recursion_in_loop
 * and unguarded_recursion both set. */
TEST(complexity_recursion_in_loop_unguarded) {
    CBMFileResult *r = extract("package p\n"
                               "func recurInLoop(n int) {\n"
                               "    for i := 0; i < n; i++ {\n"
                               "        recurInLoop(n - 1)\n"
                               "    }\n"
                               "}\n",
                               CBM_LANG_GO, "t", "recur.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "recurInLoop");
    ASSERT_NOT_NULL(d);
    ASSERT_TRUE(d->is_recursive);
    ASSERT_TRUE(d->recursion_in_loop);
    ASSERT_TRUE(d->unguarded_recursion); /* no self-call inside a conditional */
    cbm_free_result(r);
    PASS();
}

/* Self-call inside an `if` (a base-case guard) → recursive but NOT unguarded. */
TEST(complexity_guarded_recursion) {
    CBMFileResult *r = extract("package p\n"
                               "func guarded(n int) int {\n"
                               "    if n > 0 {\n"
                               "        return guarded(n - 1)\n"
                               "    }\n"
                               "    return 0\n"
                               "}\n",
                               CBM_LANG_GO, "t", "guarded.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "guarded");
    ASSERT_NOT_NULL(d);
    ASSERT_TRUE(d->is_recursive);
    ASSERT_FALSE(d->recursion_in_loop);
    ASSERT_FALSE(d->unguarded_recursion); /* self-call is guarded by `if n > 0` */
    cbm_free_result(r);
    PASS();
}

/* #599: super().save() inside a method named save is a parent-class call —
 * the receiver is super(), never self — so it must NOT flag self-recursion.
 * super()-ONLY fixture: no self.save() alongside, so the assertion cannot pass
 * vacuously off a genuine self-call. */
TEST(complexity_super_only_not_recursive) {
    CBMFileResult *r = extract("class B(A):\n"
                               "    def save(self):\n"
                               "        super().save()\n",
                               CBM_LANG_PYTHON, "t", "super_only.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "save");
    ASSERT_NOT_NULL(d);
    ASSERT_FALSE(d->is_recursive); /* parent-class call, not self-recursion */
    ASSERT_FALSE(d->unguarded_recursion);
    cbm_free_result(r);
    PASS();
}

/* #599: a same-named call on an unrelated receiver (axios.get inside a
 * function also named get) is delegation, not self-recursion. */
TEST(complexity_same_name_other_receiver_not_recursive) {
    CBMFileResult *r = extract("function get(url) {\n"
                               "    return axios.get(url);\n"
                               "}\n",
                               CBM_LANG_JAVASCRIPT, "t", "axios_get.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "get");
    ASSERT_NOT_NULL(d);
    ASSERT_FALSE(d->is_recursive); /* axios.get targets axios, not this fn */
    ASSERT_FALSE(d->unguarded_recursion);
    cbm_free_result(r);
    PASS();
}

/* Guard: genuine self-recursion through self/this receivers still trips the
 * detector after the receiver-aware narrowing (#599). */
TEST(complexity_self_receiver_still_recursive) {
    /* Python: self.recur() — same object. */
    CBMFileResult *r = extract("class C:\n"
                               "    def recur(self, n):\n"
                               "        if n > 0:\n"
                               "            self.recur(n - 1)\n",
                               CBM_LANG_PYTHON, "t", "self_recur.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "recur");
    ASSERT_NOT_NULL(d);
    ASSERT_TRUE(d->is_recursive);
    ASSERT_FALSE(d->unguarded_recursion); /* guarded by `if n > 0` */
    cbm_free_result(r);

    /* JS: this.step() — same object. */
    r = extract("class C {\n"
                "    step(n) {\n"
                "        if (n > 0) { this.step(n - 1); }\n"
                "    }\n"
                "}\n",
                CBM_LANG_JAVASCRIPT, "t", "this_step.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    d = find_def(r, "step");
    ASSERT_NOT_NULL(d);
    ASSERT_TRUE(d->is_recursive);
    ASSERT_FALSE(d->unguarded_recursion); /* guarded by `if (n > 0)` */
    cbm_free_result(r);
    PASS();
}

/* #599: chained receiver — self.obj.recur() inside recur targets self's FIELD
 * obj, a different object. The whole receiver chain ("self.obj") must be
 * compared, not just its first segment ("self"). */
TEST(complexity_chained_receiver_not_self) {
    CBMFileResult *r = extract("class C:\n"
                               "    def recur(self, n):\n"
                               "        self.obj.recur(n)\n",
                               CBM_LANG_PYTHON, "t", "chained_recur.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "recur");
    ASSERT_NOT_NULL(d);
    ASSERT_FALSE(d->is_recursive); /* self.obj is not self */
    ASSERT_FALSE(d->unguarded_recursion);
    cbm_free_result(r);
    PASS();
}

/* #599: Go method receiver — the enclosing def's own receiver identifier
 * (`s` in `func (s *Store) save()`) is whitelisted dynamically from
 * CBMDefinition.receiver, so s.save() still counts as self-recursion while
 * s.backup.save() (a field's same-named method) does not. */
TEST(complexity_go_method_receiver_self_recursion) {
    CBMFileResult *r = extract("package p\n"
                               "type Store struct{}\n"
                               "func (s *Store) save(n int) {\n"
                               "    if n > 0 {\n"
                               "        s.save(n - 1)\n"
                               "    }\n"
                               "}\n",
                               CBM_LANG_GO, "t", "store.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "save");
    ASSERT_NOT_NULL(d);
    ASSERT_TRUE(d->is_recursive);         /* s.save() == receiver s → self */
    ASSERT_FALSE(d->unguarded_recursion); /* guarded by `if n > 0` */
    cbm_free_result(r);

    /* Same-named method on a field of the receiver: NOT self-recursion. */
    r = extract("package p\n"
                "type Store struct{ backup *Store }\n"
                "func (s *Store) save(n int) {\n"
                "    s.backup.save(n)\n"
                "}\n",
                CBM_LANG_GO, "t", "store_backup.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    d = find_def(r, "save");
    ASSERT_NOT_NULL(d);
    ASSERT_FALSE(d->is_recursive); /* s.backup is not s */
    ASSERT_FALSE(d->unguarded_recursion);
    cbm_free_result(r);
    PASS();
}

/* Deep chained member access + parameter count structure smells. */
TEST(complexity_access_depth_and_params) {
    CBMFileResult *r = extract("package p\n"
                               "func deepAccess(x Foo, a int, b int, c int) int {\n"
                               "    return x.alpha.beta.gamma.delta\n"
                               "}\n",
                               CBM_LANG_GO, "t", "access.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *d = find_def(r, "deepAccess");
    ASSERT_NOT_NULL(d);
    ASSERT_GT(d->max_access_depth, 2); /* x.alpha.beta.gamma.delta */
    ASSERT_GTE(d->param_count, 3);     /* x, a, b, c (grouping may vary) */
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Perl call-graph noise (#459 follow-up)
 * ═══════════════════════════════════════════════════════════════════ */

/* Count calls whose callee_name is exactly `name` (has_call is substring). */
static int count_calls_exact(CBMFileResult *r, const char *name) {
    int n = 0;
    for (int i = 0; i < r->calls.count; i++) {
        if (r->calls.items[i].callee_name && strcmp(r->calls.items[i].callee_name, name) == 0)
            n++;
    }
    return n;
}

/* (b) A dotted config string must never be extracted as a callee. */
TEST(extract_perl_config_string_not_a_callee) {
    CBMFileResult *r = extract("package C;\n"
                               "sub run {\n"
                               "  my $cfg = { \"log4perl.appender.File.utf8\" => 1 };\n"
                               "  helper();\n"
                               "}\n"
                               "sub helper { return 1; }\n"
                               "1;\n",
                               CBM_LANG_PERL, "t", "app.pl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* No callee may contain a '.' (config/string tokens are rejected). */
    for (int i = 0; i < r->calls.count; i++) {
        ASSERT_TRUE(strchr(r->calls.items[i].callee_name, '.') == NULL);
    }
    /* (d) The genuine intra-file function call is still extracted. */
    ASSERT_TRUE(count_calls_exact(r, "helper") >= 1);
    cbm_free_result(r);
    PASS();
}

/* (a) A Perl builtin call is extracted as a non-method callee. Suppression of
 *     the resulting CALLS edge happens in the resolver (see test_registry.c /
 *     end-to-end); extraction itself keeps the bare builtin token. */
TEST(extract_perl_builtin_call_is_function_not_method) {
    CBMFileResult *r = extract("package B;\n"
                               "sub run {\n"
                               "  my @x;\n"
                               "  push @x, 1;\n"
                               "  keys %h;\n"
                               "}\n"
                               "1;\n",
                               CBM_LANG_PERL, "t", "b.pl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* push / keys are extracted (they are valid identifiers) ... */
    ASSERT_TRUE(has_call(r, "push"));
    /* ... and crucially are NOT flagged as method calls. */
    for (int i = 0; i < r->calls.count; i++) {
        ASSERT_FALSE(r->calls.items[i].is_method);
    }
    cbm_free_result(r);
    PASS();
}

/* (c) An arrow/method call is extracted with is_method=true so the resolver
 *     can suppress generic short-name matching for it. */
TEST(extract_perl_method_call_flags_is_method) {
    CBMFileResult *r = extract("package M;\n"
                               "sub run {\n"
                               "  my $self = shift;\n"
                               "  $self->commit();\n"
                               "  $dbh->commit();\n"
                               "  helper();\n"
                               "}\n"
                               "sub helper { return 1; }\n"
                               "1;\n",
                               CBM_LANG_PERL, "t", "m.pl");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* Every "commit" call is a method call (is_method set). */
    int commit_calls = 0;
    for (int i = 0; i < r->calls.count; i++) {
        if (strcmp(r->calls.items[i].callee_name, "commit") == 0) {
            commit_calls++;
            ASSERT_TRUE(r->calls.items[i].is_method);
        }
        /* The genuine function call is NOT a method. */
        if (strcmp(r->calls.items[i].callee_name, "helper") == 0) {
            ASSERT_FALSE(r->calls.items[i].is_method);
        }
    }
    ASSERT_TRUE(commit_calls >= 1);
    /* (d) genuine intra-file function call still extracted. */
    ASSERT_TRUE(count_calls_exact(r, "helper") >= 1);
    cbm_free_result(r);
    PASS();
}

/* Languages OUTSIDE the is_method flag set (only Perl and TS/JS/TSX set it) must
 * be unaffected: a Go method call never sets is_method. */
TEST(extract_flag_exempt_method_call_not_flagged_is_method) {
    CBMFileResult *r = extract("package m\n"
                               "func run(o Obj) { o.Commit(); helper() }\n",
                               CBM_LANG_GO, "t", "x.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    for (int i = 0; i < r->calls.count; i++) {
        ASSERT_FALSE(r->calls.items[i].is_method);
    }
    cbm_free_result(r);
    PASS();
}

/* Python receiver-aware flag (#1276; same intent as the Perl and TS/JS flags).
 * Pins BOTH directions: an unknown receiver (a parameter, or an attribute of
 * self) IS flagged so the resolver can suppress a weak short-name match, while
 * self/cls/super() and import-bound receivers — Python's canonical cross-file
 * call shape — are NOT, so their true edges survive. */
TEST(extract_python_member_call_flags_is_method) {
    CBMFileResult *r = extract("from pkg import helper\n"
                               "import tools as toolkit\n"
                               "\n"
                               "class C(Base):\n"
                               "    def run(self, external):\n"
                               "        external.commit()\n"
                               "        self.client.send()\n"
                               "        self.helper()\n"
                               "        super ( ).render()\n"
                               "        helper.compute()\n"
                               "        toolkit.format()\n"
                               "        helper()\n",
                               CBM_LANG_PYTHON, "t", "x.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int external = 0;
    int nested = 0;
    int self_call = 0;
    int super_call = 0;
    int imported_module = 0;
    int imported_alias = 0;
    int bare = 0;
    for (int i = 0; i < r->calls.count; i++) {
        const char *cn = r->calls.items[i].callee_name;
        if (strcmp(cn, "external.commit") == 0) {
            /* parameter receiver — unknown type */
            external++;
            ASSERT_TRUE(r->calls.items[i].is_method);
        } else if (strcmp(cn, "self.client.send") == 0) {
            /* receiver is `self.client`, an attribute of unknown type — NOT self */
            nested++;
            ASSERT_TRUE(r->calls.items[i].is_method);
        } else if (strcmp(cn, "self.helper") == 0) {
            self_call++;
            ASSERT_FALSE(r->calls.items[i].is_method);
        } else if (strstr(cn, "render") != NULL) {
            super_call++;
            ASSERT_FALSE(r->calls.items[i].is_method);
        } else if (strcmp(cn, "helper.compute") == 0) {
            imported_module++;
            ASSERT_FALSE(r->calls.items[i].is_method);
        } else if (strcmp(cn, "toolkit.format") == 0) {
            /* aliased import: local_name is "toolkit" */
            imported_alias++;
            ASSERT_FALSE(r->calls.items[i].is_method);
        } else if (strcmp(cn, "helper") == 0) {
            bare++;
            ASSERT_FALSE(r->calls.items[i].is_method);
        }
    }
    /* Each shape must appear exactly once, so a missed extraction cannot make
     * the loop above pass vacuously. */
    ASSERT_EQ(external, 1);
    ASSERT_EQ(nested, 1);
    ASSERT_EQ(self_call, 1);
    ASSERT_EQ(super_call, 1);
    ASSERT_EQ(imported_module, 1);
    ASSERT_EQ(imported_alias, 1);
    ASSERT_EQ(bare, 1);
    cbm_free_result(r);
    PASS();
}

/* Python bare-call local-binding flag (the bare-call counterpart of the
 * receiver flag above). Pins BOTH directions: a callee shadowed by a parameter
 * of an enclosing scope IS flagged so the resolver can suppress a weak
 * short-name match, while an unshadowed callee — a genuine module-level
 * function, an imported name, or a nested `def` — is NOT, so its true edge
 * survives. Every parameter binding form the grammar produces is covered, since
 * a form the extractor silently missed would leave that shape unguarded. */
TEST(extract_python_bare_call_flags_locally_bound_callee) {
    CBMFileResult *r = extract("from pkg import helper\n"
                               "\n"
                               "def outer(run, *rest, timeout=5, label: str = 'x', **opts):\n"
                               "    def inner():\n"
                               "        return run()\n"
                               "    rest()\n"
                               "    timeout()\n"
                               "    label()\n"
                               "    opts()\n"
                               "    module_level()\n"
                               "    helper()\n"
                               "    return inner()\n"
                               "\n"
                               "def typed(cb: Callable):\n"
                               "    return cb()\n"
                               "\n"
                               "apply_it = lambda fn: fn()\n",
                               CBM_LANG_PYTHON, "t", "x.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    /* callee name -> (expected flag, seen count) */
    struct {
        const char *callee;
        bool expect_bound;
        int seen;
    } cases[] = {
        {"run", true, 0},      /* closure over an ENCLOSING function's parameter */
        {"rest", true, 0},     /* *args   -> list_splat_pattern                  */
        {"timeout", true, 0},  /* default_parameter                              */
        {"label", true, 0},    /* typed_default_parameter (keyword-only)         */
        {"opts", true, 0},     /* **kwargs -> dictionary_splat_pattern           */
        {"cb", true, 0},       /* typed_parameter, no default                    */
        {"fn", true, 0},       /* lambda parameter                               */
        {"module_level", false, 0}, /* unbound: the true cross-file edge         */
        {"helper", false, 0},       /* imported name, not a parameter            */
        {"inner", false, 0},        /* nested def: a real target, keep the edge  */
    };
    const int case_count = (int)(sizeof(cases) / sizeof(cases[0]));

    for (int i = 0; i < r->calls.count; i++) {
        const char *cn = r->calls.items[i].callee_name;
        if (!cn) {
            continue;
        }
        for (int c = 0; c < case_count; c++) {
            if (strcmp(cn, cases[c].callee) != 0) {
                continue;
            }
            cases[c].seen++;
            if (r->calls.items[i].callee_is_locally_bound != cases[c].expect_bound) {
                printf("  bare-call flag mismatch for %s(): got %d, expected %d\n", cases[c].callee,
                       r->calls.items[i].callee_is_locally_bound ? 1 : 0,
                       cases[c].expect_bound ? 1 : 0);
            }
            ASSERT_EQ(r->calls.items[i].callee_is_locally_bound, cases[c].expect_bound);
        }
    }
    /* Each shape must appear exactly once, so a missed extraction cannot let the
     * loop above pass vacuously. */
    for (int c = 0; c < case_count; c++) {
        if (cases[c].seen != 1) {
            printf("  bare call %s() extracted %d times, expected 1\n", cases[c].callee,
                   cases[c].seen);
        }
        ASSERT_EQ(cases[c].seen, 1);
    }
    cbm_free_result(r);
    PASS();
}

/* The bare-call flag is DEPTH-INDEPENDENT, and the binding is UNWOUND when its
 * scope closes.
 *
 * Both properties come from the same design decision. The answer is carried by
 * the unified walk -- parameters are bound when a def or lambda scope opens and
 * unwound when it closes -- rather than recomputed per call by ascending the
 * tree. An ascending walk is O(depth) per call, and every level of f(f(f(...)))
 * is itself a bare call, so it is quadratic in a file's nesting depth; that hung
 * stack_overflow_b's 30,000-deep fixture rather than merely slowing it. An
 * earlier cut capped the ascent at 64 ancestors and FAILED OPEN past it, which
 * silently stopped suppressing on deep-but-ordinary code.
 *
 * Pinned deterministically rather than by wall clock -- a timing assertion would
 * be a lottery, not a gate. The depth case fails if a cap is reintroduced; the
 * unwind case fails if a frame's bindings outlive its scope. */
TEST(extract_python_bare_call_flag_is_depth_independent) {
    /* Shallow: return_statement / block / function_definition — 3 ancestors. */
    CBMFileResult *shallow = extract("def shallow(handler):\n"
                                     "    return handler()\n",
                                     CBM_LANG_PYTHON, "t", "s.py");
    ASSERT_NOT_NULL(shallow);
    ASSERT_FALSE(shallow->has_error);
    int shallow_seen = 0;
    for (int i = 0; i < shallow->calls.count; i++) {
        const char *cn = shallow->calls.items[i].callee_name;
        if (cn && strcmp(cn, "handler") == 0) {
            shallow_seen++;
            ASSERT_TRUE(shallow->calls.items[i].callee_is_locally_bound);
        }
    }
    ASSERT_EQ(shallow_seen, 1);
    cbm_free_result(shallow);

    /* Deep: 200 parenthesized_expression ancestors separate the SAME call from
     * its enclosing def. The parameter still shadows it, so it stays flagged --
     * depth changes nothing. This is the case a 64-ancestor cap got wrong. */
    const int PARENS = 200;
    size_t sz = (size_t)PARENS * 2 + 128;
    char *src = malloc(sz);
    ASSERT_NOT_NULL(src);
    char *w = src;
    w += snprintf(w, sz, "def deep(handler):\n    return ");
    memset(w, '(', (size_t)PARENS);
    w += PARENS;
    w += snprintf(w, sz - (size_t)(w - src), "handler()");
    memset(w, ')', (size_t)PARENS);
    w += PARENS;
    snprintf(w, sz - (size_t)(w - src), "\n");

    CBMFileResult *deep = extract(src, CBM_LANG_PYTHON, "t", "d.py");
    ASSERT_NOT_NULL(deep);
    ASSERT_FALSE(deep->has_error);
    int deep_seen = 0;
    for (int i = 0; i < deep->calls.count; i++) {
        const char *cn = deep->calls.items[i].callee_name;
        if (cn && strcmp(cn, "handler") == 0) {
            deep_seen++;
            ASSERT_TRUE(deep->calls.items[i].callee_is_locally_bound);
        }
    }
    ASSERT_EQ(deep_seen, 1);
    cbm_free_result(deep);
    free(src);

    /* Unwind: `handler` is a parameter of shadowed() and a module-level function
     * of the same name. The call INSIDE shadowed() is flagged; the call in
     * sibling(), after that scope closed, must NOT be — it really does resolve
     * to the module-level def. A binding that outlived its frame would flag it
     * and destroy a true edge, which is the one direction this guard must never
     * fail in. Nested same-name defs also pin the count: leaving the inner scope
     * must not unbind the outer one. */
    CBMFileResult *unwound = extract("def handler():\n"
                                     "    return 1\n"
                                     "\n"
                                     "def shadowed(handler):\n"
                                     "    def inner(handler):\n"
                                     "        return handler()\n"
                                     "    return inner(handler) or handler()\n"
                                     "\n"
                                     "def sibling():\n"
                                     "    return handler()\n",
                                     CBM_LANG_PYTHON, "t", "u.py");
    ASSERT_NOT_NULL(unwound);
    ASSERT_FALSE(unwound->has_error);
    int flagged = 0;
    int unflagged = 0;
    for (int i = 0; i < unwound->calls.count; i++) {
        const char *cn = unwound->calls.items[i].callee_name;
        if (!cn || strcmp(cn, "handler") != 0) {
            continue;
        }
        if (unwound->calls.items[i].callee_is_locally_bound) {
            flagged++;
        } else {
            unflagged++;
        }
    }
    /* Two shadowed calls (inner body, and shadowed()'s own tail) and exactly one
     * unshadowed call in sibling(). */
    ASSERT_EQ(flagged, 2);
    ASSERT_EQ(unflagged, 1);
    cbm_free_result(unwound);
    PASS();
}

/* TS/JS/TSX receiver-aware flag (#592/#606; same intent as the Perl flag above).
 * A member call x.foo() with a non-this/super receiver is flagged is_method so
 * the resolver can suppress a weak short-name match (`re.test()` must not bind a
 * project `test`); a bare call is not flagged. */
TEST(extract_ts_member_call_flags_is_method) {
    CBMFileResult *r = extract("const re = /^a+$/;\n"
                               "export function checkFormat(s: string) { return re.test(s); }\n"
                               "function helper() { return 1; }\n"
                               "export function run() { return helper(); }\n",
                               CBM_LANG_TYPESCRIPT, "t", "a.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int member = 0;
    int bare = 0;
    for (int i = 0; i < r->calls.count; i++) {
        const char *cn = r->calls.items[i].callee_name;
        if (strcmp(cn, "re.test") == 0) {
            member++;
            ASSERT_TRUE(r->calls.items[i].is_method);
        }
        if (strcmp(cn, "helper") == 0) {
            bare++;
            ASSERT_FALSE(r->calls.items[i].is_method);
        }
    }
    ASSERT_TRUE(member >= 1); /* re.test() flagged */
    ASSERT_TRUE(bare >= 1);   /* helper() not flagged */
    cbm_free_result(r);
    PASS();
}

/* this/super receivers keep the enclosing-class target, where a weak
 * namespace-proximity match is usually correct — so they are NOT flagged. A
 * new_expression has no member receiver and is never flagged either. */
TEST(extract_ts_this_super_receiver_not_flagged) {
    CBMFileResult *r = extract("class A extends B {\n"
                               "  m() { this.helper(); super.render(); return new A(); }\n"
                               "  helper() {}\n"
                               "}\n",
                               CBM_LANG_TYPESCRIPT, "t", "b.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    /* Every call here has a self receiver (this/super) or is a new-expression,
     * so NONE may be flagged. */
    for (int i = 0; i < r->calls.count; i++) {
        ASSERT_FALSE(r->calls.items[i].is_method);
    }
    /* Sanity: the this/super member calls were actually extracted. */
    ASSERT_TRUE(has_call(r, "helper")); /* this.helper */
    ASSERT_TRUE(has_call(r, "render")); /* super.render */
    cbm_free_result(r);
    PASS();
}

/* JS dialect behaves like TS (the flag is gated on the language set, not the
 * dialect). Replaces the pre-#592 test that asserted JS never flags is_method. */
TEST(extract_js_member_call_flags_is_method) {
    CBMFileResult *r =
        extract("function run(o){ o.commit(); helper(); }\n", CBM_LANG_JAVASCRIPT, "t", "x.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int member = 0;
    int bare = 0;
    for (int i = 0; i < r->calls.count; i++) {
        const char *cn = r->calls.items[i].callee_name;
        if (strcmp(cn, "o.commit") == 0) {
            member++;
            ASSERT_TRUE(r->calls.items[i].is_method);
        }
        if (strcmp(cn, "helper") == 0) {
            bare++;
            ASSERT_FALSE(r->calls.items[i].is_method);
        }
    }
    ASSERT_TRUE(member >= 1); /* o.commit() flagged */
    ASSERT_TRUE(bare >= 1);   /* helper() not flagged */
    cbm_free_result(r);
    PASS();
}

/* #668: walk_defs used a fixed `walk_defs_frame_t stack[4096]` — a ~160 KB
 * C-stack frame that overflowed small thread stacks (the reporter's crash was in
 * the "definitions pass" on a large SQL file), and whose `top < 4096` push guards
 * SILENTLY DROPPED every top-level definition past 4096. The growable heap stack
 * fixes both: a file with > 4096 top-level defs must extract ALL of them. RED
 * before the fix (extracted count capped near 4096), GREEN after. */
TEST(walk_defs_no_truncation_over_4096_issue668) {
    enum { N = 5000 };
    /* N top-level Python defs → N Function defs, all direct module children, so
     * walk_defs pushes all N children at once — the >4096 truncation site. */
    size_t cap = (size_t)N * 24 + 1;
    char *src = (char *)malloc(cap);
    ASSERT_NOT_NULL(src);
    size_t off = 0;
    for (int i = 0; i < N; i++) {
        off += (size_t)snprintf(src + off, cap - off, "def f%d(): pass\n", i);
    }
    CBMFileResult *r = extract(src, CBM_LANG_PYTHON, "t", "big.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    int nfuncs = count_defs_with_label(r, "Function");
    ASSERT(nfuncs >= N); /* all N present — not truncated at the old 4096 cap */
    cbm_free_result(r);
    free(src);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Suite
 * ═══════════════════════════════════════════════════════════════════ */

/* Rust: inline #[test]/#[tokio::test] functions must be marked is_test so the
 * store.c `is_test != 1` filter excludes them from graph context. Detection is
 * otherwise file-path-based (cbm_is_test_file), so test fns in a regular .rs
 * file leak. (#855) */
TEST(extract_rust_test_attr_marks_is_test_issue855) {
    CBMFileResult *r = extract("pub fn real_fn() {}\n"
                               "\n"
                               "#[test]\n"
                               "fn sync_test() {}\n"
                               "\n"
                               "#[tokio::test]\n"
                               "async fn async_test() {}\n",
                               CBM_LANG_RUST, "t", "src/lib.rs");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    int real = -1, sync = -1, asyn = -1;
    for (int i = 0; i < r->defs.count; i++) {
        const char *n = r->defs.items[i].name;
        if (!n) {
            continue;
        }
        if (strcmp(n, "real_fn") == 0) {
            real = r->defs.items[i].is_test ? 1 : 0;
        } else if (strcmp(n, "sync_test") == 0) {
            sync = r->defs.items[i].is_test ? 1 : 0;
        } else if (strcmp(n, "async_test") == 0) {
            asyn = r->defs.items[i].is_test ? 1 : 0;
        }
    }
    ASSERT(real >= 0 && sync >= 0 && asyn >= 0 && "all three fns extracted");
    ASSERT(real == 0 && "real_fn is NOT a test");
    ASSERT(sync == 1 && "#[test] fn is_test");
    ASSERT(asyn == 1 && "#[tokio::test] fn is_test");

    cbm_free_result(r);
    PASS();
}

/* Reproduce-first (ms-typescript reallyLargeFile.ts, 2026-07-07): a file
 * whose root node has hundreds of thousands of FLAT SIBLINGS (580k ////
 * comment lines in the 3.5 MB fourslash fixture) hung extraction for over
 * 15 minutes: walk_defs pushed children via index-based ts_node_child(i),
 * which is O(i) per call in tree-sitter — O(n^2) per wide node (~1.7e11
 * iterator steps on the real file; 100% of stack samples inside
 * ts_node_child_iterator_next). The supervisor then killed the silent
 * worker as a hang and, via the stale extraction marker, quarantined
 * INNOCENT files on every retry.
 *
 * This fixture is an 80k-sibling flat file: quadratic child access needs
 * minutes under ASan; the linear TSTreeCursor collection finishes in
 * milliseconds. The 30 s bound has ~100x headroom over the fixed cost —
 * RED on index-based child pushes, GREEN on the cursor walk. The def-count
 * guard keeps the test honest: extraction must actually process the whole
 * breadth, not skip it. */
/* Extract a wide-flat comment-sibling fixture of n lines (the exact shape of
 * ms-typescript's reallyLargeFile.ts: hundreds of thousands of flat comment
 * children under the root, plus sparse real defs so the breadth check cannot
 * pass vacuously). Returns elapsed milliseconds; stores the def count. */
static long extract_wide_flat_ms(int n, int *out_defs) {
    const size_t cap = (size_t)n * 24 + (size_t)8192; /* "// wide filler N\n" <= 24 chars */
    char *src = malloc(cap);
    if (!src) {
        return -1;
    }
    size_t off = 0;
    /* CONSTANT def count (10), independent of n: defs that scale WITH n make
     * the fixture superlinear through the separate per-def sibling-scan cost
     * (O(defs x siblings)) — on windows-CLANG64 ASan that pushed the ratio
     * of LINEAR walk code to 43x. Ten spread-out defs keep the breadth check
     * honest while the sibling-scan term stays 10 x n = linear. */
    const int def_stride = n / 10;
    for (int i = 0; i < n; i++) {
        off += (size_t)snprintf(src + off, cap - off, "// wide filler %d\n", i);
        if (i % def_stride == 0) {
            off += (size_t)snprintf(src + off, cap - off, "var wide_a%d = %d;\n", i, i);
        }
    }
    struct timespec a;
    struct timespec b;
    cbm_clock_gettime(CLOCK_MONOTONIC, &a);
    CBMFileResult *r =
        cbm_extract_file(src, (int)off, CBM_LANG_JAVASCRIPT, "proj", "wide.js", 0, NULL, NULL);
    cbm_clock_gettime(CLOCK_MONOTONIC, &b);
    free(src);
    if (!r) {
        return -1;
    }
    *out_defs = r->defs.count;
    cbm_free_result(r);
    return (b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
}

TEST(extract_wide_flat_file_is_linear) {
    /* SCALING-RATIO guard: assert the COMPLEXITY CLASS, not a wall-clock
     * bound. Index-based ts_node_child(i) child loops are O(i) per call —
     * quadratic per wide node — and hung a 580k-sibling file for hours
     * (ms-typescript reallyLargeFile.ts). An absolute time bound conflates
     * machine speed with complexity: the gcc-13-ARM ASan CI leg runs this
     * extraction ~200x slower than clang at the SAME (measured perfectly
     * linear: 27.1s/54.2s/108.3s for 50k/100k/200k) complexity, and flunked
     * a 30s bound on linear code. Growing the input 4x must grow the time
     * ~4x when linear and ~16x when quadratic; the 10x bound splits those
     * decisively on every toolchain. The 120ms floor keeps clock noise from
     * mattering on fast machines. */
    /* Growth and bound CALIBRATED FROM MEASUREMENT, not models. The ASan
     * test build carries a large LINEAR per-line baseline (~31us/line on
     * clang-macOS, ~540us/line on gcc-13-ARM) that dilutes small-growth
     * ratios: at 8x growth the measured quadratic ratio was 22.4 — a 24x
     * bound false-passed the known-quadratic pre-merge walk. At 20x growth
     * the measured ratios are ~20x for linear code (both toolchains) and
     * ~128x for the quadratic walk (clang-macOS) — bound 40 sits >=2x from
     * both. The 120ms floor keeps clock noise irrelevant on fast hosts. */
    enum { WF_SMALL = 20 * 1000, WF_BIG = 400 * 1000, WF_RATIO_MAX = 40, WF_FLOOR_MS = 120 };
    int defs_small = 0;
    int defs_big = 0;
    long t_small = extract_wide_flat_ms(WF_SMALL, &defs_small);
    long t_big = extract_wide_flat_ms(WF_BIG, &defs_big);
    ASSERT_GTE(t_small, 0);
    ASSERT_GTE(t_big, 0);
    /* Anti-vacuous guard: the breadth was actually walked at both sizes. */
    ASSERT_GTE(defs_small, 8);
    ASSERT_GTE(defs_big, 8);
    fprintf(stderr, "  [wide-flat] t(%d)=%ldms t(%d)=%ldms\n", WF_SMALL, t_small, WF_BIG, t_big);
    long base = t_small > WF_FLOOR_MS ? t_small : WF_FLOOR_MS;
    if (t_big > WF_RATIO_MAX * base) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "wide-flat scaling 20x input: %ldms -> %ldms (> %dx base %ldms) — "
                 "quadratic child access",
                 t_small, t_big, WF_RATIO_MAX, base);
        FAIL(msg);
    }
    PASS();
}

/* Ported from upstream a029b3a6. */
/* Issue #1006: JS/TS template-literal URLs must flatten ${...} substitutions
 * to the canonical "{}" placeholder, both as call arguments (HTTP_CALLS) and
 * as URL-shaped string_refs collected from const/return positions. */
TEST(extract_ts_template_string_url_issue1006) {
    CBMFileResult *r = extract("export function detailPath(id: string): string {\n"
                               "  return `/api/v1/things/${id}/detail`;\n"
                               "}\n"
                               "export function load(id: string) {\n"
                               "  return fetch(`/api/v1/things/${id}`);\n"
                               "}\n",
                               CBM_LANG_TYPESCRIPT, "t", "paths.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMCall *c = find_call_by_callee(r, "fetch");
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(c->first_string_arg);
    ASSERT_STR_EQ(c->first_string_arg, "/api/v1/things/{}");
    int found = 0;
    for (int i = 0; i < r->string_refs.count; i++) {
        if (r->string_refs.items[i].value &&
            strcmp(r->string_refs.items[i].value, "/api/v1/things/{}/detail") == 0) {
            found = 1;
            break;
        }
    }
    ASSERT(found);
    cbm_free_result(r);
    PASS();
}

/* Issue #1005: JAX-RS splits a route across two annotations (@GET carries the
 * verb, a sibling @Path carries the path). Returning on the first mapping
 * annotation dropped every method-level @Path, and the class-level @Path
 * prefix was never recognized at all. */
TEST(extract_java_jaxrs_path_composition_issue1005) {
    CBMFileResult *r = extract("import jakarta.ws.rs.GET;\n"
                               "import jakarta.ws.rs.Path;\n"
                               "@Path(\"/api/v1/widgets\")\n"
                               "public class WidgetResource {\n"
                               "  @GET\n"
                               "  public String list() { return \"\"; }\n"
                               "  @GET\n"
                               "  @Path(\"/count\")\n"
                               "  public String count() { return \"\"; }\n"
                               "}\n",
                               CBM_LANG_JAVA, "t", "WidgetResource.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *list = find_def_by_name(r, "list");
    ASSERT_NOT_NULL(list);
    ASSERT_NOT_NULL(list->route_path);
    ASSERT_STR_EQ(list->route_path, "/api/v1/widgets");
    ASSERT_STR_EQ(list->route_method, "GET");
    const CBMDefinition *count = find_def_by_name(r, "count");
    ASSERT_NOT_NULL(count);
    ASSERT_NOT_NULL(count->route_path);
    ASSERT_STR_EQ(count->route_path, "/api/v1/widgets/count");
    ASSERT_STR_EQ(count->route_method, "GET");
    cbm_free_result(r);
    PASS();
}

/* JAX-RS @Path values are URI templates relative to the enclosing resource;
 * the leading slash is optional and ignored by the framework. Relative class
 * and method templates must compose to the same rooted routes as the
 * slash-prefixed spelling above. */
TEST(extract_java_jaxrs_relative_path_templates) {
    CBMFileResult *r = extract("import jakarta.ws.rs.GET;\n"
                               "import jakarta.ws.rs.Path;\n"
                               "@Path(\"api/v1/widgets\")\n"
                               "public class WidgetResource {\n"
                               "  @GET\n"
                               "  public String list() { return \"\"; }\n"
                               "  @GET\n"
                               "  @Path(\"count\")\n"
                               "  public String count() { return \"\"; }\n"
                               "  @GET\n"
                               "  @Path(\"{id}/tags\")\n"
                               "  public String tags() { return \"\"; }\n"
                               "}\n",
                               CBM_LANG_JAVA, "t", "WidgetResource.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *list = find_def_by_name(r, "list");
    ASSERT_NOT_NULL(list);
    ASSERT_NOT_NULL(list->route_path);
    ASSERT_STR_EQ(list->route_path, "/api/v1/widgets");
    ASSERT_STR_EQ(list->route_method, "GET");
    const CBMDefinition *count = find_def_by_name(r, "count");
    ASSERT_NOT_NULL(count);
    ASSERT_NOT_NULL(count->route_path);
    ASSERT_STR_EQ(count->route_path, "/api/v1/widgets/count");
    ASSERT_STR_EQ(count->route_method, "GET");
    const CBMDefinition *tags = find_def_by_name(r, "tags");
    ASSERT_NOT_NULL(tags);
    ASSERT_NOT_NULL(tags->route_path);
    ASSERT_STR_EQ(tags->route_path, "/api/v1/widgets/{id}/tags");
    ASSERT_STR_EQ(tags->route_method, "GET");
    cbm_free_result(r);
    PASS();
}

/* @Path("") is legal JAX-RS and means "the class path itself". Accepting
 * relative templates must not turn it into an empty route path: the method
 * has to behave exactly like one without @Path (fallback to "/", then
 * class-level composition). The class without a class-level @Path is the
 * binding case: an empty method path would otherwise drop the route. */
TEST(extract_java_jaxrs_empty_path_means_class_path) {
    CBMFileResult *r = extract("import jakarta.ws.rs.GET;\n"
                               "import jakarta.ws.rs.Path;\n"
                               "@Path(\"api/v1/widgets\")\n"
                               "class WidgetResource {\n"
                               "  @GET\n"
                               "  @Path(\"\")\n"
                               "  public String root() { return \"\"; }\n"
                               "}\n"
                               "class RootResource {\n"
                               "  @GET\n"
                               "  @Path(\"\")\n"
                               "  public String index() { return \"\"; }\n"
                               "}\n",
                               CBM_LANG_JAVA, "t", "Resources.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *root = find_def_by_name(r, "root");
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(root->route_path);
    ASSERT_STR_EQ(root->route_path, "/api/v1/widgets");
    ASSERT_STR_EQ(root->route_method, "GET");
    const CBMDefinition *index = find_def_by_name(r, "index");
    ASSERT_NOT_NULL(index);
    ASSERT_NOT_NULL(index->route_path);
    ASSERT_STR_EQ(index->route_path, "/");
    ASSERT_STR_EQ(index->route_method, "GET");
    cbm_free_result(r);
    PASS();
}

/* Negative control: relative templates are accepted for JAX-RS @Path only.
 * A slash-less string on a non-JAX-RS mapping annotation must keep the
 * previous behaviour (the literal is not read as a route path), so the
 * relaxation cannot broaden route extraction for other frameworks. */
TEST(extract_java_spring_relative_string_not_route_path) {
    CBMFileResult *r = extract("import org.springframework.web.bind.annotation.GetMapping;\n"
                               "import org.springframework.web.bind.annotation.RequestMapping;\n"
                               "@RequestMapping(\"api\")\n"
                               "public class OrderController {\n"
                               "  @GetMapping(\"orders\")\n"
                               "  public String listOrders() { return \"\"; }\n"
                               "}\n",
                               CBM_LANG_JAVA, "t", "OrderController.java");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *list = find_def_by_name(r, "listOrders");
    ASSERT_NOT_NULL(list);
    ASSERT_NOT_NULL(list->route_path);
    ASSERT_STR_EQ(list->route_path, "/");
    ASSERT_STR_EQ(list->route_method, "GET");
    cbm_free_result(r);
    PASS();
}

/* Ported from upstream 478ce25c. */
/* Issue #1249: a mux route built as `configVar + "/literal"` (Go's idiomatic
 * configurable-base-path pattern) must index the literal suffix, both for a
 * route registration and for an outbound URL built the same way. A real BFF
 * with 47 such registrations produced only 9 Route nodes before this fix. */
TEST(extract_go_binary_concat_url_issue1249) {
    CBMFileResult *r = extract("package main\n"
                               "import \"net/http\"\n"
                               "func setup(mux *http.ServeMux, base string) {\n"
                               "    mux.HandleFunc(base+\"/login\", loginHandler)\n"
                               "}\n"
                               "func report(host string, port string) {\n"
                               "    http.Get(\"http://\" + host + \":\" + port + \"/log\")\n"
                               "}\n",
                               CBM_LANG_GO, "t", "routes.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    const CBMCall *reg = find_call_by_callee(r, "mux.HandleFunc");
    ASSERT_NOT_NULL(reg);
    ASSERT_NOT_NULL(reg->first_string_arg);
    ASSERT_STR_EQ(reg->first_string_arg, "/login");

    const CBMCall *out = find_call_by_callee(r, "http.Get");
    ASSERT_NOT_NULL(out);
    ASSERT_NOT_NULL(out->first_string_arg);
    ASSERT_STR_EQ(out->first_string_arg, "/log");

    cbm_free_result(r);
    PASS();
}

/* Same issue: when the right side of the concatenation is not itself a
 * literal (`base + suffixVar`), there is no literal route to recover. The
 * fix must leave this unresolved rather than fabricate a path. */
TEST(extract_go_binary_concat_url_no_literal_suffix_issue1249) {
    CBMFileResult *r = extract("package main\n"
                               "func setup(mux *http.ServeMux, base string, suffix string) {\n"
                               "    mux.HandleFunc(base+suffix, dynHandler)\n"
                               "}\n",
                               CBM_LANG_GO, "t", "routes.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);

    const CBMCall *reg = find_call_by_callee(r, "mux.HandleFunc");
    ASSERT_NOT_NULL(reg);
    ASSERT_NULL(reg->first_string_arg);

    cbm_free_result(r);
    PASS();
}

/* Ported from upstream cc7b4a88. */
/* Function.is_test must follow the file's test-directory membership, not just
 * a test_/_test basename convention: a helper under tests/ with none of those
 * naming conventions (tests/helpers/fixtures.c) was previously indexed as an
 * ordinary, non-test Function — invisible to store.c's `is_test != 1`
 * dead-code/search filters even though trace_path's own independent path
 * check already treated the same file as a test (#1294). */
TEST(extract_c_test_dir_marks_is_test_issue1294) {
    const char *src = "void helper(void) {}\n";

    /* Regression: a test_*-named file directly under tests/ must remain a
     * test (this already worked before the fix). */
    CBMFileResult *r1 = extract(src, CBM_LANG_C, "t", "tests/test_pipeline.c");
    ASSERT_NOT_NULL(r1);
    ASSERT_FALSE(r1->has_error);
    ASSERT(has_def(r1, "Function", "helper"));
    int reg = -1;
    for (int i = 0; i < r1->defs.count; i++) {
        if (strcmp(r1->defs.items[i].label, "Function") == 0 && r1->defs.items[i].name &&
            strcmp(r1->defs.items[i].name, "helper") == 0) {
            reg = r1->defs.items[i].is_test ? 1 : 0;
        }
    }
    ASSERT(reg == 1 && "test_*.c directly under tests/ is a test (regression)");
    cbm_free_result(r1);

    /* Positive: a file under tests/ that matches none of the test_/_test
     * naming conventions must now ALSO be a test. */
    CBMFileResult *r2 = extract(src, CBM_LANG_C, "t", "tests/helpers/fixtures.c");
    ASSERT_NOT_NULL(r2);
    ASSERT_FALSE(r2->has_error);
    ASSERT(has_def(r2, "Function", "helper"));
    int nonconv = -1;
    for (int i = 0; i < r2->defs.count; i++) {
        if (strcmp(r2->defs.items[i].label, "Function") == 0 && r2->defs.items[i].name &&
            strcmp(r2->defs.items[i].name, "helper") == 0) {
            nonconv = r2->defs.items[i].is_test ? 1 : 0;
        }
    }
    ASSERT(nonconv == 1 && "non-test_-named file under tests/ is now a test");
    cbm_free_result(r2);

    /* Negative: a file with no test/ directory or test_/_test naming in its
     * path must never be flagged — this fix must not widen detection beyond
     * the tests/ (and sibling) directory tree. */
    CBMFileResult *r3 = extract(src, CBM_LANG_C, "t", "src/pipeline/helper.c");
    ASSERT_NOT_NULL(r3);
    ASSERT_FALSE(r3->has_error);
    ASSERT(has_def(r3, "Function", "helper"));
    int outside = -1;
    for (int i = 0; i < r3->defs.count; i++) {
        if (strcmp(r3->defs.items[i].label, "Function") == 0 && r3->defs.items[i].name &&
            strcmp(r3->defs.items[i].name, "helper") == 0) {
            outside = r3->defs.items[i].is_test ? 1 : 0;
        }
    }
    ASSERT(outside == 0 && "file outside tests/ is never a test");
    cbm_free_result(r3);

    PASS();
}

/* Same convergence for Method definitions (push_method_def), which take a
 * separate code path from free functions: a class method on a class defined
 * under tests/ with no test_/_test naming (e.g. tests/helpers/base.py) must
 * be marked is_test too, and a method on the same class outside tests/ must
 * not be (#1294). */
TEST(extract_python_method_test_dir_marks_is_test_issue1294) {
    const char *src = "class Foo:\n"
                       "    def helper(self):\n"
                       "        pass\n";

    /* Python's LSP layer injects synthetic builtin stub Methods (str.upper,
     * dict.get, ...) into defs.items alongside real ones (py_builtins.c), so
     * matching must key on name, not just label="Method". */
    CBMFileResult *r1 = extract(src, CBM_LANG_PYTHON, "t", "tests/helpers/base.py");
    ASSERT_NOT_NULL(r1);
    ASSERT_FALSE(r1->has_error);
    ASSERT(has_def(r1, "Method", "helper"));
    int in_tests = -1;
    for (int i = 0; i < r1->defs.count; i++) {
        if (strcmp(r1->defs.items[i].label, "Method") == 0 && r1->defs.items[i].name &&
            strcmp(r1->defs.items[i].name, "helper") == 0) {
            in_tests = r1->defs.items[i].is_test ? 1 : 0;
        }
    }
    ASSERT(in_tests == 1 && "method on a class under tests/helpers/ is a test");
    cbm_free_result(r1);

    CBMFileResult *r2 = extract(src, CBM_LANG_PYTHON, "t", "app/models/base.py");
    ASSERT_NOT_NULL(r2);
    ASSERT_FALSE(r2->has_error);
    ASSERT(has_def(r2, "Method", "helper"));
    int outside_tests = -1;
    for (int i = 0; i < r2->defs.count; i++) {
        if (strcmp(r2->defs.items[i].label, "Method") == 0 && r2->defs.items[i].name &&
            strcmp(r2->defs.items[i].name, "helper") == 0) {
            outside_tests = r2->defs.items[i].is_test ? 1 : 0;
        }
    }
    ASSERT(outside_tests == 0 && "method on a class outside tests/ is never a test");
    cbm_free_result(r2);

    PASS();
}

/* The anonymous aggregate of a typedef is ONE definition. extract_c_typedef names
 * it from the type_definition; a second naming from the anonymous specifier gave
 * Tally two Class defs under one QN, every member twice, and a variants list. */
TEST(extract_c_anonymous_typedef_aggregate_is_one_def) {
    CBMFileResult *r = extract("typedef struct {\n"
                               "    int count;\n"
                               "} Tally;\n"
                               "typedef enum { SHADE_RED, SHADE_GREEN } Shade;\n",
                               CBM_LANG_C, "t", "tally.h");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(count_defs_named(r, "Class", "Tally"), 1);
    ASSERT_EQ(count_defs_named(r, "Field", "count"), 1);
    ASSERT_EQ(count_defs_named(r, "Enum", "Shade"), 1);
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (d->name && strcmp(d->name, "Tally") == 0) {
            ASSERT_NULL(d->variants);
        }
    }
    cbm_free_result(r);
    PASS();
}

/* ── PR C1: C graph defects behind the C/Doxygen doc-link audit ──────── */

static const CBMDefinition *c1_def(CBMFileResult *r, const char *label, const char *name) {
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (d->label && d->name && strcmp(d->label, label) == 0 && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

/* Caller QN of the first call to `callee`, or "" when there is none. */
static const char *c1_call_scope(CBMFileResult *r, const char *callee) {
    for (int i = 0; i < r->calls.count; i++) {
        const CBMCall *c = &r->calls.items[i];
        if (c->callee_name && strcmp(c->callee_name, callee) == 0) {
            return c->enclosing_func_qn ? c->enclosing_func_qn : "";
        }
    }
    return "";
}

/* Caller QN of the first C-LSP resolution whose callee QN ends in ".<leaf>". */
static const char *c1_lsp_caller(CBMFileResult *r, const char *leaf) {
    size_t ll = strlen(leaf);
    for (int i = 0; i < r->resolved_calls.count; i++) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[i];
        size_t cl = rc->callee_qn ? strlen(rc->callee_qn) : 0;
        if (cl > ll && rc->callee_qn[cl - ll - 1] == '.' &&
            strcmp(rc->callee_qn + cl - ll, leaf) == 0) {
            return rc->caller_qn ? rc->caller_qn : "";
        }
    }
    return "";
}

/* xxhash.h shape: `XXH_PUBLIC_API <RetT> name(...)` has one leading identifier more than
 * the grammar admits. tree-sitter-cpp (every .h is C++) recovers it as `RetT::name` with a
 * zero-width MISSING "::" (a Method of the return type), with a pointer return as
 * `RetT::*name` (no name at all: the function was dropped), or keeps RetT as the name and
 * puts the real one in an ERROR before the parameters. The implementation sits behind a
 * guard the preprocessed pass does not take, as in xxhash.h, so only the raw parse sees
 * it. Def QN, call-scope QN and C-LSP caller QN must all name the real function. */
static const char *C1_MACRO_PREFIX_SRC =
    "#if defined(XXH_IMPLEMENTATION)\n"
    "XXH_PUBLIC_API XXH_errorcode XXH32_freeState(XXH32_state_t* statePtr)\n"
    "{\n"
    "    XXH_free(statePtr);\n"
    "    return XXH_OK;\n"
    "}\n"
    "\n"
    "XXH_PUBLIC_API XXH32_state_t* XXH32_createState(void)\n"
    "{\n"
    "    return (XXH32_state_t*)XXH_malloc(sizeof(XXH32_state_t));\n"
    "}\n"
    "\n"
    "XXH_PUBLIC_API XXH32_hash_t XXH32 (const void* input, size_t len, XXH32_hash_t seed)\n"
    "{\n"
    "    return XXH32_endian_align(input, len, seed);\n"
    "}\n"
    "#endif\n";

TEST(extract_cpp_macro_prefixed_function_names_c1) {
    CBMFileResult *r = extract(C1_MACRO_PREFIX_SRC, CBM_LANG_CPP, "p", "xxhash.h");
    ASSERT_NOT_NULL(r);
    char want[256];

    const CBMDefinition *free_state = c1_def(r, "Function", "XXH32_freeState");
    ASSERT_NOT_NULL(free_state);
    ASSERT_EQ(count_defs_named(r, "Method", "XXH32_freeState"), 0);
    snprintf(want, sizeof(want), "%s.XXH32_freeState", r->module_qn);
    ASSERT_STR_EQ(free_state->qualified_name, want);
    ASSERT_STR_EQ(c1_call_scope(r, "XXH_free"), want);

    const CBMDefinition *create = c1_def(r, "Function", "XXH32_createState");
    ASSERT_NOT_NULL(create);
    ASSERT_EQ((int)create->start_line, 8);
    ASSERT_EQ((int)create->end_line, 11);
    ASSERT_NOT_NULL(create->signature);

    const CBMDefinition *xxh32 = c1_def(r, "Function", "XXH32");
    ASSERT_NOT_NULL(xxh32);
    ASSERT_FALSE(has_def_any(r, "XXH32_hash_t"));
    ASSERT_STR_EQ(c1_call_scope(r, "XXH32_endian_align"), xxh32->qualified_name);
    cbm_free_result(r);
    PASS();
}

/* The C-LSP walks declarators on its own (c_process_function); its caller QN must
 * name the recovered function too, or the exact-equality LSP join drops the
 * type-aware resolution of every call inside it (the call falls back to a textual
 * strategy). jemalloc-style prefix: not an export-macro candidate, so the
 * preprocessed pass keeps the same misparse and cannot mask the raw one. */
TEST(extract_cpp_macro_prefixed_lsp_caller_c1) {
    CBMFileResult *r = extract("static int je_helper(int v) { return v; }\n"
                               "JE_ALWAYS_INLINE je_errorcode je_reset(je_state_t *st)\n"
                               "{\n"
                               "    return je_helper(1);\n"
                               "}\n"
                               "JE_ALWAYS_INLINE je_hash_t je_hash (const void *in, size_t n)\n"
                               "{\n"
                               "    return je_helper(2);\n"
                               "}\n",
                               CBM_LANG_CPP, "p", "je.h");
    ASSERT_NOT_NULL(r);
    const CBMDefinition *reset = c1_def(r, "Function", "je_reset");
    const CBMDefinition *hash = c1_def(r, "Function", "je_hash");
    ASSERT_NOT_NULL(reset);
    ASSERT_NOT_NULL(hash);
    int from_reset = 0;
    int from_hash = 0;
    for (int i = 0; i < r->resolved_calls.count; i++) {
        const char *caller = r->resolved_calls.items[i].caller_qn;
        from_reset += caller && strcmp(caller, reset->qualified_name) == 0;
        from_hash += caller && strcmp(caller, hash->qualified_name) == 0;
    }
    ASSERT_GT(from_reset, 0);
    ASSERT_GT(from_hash, 0);
    ASSERT_STR_EQ(c1_lsp_caller(r, "je_helper"), reset->qualified_name);
    cbm_free_result(r);
    PASS();
}

/* The same recovery in the C grammar (lua's LUA_API): named after the return type
 * before. A function-like macro invocation that only looks like a declarator
 * (`... TEST_BEGIN (test_alignment) {`) has an untyped "parameter" no definition can
 * have, so its macro name is never adopted as the function name. */
TEST(extract_c_macro_prefixed_function_name_c1) {
    CBMFileResult *r =
        extract("#if defined(LUA_CORE)\n"
                "LUA_API lua_CFunction lua_atpanic (lua_State *L, lua_CFunction panicf) {\n"
                "  lua_CFunction old = G(L)->panic;\n"
                "  return old;\n"
                "}\n"
                "\n"
                "XXH_TEST_API XXH32_hash_t TEST_BEGIN (test_alignment) {\n"
                "  helper(1);\n"
                "}\n"
                "#endif\n",
                CBM_LANG_C, "p", "lapi.c");
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(c1_def(r, "Function", "lua_atpanic"));
    ASSERT_FALSE(has_def_any(r, "lua_CFunction"));
    ASSERT_FALSE(has_def_any(r, "TEST_BEGIN"));
    cbm_free_result(r);
    PASS();
}

/* `struct X` / `enum X` / `class X` without a body is a reference, not a definition:
 * no Class/Enum def, so nothing competes with the real definition for its QN (a later
 * or smaller-path reference used to take the node: curl 132 structs, kernel
 * task_struct, redis redisServer) and no phantom type node appears in every file that
 * merely mentions the type (`struct timeval`). */
TEST(extract_c_tag_reference_is_not_a_definition_c1) {
    CBMFileResult *r = extract("struct node {\n"
                               "    int value;\n"
                               "    struct node *next;\n"
                               "};\n"
                               "struct node *head;\n"
                               "struct timeval tv;\n"
                               "enum color { RED, GREEN };\n"
                               "enum color paint(enum color c);\n"
                               "void walk(struct node *n);\n"
                               "struct opaque;\n",
                               CBM_LANG_C, "p", "list.c");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(count_defs_named(r, "Class", "node"), 1);
    const CBMDefinition *node = c1_def(r, "Class", "node");
    ASSERT_EQ((int)node->start_line, 1);
    ASSERT_EQ((int)node->end_line, 4);
    ASSERT_EQ(count_defs_named(r, "Enum", "color"), 1);
    ASSERT_EQ((int)c1_def(r, "Enum", "color")->start_line, 7);
    ASSERT_FALSE(has_def_any(r, "timeval"));
    ASSERT_FALSE(has_def_any(r, "opaque"));
    cbm_free_result(r);

    /* .h is parsed as C++: a forward declaration before the definition. */
    r = extract("class Widget;\n"
                "struct Config;\n"
                "Widget *make_widget(struct Config *cfg);\n"
                "class Widget {\n"
                "    int size;\n"
                "};\n",
                CBM_LANG_CPP, "p", "widget.h");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(count_defs_named(r, "Class", "Widget"), 1);
    ASSERT_EQ((int)c1_def(r, "Class", "Widget")->start_line, 4);
    ASSERT_FALSE(has_def_any(r, "Config"));
    cbm_free_result(r);
    PASS();
}

/* tree-sitter can give up on a whole region and leave the definition head as loose
 * ERROR tokens (kernel include/linux/sched.h: `struct task_struct {` at 826 is never a
 * struct_specifier). Its only nodes were then the references (642 won); with
 * references no longer minting defs the head itself must be recovered, spanning to
 * its matching brace counted over the first branch of each #if group. */
TEST(extract_c_struct_head_in_error_region_recovered_c1) {
    CBMFileResult *r = extract("#ifndef SCHED_H\n"
                               "#define SCHED_H\n"
                               "struct task_struct;\n"
                               "typedef struct task_struct *(*pick_f)(int cpu);\n"
                               "\n"
                               "/* Per-task scheduler state. */\n"
                               "struct task_struct {\n"
                               "\tint state;\n"
                               "#ifdef CONFIG_SMP\n"
                               "\tstruct {\n"
                               "#else\n"
                               "\tunion {\n"
                               "#endif\n"
                               "\t\tint on_cpu;\n"
                               "\t};\n"
                               "\tint prio;\n"
                               "};\n"
                               "#endif\n",
                               CBM_LANG_CPP, "p", "sched.h");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(count_defs_named(r, "Class", "task_struct"), 1);
    const CBMDefinition *ts = c1_def(r, "Class", "task_struct");
    ASSERT_EQ((int)ts->start_line, 7);
    ASSERT_EQ((int)ts->end_line, 17);
    ASSERT_NOT_NULL(c1_def(r, "Type", "pick_f"));
    cbm_free_result(r);
    PASS();
}

/* A stray `;` after an if-block leaves the following `else if (...) {...}` without its
 * `if`; inside a function the raw parse already lost (whole-file guard + split brace),
 * error recovery reads it as a DEFINITION: type `else`, name `if`. A C keyword is never
 * a function name (curl lib/vtls/openssl.c:4242, redis deps/tre/lib/tre-parse.c:1315). */
TEST(extract_c_keyword_is_never_a_function_name_c1) {
    CBMFileResult *r = extract("#ifdef USE_OPENSSL\n"
                               "static int check(int lib, int reason)\n"
                               "{\n"
                               "  int result = 0;\n"
                               "  if(lib == 1) {\n"
                               "#ifndef HAVE_BORINGSSL_LIKE\n"
                               "    result = 1; {\n"
                               "#else\n"
                               "    result = 2; {\n"
                               "#endif\n"
                               "  }\n"
                               "  };\n"
                               "  else if(reason == 2) {\n"
                               "    result = trace_retry(reason);\n"
                               "  }\n"
                               "  return result;\n"
                               "}\n"
                               "#endif\n",
                               CBM_LANG_C, "p", "openssl.c");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(has_def_any(r, "if"));
    ASSERT_TRUE(has_call(r, "trace_retry"));
    cbm_free_result(r);
    PASS();
}

/* Preprocessor-split code the raw parse cannot read: `#ifndef X (a)) { #else (b)) {
 * #endif` leaves an extra `{`, the file-level ERROR swallows the function header and
 * the function gets no node. The #961 rescue re-parses the preprocessed text, but a
 * whole-file `#ifdef USE_OPENSSL` with no build defines leaves it nothing to parse.
 * The first-branch projection restores it (curl ossl_connect_step2,
 * Curl_ossl_ctx_init, mbed_configure_ssl, ...: all 11 curl misses). */
TEST(extract_c_function_lost_to_split_branches_restored_c1) {
    CBMFileResult *r = extract("#ifdef USE_OPENSSL\n"
                               "static int ossl_step(int lib, int reason)\n"
                               "{\n"
                               "  int result = 0;\n"
                               "  if(lib == 1) {\n"
                               "    result = 1;\n"
                               "  }\n"
                               "  else if((lib == 2) &&\n"
                               "#ifndef HAVE_BORINGSSL_LIKE\n"
                               "          (reason == 3)) {\n"
                               "#else\n"
                               "          (reason == 4)) {\n"
                               "#endif\n"
                               "    result = trace_retry(reason);\n"
                               "  }\n"
                               "  return result;\n"
                               "}\n"
                               "\n"
                               "int ossl_after(void)\n"
                               "{\n"
                               "  return ossl_step(1, 2);\n"
                               "}\n"
                               "#endif\n",
                               CBM_LANG_C, "p", "openssl.c");
    ASSERT_NOT_NULL(r);
    const CBMDefinition *step = c1_def(r, "Function", "ossl_step");
    ASSERT_NOT_NULL(step);
    ASSERT_EQ((int)step->start_line, 2);
    ASSERT_EQ((int)step->end_line, 17);
    ASSERT_EQ(count_defs_named(r, "Function", "ossl_after"), 1);
    cbm_free_result(r);
    PASS();
}

/* C typedefs carry no `name` field (the alias lives in the declarators), so no typedef
 * ever became a node and an anonymous `typedef struct {...} Name;` lost its fields and
 * enum constants too. xxhash: `@ref XXH32_state_t` could only resolve to the namespace
 * rename macro `#define XXH32_state_t XXH_IPREF(XXH32_state_t)`. */
TEST(extract_c_typedef_names_are_definitions_c1) {
    CBMFileResult *r = extract("typedef struct XXH32_state_s XXH32_state_t;\n"
                               "typedef struct {\n"
                               "    unsigned char digest[4];\n"
                               "} XXH32_canonical_t;\n"
                               "typedef enum { XXH_OK = 0, XXH_ERROR = 1 } XXH_errorcode;\n"
                               "typedef unsigned int XXH32_hash_t;\n"
                               "typedef int (*cmp_fn)(const void *a, const void *b);\n"
                               "typedef struct opaque_s opaque_s;\n"
                               "typedef struct node node;\n"
                               "struct node { int v; };\n"
                               "typedef struct named_s { int y; } named_t;\n"
                               "typedef struct same { int z; } same;\n",
                               CBM_LANG_C, "p", "xxhash.c");
    ASSERT_NOT_NULL(r);
    char want[256];
    ASSERT_NOT_NULL(c1_def(r, "Type", "XXH32_state_t"));
    const CBMDefinition *canon = c1_def(r, "Class", "XXH32_canonical_t");
    ASSERT_NOT_NULL(canon);
    ASSERT_EQ((int)canon->start_line, 2);
    snprintf(want, sizeof(want), "%s.XXH32_canonical_t.digest", r->module_qn);
    ASSERT_TRUE(has_def_qn(r, want));
    ASSERT_NOT_NULL(c1_def(r, "Enum", "XXH_errorcode"));
    snprintf(want, sizeof(want), "%s.XXH_OK", r->module_qn);
    ASSERT_TRUE(has_def_qn(r, want));
    ASSERT_NOT_NULL(c1_def(r, "Type", "XXH32_hash_t"));
    ASSERT_NOT_NULL(c1_def(r, "Type", "cmp_fn"));
    /* identity alias of an incomplete struct: often the only declaration of an
     * opaque handle type */
    ASSERT_NOT_NULL(c1_def(r, "Type", "opaque_s"));
    /* identity alias of a struct defined in the same file: the definition owns it */
    ASSERT_EQ(count_defs_named(r, "Class", "node"), 1);
    ASSERT_EQ(count_defs_named(r, "Type", "node"), 0);
    ASSERT_NOT_NULL(c1_def(r, "Class", "named_s"));
    ASSERT_NOT_NULL(c1_def(r, "Type", "named_t"));
    ASSERT_EQ(count_defs_named(r, "Class", "same"), 1);
    ASSERT_EQ(count_defs_named(r, "Type", "same"), 0);
    cbm_free_result(r);
    PASS();
}

static const CBMDefinition *c1_def_qn(CBMFileResult *r, const char *qn) {
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (d->qualified_name && strcmp(d->qualified_name, qn) == 0) {
            return d;
        }
    }
    return NULL;
}

/* The def with this label and name that starts on `line`. */
static const CBMDefinition *c1_def_at(CBMFileResult *r, const char *label, const char *name,
                                      int line) {
    for (int i = 0; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (d->label && d->name && strcmp(d->label, label) == 0 && strcmp(d->name, name) == 0 &&
            (int)d->start_line == line) {
            return d;
        }
    }
    return NULL;
}

/* A preprocessor macro's QN is `<module>.<NAME>#macro` (CBM_MACRO_QN_SUFFIX); name and
 * label stay. C keeps macros apart from ordinary identifiers, so a macro and the
 * function, type or enumerator of the same name are two entities and need two QNs: the
 * namespace rename `#define XXH32 XXH_NAME2(XXH_NAMESPACE, XXH32)` held the QN of the
 * function XXH32 (110 of 174 audited xxhash doc links ended on such a macro), and the
 * #else stand-in `#define match_proxy(a) 1` replaced the function it stands in for
 * (curl, 41 functions). Every language routed through the #define path is covered,
 * and in each of them a macro defined twice carries `variants`: the namespace and the
 * list follow one language predicate. */
TEST(extract_c_macro_qn_is_fenced_c1) {
    static const struct {
        CBMLanguage lang;
        const char *file;
    } cases[] = {{CBM_LANG_C, "cfg.c"},       {CBM_LANG_CPP, "cfg.h"},
                 {CBM_LANG_OBJC, "cfg.m"},    {CBM_LANG_CUDA, "cfg.cu"},
                 {CBM_LANG_GLSL, "cfg.glsl"}, {CBM_LANG_ISPC, "cfg.ispc"}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CBMFileResult *r = extract("#define MAX_LEN 10\n"
                                   "#define SQUARE(x) ((x) * (x))\n"
                                   "#define MAX_LEN 20\n",
                                   cases[i].lang, "p", cases[i].file);
        ASSERT_NOT_NULL(r);
        char want[256];
        const CBMDefinition *max_len = c1_def(r, "Macro", "MAX_LEN");
        ASSERT_NOT_NULL(max_len);
        snprintf(want, sizeof(want), "%s.MAX_LEN" CBM_MACRO_QN_SUFFIX, r->module_qn);
        ASSERT_STR_EQ(max_len->qualified_name, want);
        const CBMDefinition *square = c1_def(r, "Macro", "SQUARE");
        ASSERT_NOT_NULL(square);
        snprintf(want, sizeof(want), "%s.SQUARE#macro", r->module_qn);
        ASSERT_STR_EQ(square->qualified_name, want);
        /* the plain QN is free for a definition of that name */
        snprintf(want, sizeof(want), "%s.MAX_LEN", r->module_qn);
        ASSERT_NULL(c1_def_qn(r, want));
        const CBMDefinition *redefined = c1_def_at(r, "Macro", "MAX_LEN", 3);
        ASSERT_NOT_NULL(redefined);
        ASSERT_NOT_NULL(redefined->variants);
        cbm_free_result(r);
    }

    CBMFileResult *r = extract("#ifdef USE_PROXY\n"
                               "int match_proxy(int a) { return a; }\n"
                               "#else\n"
                               "#define match_proxy(a) 1\n"
                               "#endif\n",
                               CBM_LANG_C, "p", "proxy.c");
    ASSERT_NOT_NULL(r);
    const CBMDefinition *fn = c1_def(r, "Function", "match_proxy");
    const CBMDefinition *mac = c1_def(r, "Macro", "match_proxy");
    ASSERT_NOT_NULL(fn);
    ASSERT_NOT_NULL(mac);
    char want[256];
    snprintf(want, sizeof(want), "%s.match_proxy", r->module_qn);
    ASSERT_STR_EQ(fn->qualified_name, want);
    snprintf(want, sizeof(want), "%s.match_proxy#macro", r->module_qn);
    ASSERT_STR_EQ(mac->qualified_name, want);
    cbm_free_result(r);
    PASS();
}

/* The enumerators of a C enum live in the scope that holds the enum, so their QN is
 * `<scope>.<CONST>`: the enum's own name is not a segment. That is how a reference
 * spells them (`XXH_OK`, never `XXH_errorcode.XXH_OK`), and it gives the constants of
 * an anonymous enum, which has no name to nest under, a QN at all. `parent_class`
 * (the enum's QN) keeps the membership; an anonymous enum has no node to point at. */
TEST(extract_c_enumerators_are_flat_c1) {
    CBMFileResult *r = extract("enum color { RED, GREEN = 2 };\n"
                               "enum { ANON_A, ANON_B };\n"
                               "typedef enum { TD_A, TD_B } td_t;\n",
                               CBM_LANG_C, "p", "e.c");
    ASSERT_NOT_NULL(r);
    char qn[256];
    char parent[256];

    snprintf(qn, sizeof(qn), "%s.RED", r->module_qn);
    snprintf(parent, sizeof(parent), "%s.color", r->module_qn);
    const CBMDefinition *red = c1_def_qn(r, qn);
    ASSERT_NOT_NULL(red);
    ASSERT_STR_EQ(red->label, "Variable");
    ASSERT_NOT_NULL(red->parent_class);
    ASSERT_STR_EQ(red->parent_class, parent);
    snprintf(qn, sizeof(qn), "%s.GREEN", r->module_qn);
    ASSERT_NOT_NULL(c1_def_qn(r, qn));
    snprintf(qn, sizeof(qn), "%s.color.RED", r->module_qn);
    ASSERT_NULL(c1_def_qn(r, qn));
    ASSERT_NOT_NULL(c1_def_qn(r, parent)); /* the Enum itself keeps its QN */

    snprintf(qn, sizeof(qn), "%s.ANON_A", r->module_qn);
    const CBMDefinition *anon = c1_def_qn(r, qn);
    ASSERT_NOT_NULL(anon);
    ASSERT_STR_EQ(anon->label, "Variable");
    ASSERT_NULL(anon->parent_class);
    snprintf(qn, sizeof(qn), "%s.ANON_B", r->module_qn);
    ASSERT_NOT_NULL(c1_def_qn(r, qn));

    snprintf(qn, sizeof(qn), "%s.TD_A", r->module_qn);
    snprintf(parent, sizeof(parent), "%s.td_t", r->module_qn);
    const CBMDefinition *td = c1_def_qn(r, qn);
    ASSERT_NOT_NULL(td);
    ASSERT_NOT_NULL(td->parent_class);
    ASSERT_STR_EQ(td->parent_class, parent);
    ASSERT_EQ(count_defs_named(r, "Variable", "TD_A"), 1);
    cbm_free_result(r);
    PASS();
}

/* C++: an unscoped enum's enumerators belong to the enclosing scope (module, namespace
 * or class), whatever nesting holds the enum; `enum class` and `enum struct` are
 * scopes of their own and keep `<Enum>.<CONST>`. */
TEST(extract_cpp_enum_scoping_c1) {
    CBMFileResult *r = extract("enum Color { RED, GREEN };\n"
                               "namespace gfx {\n"
                               "enum Blend { ADD, MULTIPLY };\n"
                               "class Brush {\n"
                               "public:\n"
                               "    enum Shape { ROUND, SQUARE };\n"
                               "};\n"
                               "}\n"
                               "enum class Mode { FAST, SLOW };\n"
                               "enum struct Level { LOW, HIGH };\n",
                               CBM_LANG_CPP, "p", "shapes.hpp");
    ASSERT_NOT_NULL(r);
    static const struct {
        const char *qn_tail;
        const char *parent_tail;
    } want[] = {{"RED", "Color"},
                {"GREEN", "Color"},
                {"gfx.ADD", "gfx.Blend"},
                {"gfx.MULTIPLY", "gfx.Blend"},
                {"gfx.Brush.ROUND", "gfx.Brush.Shape"},
                {"gfx.Brush.SQUARE", "gfx.Brush.Shape"},
                {"Mode.FAST", "Mode"},
                {"Mode.SLOW", "Mode"},
                {"Level.LOW", "Level"},
                {"Level.HIGH", "Level"}};
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        char qn[256];
        char parent[256];
        snprintf(qn, sizeof(qn), "%s.%s", r->module_qn, want[i].qn_tail);
        snprintf(parent, sizeof(parent), "%s.%s", r->module_qn, want[i].parent_tail);
        const CBMDefinition *d = c1_def_qn(r, qn);
        if (!d) {
            fprintf(stderr, "  [c1] no def with QN %s\n", qn);
        }
        ASSERT_NOT_NULL(d);
        ASSERT_STR_EQ(d->label, "Variable");
        ASSERT_NOT_NULL(d->parent_class);
        ASSERT_STR_EQ(d->parent_class, parent);
    }
    char qn[256];
    snprintf(qn, sizeof(qn), "%s.Color.RED", r->module_qn);
    ASSERT_NULL(c1_def_qn(r, qn));
    snprintf(qn, sizeof(qn), "%s.FAST", r->module_qn);
    ASSERT_NULL(c1_def_qn(r, qn));
    cbm_free_result(r);
    PASS();
}

/* A named and an anonymous enum inside a struct, and an anonymous one inside a union:
 * one C-looking source, read under each C-family language by the two tests below. */
static const char *const C1_ENUM_IN_STRUCT_SRC = "struct conn {\n"
                                                 "    enum state { ST_IDLE, ST_BUSY } st;\n"
                                                 "    enum { KIND_A, KIND_B } kind;\n"
                                                 "    int fd;\n"
                                                 "};\n"
                                                 "union box {\n"
                                                 "    enum { BOX_A, BOX_B } tag;\n"
                                                 "    int v;\n"
                                                 "};\n";

/* Is `<module>[.<scope>].<name>` the ONE Variable def of that name, with parent_class
 * `<module>.<parent_tail>` (NULL: no parent_class)? */
static bool c1_enumerator_at(CBMFileResult *r, const char *scope, const char *name,
                             const char *parent_tail) {
    char qn[256];
    snprintf(qn, sizeof(qn), "%s%s%s.%s", r->module_qn, scope[0] ? "." : "", scope, name);
    const CBMDefinition *d = c1_def_qn(r, qn);
    if (!d || !d->label || strcmp(d->label, "Variable") != 0 ||
        count_defs_named(r, "Variable", name) != 1) {
        const CBMDefinition *other = c1_def(r, "Variable", name);
        fprintf(stderr, "  [c1] want the one Variable %s, the Variable of that name is %s\n", qn,
                other ? other->qualified_name : "(none)");
        return false;
    }
    if (!parent_tail) {
        return d->parent_class == NULL;
    }
    char parent[256];
    snprintf(parent, sizeof(parent), "%s.%s", r->module_qn, parent_tail);
    return d->parent_class && strcmp(d->parent_class, parent) == 0;
}

/* C has no struct scope for ordinary identifiers: an enum declared inside a struct or
 * union puts its enumerators in the scope around it, and the code names them
 * unqualified (curl lib/cf-h1-proxy.c: `ts->keepon = KEEPON_CONNECT;`). In C and
 * Objective-C the struct is therefore no segment of a flat enumerator's QN, named
 * enum or anonymous; `parent_class` still names a named enum. */
TEST(extract_c_enum_inside_struct_is_file_scope_c1) {
    static const struct {
        CBMLanguage lang;
        const char *file;
    } cases[] = {{CBM_LANG_C, "link.c"}, {CBM_LANG_OBJC, "link.m"}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CBMFileResult *r = extract(C1_ENUM_IN_STRUCT_SRC, cases[i].lang, "p", cases[i].file);
        ASSERT_NOT_NULL(r);
        ASSERT_TRUE(c1_enumerator_at(r, "", "ST_IDLE", "conn.state"));
        ASSERT_TRUE(c1_enumerator_at(r, "", "ST_BUSY", "conn.state"));
        ASSERT_TRUE(c1_enumerator_at(r, "", "KIND_A", NULL));
        ASSERT_TRUE(c1_enumerator_at(r, "", "KIND_B", NULL));
        ASSERT_TRUE(c1_enumerator_at(r, "", "BOX_A", NULL));
        ASSERT_TRUE(c1_enumerator_at(r, "", "BOX_B", NULL));
        cbm_free_result(r);
    }
    PASS();
}

/* C++ does scope them: `conn::ST_IDLE`. The enclosing class stays the scope in C++ and
 * CUDA -- and so in every .h, which is parsed as C++ -- for the same source. This pins
 * the other side of the rule above: the C treatment must not reach C++. */
TEST(extract_cpp_enum_inside_class_keeps_class_scope_c1) {
    static const struct {
        CBMLanguage lang;
        const char *file;
    } cases[] = {{CBM_LANG_CPP, "link.h"}, {CBM_LANG_CUDA, "link.cu"}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CBMFileResult *r = extract(C1_ENUM_IN_STRUCT_SRC, cases[i].lang, "p", cases[i].file);
        ASSERT_NOT_NULL(r);
        ASSERT_TRUE(c1_enumerator_at(r, "conn", "ST_IDLE", "conn.state"));
        ASSERT_TRUE(c1_enumerator_at(r, "conn", "ST_BUSY", "conn.state"));
        ASSERT_TRUE(c1_enumerator_at(r, "conn", "KIND_A", NULL));
        ASSERT_TRUE(c1_enumerator_at(r, "conn", "KIND_B", NULL));
        ASSERT_TRUE(c1_enumerator_at(r, "box", "BOX_A", NULL));
        ASSERT_TRUE(c1_enumerator_at(r, "box", "BOX_B", NULL));
        cbm_free_result(r);
    }
    PASS();
}

/* A macro inside an enumerator list is not enumerator grammar; the parser keeps every
 * bare identifier it can as an `enumerator` and wraps the rest in ERROR nodes. curl.h
 * declares its options as `CURLOPT(CURLOPT_URL, CURLOPTTYPE_STRINGPOINT, 2),`: the
 * second argument came back as a constant 115 times and, flattened, took the plain QN
 * of the macro `CURLOPTTYPE_LONG`. `X CURL_DEPRECATED(7.55.0, "Use Y") = BASE + 16,`
 * turned BASE and the words of the message into constants. Only the enumerator that
 * opens a slot is a constant; the comment above the macro call documents it. */
TEST(extract_c_enum_macro_wrapped_list_c1) {
    CBMFileResult *r = extract("typedef enum {\n"
                               "  /* doc of the first option */\n"
                               "  OPT(OPT_FIRST, TYPE_LONG, 1),\n"
                               "  OPT(OPT_SECOND, TYPE_STRING, 2),\n"
                               "  OPT_THIRD DEPRECATED(1.0, \"Use OPT_FIRST instead\")\n"
                               "      = BASE_LONG + 3,\n"
                               "  OPT_LAST\n"
                               "} option_t;\n",
                               CBM_LANG_C, "p", "opts.c");
    ASSERT_NOT_NULL(r);
    static const char *const real[] = {"OPT_FIRST", "OPT_SECOND", "OPT_THIRD", "OPT_LAST"};
    for (size_t i = 0; i < sizeof(real) / sizeof(real[0]); i++) {
        ASSERT_EQ(count_defs_named(r, "Variable", real[i]), 1);
    }
    static const char *const bogus[] = {"TYPE_LONG", "TYPE_STRING", "BASE_LONG",
                                        "Use",       "instead",     "OPT"};
    for (size_t i = 0; i < sizeof(bogus) / sizeof(bogus[0]); i++) {
        ASSERT_EQ(count_defs_named(r, "Variable", bogus[i]), 0);
    }
    /* Upstream also asserts OPT_FIRST's docstring; this fork attaches no
     * docstrings to enum members. */
    cbm_free_result(r);
    PASS();
}

/* One file may define a QN several times under one label: both branches of an #if, a
 * macro redefined per platform. The graph keeps one node per QN (the last by start
 * line), and before this the other definitions left no trace (curl 123 groups, redis
 * 87). The definition the graph keeps carries `variants`: every span of the group,
 * the kept one included, sorted by start line. A name defined once carries nothing. */
TEST(extract_c_variants_lists_same_file_duplicates_c1) {
    CBMFileResult *r = extract("#if A\n"
                               "int pick(int a) { return a; }\n"
                               "#else\n"
                               "int pick(int a) {\n"
                               "    return a + 1;\n"
                               "}\n"
                               "#endif\n"
                               "#ifdef B\n"
                               "#define LIMIT 1\n"
                               "#else\n"
                               "#define LIMIT 2\n"
                               "#endif\n"
                               "int once(void) { return 0; }\n",
                               CBM_LANG_C, "p", "v.c");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(count_defs_named(r, "Function", "pick"), 2);
    const CBMDefinition *kept = c1_def_at(r, "Function", "pick", 4);
    const CBMDefinition *other = c1_def_at(r, "Function", "pick", 2);
    ASSERT_NOT_NULL(kept);
    ASSERT_NOT_NULL(other);
    ASSERT_NOT_NULL(kept->variants);
    ASSERT_STR_EQ(kept->variants, "[{\"file_path\":\"v.c\",\"start_line\":2,\"end_line\":2},"
                                  "{\"file_path\":\"v.c\",\"start_line\":4,\"end_line\":6}]");
    ASSERT_NULL(other->variants); /* one carrier per group: the node the graph keeps */

    const CBMDefinition *lim_a = c1_def_at(r, "Macro", "LIMIT", 9);
    const CBMDefinition *lim_b = c1_def_at(r, "Macro", "LIMIT", 11);
    ASSERT_NOT_NULL(lim_a);
    ASSERT_NOT_NULL(lim_b);
    char want[256];
    snprintf(want, sizeof(want),
             "[{\"file_path\":\"v.c\",\"start_line\":%u,\"end_line\":%u},"
             "{\"file_path\":\"v.c\",\"start_line\":%u,\"end_line\":%u}]",
             lim_a->start_line, lim_a->end_line, lim_b->start_line, lim_b->end_line);
    ASSERT_NOT_NULL(lim_b->variants);
    ASSERT_STR_EQ(lim_b->variants, want);
    ASSERT_NULL(lim_a->variants);

    ASSERT_NULL(c1_def(r, "Function", "once")->variants);
    cbm_free_result(r);
    PASS();
}

/* `variants` describes conditional compilation, so only the C-preprocessor languages
 * get it (cbm_is_c_preprocessor_lang, the six of the test above). A repeated QN
 * elsewhere is another matter: a JSON file repeats one key name thousands of times
 * (redis src/commands: 1,618 nodes, one of them with a 75-span list), a Python module
 * may rebind a function. Both fixtures DO repeat a (QN, label) on DIFFERENT lines
 * (two defs on one span count as one and would get no list anyway), which is asserted
 * so the guard cannot go inert. */
TEST(extract_variants_only_in_c_preprocessor_languages_c1) {
    static const struct {
        CBMLanguage lang;
        const char *file;
        const char *src;
    } cases[] = {
        {CBM_LANG_JSON, "cmd.json", "{\n  \"a\": {\"name\": 1},\n  \"b\": {\"name\": 2}\n}\n"},
        {CBM_LANG_PYTHON, "mod.py",
         "def pick(a):\n    return a\n\n\ndef pick(a):\n    return a + 1\n"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CBMFileResult *r = extract(cases[i].src, cases[i].lang, "p", cases[i].file);
        ASSERT_NOT_NULL(r);
        int listed = 0;
        bool distinct_lines = false;
        for (int d = 0; d < r->defs.count; d++) {
            const CBMDefinition *a = &r->defs.items[d];
            listed += a->variants != NULL;
            for (int e = 0; a->qualified_name && a->label && e < d; e++) {
                const CBMDefinition *b = &r->defs.items[e];
                distinct_lines =
                    distinct_lines ||
                    (b->qualified_name && b->label &&
                     strcmp(a->qualified_name, b->qualified_name) == 0 &&
                     strcmp(a->label, b->label) == 0 && a->start_line != b->start_line);
            }
        }
        if (listed != 0 || !distinct_lines) {
            fprintf(stderr,
                    "  [c1-guard] %s: defs with variants=%d, repeated on distinct lines=%d\n",
                    cases[i].file, listed, distinct_lines);
        }
        ASSERT_TRUE(distinct_lines);
        ASSERT_EQ(listed, 0);
        cbm_free_result(r);
    }
    PASS();
}

/* #1916: `axios.create(...)` extraction and module default export client propagation */
TEST(extract_axios_create_client_and_module_default_export_issue1916) {
    CBMFileResult *r = extract("import axios from 'axios';\n"
                               "const api = axios.create({ baseURL: '/api' });\n"
                               "export default api;\n",
                               CBM_LANG_TYPESCRIPT, "t", "client.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *var_api = NULL;
    const CBMDefinition *mod_def = NULL;
    for (int i = 0; i < r->defs.count; i++) {
        if (r->defs.items[i].name && strcmp(r->defs.items[i].name, "api") == 0) {
            var_api = &r->defs.items[i];
        }
        if (r->defs.items[i].label && strcmp(r->defs.items[i].label, "Module") == 0) {
            mod_def = &r->defs.items[i];
        }
    }
    ASSERT_NOT_NULL(var_api);
    ASSERT_NOT_NULL(var_api->http_client);
    ASSERT_STR_EQ(var_api->http_client, "axios");
    ASSERT_NOT_NULL(var_api->http_base_url);
    ASSERT_STR_EQ(var_api->http_base_url, "/api");

    ASSERT_NOT_NULL(mod_def);
    ASSERT_NOT_NULL(mod_def->http_client);
    ASSERT_STR_EQ(mod_def->http_client, "axios");
    ASSERT_NOT_NULL(mod_def->http_base_url);
    ASSERT_STR_EQ(mod_def->http_base_url, "/api");
    cbm_free_result(r);
    PASS();
}

TEST(extract_es_default_import_flag_issue1916) {
    CBMFileResult *r = extract("import api from './client';\n"
                               "import { helper } from './util';\n",
                               CBM_LANG_TYPESCRIPT, "t", "caller.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMImport *imp_default = NULL;
    const CBMImport *imp_named = NULL;
    for (int i = 0; i < r->imports.count; i++) {
        if (r->imports.items[i].local_name && strcmp(r->imports.items[i].local_name, "api") == 0) {
            imp_default = &r->imports.items[i];
        }
        if (r->imports.items[i].local_name && strcmp(r->imports.items[i].local_name, "helper") == 0) {
            imp_named = &r->imports.items[i];
        }
    }
    ASSERT_NOT_NULL(imp_default);
    ASSERT_TRUE(imp_default->is_default);
    ASSERT_NOT_NULL(imp_named);
    ASSERT_FALSE(imp_named->is_default);
    cbm_free_result(r);
    PASS();
}

TEST(extract_lookalike_create_is_not_client_issue1916) {
    CBMFileResult *r = extract("const factory = { create: (cfg) => cfg };\n"
                               "const fakeApi = factory.create({ baseURL: '/api' });\n"
                               "export default fakeApi;\n",
                               CBM_LANG_TYPESCRIPT, "t", "fake.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    const CBMDefinition *var_fake = NULL;
    const CBMDefinition *mod_def = NULL;
    for (int i = 0; i < r->defs.count; i++) {
        if (r->defs.items[i].name && strcmp(r->defs.items[i].name, "fakeApi") == 0) {
            var_fake = &r->defs.items[i];
        }
        if (r->defs.items[i].label && strcmp(r->defs.items[i].label, "Module") == 0) {
            mod_def = &r->defs.items[i];
        }
    }
    ASSERT_NOT_NULL(var_fake);
    ASSERT_NULL(var_fake->http_client);
    ASSERT_NULL(var_fake->http_base_url);
    ASSERT_NOT_NULL(mod_def);
    ASSERT_NULL(mod_def->http_client);
    ASSERT_NULL(mod_def->http_base_url);
    cbm_free_result(r);
    PASS();
}

SUITE(extraction) {
    /* Initialize extraction library */
    cbm_init();

    /* Wide-flat-file linearity (ms-typescript hang) */
    RUN_TEST(extract_wide_flat_file_is_linear);

    /* Perl call-graph noise (#459 follow-up) */
    RUN_TEST_LANG(CBM_LANG_PERL, extract_perl_config_string_not_a_callee);
    RUN_TEST_LANG(CBM_LANG_PERL, extract_perl_builtin_call_is_function_not_method);
    RUN_TEST_LANG(CBM_LANG_PERL, extract_perl_method_call_flags_is_method);
    RUN_TEST(extract_flag_exempt_method_call_not_flagged_is_method);
    RUN_TEST(extract_python_member_call_flags_is_method);
    RUN_TEST(extract_python_bare_call_flags_locally_bound_callee);
    RUN_TEST(extract_python_bare_call_flag_is_depth_independent);
    RUN_TEST(extract_ts_member_call_flags_is_method);
    RUN_TEST(extract_ts_this_super_receiver_not_flagged);
    RUN_TEST(extract_js_member_call_flags_is_method);

    /* R box-module imports + member calls */
    RUN_TEST(extract_parse_incomplete_reports_error_ranges);
    RUN_TEST(extract_parse_budget_survives_wall_stall);
    RUN_TEST(extract_parse_budget_still_trips_on_cpu);
    RUN_TEST_LANG(CBM_LANG_R, extract_r_box_use_imports_issue218);
    RUN_TEST_LANG(CBM_LANG_R, extract_r_dollar_call_issue219);
    RUN_TEST(extract_ts_factory_object_methods_issue341);
    RUN_TEST(traversal_stack_not_in_result_arena_issue2010);
    RUN_TEST(extract_c_macro_hidden_call_survives_preprocessed_pass_issue2010);
    RUN_TEST(extract_c_macros_issue375);
    RUN_TEST(extract_cpp_macros_issue375);
    RUN_TEST_LANG(CBM_LANG_GDSCRIPT, extract_gdscript_issue186);
    RUN_TEST(extract_powershell_issue35);
    RUN_TEST_LANG(CBM_LANG_LUAU, extract_luau_issue39);
    RUN_TEST_LANG(CBM_LANG_QML, extract_qml_issue42);
    RUN_TEST_LANG(CBM_LANG_CFSCRIPT, extract_cfscript_issue38);
    RUN_TEST_LANG(CBM_LANG_CFML, extract_cfml_tag_issue38);
    RUN_TEST_LANG(CBM_LANG_GOTEMPLATE, extract_helm_templates_issue338);
    RUN_TEST(extract_helm_values_toplevel_issue338);

    /* OOP */
    RUN_TEST(java_class);
    RUN_TEST(java_method);
    RUN_TEST(java_interface);
    RUN_TEST(java_interface_no_duplicate_function_issue1234);
    RUN_TEST(java_enum_dedup_preserves_calls_issue1234);
    RUN_TEST(scala_class_no_duplicate_function_issue91);
    RUN_TEST(solidity_contract_no_duplicate_function_issue91);
    RUN_TEST(ruby_class_no_duplicate_function_issue91);
    RUN_TEST(java_class_extends_and_implements);
    RUN_TEST(python_class_base_extracted_bare);
    RUN_TEST(php_class);
    RUN_TEST(php_function);
    RUN_TEST(ruby_class);
    RUN_TEST(ruby_module);
    RUN_TEST(csharp_class);
    RUN_TEST(csharp_interface);
    RUN_TEST(extract_csharp_stacked_attributes_issue1692);
    RUN_TEST(swift_class);
    RUN_TEST(swift_protocol);
    RUN_TEST(kotlin_function);
    RUN_TEST(kotlin_class);
    RUN_TEST(scala_function);
    RUN_TEST(scala_class);
    RUN_TEST(dart_class);
    RUN_TEST(dart_extension_members);
    RUN_TEST_LANG(CBM_LANG_GROOVY, groovy_class);

    /* Systems */
    RUN_TEST(rust_function);
    RUN_TEST(rust_struct);
    RUN_TEST(go_function);
    RUN_TEST(go_struct);
    RUN_TEST(go_interface);
    RUN_TEST(zig_function);
    RUN_TEST(c_function);
    RUN_TEST(c_struct);
    RUN_TEST(c_preprocessor_invalid_shift_does_not_trap);
    RUN_TEST(cpp_class);

    /* Scripting */
    RUN_TEST(python_function);
    RUN_TEST(python_class);
    RUN_TEST(js_function);
    RUN_TEST(js_class);
    RUN_TEST(ts_function);
    RUN_TEST(ts_class);
    RUN_TEST(lua_function);
    RUN_TEST(bash_function);
    RUN_TEST_LANG(CBM_LANG_PERL, perl_function);
    RUN_TEST_LANG(CBM_LANG_R, r_function);

    /* Functional */
    RUN_TEST(elixir_function);
    RUN_TEST(elixir_call_string_argument);
    RUN_TEST(haskell_function);
    RUN_TEST_LANG(CBM_LANG_OCAML, ocaml_function);
    RUN_TEST_LANG(CBM_LANG_ERLANG, erlang_function);

    /* Markup/Config */
    RUN_TEST(yaml_variables);
    RUN_TEST(hcl_blocks);
    RUN_TEST_LANG(CBM_LANG_SQL, sql_create_table);
    RUN_TEST_LANG(CBM_LANG_DOCKERFILE, dockerfile_stages);

    /* Scientific */
    RUN_TEST_LANG(CBM_LANG_MATLAB, matlab_function);
    RUN_TEST_LANG(CBM_LANG_LEAN, lean_function);
    RUN_TEST_LANG(CBM_LANG_FORM, form_procedure);
    RUN_TEST_LANG(CBM_LANG_WOLFRAM, wolfram_function);
    RUN_TEST_LANG(CBM_LANG_MATLAB, magma_function);

    /* v0.5 expansion */
    RUN_TEST_LANG(CBM_LANG_FSHARP, fsharp_function);
    RUN_TEST_LANG(CBM_LANG_JULIA, julia_function);
    RUN_TEST_LANG(CBM_LANG_ELM, elm_function);
    RUN_TEST(nix_function);
    RUN_TEST_LANG(CBM_LANG_FORTRAN, fortran_function);

    /* OOP/Systems variants */
    RUN_TEST(swift_struct);
    RUN_TEST(swift_simple_call);
    RUN_TEST(swift_method_call);
    RUN_TEST(swift_constructor_call);
    RUN_TEST(swift_chained_call);
    RUN_TEST(swift_force_unwrap);
    RUN_TEST(swift_call_string_arg_issue1892);
    RUN_TEST(swift_labeled_call_string_arg_issue1892);
    RUN_TEST(swift_nested_url_constructor_issue1892);
    RUN_TEST(swift_nested_url_no_bang_issue1892);
    RUN_TEST(swift_non_url_constructor_untouched_issue1892);
    RUN_TEST(swift_try_bang);
    RUN_TEST_LANG(CBM_LANG_OBJC, objc_interface);
    RUN_TEST_LANG(CBM_LANG_MATLAB, objc_implementation);
    RUN_TEST(dart_top_level_function);
    RUN_TEST(rust_enum);
    RUN_TEST(zig_struct);
    RUN_TEST(zig_struct_members_are_scoped);
    RUN_TEST(zig_container_aliases_are_scoped);
    RUN_TEST(zig_comptime_if_container_is_named);
    RUN_TEST(zig_type_function_containers_are_named);
    RUN_TEST(zig_import_names_and_member_receivers);
    RUN_TEST(cpp_function);
    RUN_TEST(cpp_out_of_line_method_issue428);
    RUN_TEST_LANG(CBM_LANG_COBOL, cobol_paragraph);
    RUN_TEST_LANG(CBM_LANG_VERILOG, verilog_module);
    RUN_TEST_LANG(CBM_LANG_CUDA, cuda_kernel);
    RUN_TEST(python_decorator);
    RUN_TEST(ts_interface);
    RUN_TEST(tsx_component);
    RUN_TEST(lua_table_method);
    RUN_TEST_LANG(CBM_LANG_EMACSLISP, emacs_lisp_defun);
    RUN_TEST_LANG(CBM_LANG_EMACSLISP, emacs_lisp_defvar);
    RUN_TEST(haskell_data_type);
    RUN_TEST_LANG(CBM_LANG_CLOJURE, clojure_function);

    /* Config/Markup */
    RUN_TEST(html_elements);
    RUN_TEST_LANG(CBM_LANG_SQL, sql_function);
    RUN_TEST_LANG(CBM_LANG_MESON, meson_project);
    RUN_TEST(css_rules);
    RUN_TEST_LANG(CBM_LANG_SCSS, scss_rules);
    RUN_TEST_LANG(CBM_LANG_TOML, toml_basic);
    RUN_TEST_LANG(CBM_LANG_CMAKE, cmake_function);
    RUN_TEST(json_object);
    RUN_TEST_LANG(CBM_LANG_PROTOBUF, protobuf_message);
    RUN_TEST_LANG(CBM_LANG_GRAPHQL, graphql_type);
    RUN_TEST_LANG(CBM_LANG_SVELTE, svelte_component);
    RUN_TEST_LANG(CBM_LANG_VUE, vue_component);
    RUN_TEST_LANG(CBM_LANG_GLSL, glsl_shader);
    RUN_TEST_LANG(CBM_LANG_VIMSCRIPT, vimscript_function);

    /* Scientific extended */
    RUN_TEST_LANG(CBM_LANG_MATLAB, matlab_parse);
    RUN_TEST_LANG(CBM_LANG_MATLAB, matlab_call);
    RUN_TEST_LANG(CBM_LANG_LEAN, lean_parse);
    RUN_TEST_LANG(CBM_LANG_LEAN, lean_call);
    RUN_TEST_LANG(CBM_LANG_LEAN, lean_type_annotation_not_call);
    RUN_TEST_LANG(CBM_LANG_FORM, form_parse);
    RUN_TEST_LANG(CBM_LANG_FORM, form_call);
    RUN_TEST_LANG(CBM_LANG_MAGMA, magma_procedure);
    RUN_TEST_LANG(CBM_LANG_MAGMA, magma_parse);
    RUN_TEST_LANG(CBM_LANG_MAGMA, magma_import);
    RUN_TEST_LANG(CBM_LANG_MAGMA, magma_call);
    RUN_TEST_LANG(CBM_LANG_MATLAB, magma_disambiguation);
    RUN_TEST_LANG(CBM_LANG_WOLFRAM, wolfram_function_extended);
    RUN_TEST_LANG(CBM_LANG_WOLFRAM, wolfram_call);
    RUN_TEST_LANG(CBM_LANG_WOLFRAM, wolfram_caller_attribution);
    RUN_TEST(c_caller_attribution);
    RUN_TEST(cpp_out_of_line_method_caller_attribution);
    RUN_TEST(cpp_out_of_line_ctor_dtor_caller_attribution);
    RUN_TEST_LANG(CBM_LANG_WOLFRAM, wolfram_parse);
    RUN_TEST_LANG(CBM_LANG_WOLFRAM, wolfram_import);
    RUN_TEST_LANG(CBM_LANG_WOLFRAM, wolfram_nested_def);

    /* cbm_test.go ports */
    RUN_TEST(python_docstring);
    RUN_TEST(c_docstring_truncation_preserves_utf8_boundary);
    RUN_TEST(go_function_extraction);
    RUN_TEST(js_arrow_function);

    /* language_failures_test.go ports */
    RUN_TEST_LANG(CBM_LANG_COMMONLISP, commonlisp_defun);
    RUN_TEST_LANG(CBM_LANG_COMMONLISP, commonlisp_multiple_functions);
    RUN_TEST_LANG(CBM_LANG_COMMONLISP, commonlisp_defmacro);
    RUN_TEST_LANG(CBM_LANG_MAKEFILE, makefile_rule_as_function);
    RUN_TEST_LANG(CBM_LANG_MAKEFILE, makefile_multiple_targets);
    RUN_TEST_LANG(CBM_LANG_MAKEFILE, makefile_variable_extraction);
    RUN_TEST_LANG(CBM_LANG_VIMSCRIPT, vimscript_function_extraction);
    RUN_TEST_LANG(CBM_LANG_VIMSCRIPT, vimscript_function_without_bang);
    RUN_TEST_LANG(CBM_LANG_JULIA, julia_function_extraction);
    RUN_TEST_LANG(CBM_LANG_JULIA, julia_function_with_args);

    /* Cross-cutting */
    RUN_TEST(python_calls);
    RUN_TEST(python_iris_classMethodValue);
    RUN_TEST(go_calls);
    RUN_TEST(python_imports);
    RUN_TEST(js_imports);
    RUN_TEST(go_imports);
    RUN_TEST(go_cgo_pseudo_import_dropped);
    RUN_TEST(java_imports);
    RUN_TEST(rust_imports);
    RUN_TEST(c_imports);
    RUN_TEST(ruby_imports);
    RUN_TEST(lua_imports);
    RUN_TEST(import_stress_go);
    RUN_TEST_LANG(CBM_LANG_SVELTE, svelte_imports_basic);
    RUN_TEST_LANG(CBM_LANG_SVELTE, svelte_imports_no_script);
    RUN_TEST_LANG(CBM_LANG_VUE, vue_imports_basic);
    RUN_TEST(html_imports_basic);

    /* config_extraction_test.go ports */
    RUN_TEST_LANG(CBM_LANG_TOML, toml_basic_table_and_pair);
    RUN_TEST_LANG(CBM_LANG_TOML, toml_nested_table);
    RUN_TEST_LANG(CBM_LANG_TOML, toml_table_array_element);
    RUN_TEST_LANG(CBM_LANG_TOML, toml_dotted_key);
    RUN_TEST_LANG(CBM_LANG_TOML, toml_quoted_key);
    RUN_TEST_LANG(CBM_LANG_TOML, toml_empty_table);
    RUN_TEST_LANG(CBM_LANG_TOML, toml_comments_only);
    RUN_TEST_LANG(CBM_LANG_TOML, toml_boolean_and_integer_values);
    RUN_TEST_LANG(CBM_LANG_INI, ini_basic_section_and_setting);
    RUN_TEST_LANG(CBM_LANG_INI, ini_multiple_sections);
    RUN_TEST_LANG(CBM_LANG_INI, ini_global_keys);
    RUN_TEST_LANG(CBM_LANG_INI, ini_comments);
    RUN_TEST(json_basic_pair);
    RUN_TEST(json_nested_object);
    RUN_TEST(json_empty_object);
    RUN_TEST(json_boolean_null_values);
    RUN_TEST(json_package_json_deps);
    RUN_TEST_LANG(CBM_LANG_XML, xml_basic_element);
    RUN_TEST_LANG(CBM_LANG_XML, xml_self_closing_tag);
    RUN_TEST_LANG(CBM_LANG_XML, xml_empty_document);
    RUN_TEST_LANG(CBM_LANG_XML, xml_multiple_children);
    RUN_TEST(markdown_atx_headings);
    RUN_TEST(markdown_setext_headings);
    RUN_TEST(markdown_heading_content);
    RUN_TEST(markdown_no_headings);

    /* __init__.py / index.ts Module QN collision regression */
    RUN_TEST(python_init_module_qn_not_collide_with_folder);
    RUN_TEST(python_init_nested_module_qn);
    RUN_TEST(js_index_module_qn_not_collide_with_folder);
    RUN_TEST(python_regular_module_qn_unchanged);
    RUN_TEST(extract_java_method_annotations_issue382);
    RUN_TEST(extract_ts_url_builder_issue1009);
    RUN_TEST(extract_ts_route_handler_after_named_middleware);
    RUN_TEST(extract_ts_route_handler_after_inline_middleware);
    RUN_TEST(extract_ts_url_builder_composed_issue1009);
    RUN_TEST(extract_c_url_builder_gated_issue1009);
    RUN_TEST(extract_ts_url_builder_mixed_returns_issue1009);
    RUN_TEST(extract_ts_url_builder_ambiguous_issue1009);
    RUN_TEST(extract_ts_url_builder_reference_issue1009);
    RUN_TEST(extract_ts_url_builder_non_url_issue1009);
    RUN_TEST(extract_java_no_double_class_qn);
    RUN_TEST(extract_go_no_filename_in_module_qn);
    RUN_TEST(extract_large_ts_has_functions_issue213);

    /* Per-function complexity metrics (Tier A) */
    RUN_TEST(complexity_nested_loops_depth);
    RUN_TEST(complexity_loop_with_branch);
    RUN_TEST(complexity_flat_no_loops);
    RUN_TEST(complexity_linear_scan_in_loop);
    RUN_TEST(complexity_recursion_in_loop_unguarded);
    RUN_TEST(complexity_guarded_recursion);
    RUN_TEST(complexity_super_only_not_recursive);
    RUN_TEST(complexity_same_name_other_receiver_not_recursive);
    RUN_TEST(complexity_self_receiver_still_recursive);
    RUN_TEST(complexity_chained_receiver_not_self);
    RUN_TEST(complexity_go_method_receiver_self_recursion);
    RUN_TEST(complexity_access_depth_and_params);
    RUN_TEST(walk_defs_no_truncation_over_4096_issue668);
    RUN_TEST(extract_rust_test_attr_marks_is_test_issue855);

    cbm_shutdown();
    RUN_TEST(extract_ts_template_string_url_issue1006);
    RUN_TEST(extract_java_jaxrs_path_composition_issue1005);
    RUN_TEST(extract_java_jaxrs_relative_path_templates);
    RUN_TEST(extract_java_jaxrs_empty_path_means_class_path);
    RUN_TEST(extract_java_spring_relative_string_not_route_path);
    RUN_TEST(extract_go_binary_concat_url_issue1249);
    RUN_TEST(extract_go_binary_concat_url_no_literal_suffix_issue1249);
    RUN_TEST(extract_c_test_dir_marks_is_test_issue1294);
    RUN_TEST(extract_python_method_test_dir_marks_is_test_issue1294);
    RUN_TEST(extract_c_anonymous_typedef_aggregate_is_one_def);
    RUN_TEST(extract_cpp_macro_prefixed_function_names_c1);
    RUN_TEST(extract_cpp_macro_prefixed_lsp_caller_c1);
    RUN_TEST(extract_c_macro_prefixed_function_name_c1);
    RUN_TEST(extract_c_tag_reference_is_not_a_definition_c1);
    RUN_TEST(extract_c_struct_head_in_error_region_recovered_c1);
    RUN_TEST(extract_c_keyword_is_never_a_function_name_c1);
    RUN_TEST(extract_c_function_lost_to_split_branches_restored_c1);
    RUN_TEST(extract_c_typedef_names_are_definitions_c1);
    RUN_TEST(extract_c_macro_qn_is_fenced_c1);
    RUN_TEST(extract_c_enumerators_are_flat_c1);
    RUN_TEST(extract_cpp_enum_scoping_c1);
    RUN_TEST(extract_c_enum_inside_struct_is_file_scope_c1);
    RUN_TEST(extract_cpp_enum_inside_class_keeps_class_scope_c1);
    RUN_TEST(extract_c_enum_macro_wrapped_list_c1);
    RUN_TEST(extract_c_variants_lists_same_file_duplicates_c1);
    RUN_TEST(extract_variants_only_in_c_preprocessor_languages_c1);
    RUN_TEST(extract_axios_create_client_and_module_default_export_issue1916);
    RUN_TEST(extract_es_default_import_flag_issue1916);
    RUN_TEST(extract_lookalike_create_is_not_client_issue1916);
}
