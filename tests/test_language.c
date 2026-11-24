/*
 * test_language.c — Tests for language detection (filename + extension).
 *
 * RED phase: These tests define the expected behavior for all 64 languages.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "discover/discover.h"

/* ── Extension-based detection ─────────────────────────────────── */

TEST(lang_ext_go) {
    ASSERT_EQ(cbm_language_for_extension(".go"), CBM_LANG_GO);
    PASS();
}
TEST(lang_ext_python) {
    ASSERT_EQ(cbm_language_for_extension(".py"), CBM_LANG_PYTHON);
    PASS();
}
TEST(lang_ext_javascript) {
    ASSERT_EQ(cbm_language_for_extension(".js"), CBM_LANG_JAVASCRIPT);
    PASS();
}
TEST(lang_ext_jsx) {
    ASSERT_EQ(cbm_language_for_extension(".jsx"), CBM_LANG_JAVASCRIPT);
    PASS();
}
/* Issue #197: .mjs (ES modules) / .cjs (CommonJS) were unmapped, so those
 * files were never indexed or searchable. */
TEST(lang_ext_mjs_cjs) {
    ASSERT_EQ(cbm_language_for_extension(".mjs"), CBM_LANG_JAVASCRIPT);
    ASSERT_EQ(cbm_language_for_extension(".cjs"), CBM_LANG_JAVASCRIPT);
    PASS();
}
TEST(lang_ext_mts_cts) {
    ASSERT_EQ(cbm_language_for_extension(".mts"), CBM_LANG_TYPESCRIPT);
    ASSERT_EQ(cbm_language_for_extension(".cts"), CBM_LANG_TYPESCRIPT);
    PASS();
}
TEST(lang_ext_typescript) {
    ASSERT_EQ(cbm_language_for_extension(".ts"), CBM_LANG_TYPESCRIPT);
    PASS();
}
TEST(lang_ext_tsx) {
    ASSERT_EQ(cbm_language_for_extension(".tsx"), CBM_LANG_TSX);
    PASS();
}
TEST(lang_ext_rust) {
    ASSERT_EQ(cbm_language_for_extension(".rs"), CBM_LANG_RUST);
    PASS();
}
TEST(lang_ext_java) {
    ASSERT_EQ(cbm_language_for_extension(".java"), CBM_LANG_JAVA);
    PASS();
}
TEST(lang_ext_cpp) {
    ASSERT_EQ(cbm_language_for_extension(".cpp"), CBM_LANG_CPP);
    PASS();
}
TEST(lang_ext_hpp) {
    ASSERT_EQ(cbm_language_for_extension(".hpp"), CBM_LANG_CPP);
    PASS();
}
TEST(lang_ext_cc) {
    ASSERT_EQ(cbm_language_for_extension(".cc"), CBM_LANG_CPP);
    PASS();
}
TEST(lang_ext_cxx) {
    ASSERT_EQ(cbm_language_for_extension(".cxx"), CBM_LANG_CPP);
    PASS();
}
TEST(lang_ext_hxx) {
    ASSERT_EQ(cbm_language_for_extension(".hxx"), CBM_LANG_CPP);
    PASS();
}
TEST(lang_ext_hh) {
    ASSERT_EQ(cbm_language_for_extension(".hh"), CBM_LANG_CPP);
    PASS();
}
TEST(lang_ext_h) {
    ASSERT_EQ(cbm_language_for_extension(".h"), CBM_LANG_CPP);
    PASS();
}
TEST(lang_ext_ixx) {
    ASSERT_EQ(cbm_language_for_extension(".ixx"), CBM_LANG_CPP);
    PASS();
}
TEST(lang_ext_csharp) {
    ASSERT_EQ(cbm_language_for_extension(".cs"), CBM_LANG_CSHARP);
    PASS();
}
TEST(lang_ext_php) {
    ASSERT_EQ(cbm_language_for_extension(".php"), CBM_LANG_PHP);
    PASS();
}
TEST(lang_ext_lua) {
    ASSERT_EQ(cbm_language_for_extension(".lua"), CBM_LANG_LUA);
    PASS();
}
TEST(lang_ext_scala) {
    ASSERT_EQ(cbm_language_for_extension(".scala"), CBM_LANG_SCALA);
    PASS();
}
TEST(lang_ext_sc) {
    ASSERT_EQ(cbm_language_for_extension(".sc"), CBM_LANG_SCALA);
    PASS();
}
TEST(lang_ext_kotlin) {
    ASSERT_EQ(cbm_language_for_extension(".kt"), CBM_LANG_KOTLIN);
    PASS();
}
TEST(lang_ext_kts) {
    ASSERT_EQ(cbm_language_for_extension(".kts"), CBM_LANG_KOTLIN);
    PASS();
}
TEST(lang_ext_ruby) {
    ASSERT_EQ(cbm_language_for_extension(".rb"), CBM_LANG_RUBY);
    PASS();
}
TEST(lang_ext_rake) {
    ASSERT_EQ(cbm_language_for_extension(".rake"), CBM_LANG_RUBY);
    PASS();
}
TEST(lang_ext_gemspec) {
    ASSERT_EQ(cbm_language_for_extension(".gemspec"), CBM_LANG_RUBY);
    PASS();
}
TEST(lang_ext_c) {
    ASSERT_EQ(cbm_language_for_extension(".c"), CBM_LANG_C);
    PASS();
}
TEST(lang_ext_bash) {
    ASSERT_EQ(cbm_language_for_extension(".sh"), CBM_LANG_BASH);
    PASS();
}
TEST(lang_ext_bash2) {
    ASSERT_EQ(cbm_language_for_extension(".bash"), CBM_LANG_BASH);
    PASS();
}
TEST(lang_ext_zig) {
    ASSERT_EQ(cbm_language_for_extension(".zig"), CBM_LANG_ZIG);
    PASS();
}
TEST(lang_ext_elixir) {
    ASSERT_EQ(cbm_language_for_extension(".ex"), CBM_LANG_ELIXIR);
    PASS();
}
TEST(lang_ext_exs) {
    ASSERT_EQ(cbm_language_for_extension(".exs"), CBM_LANG_ELIXIR);
    PASS();
}
TEST(lang_ext_haskell) {
    ASSERT_EQ(cbm_language_for_extension(".hs"), CBM_LANG_HASKELL);
    PASS();
}
TEST(lang_ext_ocaml) {
    ASSERT_EQ(cbm_language_for_extension(".ml"), CBM_LANG_OCAML);
    PASS();
}
TEST(lang_ext_mli) {
    ASSERT_EQ(cbm_language_for_extension(".mli"), CBM_LANG_OCAML);
    PASS();
}
TEST(lang_ext_swift) {
    ASSERT_EQ(cbm_language_for_extension(".swift"), CBM_LANG_SWIFT);
    PASS();
}
TEST(lang_ext_dart) {
    ASSERT_EQ(cbm_language_for_extension(".dart"), CBM_LANG_DART);
    PASS();
}
TEST(lang_ext_perl) {
    ASSERT_EQ(cbm_language_for_extension(".pl"), CBM_LANG_PERL);
    PASS();
}
TEST(lang_ext_pm) {
    ASSERT_EQ(cbm_language_for_extension(".pm"), CBM_LANG_PERL);
    PASS();
}
TEST(lang_ext_groovy) {
    ASSERT_EQ(cbm_language_for_extension(".groovy"), CBM_LANG_GROOVY);
    PASS();
}
TEST(lang_ext_gradle) {
    ASSERT_EQ(cbm_language_for_extension(".gradle"), CBM_LANG_GROOVY);
    PASS();
}
TEST(lang_ext_erlang) {
    ASSERT_EQ(cbm_language_for_extension(".erl"), CBM_LANG_ERLANG);
    PASS();
}
TEST(lang_ext_r) {
    ASSERT_EQ(cbm_language_for_extension(".r"), CBM_LANG_R);
    PASS();
}
TEST(lang_ext_R) {
    ASSERT_EQ(cbm_language_for_extension(".R"), CBM_LANG_R);
    PASS();
}

