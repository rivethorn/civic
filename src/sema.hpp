#pragma once

#include "ast.hpp"

namespace civic {
enum class Type {
	I8,
	I16,
	I32,
	I64,
	I128,
	U8,
	U16,
	U32,
	U64,
	U128,
	Isize,
	Usize,
	F64,
	Bool,
	String,
	StringPointer,
	Void,
	Array,
	Vector,
	Map
};

Type parseTypeName(const std::string& name, Location location);
Type vectorElementType(const std::string& typeName, Location location);
Type arrayElementType(const std::string& typeName, Location location);
Type mapValueType(const std::string& typeName, Location location);
void validateProgram(const Program& program, bool requireMain = true);
} // namespace civic
