#include "parser.hpp"

#include "lexer.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace civic {
namespace {
class Parser {
public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  Program parse() {
    Program program;
    bool sawDeclaration = false;
    while (!atEnd()) {
      if (match("#")) {
        const Token& keyword = expectIdentifier("expected a directive after '#'");
        if (keyword.text == "include") {
          expect("<", "expected '<' after '#include'");
          std::string path = expectIdentifier("expected header name after '<'").text;
          while (match("/")) {
            path += "/" + expectIdentifier("expected header name after '/'").text;
          }
          expect(">", "expected '>' after '#include <...>'");
          IncludeDecl include;
          include.path = path;
          include.location = previous().location;
          program.includes.push_back(std::move(include));
          sawDeclaration = true;
          continue;
        }
        if (keyword.text == "header") {
          if (sawDeclaration) {
            throw Diagnostic(peek().location, "'#header' must be the first declaration in a header file");
          }
          std::string name;
          if (peek().kind == Token::Kind::Identifier && peek().location.line == keyword.location.line) {
            name = advance().text;
            while (match("/")) {
              name += "/" + expectIdentifier("expected header name after '/'").text;
            }
          }
          program.isHeader = true;
          program.headerName = name;
          continue;
        }
        throw Diagnostic(keyword.location, "unknown directive '#'" + keyword.text);
      }
      sawDeclaration = true;
      size_t namePosition = 0;
      if (looksLikeDeclaration(&namePosition) &&
          !(peek(namePosition + 1).kind == Token::Kind::Symbol && peek(namePosition + 1).text == "(")) {
        throw Diagnostic(peek().location, "top-level variables are not part of v1");
      }
      program.functions.push_back(parseFunction());
    }
    return program;
  }

private:
  const Token& peek(size_t ahead = 0) const {
    size_t index = position_ + ahead;
    return tokens_[index < tokens_.size() ? index : tokens_.size() - 1];
  }
  const Token& previous() const { return tokens_[position_ - 1]; }
  bool atEnd() const { return peek().kind == Token::Kind::End; }
  const Token& advance() {
    if (!atEnd()) ++position_;
    return previous();
  }
  bool isSyntax(const std::string& text) const {
    return peek().kind != Token::Kind::String && peek().kind != Token::Kind::Number &&
           peek().kind != Token::Kind::End && peek().text == text;
  }
  bool match(const std::string& text) {
    if (!isSyntax(text)) return false;
    advance();
    return true;
  }
  const Token& expect(const std::string& text, const std::string& message) {
    if (!isSyntax(text)) throw Diagnostic(peek().location, message);
    return advance();
  }
  const Token& expectIdentifier(const std::string& message) {
    if (peek().kind != Token::Kind::Identifier) throw Diagnostic(peek().location, message);
    return advance();
  }

  size_t findMatchingParen(size_t openIndex) const {
    int depth = 0;
    for (size_t index = openIndex; index + 1 < tokens_.size(); ++index) {
      const Token& token = tokens_[index];
      if (token.kind != Token::Kind::Symbol) continue;
      if (token.text == "(") {
        ++depth;
      } else if (token.text == ")") {
        --depth;
        if (depth == 0) return index;
      }
    }
    return 0;
  }

  ExprPtr parseMaybeParenExpression() {
    if (peek().kind == Token::Kind::Symbol && peek().text == "(") {
      const size_t close = findMatchingParen(position_);
      const Token& after = tokens_[close + 1];
      if (after.kind == Token::Kind::Symbol && after.text == "{") {
        expect("(", "expected '(' before the condition");
        ExprPtr expression = parseExpression();
        expect(")", "expected ')' after the condition");
        return expression;
      }
    }
    return parseExpression();
  }

  ExprPtr parseCaseConstant() {
    const Token& token = peek();
    if (token.kind == Token::Kind::Number && token.text.rfind("f:", 0) != 0) {
      advance();
      auto value = std::make_shared<Expr>();
      value->kind = Expr::Kind::Integer;
      value->text = token.text;
      value->location = token.location;
      return value;
    }
    if (token.kind == Token::Kind::Symbol && token.text == "-") {
      const Token& number = peek(1);
      if (number.kind == Token::Kind::Number && number.text.rfind("f:", 0) != 0) {
        advance();
        auto value = std::make_shared<Expr>();
        value->kind = Expr::Kind::Integer;
        value->text = number.text;
        value->location = number.location;
        auto negated = std::make_shared<Expr>();
        negated->kind = Expr::Kind::Unary;
        negated->text = "-";
        negated->location = token.location;
        negated->children.push_back(value);
        advance();
        return negated;
      }
    }
    if (token.kind == Token::Kind::String) {
      advance();
      auto value = std::make_shared<Expr>();
      value->kind = Expr::Kind::String;
      value->text = token.text;
      value->location = token.location;
      return value;
    }
    throw Diagnostic(token.location, "case labels must be integer or string constants");
  }

