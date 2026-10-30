#include "fmt.hpp"

#include "lexer.hpp"

#include <sstream>

namespace civic {
namespace {
constexpr size_t kWidth		= 100;
constexpr size_t kMultiline = static_cast<size_t>(-1);

int binaryPrecedence(const std::string& op) {
	if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=")
		return 1;
	if (op == "||")
		return 2;
	if (op == "&&")
		return 3;
	if (op == "==" || op == "!=")
		return 4;
	if (op == "<" || op == "<=" || op == ">" || op == ">=")
		return 5;
	if (op == "+" || op == "-")
		return 6;
	if (op == "*" || op == "/" || op == "%")
		return 7;
	return 0;
}

// Inserts a space after commas inside generic type arguments so that
// `map<string,int>` prints as `map<string, int>`; safe because commas can
// only appear inside <...> in these type name strings.
std::string canonicalizeType(const std::string& type) {
	std::string out;
	size_t depth = 0;
	for (char ch : type) {
		if (ch == '<')
			++depth;
		if (ch == '>')
			--depth;
		if (depth > 0 && ch == ',') {
			out += ", ";
			continue;
		}
		if (depth > 0 && ch == ' ')
			continue;
		out.push_back(ch);
	}
	return out;
}

std::string escapeString(const std::string& text) {
	std::string out;
	for (char ch : text) {
		switch (ch) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out.push_back(ch);
		}
	}
	return out;
}

std::vector<std::string> splitLines(const std::string& text) {
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= text.size()) {
		size_t stop = text.find('\n', start);
		if (stop == std::string::npos) {
			lines.push_back(text.substr(start));
			break;
		}
		lines.push_back(text.substr(start, stop - start));
		start = stop + 1;
	}
	return lines;
}

class Formatter {
public:
	explicit Formatter(const ParsedFile& parsed) : parsed_(parsed) {
		for (const Comment& comment : parsed.comments)
			comments_.push_back(&comment);
	}

	std::string format() {
		const Program& program = parsed_.program;
		size_t prevEnd		   = 0;
		if (program.isHeader) {
			statementLine_ = 1;
			put("#header");
			if (!program.headerName.empty())
				put(" " + program.headerName);
			finishTopLine(1);
			prevEnd = 1;
		}
		for (const IncludeDecl& include : program.includes) {
			flushCommentsBefore(include.location.line, prevEnd);
			if (prevEnd > 0 && include.location.line > prevEnd + 1)
				blankLine();
			statementLine_ = include.location.line;
			emitDirective("#include", include.path);
			const size_t trailing = flushTrailing(include.location.line);
			prevEnd				  = std::max(prevEnd, trailing);
			finishTopLine(include.location.line);
			prevEnd = std::max(prevEnd, include.location.line);
		}
		for (const FunctionDecl& function : program.functions) {
			flushCommentsBefore(function.location.line, prevEnd);
			// Canonical: exactly one blank line between top-level declarations,
			// unless a comment already sits directly above the declaration.
			const bool commentHugs =
				lastCommentEnd_ > 0 &&
				lastCommentEnd_ + 1 >= function.location.line;
			if (prevEnd > 0 && !commentHugs)
				blankLine();
			statementLine_ = function.location.line;
			emitFunctionDecl(function);
			prevEnd = function.body ? endLineOf(function.body)
									: function.location.line;
		}
		while (cursor_ <
			   comments_.size()) { // comments after the last declaration
			const Comment& comment = *comments_[cursor_];
			if (comment.location.line > prevEnd + 1 && prevEnd > 0)
				blankLine();
			emitComment(comment);
			++cursor_;
			prevEnd = comment.endLine;
		}
		if (!piece_.empty())
			endLine(); // every content line already ended
		return out_.str();
	}

private:
	// ---------------------------------------------------------------- output

	void put(const std::string& text) {
		piece_ += text;
	}

	void endLine(size_t extraIndent = 0) {
		out_ << std::string(indent_ + extraIndent, '\t') << piece_ << '\n';
		piece_.clear();
		++linesWritten_;
	}

	void blankLine() {
		// Never strand a pending piece: flush it before writing the gap.
		if (!piece_.empty())
			endLine();
		out_ << '\n';
	}

	std::string indentString() const {
		return std::string(indent_, '\t');
	}

	size_t column() const {
		return indent_ + piece_.size();
	}

