#include "sema.hpp"

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace civic {
Type parseTypeName(const std::string& name, Location location) {
  if (name == "int") return Type::I32;
  if (name == "float" || name == "double") return Type::F64;
  if (name == "bool") return Type::Bool;
  if (name == "String") return Type::String;
  if (name == "String*") return Type::StringPointer;
  if (name == "void") return Type::Void;
  if (name.rfind("vector<", 0) == 0) {
    throw Diagnostic(location, "vector<T> is not implemented yet");
  }
  throw Diagnostic(location, "unknown type '" + name + "'");
}

namespace {
struct FunctionType {
  Type result;
  std::vector<Type> parameters;
};

class Analyzer {
public:
  explicit Analyzer(const Program& program) : program_(program) {
    for (const ImportDecl& import : program.imports) {
      if (import.path == "std.io") hasIo_ = true;
    }
    functions_.emplace("__civic_string_length",
                       FunctionType{Type::I32, {Type::String}});
    functions_.emplace("__civic_string_equals",
                       FunctionType{Type::Bool, {Type::String, Type::String}});
    functions_.emplace("__civic_string_contains",
                       FunctionType{Type::Bool, {Type::String, Type::String}});
    functions_.emplace("__civic_string_starts_with",
                       FunctionType{Type::Bool, {Type::String, Type::String}});
    functions_.emplace("__civic_string_ends_with",
                       FunctionType{Type::Bool, {Type::String, Type::String}});
    std::unordered_set<std::string> declaredFunctions;
    for (const FunctionDecl& function : program.functions) {
      if (!declaredFunctions.insert(function.name).second) {
        throw Diagnostic(function.location, "duplicate function '" + function.name + "'");
      }
      FunctionType signature{parseTypeName(function.returnType, function.location), {}};
      for (const Parameter& parameter : function.parameters) {
        signature.parameters.push_back(parseTypeName(parameter.typeName, parameter.location));
      }
      auto existing = functions_.find(function.name);
      if (existing != functions_.end() &&
          existing->first.rfind("__civic_string_", 0) != 0) {
        throw Diagnostic(function.location, "duplicate function '" + function.name + "'");
      }
      if (existing != functions_.end() &&
          (existing->second.result != signature.result ||
           existing->second.parameters != signature.parameters)) {
        throw Diagnostic(function.location,
                         "incorrect signature for compiler string intrinsic");
      }
      functions_[function.name] = std::move(signature);
    }
  }

  void run() {
    auto mainFunction = functions_.find("main");
    if (mainFunction == functions_.end()) {
      throw Diagnostic({1, 1}, "program is missing 'int main()'");
    }
    bool noArgs = mainFunction->second.parameters.empty();
    bool cArgs = mainFunction->second.parameters.size() == 2 &&
                 mainFunction->second.parameters[0] == Type::I32 &&
                 mainFunction->second.parameters[1] == Type::StringPointer;
    if (mainFunction->second.result != Type::I32 || (!noArgs && !cArgs)) {
      throw Diagnostic({1, 1}, "'main' must have signature int main() or int main(int, String*)");
    }
    for (const FunctionDecl& function : program_.functions) {
      if (!function.body) continue;
      currentReturn_ = parseTypeName(function.returnType, function.location);
      scopes_.clear();
      scopes_.emplace_back();
      for (size_t i = 0; i < function.parameters.size(); ++i) {
        const Parameter& parameter = function.parameters[i];
        if (!scopes_.back().emplace(parameter.name, functions_.at(function.name).parameters[i]).second) {
          throw Diagnostic(parameter.location, "duplicate parameter '" + parameter.name + "'");
        }
      }
      analyzeStatement(function.body);
    }
  }

private:
  Type expressionType(const ExprPtr& expression) {
    switch (expression->kind) {
      case Expr::Kind::Integer:
        try {
          long long value = std::stoll(expression->text);
          if (value > std::numeric_limits<int32_t>::max()) {
            throw Diagnostic(expression->location, "integer literal is outside the int range");
          }
        } catch (const std::invalid_argument&) {
          throw Diagnostic(expression->location, "invalid integer literal");
        } catch (const std::out_of_range&) {
          throw Diagnostic(expression->location, "integer literal is outside the int range");
        }
        return Type::I32;
      case Expr::Kind::Float: return Type::F64;
      case Expr::Kind::String: return Type::String;
      case Expr::Kind::Boolean: return Type::Bool;
      case Expr::Kind::Index: {
        Type base = expressionType(expression->children[0]);
        Type index = expressionType(expression->children[1]);
        if (base != Type::StringPointer || index != Type::I32) {
          throw Diagnostic(expression->location, "indexing currently supports String* and int indices");
        }
        return Type::String;
      }
      case Expr::Kind::Name: {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
          auto variable = scope->find(expression->text);
          if (variable != scope->end()) return variable->second;
        }
        throw Diagnostic(expression->location, "unknown variable '" + expression->text + "'");
      }
      case Expr::Kind::Unary: {
        Type operand = expressionType(expression->children[0]);
        if (expression->text == "++" || expression->text == "--" ||
            expression->text == "post++" || expression->text == "post--") {
          if (expression->children[0]->kind != Expr::Kind::Name ||
              (operand != Type::I32 && operand != Type::F64)) {
            throw Diagnostic(expression->location, "increment requires a numeric variable");
          }
          return operand;
        }
        if (expression->text == "!") {
          requireConditionType(operand, expression->location);
          return Type::Bool;
        }
        if (operand != Type::I32 && operand != Type::F64) {
          throw Diagnostic(expression->location, "unary arithmetic requires a number");
        }
        return operand;
      }
      case Expr::Kind::Binary: {
        const std::string& op = expression->text;
        if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") {
          if (expression->children[0]->kind != Expr::Kind::Name) {
            throw Diagnostic(expression->location, "assignment target must be a variable");
          }
          Type lhs = expressionType(expression->children[0]);
          Type rhs = expressionType(expression->children[1]);
          if (lhs != rhs) throw Diagnostic(expression->location, "assignment types do not match");
          if (op != "=" && lhs != Type::I32 && lhs != Type::F64) {
            throw Diagnostic(expression->location, "compound assignment requires a number");
          }
          return lhs;
        }
        Type lhs = expressionType(expression->children[0]);
        Type rhs = expressionType(expression->children[1]);
        if (op == "&&" || op == "||") {
          requireConditionType(lhs, expression->location);
          requireConditionType(rhs, expression->location);
          return Type::Bool;
        }
        if (op == "==" || op == "!=") {
          if (lhs != rhs || (lhs != Type::I32 && lhs != Type::F64 &&
                             lhs != Type::Bool && lhs != Type::String)) {
            throw Diagnostic(expression->location, "equality requires matching scalar or String values");
          }
          return Type::Bool;
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
          if (lhs != rhs || (lhs != Type::I32 && lhs != Type::F64)) {
            throw Diagnostic(expression->location, "ordered comparison requires matching numbers");
          }
          return Type::Bool;
        }
        if (lhs != rhs || (lhs != Type::I32 && lhs != Type::F64)) {
          throw Diagnostic(expression->location, "arithmetic requires matching numeric operands");
        }
        return lhs;
      }
      case Expr::Kind::Call: {
        if (expression->text == "assert") {
          if (expression->children.size() != 1) {
            throw Diagnostic(expression->location, "assert expects exactly one argument");
          }
          requireConditionType(expressionType(expression->children[0]), expression->location);
          return Type::Void;
        }
        if (expression->text == "print" || expression->text == "println") {
          if (!hasIo_) throw Diagnostic(expression->location, "import std.io to use print/println");
          for (const ExprPtr& argument : expression->children) {
            Type type = expressionType(argument);
            if (type == Type::Void || type == Type::StringPointer) {
              throw Diagnostic(argument->location, "cannot print this value");
            }
          }
          return Type::Void;
        }
        auto function = functions_.find(expression->text);
        if (function == functions_.end()) {
          throw Diagnostic(expression->location, "unknown function '" + expression->text + "'");
        }
        if (function->second.parameters.size() != expression->children.size()) {
          throw Diagnostic(expression->location, "wrong number of arguments to '" + expression->text + "'");
        }
        for (size_t i = 0; i < expression->children.size(); ++i) {
          if (expressionType(expression->children[i]) != function->second.parameters[i]) {
            throw Diagnostic(expression->children[i]->location, "argument type does not match parameter");
          }
        }
        return function->second.result;
      }
    }
    return Type::Void;
  }

  static void requireConditionType(Type type, Location location) {
    if (type != Type::Bool && type != Type::I32) {
      throw Diagnostic(location, "condition must be bool or int");
    }
  }

  void analyzeStatement(const StmtPtr& statement) {
    switch (statement->kind) {
      case Stmt::Kind::Block:
        scopes_.emplace_back();
        for (const StmtPtr& child : statement->children) analyzeStatement(child);
        scopes_.pop_back();
        return;
      case Stmt::Kind::Variable: {
        Type type = parseTypeName(statement->typeName, statement->location);
        if (type == Type::Void) throw Diagnostic(statement->location, "variables cannot have type void");
        if (statement->expression && expressionType(statement->expression) != type) {
          throw Diagnostic(statement->location, "initializer type does not match variable type");
        }
        auto& current = scopes_.back();
        if (!current.emplace(statement->name, type).second) {
          throw Diagnostic(statement->location, "duplicate variable '" + statement->name + "'");
        }
        return;
      }
      case Stmt::Kind::Expression:
        expressionType(statement->expression);
        return;
      case Stmt::Kind::Return:
        if (currentReturn_ == Type::Void) {
          if (statement->expression) throw Diagnostic(statement->location, "void function cannot return a value");
        } else {
          if (!statement->expression) throw Diagnostic(statement->location, "non-void function must return a value");
          if (expressionType(statement->expression) != currentReturn_) {
            throw Diagnostic(statement->location, "return type does not match function return type");
          }
        }
        return;
      case Stmt::Kind::If:
      case Stmt::Kind::While:
        requireConditionType(expressionType(statement->expression), statement->location);
        analyzeStatement(statement->children[0]);
        if (statement->alternative) analyzeStatement(statement->alternative);
        return;
      case Stmt::Kind::For:
        scopes_.emplace_back();
        if (statement->initializer) analyzeStatement(statement->initializer);
        if (statement->expression) {
          requireConditionType(expressionType(statement->expression), statement->location);
        }
        if (statement->step) expressionType(statement->step);
        analyzeStatement(statement->children[0]);
        scopes_.pop_back();
        return;
    }
  }

  const Program& program_;
  bool hasIo_ = false;
  Type currentReturn_ = Type::Void;
  std::unordered_map<std::string, FunctionType> functions_;
  std::vector<std::unordered_map<std::string, Type>> scopes_;
};
}  // namespace

void validateProgram(const Program& program) {
  Analyzer(program).run();
}
}  // namespace civic
