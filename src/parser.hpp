#pragma once

#include <string>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

namespace civic {
Program parseSourceText(const std::string& source);
Program parseSourceText(const std::string& source,
						const std::string& sourceName,
						const std::string& standardLibraryPath);
Program parseSourceFile(const std::string& path,
						const std::string& standardLibraryPath);

// Parses exactly one source file: no include resolution and no visibility
// assignment, so the result describes only the file's own text. Comments are
// recorded so the formatter can replay them.
struct ParsedFile {
	Program program;
	std::vector<Comment> comments;
	std::vector<Token> tokens;
};
ParsedFile parseFileText(const std::string& source, const std::string& name);
} // namespace civic