  ExprPtr parseIfExpression(const Token& keyword) {
    auto expression = std::make_shared<Expr>();
    expression->kind = Expr::Kind::If;
    expression->location = keyword.location;
    expression->children.push_back(parseMaybeParenExpression());
    if (peek().kind == Token::Kind::Symbol && peek().text == "{") {
      expect("{", "expected '{' to begin an if result arm");
      expression->children.push_back(parseExpression());
      expect("}", "expected '}' to close an if result arm");
    } else {
      expression->children.push_back(parseExpression());
    }
    expect("else", "an if expression requires an else arm");
    if (peek().kind == Token::Kind::Identifier && peek().text == "if") {
      const Token& elseIf = advance();
      expression->children.push_back(parseIfExpression(elseIf));
      return expression;
    }
    if (peek().kind == Token::Kind::Symbol && peek().text == "{") {
      expect("{", "expected '{' to begin an else result arm");
      expression->children.push_back(parseExpression());
      expect("}", "expected '}' to close an else result arm");
    } else {
      expression->children.push_back(parseExpression());
    }
    return expression;
  }

  ExprPtr parseSwitchExpression(const Token& keyword) {
    auto expression = std::make_shared<Expr>();
    expression->kind = Expr::Kind::Switch;
    expression->location = keyword.location;
    expression->children.push_back(parseMaybeParenExpression());
    expect("{", "expected '{' after switch");
    bool sawDefault = false;
    while (!match("}")) {
      const bool isDefault = match("default");
      if (!isDefault && !match("case")) {
        throw Diagnostic(peek().location, "expected 'case' or 'default' arm in switch expression");
      }
      ExprArm arm;
      if (isDefault) {
        if (sawDefault) throw Diagnostic(peek().location, "duplicate 'default' arm in switch");
        sawDefault = true;
        expect(":", "expected ':' after 'default'");
      } else {
        arm.values.push_back(parseCaseConstant());
        while (match(",")) arm.values.push_back(parseCaseConstant());
        expect(":", "expected ':' after case label");
      }
      arm.body = parseExpression();
      match(";");
      expression->arms.push_back(std::move(arm));
    }
    return expression;
  }

  bool looksLikeTupleAssign() const {
    size_t index = position_;
    int names = 0;
    while (index + 2 < tokens_.size() && tokens_[index].kind == Token::Kind::Identifier &&
           tokens_[index + 1].kind == Token::Kind::Symbol && tokens_[index + 1].text == "," &&
           tokens_[index + 2].kind == Token::Kind::Identifier) {
      index += 2;
      ++names;
    }
    if (names == 0) return false;
    const Token& tail = tokens_[index + 1];
    return tail.kind == Token::Kind::Symbol && tail.text == "=";
  }

  StmtPtr parseTupleAssign() {
    auto statement = std::make_shared<Stmt>();
    statement->kind = Stmt::Kind::TupleAssign;
    statement->location = peek().location;
    statement->names.push_back(expectIdentifier("expected variable name").text);
    while (match(",")) statement->names.push_back(expectIdentifier("expected variable name").text);
    expect("=", "expected '=' after tuple assignment");
    statement->expression = parseExpression();
    expect(";", "expected ';' after tuple assignment");
    return statement;
  }

  StmtPtr parseForIn() {
    auto statement = std::make_shared<Stmt>();
    statement->kind = Stmt::Kind::For;
    statement->location = previous().location;
    const Token& variable = advance();
    const Token& inKeyword = advance();
    (void)inKeyword;
    ExprPtr first = parseExpression();
    if (peek().kind == Token::Kind::Symbol && (peek().text == ".." || peek().text == "..=")) {
      bool inclusive = peek().text == "..=";
      advance();
      statement = desugarRangeFor(variable, first, inclusive);
      statement->children.push_back(parseStatement());
    } else {
      StmtPtr body = parseStatement();
      statement = desugarCollectionFor(variable, first);
      statement->children[0]->children.push_back(body);
    }
    return statement;
  }

