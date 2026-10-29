#include "sema.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace civic {
Type parseTypeName(const std::string& name, Location location) {
  std::string base = name;
  if (base.rfind("const ", 0) == 0) base = base.substr(6);
  if (base == "int") return Type::I32;
  if (base == "float" || base == "double") return Type::F64;
  if (base == "bool") return Type::Bool;
  if (base == "string") return Type::String;
  if (base == "string*") return Type::StringPointer;
  if (base == "void") return Type::Void;
  if (base == "int[]") return Type::Array;
  if (base.rfind("vector<", 0) == 0 || base.rfind("vec<", 0) == 0 || base == "vec") {
    return Type::Vector;
  }
  if (base.rfind("map<", 0) == 0) return Type::Map;
  if (base.rfind("vector<", 0) == 0) {
    throw Diagnostic(location, "vector<T> is not implemented yet");
  }
  throw Diagnostic(location, "unknown type '" + name + "'");
}

bool isConstTypeName(const std::string& name) {
  return name.rfind("const ", 0) == 0;
}

Type vectorElementType(const std::string& typeName, Location location) {
  std::string base = typeName;
  if (base.rfind("const ", 0) == 0) base = base.substr(6);
  std::string element = base;
  if (element.rfind("vector<", 0) == 0) {
    element = element.substr(8, element.size() - 9);
  } else if (element.rfind("vec<", 0) == 0) {
    element = element.substr(4, element.size() - 5);
  } else if (element == "vec") {
    element = "int";
  }
  return parseTypeName(element, location);
}

Type arrayElementType(const std::string& typeName, Location location) {
  std::string base = typeName;
  if (base.rfind("const ", 0) == 0) base = base.substr(6);
  return parseTypeName(base.substr(0, base.size() - 2), location);
}

Type mapValueType(const std::string& typeName, Location location) {
  std::string base = typeName;
  if (base.rfind("const ", 0) == 0) base = base.substr(6);
  std::string inner = base.substr(5, base.size() - 6);
  const size_t comma = inner.find(',');
  const std::string valueType = inner.substr(comma + 1);
  return parseTypeName(valueType, location);
}

namespace {
struct FunctionType {
  Type result;
  std::vector<Type> results;
  std::vector<Type> parameters;
  std::vector<Type> resultTypes() const {
    return results.empty() ? std::vector<Type>{result} : results;
  }
};

struct TypedVariable {
  Type type;
  bool isConst = false;
  std::string typeName;
};

class Analyzer {
public:
  explicit Analyzer(const Program& program, bool requireMain = true)
      : program_(program), requireMain_(requireMain) {
    auto registerBuiltin = [&](const std::string& name, Type result, std::vector<Type> parameters) {
      FunctionType type;
      type.result = result;
      type.results = {result};
      type.parameters = std::move(parameters);
      functions_.emplace(name, std::move(type));
    };
    registerBuiltin("__civic_string_length", Type::I32, {Type::String});
    registerBuiltin("__civic_string_equals", Type::Bool, {Type::String, Type::String});
    registerBuiltin("__civic_string_contains", Type::Bool, {Type::String, Type::String});
    registerBuiltin("__civic_string_starts_with", Type::Bool, {Type::String, Type::String});
    registerBuiltin("__civic_string_ends_with", Type::Bool, {Type::String, Type::String});
    std::unordered_set<std::string> declaredFunctions;
    for (const FunctionDecl& function : program.functions) {
      if (!declaredFunctions.insert(function.name).second) {
        throw Diagnostic(function.location, "duplicate function '" + function.name + "'");
      }
      FunctionType signature;
      signature.result = parseTypeName(function.returnType, function.location);
      const std::vector<std::string>& declaredReturns =
          function.returnTypes.empty() ? std::vector<std::string>{function.returnType}
                                      : function.returnTypes;
      for (const std::string& returnType : declaredReturns) {
        signature.results.push_back(parseTypeName(returnType, function.location));
      }
      for (const Parameter& parameter : function.parameters) {
        signature.parameters.push_back(parseTypeName(parameter.typeName, parameter.location));
      }
      auto existing = functions_.find(function.name);
      if (existing != functions_.end() &&
          existing->first.rfind("__civic_string_", 0) != 0) {
        throw Diagnostic(function.location, "duplicate function '" + function.name + "'");
      }
      if (existing != functions_.end() &&
          (existing->second.resultTypes() != signature.resultTypes() ||
           existing->second.parameters != signature.parameters)) {
        throw Diagnostic(function.location,
                         "incorrect signature for compiler string intrinsic");
      }
      functions_[function.name] = std::move(signature);
    }
  }

