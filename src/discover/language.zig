//! language.zig — Language detection from filename and extension.
//!
//! Implements the language part of src/discover/discover.h.
//!
//! Maps file extensions and special filenames to CBMLanguage values.
//! Handles .m disambiguation (Objective-C vs Magma vs MATLAB) and .cls / .frm
//! disambiguation (Visual Basic 6 vs Apex / FORM). Consults the process-global
//! user config (set via cbm_set_user_lang_config) before the built-in table.

const std = @import("std");
const c = @import("c");
const userconfig = @import("userconfig.zig");

const Str = [*:0]const u8;
const Lang = c.CBMLanguage;

const LANG_UNKNOWN: Lang = c.CBM_LANG_COUNT;

/// A (key, language) table row.
const Entry = struct { []const u8, Lang };

// ── Extension → Language lookup table ──────────────────────────────

/// Extension including the dot. Scanned linearly: the first match wins.
const EXT_TABLE = [_]Entry{
    // Bash
    .{ ".bash", c.CBM_LANG_BASH },
    .{ ".sh", c.CBM_LANG_BASH },

    // C
    .{ ".c", c.CBM_LANG_C },

    // C++
    .{ ".cc", c.CBM_LANG_CPP },
    .{ ".ccm", c.CBM_LANG_CPP },
    .{ ".cpp", c.CBM_LANG_CPP },
    .{ ".cppm", c.CBM_LANG_CPP },
    .{ ".cxx", c.CBM_LANG_CPP },
    .{ ".h", c.CBM_LANG_CPP },
    .{ ".hh", c.CBM_LANG_CPP },
    .{ ".hpp", c.CBM_LANG_CPP },
    .{ ".hxx", c.CBM_LANG_CPP },
    .{ ".ixx", c.CBM_LANG_CPP },

    // C#
    .{ ".cs", c.CBM_LANG_CSHARP },

    // Clojure
    .{ ".clj", c.CBM_LANG_CLOJURE },
    .{ ".cljc", c.CBM_LANG_CLOJURE },
    .{ ".cljs", c.CBM_LANG_CLOJURE },

    // CMake
    .{ ".cmake", c.CBM_LANG_CMAKE },

    // COBOL
    .{ ".cbl", c.CBM_LANG_COBOL },
    .{ ".cob", c.CBM_LANG_COBOL },

    // Common Lisp
    .{ ".cl", c.CBM_LANG_COMMONLISP },
    .{ ".lisp", c.CBM_LANG_COMMONLISP },
    .{ ".lsp", c.CBM_LANG_COMMONLISP },

    // CSS
    .{ ".css", c.CBM_LANG_CSS },

    // CUDA
    .{ ".cu", c.CBM_LANG_CUDA },
    .{ ".cuh", c.CBM_LANG_CUDA },

    // Dart
    .{ ".dart", c.CBM_LANG_DART },

    // Dockerfile
    .{ ".dockerfile", c.CBM_LANG_DOCKERFILE },

    // Elixir
    .{ ".ex", c.CBM_LANG_ELIXIR },
    .{ ".exs", c.CBM_LANG_ELIXIR },

    // DotEnv
    .{ ".env", c.CBM_LANG_DOTENV },

    // Elm
    .{ ".elm", c.CBM_LANG_ELM },

    // Emacs Lisp
    .{ ".el", c.CBM_LANG_EMACSLISP },

    // Erlang
    .{ ".erl", c.CBM_LANG_ERLANG },

    // F#
    .{ ".fs", c.CBM_LANG_FSHARP },
    .{ ".fsi", c.CBM_LANG_FSHARP },
    .{ ".fsx", c.CBM_LANG_FSHARP },

    // FORM
    .{ ".frm", c.CBM_LANG_FORM },
    .{ ".prc", c.CBM_LANG_FORM },

    // Fortran
    .{ ".f03", c.CBM_LANG_FORTRAN },
    .{ ".f08", c.CBM_LANG_FORTRAN },
    .{ ".f90", c.CBM_LANG_FORTRAN },
    .{ ".f95", c.CBM_LANG_FORTRAN },

    // GLSL
    .{ ".frag", c.CBM_LANG_GLSL },
    .{ ".glsl", c.CBM_LANG_GLSL },
    .{ ".vert", c.CBM_LANG_GLSL },

    // Go
    .{ ".go", c.CBM_LANG_GO },

    // GraphQL
    .{ ".gql", c.CBM_LANG_GRAPHQL },
    .{ ".graphql", c.CBM_LANG_GRAPHQL },

    // Groovy
    .{ ".gradle", c.CBM_LANG_GROOVY },
    .{ ".groovy", c.CBM_LANG_GROOVY },

    // Haskell
    .{ ".hs", c.CBM_LANG_HASKELL },

    // HCL / Terraform
    .{ ".hcl", c.CBM_LANG_HCL },
    .{ ".tf", c.CBM_LANG_HCL },

    // HTML
    .{ ".htm", c.CBM_LANG_HTML },
    .{ ".html", c.CBM_LANG_HTML },

    // INI
    .{ ".cfg", c.CBM_LANG_INI },
    .{ ".conf", c.CBM_LANG_INI },
    .{ ".ini", c.CBM_LANG_INI },

    // Java
    .{ ".java", c.CBM_LANG_JAVA },

    // JavaScript
    .{ ".js", c.CBM_LANG_JAVASCRIPT },
    .{ ".jsx", c.CBM_LANG_JAVASCRIPT },
    .{ ".mjs", c.CBM_LANG_JAVASCRIPT }, // ES modules (#197)
    .{ ".cjs", c.CBM_LANG_JAVASCRIPT }, // CommonJS modules

    // JSON
    .{ ".json", c.CBM_LANG_JSON },

    // Julia
    .{ ".jl", c.CBM_LANG_JULIA },

    // Kotlin
    .{ ".kt", c.CBM_LANG_KOTLIN },
    .{ ".kts", c.CBM_LANG_KOTLIN },

    // Lean
    .{ ".lean", c.CBM_LANG_LEAN },

    // Lua
    .{ ".lua", c.CBM_LANG_LUA },

    // Magma
    .{ ".mag", c.CBM_LANG_MAGMA },
    .{ ".magma", c.CBM_LANG_MAGMA },

    // Makefile
    .{ ".mk", c.CBM_LANG_MAKEFILE },

    // Markdown
    .{ ".md", c.CBM_LANG_MARKDOWN },
    .{ ".mdx", c.CBM_LANG_MARKDOWN },

    // MATLAB
    .{ ".m", c.CBM_LANG_MATLAB },
    .{ ".matlab", c.CBM_LANG_MATLAB },
    .{ ".mlx", c.CBM_LANG_MATLAB },

    // Meson
    .{ ".meson", c.CBM_LANG_MESON },

    // Nix
    .{ ".nix", c.CBM_LANG_NIX },

    // OCaml
    .{ ".ml", c.CBM_LANG_OCAML },
    .{ ".mli", c.CBM_LANG_OCAML },

    // Perl
    .{ ".pl", c.CBM_LANG_PERL },
    .{ ".pm", c.CBM_LANG_PERL },

    // PHP
    .{ ".php", c.CBM_LANG_PHP },

    // Protobuf
    .{ ".proto", c.CBM_LANG_PROTOBUF },

    // Python
    .{ ".py", c.CBM_LANG_PYTHON },

    // R — case insensitive handled separately
    .{ ".R", c.CBM_LANG_R },
    .{ ".r", c.CBM_LANG_R },

    // Ruby
    .{ ".gemspec", c.CBM_LANG_RUBY },
    .{ ".rake", c.CBM_LANG_RUBY },
    .{ ".rb", c.CBM_LANG_RUBY },

    // Rust
    .{ ".rs", c.CBM_LANG_RUST },

    // Scala
    .{ ".sc", c.CBM_LANG_SCALA },
    .{ ".scala", c.CBM_LANG_SCALA },

    // SCSS
    .{ ".scss", c.CBM_LANG_SCSS },

    // SQL
    .{ ".sql", c.CBM_LANG_SQL },

    // Svelte
    .{ ".svelte", c.CBM_LANG_SVELTE },

    // Swift
    .{ ".swift", c.CBM_LANG_SWIFT },

    // SystemVerilog + Verilog
    .{ ".sv", c.CBM_LANG_VERILOG },
    .{ ".v", c.CBM_LANG_VERILOG },

    // TOML
    .{ ".toml", c.CBM_LANG_TOML },

    // TSX
    .{ ".tsx", c.CBM_LANG_TSX },

    // TypeScript
    .{ ".ts", c.CBM_LANG_TYPESCRIPT },
    .{ ".mts", c.CBM_LANG_TYPESCRIPT }, // TS ES modules
    .{ ".cts", c.CBM_LANG_TYPESCRIPT }, // TS CommonJS modules

    // VimScript
    .{ ".vim", c.CBM_LANG_VIMSCRIPT },
    .{ ".vimrc", c.CBM_LANG_VIMSCRIPT },
    .{ "justfile", c.CBM_LANG_JUST },
    .{ "Justfile", c.CBM_LANG_JUST },
    .{ ".justfile", c.CBM_LANG_JUST },
    .{ ".just", c.CBM_LANG_JUST }, // `import 'common.just'` target files
    .{ "hyprland.conf", c.CBM_LANG_HYPRLANG },
    .{ "ssh_config", c.CBM_LANG_SSHCONFIG },
    .{ "sshd_config", c.CBM_LANG_SSHCONFIG },
    .{ "BUILD", c.CBM_LANG_STARLARK },
    .{ "BUILD.bazel", c.CBM_LANG_STARLARK },
    .{ "WORKSPACE", c.CBM_LANG_STARLARK },
    .{ "WORKSPACE.bazel", c.CBM_LANG_STARLARK },

    // BitBake include fragments — `require/include foo.inc` target files.
    .{ ".inc", c.CBM_LANG_BITBAKE },

    // Vue
    .{ ".vue", c.CBM_LANG_VUE },

    // Wolfram
    .{ ".wl", c.CBM_LANG_WOLFRAM },
    .{ ".wls", c.CBM_LANG_WOLFRAM },

    // XML
    .{ ".xml", c.CBM_LANG_XML },
    .{ ".xsd", c.CBM_LANG_XML },
    .{ ".xsl", c.CBM_LANG_XML },
    .{ ".svg", c.CBM_LANG_XML },

    // YAML
    .{ ".yaml", c.CBM_LANG_YAML },
    .{ ".yml", c.CBM_LANG_YAML },

    // Ada
    .{ ".adb", c.CBM_LANG_ADA },

    // Ada
    .{ ".ads", c.CBM_LANG_ADA },

    // Agda
    .{ ".agda", c.CBM_LANG_AGDA },

    // Astro
    .{ ".astro", c.CBM_LANG_ASTRO },

    // AWK
    .{ ".awk", c.CBM_LANG_AWK },

    // BitBake
    .{ ".bb", c.CBM_LANG_BITBAKE },

    // BitBake
    .{ ".bbappend", c.CBM_LANG_BITBAKE },

    // BitBake
    .{ ".bbclass", c.CBM_LANG_BITBAKE },

    // Beancount
    .{ ".beancount", c.CBM_LANG_BEANCOUNT },

    // BibTeX
    .{ ".bib", c.CBM_LANG_BIBTEX },

    // Bicep
    .{ ".bicep", c.CBM_LANG_BICEP },

    // Blade
    // .blade.php handled by userconfig compound extensions, not EXT_TABLE

    // Starlark
    .{ ".bzl", c.CBM_LANG_STARLARK },

    // Cairo
    .{ ".cairo", c.CBM_LANG_CAIRO },

    // Cap'n Proto
    .{ ".capnp", c.CBM_LANG_CAPNP },

    // Apex
    .{ ".cls", c.CBM_LANG_APEX },

    // Crystal
    .{ ".cr", c.CBM_LANG_CRYSTAL },

    // CSV
    .{ ".csv", c.CBM_LANG_CSV },

    // D
    .{ ".d", c.CBM_LANG_DLANG },

    // Diff
    .{ ".diff", c.CBM_LANG_DIFF },

    // Pascal
    .{ ".dpr", c.CBM_LANG_PASCAL },

    // DeviceTree
    .{ ".dts", c.CBM_LANG_DEVICETREE },

    // DeviceTree
    .{ ".dtsi", c.CBM_LANG_DEVICETREE },

    // FunC
    .{ ".fc", c.CBM_LANG_FUNC },

    // Fish
    .{ ".fish", c.CBM_LANG_FISH },

    // Fennel
    .{ ".fnl", c.CBM_LANG_FENNEL },

    // HLSL
    .{ ".fx", c.CBM_LANG_HLSL },

    // GDScript
    .{ ".gd", c.CBM_LANG_GDSCRIPT },

    // Gleam
    .{ ".gleam", c.CBM_LANG_GLEAM },

    // GN
    .{ ".gn", c.CBM_LANG_GN },

    // GN
    .{ ".gni", c.CBM_LANG_GN },

    // Go Template
    .{ ".gotmpl", c.CBM_LANG_GOTEMPLATE },
    .{ ".tpl", c.CBM_LANG_GOTEMPLATE }, // Helm _helpers.tpl named-template definitions

    // Hare
    .{ ".ha", c.CBM_LANG_HARE },

    // Hyprlang
    .{ ".hl", c.CBM_LANG_HYPRLANG },

    // HLSL
    .{ ".hlsl", c.CBM_LANG_HLSL },

    // HLSL
    .{ ".hlsli", c.CBM_LANG_HLSL },

    // ISPC
    .{ ".ispc", c.CBM_LANG_ISPC },

    // Jinja2
    .{ ".j2", c.CBM_LANG_JINJA2 },

    // Janet
    .{ ".janet", c.CBM_LANG_JANET },

    // Jinja2
    .{ ".jinja", c.CBM_LANG_JINJA2 },

    // Jinja2
    .{ ".jinja2", c.CBM_LANG_JINJA2 },

    // JSON5
    .{ ".json5", c.CBM_LANG_JSON5 },

    // Jsonnet
    .{ ".jsonnet", c.CBM_LANG_JSONNET },

    // KDL
    .{ ".kdl", c.CBM_LANG_KDL },

    // Linker Script
    .{ ".ld", c.CBM_LANG_LINKERSCRIPT },

    // Linker Script
    .{ ".lds", c.CBM_LANG_LINKERSCRIPT },

    // Jsonnet
    .{ ".libsonnet", c.CBM_LANG_JSONNET },

    // Liquid
    .{ ".liquid", c.CBM_LANG_LIQUID },

    // LLVM IR
    .{ ".ll", c.CBM_LANG_LLVM_IR },

    // Pascal
    .{ ".lpr", c.CBM_LANG_PASCAL },

    // Luau
    .{ ".luau", c.CBM_LANG_LUAU },

    // Qt QML
    .{ ".qml", c.CBM_LANG_QML },

    // CFML / ColdFusion — .cfc components are script-dialect; .cfm are tag templates
    .{ ".cfc", c.CBM_LANG_CFSCRIPT },
    .{ ".cfm", c.CBM_LANG_CFML },

    // Mermaid
    .{ ".mermaid", c.CBM_LANG_MERMAID },

    // Mermaid
    .{ ".mmd", c.CBM_LANG_MERMAID },

    // Move
    .{ ".move", c.CBM_LANG_MOVE },

    // NASM
    .{ ".nasm", c.CBM_LANG_NASM },

    // Nickel
    .{ ".ncl", c.CBM_LANG_NICKEL },

    // Nim

    // Nim

    // Squirrel
    .{ ".nut", c.CBM_LANG_SQUIRREL },

    // Odin
    .{ ".odin", c.CBM_LANG_ODIN },

    // DeviceTree
    .{ ".overlay", c.CBM_LANG_DEVICETREE },

    // Pascal
    .{ ".pas", c.CBM_LANG_PASCAL },

    // Diff
    .{ ".patch", c.CBM_LANG_DIFF },

    // Pine Script
    .{ ".pine", c.CBM_LANG_PINE },

    // Pkl
    .{ ".pkl", c.CBM_LANG_PKL },

    // PO
    .{ ".po", c.CBM_LANG_PO },

    // Pony
    .{ ".pony", c.CBM_LANG_PONY },

    // PO
    .{ ".pot", c.CBM_LANG_PO },

    // Puppet
    .{ ".pp", c.CBM_LANG_PUPPET },

    // Prisma
    .{ ".prisma", c.CBM_LANG_PRISMA },

    // Properties
    .{ ".properties", c.CBM_LANG_PROPERTIES },

    // PowerShell
    .{ ".ps1", c.CBM_LANG_POWERSHELL },

    // PowerShell
    .{ ".psd1", c.CBM_LANG_POWERSHELL },

    // PowerShell
    .{ ".psm1", c.CBM_LANG_POWERSHELL },

    // PureScript
    .{ ".purs", c.CBM_LANG_PURESCRIPT },

    // ReScript
    .{ ".res", c.CBM_LANG_RESCRIPT },

    // ReScript
    .{ ".resi", c.CBM_LANG_RESCRIPT },

    // Regex
    .{ ".re", c.CBM_LANG_REGEX },

    // Racket
    .{ ".rkt", c.CBM_LANG_RACKET },

    // RON
    .{ ".ron", c.CBM_LANG_RON },

    // reStructuredText
    .{ ".rst", c.CBM_LANG_RST },

    // Assembly
    .{ ".s", c.CBM_LANG_ASSEMBLY },

    // Assembly
    .{ ".S", c.CBM_LANG_ASSEMBLY },

    // Scheme
    .{ ".scm", c.CBM_LANG_SCHEME },

    // Slang
    .{ ".slang", c.CBM_LANG_SLANG },

    // Smali
    .{ ".smali", c.CBM_LANG_SMALI },

    // Smithy
    .{ ".smithy", c.CBM_LANG_SMITHY },

    // Solidity
    .{ ".sol", c.CBM_LANG_SOLIDITY },

    // SOQL
    .{ ".soql", c.CBM_LANG_SOQL },

    // SOSL
    .{ ".sosl", c.CBM_LANG_SOSL },

    // Scheme
    .{ ".ss", c.CBM_LANG_SCHEME },

    // Starlark
    .{ ".star", c.CBM_LANG_STARLARK },

    // SystemVerilog

    // SystemVerilog

    // Sway
    .{ ".sw", c.CBM_LANG_SWAY },

    // Tcl
    .{ ".tcl", c.CBM_LANG_TCL },

    // TableGen
    .{ ".td", c.CBM_LANG_TABLEGEN },

    // Templ
    .{ ".templ", c.CBM_LANG_TEMPL },

    // Thrift
    .{ ".thrift", c.CBM_LANG_THRIFT },

    // Teal
    .{ ".tl", c.CBM_LANG_TEAL },

    // TLA+
    .{ ".tla", c.CBM_LANG_TLAPLUS },

    // Go Template
    .{ ".tmpl", c.CBM_LANG_GOTEMPLATE },

    // Apex
    .{ ".trigger", c.CBM_LANG_APEX },

    // Typst
    .{ ".typ", c.CBM_LANG_TYPST },

    // VHDL
    .{ ".vhd", c.CBM_LANG_VHDL },

    // VHDL
    .{ ".vhdl", c.CBM_LANG_VHDL },

    // WGSL
    .{ ".wgsl", c.CBM_LANG_WGSL },

    // WIT
    .{ ".wit", c.CBM_LANG_WIT },

    // Zsh
    .{ ".zsh", c.CBM_LANG_ZSH },

    // Zig
    .{ ".zig", c.CBM_LANG_ZIG },
};