  StmtPtr desugarRangeFor(const Token& variable, const ExprPtr& low, bool inclusive) {
    ExprPtr high = parseExpression();
    ExprPtr step;
    if (match("step")) step = parseExpression();

    auto statement = std::make_shared<Stmt>();
    statement->kind = Stmt::Kind::For;
    statement->location = variable.location;

    auto initializer = std::make_shared<Stmt>();
    initializer->kind = Stmt::Kind::Variable;
    initializer->location = variable.location;
    initializer->typeName = "int";
    initializer->name = variable.text;
    initializer->expression = low;

    auto condition = std::make_shared<Expr>();
    condition->kind = Expr::Kind::Binary;
    condition->text = inclusive ? "<=" : "<";
    condition->location = low->location;
    auto variableName = std::make_shared<Expr>();
    variableName->kind = Expr::Kind::Name;
    variableName->text = variable.text;
    variableName->location = variable.location;
    condition->children = {variableName, high};
    statement->initializer = initializer;
    statement->expression = condition;

    if (step) {
      auto assignment = std::make_shared<Expr>();
      assignment->kind = Expr::Kind::Binary;
      assignment->text = "+=";
      assignment->location = variable.location;
      auto stepVariable = std::make_shared<Expr>();
      stepVariable->kind = Expr::Kind::Name;
      stepVariable->text = variable.text;
      stepVariable->location = variable.location;
      assignment->children = {stepVariable, step};
      statement->step = assignment;
    } else {
      auto increment = std::make_shared<Expr>();
      increment->kind = Expr::Kind::Unary;
      increment->text = "post++";
      increment->location = variable.location;
      auto stepVariable = std::make_shared<Expr>();
      stepVariable->kind = Expr::Kind::Name;
      stepVariable->text = variable.text;
      stepVariable->location = variable.location;
      increment->children = {stepVariable};
      statement->step = increment;
    }
    return statement;
  }

  StmtPtr desugarCollectionFor(const Token& variable, const ExprPtr& collection) {
    if (collection->kind != Expr::Kind::Name) {
      throw Diagnostic(
          collection->location,
          "for-in over collections expects a variable holding an array, vector, or map");
    }
    auto statement = std::make_shared<Stmt>();
    statement->kind = Stmt::Kind::For;
    statement->location = variable.location;
    const std::string hidden = variable.text + "__for";

    auto initializer = std::make_shared<Stmt>();
    initializer->kind = Stmt::Kind::Variable;
    initializer->location = variable.location;
    initializer->typeName = "int";
    initializer->name = hidden;
    initializer->expression = integerExpression(variable.location, 0);

    auto sizeCall = std::make_shared<Expr>();
    sizeCall->kind = Expr::Kind::Call;
    sizeCall->text = "size";
    sizeCall->receiver = collection->text;
    sizeCall->location = collection->location;

    auto condition = std::make_shared<Expr>();
    condition->kind = Expr::Kind::Binary;
    condition->text = "<";
    condition->location = variable.location;
    auto hiddenName = std::make_shared<Expr>();
    hiddenName->kind = Expr::Kind::Name;
    hiddenName->text = hidden;
    hiddenName->location = variable.location;
    condition->children = {hiddenName, sizeCall};

    auto step = std::make_shared<Expr>();
    step->kind = Expr::Kind::Unary;
    step->text = "post++";
    step->location = variable.location;
    auto stepVariable = std::make_shared<Expr>();
    stepVariable->kind = Expr::Kind::Name;
    stepVariable->text = hidden;
    stepVariable->location = variable.location;
    step->children = {stepVariable};

    auto indexExpr = std::make_shared<Expr>();
    indexExpr->kind = Expr::Kind::Index;
    indexExpr->location = collection->location;
    auto collectionName = std::make_shared<Expr>();
    collectionName->kind = Expr::Kind::Name;
    collectionName->text = collection->text;
    collectionName->location = collection->location;
    auto indexName = std::make_shared<Expr>();
    indexName->kind = Expr::Kind::Name;
    indexName->text = hidden;
    indexName->location = variable.location;
    indexExpr->children = {collectionName, indexName};

    auto binding = std::make_shared<Stmt>();
    binding->kind = Stmt::Kind::Variable;
    binding->location = variable.location;
    binding->typeName = "auto";
    binding->name = variable.text;
    binding->expression = indexExpr;

    auto body = std::make_shared<Stmt>();
    body->kind = Stmt::Kind::Block;
    body->location = variable.location;
    body->children.push_back(binding);

    statement->initializer = initializer;
    statement->expression = condition;
    statement->step = step;
    statement->children.push_back(body);
    return statement;
  }

  ExprPtr integerExpression(Location location, int64_t value) {
    auto expression = std::make_shared<Expr>();
    expression->kind = Expr::Kind::Integer;
    expression->text = std::to_string(value);
    expression->location = location;
    return expression;
  }