  void run() {
    if (const auto mainIterator = functions_.find("main"); mainIterator != functions_.end()) {
      if (requireMain_ == false && program_.isHeader) {
        throw Diagnostic({1, 1}, "headers cannot declare 'int main()'");
      }
      const FunctionType& mainFunction = mainIterator->second;
      const bool noArgs = mainFunction.parameters.empty();
      const bool cArgs = mainFunction.parameters.size() == 2 &&
                        mainFunction.parameters[0] == Type::I32 &&
                        mainFunction.parameters[1] == Type::StringPointer;
      if (mainFunction.resultTypes() != std::vector<Type>{Type::I32} || (!noArgs && !cArgs)) {
        throw Diagnostic({1, 1}, "'main' must have signature int main() or int main(int, string*)");
      }
    } else if (requireMain_) {
      throw Diagnostic({1, 1}, "program is missing 'int main()'");
    }
    for (const FunctionDecl& function : program_.functions) {
      if (!function.body) continue;
      currentReturnTypes_ = functions_.at(function.name).resultTypes();
      currentReturn_ = currentReturnTypes_.empty() ? Type::Void : currentReturnTypes_[0];
      currentVisible_ = function.visibleFiles;
      currentHasIo_ = function.hasIo;
      if (currentVisible_.empty()) currentVisible_.push_back(function.location.file);

      scopes_.clear();
      scopes_.emplace_back();
      for (size_t i = 0; i < function.parameters.size(); ++i) {
        const Parameter& parameter = function.parameters[i];
        if (!scopes_.back()
                 .emplace(parameter.name,
                          TypedVariable{functions_.at(function.name).parameters[i],
                                        isConstTypeName(parameter.typeName), parameter.typeName})
                 .second) {
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
        const TypedVariable& variable =
            findVariable(expression->children[0]->text, expression->location);
        if (base == Type::Array) {
          if (index != Type::I32) {
            throw Diagnostic(expression->location, "array index must be an int");
          }
          return arrayElementType(variable.typeName, expression->location);
        }
        if (base == Type::Vector) {
          if (index != Type::I32) {
            throw Diagnostic(expression->location, "vector index must be an int");
          }
          return vectorElementType(variable.typeName, expression->location);
        }
        if (base == Type::Map) {
          if (index == Type::String) {
            return mapValueType(variable.typeName, expression->location);
          }
          if (index == Type::I32) {
            // Positional key access used by the for-in desugaring for maps.
            if (variable.typeName.empty()) {
              throw Diagnostic(expression->location,
                               "positional map access is only valid inside a for-in loop");
            }
            return Type::String;
          }
          throw Diagnostic(expression->location, "map keys must be strings");
        }
        if (base != Type::StringPointer || index != Type::I32) {
          throw Diagnostic(expression->location,
                           "indexing currently supports string* and int indices");
        }
        return Type::String;
      }
      case Expr::Kind::ArrayLiteral: {
        if (expression->children.empty()) return Type::Array;
        Type element = expressionType(expression->children[0]);
        for (const ExprPtr& child : expression->children) {
          if (expressionType(child) != element) {
            throw Diagnostic(expression->location, "array literal elements must have the same type");
          }
        }
        return Type::Array;
      }
      case Expr::Kind::MapLiteral: {
        if (expression->children.size() % 2 != 0) {
          throw Diagnostic(expression->location, "map literal entries must be key/value pairs");
        }
        for (size_t index = 0; index < expression->children.size(); index += 2) {
          if (expressionType(expression->children[index]) != Type::String) {
            throw Diagnostic(expression->children[index]->location,
                             "map literal keys must be strings");
          }
        }
        return Type::Map;
      }
      case Expr::Kind::Name: {
        const TypedVariable& variable = findVariable(expression->text, expression->location);
        return variable.type;
      }
      case Expr::Kind::Unary: {
        Type operand = expressionType(expression->children[0]);
        if (expression->text == "++" || expression->text == "--" ||
            expression->text == "post++" || expression->text == "post--") {
          requireMutableVariable(expression->children[0]);
          if (operand != Type::I32 && operand != Type::F64) {
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
          if (expression->children[0]->kind != Expr::Kind::Name &&
              expression->children[0]->kind != Expr::Kind::Index) {
            throw Diagnostic(expression->location, "assignment target must be a variable");
          }
          requireMutableVariable(expression->children[0]);
          Type lhs = expressionType(expression->children[0]);
          Type rhs = expressionType(expression->children[1]);
          if (lhs != rhs) throw Diagnostic(expression->location, "assignment types do not match");
          if (expression->children[0]->kind == Expr::Kind::Index) {
            if (op != "=") {
              throw Diagnostic(expression->location,
                               "compound assignment is not supported on collection elements");
            }
            return lhs;
          }
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
            throw Diagnostic(expression->location, "equality requires matching scalar or string values");
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
      case Expr::Kind::If: {
        requireConditionType(expressionType(expression->children[0]), expression->location);
        Type thenType = expressionType(expression->children[1]);
        Type elseType = expressionType(expression->children[2]);
        if (thenType != elseType) {
          throw Diagnostic(expression->location, "if expression arms must have the same type");
        }
        return thenType;
      }
      case Expr::Kind::Switch:
        return analyzeSwitchExpression(expression);
      case Expr::Kind::Call: {
        const std::vector<Type> results = analyzeCall(expression);
        if (results.size() != 1) {
          throw Diagnostic(
              expression->location,
              "call '" + expression->text + "' returns " + std::to_string(results.size()) +
                  " values; bind them with 'int name, int name = ...' or return them directly");
        }
        return results[0];
      }
    }
    return Type::Void;
  }

  std::vector<Type> analyzeCall(const ExprPtr& call) {
    if (call->text == "assert") {
      if (call->children.size() != 1) {
        throw Diagnostic(call->location, "assert expects exactly one argument");
      }
      requireConditionType(expressionType(call->children[0]), call->location);
      return std::vector<Type>{Type::Void};
    }
    if (call->text == "print" || call->text == "println") {
      if (!currentHasIo_) {
        throw Diagnostic(call->location, "print/println require #include <std/io>");
      }
      for (const ExprPtr& argument : call->children) {
        Type type = expressionType(argument);
        if (type == Type::Void || type == Type::StringPointer) {
          throw Diagnostic(argument->location, "cannot print this value");
        }
      }
      return std::vector<Type>{Type::Void};
    }
    if (!call->receiver.empty()) {
      return analyzeMethodCall(call);
    }
    const FunctionDecl* function = findVisibleFunction(call->text);
    if (function == nullptr) {
      throw Diagnostic(call->location, "unknown function '" + call->text + "'");
    }
    const FunctionType& signature = functions_.at(function->name);
    if (signature.parameters.size() != call->children.size()) {
      throw Diagnostic(call->location, "wrong number of arguments to '" + call->text + "'");
    }
    for (size_t i = 0; i < call->children.size(); ++i) {
      if (expressionType(call->children[i]) != signature.parameters[i]) {
        throw Diagnostic(call->children[i]->location, "argument type does not match parameter");
      }
    }
    return signature.resultTypes();
  }

  std::vector<Type> analyzeMethodCall(const ExprPtr& call) {
    const TypedVariable& receiver = findVariable(call->receiver, call->location);
    const std::string& method = call->text;
    const std::string& typeName = receiver.typeName;
    const bool isVector = receiver.type == Type::Vector;
    const bool isArray = receiver.type == Type::Array;
    const bool isMap = receiver.type == Type::Map;
    if (!isVector && !isArray && !isMap) {
      throw Diagnostic(call->location, "type '" + typeName + "' has no method '" + method + "'");
    }
    if (method == "size") {
      if (!call->children.empty()) {
        throw Diagnostic(call->location, "size takes no arguments");
      }
      return std::vector<Type>{Type::I32};
    }
    if (method == "at") {
      if (call->children.size() != 1) {
        throw Diagnostic(call->location, "at expects exactly one argument");
      }
      const Type argument = expressionType(call->children[0]);
      if (isMap) {
        if (argument != Type::String) {
          throw Diagnostic(call->children[0]->location, "map keys must be strings");
        }
        return std::vector<Type>{mapValueType(typeName, call->location)};
      }
      if (argument != Type::I32) {
        throw Diagnostic(call->children[0]->location, "collection index must be an int");
      }
      const Type element = isArray ? arrayElementType(typeName, call->location)
                                   : vectorElementType(typeName, call->location);
      return std::vector<Type>{element};
    }
    if (method == "insert" || method == "push") {
      if (isArray) {
        throw Diagnostic(call->location, "arrays are fixed size and do not support insert");
      }
      if (isMap) {
        if (method == "push" || call->children.size() != 2) {
          throw Diagnostic(call->location, "map insert expects (key, value)");
        }
        if (expressionType(call->children[0]) != Type::String) {
          throw Diagnostic(call->children[0]->location, "map keys must be strings");
        }
        if (expressionType(call->children[1]) != mapValueType(typeName, call->location)) {
          throw Diagnostic(call->children[1]->location, "map value type does not match");
        }
        return std::vector<Type>{Type::Void};
      }
      const Type element = vectorElementType(typeName, call->location);
      if (call->children.size() == 1) {
        if (expressionType(call->children[0]) != element) {
          throw Diagnostic(call->children[0]->location, "inserted value type does not match the vector element type");
        }
        return std::vector<Type>{Type::Void};
      }
      if (call->children.size() != 2) {
        throw Diagnostic(call->location, "vector insert expects (value) or (index, value)");
      }
      if (expressionType(call->children[0]) != Type::I32) {
        throw Diagnostic(call->children[0]->location, "vector insert index must be an int");
      }
      if (expressionType(call->children[1]) != element) {
        throw Diagnostic(call->children[1]->location, "inserted value type does not match the vector element type");
      }
      return std::vector<Type>{Type::Void};
    }
    throw Diagnostic(call->location, "type '" + typeName + "' has no method '" + method + "'");
  }

  Type analyzeSwitchExpression(const ExprPtr& expression) {
    Type subject = expressionType(expression->children[0]);
    if (subject != Type::I32 && subject != Type::String) {
      throw Diagnostic(expression->location, "switch requires an int or string value");
    }
    std::unordered_set<long long> seenInts;
    std::unordered_set<std::string> seenStrings;
    std::optional<Type> unified;
    bool hasDefault = false;
    for (const ExprArm& arm : expression->arms) {
      if (arm.values.empty()) {
        if (hasDefault) throw Diagnostic(expression->location, "duplicate 'default' arm in switch");
        hasDefault = true;
      } else {
        for (const ExprPtr& value : arm.values) {
          requireCaseValue(value, subject, value->location);
          const bool isInt = value->kind == Expr::Kind::Integer ||
                             (value->kind == Expr::Kind::Unary && value->text == "-" &&
                              value->children[0]->kind == Expr::Kind::Integer);
          if (isInt) {
            long long number = std::stoll(value->text);
            if (value->kind == Expr::Kind::Unary) number = -number;
            if (!seenInts.insert(number).second) {
              throw Diagnostic(value->location, "duplicate case label in switch");
            }
          } else if (!seenStrings.insert(value->text).second) {
            throw Diagnostic(value->location, "duplicate case label in switch");
          }
        }
      }
      const Type armType = expressionType(arm.body);
      if (arm.values.empty() || !unified) {
        unified = armType;
      } else if (*unified != armType) {
        throw Diagnostic(expression->location, "switch expression arms must have the same type");
      }
    }
    if (!hasDefault) {
      throw Diagnostic(expression->location, "a switch expression requires a 'default' arm");
    }
    return *unified;
  }

  static void requireCaseValue(const ExprPtr& value, Type subject, Location location) {
    const bool isInt = value->kind == Expr::Kind::Integer ||
                      (value->kind == Expr::Kind::Unary && value->text == "-" &&
                       value->children[0]->kind == Expr::Kind::Integer);
    const bool isString = value->kind == Expr::Kind::String;
    const bool matches = subject == Type::String ? isString : subject == Type::I32 && isInt;
    if (!matches) {
      throw Diagnostic(location, "case label type does not match the switch value type");
    }
  }

  const FunctionDecl* findVisibleFunction(const std::string& name) const {
    for (const FunctionDecl& function : program_.functions) {
      if (function.name != name) continue;
      if (std::find(currentVisible_.begin(), currentVisible_.end(), function.location.file) ==
          currentVisible_.end()) {
        continue;
      }
      return &function;
    }
    return nullptr;
  }

  const TypedVariable& findVariable(const std::string& name, Location location) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      auto variable = scope->find(name);
      if (variable != scope->end()) return variable->second;
    }
    throw Diagnostic(location, "unknown variable '" + name + "'");
  }

  void requireMutableVariable(const ExprPtr& expression) {
    if (expression->kind == Expr::Kind::Index) {
      const TypedVariable& variable =
          findVariable(expression->children[0]->text, expression->location);
      if (variable.isConst) {
        throw Diagnostic(expression->location, "cannot modify const variable '" +
                                                  expression->children[0]->text + "'");
      }
      return;
    }
    if (expression->kind != Expr::Kind::Name) return;
    const std::string& name = expression->text;
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      auto variable = scope->find(name);
      if (variable == scope->end()) continue;
      if (variable->second.isConst) {
        throw Diagnostic(expression->location, "cannot modify const variable '" + name + "'");
      }
      return;
    }
    // Unknown names are diagnosed by expressionType later.
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
        if (statement->names.size() > 1) {
          if (statement->expression == nullptr) {
            throw Diagnostic(statement->location, "multi-value declarations require an initializer");
          }
          if (statement->expression->kind != Expr::Kind::Call) {
            throw Diagnostic(statement->location,
                             "the initializer of a multi-value declaration must be a function call");
          }
          const std::vector<Type> results = analyzeCall(statement->expression);
          if (results.size() != statement->names.size()) {
            throw Diagnostic(
                statement->location,
                "the function returns " + std::to_string(results.size()) +
                    " values but the declaration names " + std::to_string(statement->names.size()) +
                    " variables");
          }
          for (size_t index = 0; index < statement->names.size(); ++index) {
            const Type type = parseTypeName(statement->typeNames[index], statement->location);
            if (type == Type::Void) {
              throw Diagnostic(statement->location, "variables cannot have type void");
            }
            if (type != results[index]) {
              throw Diagnostic(statement->location, "declared type does not match returned value");
            }
            auto& current = scopes_.back();
            if (!current
                     .emplace(statement->names[index],
                              TypedVariable{type, isConstTypeName(statement->typeNames[index]),
                                           statement->typeNames[index]})
                     .second) {
              throw Diagnostic(statement->location, "duplicate variable '" + statement->names[index] + "'");
            }
          }
          return;
        }
        Type type;
        const bool inferred = statement->typeName == "auto";
        if (inferred) {
          if (!statement->expression) {
            throw Diagnostic(statement->location, "declarations with type 'auto' require an initializer");
          }
          type = expressionType(statement->expression);
          if (type == Type::Array || type == Type::Vector || type == Type::Map) {
            throw Diagnostic(statement->location,
                             "'auto' cannot infer a whole collection; use '= collection[index]'");
          }
        } else {
          type = parseTypeName(statement->typeName, statement->location);
        }
        if (type == Type::Void) throw Diagnostic(statement->location, "variables cannot have type void");
        if (statement->expression) {
          const ExprPtr& initializer = statement->expression;
          const Type initializerType = expressionType(initializer);
          if (type == Type::Array && initializer->kind == Expr::Kind::ArrayLiteral) {
            const Type element = arrayElementType(statement->typeName, statement->location);
            for (const ExprPtr& entry : initializer->children) {
              if (expressionType(entry) != element) {
                throw Diagnostic(entry->location, "array element type does not match the declared element type");
              }
            }
          } else if (type == Type::Vector && initializer->kind == Expr::Kind::ArrayLiteral) {
            const Type element = vectorElementType(statement->typeName, statement->location);
            for (const ExprPtr& entry : initializer->children) {
              if (expressionType(entry) != element) {
                throw Diagnostic(entry->location, "vector element type does not match the declared element type");
              }
            }
          } else if (type == Type::Map && initializer->kind == Expr::Kind::MapLiteral) {
            const Type value = mapValueType(statement->typeName, statement->location);
            if (initializer->children.size() % 2 != 0) {
              throw Diagnostic(initializer->location, "map literal entries must be key/value pairs");
            }
            for (size_t index = 0; index < initializer->children.size(); index += 2) {
              if (expressionType(initializer->children[index]) != Type::String) {
                throw Diagnostic(initializer->children[index]->location, "map literal keys must be strings");
              }
              if (expressionType(initializer->children[index + 1]) != value) {
                throw Diagnostic(initializer->children[index + 1]->location,
                                 "map literal value type does not match the declared value type");
              }
            }
          } else if (initializerType != type) {
            throw Diagnostic(statement->location, "initializer type does not match variable type");
          }
        }
        const std::string storedTypeName = inferred ? "" : statement->typeName;
        auto& current = scopes_.back();
        if (!current.emplace(statement->name, TypedVariable{type, isConstTypeName(statement->typeName),
                                                              storedTypeName})
                     .second) {
          throw Diagnostic(statement->location, "duplicate variable '" + statement->name + "'");
        }
        return;
      }
      case Stmt::Kind::Expression:
        expressionType(statement->expression);
        return;
      case Stmt::Kind::Return: {
        if (statement->returnValues.empty()) {
          if (currentReturnTypes_.size() == 1 && currentReturnTypes_[0] != Type::Void) {
            throw Diagnostic(statement->location, "non-void function must return a value");
          }
          if (currentReturnTypes_.size() != 1) {
            throw Diagnostic(statement->location, "void functions cannot declare multiple returns");
          }
          return;
        }
        if (statement->returnValues.size() > 1) {
          if (statement->returnValues.size() != currentReturnTypes_.size()) {
            throw Diagnostic(statement->location, "the return list does not match the function's return types");
          }
          for (size_t index = 0; index < statement->returnValues.size(); ++index) {
            if (expressionType(statement->returnValues[index]) != currentReturnTypes_[index]) {
              throw Diagnostic(statement->location, "return type does not match function return type");
            }
          }
          return;
        }
        const ExprPtr& value = statement->returnValues[0];
        if (value->kind == Expr::Kind::Call) {
          const std::vector<Type> results = analyzeCall(value);
          if (results != currentReturnTypes_) {
            throw Diagnostic(statement->location, "the call return list does not match the function's return types");
          }
          return;
        }
        if (currentReturnTypes_.size() != 1) {
          throw Diagnostic(statement->location, "return type does not match function return type");
        }
        if (currentReturnTypes_[0] == Type::Void) {
          throw Diagnostic(statement->location, "void function cannot return a value");
        }
        if (expressionType(value) != currentReturnTypes_[0]) {
          throw Diagnostic(statement->location, "return type does not match function return type");
        }
        return;
      }
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
      case Stmt::Kind::Switch: {
        Type subject = expressionType(statement->expression);
        if (subject != Type::I32 && subject != Type::String) {
          throw Diagnostic(statement->location, "switch requires an int or string value");
        }
        std::unordered_set<long long> seenInts;
        std::unordered_set<std::string> seenStrings;
        bool sawDefault = false;
        for (const SwitchArm& arm : statement->switchArms) {
          if (arm.values.empty()) {
            if (sawDefault) throw Diagnostic(statement->location, "duplicate 'default' arm in switch");
            sawDefault = true;
            continue;
          }
          for (const ExprPtr& value : arm.values) {
            requireCaseValue(value, subject, value->location);
            const bool isInt = value->kind == Expr::Kind::Integer ||
                               (value->kind == Expr::Kind::Unary && value->text == "-" &&
                                value->children[0]->kind == Expr::Kind::Integer);
            if (isInt) {
              long long number = std::stoll(value->text);
              if (value->kind == Expr::Kind::Unary) number = -number;
              if (!seenInts.insert(number).second) {
                throw Diagnostic(value->location, "duplicate case label in switch");
              }
            } else if (!seenStrings.insert(value->text).second) {
              throw Diagnostic(value->location, "duplicate case label in switch");
            }
          }
        }
        for (const SwitchArm& arm : statement->switchArms) {
          scopes_.emplace_back();
          analyzeStatement(arm.body);
          scopes_.pop_back();
        }
        return;
      }
      case Stmt::Kind::TupleAssign: {
        if (statement->expression == nullptr || statement->expression->kind != Expr::Kind::Call) {
          throw Diagnostic(statement->location,
                           "the right side of a multiple assignment must be a function call");
        }
        const std::vector<Type> results = analyzeCall(statement->expression);
        if (results.size() != statement->names.size()) {
          throw Diagnostic(statement->location,
                           "the function returns " + std::to_string(results.size()) +
                               " values but the assignment names " +
                               std::to_string(statement->names.size()) + " variables");
        }
        for (size_t index = 0; index < statement->names.size(); ++index) {
          const TypedVariable& target = findVariable(statement->names[index], statement->location);
          if (target.isConst) {
            throw Diagnostic(statement->location, "cannot modify const variable '" +
                                                          statement->names[index] + "'");
          }
          if (target.type != results[index]) {
            throw Diagnostic(statement->location, "assigned type does not match variable type");
          }
        }
        return;
      }
    }
  }

  const Program& program_;
  Type currentReturn_ = Type::Void;
  std::vector<Type> currentReturnTypes_ = {Type::Void};
  std::unordered_map<std::string, FunctionType> functions_;
  std::vector<std::string> currentVisible_;
  bool currentHasIo_ = false;
  bool requireMain_ = true;
  std::vector<std::unordered_map<std::string, TypedVariable>> scopes_;
};
}  // namespace

void validateProgram(const Program& program, bool requireMain) {
  Analyzer(program, requireMain).run();
}
}  // namespace civic