// ── Special filename → Language lookup ─────────────────────────────

const FILENAME_TABLE = [_]Entry{
    .{ "CMakeLists.txt", c.CBM_LANG_CMAKE },
    .{ "Dockerfile", c.CBM_LANG_DOCKERFILE },
    .{ "GNUmakefile", c.CBM_LANG_MAKEFILE },
    .{ "Makefile", c.CBM_LANG_MAKEFILE },
    .{ "makefile", c.CBM_LANG_MAKEFILE },
    .{ "meson.build", c.CBM_LANG_MESON },
    .{ "meson.options", c.CBM_LANG_MESON },
    .{ "meson_options.txt", c.CBM_LANG_MESON },
    .{ "kustomization.yaml", c.CBM_LANG_KUSTOMIZE },
    .{ "kustomization.yml", c.CBM_LANG_KUSTOMIZE },
    // Note: FILENAME_TABLE uses case-sensitive strcmp, so mixed-case variants
    // (e.g. "Kustomization.yaml") are not matched here.  They fall through to
    // CBM_LANG_YAML and are re-classified by cbm_is_kustomize_file() in
    // pass_k8s.c, which performs a case-insensitive comparison.  This is the
    // intended behaviour — no additional entries are needed.
    .{ ".vimrc", c.CBM_LANG_VIMSCRIPT },
    .{ ".zshrc", c.CBM_LANG_ZSH },
    .{ ".zshenv", c.CBM_LANG_ZSH },
    .{ ".zprofile", c.CBM_LANG_ZSH },
    .{ "justfile", c.CBM_LANG_JUST },
    .{ "Justfile", c.CBM_LANG_JUST },
    .{ ".justfile", c.CBM_LANG_JUST },
    .{ "hyprland.conf", c.CBM_LANG_HYPRLANG },
    .{ "ssh_config", c.CBM_LANG_SSHCONFIG },
    .{ "sshd_config", c.CBM_LANG_SSHCONFIG },
    .{ ".ssh/config", c.CBM_LANG_SSHCONFIG },
    .{ "BUILD", c.CBM_LANG_STARLARK },
    .{ "BUILD.bazel", c.CBM_LANG_STARLARK },
    .{ "WORKSPACE", c.CBM_LANG_STARLARK },
    .{ "WORKSPACE.bazel", c.CBM_LANG_STARLARK },
    .{ "requirements.txt", c.CBM_LANG_REQUIREMENTS },
    .{ "requirements-dev.txt", c.CBM_LANG_REQUIREMENTS },
    .{ "requirements-test.txt", c.CBM_LANG_REQUIREMENTS },
    .{ "Kconfig", c.CBM_LANG_KCONFIG },
    .{ "go.mod", c.CBM_LANG_GOMOD },
    .{ ".env", c.CBM_LANG_DOTENV },
    .{ ".env.local", c.CBM_LANG_DOTENV },
    .{ ".gitattributes", c.CBM_LANG_GITATTRIBUTES },
};

