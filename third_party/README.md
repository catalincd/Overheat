Vendored tree-sitter grammars (MIT), generated sources only:
- tree-sitter-c v0.24.1   https://github.com/tree-sitter/tree-sitter-c
- tree-sitter-cpp v0.23.4 https://github.com/tree-sitter/tree-sitter-cpp
- tree-sitter-rust v0.24.2 https://github.com/tree-sitter/tree-sitter-rust
  (ABI 15; from https://github.com/tree-sitter/tree-sitter-rust/archive/refs/tags/v0.24.2.tar.gz,
  sha256 061e90a539a55a6aa65dceb0ad6425c50ab1a6e3e6d4ba430e2795ed4550f10e;
  src/parser.c, src/scanner.c, src/tree_sitter/*.h and LICENSE copied unchanged)
- tree-sitter v0.26.9 (core library: lib/src + lib/include, wasm support removed) https://github.com/tree-sitter/tree-sitter

Adding a grammar: copy its src/parser.c, src/scanner.c (if any), src/tree_sitter/*.h and LICENSE
into tree-sitter-<name>/, add <name> to TS_GRAMMARS in CMakeLists.txt, and add a row to the
GRAMMARS table in src/editor/syntax.cpp (highlight rules + classifier). Its LANGUAGE_VERSION
must be within the core's range (13..15 for v0.26).