	bool fits(size_t width) const {
		return width != kMultiline && column() + width <= kWidth;
	}

	void emitDirective(const char* keyword, const std::string& value) {
		put(keyword);
		if (!value.empty())
			put(" <" + value + ">");
	}

	// ---------------------------------------------------------------- probes

	// Snapshot restored after measuring a construct so real output is
	// untouched.
	struct ProbeState {
		std::string out;
		std::string piece;
		size_t indent		  = 0;
		size_t cursor		  = 0;
		size_t statementLine  = 0;
		size_t linesWritten	  = 0;
		size_t lastCommentEnd = 0;
		bool breakAtEnd		  = true;
		bool probing		  = false;
	};

	ProbeState saveProbe() const {
		ProbeState state;
		state.out			 = out_.str();
		state.piece			 = piece_;
		state.indent		 = indent_;
		state.cursor		 = cursor_;
		state.statementLine	 = statementLine_;
		state.linesWritten	 = linesWritten_;
		state.lastCommentEnd = lastCommentEnd_;
		state.breakAtEnd	 = breakAtEnd_;
		state.probing		 = probing_;
		return state;
	}

	void restoreProbe(const ProbeState& state) {
		out_.str(state.out);
		out_.clear();
		// str() rewinds the write position; re-seek to the end so real output
		// appends after the restored content instead of overwriting it.
		out_.seekp(0, std::ios::end);
		piece_			= state.piece;
		indent_			= state.indent;
		cursor_			= state.cursor;
		statementLine_	= state.statementLine;
		linesWritten_	= state.linesWritten;
		lastCommentEnd_ = state.lastCommentEnd;
		breakAtEnd_		= state.breakAtEnd;
		probing_		= state.probing;
	}

	// ------------------------------------------------------------- measuring

	// Width of a one-line rendering relative to the current column.
	size_t measureExpr(const ExprPtr& expression, int minPrec) {
		if (!expression)
			return 0;
		ProbeState state	= saveProbe();
		probing_			= true;
		const size_t before = piece_.size();
		emitExpr(expression, minPrec);
		const size_t width	 = piece_.size() - before;
		const bool multiline = linesWritten_ != state.linesWritten;
		restoreProbe(state);
		return multiline ? kMultiline : width;
	}

	size_t measureStmt(const StmtPtr& statement) {
		ProbeState state	= saveProbe();
		probing_			= true;
		statementLine_		= statement->location.line;
		breakAtEnd_			= false;
		const size_t before = piece_.size();
		emitStmt(statement, endLineOf(statement));
		const size_t width	 = piece_.size() - before;
		const bool multiline = linesWritten_ != state.linesWritten;
		restoreProbe(state);
		return multiline ? kMultiline : width;
	}

	// Renders an expression to a single-line string. When the construct has a
	// multiline layout, nesting a probe forces fallbacks, but any spill is
	// joined with spaces so callers always get coherent text.
	std::string renderLine(const ExprPtr& expression, int minPrec) {
		ProbeState state		 = saveProbe();
		probing_				 = true;
		const size_t pieceBefore = piece_.size();
		emitExpr(expression, minPrec);
		std::string text;
		if (linesWritten_ != state.linesWritten) {
			text = joinedProbeLines(state.out, piece_.substr(pieceBefore));
		} else {
			text = piece_.substr(pieceBefore);
		}
		restoreProbe(state);
		return text;
	}

	std::string joinedProbeLines(const std::string& saved,
								 const std::string& tail) const {
		const std::string after = out_.str();
		const std::string added = after.size() >= saved.size()
									  ? after.substr(saved.size())
									  : std::string();
		std::string joined;
		auto push = [&joined](const std::string& line) {
			const size_t first = line.find_first_not_of(" \t");
			if (first == std::string::npos)
				return;
			if (!joined.empty())
				joined += " ";
			joined += line.substr(first);
		};
		for (const std::string& line : splitLines(added))
			push(line);
		push(tail);
		return joined;
	}

	// ------------------------------------------------------------ statements