  std::string parseType() {
    bool isConst = false;
    for (;;) {
      const Token& token = peek();
      if (token.kind == Token::Kind::Identifier && token.text == "const") {
        advance();
        isConst = true;
        continue;
      }
      break;
    }
    std::string type = expectIdentifier("expected a type name").text;
    if (match("<")) {
      type += "<" + parseType();
      while (match(",")) type += "," + parseType();
      expect(">", "expected '>' after generic type arguments");
      type += ">";
    }
    for (;;) {
      const Token& token = peek();
      if (token.kind == Token::Kind::Symbol && token.text == "*") {
        advance();
        type += "*";
        continue;
      }
      if (token.kind == Token::Kind::Symbol && token.text == "[") {
        advance();
        expect("]", "expected ']' after array type suffix");
        type += "[]";
        continue;
      }
      if (token.kind == Token::Kind::Identifier && token.text == "const") {
        advance();
        isConst = true;
        continue;
      }
      break;
    }
    if (isConst) type.insert(0, "const ");
    return type;
  }

  FunctionDecl parseFunction() {
    FunctionDecl function;
    function.location = peek().location;
    function.returnType = parseType();
    function.returnTypes.push_back(function.returnType);
    if (match(",")) {
      do {
        function.returnTypes.push_back(parseType());
      } while (match(","));
    }
    function.name = expectIdentifier("expected function name").text;
    function.symbolName = function.name;
    if (match("<")) {
      do {
        GenericParameter parameter;
        parameter.location = peek().location;
        parameter.name = expectIdentifier("expected generic parameter name").text;
        if (match(":")) {
          do {
            parameter.bounds.push_back(
                expectIdentifier("expected concept name after ':'").text);
          } while (match("+"));
        }
        function.genericParameters.push_back(std::move(parameter));
      } while (match(","));
      expect(">", "expected '>' after generic parameters");
    }
    expect("(", "expected '(' after function name");
    if (!match(")")) {
      do {
        Parameter parameter;
        parameter.location = peek().location;
        parameter.typeName = parseType();
        parameter.name = expectIdentifier("expected parameter name").text;
        function.parameters.push_back(std::move(parameter));
      } while (match(","));
      expect(")", "expected ')' after parameters");
    }
    if (match(";")) {
      function.body.reset();
    } else {
      function.body = parseBlock();
    }
    return function;
  }

  StmtPtr parseBlock() {
    const Token& open = expect("{", "expected '{' to begin block");
    auto block = std::make_shared<Stmt>();
    block->kind = Stmt::Kind::Block;
    block->location = open.location;
    while (!atEnd() && !isSyntax("}")) {
      block->children.push_back(parseStatement());
    }
    expect("}", "expected '}' to close block");
    return block;
  }

  StmtPtr parseStatement() {
    if (isSyntax("{")) return parseBlock();
    if (match("return")) {
      auto statement = std::make_shared<Stmt>();
      statement->kind = Stmt::Kind::Return;
      statement->location = previous().location;
      if (!isSyntax(";")) {
        statement->returnValues.push_back(parseExpression());
        while (match(",")) statement->returnValues.push_back(parseExpression());
        if (statement->returnValues.size() == 1) statement->expression = statement->returnValues[0];
      }
      expect(";", "expected ';' after return");
      return statement;
    }
    if (match("if")) {
      auto statement = std::make_shared<Stmt>();
      statement->kind = Stmt::Kind::If;
      statement->location = previous().location;
      statement->expression = parseMaybeParenExpression();
      statement->children.push_back(parseStatement());
      if (match("else")) statement->alternative = parseStatement();
      return statement;
    }
    if (match("switch")) {
      auto statement = std::make_shared<Stmt>();
      statement->kind = Stmt::Kind::Switch;
      statement->location = previous().location;
      statement->expression = parseMaybeParenExpression();
      expect("{", "expected '{' after switch");
      bool sawDefault = false;
      while (!match("}")) {
        const bool isDefault = match("default");
        if (!isDefault && !match("case")) {
          throw Diagnostic(peek().location, "expected 'case' or 'default' arm in switch");
        }
        SwitchArm arm;
        if (isDefault) {
          if (sawDefault) throw Diagnostic(peek().location, "duplicate 'default' arm in switch");
          sawDefault = true;
          expect(":", "expected ':' after 'default'");
        } else {
          arm.values.push_back(parseCaseConstant());
          while (match(",")) arm.values.push_back(parseCaseConstant());
          expect(":", "expected ':' after case label");
        }
        arm.body = parseBlock();
        statement->switchArms.push_back(std::move(arm));
      }
      return statement;
    }
    if (match("while")) {
      auto statement = std::make_shared<Stmt>();
      statement->kind = Stmt::Kind::While;
      statement->location = previous().location;
      statement->expression = parseMaybeParenExpression();
      statement->children.push_back(parseStatement());
      return statement;
    }
    if (match("for")) {
      if (peek().kind == Token::Kind::Identifier && peek(1).kind == Token::Kind::Identifier &&
          peek(1).text == "in") {
        return parseForIn();
      }
      auto statement = std::make_shared<Stmt>();
      statement->kind = Stmt::Kind::For;
      statement->location = previous().location;
      const bool parenthesized = isSyntax("(");
      if (parenthesized) expect("(", "expected '(' after for");
      if (!isSyntax(";")) {
        if (looksLikeDeclaration()) {
          statement->initializer = parseVariable(false);
        } else {
          auto initializer = std::make_shared<Stmt>();
          initializer->kind = Stmt::Kind::Expression;
          initializer->location = peek().location;
          initializer->expression = parseExpression();
          statement->initializer = std::move(initializer);
        }
      }
      expect(";", "expected ';' after for initializer");
      if (!isSyntax(";")) statement->expression = parseExpression();
      expect(";", "expected ';' after for condition");
      if (!isSyntax(")")) statement->step = parseExpression();
      if (parenthesized) expect(")", "expected ')' after for clauses");
      statement->children.push_back(parseStatement());
      return statement;
    }
    if (looksLikeTupleAssign()) {
      return parseTupleAssign();
    }
    if (looksLikeDeclaration()) {
      return parseVariable(true);
    }
    auto statement = std::make_shared<Stmt>();
    statement->kind = Stmt::Kind::Expression;
    statement->location = peek().location;
    statement->expression = parseExpression();
    expect(";", "expected ';' after expression");
    return statement;
  }

