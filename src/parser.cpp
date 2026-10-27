#include "parser.hpp"

#include "lexer.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace civic {
namespace {
class Parser {
public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  Program parse() {
    Program program;
    while (!atEnd()) {
      if (match("import")) {
        ImportDecl import;
        import.location = previous().location;
        if (peek().kind == Token::Kind::String) {
          import.path = advance().text;
          import.isPath = true;
        } else {
          import.path = expectIdentifier("expected module name after 'import'").text;
          while (match(".")) {
            import.path += "." + expectIdentifier("expected module name after '.'").text;
          }
        }
        expect(";", "expected ';' after import declaration");
        program.imports.push_back(std::move(import));
      } else {
        program.functions.push_back(parseFunction());
      }
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

  std::string parseType() {
    std::string type = expectIdentifier("expected a type name").text;
    if (match("<")) {
      type += "<" + parseType();
      while (match(",")) type += "," + parseType();
      expect(">", "expected '>' after generic type arguments");
      type += ">";
    }
    while (match("*")) type += "*";
    return type;
  }

  FunctionDecl parseFunction() {
    FunctionDecl function;
    function.location = peek().location;
    function.returnType = parseType();
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
      if (!isSyntax(";")) statement->expression = parseExpression();
      expect(";", "expected ';' after return");
      return statement;
    }
    if (match("if")) {
      auto statement = std::make_shared<Stmt>();
      statement->kind = Stmt::Kind::If;
      statement->location = previous().location;
      expect("(", "expected '(' after if");
      statement->expression = parseExpression();
      expect(")", "expected ')' after condition");
      statement->children.push_back(parseStatement());
      if (match("else")) statement->alternative = parseStatement();
      return statement;
    }
    if (match("while")) {
      auto statement = std::make_shared<Stmt>();
      statement->kind = Stmt::Kind::While;
      statement->location = previous().location;
      expect("(", "expected '(' after while");
      statement->expression = parseExpression();
      expect(")", "expected ')' after condition");
      statement->children.push_back(parseStatement());
      return statement;
    }
    if (match("for")) {
      auto statement = std::make_shared<Stmt>();
      statement->kind = Stmt::Kind::For;
      statement->location = previous().location;
      expect("(", "expected '(' after for");
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
      expect(")", "expected ')' after for clauses");
      statement->children.push_back(parseStatement());
      return statement;
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
      if (match("=")) statement->expression = parseExpression();
      if (consumeTerminator) {
      expect(";", "expected ';' after variable declaration");
      }
      return statement;
  }

  bool looksLikeDeclaration() const {
    if (peek().kind != Token::Kind::Identifier) return false;
    const std::string& first = peek().text;
    bool knownType = first == "int" || first == "float" || first == "double" ||
                     first == "bool" || first == "String" || first == "vector";
    if (!knownType) return false;
    size_t afterType = 1;
    if (first == "vector" && peek(afterType).text == "<") {
      int depth = 1;
      ++afterType;
      while (depth && peek(afterType).kind != Token::Kind::End) {
        if (peek(afterType).text == "<") ++depth;
        if (peek(afterType).text == ">") --depth;
        ++afterType;
      }
    }
    while (peek(afterType).text == "*") ++afterType;
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

void mergeProgram(Program& into, Program from) {
  into.imports.insert(into.imports.end(), from.imports.begin(), from.imports.end());
  into.functions.insert(into.functions.end(),
                        std::make_move_iterator(from.functions.begin()),
                        std::make_move_iterator(from.functions.end()));
}

void loadModules(const std::filesystem::path& path,
                 const std::filesystem::path& standardLibraryPath, Program& program,
                 std::unordered_set<std::string>& loaded);
void loadImports(const std::vector<ImportDecl>& imports,
                 const std::filesystem::path& sourcePath,
                 const std::filesystem::path& standardLibraryPath, Program& program,
                 std::unordered_set<std::string>& loaded);

void loadModules(const std::filesystem::path& path,
                 const std::filesystem::path& standardLibraryPath, Program& program,
                 std::unordered_set<std::string>& loaded) {
  std::filesystem::path canonical = std::filesystem::weakly_canonical(path);
  std::string key = canonical.string();
  if (!loaded.insert(key).second) return;

  Program parsed = Parser(Lexer(readFile(canonical), canonical.string()).tokenize()).parse();
  auto imports = parsed.imports;
  mergeProgram(program, std::move(parsed));
  loadImports(imports, canonical, standardLibraryPath, program, loaded);
}

void loadImports(const std::vector<ImportDecl>& imports,
                 const std::filesystem::path& sourcePath,
                 const std::filesystem::path& standardLibraryPath, Program& program,
                 std::unordered_set<std::string>& loaded) {
  for (const ImportDecl& import : imports) {
    if (!import.isPath) {
      if (import.path == "std.io" || import.path == "std.core") {
        continue;
      }
      std::filesystem::path modulePath = standardLibraryPath / "std";
      std::string relativeModule = import.path;
      if (relativeModule.rfind("std.", 0) != 0) {
        throw Diagnostic(import.location, "unknown standard module '" + import.path + "'");
      }
      relativeModule.erase(0, 4);
      for (char& ch : relativeModule) {
        if (ch == '.') ch = std::filesystem::path::preferred_separator;
      }
      modulePath /= relativeModule + ".cv";
      if (!std::filesystem::is_regular_file(modulePath)) {
        throw Diagnostic(import.location, "standard module '" + import.path + "' was not found");
      }
      loadModules(modulePath, standardLibraryPath, program, loaded);
      continue;
    }
    std::filesystem::path importedPath(import.path);
    if (importedPath.extension() != ".cv") importedPath += ".cv";
    if (importedPath.is_relative()) importedPath = sourcePath.parent_path() / importedPath;
    loadModules(importedPath, standardLibraryPath, program, loaded);
  }
}
}  // namespace

Program parseSourceText(const std::string& source) {
  return Parser(Lexer(source, "<input>").tokenize()).parse();
}

Program parseSourceText(const std::string& source, const std::string& sourceName,
                        const std::string& standardLibraryPath) {
  Program program = Parser(Lexer(source, sourceName).tokenize()).parse();
  const std::filesystem::path canonicalSource =
      std::filesystem::weakly_canonical(sourceName);
  std::unordered_set<std::string> loaded{canonicalSource.string()};
  loadImports(program.imports, canonicalSource,
              std::filesystem::path(standardLibraryPath), program, loaded);
  return program;
}

Program parseSourceFile(const std::string& path, const std::string& standardLibraryPath) {
  Program program;
  std::unordered_set<std::string> loaded;
  loadModules(path, standardLibraryPath, program, loaded);
  return program;
}
}  // namespace civic