/* Tier 2 programming */
TEST(lang_ext_clojure) {
    ASSERT_EQ(cbm_language_for_extension(".clj"), CBM_LANG_CLOJURE);
    PASS();
}
TEST(lang_ext_cljs) {
    ASSERT_EQ(cbm_language_for_extension(".cljs"), CBM_LANG_CLOJURE);
    PASS();
}
TEST(lang_ext_cljc) {
    ASSERT_EQ(cbm_language_for_extension(".cljc"), CBM_LANG_CLOJURE);
    PASS();
}
TEST(lang_ext_fsharp) {
    ASSERT_EQ(cbm_language_for_extension(".fs"), CBM_LANG_FSHARP);
    PASS();
}
TEST(lang_ext_fsi) {
    ASSERT_EQ(cbm_language_for_extension(".fsi"), CBM_LANG_FSHARP);
    PASS();
}
TEST(lang_ext_fsx) {
    ASSERT_EQ(cbm_language_for_extension(".fsx"), CBM_LANG_FSHARP);
    PASS();
}
TEST(lang_ext_julia) {
    ASSERT_EQ(cbm_language_for_extension(".jl"), CBM_LANG_JULIA);
    PASS();
}
TEST(lang_ext_vim) {
    ASSERT_EQ(cbm_language_for_extension(".vim"), CBM_LANG_VIMSCRIPT);
    PASS();
}
TEST(lang_ext_nix) {
    ASSERT_EQ(cbm_language_for_extension(".nix"), CBM_LANG_NIX);
    PASS();
}
TEST(lang_ext_commonlisp) {
    ASSERT_EQ(cbm_language_for_extension(".lisp"), CBM_LANG_COMMONLISP);
    PASS();
}
TEST(lang_ext_lsp) {
    ASSERT_EQ(cbm_language_for_extension(".lsp"), CBM_LANG_COMMONLISP);
    PASS();
}
TEST(lang_ext_cl) {
    ASSERT_EQ(cbm_language_for_extension(".cl"), CBM_LANG_COMMONLISP);
    PASS();
}
TEST(lang_ext_elm) {
    ASSERT_EQ(cbm_language_for_extension(".elm"), CBM_LANG_ELM);
    PASS();
}
TEST(lang_ext_fortran) {
    ASSERT_EQ(cbm_language_for_extension(".f90"), CBM_LANG_FORTRAN);
    PASS();
}
TEST(lang_ext_f95) {
    ASSERT_EQ(cbm_language_for_extension(".f95"), CBM_LANG_FORTRAN);
    PASS();
}
TEST(lang_ext_f03) {
    ASSERT_EQ(cbm_language_for_extension(".f03"), CBM_LANG_FORTRAN);
    PASS();
}
TEST(lang_ext_f08) {
    ASSERT_EQ(cbm_language_for_extension(".f08"), CBM_LANG_FORTRAN);
    PASS();
}
TEST(lang_ext_cuda) {
    ASSERT_EQ(cbm_language_for_extension(".cu"), CBM_LANG_CUDA);
    PASS();
}
TEST(lang_ext_cuh) {
    ASSERT_EQ(cbm_language_for_extension(".cuh"), CBM_LANG_CUDA);
    PASS();
}
TEST(lang_ext_cobol) {
    ASSERT_EQ(cbm_language_for_extension(".cob"), CBM_LANG_COBOL);
    PASS();
}
TEST(lang_ext_cbl) {
    ASSERT_EQ(cbm_language_for_extension(".cbl"), CBM_LANG_COBOL);
    PASS();
}
TEST(lang_ext_verilog) {
    ASSERT_EQ(cbm_language_for_extension(".v"), CBM_LANG_VERILOG);
    PASS();
}
TEST(lang_ext_sv) {
    ASSERT_EQ(cbm_language_for_extension(".sv"), CBM_LANG_VERILOG);
    PASS();
}
TEST(lang_ext_emacslisp) {
    ASSERT_EQ(cbm_language_for_extension(".el"), CBM_LANG_EMACSLISP);
    PASS();
}

/* Scientific/math */
TEST(lang_ext_matlab) {
    ASSERT_EQ(cbm_language_for_extension(".matlab"), CBM_LANG_MATLAB);
    PASS();
}
TEST(lang_ext_mlx) {
    ASSERT_EQ(cbm_language_for_extension(".mlx"), CBM_LANG_MATLAB);
    PASS();
}
TEST(lang_ext_lean) {
    ASSERT_EQ(cbm_language_for_extension(".lean"), CBM_LANG_LEAN);
    PASS();
}
TEST(lang_ext_form) {
    ASSERT_EQ(cbm_language_for_extension(".frm"), CBM_LANG_FORM);
    PASS();
}
TEST(lang_ext_prc) {
    ASSERT_EQ(cbm_language_for_extension(".prc"), CBM_LANG_FORM);
    PASS();
}
TEST(lang_ext_magma) {
    ASSERT_EQ(cbm_language_for_extension(".mag"), CBM_LANG_MAGMA);
    PASS();
}
TEST(lang_ext_magma2) {
    ASSERT_EQ(cbm_language_for_extension(".magma"), CBM_LANG_MAGMA);
    PASS();
}
TEST(lang_ext_wolfram) {
    ASSERT_EQ(cbm_language_for_extension(".wl"), CBM_LANG_WOLFRAM);
    PASS();
}
TEST(lang_ext_wls) {
    ASSERT_EQ(cbm_language_for_extension(".wls"), CBM_LANG_WOLFRAM);
    PASS();
}

/* Helper languages */
TEST(lang_ext_html) {
    ASSERT_EQ(cbm_language_for_extension(".html"), CBM_LANG_HTML);
    PASS();
}
TEST(lang_ext_htm) {
    ASSERT_EQ(cbm_language_for_extension(".htm"), CBM_LANG_HTML);
    PASS();
}
TEST(lang_ext_css) {
    ASSERT_EQ(cbm_language_for_extension(".css"), CBM_LANG_CSS);
    PASS();
}
TEST(lang_ext_scss) {
    ASSERT_EQ(cbm_language_for_extension(".scss"), CBM_LANG_SCSS);
    PASS();
}
TEST(lang_ext_yaml) {
    ASSERT_EQ(cbm_language_for_extension(".yml"), CBM_LANG_YAML);
    PASS();
}
TEST(lang_ext_yaml2) {
    ASSERT_EQ(cbm_language_for_extension(".yaml"), CBM_LANG_YAML);
    PASS();
}
TEST(lang_ext_toml) {
    ASSERT_EQ(cbm_language_for_extension(".toml"), CBM_LANG_TOML);
    PASS();
}
TEST(lang_ext_hcl) {
    ASSERT_EQ(cbm_language_for_extension(".tf"), CBM_LANG_HCL);
    PASS();
}
TEST(lang_ext_hcl2) {
    ASSERT_EQ(cbm_language_for_extension(".hcl"), CBM_LANG_HCL);
    PASS();
}
TEST(lang_ext_sql) {
    ASSERT_EQ(cbm_language_for_extension(".sql"), CBM_LANG_SQL);
    PASS();
}
TEST(lang_ext_dockerfile) {
    ASSERT_EQ(cbm_language_for_extension(".dockerfile"), CBM_LANG_DOCKERFILE);
    PASS();
}
TEST(lang_ext_json) {
    ASSERT_EQ(cbm_language_for_extension(".json"), CBM_LANG_JSON);
    PASS();
}
TEST(lang_ext_xml) {
    ASSERT_EQ(cbm_language_for_extension(".xml"), CBM_LANG_XML);
    PASS();
}
TEST(lang_ext_xsl) {
    ASSERT_EQ(cbm_language_for_extension(".xsl"), CBM_LANG_XML);
    PASS();
}
TEST(lang_ext_xsd) {
    ASSERT_EQ(cbm_language_for_extension(".xsd"), CBM_LANG_XML);
    PASS();
}
TEST(lang_ext_svg) {
    ASSERT_EQ(cbm_language_for_extension(".svg"), CBM_LANG_XML);
    PASS();
}
TEST(lang_ext_markdown) {
    ASSERT_EQ(cbm_language_for_extension(".md"), CBM_LANG_MARKDOWN);
    PASS();
}
TEST(lang_ext_mdx) {
    ASSERT_EQ(cbm_language_for_extension(".mdx"), CBM_LANG_MARKDOWN);
    PASS();
}
TEST(lang_ext_makefile) {
    ASSERT_EQ(cbm_language_for_extension(".mk"), CBM_LANG_MAKEFILE);
    PASS();
}
TEST(lang_ext_cmake) {
    ASSERT_EQ(cbm_language_for_extension(".cmake"), CBM_LANG_CMAKE);
    PASS();
}
TEST(lang_ext_protobuf) {
    ASSERT_EQ(cbm_language_for_extension(".proto"), CBM_LANG_PROTOBUF);
    PASS();
}
TEST(lang_ext_graphql) {
    ASSERT_EQ(cbm_language_for_extension(".graphql"), CBM_LANG_GRAPHQL);
    PASS();
}
TEST(lang_ext_gql) {
    ASSERT_EQ(cbm_language_for_extension(".gql"), CBM_LANG_GRAPHQL);
    PASS();
}
TEST(lang_ext_vue) {
    ASSERT_EQ(cbm_language_for_extension(".vue"), CBM_LANG_VUE);
    PASS();
}
TEST(lang_ext_svelte) {
    ASSERT_EQ(cbm_language_for_extension(".svelte"), CBM_LANG_SVELTE);
    PASS();
}
TEST(lang_ext_meson) {
    ASSERT_EQ(cbm_language_for_extension(".meson"), CBM_LANG_MESON);
    PASS();
}
TEST(lang_ext_glsl) {
    ASSERT_EQ(cbm_language_for_extension(".glsl"), CBM_LANG_GLSL);
    PASS();
}
TEST(lang_ext_vert) {
    ASSERT_EQ(cbm_language_for_extension(".vert"), CBM_LANG_GLSL);
    PASS();
}
TEST(lang_ext_frag) {
    ASSERT_EQ(cbm_language_for_extension(".frag"), CBM_LANG_GLSL);
    PASS();
}
TEST(lang_ext_ini) {
    ASSERT_EQ(cbm_language_for_extension(".ini"), CBM_LANG_INI);
    PASS();
}
TEST(lang_ext_cfg) {
    ASSERT_EQ(cbm_language_for_extension(".cfg"), CBM_LANG_INI);
    PASS();
}
TEST(lang_ext_conf) {
    ASSERT_EQ(cbm_language_for_extension(".conf"), CBM_LANG_INI);
    PASS();
}

