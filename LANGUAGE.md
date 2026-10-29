# Civic Language Specification

**Specification version:** 0.1  
**Source extension:** `.cv`  
**Status:** This document defines the intended v1 language. The implementation
status table at the end distinguishes specified behavior from compiler support.

## 1. Design and compilation model

Civic is a statically typed, ahead-of-time compiled language with C-like
declarations and control flow, class-based value types, deterministic
destruction, and a header-based module system. The compiler parses Civic source
and lowers it to LLVM IR; Clang links the generated IR with the platform runtime.
There is no C++ source translation step, and `std::` is not Civic syntax.

Compilation of a project starts at `src/main.cv`. Includes form a module graph;
each source file is parsed once per compilation. A header's declarations are
visible in every file that transitively includes it. Duplicate exported
names are errors. Cyclic includes are permitted only when declarations can be
resolved after the complete graph is loaded.

## 2. Source encoding and lexical rules

Source is UTF-8. Identifiers are ASCII letters or `_` followed by ASCII letters,
digits, or `_`. Identifiers are case-sensitive. `//` starts a line comment and
`/* ... */` delimits a non-nesting block comment. Comments do not occur inside
string literals.

string literals use double quotes and support `\\`, `\"`, `\n`, `\r`, and `\t`.
An unescaped newline or end of file before the closing quote is an error.
Integer literals are decimal and must fit the nonnegative range of `int`;
`INT_MIN` can be formed by arithmetic. Floating literals contain a decimal
point and digits on both sides. Integer literals have type `int`; floating
literals have type `double`. `true` and `false` are Boolean literals.

Whitespace separates tokens but is otherwise insignificant. Civic does not
perform textual C preprocessor substitution: the compiler resolves `#include`
and `#header` directives itself and never splices file text into the including
file. There are no macros; `#include <name>` is the only include directive, and
it takes an angle-bracket module name with no trailing semicolon.

## 3. Types

The primitive types are:

| Type | Meaning |
|---|---|
| `void` | No value; valid only as a function result |
| `bool` | `true` or `false` |
| `char` | Unsigned 8-bit byte |
| `int` | Signed 32-bit integer |
| `float`, `double` | IEEE-754 binary64 floating-point value |
| `string` | UTF-8 text value |

`T*` is an explicit, non-owning pointer to `T`. Pointers may be null; dereference
requires a non-null value. Raw pointers do not participate in automatic
ownership or borrow checking.

`vector<T>` is a dynamic, contiguous sequence. Its elements have type `T` and
are destroyed in reverse order when the vector is destroyed. `unique_ptr<T>`
owns one object and is move-only. `shared_ptr<T>` shares ownership and destroys
the object when the last owner is destroyed. These library types use ordinary
Civic value and lifetime rules.

Three built-in collection types are currently implemented:

| Type | Meaning |
|---|---|
| `T[]` (for example `int[]`) | Fixed-size stack array sized from its literal |
| `vec<T>` (`vec` alone means `vec<int>`) | Growable vector backed by a heap buffer; capacity doubles when exhausted |
| `map<string, T>` | String-keyed map; iteration yields keys in insertion order |

Collection literals are accepted only as variable initializers:

```cv
int[] squares = [1, 4, 9];
vec<int> values = [10, 20];
vec defaults = [5, 6, 7];              // vector<int>
map<string, int> ages = { "ada": 36, "grace": 45 };
```

Elements are read and written with `c[index]`; map values use `m["key"]`.
Whole collections cannot be copied, passed, or returned; operations go through
their elements and member functions (below). Out-of-range array and vector
access traps at runtime; reading a missing map key yields the zero value of
the value type.

There are no implicit numeric conversions in v1. Assignment, initialization,
function arguments, and returns require the same type. `float` and `double`
denote the same floating-point type.

## 4. Declarations and scopes

Top-level declarations are includes, functions, classes, structs, and template
declarations. Local variables are declared with a type followed by a name and
optional initializer:

```cv
int count = 0;
string title = "Civic";
```

An uninitialized local is zero-initialized for primitive types and
default-initialized for class types. A local's scope is the innermost enclosing
block. A nested block may shadow a name from an outer block. Redeclaring a name
in the same scope is an error.

The `const` qualifier marks a local variable or a parameter read-only:

```cv
const int limit = 10;
const string title = "Civic";
```

Assignment, compound assignment, and increment/decrement on a `const` variable
or parameter are compile-time errors. `const` may also be written after the
base type (`int const`, `int const*`); both orders are accepted and treated
identically.

