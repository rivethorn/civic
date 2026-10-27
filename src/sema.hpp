#pragma once

#include "ast.hpp"

namespace civic {
enum class Type { I32, F64, Bool, String, StringPointer, Void };

Type parseTypeName(const std::string& name, Location location);
void validateProgram(const Program& program);
}  // namespace civic