/// Built-in compound extensions, probed before the single-extension lookup.
const COMPOUND_EXT_TABLE = [_]Entry{
    .{ ".blade.php", c.CBM_LANG_BLADE },
};

// ── Language names ─────────────────────────────────────────────────

const LANG_NAME_LIST = [_]struct { Lang, Str }{
    .{ c.CBM_LANG_GO, "Go" },
    .{ c.CBM_LANG_PYTHON, "Python" },
    .{ c.CBM_LANG_JAVASCRIPT, "JavaScript" },
    .{ c.CBM_LANG_TYPESCRIPT, "TypeScript" },
    .{ c.CBM_LANG_TSX, "TSX" },
    .{ c.CBM_LANG_RUST, "Rust" },
    .{ c.CBM_LANG_JAVA, "Java" },
    .{ c.CBM_LANG_CPP, "C++" },
    .{ c.CBM_LANG_CSHARP, "C#" },
    .{ c.CBM_LANG_PHP, "PHP" },
    .{ c.CBM_LANG_LUA, "Lua" },
    .{ c.CBM_LANG_SCALA, "Scala" },
    .{ c.CBM_LANG_KOTLIN, "Kotlin" },
    .{ c.CBM_LANG_RUBY, "Ruby" },
    .{ c.CBM_LANG_C, "C" },
    .{ c.CBM_LANG_BASH, "Bash" },
    .{ c.CBM_LANG_ZIG, "Zig" },
    .{ c.CBM_LANG_ELIXIR, "Elixir" },
    .{ c.CBM_LANG_HASKELL, "Haskell" },
    .{ c.CBM_LANG_OCAML, "OCaml" },
    .{ c.CBM_LANG_OBJC, "Objective-C" },
    .{ c.CBM_LANG_SWIFT, "Swift" },
    .{ c.CBM_LANG_DART, "Dart" },
    .{ c.CBM_LANG_PERL, "Perl" },
    .{ c.CBM_LANG_GROOVY, "Groovy" },
    .{ c.CBM_LANG_ERLANG, "Erlang" },
    .{ c.CBM_LANG_R, "R" },
    .{ c.CBM_LANG_HTML, "HTML" },
    .{ c.CBM_LANG_CSS, "CSS" },
    .{ c.CBM_LANG_SCSS, "SCSS" },
    .{ c.CBM_LANG_YAML, "YAML" },
    .{ c.CBM_LANG_TOML, "TOML" },
    .{ c.CBM_LANG_HCL, "HCL" },
    .{ c.CBM_LANG_SQL, "SQL" },
    .{ c.CBM_LANG_DOCKERFILE, "Dockerfile" },
    .{ c.CBM_LANG_CLOJURE, "Clojure" },
    .{ c.CBM_LANG_FSHARP, "F#" },
    .{ c.CBM_LANG_JULIA, "Julia" },
    .{ c.CBM_LANG_VIMSCRIPT, "VimScript" },
    .{ c.CBM_LANG_NIX, "Nix" },
    .{ c.CBM_LANG_COMMONLISP, "Common Lisp" },
    .{ c.CBM_LANG_ELM, "Elm" },
    .{ c.CBM_LANG_FORTRAN, "Fortran" },
    .{ c.CBM_LANG_CUDA, "CUDA" },
    .{ c.CBM_LANG_COBOL, "COBOL" },
    .{ c.CBM_LANG_VERILOG, "Verilog" },
    .{ c.CBM_LANG_EMACSLISP, "Emacs Lisp" },
    .{ c.CBM_LANG_JSON, "JSON" },
    .{ c.CBM_LANG_XML, "XML" },
    .{ c.CBM_LANG_MARKDOWN, "Markdown" },
    .{ c.CBM_LANG_MAKEFILE, "Makefile" },
    .{ c.CBM_LANG_CMAKE, "CMake" },
    .{ c.CBM_LANG_PROTOBUF, "Protobuf" },
    .{ c.CBM_LANG_GRAPHQL, "GraphQL" },
    .{ c.CBM_LANG_VUE, "Vue" },
    .{ c.CBM_LANG_SVELTE, "Svelte" },
    .{ c.CBM_LANG_MESON, "Meson" },
    .{ c.CBM_LANG_GLSL, "GLSL" },
    .{ c.CBM_LANG_INI, "INI" },
    .{ c.CBM_LANG_MATLAB, "MATLAB" },
    .{ c.CBM_LANG_LEAN, "Lean" },
    .{ c.CBM_LANG_FORM, "FORM" },
    .{ c.CBM_LANG_MAGMA, "Magma" },
    .{ c.CBM_LANG_WOLFRAM, "Wolfram" },
    .{ c.CBM_LANG_KUSTOMIZE, "Kustomize" },
    .{ c.CBM_LANG_K8S, "Kubernetes" },
    .{ c.CBM_LANG_PINE, "PineScript" },
    .{ c.CBM_LANG_SOLIDITY, "Solidity" },
    .{ c.CBM_LANG_TYPST, "Typst" },
    .{ c.CBM_LANG_GDSCRIPT, "GDScript" },
    .{ c.CBM_LANG_GLEAM, "Gleam" },
    .{ c.CBM_LANG_POWERSHELL, "PowerShell" },
    .{ c.CBM_LANG_PASCAL, "Pascal" },
    .{ c.CBM_LANG_DLANG, "D" },
    .{ c.CBM_LANG_NIM, "Nim" },
    .{ c.CBM_LANG_SCHEME, "Scheme" },
    .{ c.CBM_LANG_FENNEL, "Fennel" },
    .{ c.CBM_LANG_FISH, "Fish" },
    .{ c.CBM_LANG_AWK, "AWK" },
    .{ c.CBM_LANG_ZSH, "Zsh" },
    .{ c.CBM_LANG_TCL, "Tcl" },
    .{ c.CBM_LANG_ADA, "Ada" },
    .{ c.CBM_LANG_AGDA, "Agda" },
    .{ c.CBM_LANG_RACKET, "Racket" },
    .{ c.CBM_LANG_ODIN, "Odin" },
    .{ c.CBM_LANG_RESCRIPT, "ReScript" },
    .{ c.CBM_LANG_PURESCRIPT, "PureScript" },
    .{ c.CBM_LANG_NICKEL, "Nickel" },
    .{ c.CBM_LANG_CRYSTAL, "Crystal" },
    .{ c.CBM_LANG_TEAL, "Teal" },
    .{ c.CBM_LANG_HARE, "Hare" },
    .{ c.CBM_LANG_PONY, "Pony" },
    .{ c.CBM_LANG_LUAU, "Luau" },
    .{ c.CBM_LANG_QML, "QML" },
    .{ c.CBM_LANG_CFSCRIPT, "CFML" },
    .{ c.CBM_LANG_CFML, "CFML" },
    .{ c.CBM_LANG_JANET, "Janet" },
    .{ c.CBM_LANG_SWAY, "Sway" },
    .{ c.CBM_LANG_NASM, "NASM" },
    .{ c.CBM_LANG_ASSEMBLY, "Assembly" },
    .{ c.CBM_LANG_ASTRO, "Astro" },
    .{ c.CBM_LANG_BLADE, "Blade" },
    .{ c.CBM_LANG_JUST, "Just" },
    .{ c.CBM_LANG_GOTEMPLATE, "Go Template" },
    .{ c.CBM_LANG_TEMPL, "Templ" },
    .{ c.CBM_LANG_LIQUID, "Liquid" },
    .{ c.CBM_LANG_JINJA2, "Jinja2" },
    .{ c.CBM_LANG_PRISMA, "Prisma" },
    .{ c.CBM_LANG_HYPRLANG, "Hyprlang" },
    .{ c.CBM_LANG_DOTENV, "DotEnv" },
    .{ c.CBM_LANG_SYSTEMVERILOG, "SystemVerilog" },
    .{ c.CBM_LANG_DIFF, "Diff" },
    .{ c.CBM_LANG_WGSL, "WGSL" },
    .{ c.CBM_LANG_KDL, "KDL" },
    .{ c.CBM_LANG_JSON5, "JSON5" },
    .{ c.CBM_LANG_JSONNET, "Jsonnet" },
    .{ c.CBM_LANG_RON, "RON" },
    .{ c.CBM_LANG_THRIFT, "Thrift" },
    .{ c.CBM_LANG_CAPNP, "Cap'n Proto" },
    .{ c.CBM_LANG_PROPERTIES, "Properties" },
    .{ c.CBM_LANG_SSHCONFIG, "SSH Config" },
    .{ c.CBM_LANG_BIBTEX, "BibTeX" },
    .{ c.CBM_LANG_STARLARK, "Starlark" },
    .{ c.CBM_LANG_BICEP, "Bicep" },
    .{ c.CBM_LANG_CSV, "CSV" },
    .{ c.CBM_LANG_REQUIREMENTS, "Requirements" },
    .{ c.CBM_LANG_HLSL, "HLSL" },
    .{ c.CBM_LANG_VHDL, "VHDL" },
    .{ c.CBM_LANG_DEVICETREE, "DeviceTree" },
    .{ c.CBM_LANG_LINKERSCRIPT, "Linker Script" },
    .{ c.CBM_LANG_GN, "GN" },
    .{ c.CBM_LANG_KCONFIG, "Kconfig" },
    .{ c.CBM_LANG_BITBAKE, "BitBake" },
    .{ c.CBM_LANG_SMALI, "Smali" },
    .{ c.CBM_LANG_TABLEGEN, "TableGen" },
    .{ c.CBM_LANG_ISPC, "ISPC" },
    .{ c.CBM_LANG_CAIRO, "Cairo" },
    .{ c.CBM_LANG_MOVE, "Move" },
    .{ c.CBM_LANG_SQUIRREL, "Squirrel" },
    .{ c.CBM_LANG_FUNC, "FunC" },
    .{ c.CBM_LANG_REGEX, "Regex" },
    .{ c.CBM_LANG_JSDOC, "JSDoc" },
    .{ c.CBM_LANG_RST, "reStructuredText" },
    .{ c.CBM_LANG_BEANCOUNT, "Beancount" },
    .{ c.CBM_LANG_MERMAID, "Mermaid" },
    .{ c.CBM_LANG_PUPPET, "Puppet" },
    .{ c.CBM_LANG_PO, "PO" },
    .{ c.CBM_LANG_GITATTRIBUTES, "gitattributes" },
    .{ c.CBM_LANG_GITIGNORE, "gitignore" },
    .{ c.CBM_LANG_SLANG, "Slang" },
    .{ c.CBM_LANG_LLVM_IR, "LLVM IR" },
    .{ c.CBM_LANG_SMITHY, "Smithy" },
    .{ c.CBM_LANG_WIT, "WIT" },
    .{ c.CBM_LANG_TLAPLUS, "TLA+" },
    .{ c.CBM_LANG_PKL, "Pkl" },
    .{ c.CBM_LANG_GOMOD, "Go Mod" },
    .{ c.CBM_LANG_APEX, "Apex" },
    .{ c.CBM_LANG_SOQL, "SOQL" },
    .{ c.CBM_LANG_SOSL, "SOSL" },
};