  StmtPtr parseVariable(bool consumeTerminator) {
      auto statement = std::make_shared<Stmt>();
      statement->kind = Stmt::Kind::Variable;
      statement->location = peek().location;
      statement->typeName = parseType();
      statement->name = expectIdentifier("expected variable name").text;
      if (match(",")) {
        statement->typeNames.push_back(statement->typeName);
        statement->names.push_back(statement->name);
        do {
          statement->typeNames.push_back(parseType());
          statement->names.push_back(expectIdentifier("expected variable name").text);
        } while (match(","));
        expect("=", "expected '=' after multi-value declarations");
        statement->expression = parseExpression();
      } else if (match("=")) {
        if (peek().kind == Token::Kind::Symbol && peek().text == "[") {
          statement->expression = parseArrayLiteral();
        } else if (peek().kind == Token::Kind::Symbol && peek().text == "{") {
          statement->expression = parseMapLiteral();
        } else {
          statement->expression = parseExpression();
        }
      }
      if (consumeTerminator) {
      expect(";", "expected ';' after variable declaration");
      }
      return statement;
  }

  ExprPtr parseArrayLiteral() {
    expect("[", "expected '[' to begin an array literal");
    auto array = std::make_shared<Expr>();
    array->kind = Expr::Kind::ArrayLiteral;
    array->location = peek().location;
    if (!isSyntax("]")) {
      array->children.push_back(parseExpression());
      while (match(",")) {
        if (isSyntax("]")) break;  // allow a trailing comma
        array->children.push_back(parseExpression());
      }
    }
    expect("]", "expected ']' to end an array literal");
    return array;
  }

  ExprPtr parseMapLiteral() {
    expect("{", "expected '{' to begin a map literal");
    auto map = std::make_shared<Expr>();
    map->kind = Expr::Kind::MapLiteral;
    map->location = peek().location;
    if (!isSyntax("}")) {
      map->children.push_back(parseExpression());
      expect(":", "expected ':' in a map literal");
      map->children.push_back(parseExpression());
      while (match(",")) {
        if (isSyntax("}")) break;  // allow a trailing comma
        map->children.push_back(parseExpression());
        expect(":", "expected ':' in a map literal");
        map->children.push_back(parseExpression());
      }
    }
    expect("}", "expected '}' to end a map literal");
    return map;
  }