/* Unknown extension */
TEST(lang_ext_unknown) {
    ASSERT_EQ(cbm_language_for_extension(".xyz"), CBM_LANG_COUNT);
    PASS();
}
TEST(lang_ext_null) {
    ASSERT_EQ(cbm_language_for_extension(""), CBM_LANG_COUNT);
    PASS();
}

/* ── Filename-based detection ──────────────────────────────────── */

TEST(lang_fn_makefile) {
    ASSERT_EQ(cbm_language_for_filename("Makefile"), CBM_LANG_MAKEFILE);
    PASS();
}
TEST(lang_fn_gnumakefile) {
    ASSERT_EQ(cbm_language_for_filename("GNUmakefile"), CBM_LANG_MAKEFILE);
    PASS();
}
TEST(lang_fn_makefile_lower) {
    ASSERT_EQ(cbm_language_for_filename("makefile"), CBM_LANG_MAKEFILE);
    PASS();
}
TEST(lang_fn_cmake) {
    ASSERT_EQ(cbm_language_for_filename("CMakeLists.txt"), CBM_LANG_CMAKE);
    PASS();
}
TEST(lang_fn_dockerfile) {
    ASSERT_EQ(cbm_language_for_filename("Dockerfile"), CBM_LANG_DOCKERFILE);
    PASS();
}
TEST(lang_fn_meson_build) {
    ASSERT_EQ(cbm_language_for_filename("meson.build"), CBM_LANG_MESON);
    PASS();
}
TEST(lang_fn_meson_opts) {
    ASSERT_EQ(cbm_language_for_filename("meson.options"), CBM_LANG_MESON);
    PASS();
}
TEST(lang_fn_meson_opts_txt) {
    ASSERT_EQ(cbm_language_for_filename("meson_options.txt"), CBM_LANG_MESON);
    PASS();
}
TEST(lang_fn_vimrc) {
    ASSERT_EQ(cbm_language_for_filename(".vimrc"), CBM_LANG_VIMSCRIPT);
    PASS();
}

/* issue #258: .blade.php is a built-in compound extension → Blade by default
 * (previously fell through to the single-extension lookup and was mis-typed as
 * PHP). Plain .php still maps to PHP. */
TEST(lang_fn_blade_php_compound_issue258) {
    ASSERT_EQ(cbm_language_for_filename("login.blade.php"), CBM_LANG_BLADE);
    ASSERT_EQ(cbm_language_for_filename("alert.blade.php"), CBM_LANG_BLADE);
    ASSERT_EQ(cbm_language_for_filename("index.php"), CBM_LANG_PHP);
    PASS();
}

/* Filename with extension falls through to extension lookup */
TEST(lang_fn_main_go) {
    ASSERT_EQ(cbm_language_for_filename("main.go"), CBM_LANG_GO);
    PASS();
}
TEST(lang_fn_test_py) {
    ASSERT_EQ(cbm_language_for_filename("test.py"), CBM_LANG_PYTHON);
    PASS();
}
TEST(lang_fn_unknown) {
    ASSERT_EQ(cbm_language_for_filename("README"), CBM_LANG_COUNT);
    PASS();
}

/* ── Language name ─────────────────────────────────────────────── */

TEST(lang_name_go) {
    ASSERT_STR_EQ(cbm_language_name(CBM_LANG_GO), "Go");
    PASS();
}
TEST(lang_name_python) {
    ASSERT_STR_EQ(cbm_language_name(CBM_LANG_PYTHON), "Python");
    PASS();
}
TEST(lang_name_cpp) {
    ASSERT_STR_EQ(cbm_language_name(CBM_LANG_CPP), "C++");
    PASS();
}
TEST(lang_name_csharp) {
    ASSERT_STR_EQ(cbm_language_name(CBM_LANG_CSHARP), "C#");
    PASS();
}
TEST(lang_name_unknown) {
    ASSERT_STR_EQ(cbm_language_name(CBM_LANG_COUNT), "Unknown");
    PASS();
}

/* ── .m disambiguation ─────────────────────────────────────────── */

/* These tests need temp files with content markers */
TEST(lang_m_objc) {
    /* Write a temp file with Objective-C markers */
    char path[256];
    snprintf(path, sizeof(path), "%s/test_lang_objc.m", cbm_tmpdir());
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "#import <Foundation/Foundation.h>\n@interface Foo : NSObject\n@end\n");
    fclose(f);

    ASSERT_EQ(cbm_disambiguate_m(path), CBM_LANG_OBJC);
    remove(path);
    PASS();
}

TEST(lang_m_magma) {
    char path[256];
    snprintf(path, sizeof(path), "%s/test_lang_magma.m", cbm_tmpdir());
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "function MyFunc(x)\n  return x^2;\nend function;\n");
    fclose(f);

    ASSERT_EQ(cbm_disambiguate_m(path), CBM_LANG_MAGMA);
    remove(path);
    PASS();
}

TEST(lang_m_matlab) {
    char path[256];
    snprintf(path, sizeof(path), "%s/test_lang_matlab.m", cbm_tmpdir());
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "function y = square(x)\n  y = x.^2;\nend\n");
    fclose(f);

    ASSERT_EQ(cbm_disambiguate_m(path), CBM_LANG_MATLAB);
    remove(path);
    PASS();
}

TEST(lang_m_default_on_read_fail) {
    /* Non-existent file defaults to MATLAB */
    ASSERT_EQ(cbm_disambiguate_m("/tmp/nonexistent_file_12345.m"), CBM_LANG_MATLAB);
    PASS();
}

/* ── .cls / .frm: VB6 vs Apex / FORM (upstream #721) ──────────── */

static bool write_probe_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return false;
    }
    fputs(content, f);
    fclose(f);
    return true;
}

