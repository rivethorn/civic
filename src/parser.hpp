#pragma once

#include <string>

#include "ast.hpp"

namespace civic {
Program parseSourceText(const std::string& source);
Program parseSourceText(const std::string& source, const std::string& sourceName,
                        const std::string& standardLibraryPath);
Program parseSourceFile(const std::string& path, const std::string& standardLibraryPath);
}  // namespace civic
