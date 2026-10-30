#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ast.hpp"

namespace civic {
struct Token {
	enum class Kind {
		Identifier,
		Number,
		String,
		Symbol,
		End
	};
	Kind kind;
	std::string text;
	Location location;
};

// A comment captured during lexing. Order in the vector is source order.
struct Comment {
	std::string text; // full text including any // or /* */ markers, no newline
	Location location; // where the comment starts
	size_t endLine;	   // last source line the comment occupies
	bool block = false;
};

class Lexer {
public:
	explicit Lexer(const std::string& source, std::string sourceName = {})
		: source_(source), sourceName_(std::move(sourceName)) {}
	std::vector<Token> tokenize();
	std::vector<Comment> takeComments() {
		return std::move(comments_);
	}

private:
	const std::string& source_;
	std::string sourceName_;
	std::vector<Comment> comments_;
};
} // namespace civic