	void emitFunctionDecl(const FunctionDecl& function) {
		std::string result;
		for (size_t index = 0; index < function.returnTypes.size(); ++index) {
			if (index > 0)
				result += ", ";
			result += canonicalizeType(function.returnTypes[index]);
		}
		put(result);
		put(" " + function.name);
		if (!function.genericParameters.empty()) {
			put("<");
			for (size_t index = 0; index < function.genericParameters.size();
				 ++index) {
				if (index > 0)
					put(", ");
				put(function.genericParameters[index].name);
			}
			put(">");
		}
		emitParameterList(function.parameters);
		if (!function.body) { // forward declaration
			put(";");
			endLine();
			return;
		}
		if (function.body->children.empty()) {
			put(" {}");
			endLine();
			return;
		}
		put(" {");
		endLine();
		emitStmtList(function.body->children, 0, function.body->location.line);
		put("}");
		endLine();
	}

	void emitParameterList(const std::vector<Parameter>& parameters) {
		std::vector<std::string> rendered;
		size_t width = 0;
		for (const Parameter& parameter : parameters) {
			rendered.push_back(canonicalizeType(parameter.typeName) + " " +
							   parameter.name);
			width += rendered.back().size();
		}
		const bool multiline =
			probing_
				? false
				: !parameters.empty() &&
					  (parameters.size() > 3 ||
					   column() + width + 2 * parameters.size() + 2 > kWidth);
		put("(");
		if (multiline) {
			// Prefix line ends here; parameters belong on their own rows and
			// the closer glues onto the row the caller finishes with `) {`.
			endLine();
			++indent_;
			for (size_t index = 0; index < rendered.size(); ++index) {
				put(rendered[index] + (index + 1 < rendered.size() ? "," : ""));
				endLine();
			}
			--indent_;
			put(")");
			return;
		}
		for (size_t index = 0; index < rendered.size(); ++index) {
			if (index > 0)
				put(", ");
			put(rendered[index]);
		}
		put(")");
	}

	void emitStmtList(const std::vector<StmtPtr>& statements, size_t from,
					  size_t openLine) {
		++indent_;
		size_t prevEnd = openLine;
		for (size_t index = from; index < statements.size(); ++index) {
			const StmtPtr& stmt	   = statements[index];
			const size_t sourceEnd = endLineOf(stmt);
			flushCommentsBefore(stmt->location.line,
								index > from ? prevEnd : openLine);
			const size_t commentEnd = lastCommentEnd_;
			if (index > from) {
				const size_t reference = std::max(prevEnd, commentEnd);
				if (stmt->location.line > reference + 1)
					blankLine();
			}
			emitStmt(stmt, sourceEnd);
			prevEnd = std::max(std::max(prevEnd, sourceEnd), commentEnd);
		}
		--indent_;
	}

	void emitStmt(const StmtPtr& statement, size_t sourceEnd) {
		statementLine_ = statement->location.line;
		switch (statement->kind) {
		case Stmt::Kind::Block:
			emitBlock(statement);
			break;
		case Stmt::Kind::Variable:
			emitVariable(statement);
			break;
		case Stmt::Kind::Expression:
			emitExpressionStatement(statement);
			break;
		case Stmt::Kind::Return:
			emitReturn(statement);
			break;
		case Stmt::Kind::If:
			emitIf(statement);
			break;
		case Stmt::Kind::While:
			emitWhile(statement);
			break;
		case Stmt::Kind::For:
			emitFor(statement);
			break;
		case Stmt::Kind::Switch:
			emitSwitchStatement(statement);
			break;
		case Stmt::Kind::TupleAssign: {
			std::string names;
			for (size_t index = 0; index < statement->names.size(); ++index) {
				if (index > 0)
					names += ", ";
				names += statement->names[index];
			}
			emitValue(names + " =", statement->expression, true);
			break;
		}
		}
		// A nested body may suppress the final newline so caller text (like
		// `else`) can glue onto the same line; finishStmt resets the flag.
		finishStmt(sourceEnd);
	}

	void finishStmt(size_t sourceEnd) {
		flushTrailing(sourceEnd);
		if (breakAtEnd_) {
			if (!piece_.empty())
				endLine();
		} else {
			breakAtEnd_ = true;
		}
	}

	void finishTopLine(size_t sourceEnd) {
		flushTrailing(sourceEnd);
		if (!piece_.empty())
			endLine();
	}

	void emitBlock(const StmtPtr& statement) {
		if (statement->children.empty()) {
			put("{}");
			return;
		}
		put("{");
		endLine();
		emitStmtList(statement->children, 0, statement->location.line);
		put("}");
	}

