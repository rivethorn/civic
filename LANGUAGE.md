# Civic Language Specification

**Specification version:** 0.1  
**Source extension:** `.cv`  
**Status:** This document defines the intended v1 language. The implementation
status table at the end distinguishes specified behavior from compiler support.

## 1. Design and compilation model

Civic is a statically typed, ahead-of-time compiled language with C-like
declarations and control flow, class-based value types, deterministic
destruction, and a headerless module system. The compiler parses Civic source
and lowers it to LLVM IR; Clang links the generated IR with the platform runtime.
There is no C++ source translation step, and `std::` is not Civic syntax.

Compilation of a project starts at `src/main.cv`. Imports form a module graph;
each source file is parsed once per compilation. A module exports its
top-level declarations into the importing compilation unit. Duplicate exported
names are errors. Cyclic imports are permitted only when declarations can be
resolved after the complete graph is loaded.

## 2. Source encoding and lexical rules

Source is UTF-8. Identifiers are ASCII letters or `_` followed by ASCII letters,
digits, or `_`. Identifiers are case-sensitive. `//` starts a line comment and
`/* ... */` delimits a non-nesting block comment. Comments do not occur inside
string literals.

String literals use double quotes and support `\\`, `\"`, `\n`, `\r`, and `\t`.
An unescaped newline or end of file before the closing quote is an error.
Integer literals are decimal and must fit the nonnegative range of `int`;
`INT_MIN` can be formed by arithmetic. Floating literals contain a decimal
point and digits on both sides. Integer literals have type `int`; floating
literals have type `double`. `true` and `false` are Boolean literals.

Whitespace separates tokens but is otherwise insignificant. Civic does not
perform C preprocessor substitution: `#include`, macros, and header files are
not part of the language.

## 3. Types

The primitive types are:

| Type | Meaning |
|---|---|
| `void` | No value; valid only as a function result |
| `bool` | `true` or `false` |
| `char` | Unsigned 8-bit byte |
| `int` | Signed 32-bit integer |
| `float`, `double` | IEEE-754 binary64 floating-point value |
| `String` | UTF-8 text value |

`T*` is an explicit, non-owning pointer to `T`. Pointers may be null; dereference
requires a non-null value. Raw pointers do not participate in automatic
ownership or borrow checking.

`vector<T>` is a dynamic, contiguous sequence. Its elements have type `T` and
are destroyed in reverse order when the vector is destroyed. `unique_ptr<T>`
owns one object and is move-only. `shared_ptr<T>` shares ownership and destroys
the object when the last owner is destroyed. These library types use ordinary
Civic value and lifetime rules.

There are no implicit numeric conversions in v1. Assignment, initialization,
function arguments, and returns require the same type. `float` and `double`
denote the same floating-point type.

## 4. Declarations and scopes

Top-level declarations are imports, functions, classes, structs, and template
declarations. Local variables are declared with a type followed by a name and
optional initializer:

```cv
int count = 0;
String title = "Civic";
```

An uninitialized local is zero-initialized for primitive types and
default-initialized for class types. A local's scope is the innermost enclosing
block. A nested block may shadow a name from an outer block. Redeclaring a name
in the same scope is an error.

Global mutable variables are not part of v1. Constants and compile-time
evaluation are reserved for a later version.

## 5. Functions

Functions are declared with a result type, name, typed parameter list, and body:

```cv
int sum(int left, int right) {
  return left + right;
}
```

Parameters are passed by value. Functions may be declared in any source module;
the compiler resolves calls across the complete import graph. V1 does not
support overloading, default arguments, variadic Civic functions, or nested
functions.

Every non-`void` function must return a value of its declared type on every
reachable path. A `void` function may use `return;` or fall through. The
executable entry point is `int main()` or `int main(int argc, String* argv)`.
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

Postfix `++` and `--` are also supported. Update operators require a mutable
numeric variable. Arithmetic operands must have matching numeric types.
Comparisons produce `bool`. `String` equality compares text contents.

Assignment requires a mutable variable and matching types. Compound assignment
is numeric only. Addition, subtraction, and multiplication on `int` wrap to 32
bits. Division or remainder by zero, and `INT_MIN / -1` or `INT_MIN % -1`,
trap at runtime.
Floating arithmetic follows IEEE-754.

V1 does not support operator overloading, casts, pointer arithmetic, lambdas,
conditional expressions, or exceptions.

## 8. Classes, structs, and methods

Classes and structs are named value types. Omitted access labels mean public.
`private:` may be used to hide implementation details; inheritance and
`protected:` access are not part of v1. `struct` and `class` have identical
layout and lifetime semantics.

Fields are initialized in declaration order. Methods are declared in the class
body and have an implicit `this` reference to the current object. Constructors
use the class name; a destructor uses `~ClassName()`:

