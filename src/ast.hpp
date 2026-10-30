#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace civic {
struct Location {
	size_t line	  = 1;
	size_t column = 1;
	std::string file;
};

struct Diagnostic : std::runtime_error {
	Location location;
	Diagnostic(Location where, const std::string& message)
		: std::runtime_error(message), location(where) {}
};

struct IncludeDecl {
	std::string path;
	Location location;
};

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

// One arm of a switch expression: a list of case constants, or no values at
// all for the default arm. The case values may be empty for the default arm.
struct ExprArm {
	std::vector<ExprPtr> values;
	ExprPtr body;
};

struct Expr {
	enum class Kind {
		Integer,
		Float,
		String,
		Boolean,
		Name,
		Unary,
		Binary,
		Call,
		Index,
		If,
		Switch,
		ArrayLiteral,
		MapLiteral
	};
	Kind kind;
	std::string text;
	std::string resolvedName;
	std::string receiver;
	Location location;
	std::vector<std::string> typeArguments;
	std::vector<std::shared_ptr<Expr>> children;
	std::vector<ExprArm> arms;
};

struct Stmt;
using StmtPtr = std::shared_ptr<Stmt>;

// One arm of a switch statement: a list of case constants, or no values at
// all for the default arm, plus the statement list that runs when they match.
struct SwitchArm {
	std::vector<ExprPtr> values;
	std::shared_ptr<Stmt> body;
};

struct Stmt {
	enum class Kind {
		Block,
		Variable,
		Expression,
		Return,
		If,
		While,
		For,
		Switch,
		TupleAssign
	};
	Kind kind;
	Location location;
	std::string typeName;
	std::string name;
	std::shared_ptr<Stmt> initializer;
	ExprPtr expression;
	ExprPtr step;
	std::vector<std::shared_ptr<Stmt>> children;
	std::shared_ptr<Stmt> alternative;
	std::vector<SwitchArm> switchArms;
	std::vector<ExprPtr> returnValues;
	std::vector<std::string> typeNames;
	std::vector<std::string> names;
	// For-in provenance (filled by the parser's desugaring so a printer can
	// recover the original syntax).
	bool isForIn		= false;
	bool forInInclusive = false;
	std::string forInVariable;
	ExprPtr forInIterable;
	// Last source line the statement occupies (0 when unknown). Set by the
	// parser so a printer can reason about blank lines and comments.
	size_t endLine = 0;
};

struct Parameter {
	std::string typeName;
	std::string name;
	Location location;
};

struct GenericParameter {
	std::string name;
	std::vector<std::string> bounds;
	Location location;
};

struct FunctionDecl {
	std::string returnType;
	std::vector<std::string> returnTypes;
	std::string name;
	std::string symbolName;
	std::vector<GenericParameter> genericParameters;
	std::vector<Parameter> parameters;
	StmtPtr body;
	Location location;
	std::vector<std::string> visibleFiles;
	bool hasIo = false;
};

struct Program {
	std::vector<IncludeDecl> includes;
	std::vector<FunctionDecl> functions;
	bool isHeader = false;
	std::string headerName;
};
} // namespace civic