  bool looksLikeDeclaration(size_t* namePosition = nullptr) const {
    size_t afterType = 0;
    if (peek().kind == Token::Kind::Identifier && peek().text == "const") ++afterType;
    if (peek(afterType).kind != Token::Kind::Identifier) return false;
    const std::string& first = peek(afterType).text;
    bool knownType = first == "int" || first == "uint" ||
                     first == "i8" || first == "i16" || first == "i32" ||
                     first == "i64" || first == "i128" ||
                     first == "u8" || first == "u16" || first == "u32" ||
                     first == "u64" || first == "u128" ||
                     first == "float" || first == "double" ||
                     first == "bool" || first == "string" ||
                     first == "vector" || first == "vec" || first == "map";
    if (!knownType) return false;
    ++afterType;
    if (first == "vector" || first == "vec" || first == "map") {
      if (peek(afterType).kind == Token::Kind::Symbol && peek(afterType).text == "<") {
        int depth = 1;
        ++afterType;
        while (depth > 0 && peek(afterType).kind != Token::Kind::End) {
          if (peek(afterType).kind == Token::Kind::Symbol) {
            if (peek(afterType).text == "<") ++depth;
            if (peek(afterType).text == ">") --depth;
          }
          ++afterType;
        }
      }
    }
    while (peek(afterType).kind == Token::Kind::Symbol &&
           (peek(afterType).text == "*" || peek(afterType).text == "const")) ++afterType;
    if (peek(afterType).kind == Token::Kind::Symbol && peek(afterType).text == "[" &&
        peek(afterType + 1).kind == Token::Kind::Symbol && peek(afterType + 1).text == "]") {
      afterType += 2;
      while (peek(afterType).kind == Token::Kind::Symbol &&
             (peek(afterType).text == "*" || peek(afterType).text == "const")) ++afterType;
    }
    if (namePosition) *namePosition = afterType;
    return peek(afterType).kind == Token::Kind::Identifier;
  }

  ExprPtr parseExpression(int minPrecedence = 1) {
    ExprPtr left = parseUnary();
    while (true) {
      int precedence = peek().kind == Token::Kind::Symbol
                           ? binaryPrecedence(peek().text)
                           : 0;
      if (precedence < minPrecedence) break;
      Token op = advance();
      bool rightAssociative = precedence == 1;
      ExprPtr right = parseExpression(precedence + (rightAssociative ? 0 : 1));
      auto binary = std::make_shared<Expr>();
      binary->kind = Expr::Kind::Binary;
      binary->text = op.text;
      binary->location = op.location;
      binary->children = {left, right};
      left = std::move(binary);
    }
    if (minPrecedence <= 1 && peek().kind == Token::Kind::Symbol && peek().text == "?") {
      advance();
      ExprPtr thenExpression = parseExpression(2);
      expect(":", "expected ':' in a ternary expression");
      ExprPtr elseExpression = parseExpression(1);
      auto ternary = std::make_shared<Expr>();
      ternary->kind = Expr::Kind::If;
      ternary->location = left->location;
      ternary->children.push_back(left);
      ternary->children.push_back(thenExpression);
      ternary->children.push_back(elseExpression);
      return ternary;
    }
    return left;
  }

  static int binaryPrecedence(const std::string& op) {
    if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") return 1;
    if (op == "||") return 2;
    if (op == "&&") return 3;
    if (op == "==" || op == "!=") return 4;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 5;
    if (op == "+" || op == "-") return 6;
    if (op == "*" || op == "/" || op == "%") return 7;
    return 0;
  }

  ExprPtr parseUnary() {
    if (peek().kind == Token::Kind::Symbol &&
        (peek().text == "!" || peek().text == "-" || peek().text == "+" ||
         peek().text == "++" || peek().text == "--")) {
      Token op = advance();
      auto expression = std::make_shared<Expr>();
      expression->kind = Expr::Kind::Unary;
      expression->text = op.text;
      expression->location = op.location;
      expression->children.push_back(parseUnary());
      return expression;
    }
    ExprPtr expression = parsePrimary();
    if (peek().kind == Token::Kind::Symbol &&
        (peek().text == "++" || peek().text == "--")) {
      Token op = advance();
      auto update = std::make_shared<Expr>();
      update->kind = Expr::Kind::Unary;
      update->text = "post" + op.text;
      update->location = op.location;
      update->children.push_back(expression);
      expression = std::move(update);
    }
    return expression;
  }