Global mutable variables are not part of v1. Constants and compile-time
evaluation are reserved for a later version.

The `auto` type name is accepted only as a `for-in` loop binding; its type is
inferred from the collection being iterated (the element type, or the key type
for maps). It cannot infer a whole-collection value or be used to declare an
ordinary variable.

## 5. Functions

Functions are declared with a result type, name, typed parameter list, and body:

```cv
int sum(int left, int right) {
  return left + right;
}
```

Parameters are passed by value. Functions may be declared in any source file;
the compiler resolves calls across the complete include graph. V1 does not
support overloading, default arguments, variadic Civic functions, or nested
functions.

Every non-`void` function must return a value of its declared type on every
reachable path. A `void` function may use `return;` or fall through. The
executable entry point is `int main()` or `int main(int argc, string* argv)`.
Falling off the end of `main` returns zero.

## 6. Statements and control flow

V1 supports blocks, expression statements, variable declarations, `return`,
`if`/`else`, `while`, and C-style `for` loops:

```cv
for (int index = 0; index < limit; index++) {
  total += index;
}
```

A `for` initializer may be a declaration, an expression, or empty. Its variable
is scoped to the loop. Its condition may be empty (meaning true); its step may
be empty. `break` and `continue` are reserved but not currently in the v1
implementation.

`for-in` loops iterate a half-open range or a collection:

```cv
for i in 0..5 { total += i; }        // 0, 1, 2, 3, 4
for i in 0..=5 step 2 { total += i; } // 0, 2, 4
for x in values { total += x; }      // each vector/array element
for key in ages { count += 1; }      // each map key, in insertion order
```

A range bound `a..b` excludes `b`; `a..=b` includes it. `step expression`
replaces the implied increment of one.
Iterating arrays and vectors yields their elements; iterating a map yields its
keys. The loop variable's type may be written `auto` for collections, in which
case it is inferred.

Conditions accept `bool` or `int`; zero is false and nonzero is true.
`if`/`else` and loop bodies may be a single statement or a block. Return paths
and branches must preserve the declared function result type.

## 7. Expressions and operators

The operators, from highest to lowest precedence, are:

| Operators | Associativity | Meaning |
|---|---|---|
| unary `!`, `+`, `-`, prefix `++`, `--` | right | Logical not, sign, update |
| `*`, `/`, `%` | left | Multiplication, division, remainder |
| `+`, `-` | left | Addition, subtraction |
| `<`, `<=`, `>`, `>=` | left | Ordered numeric comparison |
| `==`, `!=` | left | Equality comparison |
| `&&` | left | Short-circuit logical and |
| `||` | left | Short-circuit logical or |
| `=`, `+=`, `-=`, `*=`, `/=` | right | Assignment |
| `cond ? a : b` | right | Conditional (ternary) expression |

Postfix `++` and `--` are also supported. Update operators require a mutable
numeric variable. Arithmetic operands must have matching numeric types.
Comparisons produce `bool`. `string` equality compares text contents.

The ternary expression evaluates `cond`, then either `a` or `b`; both arms
must have the same type, which is the type of the whole expression. Binding is
looser than `||`, so `a > b ? 1 : 0` reads as `(a > b) ? 1 : 0`; nested
ternaries in the middle arm require parentheses.

An `if` expression (`if (cond) { a } else { b }`) is available where an
expression is expected.

Assignment requires a mutable variable and matching types. Compound assignment
is numeric only. Addition, subtraction, and multiplication on `int` wrap to 32
bits. Division or remainder by zero, and `INT_MIN / -1` or `INT_MIN % -1`,
trap at runtime.
Floating arithmetic follows IEEE-754.

V1 does not support operator overloading, casts, pointer arithmetic, lambdas,
or exceptions.

Indexing `c[index]` works on the built-in collections and on `string`.
Compound assignment is not accepted on an indexed collection element.

## 8. Classes, structs, and methods

Classes and structs are named value types. Omitted access labels mean public.
`private:` may be used to hide implementation details; inheritance and
`protected:` access are not part of v1. `struct` and `class` have identical
layout and lifetime semantics.

Fields are initialized in declaration order. Methods are declared in the class
body and have an implicit `this` reference to the current object. Constructors
use the class name; a destructor uses `~ClassName()`:

```cv
#include <std/io>

class Counter {
  int value;

  Counter(int initial) {
    value = initial;
  }

  ~Counter() {
    println("counter destroyed");
  }

  int get() {
    return value;
  }
};
```

Constructors have no result type. A class may have overloaded constructors but
at most one destructor. Destructors take no parameters, return no value, and
must not throw. Methods are non-virtual in v1. Operator overloads and class
inheritance are excluded.