/// LANG_NAMES[lang], null for a language without a name.
const LANG_NAMES = blk: {
    var names: [c.CBM_LANG_COUNT]?Str = @splat(null);
    for (LANG_NAME_LIST) |entry| names[entry[0]] = entry[1];
    break :blk names;
};

fn findEntry(table: []const Entry, key: []const u8) ?Lang {
    for (table) |entry| {
        if (std.mem.eql(u8, entry[0], key)) return entry[1];
    }
    return null;
}

// ── Public API ─────────────────────────────────────────────────────

pub export fn cbm_language_for_extension(ext_c: ?Str) Lang {
    const ext = ext_c orelse return LANG_UNKNOWN;
    if (ext[0] == 0) return LANG_UNKNOWN;

    // Check user-defined overrides first
    if (userconfig.cbm_get_user_lang_config()) |ucfg| {
        const ulang = userconfig.lookup(ucfg, ext);
        if (ulang != LANG_UNKNOWN) return ulang;
    }

    return findEntry(&EXT_TABLE, std.mem.span(ext)) orelse LANG_UNKNOWN;
}

pub export fn cbm_language_for_filename(filename_c: ?Str) Lang {
    const filename_z = filename_c orelse return LANG_UNKNOWN;
    const filename = std.mem.span(filename_z);
    if (filename.len == 0) return LANG_UNKNOWN;

    // Check special filenames first
    if (findEntry(&FILENAME_TABLE, filename)) |lang| return lang;

    // DotEnv variant filenames (".env.local", ".env.production", …): the
    // filename starts with ".env." but its last "extension" (e.g. ".local")
    // is not a real language extension. Match the dotenv convention used by
    // pass_envscan/pass_infrascan (".env" exact, ".env." prefix, "*.env"
    // suffix) so file-index routing agrees with direct extraction.
    if (std.mem.startsWith(u8, filename, ".env.")) return c.CBM_LANG_DOTENV;

    // Fall back to extension-based lookup. Built-in extensions use the last
    // dot only.
    const last_dot = std.mem.lastIndexOfScalar(u8, filename, '.') orelse return LANG_UNKNOWN;

    // Probe compound extensions (e.g. ".blade.php") from the first dot toward
    // the last. Built-in compounds are checked first so e.g. Laravel Blade
    // templates map to Blade rather than the single-extension fallback (PHP);
    // user config can still add more (#258).
    const ucfg = userconfig.cbm_get_user_lang_config();
    var dot = std.mem.indexOfScalar(u8, filename, '.');
    while (dot) |p| {
        if (p >= last_dot) break;
        if (findEntry(&COMPOUND_EXT_TABLE, filename[p..])) |lang| return lang;
        if (ucfg) |cfg| {
            const lang = userconfig.lookup(cfg, filename_z + p);
            if (lang != LANG_UNKNOWN) return lang;
        }
        dot = std.mem.indexOfScalarPos(u8, filename, p + 1, '.');
    }

    // Standard single-extension lookup (built-ins + user overrides).
    return cbm_language_for_extension(filename_z + last_dot);
}