  ExprPtr parsePrimary() {
    Token token = advance();
    auto expression = std::make_shared<Expr>();
    expression->location = token.location;
    if (token.kind == Token::Kind::Identifier && token.text == "if") {
      return parseIfExpression(token);
    }
    if (token.kind == Token::Kind::Identifier && token.text == "switch") {
      return parseSwitchExpression(token);
    }
    if (token.kind == Token::Kind::Number) {
      if (token.text.rfind("f:", 0) == 0) {
        expression->kind = Expr::Kind::Float;
        expression->text = token.text.substr(2);
      } else {
        expression->kind = Expr::Kind::Integer;
        expression->text = token.text;
      }
    } else if (token.kind == Token::Kind::String) {
      expression->kind = Expr::Kind::String;
      expression->text = token.text;
    } else if (token.kind == Token::Kind::Identifier) {
      if (token.text == "true" || token.text == "false") {
        expression->kind = Expr::Kind::Boolean;
        expression->text = token.text;
      } else if (token.text == "break" || token.text == "continue") {
        throw Diagnostic(token.location,
                         "'" + token.text + "' is not supported yet; switch arms do not "
                         "fall through and v1 does not support loop break/continue");
      } else {
        expression->kind = Expr::Kind::Name;
        expression->text = token.text;
      }
    } else if (token.text == "(") {
      expression = parseExpression();
      expect(")", "expected ')' after expression");
    } else {
      throw Diagnostic(token.location, "expected an expression");
    }

    while (true) {
      if (match("(")) {
        if (expression->kind != Expr::Kind::Name) {
          throw Diagnostic(expression->location, "only named functions can be called");
        }
        auto call = std::make_shared<Expr>();
        call->kind = Expr::Kind::Call;
        call->text = expression->text;
        call->resolvedName = call->text;
        call->location = expression->location;
        if (match("<")) {
          do {
            call->typeArguments.push_back(parseType());
          } while (match(","));
          expect(">", "expected '>' after explicit generic arguments");
          if (!isSyntax("(")) {
            throw Diagnostic(peek().location,
                             "explicit generic arguments must be followed by a call");
          }
        }
        if (!match(")")) {
          do {
            call->children.push_back(parseExpression());
          } while (match(","));
          expect(")", "expected ')' after call arguments");
        }
        expression = std::move(call);
      } else if (match("[")) {
        Token open = previous();
        ExprPtr index = parseExpression();
        expect("]", "expected ']' after index");
        auto indexed = std::make_shared<Expr>();
        indexed->kind = Expr::Kind::Index;
        indexed->location = open.location;
        indexed->children = {expression, index};
        expression = std::move(indexed);
      } else if (match(".")) {
        const Token& dotToken = previous();
        (void)dotToken;
        if (peek().kind != Token::Kind::Identifier) {
          throw Diagnostic(peek().location, "expected a method name after '.'");
        }
        Token methodToken = advance();
        auto call = std::make_shared<Expr>();
        call->kind = Expr::Kind::Call;
        call->text = methodToken.text;
        call->receiver = expression->kind == Expr::Kind::Name ? expression->text : "";
        call->location = methodToken.location;
        expect("(", "expected '(' after method name");
        if (!match(")")) {
          do {
            call->children.push_back(parseExpression());
          } while (match(","));
          expect(")", "expected ')' after method arguments");
        }
        expression = std::move(call);
      } else {
        break;
      }
    }
    return expression;
  }