## 9. Copying, moving, and object lifetimes

Class values follow C++-style copy/move construction with deterministic RAII.
Copy construction duplicates a value; move construction transfers its
resources. A type may define copy and move constructors. If it does not, the
compiler synthesizes field-wise operations when every field permits them;
otherwise that operation is unavailable. `unique_ptr<T>` is never copyable.
`move(value)` explicitly requests a move from a named value. A moved-from
object remains destructible and may be assigned a new value.

Objects are destroyed in reverse order of completed construction. This applies
to normal block exit, `return`, conditional branches, loop exits, and
partially completed construction. Arrays and vectors destroy elements in
reverse order.

V1 has no borrow checker. References and pointers do not extend an object's
lifetime; using a pointer after its pointee's lifetime is invalid. The compiler
must diagnose statically evident use-after-move and invalid copy operations.

## 10. Templates and standard library

Templates use C++-style type parameters and are instantiated at compile time:

```cv
template <typename T>
class Box {
  T value;
};
```

V1 templates are monomorphized for each used type argument. Template
specialization, non-type parameters, concepts, and runtime generics are not
supported.

The standard library is header-based. Standard modules are headers located under
the `stdlib/` directory of the Civic root, and they are included with C-style
module names:

```cv
#include <std/io>
```

Module names use `/` for nested names, so the `std.io` module lives in
`<root>/stdlib/std/io.cv` and is written `#include <std/io>`. Custom modules use
the same shape and are resolved relative to the including source file; the
`.cv` extension is added when the module name does not already end with it:

```cv
#include <myproject/util>
```

Includes do not stay at the file boundary: declarations are visible in every
file that transitively includes the declaring header. A source file that
includes `<header1>` sees the declarations of `<header1>` plus any headers
`<header1>` itself includes, recursively. A file that is only reachable from
some other header remains invisible; each file that needs a call must include
a header that reaches it.

Includes are deduplicated by resolved path; including the same header twice is
a no-op. A header begins with a `#header` marker on its first line. The marker
may carry a module name on the same line (`#header io`); when the name is
omitted, the header's name defaults to its file name. The name is
documentation only; it is not checked against the file name and no other part
of the language reads it:

```cv
#header io
```

Entry source files cannot be marked `#header`. `#include` accepts only
angle-bracket module names; quoted include paths such as `#include "math.cv";`
are not supported. Resolution prefers the Civic standard library and then falls
back to the directory of the including source file, so a custom header cannot
shadow a standard library header.

`std.io` provides `print(value...)` and `println(value...)`. They accept `int`,
floating-point, `bool`, and `string`; `println` appends one line feed. Once any
file in the compilation's include graph reaches `std.io`, the built-ins are
usable everywhere in the program. `assert(condition)` is available without an
include and terminates the program
if its `bool` or `int` condition is false.

The v1 `string` API is:

| Member | Contract |
|---|---|
| `string()` | Empty string |
| `string(const char*)` | Copy a null-terminated UTF-8 string |
| `size()` | Number of UTF-8 bytes, excluding the terminator |
| `empty()` | Whether `size()` is zero |
| `c_str()` | Borrowed null-terminated UTF-8 pointer |
| `operator[](int)` | Byte at a zero-based index; out-of-range access traps |

`vector<T>` provides `size()`, `capacity()`, `empty()`, `push(value)`, `pop()`,
`clear()`, `reserve(count)`, and zero-based `operator[]`. `pop()` and indexing
on an empty or out-of-range vector trap. Reallocation preserves element values
and invalidates references and pointers to its elements.

The built-in collections implemented today expose this member API:

| Collection | Members |
|---|---|
| `T[]` | `size()`, `at(index)`, zero-based `operator[](int)` read/write |
| `vec<T>` | Everything `T[]` provides plus `insert(value)` (append) and `insert(index, value)` (inserts and shifts right); `push(value)` is an alias for the first form |
| `map<string, T>` | `at(key)`, `insert(key, value)` (overwrites an existing key), `operator[](string)` read/write (writing a new key appends; reading a missing key yields the zero value), `size()` |

`size()` on a map counts its keys; array `size()` is its declared literal
length. Direct `operator[](int)` on `map<K,V>` (positional key access by
insertion order) is reserved for the `for-in` desugaring and is not part of
the user-facing surface.

`unique_ptr<T>` provides null construction, `get()`, `release()`,
`reset(pointer)`, dereference, and member access. `make_unique<T>(args...)`
allocates and constructs an object. `shared_ptr<T>` provides empty
construction, copy/move, `get()`, `use_count()`, `reset()`, dereference, and
member access. The library does not expose C++ namespace names to Civic code.