pub export fn cbm_language_name(lang: Lang) Str {
    if (lang >= c.CBM_LANG_COUNT) return "Unknown";
    return LANG_NAMES[lang] orelse "Unknown";
}

pub export fn cbm_language_tier(lang: Lang) c_int {
    if (lang >= c.CBM_LANG_COUNT) return 0;
    return switch (lang) {
        // Tier 1: shared with octacity-org/badbox.
        c.CBM_LANG_BASH,
        c.CBM_LANG_C,
        c.CBM_LANG_CPP,
        c.CBM_LANG_CSHARP,
        c.CBM_LANG_CSS,
        c.CBM_LANG_DART,
        c.CBM_LANG_ELIXIR,
        c.CBM_LANG_GO,
        c.CBM_LANG_HASKELL,
        c.CBM_LANG_HCL,
        c.CBM_LANG_HTML,
        c.CBM_LANG_JAVA,
        c.CBM_LANG_JAVASCRIPT,
        c.CBM_LANG_JSON,
        c.CBM_LANG_KOTLIN,
        c.CBM_LANG_LUA,
        c.CBM_LANG_MARKDOWN,
        c.CBM_LANG_NIX,
        c.CBM_LANG_PHP,
        c.CBM_LANG_POWERSHELL,
        c.CBM_LANG_PYTHON,
        c.CBM_LANG_RUBY,
        c.CBM_LANG_RUST,
        c.CBM_LANG_SCALA,
        c.CBM_LANG_SOLIDITY,
        c.CBM_LANG_SWIFT,
        c.CBM_LANG_TSX,
        c.CBM_LANG_TYPESCRIPT,
        c.CBM_LANG_YAML,
        c.CBM_LANG_ZIG,
        => 1,
        // Tier 2: other mainstream languages and build/config formats.
        c.CBM_LANG_OCAML,
        c.CBM_LANG_OBJC,
        c.CBM_LANG_PERL,
        c.CBM_LANG_GROOVY,
        c.CBM_LANG_ERLANG,
        c.CBM_LANG_R,
        c.CBM_LANG_SCSS,
        c.CBM_LANG_TOML,
        c.CBM_LANG_SQL,
        c.CBM_LANG_DOCKERFILE,
        c.CBM_LANG_CLOJURE,
        c.CBM_LANG_FSHARP,
        c.CBM_LANG_JULIA,
        c.CBM_LANG_CUDA,
        c.CBM_LANG_FORTRAN,
        c.CBM_LANG_MATLAB,
        c.CBM_LANG_XML,
        c.CBM_LANG_MAKEFILE,
        c.CBM_LANG_CMAKE,
        c.CBM_LANG_PROTOBUF,
        c.CBM_LANG_GRAPHQL,
        c.CBM_LANG_VUE,
        c.CBM_LANG_SVELTE,
        c.CBM_LANG_GOMOD,
        c.CBM_LANG_KUSTOMIZE,
        c.CBM_LANG_K8S,
        => 2,
        else => 3,
    };
}