	void emitVariable(const StmtPtr& statement) {
		if (!statement->expression) {
			put(variableHead(statement) + ";");
			return;
		}
		emitValue(variableHead(statement) + " =", statement->expression, true);
	}

	std::string variableHead(const StmtPtr& statement) {
		std::vector<std::pair<std::string, std::string>> bindings;
		if (statement->names.empty()) {
			bindings.emplace_back(canonicalizeType(statement->typeName),
								  statement->name);
		} else {
			for (size_t index = 0; index < statement->names.size(); ++index) {
				bindings.emplace_back(
					canonicalizeType(statement->typeNames[index]),
					statement->names[index]);
			}
		}
		std::string head;
		for (size_t index = 0; index < bindings.size(); ++index) {
			if (index > 0)
				head += ", ";
			head += bindings[index].first + " " + bindings[index].second;
		}
		return head;
	}

	// Emits `<head> <expression>[;]`, wrapping the expression when it would
	// overflow the line.
	void emitValue(const std::string& head, const ExprPtr& expression,
				   bool semicolon) {
		put(head);
		const size_t width = measureExpr(expression, 1);
		if (fits(width + 1)) {
			put(" ");
			emitExpr(expression, 1);
		} else if (expression->kind == Expr::Kind::Call ||
				   expression->kind == Expr::Kind::ArrayLiteral ||
				   expression->kind == Expr::Kind::MapLiteral) {
			put(" ");
			emitExprKeepIndent(expression, 1);
		} else {
			// Head line ends here; the value continues on the next, indented
			// row, and long binary chains break one operator per row.
			endLine();
			emitExprBroken(expression, 1, 1, 1);
			put(semicolon ? ";" : "");
			endLine(1);
			return;
		}
		if (semicolon)
			put(";");
	}

	void emitExprKeepIndent(const ExprPtr& expression, int minPrec) {
		if (expression->kind == Expr::Kind::Call)
			emitCall(expression, minPrec, true);
		else if (expression->kind == Expr::Kind::ArrayLiteral ||
				 expression->kind == Expr::Kind::MapLiteral)
			emitLiteral(expression);
		else
			emitExpr(expression, minPrec);
	}

	// Width check relative to the row where the content will actually print:
	// continuation rows print at indent_ + depth.
	bool fitsAt(size_t width, size_t depth, size_t followRoom = 0) const {
		return width != kMultiline &&
			   (indent_ + depth) + piece_.size() + width + followRoom <=
				   kWidth;
	}

	// Emits an expression that does not fit beside its head: it is placed on
	// the next row (the caller ends the head line) and long binary chains
	// break one operator per row. `followRoom` reserves room for the trailing
	// text (operator or `;`) that the caller appends, so no committed row can
	// exceed the width later. Pending piece content belongs to the caller.
	void emitExprBroken(const ExprPtr& expression, int minPrec, size_t depth,
						size_t followRoom) {
		if (fitsAt(measureExpr(expression, minPrec), depth, followRoom)) {
			emitExpr(expression, minPrec);
			return;
		}
		if (expression->kind == Expr::Kind::Binary) {
			const int own = binaryPrecedence(expression->text);
			if (own > 0) {
				const bool rightAssoc = own == 1;
				const int leftPrec	  = rightAssoc ? own + 1 : own;
				const int rightPrec	  = rightAssoc ? own : own + 1;
				const size_t opRoom	  = 1 + expression->text.size();
				if (fitsAt(measureExpr(expression->children[0], leftPrec),
						   depth, opRoom + followRoom)) {
					emitExpr(expression->children[0], leftPrec);
				} else {
					emitExprBroken(expression->children[0], leftPrec, depth,
								   opRoom + followRoom);
				}
				put(" " + expression->text);
				endLine(
					depth); // row always ends with the dangling operator here
				emitExprBroken(expression->children[1], rightPrec, depth,
							   followRoom);
				return;
			}
		}
		emitExprKeepIndent(expression, minPrec);
	}

	void emitExpressionStatement(const StmtPtr& statement) {
		const ExprPtr& expression = statement->expression;
		const size_t width		  = measureExpr(expression, 1);
		if (fits(width + 1)) {
			emitExpr(expression, 1);
			put(";");
			return;
		}
		const bool assignment =
			expression->kind == Expr::Kind::Binary &&
			(expression->text == "=" || expression->text == "+=" ||
			 expression->text == "-=" || expression->text == "*=" ||
			 expression->text == "/=");
		if (assignment) {
			emitExpr(expression->children[0], 9);
			put(" " + expression->text);
			endLine();
			emitExprBroken(expression->children[1], 1, 1, 1);
			put(";");
			endLine(1);
			return;
		}
		emitExprBroken(expression, 1, 1, 1);
		put(";");
		endLine(1);
	}

