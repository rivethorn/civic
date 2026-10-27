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
either `int main()` or `int main(int argc, String* argv)`.

Direct source-file commands remain available:

```sh
civic build path/to/main.cv [-o executable]
civic run path/to/main.cv [-- program-args...]
civic test [file.cv|directory ...]
civic emit-ir path/to/main.cv [-o output.ll]
civic lsp
```

`civic lsp` runs a Language Server Protocol server over standard input/output.
It supports document open/change/close synchronization, parser and semantic
diagnostics, completion, hover, and function-definition navigation. Configure
your editor to launch `civic lsp` as a stdio language server.

With no paths, `civic test` recursively discovers `.cv` files under `tests/`.
Each test is compiled and executed; a nonzero exit (including a failed `assert`)
marks that test as failed.

## Current language subset

The compiler currently supports functions, typed parameters and locals, `int`,
`float`/`double`, `bool`, string-literal-backed `String`, arithmetic and comparison
expressions, calls, `if`/`else`, `while`, C-style `for`, imports, `print`/`println`,
and `assert`. `std.io` provides the output built-ins; standard-library names in
Civic source are Civic names, not C++ `std::` names.

Classes, user-defined RAII, generic `vector<T>`, smart pointers, and broader C
compatibility are not implemented yet; unsupported types produce diagnostics
rather than being silently translated.