TEST(lang_cls_vb6_class_module_unsupported) {
    char path[256];
    snprintf(path, sizeof(path), "%s/test_lang_vb6.cls", cbm_tmpdir());
    ASSERT_TRUE(write_probe_file(path, "VERSION 1.0 CLASS\r\nBEGIN\r\n  MultiUse = -1  'True\r\n"
                                       "END\r\nAttribute VB_Name = \"Widget\"\r\n"
                                       "Option Explicit\r\n\r\nPublic Sub Go()\r\nEnd Sub\r\n"));
    ASSERT_EQ(cbm_disambiguate_cls(path), CBM_LANG_COUNT);
    remove(path);
    PASS();
}

TEST(lang_cls_apex_stays_apex) {
    char path[256];
    snprintf(path, sizeof(path), "%s/test_lang_apex.cls", cbm_tmpdir());
    ASSERT_TRUE(write_probe_file(path, "public with sharing class Widget {\n"
                                       "  public void go() {}\n}\n"));
    ASSERT_EQ(cbm_disambiguate_cls(path), CBM_LANG_APEX);
    remove(path);
    /* Unreadable file keeps the pre-existing owner. */
    ASSERT_EQ(cbm_disambiguate_cls("/tmp/nonexistent_file_12345.cls"), CBM_LANG_APEX);
    PASS();
}

TEST(lang_frm_vb6_form_unsupported) {
    char path[256];
    snprintf(path, sizeof(path), "%s/test_lang_vb6.frm", cbm_tmpdir());
    ASSERT_TRUE(write_probe_file(path, "VERSION 5.00\r\nBegin VB.Form Form1 \r\n"
                                       "   Caption         =   \"Hi\"\r\nEnd\r\n"
                                       "Attribute VB_Name = \"Form1\"\r\nOption Explicit\r\n"));
    ASSERT_EQ(cbm_disambiguate_frm(path), CBM_LANG_COUNT);
    remove(path);
    PASS();
}

TEST(lang_frm_form_stays_form) {
    char path[256];
    snprintf(path, sizeof(path), "%s/test_lang_form.frm", cbm_tmpdir());
    ASSERT_TRUE(write_probe_file(path, "Symbols x, y;\nLocal F = x + y;\nPrint;\n.end\n"));
    ASSERT_EQ(cbm_disambiguate_frm(path), CBM_LANG_FORM);
    remove(path);
    ASSERT_EQ(cbm_disambiguate_frm("/tmp/nonexistent_file_12345.frm"), CBM_LANG_FORM);
    PASS();
}

/* #147: a binary resource shares the .res extension with ReScript.
 * Parsed as ReScript it can hang the indexer; binary content is not
 * ReScript source and must not be indexed as such. */
TEST(lang_res_binary_resource_unsupported) {
    static const unsigned char godot_head[] = {'R',  'S',  'R',  'C',  0x00, 0x00, 0x00, 0x00,
                                               0x04, 0x00, 0x00, 0x00, 'A',  'r',  'r',  'a',
                                               'y',  'M',  'e',  's',  'h',  0x00, 0xff, 0x80};
    char path[256];
    snprintf(path, sizeof(path), "%s/test_lang_godot.res", cbm_tmpdir());
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(fwrite(godot_head, 1, sizeof(godot_head), f), sizeof(godot_head));
    fclose(f);
    ASSERT_EQ(cbm_disambiguate_res(path), CBM_LANG_COUNT);
    remove(path);
    PASS();
}

TEST(lang_res_rescript_stays_rescript) {
    char path[256];
    snprintf(path, sizeof(path), "%s/test_lang_rescript.res", cbm_tmpdir());
    ASSERT_TRUE(write_probe_file(path, "let greet = name => `Hello ${name}`\n"
                                       "module M = {\n  let x = 1\n}\n"));
    ASSERT_EQ(cbm_disambiguate_res(path), CBM_LANG_RESCRIPT);
    remove(path);
    ASSERT_EQ(cbm_disambiguate_res("/tmp/nonexistent_file_12345.res"), CBM_LANG_RESCRIPT);
    PASS();
}

/* --- New languages (auto-generated) --- */
TEST(lang_ext_solidity) {
    ASSERT_EQ(cbm_language_for_extension(".sol"), CBM_LANG_SOLIDITY);
    PASS();
}

TEST(lang_ext_typst) {
    ASSERT_EQ(cbm_language_for_extension(".typ"), CBM_LANG_TYPST);
    PASS();
}

TEST(lang_ext_gdscript) {
    ASSERT_EQ(cbm_language_for_extension(".gd"), CBM_LANG_GDSCRIPT);
    PASS();
}

TEST(lang_ext_gleam) {
    ASSERT_EQ(cbm_language_for_extension(".gleam"), CBM_LANG_GLEAM);
    PASS();
}

TEST(lang_ext_powershell) {
    ASSERT_EQ(cbm_language_for_extension(".ps1"), CBM_LANG_POWERSHELL);
    ASSERT_EQ(cbm_language_for_extension(".psm1"), CBM_LANG_POWERSHELL);
    ASSERT_EQ(cbm_language_for_extension(".psd1"), CBM_LANG_POWERSHELL);
    PASS();
}

TEST(lang_ext_pascal) {
    ASSERT_EQ(cbm_language_for_extension(".pas"), CBM_LANG_PASCAL);
    ASSERT_EQ(cbm_language_for_extension(".lpr"), CBM_LANG_PASCAL);
    ASSERT_EQ(cbm_language_for_extension(".dpr"), CBM_LANG_PASCAL);
    PASS();
}

TEST(lang_ext_d) {
    ASSERT_EQ(cbm_language_for_extension(".d"), CBM_LANG_DLANG);
    PASS();
}

TEST(lang_ext_nim) {
    /* nim grammar removed — .nim/.nims no longer map to a language */
    ASSERT_EQ(cbm_language_for_extension(".nim"), CBM_LANG_COUNT);
    ASSERT_EQ(cbm_language_for_extension(".nims"), CBM_LANG_COUNT);
    PASS();
}

TEST(lang_ext_scheme) {
    ASSERT_EQ(cbm_language_for_extension(".scm"), CBM_LANG_SCHEME);
    ASSERT_EQ(cbm_language_for_extension(".ss"), CBM_LANG_SCHEME);
    PASS();
}

TEST(lang_ext_fennel) {
    ASSERT_EQ(cbm_language_for_extension(".fnl"), CBM_LANG_FENNEL);
    PASS();
}

TEST(lang_ext_fish) {
    ASSERT_EQ(cbm_language_for_extension(".fish"), CBM_LANG_FISH);
    PASS();
}

TEST(lang_ext_awk) {
    ASSERT_EQ(cbm_language_for_extension(".awk"), CBM_LANG_AWK);
    PASS();
}

TEST(lang_ext_zsh) {
    ASSERT_EQ(cbm_language_for_extension(".zsh"), CBM_LANG_ZSH);
    PASS();
}

TEST(lang_ext_tcl) {
    ASSERT_EQ(cbm_language_for_extension(".tcl"), CBM_LANG_TCL);
    PASS();
}

TEST(lang_ext_ada) {
    ASSERT_EQ(cbm_language_for_extension(".adb"), CBM_LANG_ADA);
    ASSERT_EQ(cbm_language_for_extension(".ads"), CBM_LANG_ADA);
    PASS();
}

TEST(lang_ext_agda) {
    ASSERT_EQ(cbm_language_for_extension(".agda"), CBM_LANG_AGDA);
    PASS();
}

TEST(lang_ext_racket) {
    ASSERT_EQ(cbm_language_for_extension(".rkt"), CBM_LANG_RACKET);
    PASS();
}

TEST(lang_ext_odin) {
    ASSERT_EQ(cbm_language_for_extension(".odin"), CBM_LANG_ODIN);
    PASS();
}

TEST(lang_ext_rescript) {
    ASSERT_EQ(cbm_language_for_extension(".res"), CBM_LANG_RESCRIPT);
    ASSERT_EQ(cbm_language_for_extension(".resi"), CBM_LANG_RESCRIPT);
    PASS();
}

TEST(lang_ext_purescript) {
    ASSERT_EQ(cbm_language_for_extension(".purs"), CBM_LANG_PURESCRIPT);
    PASS();
}

TEST(lang_ext_nickel) {
    ASSERT_EQ(cbm_language_for_extension(".ncl"), CBM_LANG_NICKEL);
    PASS();
}

