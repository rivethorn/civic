#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace civic {
struct Location {
  size_t line = 1;
  size_t column = 1;
  std::string file;
};

struct Diagnostic : std::runtime_error {
  Location location;
  Diagnostic(Location where, const std::string& message)
      : std::runtime_error(message), location(where) {}
};

struct ImportDecl {
  std::string path;
  bool isPath = false;
  Location location;
};

struct Expr {
  enum class Kind { Integer, Float, String, Boolean, Name, Unary, Binary, Call, Index };
  Kind kind;
  std::string text;
  std::string resolvedName;
  Location location;
  std::vector<std::string> typeArguments;
  std::vector<std::shared_ptr<Expr>> children;
};
using ExprPtr = std::shared_ptr<Expr>;

struct Stmt {
  enum class Kind { Block, Variable, Expression, Return, If, While, For };
  Kind kind;
  Location location;
  std::string typeName;
  std::string name;
  std::shared_ptr<Stmt> initializer;
  ExprPtr expression;
  ExprPtr step;
  std::vector<std::shared_ptr<Stmt>> children;
  std::shared_ptr<Stmt> alternative;
};
using StmtPtr = std::shared_ptr<Stmt>;

struct Parameter {
  std::string typeName;
  std::string name;
  Location location;
};

struct GenericParameter {
  std::string name;
  std::vector<std::string> bounds;
  Location location;
};

struct FunctionDecl {
  std::string returnType;
  std::string name;
  std::string symbolName;
  std::vector<GenericParameter> genericParameters;
  std::vector<Parameter> parameters;
  StmtPtr body;
  Location location;
};

struct Program {
  std::vector<ImportDecl> imports;
  std::vector<FunctionDecl> functions;
};
}  // namespace civic