	void emitReturn(const StmtPtr& statement) {
		if (statement->returnValues.empty()) {
			put("return;");
			return;
		}
		std::vector<size_t> widths;
		size_t width = 0;
		for (const ExprPtr& value : statement->returnValues) {
			const size_t item = measureExpr(value, 1);
			widths.push_back(item);
			width += item;
		}
		if (fits(width + 2 * widths.size() + 9)) {
			put("return");
			for (size_t index = 0; index < statement->returnValues.size();
				 ++index) {
				if (index > 0)
					put(",");
				put(" ");
				emitExpr(statement->returnValues[index], 1);
			}
			put(";");
			return;
		}
		put("return");
		endLine();
		for (size_t index = 0; index < statement->returnValues.size();
			 ++index) {
			emitExprBroken(statement->returnValues[index], 1, 1, 1);
			put(index + 1 < statement->returnValues.size() ? "," : ";");
			endLine(1);
		}
	}

	void emitIf(const StmtPtr& statement) {
		put("if (" + renderLine(statement->expression, 1) + ")");
		emitBody(statement->children[0]);
		if (statement->alternative) {
			put(" else");
			if (statement->alternative->kind == Stmt::Kind::If) {
				emitIf(statement->alternative);
			} else {
				emitBody(statement->alternative);
			}
		}
	}

	void emitWhile(const StmtPtr& statement) {
		put("while (" + renderLine(statement->expression, 1) + ")");
		emitBody(statement->children[0]);
	}

	void emitFor(const StmtPtr& statement) {
		if (statement->isForIn && statement->forInIterable) {
			put("for " + statement->forInVariable + " in " +
				renderLine(statement->forInIterable, 1));
			// children[0] is the sugar block: [auto binding, ...user body].
			// Print one brace pair around the user's statements, flattening
			// their block so repeated formatting never grows nesting.
			const StmtPtr sugar = statement->children[0];
			std::vector<StmtPtr> statements;
			if (sugar->children.size() > 1 &&
				sugar->children[1]->kind == Stmt::Kind::Block) {
				statements = sugar->children[1]->children;
			} else {
				statements.assign(sugar->children.begin() + 1,
								  sugar->children.end());
			}
			emitStatementBlock(statements, statement->location.line);
			return;
		}
		if (statement->isForIn) {
			put("for " + statement->forInVariable + " in " +
				renderLine(statement->initializer->expression, 1) +
				(statement->forInInclusive ? "..=" : "..") +
				renderLine(statement->expression->children[1], 1));
			if (statement->step && statement->step->text == "+=") {
				put(" step " + renderLine(statement->step->children[1], 1));
			}
			if (statement->children[0]->kind == Stmt::Kind::Block) {
				emitStatementBlock(statement->children[0]->children,
								   statement->location.line);
			} else {
				put(" {");
				endLine();
				++indent_;
				emitStmt(statement->children[0],
						 endLineOf(statement->children[0]));
				--indent_;
				put("}");
			}
			return;
		}
		put("for (");
		if (statement->initializer) {
			if (statement->initializer->kind == Stmt::Kind::Variable) {
				put(variableHead(statement->initializer));
				if (statement->initializer->expression) {
					emitValue(" =", statement->initializer->expression, false);
				}
			} else if (statement->initializer->expression) {
				emitExpr(statement->initializer->expression, 1);
			}
		}
		put("; " +
			(statement->expression ? renderLine(statement->expression, 1)
								   : "") +
			"; ");
		if (statement->step)
			put(renderLine(statement->step, 1));
		put(")");
		emitBody(statement->children[0]);
	}

	void emitBlockBody(const StmtPtr& body, size_t from) {
		put(" {");
		endLine();
		emitStmtList(body->children, from, body->location.line);
		put("}");
	}

	// Prints `{ ... }` around the given statements (loop/branch bodies).
	void emitStatementBlock(const std::vector<StmtPtr>& statements,
							size_t openLine) {
		if (statements.empty()) {
			put(" {}");
			return;
		}
		put(" {");
		endLine();
		emitStmtList(statements, 0, openLine);
		put("}");
	}