TEST(lang_ext_crystal) {
    ASSERT_EQ(cbm_language_for_extension(".cr"), CBM_LANG_CRYSTAL);
    PASS();
}

TEST(lang_ext_teal) {
    ASSERT_EQ(cbm_language_for_extension(".tl"), CBM_LANG_TEAL);
    PASS();
}

TEST(lang_ext_hare) {
    ASSERT_EQ(cbm_language_for_extension(".ha"), CBM_LANG_HARE);
    PASS();
}

TEST(lang_ext_pony) {
    ASSERT_EQ(cbm_language_for_extension(".pony"), CBM_LANG_PONY);
    PASS();
}

TEST(lang_ext_luau) {
    ASSERT_EQ(cbm_language_for_extension(".luau"), CBM_LANG_LUAU);
    PASS();
}

TEST(lang_ext_qml) {
    ASSERT_EQ(cbm_language_for_extension(".qml"), CBM_LANG_QML);
    PASS();
}

TEST(lang_ext_cfml) {
    ASSERT_EQ(cbm_language_for_extension(".cfc"), CBM_LANG_CFSCRIPT);
    ASSERT_EQ(cbm_language_for_extension(".cfm"), CBM_LANG_CFML);
    PASS();
}

TEST(lang_ext_helm_tpl) {
    ASSERT_EQ(cbm_language_for_extension(".tpl"), CBM_LANG_GOTEMPLATE);
    PASS();
}

TEST(lang_ext_janet) {
    ASSERT_EQ(cbm_language_for_extension(".janet"), CBM_LANG_JANET);
    PASS();
}

TEST(lang_ext_sway) {
    ASSERT_EQ(cbm_language_for_extension(".sw"), CBM_LANG_SWAY);
    PASS();
}

TEST(lang_ext_nasm) {
    ASSERT_EQ(cbm_language_for_extension(".nasm"), CBM_LANG_NASM);
    PASS();
}

TEST(lang_ext_assembly) {
    ASSERT_EQ(cbm_language_for_extension(".s"), CBM_LANG_ASSEMBLY);
    ASSERT_EQ(cbm_language_for_extension(".S"), CBM_LANG_ASSEMBLY);
    PASS();
}

TEST(lang_ext_astro) {
    ASSERT_EQ(cbm_language_for_extension(".astro"), CBM_LANG_ASTRO);
    PASS();
}

TEST(lang_ext_gotemplate) {
    ASSERT_EQ(cbm_language_for_extension(".tmpl"), CBM_LANG_GOTEMPLATE);
    ASSERT_EQ(cbm_language_for_extension(".gotmpl"), CBM_LANG_GOTEMPLATE);
    PASS();
}

TEST(lang_ext_templ) {
    ASSERT_EQ(cbm_language_for_extension(".templ"), CBM_LANG_TEMPL);
    PASS();
}

TEST(lang_ext_liquid) {
    ASSERT_EQ(cbm_language_for_extension(".liquid"), CBM_LANG_LIQUID);
    PASS();
}

TEST(lang_ext_jinja2) {
    ASSERT_EQ(cbm_language_for_extension(".j2"), CBM_LANG_JINJA2);
    ASSERT_EQ(cbm_language_for_extension(".jinja2"), CBM_LANG_JINJA2);
    ASSERT_EQ(cbm_language_for_extension(".jinja"), CBM_LANG_JINJA2);
    PASS();
}

TEST(lang_ext_prisma) {
    ASSERT_EQ(cbm_language_for_extension(".prisma"), CBM_LANG_PRISMA);
    PASS();
}

TEST(lang_ext_hyprlang) {
    ASSERT_EQ(cbm_language_for_extension(".hl"), CBM_LANG_HYPRLANG);
    PASS();
}

TEST(lang_ext_diff) {
    ASSERT_EQ(cbm_language_for_extension(".diff"), CBM_LANG_DIFF);
    ASSERT_EQ(cbm_language_for_extension(".patch"), CBM_LANG_DIFF);
    PASS();
}

TEST(lang_ext_wgsl) {
    ASSERT_EQ(cbm_language_for_extension(".wgsl"), CBM_LANG_WGSL);
    PASS();
}

TEST(lang_ext_kdl) {
    ASSERT_EQ(cbm_language_for_extension(".kdl"), CBM_LANG_KDL);
    PASS();
}

TEST(lang_ext_json5) {
    ASSERT_EQ(cbm_language_for_extension(".json5"), CBM_LANG_JSON5);
    PASS();
}

TEST(lang_ext_jsonnet) {
    ASSERT_EQ(cbm_language_for_extension(".jsonnet"), CBM_LANG_JSONNET);
    ASSERT_EQ(cbm_language_for_extension(".libsonnet"), CBM_LANG_JSONNET);
    PASS();
}

TEST(lang_ext_ron) {
    ASSERT_EQ(cbm_language_for_extension(".ron"), CBM_LANG_RON);
    PASS();
}

TEST(lang_ext_thrift) {
    ASSERT_EQ(cbm_language_for_extension(".thrift"), CBM_LANG_THRIFT);
    PASS();
}

TEST(lang_ext_capnp) {
    ASSERT_EQ(cbm_language_for_extension(".capnp"), CBM_LANG_CAPNP);
    PASS();
}

TEST(lang_ext_properties) {
    ASSERT_EQ(cbm_language_for_extension(".properties"), CBM_LANG_PROPERTIES);
    PASS();
}

TEST(lang_ext_bibtex) {
    ASSERT_EQ(cbm_language_for_extension(".bib"), CBM_LANG_BIBTEX);
    PASS();
}

TEST(lang_ext_starlark) {
    ASSERT_EQ(cbm_language_for_extension(".star"), CBM_LANG_STARLARK);
    ASSERT_EQ(cbm_language_for_extension(".bzl"), CBM_LANG_STARLARK);
    PASS();
}

TEST(lang_ext_bicep) {
    ASSERT_EQ(cbm_language_for_extension(".bicep"), CBM_LANG_BICEP);
    PASS();
}

TEST(lang_ext_csv) {
    ASSERT_EQ(cbm_language_for_extension(".csv"), CBM_LANG_CSV);
    PASS();
}

TEST(lang_ext_hlsl) {
    ASSERT_EQ(cbm_language_for_extension(".hlsl"), CBM_LANG_HLSL);
    ASSERT_EQ(cbm_language_for_extension(".hlsli"), CBM_LANG_HLSL);
    ASSERT_EQ(cbm_language_for_extension(".fx"), CBM_LANG_HLSL);
    PASS();
}

TEST(lang_ext_vhdl) {
    ASSERT_EQ(cbm_language_for_extension(".vhd"), CBM_LANG_VHDL);
    ASSERT_EQ(cbm_language_for_extension(".vhdl"), CBM_LANG_VHDL);
    PASS();
}

TEST(lang_ext_devicetree) {
    ASSERT_EQ(cbm_language_for_extension(".dts"), CBM_LANG_DEVICETREE);
    ASSERT_EQ(cbm_language_for_extension(".dtsi"), CBM_LANG_DEVICETREE);
    ASSERT_EQ(cbm_language_for_extension(".overlay"), CBM_LANG_DEVICETREE);
    PASS();
}

TEST(lang_ext_linkerscript) {
    ASSERT_EQ(cbm_language_for_extension(".ld"), CBM_LANG_LINKERSCRIPT);
    ASSERT_EQ(cbm_language_for_extension(".lds"), CBM_LANG_LINKERSCRIPT);
    PASS();
}

TEST(lang_ext_gn) {
    ASSERT_EQ(cbm_language_for_extension(".gn"), CBM_LANG_GN);
    ASSERT_EQ(cbm_language_for_extension(".gni"), CBM_LANG_GN);
    PASS();
}

TEST(lang_ext_bitbake) {
    ASSERT_EQ(cbm_language_for_extension(".bb"), CBM_LANG_BITBAKE);
    ASSERT_EQ(cbm_language_for_extension(".bbclass"), CBM_LANG_BITBAKE);
    ASSERT_EQ(cbm_language_for_extension(".bbappend"), CBM_LANG_BITBAKE);
    PASS();
}

TEST(lang_ext_smali) {
    ASSERT_EQ(cbm_language_for_extension(".smali"), CBM_LANG_SMALI);
    PASS();
}