  std::vector<Token> tokens_;
  size_t position_ = 0;
};

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read source file: " + path.string());
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

struct FileGraph {
  std::vector<std::string> includePaths;  // resolved paths of the file's direct includes
  bool hasIo = false;
};

struct Reachable {
  std::vector<std::string> files;
  bool hasIo = false;
};

std::filesystem::path resolveIncludePath(const std::string& path, const std::filesystem::path& sourceDirectory,
                                        const std::string& standardLibraryPath, const Location& location) {
  std::filesystem::path candidate = std::filesystem::path(standardLibraryPath) / (path + ".cv");
  std::error_code error;
  if (std::filesystem::is_regular_file(candidate, error)) return candidate;
  candidate = sourceDirectory / (path + ".cv");
  if (std::filesystem::is_regular_file(candidate, error)) return candidate;
  throw Diagnostic(location, "header '" + path + "' was not found");
}

// The files visible from a file are its transitive include graph. The visiting
// set guards against include cycles: a cycle is cut rather than rejected.
Reachable reachableFiles(const std::string& key, const std::unordered_map<std::string, FileGraph>& files,
                         std::unordered_set<std::string>& visiting) {
  Reachable result;
  if (!visiting.insert(key).second) return result;
  result.files.push_back(key);
  const auto entry = files.find(key);
  if (entry != files.end()) {
    result.hasIo = entry->second.hasIo;
    for (const std::string& include : entry->second.includePaths) {
      Reachable child = reachableFiles(include, files, visiting);
      result.files.insert(result.files.end(), child.files.begin(), child.files.end());
      result.hasIo = result.hasIo || child.hasIo;
    }
  }
  visiting.erase(key);
  return result;
}

void loadModules(const std::filesystem::path& path, const std::string& standardLibraryPath, Program& program,
                 std::unordered_set<std::string>& loaded, const std::optional<Location>& errorLocation,
                 std::unordered_map<std::string, FileGraph>& files);

// A function is visible in every file that transitively includes the file that
// declares it. Builtin output is available program-wide once any file in the
// include graph reaches std/io.
void assignFunctionVisibility(Program& program, const std::unordered_map<std::string, FileGraph>& files,
                              const std::string& entryKey) {
  std::unordered_set<std::string> programVisiting;
  const Reachable programReach = reachableFiles(entryKey, files, programVisiting);
  for (FunctionDecl& function : program.functions) {
    Reachable reached;
    if (function.location.file.empty()) {
      reached.files.push_back(function.location.file);
    } else {
      std::error_code error;
      const std::string located = std::filesystem::weakly_canonical(function.location.file, error).string();
      const std::string key = error ? function.location.file : located;
      std::unordered_set<std::string> visiting;
      reached = reachableFiles(key, files, visiting);
    }
    function.visibleFiles = reached.files;
    function.hasIo = programReach.hasIo;
  }
}

void loadFromParsed(Program& parsed, const std::filesystem::path& sourcePath,
                    const std::string& standardLibraryPath, Program& program,
                    std::unordered_set<std::string>& loaded,
                    const std::optional<Location>& errorLocation, bool copyIncludes,
                    std::unordered_map<std::string, FileGraph>& files) {
  const std::string sourceKey = sourcePath.string();
  // Only the entry file's own header status propagates; included modules are
  // always headers by construction.
  if (copyIncludes && parsed.isHeader) program.isHeader = true;

  FileGraph graph;
  if (copyIncludes) program.includes = parsed.includes;
  for (const IncludeDecl& include : parsed.includes) {
    const std::filesystem::path resolved = resolveIncludePath(include.path, sourcePath.parent_path(),
                                                                standardLibraryPath, include.location);
    graph.includePaths.push_back(resolved.string());
    if (include.path == "std/io") graph.hasIo = true;
    loadModules(resolved, standardLibraryPath, program, loaded, include.location, files);
  }
  files[sourceKey] = std::move(graph);
  program.functions.insert(program.functions.end(), std::make_move_iterator(parsed.functions.begin()),
                           std::make_move_iterator(parsed.functions.end()));
}

void loadModules(const std::filesystem::path& path, const std::string& standardLibraryPath, Program& program,
                 std::unordered_set<std::string>& loaded, const std::optional<Location>& errorLocation,
                 std::unordered_map<std::string, FileGraph>& files) {
  std::filesystem::path canonical = std::filesystem::weakly_canonical(path);
  std::string key = canonical.string();
  if (!loaded.insert(key).second) return;

  Program parsed = Parser(Lexer(readFile(canonical), canonical.string()).tokenize()).parse();
  if (errorLocation && !parsed.isHeader) {
    throw Diagnostic(*errorLocation, "included file '" + path.string() + "' must begin with '#header'");
  }
  if (parsed.isHeader) {
    if (parsed.headerName.empty()) parsed.headerName = canonical.stem().string();
    for (const FunctionDecl& function : parsed.functions) {
      if (function.name == "main") {
        throw Diagnostic(function.location, "headers cannot declare 'int main()'");
      }
    }
  }
  loadFromParsed(parsed, canonical, standardLibraryPath, program, loaded, errorLocation, false, files);
}
}  // namespace

Program parseSourceText(const std::string& source) {
  return Parser(Lexer(source, "<input>").tokenize()).parse();
}

Program parseSourceText(const std::string& source, const std::string& sourceName,
                        const std::string& standardLibraryPath) {
  Program program = Parser(Lexer(source, sourceName).tokenize()).parse();
  const std::filesystem::path canonicalSource = std::filesystem::weakly_canonical(std::filesystem::path(sourceName));
  std::unordered_set<std::string> loaded;
  std::unordered_map<std::string, FileGraph> files;
  Program result;
  loadFromParsed(program, canonicalSource, standardLibraryPath, result, loaded, std::nullopt, true, files);
  assignFunctionVisibility(result, files, canonicalSource.string());
  return result;
}

Program parseSourceFile(const std::string& path, const std::string& standardLibraryPath) {
  std::filesystem::path canonical = std::filesystem::weakly_canonical(std::filesystem::path(path));
  Program program;
  std::unordered_set<std::string> loaded;
  std::unordered_map<std::string, FileGraph> files;
  Program parsed = Parser(Lexer(readFile(canonical), canonical.string()).tokenize()).parse();
  if (parsed.isHeader) {
    throw Diagnostic(Location{1, 1, canonical.string()}, "entry file cannot be marked '#header'");
  }
  loadFromParsed(parsed, canonical, standardLibraryPath, program, loaded, std::nullopt, true, files);
  assignFunctionVisibility(program, files, canonical.string());
  return program;
}
ParsedFile parseFileText(const std::string& source, const std::string& name) {
  Lexer lexer(source, name);
  ParsedFile parsed;
  parsed.tokens = lexer.tokenize();
  parsed.comments = lexer.takeComments();
  Parser parser(parsed.tokens);
  parsed.program = parser.parse();
  return parsed;
}
}  // namespace civic