// ── Shebang interpreter detection (extensionless scripts) ───────────

/// Basename of an interpreter path: the segment after the last '/'. Shebangs
/// are a POSIX convention, so only '/' is treated as a separator.
fn interpBasename(path: []const u8) []const u8 {
    const slash = std.mem.lastIndexOfScalar(u8, path, '/') orelse return path;
    return path[slash + 1 ..];
}

/// "python" optionally followed by an explicit numeric version (digits and
/// dots only, e.g. "python3", "python3.12"). Bounded and explicit so
/// arbitrary suffixes like "python-wrapper" are rejected.
fn isPythonInterp(base: []const u8) bool {
    if (!std.mem.startsWith(u8, base, "python")) return false;
    const version = base["python".len..];
    if (version.len == 0) return true;

    // Each numeric component must contain at least one digit.
    var need_digit = true;
    for (version) |ch| {
        if (std.ascii.isDigit(ch)) {
            need_digit = false;
        } else if (ch == '.' and !need_digit) {
            need_digit = true;
        } else {
            return false;
        }
    }
    return !need_digit;
}

const INTERP_TABLE = [_]Entry{
    .{ "sh", c.CBM_LANG_BASH },           .{ "bash", c.CBM_LANG_BASH }, .{ "dash", c.CBM_LANG_BASH },
    .{ "ksh", c.CBM_LANG_BASH },          .{ "zsh", c.CBM_LANG_BASH },  .{ "node", c.CBM_LANG_JAVASCRIPT },
    .{ "nodejs", c.CBM_LANG_JAVASCRIPT }, .{ "ruby", c.CBM_LANG_RUBY }, .{ "perl", c.CBM_LANG_PERL },
    .{ "php", c.CBM_LANG_PHP },           .{ "lua", c.CBM_LANG_LUA },
};