TEST(lang_ext_tablegen) {
    ASSERT_EQ(cbm_language_for_extension(".td"), CBM_LANG_TABLEGEN);
    PASS();
}

TEST(lang_ext_ispc) {
    ASSERT_EQ(cbm_language_for_extension(".ispc"), CBM_LANG_ISPC);
    PASS();
}

TEST(lang_ext_cairo) {
    ASSERT_EQ(cbm_language_for_extension(".cairo"), CBM_LANG_CAIRO);
    PASS();
}

TEST(lang_ext_move) {
    ASSERT_EQ(cbm_language_for_extension(".move"), CBM_LANG_MOVE);
    PASS();
}

TEST(lang_ext_squirrel) {
    ASSERT_EQ(cbm_language_for_extension(".nut"), CBM_LANG_SQUIRREL);
    PASS();
}

TEST(lang_ext_func) {
    ASSERT_EQ(cbm_language_for_extension(".fc"), CBM_LANG_FUNC);
    PASS();
}

TEST(lang_ext_rst) {
    ASSERT_EQ(cbm_language_for_extension(".rst"), CBM_LANG_RST);
    PASS();
}

TEST(lang_ext_beancount) {
    ASSERT_EQ(cbm_language_for_extension(".beancount"), CBM_LANG_BEANCOUNT);
    PASS();
}

TEST(lang_ext_mermaid) {
    ASSERT_EQ(cbm_language_for_extension(".mmd"), CBM_LANG_MERMAID);
    ASSERT_EQ(cbm_language_for_extension(".mermaid"), CBM_LANG_MERMAID);
    PASS();
}

TEST(lang_ext_puppet) {
    ASSERT_EQ(cbm_language_for_extension(".pp"), CBM_LANG_PUPPET);
    PASS();
}

TEST(lang_ext_po) {
    ASSERT_EQ(cbm_language_for_extension(".po"), CBM_LANG_PO);
    ASSERT_EQ(cbm_language_for_extension(".pot"), CBM_LANG_PO);
    PASS();
}

TEST(lang_ext_slang) {
    ASSERT_EQ(cbm_language_for_extension(".slang"), CBM_LANG_SLANG);
    PASS();
}

TEST(lang_ext_llvm) {
    ASSERT_EQ(cbm_language_for_extension(".ll"), CBM_LANG_LLVM_IR);
    PASS();
}

TEST(lang_ext_smithy) {
    ASSERT_EQ(cbm_language_for_extension(".smithy"), CBM_LANG_SMITHY);
    PASS();
}

TEST(lang_ext_wit) {
    ASSERT_EQ(cbm_language_for_extension(".wit"), CBM_LANG_WIT);
    PASS();
}

TEST(lang_ext_tlaplus) {
    ASSERT_EQ(cbm_language_for_extension(".tla"), CBM_LANG_TLAPLUS);
    PASS();
}

TEST(lang_ext_pkl) {
    ASSERT_EQ(cbm_language_for_extension(".pkl"), CBM_LANG_PKL);
    PASS();
}

TEST(lang_ext_apex) {
    ASSERT_EQ(cbm_language_for_extension(".cls"), CBM_LANG_APEX);
    ASSERT_EQ(cbm_language_for_extension(".trigger"), CBM_LANG_APEX);
    PASS();
}

TEST(lang_ext_soql) {
    ASSERT_EQ(cbm_language_for_extension(".soql"), CBM_LANG_SOQL);
    PASS();
}

TEST(lang_ext_sosl) {
    ASSERT_EQ(cbm_language_for_extension(".sosl"), CBM_LANG_SOSL);
    PASS();
}

/* --- Ported from lang_test.go: TestForLanguage --- */
TEST(lang_all_have_names) {
    /* Every language enum value from 0 to CBM_LANG_COUNT-1
     * should have a non-"Unknown" name. */
    for (int i = 0; i < CBM_LANG_COUNT; i++) {
        const char *name = cbm_language_name((CBMLanguage)i);
        ASSERT_NOT_NULL(name);
        ASSERT_TRUE(strcmp(name, "Unknown") != 0);
    }
    PASS();
}

