#include "lexer.hpp"

#include <cctype>
#include <string>

namespace civic {
std::vector<Token> Lexer::tokenize() const {
  std::vector<Token> tokens;
  size_t offset = 0;
  size_t line = 1;
  size_t column = 1;
  auto location = [&]() { return Location{line, column, sourceName_}; };

  auto advance = [&]() {
    char ch = source_[offset++];
    if (ch == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
    return ch;
  };
  auto add = [&](Token::Kind kind, std::string text, Location location) {
    tokens.push_back({kind, std::move(text), location});
  };

  while (offset < source_.size()) {
    char ch = source_[offset];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      advance();
      continue;
    }
    if (ch == '/' && offset + 1 < source_.size() && source_[offset + 1] == '/') {
      while (offset < source_.size() && advance() != '\n') {}
      continue;
    }
    if (ch == '/' && offset + 1 < source_.size() && source_[offset + 1] == '*') {
      Location start = location();
      advance();
      advance();
      bool closed = false;
      while (offset + 1 < source_.size()) {
        if (source_[offset] == '*' && source_[offset + 1] == '/') {
          advance();
          advance();
          closed = true;
          break;
        }
        advance();
      }
      if (!closed) {
        throw Diagnostic(start, "unterminated block comment");
      }
      continue;
    }

    Location start = location();
    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
      std::string text;
      while (offset < source_.size() &&
             (std::isalnum(static_cast<unsigned char>(source_[offset])) || source_[offset] == '_')) {
        text.push_back(advance());
      }
      add(Token::Kind::Identifier, text, start);
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      std::string text;
      while (offset < source_.size() && std::isdigit(static_cast<unsigned char>(source_[offset]))) {
        text.push_back(advance());
      }
      Token::Kind kind = Token::Kind::Number;
      if (offset < source_.size() && source_[offset] == '.' &&
          offset + 1 < source_.size() && std::isdigit(static_cast<unsigned char>(source_[offset + 1]))) {
        kind = Token::Kind::Number;
        text.push_back(advance());
        while (offset < source_.size() && std::isdigit(static_cast<unsigned char>(source_[offset]))) {
          text.push_back(advance());
        }
        text.insert(0, "f:");
      }
      add(kind, text, start);
      continue;
    }
    if (ch == '"') {
      advance();
      std::string text;
      bool closed = false;
      while (offset < source_.size()) {
        char next = advance();
        if (next == '"') {
          closed = true;
          break;
        }
        if (next == '\n') {
          throw Diagnostic(start, "newline in string literal");
        }
        if (next == '\\') {
          if (offset == source_.size()) break;
          char escaped = advance();
          switch (escaped) {
            case 'n': text.push_back('\n'); break;
            case 'r': text.push_back('\r'); break;
            case 't': text.push_back('\t'); break;
            case '\\': text.push_back('\\'); break;
            case '"': text.push_back('"'); break;
            default: throw Diagnostic({line, column - 1, sourceName_}, "unsupported escape sequence");
          }
        } else {
          text.push_back(next);
        }
      }
      if (!closed) {
        throw Diagnostic(start, "unterminated string literal");
      }
      add(Token::Kind::String, text, start);
      continue;
    }

    static const char* const operators[] = {
        "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/="};
    bool matched = false;
    for (const char* op : operators) {
      std::string candidate(op);
      if (source_.compare(offset, candidate.size(), candidate) == 0) {
        for (size_t i = 0; i < candidate.size(); ++i) advance();
        add(Token::Kind::Symbol, candidate, start);
        matched = true;
        break;
      }
    }
    if (matched) continue;

    static const std::string symbols = "{}();,.:<>+-*/%=!&|[]";
    if (symbols.find(ch) != std::string::npos) {
      add(Token::Kind::Symbol, std::string(1, advance()), start);
      continue;
    }
    throw Diagnostic(start, std::string("unexpected character '") + ch + "'");
  }

  tokens.push_back({Token::Kind::End, "", location()});
  return tokens;
}
}  // namespace civic