	void emitSwitchStatement(const StmtPtr& statement) {
		put("switch (" + renderLine(statement->expression, 1) + ") {");
		endLine();
		++indent_;
		for (const SwitchArm& arm : statement->switchArms) {
			emitSwitchArm(arm);
		}
		--indent_;
		put("}");
	}

	// Arms always keep their braces so the arm's block scope survives.
	void emitSwitchArm(const SwitchArm& arm) {
		put(armLabel(arm));
		bool compact = false;
		if (arm.body->children.size() == 1) {
			const size_t inner = measureStmt(arm.body->children[0]);
			compact			   = inner != kMultiline && fits(inner + 4);
		}
		if (compact) {
			put(" { ");
			breakAtEnd_ = false;
			emitStmt(arm.body->children[0], endLineOf(arm.body->children[0]));
			put(" }");
			endLine();
			return;
		}
		put(" {");
		endLine();
		emitStmtList(arm.body->children, 0, arm.body->location.line);
		put("}");
		endLine();
	}

	std::string armLabel(const SwitchArm& arm) {
		if (arm.values.empty())
			return "default:";
		std::string label = "case";
		for (size_t index = 0; index < arm.values.size(); ++index) {
			label +=
				(index > 0 ? ", " : " ") + renderLine(arm.values[index], 1);
		}
		return label + ":";
	}

	// Prints a single-statement (non-braced) if/while/for body.
	void emitBody(const StmtPtr& body) {
		if (body->kind != Stmt::Kind::Block) {
			breakAtEnd_ = false;
			emitStmt(body, endLineOf(body));
			return;
		}
		emitBlockBody(body, 0);
	}

	// ------------------------------------------------------------ expressions

	int precedenceOf(const ExprPtr& expression) const {
		switch (expression->kind) {
		case Expr::Kind::Unary:
			return 8;
		case Expr::Kind::Binary:
			return binaryPrecedence(expression->text);
		case Expr::Kind::If:
			return expression->text == "?" ? 1 : 9;
		default:
			return 9;
		}
	}

	void emitExpr(const ExprPtr& expression, int minPrec) {
		if (!expression)
			return;
		const int own			 = precedenceOf(expression);
		const bool parenthesized = own < minPrec;
		if (parenthesized)
			put("(");
		switch (expression->kind) {
		case Expr::Kind::Integer:
		case Expr::Kind::Float:
		case Expr::Kind::Boolean:
			put(expression->text);
			break;
		case Expr::Kind::String:
			put("\"" + escapeString(expression->text) + "\"");
			break;
		case Expr::Kind::Name:
			put(expression->text);
			break;
		case Expr::Kind::Unary:
			emitUnary(expression);
			break;
		case Expr::Kind::Binary:
			emitBinary(expression);
			break;
		case Expr::Kind::Call:
			emitCall(expression, 1, false);
			break;
		case Expr::Kind::Index:
			emitExpr(expression->children[0], 9);
			put("[");
			emitExpr(expression->children[1], 1);
			put("]");
			break;
		case Expr::Kind::If:
			if (expression->text == "?")
				emitTernary(expression);
			else
				emitKeywordIf(expression);
			break;
		case Expr::Kind::Switch:
			emitSwitchExpr(expression);
			break;
		case Expr::Kind::ArrayLiteral:
		case Expr::Kind::MapLiteral:
			emitLiteral(expression);
			break;
		}
		if (parenthesized)
			put(")");
	}

	void emitUnary(const ExprPtr& expression) {
		if (expression->text == "post++" || expression->text == "post--") {
			// Postfix desugar marker stored with a `post` prefix.
			emitExpr(expression->children[0], 8);
			put(expression->text.substr(4));
			return;
		}
		put(expression->text);
		const ExprPtr& operand = expression->children[0];
		// `-(-x)` would lex as `--`, so adjacent +/- unaries re-parenthesize.
		const bool adjacencyHazard =
			(expression->text == "+" || expression->text == "-") &&
			operand->kind == Expr::Kind::Unary &&
			(operand->text == "+" || operand->text == "-");
		if (adjacencyHazard) {
			put("(");
			emitExpr(operand, 1);
			put(")");
		} else {
			emitExpr(operand, 8);
		}
	}

