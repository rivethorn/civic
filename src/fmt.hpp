#pragma once

#include <string>

#include "parser.hpp"

namespace civic {
// Reprints a single parsed file (the result of parseFileText) in canonical
// style: 4-space indentation, 100-column wrapping for statement-level
// expressions, K&R braces, and at most one blank line between declarations.
// Comments are preserved (re-indented, not reflowed). Throws Diagnostic.
std::string formatProgram(const ParsedFile& parsed);
} // namespace civic
