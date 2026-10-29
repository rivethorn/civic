<div align="center">
    <h1>Civic</h1>
    <p>Civilized C.</p>
</div>

Civic is an LLVM-backed compiler prototype. Its native compiler build uses CMake and
the LLVM C++ libraries; source files are parsed into Civic ASTs and lowered to LLVM
IR before the configured Clang driver creates a native executable.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`LLVM_DIR` can be set when LLVM's CMake package is not in the default search path.
The CMake configuration also locates Clang for the final native-code link.

## Projects and CLI

Once the compiler is available on `PATH`:

```sh
civic new hello-world
cd hello-world
civic build
civic run
civic run --release -- argument1 argument2
```

`civic new` creates `civic.toml` and `src/main.cv`. Project builds go to
`target/debug/` or `target/release/`; `--manifest-path` can select a project
manifest outside the current directory tree. The program entry point may use
either `int main()` or `int main(int argc, string* argv)`.

Direct source-file commands remain available:

```sh
civic build path/to/main.cv [-o executable]
civic run path/to/main.cv [-- program-args...]
civic test [file.cv|directory ...]
civic check path/to/file.cv
civic emit-ir path/to/main.cv [-o output.ll]
civic lsp
```

`civic check` parses and validates a single file without generating code,
printing the first diagnostic with a source excerpt and caret. `#header` files
are validated as declaration-only modules, so they pass without an `int
main()`.

`civic lsp` runs a Language Server Protocol server over standard input/output.
It supports document open/change/close synchronization, compiler-backed parser
and semantic diagnostics (including source excerpts in messages), completion,
hover, and function-definition navigation. Header files are validated without
the entry-point requirement, so they are never flagged for a missing
`int main()`. Configure your editor to launch `civic lsp` as a stdio language
server. A Neovim plugin lives in [`nvim/`](nvim/README.md): syntax,
quickfix-aware CLI commands (`:CivicBuild`, `:CivicRun`, …), automatic `gf`
include resolution, and native `vim.lsp` auto-attachment. Run `:checkhealth
civic` to verify it.

With no paths, `civic test` recursively discovers `.cv` files under `tests/`.
Each test is compiled and executed; a nonzero exit (including a failed `assert`)
marks that test as failed.

## Current language subset

The compiler currently supports functions, typed parameters and locals,
multiple-return-value calls, `int`, `float`/`double`, `bool`,
string-literal-backed `string`, arithmetic and comparison expressions,
ternary `?:`, `if`/`switch` expressions, calls, `if`/`else`, `while`, C-style
`for`, `for-in` over ranges and collections, built-in collections (`int[]` fixed arrays, `vec<T>` growable vectors,
`map<string, T>` maps with `.at()`/`.insert()`/`.size()`), `#include`,
`print`/`println`, and `assert`.
`std.io` provides the output built-ins; standard-library names in Civic source
are Civic names, not C++ `std::` names.

Classes, user-defined RAII, smart pointers, and broader C compatibility are not
implemented yet; unsupported types produce diagnostics rather than being
silently translated.