/* Tier 1 is exactly the languages CBM shares with octacity-org/badbox. */
TEST(lang_tier1_matches_badbox) {
    static const CBMLanguage TIER1[] = {
        CBM_LANG_BASH,       CBM_LANG_C,      CBM_LANG_CPP,        CBM_LANG_CSHARP,
        CBM_LANG_CSS,        CBM_LANG_DART,   CBM_LANG_ELIXIR,     CBM_LANG_GO,
        CBM_LANG_HASKELL,    CBM_LANG_HCL,    CBM_LANG_HTML,       CBM_LANG_JAVA,
        CBM_LANG_JAVASCRIPT, CBM_LANG_JSON,   CBM_LANG_KOTLIN,     CBM_LANG_LUA,
        CBM_LANG_MARKDOWN,   CBM_LANG_NIX,    CBM_LANG_PHP,        CBM_LANG_PYTHON,
        CBM_LANG_RUBY,       CBM_LANG_RUST,   CBM_LANG_SCALA,      CBM_LANG_SOLIDITY,
        CBM_LANG_SWIFT,      CBM_LANG_TSX,    CBM_LANG_TYPESCRIPT, CBM_LANG_YAML,
        CBM_LANG_POWERSHELL, CBM_LANG_ZIG,
    };
    int expected = (int)(sizeof(TIER1) / sizeof(TIER1[0]));
    for (int i = 0; i < expected; i++) {
        ASSERT_EQ(cbm_language_tier(TIER1[i]), 1);
    }
    int tier1 = 0;
    for (int i = 0; i < CBM_LANG_COUNT; i++) {
        int tier = cbm_language_tier((CBMLanguage)i);
        ASSERT_TRUE(tier >= 1 && tier <= 3);
        tier1 += tier == 1;
    }
    ASSERT_EQ(tier1, expected);
    ASSERT_EQ(cbm_language_tier(CBM_LANG_OCAML), 2);
    ASSERT_EQ(cbm_language_tier(CBM_LANG_COBOL), 3);
    ASSERT_EQ(cbm_language_tier(CBM_LANG_COUNT), 0);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────── */

SUITE(language) {
    /* Extension: Tier 1 programming */
    RUN_TEST(lang_ext_go);
    RUN_TEST(lang_ext_python);
    RUN_TEST(lang_ext_javascript);
    RUN_TEST(lang_ext_jsx);
    RUN_TEST(lang_ext_mjs_cjs);
    RUN_TEST(lang_ext_mts_cts);
    RUN_TEST(lang_ext_typescript);
    RUN_TEST(lang_ext_tsx);
    RUN_TEST(lang_ext_rust);
    RUN_TEST(lang_ext_java);
    RUN_TEST(lang_ext_cpp);
    RUN_TEST(lang_ext_hpp);
    RUN_TEST(lang_ext_cc);
    RUN_TEST(lang_ext_cxx);
    RUN_TEST(lang_ext_hxx);
    RUN_TEST(lang_ext_hh);
    RUN_TEST(lang_ext_h);
    RUN_TEST(lang_ext_ixx);
    RUN_TEST(lang_ext_csharp);
    RUN_TEST(lang_ext_php);
    RUN_TEST(lang_ext_lua);
    RUN_TEST(lang_ext_scala);
    RUN_TEST(lang_ext_sc);
    RUN_TEST(lang_ext_kotlin);
    RUN_TEST(lang_ext_kts);
    RUN_TEST(lang_ext_ruby);
    RUN_TEST(lang_ext_rake);
    RUN_TEST(lang_ext_gemspec);
    RUN_TEST(lang_ext_c);
    RUN_TEST(lang_ext_bash);
    RUN_TEST(lang_ext_bash2);
    RUN_TEST(lang_ext_zig);
    RUN_TEST(lang_ext_elixir);
    RUN_TEST(lang_ext_exs);
    RUN_TEST(lang_ext_haskell);
    RUN_TEST_LANG(CBM_LANG_OCAML, lang_ext_ocaml);
    RUN_TEST_LANG(CBM_LANG_OCAML, lang_ext_mli);
    RUN_TEST(lang_ext_swift);
    RUN_TEST(lang_ext_dart);
    RUN_TEST_LANG(CBM_LANG_PERL, lang_ext_perl);
    RUN_TEST_LANG(CBM_LANG_PERL, lang_ext_pm);
    RUN_TEST_LANG(CBM_LANG_GROOVY, lang_ext_groovy);
    RUN_TEST_LANG(CBM_LANG_GROOVY, lang_ext_gradle);
    RUN_TEST_LANG(CBM_LANG_ERLANG, lang_ext_erlang);
    RUN_TEST_LANG(CBM_LANG_R, lang_ext_r);
    RUN_TEST_LANG(CBM_LANG_R, lang_ext_R);

    /* Extension: Tier 2 programming */
    RUN_TEST_LANG(CBM_LANG_CLOJURE, lang_ext_clojure);
    RUN_TEST_LANG(CBM_LANG_CLOJURE, lang_ext_cljs);
    RUN_TEST_LANG(CBM_LANG_CLOJURE, lang_ext_cljc);
    RUN_TEST_LANG(CBM_LANG_FSHARP, lang_ext_fsharp);
    RUN_TEST_LANG(CBM_LANG_FSHARP, lang_ext_fsi);
    RUN_TEST_LANG(CBM_LANG_FSHARP, lang_ext_fsx);
    RUN_TEST_LANG(CBM_LANG_JULIA, lang_ext_julia);
    RUN_TEST_LANG(CBM_LANG_VIMSCRIPT, lang_ext_vim);
    RUN_TEST(lang_ext_nix);
    RUN_TEST_LANG(CBM_LANG_COMMONLISP, lang_ext_commonlisp);
    RUN_TEST_LANG(CBM_LANG_COMMONLISP, lang_ext_lsp);
    RUN_TEST_LANG(CBM_LANG_COMMONLISP, lang_ext_cl);
    RUN_TEST_LANG(CBM_LANG_ELM, lang_ext_elm);
    RUN_TEST_LANG(CBM_LANG_FORTRAN, lang_ext_fortran);
    RUN_TEST_LANG(CBM_LANG_FORTRAN, lang_ext_f95);
    RUN_TEST_LANG(CBM_LANG_FORTRAN, lang_ext_f03);
    RUN_TEST_LANG(CBM_LANG_FORTRAN, lang_ext_f08);
    RUN_TEST_LANG(CBM_LANG_CUDA, lang_ext_cuda);
    RUN_TEST_LANG(CBM_LANG_CUDA, lang_ext_cuh);
    RUN_TEST_LANG(CBM_LANG_COBOL, lang_ext_cobol);
    RUN_TEST_LANG(CBM_LANG_COBOL, lang_ext_cbl);
    RUN_TEST_LANG(CBM_LANG_VERILOG, lang_ext_verilog);
    RUN_TEST_LANG(CBM_LANG_VERILOG, lang_ext_sv);
    RUN_TEST_LANG(CBM_LANG_EMACSLISP, lang_ext_emacslisp);

    /* Extension: Scientific/math */
    RUN_TEST_LANG(CBM_LANG_MATLAB, lang_ext_matlab);
    RUN_TEST_LANG(CBM_LANG_MATLAB, lang_ext_mlx);
    RUN_TEST_LANG(CBM_LANG_LEAN, lang_ext_lean);
    RUN_TEST_LANG(CBM_LANG_FORM, lang_ext_form);
    RUN_TEST_LANG(CBM_LANG_FORM, lang_ext_prc);
    RUN_TEST_LANG(CBM_LANG_MAGMA, lang_ext_magma);
    RUN_TEST_LANG(CBM_LANG_MAGMA, lang_ext_magma2);
    RUN_TEST_LANG(CBM_LANG_WOLFRAM, lang_ext_wolfram);
    RUN_TEST_LANG(CBM_LANG_WOLFRAM, lang_ext_wls);

    /* Extension: Helper languages */
    RUN_TEST(lang_ext_html);
    RUN_TEST(lang_ext_htm);
    RUN_TEST(lang_ext_css);
    RUN_TEST_LANG(CBM_LANG_SCSS, lang_ext_scss);
    RUN_TEST(lang_ext_yaml);
    RUN_TEST(lang_ext_yaml2);
    RUN_TEST_LANG(CBM_LANG_TOML, lang_ext_toml);
    RUN_TEST(lang_ext_hcl);
    RUN_TEST(lang_ext_hcl2);
    RUN_TEST_LANG(CBM_LANG_SQL, lang_ext_sql);
    RUN_TEST_LANG(CBM_LANG_DOCKERFILE, lang_ext_dockerfile);
    RUN_TEST(lang_ext_json);
    RUN_TEST_LANG(CBM_LANG_XML, lang_ext_xml);
    RUN_TEST_LANG(CBM_LANG_XML, lang_ext_xsl);
    RUN_TEST_LANG(CBM_LANG_XML, lang_ext_xsd);
    RUN_TEST_LANG(CBM_LANG_XML, lang_ext_svg);
    RUN_TEST(lang_ext_markdown);
    RUN_TEST(lang_ext_mdx);
    RUN_TEST_LANG(CBM_LANG_MAKEFILE, lang_ext_makefile);
    RUN_TEST_LANG(CBM_LANG_CMAKE, lang_ext_cmake);
    RUN_TEST_LANG(CBM_LANG_PROTOBUF, lang_ext_protobuf);
    RUN_TEST_LANG(CBM_LANG_GRAPHQL, lang_ext_graphql);
    RUN_TEST_LANG(CBM_LANG_GRAPHQL, lang_ext_gql);
    RUN_TEST_LANG(CBM_LANG_VUE, lang_ext_vue);
    RUN_TEST_LANG(CBM_LANG_SVELTE, lang_ext_svelte);
    RUN_TEST_LANG(CBM_LANG_MESON, lang_ext_meson);
    RUN_TEST_LANG(CBM_LANG_GLSL, lang_ext_glsl);
    RUN_TEST_LANG(CBM_LANG_GLSL, lang_ext_vert);
    RUN_TEST_LANG(CBM_LANG_GLSL, lang_ext_frag);
    RUN_TEST_LANG(CBM_LANG_INI, lang_ext_ini);
    RUN_TEST_LANG(CBM_LANG_INI, lang_ext_cfg);
    RUN_TEST_LANG(CBM_LANG_INI, lang_ext_conf);

    /* Unknown/edge cases */
    RUN_TEST(lang_ext_unknown);
    RUN_TEST(lang_ext_null);

    /* Filename-based */
    RUN_TEST_LANG(CBM_LANG_MAKEFILE, lang_fn_makefile);
    RUN_TEST_LANG(CBM_LANG_MAKEFILE, lang_fn_gnumakefile);
    RUN_TEST_LANG(CBM_LANG_MAKEFILE, lang_fn_makefile_lower);
    RUN_TEST_LANG(CBM_LANG_CMAKE, lang_fn_cmake);
    RUN_TEST_LANG(CBM_LANG_DOCKERFILE, lang_fn_dockerfile);
    RUN_TEST_LANG(CBM_LANG_MESON, lang_fn_meson_build);
    RUN_TEST_LANG(CBM_LANG_MESON, lang_fn_meson_opts);
    RUN_TEST_LANG(CBM_LANG_MESON, lang_fn_meson_opts_txt);
    RUN_TEST_LANG(CBM_LANG_VIMSCRIPT, lang_fn_vimrc);
    RUN_TEST_LANG(CBM_LANG_BLADE, lang_fn_blade_php_compound_issue258);
    RUN_TEST(lang_fn_main_go);
    RUN_TEST(lang_fn_test_py);
    RUN_TEST(lang_fn_unknown);

    /* Language names */
    RUN_TEST(lang_name_go);
    RUN_TEST(lang_name_python);
    RUN_TEST(lang_name_cpp);
    RUN_TEST(lang_name_csharp);
    RUN_TEST(lang_name_unknown);

    /* .m disambiguation */
    RUN_TEST_LANG(CBM_LANG_MATLAB, lang_m_objc);
    RUN_TEST_LANG(CBM_LANG_MATLAB, lang_m_magma);
    RUN_TEST_LANG(CBM_LANG_MATLAB, lang_m_matlab);
    RUN_TEST_LANG(CBM_LANG_MATLAB, lang_m_default_on_read_fail);
    RUN_TEST_LANG(CBM_LANG_APEX, lang_cls_vb6_class_module_unsupported);
    RUN_TEST_LANG(CBM_LANG_APEX, lang_cls_apex_stays_apex);
    RUN_TEST_LANG(CBM_LANG_FORM, lang_frm_vb6_form_unsupported);
    RUN_TEST_LANG(CBM_LANG_FORM, lang_frm_form_stays_form);
    RUN_TEST_LANG(CBM_LANG_RESCRIPT, lang_res_binary_resource_unsupported);
    RUN_TEST_LANG(CBM_LANG_RESCRIPT, lang_res_rescript_stays_rescript);

    /* Go test ports */
    /* New languages */
    RUN_TEST(lang_ext_solidity);
    RUN_TEST_LANG(CBM_LANG_TYPST, lang_ext_typst);
    RUN_TEST_LANG(CBM_LANG_GDSCRIPT, lang_ext_gdscript);
    RUN_TEST_LANG(CBM_LANG_GLEAM, lang_ext_gleam);
    RUN_TEST(lang_ext_powershell);
    RUN_TEST_LANG(CBM_LANG_PASCAL, lang_ext_pascal);
    RUN_TEST_LANG(CBM_LANG_DLANG, lang_ext_d);
    RUN_TEST_LANG(CBM_LANG_NIM, lang_ext_nim);
    RUN_TEST_LANG(CBM_LANG_SCHEME, lang_ext_scheme);
    RUN_TEST_LANG(CBM_LANG_FENNEL, lang_ext_fennel);
    RUN_TEST_LANG(CBM_LANG_FISH, lang_ext_fish);
    RUN_TEST_LANG(CBM_LANG_AWK, lang_ext_awk);
    RUN_TEST_LANG(CBM_LANG_ZSH, lang_ext_zsh);
    RUN_TEST_LANG(CBM_LANG_TCL, lang_ext_tcl);
    RUN_TEST_LANG(CBM_LANG_ADA, lang_ext_ada);
    RUN_TEST_LANG(CBM_LANG_AGDA, lang_ext_agda);
    RUN_TEST_LANG(CBM_LANG_RACKET, lang_ext_racket);
    RUN_TEST_LANG(CBM_LANG_ODIN, lang_ext_odin);
    RUN_TEST_LANG(CBM_LANG_RESCRIPT, lang_ext_rescript);
    RUN_TEST_LANG(CBM_LANG_PURESCRIPT, lang_ext_purescript);
    RUN_TEST_LANG(CBM_LANG_NICKEL, lang_ext_nickel);
    RUN_TEST_LANG(CBM_LANG_CRYSTAL, lang_ext_crystal);
    RUN_TEST_LANG(CBM_LANG_TEAL, lang_ext_teal);
    RUN_TEST_LANG(CBM_LANG_HARE, lang_ext_hare);
    RUN_TEST_LANG(CBM_LANG_PONY, lang_ext_pony);
    RUN_TEST_LANG(CBM_LANG_LUAU, lang_ext_luau);
    RUN_TEST_LANG(CBM_LANG_QML, lang_ext_qml);
    RUN_TEST_LANG(CBM_LANG_CFML, lang_ext_cfml);
    RUN_TEST_LANG(CBM_LANG_GOTEMPLATE, lang_ext_helm_tpl);
    RUN_TEST_LANG(CBM_LANG_JANET, lang_ext_janet);
    RUN_TEST_LANG(CBM_LANG_SWAY, lang_ext_sway);
    RUN_TEST_LANG(CBM_LANG_NASM, lang_ext_nasm);
    RUN_TEST_LANG(CBM_LANG_ASSEMBLY, lang_ext_assembly);
    RUN_TEST_LANG(CBM_LANG_ASTRO, lang_ext_astro);
    RUN_TEST_LANG(CBM_LANG_GOTEMPLATE, lang_ext_gotemplate);
    RUN_TEST_LANG(CBM_LANG_TEMPL, lang_ext_templ);
    RUN_TEST_LANG(CBM_LANG_LIQUID, lang_ext_liquid);
    RUN_TEST_LANG(CBM_LANG_JINJA2, lang_ext_jinja2);
    RUN_TEST_LANG(CBM_LANG_PRISMA, lang_ext_prisma);
    RUN_TEST_LANG(CBM_LANG_HYPRLANG, lang_ext_hyprlang);
    RUN_TEST_LANG(CBM_LANG_DIFF, lang_ext_diff);
    RUN_TEST_LANG(CBM_LANG_WGSL, lang_ext_wgsl);
    RUN_TEST_LANG(CBM_LANG_KDL, lang_ext_kdl);
    RUN_TEST_LANG(CBM_LANG_JSON5, lang_ext_json5);
    RUN_TEST_LANG(CBM_LANG_JSONNET, lang_ext_jsonnet);
    RUN_TEST_LANG(CBM_LANG_RON, lang_ext_ron);
    RUN_TEST_LANG(CBM_LANG_THRIFT, lang_ext_thrift);
    RUN_TEST_LANG(CBM_LANG_CAPNP, lang_ext_capnp);
    RUN_TEST_LANG(CBM_LANG_PROPERTIES, lang_ext_properties);
    RUN_TEST_LANG(CBM_LANG_BIBTEX, lang_ext_bibtex);
    RUN_TEST_LANG(CBM_LANG_STARLARK, lang_ext_starlark);
    RUN_TEST_LANG(CBM_LANG_BICEP, lang_ext_bicep);
    RUN_TEST_LANG(CBM_LANG_CSV, lang_ext_csv);
    RUN_TEST_LANG(CBM_LANG_HLSL, lang_ext_hlsl);
    RUN_TEST_LANG(CBM_LANG_VHDL, lang_ext_vhdl);
    RUN_TEST_LANG(CBM_LANG_DEVICETREE, lang_ext_devicetree);
    RUN_TEST_LANG(CBM_LANG_LINKERSCRIPT, lang_ext_linkerscript);
    RUN_TEST_LANG(CBM_LANG_GN, lang_ext_gn);
    RUN_TEST_LANG(CBM_LANG_BITBAKE, lang_ext_bitbake);
    RUN_TEST_LANG(CBM_LANG_SMALI, lang_ext_smali);
    RUN_TEST_LANG(CBM_LANG_TABLEGEN, lang_ext_tablegen);
    RUN_TEST_LANG(CBM_LANG_ISPC, lang_ext_ispc);
    RUN_TEST_LANG(CBM_LANG_CAIRO, lang_ext_cairo);
    RUN_TEST_LANG(CBM_LANG_MOVE, lang_ext_move);
    RUN_TEST_LANG(CBM_LANG_SQUIRREL, lang_ext_squirrel);
    RUN_TEST_LANG(CBM_LANG_FUNC, lang_ext_func);
    RUN_TEST_LANG(CBM_LANG_RST, lang_ext_rst);
    RUN_TEST_LANG(CBM_LANG_BEANCOUNT, lang_ext_beancount);
    RUN_TEST_LANG(CBM_LANG_MERMAID, lang_ext_mermaid);
    RUN_TEST_LANG(CBM_LANG_PUPPET, lang_ext_puppet);
    RUN_TEST_LANG(CBM_LANG_PO, lang_ext_po);
    RUN_TEST_LANG(CBM_LANG_SLANG, lang_ext_slang);
    RUN_TEST_LANG(CBM_LANG_LLVM_IR, lang_ext_llvm);
    RUN_TEST_LANG(CBM_LANG_SMITHY, lang_ext_smithy);
    RUN_TEST_LANG(CBM_LANG_WIT, lang_ext_wit);
    RUN_TEST_LANG(CBM_LANG_TLAPLUS, lang_ext_tlaplus);
    RUN_TEST_LANG(CBM_LANG_PKL, lang_ext_pkl);
    RUN_TEST_LANG(CBM_LANG_APEX, lang_ext_apex);
    RUN_TEST_LANG(CBM_LANG_SOQL, lang_ext_soql);
    RUN_TEST_LANG(CBM_LANG_SOSL, lang_ext_sosl);

    RUN_TEST(lang_all_have_names);
    RUN_TEST(lang_tier1_matches_badbox);
}
