#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ast.hpp"

namespace civic {
struct Token {
  enum class Kind { Identifier, Number, String, Symbol, End };
  Kind kind;
  std::string text;
  Location location;
};

class Lexer {
public:
  explicit Lexer(const std::string& source, std::string sourceName = {})
      : source_(source), sourceName_(std::move(sourceName)) {}
  std::vector<Token> tokenize() const;

private:
  const std::string& source_;
  std::string sourceName_;
};
}  // namespace civic