	void emitBinary(const ExprPtr& expression) {
		const int own		  = binaryPrecedence(expression->text);
		const bool rightAssoc = own == 1;
		emitExpr(expression->children[0], rightAssoc ? own + 1 : own);
		put(" " + expression->text + " ");
		emitExpr(expression->children[1], rightAssoc ? own : own + 1);
	}

	void emitTernary(const ExprPtr& expression) {
		emitExpr(expression->children[0], 1);
		put(" ? ");
		emitExpr(expression->children[1], 2);
		put(" : ");
		emitExpr(expression->children[2], 1);
	}

	void emitKeywordIf(const ExprPtr& expression) {
		put("if (" + renderLine(expression->children[0], 1) + ") { ");
		emitExpr(expression->children[1], 1);
		put(" }");
		const ExprPtr& elseArm = expression->children[2];
		if (elseArm->kind == Expr::Kind::If && elseArm->text.empty()) {
			put(" else ");
			emitExpr(elseArm, 9);
		} else {
			put(" else { ");
			emitExpr(elseArm, 1);
			put(" }");
		}
	}

	void emitSwitchExpr(const ExprPtr& expression) {
		if (probing_) {
			// Inline rendering for fit probes and line-pieces.
			put("switch (" + renderLine(expression->children[0], 1) + ") {");
			for (const ExprArm& arm : expression->arms) {
				put(" " + switchExprArm(arm));
			}
			put(" }");
			return;
		}
		put("switch (" + renderLine(expression->children[0], 1) + ") {");
		endLine();
		++indent_;
		for (const ExprArm& arm : expression->arms) {
			put(switchExprArm(arm));
			endLine();
		}
		--indent_;
		put("}");
	}

	std::string switchExprArm(const ExprArm& arm) {
		if (arm.values.empty()) {
			return "default: " + renderLine(arm.body, 1);
		}
		std::string label = "case";
		for (size_t index = 0; index < arm.values.size(); ++index) {
			label +=
				(index > 0 ? ", " : " ") + renderLine(arm.values[index], 1);
		}
		return label + ": " + renderLine(arm.body, 1);
	}

	void emitCall(const ExprPtr& call, int /*minPrec*/,
				  bool allowMultilineArgs) {
		const size_t columnBefore = column();
		std::string prefix;
		if (!call->receiver.empty())
			prefix += call->receiver + ".";
		prefix += call->text;
		if (!call->typeArguments.empty()) {
			prefix += "<";
			for (size_t index = 0; index < call->typeArguments.size();
				 ++index) {
				if (index > 0)
					prefix += ", ";
				prefix += canonicalizeType(call->typeArguments[index]);
			}
			prefix += ">";
		}
		std::vector<std::string> args;
		size_t argsWidth = 0;
		for (const ExprPtr& argument : call->children) {
			args.push_back(renderLine(argument, 1));
			argsWidth += args.back().size();
		}
		if (args.size() > 0)
			argsWidth += 2 * (args.size() - 1);
		const bool multiline =
			allowMultilineArgs && !probing_ && !args.empty() &&
			columnBefore + prefix.size() + argsWidth + 4 > kWidth;
		put(prefix + "(");
		if (multiline) {
			// Prefix line ends here; each argument takes a row, the closer is a
			// bare `)` (calls cannot carry a trailing comma).
			endLine();
			++indent_;
			for (size_t index = 0; index < args.size(); ++index) {
				put(args[index] + (index + 1 < args.size() ? "," : ""));
				endLine();
			}
			--indent_;
			put(")");
			return;
		}
		for (size_t index = 0; index < args.size(); ++index) {
			if (index > 0)
				put(", ");
			put(args[index]);
		}
		put(")");
	}

	void emitLiteral(const ExprPtr& literal) {
		const bool isMap = literal->kind == Expr::Kind::MapLiteral;
		std::vector<std::string> rendered;
		size_t inlineWidth = 2; // brackets
		for (const ExprPtr& child : literal->children) {
			rendered.push_back(renderLine(child, 1));
			inlineWidth += rendered.back().size();
		}
		for (size_t index = 1; index < rendered.size(); ++index) {
			inlineWidth += 2; // ", / ": " separators
		}
		const bool multiline =
			!probing_ && !rendered.empty() && !fits(inlineWidth);
		put(isMap ? "{" : "[");
		if (multiline) {
			// Collection literals keep a trailing comma when spread over lines.
			endLine();
			++indent_;
			if (isMap) {
				for (size_t index = 0; index + 1 < rendered.size();
					 index += 2) {
					put(rendered[index] + ": " + rendered[index + 1] + ",");
					endLine();
				}
			} else {
				for (const std::string& item : rendered) {
					put(item + ",");
					endLine();
				}
			}
			--indent_;
			put(isMap ? "}" : "]");
			return;
		}
		for (size_t index = 0; index < rendered.size(); ++index) {
			if (index > 0)
				put(isMap ? ((index % 2) == 0 ? ", " : ": ") : ", ");
			put(rendered[index]);
		}
		put(isMap ? "}" : "]");
	}

