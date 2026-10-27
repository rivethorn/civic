#include "codegen.hpp"

#include "sema.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/TargetParser/Triple.h>
#if __has_include(<llvm/TargetParser/Host.h>)
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace civic {
namespace {
llvm::Type* llvmType(Type type, llvm::LLVMContext& context) {
  switch (type) {
    case Type::I32: return llvm::Type::getInt32Ty(context);
    case Type::F64: return llvm::Type::getDoubleTy(context);
    case Type::Bool: return llvm::Type::getInt1Ty(context);
    case Type::String: return llvm::PointerType::get(context, 0);
    case Type::StringPointer: return llvm::PointerType::get(context, 0);
    case Type::Void: return llvm::Type::getVoidTy(context);
  }
  return llvm::Type::getVoidTy(context);
}

class Generator {
public:
  Generator(const Program& program, const std::string& moduleName)
      : program_(program), context_(), module_(moduleName, context_), builder_(context_) {
    module_.setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
    declareFunctions();
  }

  std::string run() {
    for (const FunctionDecl& function : program_.functions) generateFunction(function);
    std::string errors;
    llvm::raw_string_ostream errorStream(errors);
    if (llvm::verifyModule(module_, &errorStream)) {
      errorStream.flush();
      throw Diagnostic({1, 1}, "LLVM verification failed: " + errors);
    }
    std::string result;
    llvm::raw_string_ostream stream(result);
    module_.print(stream, nullptr);
    stream.flush();
    return result;
  }

private:
  llvm::FunctionType* functionType(const FunctionDecl& function) {
    std::vector<llvm::Type*> parameters;
    for (const Parameter& parameter : function.parameters) {
      parameters.push_back(llvmType(parseTypeName(parameter.typeName, parameter.location), context_));
    }
    return llvm::FunctionType::get(
        llvmType(parseTypeName(function.returnType, function.location), context_), parameters, false);
  }

  void declareFunctions() {
    for (const FunctionDecl& function : program_.functions) {
      if (function.name.rfind("__civic_string_", 0) == 0) continue;
      llvm::Function::Create(functionType(function), llvm::GlobalValue::ExternalLinkage,
                             function.name, module_);
    }
    std::vector<llvm::Type*> printfArguments{llvm::PointerType::get(context_, 0)};
    llvm::FunctionType* printfType = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(context_), printfArguments, true);
    module_.getOrInsertFunction("printf", printfType);
  }

  llvm::AllocaInst* createEntryAlloca(llvm::Function* function, const std::string& name, Type type) {
    llvm::IRBuilder<> entryBuilder(&function->getEntryBlock(),
                                   function->getEntryBlock().begin());
    return entryBuilder.CreateAlloca(llvmType(type, context_), nullptr, name);
  }

  void generateFunction(const FunctionDecl& declaration) {
    if (!declaration.body) return;
    llvm::Function* function = module_.getFunction(declaration.name);
    auto* entry = llvm::BasicBlock::Create(context_, "entry", function);
    builder_.SetInsertPoint(entry);
    scopes_.clear();
    scopes_.emplace_back();
    auto arg = function->arg_begin();
    for (const Parameter& parameter : declaration.parameters) {
      Type type = parseTypeName(parameter.typeName, parameter.location);
      llvm::AllocaInst* slot = createEntryAlloca(function, parameter.name, type);
      builder_.CreateStore(&*arg, slot);
      scopes_.back()[parameter.name] = {slot, type};
      ++arg;
    }
    generateStatement(declaration.body);
    if (!hasTerminator(builder_.GetInsertBlock())) {
      Type returnType = parseTypeName(declaration.returnType, declaration.location);
      if (returnType == Type::Void) {
        builder_.CreateRetVoid();
      } else if (declaration.name == "main" && returnType == Type::I32) {
        builder_.CreateRet(llvm::ConstantInt::get(llvmType(Type::I32, context_), 0));
      } else {
        throw Diagnostic(declaration.location, "control reaches end of non-void function '" +
                                                  declaration.name + "'");
      }
    }
  }

  void generateStatement(const StmtPtr& statement) {
    if (hasTerminator(builder_.GetInsertBlock())) return;
    switch (statement->kind) {
      case Stmt::Kind::Block:
        scopes_.emplace_back();
        for (const StmtPtr& child : statement->children) generateStatement(child);
        scopes_.pop_back();
        return;
      case Stmt::Kind::Variable: {
        Type type = parseTypeName(statement->typeName, statement->location);
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        llvm::AllocaInst* slot = createEntryAlloca(function, statement->name, type);
        scopes_.back()[statement->name] = {slot, type};
        llvm::Value* initial = statement->expression
                                   ? generateExpression(statement->expression)
                                   : llvm::Constant::getNullValue(llvmType(type, context_));
        builder_.CreateStore(initial, slot);
        return;
      }
      case Stmt::Kind::Expression:
        generateExpression(statement->expression);
        return;
      case Stmt::Kind::Return:
        if (statement->expression) {
          builder_.CreateRet(generateExpression(statement->expression));
        } else {
          builder_.CreateRetVoid();
        }
        return;
      case Stmt::Kind::If:
        generateIf(statement);
        return;
      case Stmt::Kind::While:
        generateWhile(statement);
        return;
      case Stmt::Kind::For:
        generateFor(statement);
        return;
    }
  }

  void generateIf(const StmtPtr& statement) {
    llvm::Function* function = builder_.GetInsertBlock()->getParent();
    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(context_, "if.then", function);
    llvm::BasicBlock* elseBlock = statement->alternative
                                      ? llvm::BasicBlock::Create(context_, "if.else", function)
                                      : nullptr;
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(context_, "if.end", function);
    llvm::Value* condition = toCondition(generateExpression(statement->expression));
    builder_.CreateCondBr(condition, thenBlock, elseBlock ? elseBlock : mergeBlock);

    builder_.SetInsertPoint(thenBlock);
    generateStatement(statement->children[0]);
    bool thenTerminates = hasTerminator(builder_.GetInsertBlock());
    if (!thenTerminates) builder_.CreateBr(mergeBlock);

    bool elseTerminates = false;
    if (elseBlock) {
      builder_.SetInsertPoint(elseBlock);
      generateStatement(statement->alternative);
      elseTerminates = hasTerminator(builder_.GetInsertBlock());
      if (!elseTerminates) builder_.CreateBr(mergeBlock);
    }
    if (elseBlock && thenTerminates && elseTerminates) {
      mergeBlock->eraseFromParent();
      builder_.SetInsertPoint(thenBlock);
      return;
    }
    builder_.SetInsertPoint(mergeBlock);
  }

  void generateWhile(const StmtPtr& statement) {
    llvm::Function* function = builder_.GetInsertBlock()->getParent();
    llvm::BasicBlock* conditionBlock = llvm::BasicBlock::Create(context_, "while.cond", function);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context_, "while.body", function);
    llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(context_, "while.end", function);
    builder_.CreateBr(conditionBlock);
    builder_.SetInsertPoint(conditionBlock);
    builder_.CreateCondBr(toCondition(generateExpression(statement->expression)), bodyBlock, endBlock);
    builder_.SetInsertPoint(bodyBlock);
    generateStatement(statement->children[0]);
    if (!hasTerminator(builder_.GetInsertBlock())) builder_.CreateBr(conditionBlock);
    builder_.SetInsertPoint(endBlock);
  }

  void generateFor(const StmtPtr& statement) {
    scopes_.emplace_back();
    if (statement->initializer) generateStatement(statement->initializer);
    llvm::Function* function = builder_.GetInsertBlock()->getParent();
    llvm::BasicBlock* conditionBlock = llvm::BasicBlock::Create(context_, "for.cond", function);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context_, "for.body", function);
    llvm::BasicBlock* stepBlock = llvm::BasicBlock::Create(context_, "for.step", function);
    llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(context_, "for.end", function);
    builder_.CreateBr(conditionBlock);
    builder_.SetInsertPoint(conditionBlock);
    llvm::Value* condition = statement->expression
                                 ? toCondition(generateExpression(statement->expression))
                                 : llvm::ConstantInt::getTrue(context_);
    builder_.CreateCondBr(condition, bodyBlock, endBlock);
    builder_.SetInsertPoint(bodyBlock);
    generateStatement(statement->children[0]);
    if (!hasTerminator(builder_.GetInsertBlock())) builder_.CreateBr(stepBlock);
    builder_.SetInsertPoint(stepBlock);
    if (statement->step) generateExpression(statement->step);
    if (!hasTerminator(builder_.GetInsertBlock())) builder_.CreateBr(conditionBlock);
    builder_.SetInsertPoint(endBlock);
    scopes_.pop_back();
  }

  static bool hasTerminator(const llvm::BasicBlock* block) {
    return !block->empty() && block->back().isTerminator();
  }

  llvm::Value* toCondition(llvm::Value* value) {
    if (value->getType()->isIntegerTy(1)) return value;
    if (value->getType()->isIntegerTy()) {
      return builder_.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "truth");
    }
    return builder_.CreateFCmpONE(value, llvm::ConstantFP::get(value->getType(), 0.0), "truth");
  }

  llvm::Constant* stringPointer(const std::string& value, const std::string& name) {
    llvm::Constant* data = llvm::ConstantDataArray::getString(context_, value, true);
    auto* global = new llvm::GlobalVariable(
        module_, data->getType(), true, llvm::GlobalValue::PrivateLinkage, data, name);
    llvm::Constant* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0);
    llvm::Constant* indices[] = {zero, zero};
    return llvm::ConstantExpr::getInBoundsGetElementPtr(data->getType(), global, indices);
  }

  std::pair<llvm::Value*, Type> lookupVariable(const ExprPtr& expression) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      auto variable = scope->find(expression->text);
      if (variable != scope->end()) return variable->second;
    }
    throw Diagnostic(expression->location, "unknown variable '" + expression->text + "'");
  }

  llvm::Value* generateExpression(const ExprPtr& expression) {
    switch (expression->kind) {
      case Expr::Kind::Integer:
        return llvm::ConstantInt::get(llvmType(Type::I32, context_),
                                      std::stoll(expression->text), true);
      case Expr::Kind::Float:
        return llvm::ConstantFP::get(llvmType(Type::F64, context_),
                                     std::stod(expression->text));
      case Expr::Kind::String:
        return stringPointer(expression->text, "str");
      case Expr::Kind::Boolean:
        return llvm::ConstantInt::get(llvmType(Type::Bool, context_),
                                      expression->text == "true");
      case Expr::Kind::Name: {
        auto variable = lookupVariable(expression);
        return builder_.CreateLoad(llvmType(variable.second, context_), variable.first,
                                   expression->text);
      }
      case Expr::Kind::Index: {
        llvm::Value* base = generateExpression(expression->children[0]);
        llvm::Value* index = generateExpression(expression->children[1]);
        llvm::Value* address = builder_.CreateInBoundsGEP(
            llvm::PointerType::get(context_, 0), base, index, "index.address");
        return builder_.CreateLoad(llvm::PointerType::get(context_, 0), address, "index");
      }
      case Expr::Kind::Unary: {
        if (expression->text == "++" || expression->text == "--" ||
            expression->text == "post++" || expression->text == "post--") {
          auto variable = lookupVariable(expression->children[0]);
          llvm::Value* oldValue = builder_.CreateLoad(
              llvmType(variable.second, context_), variable.first, "increment.old");
          llvm::Value* one = variable.second == Type::F64
                                 ? static_cast<llvm::Value*>(llvm::ConstantFP::get(
                                       llvmType(Type::F64, context_), 1.0))
                                 : static_cast<llvm::Value*>(llvm::ConstantInt::get(
                                       llvmType(Type::I32, context_), 1));
          bool decrement = expression->text == "--" || expression->text == "post--";
          llvm::Value* next = arithmetic(decrement ? "-" : "+", oldValue, one);
          builder_.CreateStore(next, variable.first);
          return expression->text.rfind("post", 0) == 0 ? oldValue : next;
        }
        llvm::Value* value = generateExpression(expression->children[0]);
        if (expression->text == "!") return builder_.CreateNot(toCondition(value), "not");
        if (expression->text == "-") {
          return value->getType()->isFloatingPointTy()
                     ? builder_.CreateFNeg(value, "neg")
                     : builder_.CreateNeg(value, "neg");
        }
        return value;
      }
      case Expr::Kind::Binary:
        return generateBinary(expression);
      case Expr::Kind::Call:
        return generateCall(expression);
    }
    return nullptr;
  }

  llvm::Value* generateBinary(const ExprPtr& expression) {
    const std::string& op = expression->text;
    if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") {
      auto target = lookupVariable(expression->children[0]);
      llvm::Value* rhs = generateExpression(expression->children[1]);
      if (op != "=") {
        llvm::Value* lhs = builder_.CreateLoad(llvmType(target.second, context_), target.first);
        rhs = arithmetic(op.substr(0, 1), lhs, rhs);
      }
      builder_.CreateStore(rhs, target.first);
      return rhs;
    }
    llvm::Value* lhs = generateExpression(expression->children[0]);
    if (op == "&&" || op == "||") {
      llvm::Value* leftCondition = toCondition(lhs);
      llvm::Function* function = builder_.GetInsertBlock()->getParent();
      llvm::BasicBlock* leftBlock = builder_.GetInsertBlock();
      llvm::BasicBlock* rightBlock = llvm::BasicBlock::Create(context_, "logic.rhs", function);
      llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(context_, "logic.end", function);
      bool isAnd = op == "&&";
      builder_.CreateCondBr(leftCondition, isAnd ? rightBlock : mergeBlock,
                            isAnd ? mergeBlock : rightBlock);
      builder_.SetInsertPoint(rightBlock);
      llvm::Value* rightCondition = toCondition(generateExpression(expression->children[1]));
      llvm::BasicBlock* rightEnd = builder_.GetInsertBlock();
      if (!hasTerminator(rightEnd)) builder_.CreateBr(mergeBlock);
      builder_.SetInsertPoint(mergeBlock);
      llvm::PHINode* result = builder_.CreatePHI(llvm::Type::getInt1Ty(context_), 2, "logic");
      result->addIncoming(llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), !isAnd),
                          leftBlock);
      result->addIncoming(rightCondition, rightEnd);
      return result;
    }
    llvm::Value* rhs = generateExpression(expression->children[1]);
    if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
      bool floating = lhs->getType()->isFloatingPointTy();
      if (lhs->getType()->isPointerTy() && (op == "==" || op == "!=")) {
        llvm::FunctionCallee strcmpFunction = module_.getOrInsertFunction(
            "strcmp", llvm::FunctionType::get(llvm::Type::getInt32Ty(context_),
                {llvm::PointerType::get(context_, 0), llvm::PointerType::get(context_, 0)},
                false));
        llvm::Value* comparison = builder_.CreateCall(strcmpFunction, {lhs, rhs}, "strcmp");
        return op == "=="
                   ? builder_.CreateICmpEQ(comparison, llvm::ConstantInt::get(
                         llvm::Type::getInt32Ty(context_), 0), "string.eq")
                   : builder_.CreateICmpNE(comparison, llvm::ConstantInt::get(
                         llvm::Type::getInt32Ty(context_), 0), "string.ne");
      }
      if (floating) {
        if (op == "==") return builder_.CreateFCmpOEQ(lhs, rhs, "cmp");
        if (op == "!=") return builder_.CreateFCmpUNE(lhs, rhs, "cmp");
        if (op == "<") return builder_.CreateFCmpOLT(lhs, rhs, "cmp");
        if (op == "<=") return builder_.CreateFCmpOLE(lhs, rhs, "cmp");
        if (op == ">") return builder_.CreateFCmpOGT(lhs, rhs, "cmp");
        return builder_.CreateFCmpOGE(lhs, rhs, "cmp");
      }
      if (op == "==") return builder_.CreateICmpEQ(lhs, rhs, "cmp");
      if (op == "!=") return builder_.CreateICmpNE(lhs, rhs, "cmp");
      if (op == "<") return builder_.CreateICmpSLT(lhs, rhs, "cmp");
      if (op == "<=") return builder_.CreateICmpSLE(lhs, rhs, "cmp");
      if (op == ">") return builder_.CreateICmpSGT(lhs, rhs, "cmp");
      return builder_.CreateICmpSGE(lhs, rhs, "cmp");
    }
    return arithmetic(op, lhs, rhs);
  }

  llvm::Value* arithmetic(const std::string& op, llvm::Value* lhs, llvm::Value* rhs) {
    bool floating = lhs->getType()->isFloatingPointTy();
    if (!floating && (op == "/" || op == "%")) {
      llvm::Value* divisionByZero = builder_.CreateICmpEQ(
          rhs, llvm::ConstantInt::get(rhs->getType(), 0), "division.zero");
      llvm::Value* minimum = builder_.CreateICmpEQ(
          lhs, llvm::ConstantInt::get(lhs->getType(), std::numeric_limits<int32_t>::min()),
          "division.minimum");
      llvm::Value* negativeOne = builder_.CreateICmpEQ(
          rhs, llvm::ConstantInt::getSigned(rhs->getType(), -1), "division.negative.one");
      llvm::Value* overflow = builder_.CreateAnd(minimum, negativeOne, "division.overflow");
      llvm::Value* invalid = builder_.CreateOr(divisionByZero, overflow, "division.invalid");
      llvm::Function* function = builder_.GetInsertBlock()->getParent();
      llvm::BasicBlock* validBlock = llvm::BasicBlock::Create(context_, "division.valid", function);
      llvm::BasicBlock* failureBlock = llvm::BasicBlock::Create(context_, "division.failure", function);
      builder_.CreateCondBr(invalid, failureBlock, validBlock);
      builder_.SetInsertPoint(failureBlock);
      llvm::Function* trap = llvm::Intrinsic::getOrInsertDeclaration(
          &module_, llvm::Intrinsic::trap);
      builder_.CreateCall(trap);
      builder_.CreateUnreachable();
      builder_.SetInsertPoint(validBlock);
    }
    if (floating) {
      if (op == "+") return builder_.CreateFAdd(lhs, rhs, "add");
      if (op == "-") return builder_.CreateFSub(lhs, rhs, "sub");
      if (op == "*") return builder_.CreateFMul(lhs, rhs, "mul");
      if (op == "/") return builder_.CreateFDiv(lhs, rhs, "div");
      if (op == "%") return builder_.CreateFRem(lhs, rhs, "rem");
    } else {
      if (op == "+") return builder_.CreateAdd(lhs, rhs, "add");
      if (op == "-") return builder_.CreateSub(lhs, rhs, "sub");
      if (op == "*") return builder_.CreateMul(lhs, rhs, "mul");
      if (op == "/") return builder_.CreateSDiv(lhs, rhs, "div");
      if (op == "%") return builder_.CreateSRem(lhs, rhs, "rem");
    }
    return nullptr;
  }

  llvm::Value* generateCall(const ExprPtr& expression) {
    if (expression->text == "assert") {
      llvm::Value* condition = toCondition(generateExpression(expression->children[0]));
      llvm::Function* function = builder_.GetInsertBlock()->getParent();
      llvm::BasicBlock* success = llvm::BasicBlock::Create(context_, "assert.ok", function);
      llvm::BasicBlock* failure = llvm::BasicBlock::Create(context_, "assert.fail", function);
      builder_.CreateCondBr(condition, success, failure);
      builder_.SetInsertPoint(failure);
      llvm::Function* trap = llvm::Intrinsic::getOrInsertDeclaration(
          &module_, llvm::Intrinsic::trap);
      builder_.CreateCall(trap);
      builder_.CreateUnreachable();
      builder_.SetInsertPoint(success);
      return nullptr;
    }
    if (expression->text == "print" || expression->text == "println") {
      llvm::FunctionCallee printf = module_.getOrInsertFunction(
          "printf", llvm::FunctionType::get(llvm::Type::getInt32Ty(context_),
              {llvm::PointerType::get(context_, 0)}, true));
      for (const ExprPtr& argument : expression->children) {
        llvm::Value* value = generateExpression(argument);
        std::string format;
        std::vector<llvm::Value*> arguments;
        if (value->getType()->isPointerTy()) {
          format = "%s";
          arguments.push_back(value);
        } else if (value->getType()->isFloatingPointTy()) {
          format = "%g";
          arguments.push_back(value);
        } else if (value->getType()->isIntegerTy(1)) {
          llvm::Value* yes = stringPointer("true", "true");
          llvm::Value* no = stringPointer("false", "false");
          format = "%s";
          arguments.push_back(builder_.CreateSelect(value, yes, no, "boolstr"));
        } else {
          format = "%d";
          arguments.push_back(value);
        }
        std::vector<llvm::Value*> callArguments{stringPointer(format, "fmt")};
        callArguments.insert(callArguments.end(), arguments.begin(), arguments.end());
        builder_.CreateCall(printf, callArguments);
      }
      if (expression->text == "println") {
        builder_.CreateCall(printf, {stringPointer("\n", "newline")});
      }
      return nullptr;
    }
    if (expression->text.rfind("__civic_string_", 0) == 0) {
      std::vector<llvm::Value*> arguments;
      for (const ExprPtr& argument : expression->children) {
        arguments.push_back(generateExpression(argument));
      }
      llvm::Type* pointer = llvm::PointerType::get(context_, 0);
      auto libc = [&](const std::string& name, llvm::Type* result,
                      std::vector<llvm::Type*> parameters) {
        return module_.getOrInsertFunction(
            name, llvm::FunctionType::get(result, std::move(parameters), false));
      };
      llvm::Type* i32 = llvm::Type::getInt32Ty(context_);
      llvm::Type* i1 = llvm::Type::getInt1Ty(context_);
      if (expression->text == "__civic_string_length") {
        auto function = libc("strlen", llvm::Type::getInt64Ty(context_), {pointer});
        llvm::Value* length = builder_.CreateCall(function, arguments, "strlen");
        return builder_.CreateTrunc(length, i32, "string.length");
      }
      if (expression->text == "__civic_string_equals") {
        auto function = libc("strcmp", i32, {pointer, pointer});
        llvm::Value* result = builder_.CreateCall(function, arguments, "strcmp");
        return builder_.CreateICmpEQ(result, llvm::ConstantInt::get(i32, 0), "string.eq");
      }
      if (expression->text == "__civic_string_contains") {
        auto function = libc("strstr", pointer, {pointer, pointer});
        llvm::Value* result = builder_.CreateCall(function, arguments, "strstr");
        return builder_.CreateICmpNE(result, llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(pointer)), "string.contains");
      }
      auto strlenFunction = libc("strlen", llvm::Type::getInt64Ty(context_), {pointer});
      llvm::Value* valueLength = builder_.CreateCall(strlenFunction, {arguments[0]}, "value.len");
      llvm::Value* patternLength = builder_.CreateCall(strlenFunction, {arguments[1]}, "pattern.len");
      if (expression->text == "__civic_string_starts_with") {
        llvm::Value* lengthFits = builder_.CreateICmpULE(patternLength, valueLength, "prefix.fits");
        llvm::Value* compared = builder_.CreateCall(
            libc("strncmp", i32, {pointer, pointer, llvm::Type::getInt64Ty(context_)}),
            {arguments[0], arguments[1], patternLength}, "strncmp");
        llvm::Value* equal = builder_.CreateICmpEQ(compared, llvm::ConstantInt::get(i32, 0),
                                                   "prefix.matches");
        return builder_.CreateAnd(lengthFits, equal, "starts.with");
      }

      llvm::Function* parent = builder_.GetInsertBlock()->getParent();
      llvm::BasicBlock* compareBlock = llvm::BasicBlock::Create(context_, "suffix.compare", parent);
      llvm::BasicBlock* missBlock = llvm::BasicBlock::Create(context_, "suffix.miss", parent);
      llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(context_, "suffix.end", parent);
      llvm::Value* lengthFits = builder_.CreateICmpULE(patternLength, valueLength, "suffix.fits");
      builder_.CreateCondBr(lengthFits, compareBlock, missBlock);
      builder_.SetInsertPoint(compareBlock);
      llvm::Value* offset = builder_.CreateSub(valueLength, patternLength, "suffix.offset");
      llvm::Value* suffixStart = builder_.CreateGEP(
          llvm::Type::getInt8Ty(context_), arguments[0], offset, "suffix.start");
      llvm::Value* compared = builder_.CreateCall(
          libc("strcmp", i32, {pointer, pointer}), {suffixStart, arguments[1]}, "suffix.compare");
      llvm::Value* matches = builder_.CreateICmpEQ(
          compared, llvm::ConstantInt::get(i32, 0), "suffix.matches");
      llvm::BasicBlock* compareEnd = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBlock);
      builder_.SetInsertPoint(missBlock);
      builder_.CreateBr(mergeBlock);
      builder_.SetInsertPoint(mergeBlock);
      llvm::PHINode* result = builder_.CreatePHI(i1, 2, "ends.with");
      result->addIncoming(matches, compareEnd);
      result->addIncoming(llvm::ConstantInt::getFalse(context_), missBlock);
      return result;
    }
    std::vector<llvm::Value*> arguments;
    for (const ExprPtr& argument : expression->children) {
      arguments.push_back(generateExpression(argument));
    }
    llvm::Function* function = module_.getFunction(expression->text);
    return builder_.CreateCall(function, arguments,
                               function->getReturnType()->isVoidTy() ? "" : "call");
  }

  const Program& program_;
  llvm::LLVMContext context_;
  llvm::Module module_;
  llvm::IRBuilder<> builder_;
  std::vector<std::unordered_map<std::string, std::pair<llvm::AllocaInst*, Type>>> scopes_;
};
}  // namespace

std::string generateLLVMIR(const Program& program, const std::string& moduleName) {
  return Generator(program, moduleName).run();
}
}  // namespace civic