/// Map an interpreter basename to a language, or CBM_LANG_COUNT if
/// unrecognized. Non-python interpreters are matched exactly.
fn langForInterpreter(base: []const u8) Lang {
    if (isPythonInterp(base)) return c.CBM_LANG_PYTHON;
    return findEntry(&INTERP_TABLE, base) orelse LANG_UNKNOWN;
}

pub export fn cbm_language_from_shebang(path_c: ?Str) Lang {
    const path = path_c orelse return LANG_UNKNOWN;

    const f = c.cbm_fopen(path, "rb") orelse return LANG_UNKNOWN; // fail closed on read error

    // Read only a bounded first line.
    var buf: [256]u8 = undefined;
    const n = c.fread(&buf, 1, buf.len - 1, f);

    // Fail closed on any read error rather than parsing a partial buffer.
    if (c.ferror(f) != 0) {
        _ = c.fclose(f);
        return LANG_UNKNOWN;
    }

    // If the bounded buffer filled without containing a newline, the first
    // line may extend past our bound. Probe a single extra byte to tell an
    // exact EOF (the whole file is <= 255 bytes) from a truncated longer
    // line: any surviving byte -- including a newline just beyond the bound --
    // means the first line was cut off, so fail closed. A probe read error
    // fails closed too. This keeps the read bounded (no unbounded line read
    // or allocation).
    const have_newline = std.mem.indexOfScalar(u8, buf[0..n], '\n') != null;
    if (!have_newline and n == buf.len - 1) {
        const probe = c.fgetc(f);
        if (probe != c.EOF or c.ferror(f) != 0) {
            _ = c.fclose(f);
            return LANG_UNKNOWN;
        }
    }
    _ = c.fclose(f);

    // Must begin with "#!".
    if (n < 2 or buf[0] != '#' or buf[1] != '!') return LANG_UNKNOWN;

    // Isolate the first line; reject an embedded NUL before the newline.
    var line_len: usize = 0;
    while (line_len < n and buf[line_len] != '\n') : (line_len += 1) {
        if (buf[line_len] == 0) return LANG_UNKNOWN; // embedded NUL — treat as binary
    }
    // Trim a trailing CR so CRLF first lines parse.
    if (line_len > 0 and buf[line_len - 1] == '\r') line_len -= 1;

    // First token after "#!" is the interpreter (or env).
    var tokens = std.mem.tokenizeAny(u8, buf[2..line_len], " \t");
    const interp = tokens.next() orelse return LANG_UNKNOWN;
    var base = interpBasename(interp);

    // "env [-S] <interp> [args...]": the real interpreter is the next token.
    // Only the plain "env <interp>" and "env -S/--split-string <interp> [args]"
    // shapes are supported. After the optional -S, the interpreter token must
    // be a real command, so reject option tokens (leading '-') and NAME=value
    // assignments (containing '=') -- e.g. "env PYTHON=/usr/bin/python
    // python-wrapper", where env would treat the first token as an env-var
    // setting rather than the program to run.
    if (std.mem.eql(u8, base, "env")) {
        var tok = tokens.next();
        if (tok) |t| {
            if (std.mem.eql(u8, t, "-S") or std.mem.eql(u8, t, "--split-string")) {
                tok = tokens.next();
            }
        }
        const t = tok orelse return LANG_UNKNOWN;
        if (t[0] == '-' or std.mem.indexOfScalar(u8, t, '=') != null) return LANG_UNKNOWN;
        base = interpBasename(t);
    }

    return langForInterpreter(base);
}