## 11. Diagnostics and implementation status

Lexical, parse, and semantic errors are reported with source path, line, column,
and a source excerpt when available. Invalid programs must fail compilation;
the compiler must not silently translate unsupported constructs.

The current compiler implements includes (`#include <std/...>`), primitive types, `string` literals,
functions (including multiple-return-value calls), local variables, calls,
arithmetic/comparison/assignment, `if`/`else`, `if` expressions, `switch`
expressions, ternary `?:`, `while`, C-style `for`, `for-in` over ranges and
collections, built-in collections `T[]`, `vec<T>`, and `map<string, T>`,
`int main` with optional `argc`/`argv`, output built-ins, `assert`, LLVM IR
emission, and native linking.

`civic check <source.cv>` runs parse and semantic validation without generating
code and prints the first diagnostic with a source excerpt, returning a nonzero
exit code when the file is invalid. Header files (`#header`) are correct when
they define no `int main()`; only entry files require one. The LSP publishes
diagnostics produced by this same compiler analysis.

Classes, access labels, RAII injection, copy/move operations, templates,
`vector<T>`, smart pointers, and several operators/features above are specified
as the v1 target but are not implemented yet. In particular, the current
`string` implementation is backed by string literals or `argv`; it is not the
owning `string` value described above. The implementation must reject
unsupported types and syntax rather than pretend they work.

## 12. Project manifest and CLI behavior

`civic new <name>` creates a directory containing `civic.toml`,
`src/main.cv`, and a `.gitignore` for `target/`. The package name contains
letters, digits, `-`, or `_`. A project manifest has a `[package]` table with a
quoted `name` and `version`; the entry point is currently fixed at
`src/main.cv`.

The compiler resolves its Civic root independently of the current working
directory. `CIVIC_ROOT`, when set, names a root containing `stdlib/`. Otherwise,
the compiler locates its own executable and searches the build-tree and
installed layouts. A CMake build-tree root is the source tree containing both
`CMakeLists.txt` and `stdlib/`. An installed root is the data directory
containing `.civic-root` and `stdlib/` (normally `share/civic`). `#include
<std/name>` resolves to `<root>/stdlib/std/name.cv`. Includes that do not name
a standard module are resolved relative to the directory of the including
source file.

`civic build` and `civic run` search the current directory and its parents for
`civic.toml`. `--manifest-path <path>` selects a manifest or project directory.
`civic --version` (also `-v`, or the `civic version` command) prints the
compiler version, which the LSP server also reports in its `serverInfo`. The default profile is debug; `--release` selects the release profile. Outputs
are written to `target/debug/<name>` or `target/release/<name>`. `civic run`
builds then executes the selected profile. Arguments after `--` are passed to
the program's `argc`/`argv`. Direct `.cv` file forms of `build` and `run` are
also supported.

`civic check <source.cv>` validates a single file: it parses the program,
resolves its includes, and runs semantic analysis without generating code. It
prints the first diagnostic with the offending source line and a caret, and
exits 0 when the file is valid, 1 when it is not. `#header` files are
validated as declaration-only modules, so they pass without an `int main()`.

`civic lsp` starts the Language Server Protocol server over standard input and
output. It advertises full-document synchronization, publishes lexical, parse,
and semantic diagnostics on open and change, and supports completion, hover,
and function-definition navigation. Header files are validated without the
entry-point requirement, so they are never flagged for a missing `int main()`,
while a genuine error in a header still produces a diagnostic; published
messages include the offending source line and caret. The server uses UTF-16
LSP character positions and standard-library includes are resolved through the
discovered Civic root.

## 13. Implementation matrix

| Feature | v1 specification | Current compiler |
|---|---:|---:|
| Functions, local variables, calls | Yes | Yes |
| `int`, `float`, `double`, `bool`, `string` | Yes | Yes, with literal-backed `string` |
| `if`, `while`, C-style `for` | Yes | Yes |
| `for-in` over ranges and collections | Yes | Yes |
| `if` expressions, `switch` expressions, ternary `?:` | Yes | Yes |
| Built-in collections `T[]`, `vec<T>`, `map<string, T>` | Yes | Yes |
| Short-circuit `&&` and `||` | Yes | Yes |
| `string` content equality | Yes | Yes |
| File and standard-module includes | Yes | Yes |
| `main` arguments and source diagnostics | Yes | Yes |
| Project manifest, `new`, profiles, argument forwarding | Yes | Yes |
| `civic check` compiler-backed diagnostics | Yes | Yes |
| LSP diagnostics, completion, hover, function definitions | Yes | Yes |
| `char`, classes, methods, access labels | Yes | No |
| RAII, copy/move semantics | Yes | No |
| Templates, `vector<T>`, smart pointers | Yes | No |
| User-defined operator overloads, inheritance | No | No |

