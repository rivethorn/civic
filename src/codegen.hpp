#pragma once

#include <string>

#include "ast.hpp"

namespace civic {
std::string generateLLVMIR(const Program& program, const std::string& moduleName);
}  // namespace civic