// ── .m file disambiguation ─────────────────────────────────────────

/// The C string view of a buffer read from a file: it ends at the first NUL,
/// as strstr and friends saw it.
fn cString(buf: []const u8) []const u8 {
    return std.mem.sliceTo(buf, 0);
}

fn contains(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

fn hasObjcMarkers(buf: []const u8) bool {
    return contains(buf, "@interface") or contains(buf, "@implementation") or
        contains(buf, "@protocol") or contains(buf, "@property") or
        contains(buf, "#import") or contains(buf, "@selector") or
        contains(buf, "@encode") or contains(buf, "@synthesize") or
        contains(buf, "@dynamic");
}

fn hasMagmaEndMarkers(buf: []const u8) bool {
    return contains(buf, "end function;") or contains(buf, "end procedure;") or
        contains(buf, "end intrinsic;") or contains(buf, "end if;") or
        contains(buf, "end for;") or contains(buf, "end while;");
}

/// Check for "intrinsic Name(" or "procedure Name(" patterns, at the first
/// occurrence of each marker.
fn hasMagmaCallablePattern(buf: []const u8) bool {
    for ([_][]const u8{ "intrinsic ", "procedure " }) |marker| {
        var p = (std.mem.indexOf(u8, buf, marker) orelse continue) + marker.len;
        while (p < buf.len and std.ascii.isAlphabetic(buf[p])) p += 1;
        if (p < buf.len and buf[p] == '(') return true;
    }
    return false;
}

/// Scan lines for MATLAB-specific markers (function/classdef/%%).
fn hasMatlabLineMarkers(buf: []const u8) bool {
    var lines = std.mem.splitScalar(u8, buf, '\n');
    while (lines.next()) |line| {
        const p = std.mem.trimStart(u8, line, " \t");
        if (std.mem.startsWith(u8, p, "function ") or
            std.mem.startsWith(u8, p, "function\t") or
            std.mem.startsWith(u8, p, "classdef ") or
            std.mem.startsWith(u8, p, "classdef\t") or std.mem.startsWith(u8, p, "%%") or
            (p.len > 0 and p[0] == '%' and (p.len < 2 or p[1] != '{')))
        {
            return true;
        }
    }
    return false;
}

/// Read the first 4KB of path into buf. Returns the C string view of what
/// was read, or null when the file is unreadable.
fn readHead4k(path: Str, buf: *[4096 + 1]u8) ?[]const u8 {
    const f = c.cbm_fopen(path, "r") orelse return null;
    const n = c.fread(buf, 1, 4096, f);
    buf[n] = 0;
    _ = c.fclose(f);
    return cString(buf[0..n]);
}

pub export fn cbm_disambiguate_m(path_c: ?Str) Lang {
    const path = path_c orelse return c.CBM_LANG_MATLAB;
    var raw: [4096 + 1]u8 = undefined;
    const buf = readHead4k(path, &raw) orelse return c.CBM_LANG_MATLAB;

    if (hasObjcMarkers(buf)) return c.CBM_LANG_OBJC;
    if (hasMagmaEndMarkers(buf)) return c.CBM_LANG_MAGMA;
    if ((contains(buf, "intrinsic ") or contains(buf, "procedure ")) and
        hasMagmaCallablePattern(buf))
    {
        return c.CBM_LANG_MAGMA;
    }
    if (hasMatlabLineMarkers(buf)) return c.CBM_LANG_MATLAB;

    return c.CBM_LANG_MATLAB;
}

/// Visual Basic 6 / VBA source exports are recognisable from their header
/// (upstream #721): every module carries `Attribute VB_Name = "..."`, class
/// modules open with `VERSION 1.0 CLASS`, forms/controls with `VERSION 5.00` +
/// `Begin VB.Form` / `Begin VB.UserControl`. None of these occur in Apex
/// classes or in FORM programs, whose .cls / .frm extensions VB6 happens to
/// share.
fn hasVb6Markers(buf: []const u8) bool {
    return contains(buf, "Attribute VB_Name") or contains(buf, "VERSION 1.0 CLASS") or
        contains(buf, "Begin VB.") or contains(buf, "\nOption Explicit");
}

/// There is no Visual Basic grammar, so a VB6 form or class module is
/// reported as unsupported (CBM_LANG_COUNT) instead of being handed to the
/// FORM grammar (no defs) or the Apex grammar (junk nodes, extract timeouts).
/// Any doubt keeps the extension's owner.
pub export fn cbm_disambiguate_frm(path_c: ?Str) Lang {
    var raw: [4096 + 1]u8 = undefined;
    const buf = readHead4k(path_c orelse return c.CBM_LANG_FORM, &raw) orelse
        return c.CBM_LANG_FORM;
    // VB6 form files open with "VERSION x.yy" on line 1.
    if (std.mem.startsWith(u8, buf, "VERSION ") and buf.len > "VERSION ".len and
        std.ascii.isDigit(buf["VERSION ".len]))
    {
        return LANG_UNKNOWN;
    }
    return if (hasVb6Markers(buf)) LANG_UNKNOWN else c.CBM_LANG_FORM;
}

pub export fn cbm_disambiguate_cls(path_c: ?Str) Lang {
    var raw: [4096 + 1]u8 = undefined;
    const buf = readHead4k(path_c orelse return c.CBM_LANG_APEX, &raw) orelse
        return c.CBM_LANG_APEX;
    return if (hasVb6Markers(buf)) LANG_UNKNOWN else c.CBM_LANG_APEX;
}

pub export fn cbm_disambiguate_res(path_c: ?Str) Lang {
    const path = path_c orelse return c.CBM_LANG_RESCRIPT;
    const f = c.cbm_fopen(path, "rb") orelse return c.CBM_LANG_RESCRIPT;
    var buf: [4096]u8 = undefined;
    const n = c.fread(&buf, 1, buf.len, f);
    _ = c.fclose(f);
    return if (std.mem.indexOfScalar(u8, buf[0..n], 0) != null) LANG_UNKNOWN else c.CBM_LANG_RESCRIPT;
}