```cv
import std.io;

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

The standard library is headerless. Standard modules use dotted import names:

```cv
import std.io;
```

Quoted imports are source paths relative to the importing file; `.cv` is added
when omitted:

```cv
import "math.cv";
```

`std.io` provides `print(value...)` and `println(value...)`. They accept `int`,
floating-point, `bool`, and `String`; `println` appends one line feed.
`assert(condition)` is available without an import and terminates the program
if its `bool` or `int` condition is false.

The v1 `String` API is:

| Member | Contract |
|---|---|
| `String()` | Empty string |
| `String(const char*)` | Copy a null-terminated UTF-8 string |
| `size()` | Number of UTF-8 bytes, excluding the terminator |
| `empty()` | Whether `size()` is zero |
| `c_str()` | Borrowed null-terminated UTF-8 pointer |
| `operator[](int)` | Byte at a zero-based index; out-of-range access traps |

`vector<T>` provides `size()`, `capacity()`, `empty()`, `push(value)`, `pop()`,
`clear()`, `reserve(count)`, and zero-based `operator[]`. `pop()` and indexing
on an empty or out-of-range vector trap. Reallocation preserves element values
and invalidates references and pointers to its elements.

`unique_ptr<T>` provides null construction, `get()`, `release()`,
`reset(pointer)`, dereference, and member access. `make_unique<T>(args...)`
allocates and constructs an object. `shared_ptr<T>` provides empty
construction, copy/move, `get()`, `use_count()`, `reset()`, dereference, and
member access. The library does not expose C++ namespace names to Civic code.

## 11. Diagnostics and implementation status

Lexical, parse, and semantic errors are reported with source path, line, column,
and a source excerpt when available. Invalid programs must fail compilation;
the compiler must not silently translate unsupported constructs.

The current compiler implements imports, primitive types, `String` literals,
functions, local variables, calls, arithmetic/comparison/assignment, `if`,
`while`, C-style `for`, `int main` with optional `argc`/`argv`, output built-ins,
assertions, LLVM IR emission, and native linking.

Classes, access labels, RAII injection, copy/move operations, templates,
`vector<T>`, smart pointers, and several operators/features above are specified
as the v1 target but are not implemented yet. In particular, the current
`String` implementation is backed by string literals or `argv`; it is not the
owning `String` value described above. The implementation must reject
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
containing `.civic-root` and `stdlib/` (normally `share/civic`). `import
std.name;` resolves to `<root>/stdlib/std/name.cv`. Quoted source imports stay
relative to the importing source file.

`civic build` and `civic run` search the current directory and its parents for
`civic.toml`. `--manifest-path <path>` selects a manifest or project directory.
The default profile is debug; `--release` selects the release profile. Outputs
are written to `target/debug/<name>` or `target/release/<name>`. `civic run`
builds then executes the selected profile. Arguments after `--` are passed to
the program's `argc`/`argv`. Direct `.cv` file forms of `build` and `run` are
also supported.

`civic lsp` starts the Language Server Protocol server over standard input and
output. It advertises full-document synchronization, publishes lexical, parse,
and semantic diagnostics on open and change, and supports completion, hover,
and function-definition navigation. The server uses UTF-16 LSP character
positions and standard-library imports are resolved through the discovered
Civic root.

## 13. Implementation matrix

| Feature | v1 specification | Current compiler |
|---|---:|---:|
| Functions, local variables, calls | Yes | Yes |
| `int`, `float`, `double`, `bool`, `String` | Yes | Yes, with literal-backed `String` |
| `if`, `while`, C-style `for` | Yes | Yes |
| Short-circuit `&&` and `||` | Yes | Yes |
| `String` content equality | Yes | Yes |
| File and standard-module imports | Yes | Yes |
| `main` arguments and source diagnostics | Yes | Yes |
| Project manifest, `new`, profiles, argument forwarding | Yes | Yes |
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
program          = { import | declaration } ;
import           = "import" ( module-name | string-literal ) ";" ;
module-name      = identifier { "." identifier } ;
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

type             = [ "const" ] base-type { "*" | "&" | "&&" } ;
base-type        = "void" | "bool" | "char" | "int" | "float" | "double"
                 | "String" | identifier [ "<" type { "," type } ">" ] ;
block            = "{" { statement } "}" ;
statement        = block | variable-declaration | expression ";"
                 | "return" [ expression ] ";"
                 | "if" "(" expression ")" statement
                   [ "else" statement ]
                 | "while" "(" expression ")" statement
                 | "for" "(" [ for-initializer ] ";"
                   [ expression ] ";" [ expression ] ")" statement ;
for-initializer  = variable-declaration-without-semicolon | expression ;
variable-declaration
                 = type identifier [ "=" expression ] ";" ;
variable-declaration-without-semicolon
                 = type identifier [ "=" expression ] ;

expression       = assignment ;
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
                   | "." identifier | "++" | "--" } ;
arguments        = expression { "," expression } ;
primary          = identifier | integer-literal | float-literal
                 | string-literal | "true" | "false"
                 | "(" expression ")" ;
```

In the current compiler, `char`, class/struct declarations and members,
templates, references, member access, and generic type arguments are specified
but not yet accepted. This grammar records the intended v1 surface, not a claim
that those features are already usable.