	//                                                                 comments

	size_t endLineOfExpr(const ExprPtr& expression) {
		if (!expression)
			return 1;
		size_t end = expression->location.line;
		for (const ExprPtr& child : expression->children) {
			end = std::max(end, endLineOfExpr(child));
		}
		for (const ExprArm& arm : expression->arms) {
			for (const ExprPtr& value : arm.values)
				end = std::max(end, endLineOfExpr(value));
			end = std::max(end, endLineOfExpr(arm.body));
		}
		return end;
	}

	size_t endLineOf(const StmtPtr& statement) {
		if (statement->endLine != 0)
			return statement->endLine; // exact from parser
		size_t end = statement->location.line;
		if (statement->initializer)
			end = std::max(end, endLineOf(statement->initializer));
		if (statement->expression)
			end = std::max(end, endLineOfExpr(statement->expression));
		if (statement->step)
			end = std::max(end, endLineOfExpr(statement->step));
		if (statement->forInIterable)
			end = std::max(end, endLineOfExpr(statement->forInIterable));
		for (const StmtPtr& child : statement->children)
			end = std::max(end, endLineOf(child));
		if (statement->alternative)
			end = std::max(end, endLineOf(statement->alternative));
		for (const SwitchArm& arm : statement->switchArms) {
			for (const ExprPtr& value : arm.values)
				end = std::max(end, endLineOfExpr(value));
			if (arm.body)
				end = std::max(end, endLineOf(arm.body));
		}
		for (const ExprPtr& value : statement->returnValues) {
			end = std::max(end, endLineOfExpr(value));
		}
		return end;
	}

	void flushCommentsBefore(size_t line, size_t prevEnd) {
		lastCommentEnd_ = 0;
		while (cursor_ < comments_.size() &&
			   comments_[cursor_]->location.line < line) {
			const Comment& comment = *comments_[cursor_];
			if (comment.location.line > prevEnd + 1)
				blankLine();
			emitComment(comment);
			++cursor_;
			prevEnd			= comment.endLine;
			lastCommentEnd_ = comment.endLine;
		}
	}

	void emitComment(const Comment& comment) {
		size_t start = 0;
		while (start <= comment.text.size()) {
			const size_t stop = comment.text.find('\n', start);
			const std::string segment =
				stop == std::string::npos
					? comment.text.substr(start)
					: comment.text.substr(start, stop - start);
			piece_ += segment;
			if (stop == std::string::npos)
				break;
			endLine();
			start = stop + 1;
		}
		endLine();
	}

	// Attaches a comment that starts on the statement's own source lines to the
	// line being built; returns its end line (0 when nothing attached).
	size_t flushTrailing(size_t sourceEnd) {
		if (cursor_ >= comments_.size())
			return 0;
		const Comment& comment = *comments_[cursor_];
		if (comment.location.line > sourceEnd ||
			comment.location.line < statementLine_)
			return 0;
		++cursor_;
		if (!piece_.empty())
			piece_ += " ";
		piece_ += comment.text;
		lastCommentEnd_ = comment.endLine;
		return comment.endLine;
	}

	// ------------------------------------------------------------------ state

	const ParsedFile& parsed_;
	std::vector<const Comment*> comments_;
	std::ostringstream out_;
	std::string piece_;
	size_t indent_		   = 0;
	size_t cursor_		   = 0;
	size_t statementLine_  = 0;
	size_t linesWritten_   = 0;
	size_t lastCommentEnd_ = 0;
	bool breakAtEnd_	   = true;
	bool probing_		   = false;
};
} // namespace

std::string formatProgram(const ParsedFile& parsed) {
	Formatter formatter(parsed);
	return formatter.format();
}
} // namespace civic