## Appendix A: Core grammar

The grammar below omits whitespace and comments, which may appear between any
two tokens. It describes the intended v1 syntax; semantic checks further
restrict types and l-values.

```ebnf
program          = { include | declaration } ;
include          = "#include" "<" module-name ">" ;
module-name      = identifier { "/" identifier } ;
declaration      = function | class | struct | template-declaration ;
function         = type identifier "(" [ parameters ] ")" block ;
parameters       = parameter { "," parameter } ;
parameter        = type identifier ;
class            = "class" identifier "{" { member } "}" [ ";" ] ;
struct           = "struct" identifier "{" { member } "}" [ ";" ] ;
member           = access-label | field | method | constructor | destructor ;
access-label     = "public" ":" | "private" ":" ;
field            = type identifier [ "=" expression ] ";" ;
method           = function ;
constructor      = identifier "(" [ parameters ] ")" block ;
destructor       = "~" identifier "(" ")" block ;
template-declaration
                 = "template" "<" "typename" identifier ">" declaration ;

type             = [ "const" ] base-type { "*" | "[" "]" } ;
base-type        = "void" | "bool" | "char" | "int" | "float" | "double"
                 | "string" | identifier [ "<" type { "," type } ">" ] | "auto" ;
block            = "{" { statement } "}" ;
statement        = block | variable-declaration | expression ";"
                 | "return" [ expression { "," expression } ] ";"
                 | "if" "(" expression ")" statement
                   [ "else" statement ]
                 | "while" "(" expression ")" statement
                 | "for" "(" [ for-initializer ] ";"
                   [ expression ] ";" [ expression ] ")" statement
                 | "for" identifier "in" expression statement ;
for-initializer  = variable-declaration-without-semicolon | expression ;
variable-declaration
                 = type identifier [ "=" expression ] ";"
                 | type identifier "," type identifier "=" expression ";" ;
variable-declaration-without-semicolon
                 = type identifier [ "=" expression ] ;

expression       = conditional ;
conditional      = assignment [ "?" assignment ":" conditional ] ;
assignment       = logical-or [ ( "=" | "+=" | "-=" | "*=" | "/=" )
                   assignment ] ;
logical-or       = logical-and { "||" logical-and } ;
logical-and      = equality { "&&" equality } ;
equality         = comparison { ( "==" | "!=" ) comparison } ;
comparison       = additive { ( "<" | "<=" | ">" | ">=" ) additive } ;
additive         = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative   = unary { ( "*" | "/" | "%" ) unary } ;
unary            = ( "!" | "+" | "-" | "++" | "--" ) unary | postfix ;
postfix          = primary { "(" [ arguments ] ")" | "[" expression "]"
                   | "." identifier [ "(" [ arguments ] ")" ] | "++" | "--" } ;
arguments        = expression { "," expression } ;
primary          = identifier | integer-literal | float-literal
                 | string-literal | "true" | "false"
                 | array-literal | map-literal | if-expression
                 | switch-expression
                 | "(" expression ")" ;
array-literal    = "[" [ arguments ] "]" ;
map-literal      = "{" [ map-entry { "," map-entry } [ "," ] ] "}" ;
map-entry        = string-literal ":" expression ;
if-expression    = "if" "(" expression ")" block "else" block ;
switch-expression
                 = "switch" "(" expression ")" "{" { switch-arm } "}" ;
switch-arm       = "case" case-constant { "," case-constant } ":" expression [ ";" ]
                 | "default" ":" expression [ ";" ] ;
```

Collection literals (`array-literal`, `map-literal`) are accepted only as a
variable initializer, where they must match the declared collection type.
`for-in` over a range (`..`/`..=`, optional `step`) is resolved by the parser
into the C-style loop form; `for-in` over a collection binds its variable
(`auto` allowed) to each element, or to each map key.

In the current compiler, `char`, class/struct declarations and members,
templates, references, and standalone class generic type arguments are
specified but not yet accepted; `vec<T>`, `map<K,V>`, collection literals,
`auto` for-in bindings, `if`/`switch` expressions, ternary `?:`, and
`for-in` are accepted. This grammar records the intended v1 surface, not a
claim that everything above is already usable.
