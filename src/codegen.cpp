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

#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace civic {
namespace {
llvm::Type* llvmTypeFree(Type type, llvm::LLVMContext& context) {
	switch (type) {
	case Type::I8:
		return llvm::Type::getInt8Ty(context);
	case Type::I16:
		return llvm::Type::getInt16Ty(context);
	case Type::I32:
		return llvm::Type::getInt32Ty(context);
	case Type::I64:
		return llvm::Type::getInt64Ty(context);
	case Type::I128:
		return llvm::Type::getInt128Ty(context);
	case Type::U8:
		return llvm::Type::getInt8Ty(context);
	case Type::U16:
		return llvm::Type::getInt16Ty(context);
	case Type::U32:
		return llvm::Type::getInt32Ty(context);
	case Type::U64:
		return llvm::Type::getInt64Ty(context);
	case Type::U128:
		return llvm::Type::getInt128Ty(context);
	case Type::F64:
		return llvm::Type::getDoubleTy(context);
	case Type::Bool:
		return llvm::Type::getInt1Ty(context);
	case Type::String:
		return llvm::PointerType::get(context, 0);
	case Type::StringPointer:
		return llvm::PointerType::get(context, 0);
	case Type::Void:
		return llvm::Type::getVoidTy(context);
	case Type::Array:
		return llvm::ArrayType::get(llvm::Type::getInt32Ty(context), 0);
	}
	return llvm::Type::getVoidTy(context);
}

class Generator {
public:
	Generator(const Program& program, const std::string& moduleName)
		: program_(program), context_(), module_(moduleName, context_),
		  builder_(context_), pointerSizeInBits_(64) {
		auto triple = llvm::Triple(llvm::sys::getDefaultTargetTriple());
		module_.setTargetTriple(triple);
		if (triple.getArch() == llvm::Triple::x86 ||
			triple.getArch() == llvm::Triple::aarch64_32 ||
			triple.getArch() == llvm::Triple::arm ||
			triple.getArch() == llvm::Triple::thumb ||
			triple.getArch() == llvm::Triple::mips ||
			triple.getArch() == llvm::Triple::sparc) {
			pointerSizeInBits_ = 32;
		}
		declareFunctions();
	}

	// Pointer-sized signed integer type (isize).
	llvm::Type* isizeLLVMType() {
		return llvm::Type::getIntNTy(context_, pointerSizeInBits_);
	}

	// Pointer-sized unsigned integer type (usize).
	llvm::Type* usizeLLVMType() {
		return llvm::Type::getIntNTy(context_, pointerSizeInBits_);
	}

	// A resolved variable storage slot plus its semantic and textual types.
	struct StoredVariable {
		llvm::Value* slot;
		Type type;
		std::string typeName;
	};

	std::string run() {
		for (const FunctionDecl& function : program_.functions)
			generateFunction(function);
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
	// Per-Generator LLVM struct types; cached for this module's context only.
	std::unordered_map<std::string, llvm::StructType*> structTypes_;
	// Pointer size in bits for isize/usize types (e.g. 64 on x86_64).
	unsigned pointerSizeInBits_;

	llvm::Type* llvmType(Type type) {
		switch (type) {
		case Type::I8:
			return llvm::Type::getInt8Ty(context_);
		case Type::I16:
			return llvm::Type::getInt16Ty(context_);
		case Type::I32:
			return llvm::Type::getInt32Ty(context_);
		case Type::I64:
			return llvm::Type::getInt64Ty(context_);
		case Type::I128:
			return llvm::Type::getInt128Ty(context_);
		case Type::U8:
			return llvm::Type::getInt8Ty(context_);
		case Type::U16:
			return llvm::Type::getInt16Ty(context_);
		case Type::U32:
			return llvm::Type::getInt32Ty(context_);
		case Type::U64:
			return llvm::Type::getInt64Ty(context_);
		case Type::U128:
			return llvm::Type::getInt128Ty(context_);
		case Type::Isize:
			return isizeLLVMType();
		case Type::Usize:
			return usizeLLVMType();
		case Type::F64:
			return llvm::Type::getDoubleTy(context_);
		case Type::Bool:
			return llvm::Type::getInt1Ty(context_);
		case Type::String:
			return llvm::PointerType::get(context_, 0);
		case Type::StringPointer:
			return llvm::PointerType::get(context_, 0);
		case Type::Void:
			return llvm::Type::getVoidTy(context_);
		case Type::Array:
			return llvm::ArrayType::get(llvm::Type::getInt32Ty(context_), 0);
		case Type::Vector:
			return vectorStructType();
		case Type::Map:
			return mapStructType();
		}
		return llvm::Type::getVoidTy(context_);
	}

	llvm::StructType* structTypeFor(const std::string& key,
									const llvm::ArrayRef<llvm::Type*>& fields,
									const std::string& name) {
		auto& cached = structTypes_[key];
		if (!cached)
			cached = llvm::StructType::create(context_, fields, name);
		return cached;
	}

	llvm::StructType* vectorStructType() {
		return structTypeFor("civic.vector",
							 {llvm::PointerType::get(context_, 0),
							  llvm::Type::getInt32Ty(context_),
							  llvm::Type::getInt32Ty(context_)},
							 "civic.vector");
	}

	llvm::StructType* mapStructType() {
		return structTypeFor("civic.map",
							 {llvm::Type::getInt32Ty(context_),
							  llvm::Type::getInt32Ty(context_),
							  llvm::PointerType::get(context_, 0)},
							 "civic.map");
	}

	llvm::StructType* mapEntryType(Type valueType) {
		return structTypeFor(
			"civic.map.entry." + std::to_string(static_cast<int>(valueType)),
			{llvm::PointerType::get(context_, 0), llvmType(valueType)},
			"civic.map.entry");
	}

	llvm::FunctionType* functionType(const FunctionDecl& function) {
		std::vector<llvm::Type*> parameters;
		for (const Parameter& parameter : function.parameters) {
			parameters.push_back(llvmType(
				parseTypeName(parameter.typeName, parameter.location)));
		}
		std::vector<std::string> returnTypes =
			function.returnTypes.empty()
				? std::vector<std::string>{function.returnType}
				: function.returnTypes;
		llvm::Type* returnType =
			llvmType(parseTypeName(returnTypes[0], function.location));
		if (returnTypes.size() > 1) {
			std::vector<llvm::Type*> fields;
			for (const std::string& returnType : returnTypes) {
				fields.push_back(
					llvmType(parseTypeName(returnType, function.location)));
			}
			returnType = llvm::StructType::create(context_, fields,
												  function.name + ".return");
		}
		return llvm::FunctionType::get(returnType, parameters, false);
	}

	void declareFunctions() {
		for (const FunctionDecl& function : program_.functions) {
			if (function.name.rfind("__civic_string_", 0) == 0)
				continue;
			llvm::Function::Create(functionType(function),
								   llvm::GlobalValue::ExternalLinkage,
								   function.name, module_);
		}
		std::vector<llvm::Type*> printfArguments{
			llvm::PointerType::get(context_, 0)};
		llvm::FunctionType* printfType = llvm::FunctionType::get(
			llvm::Type::getInt32Ty(context_), printfArguments, true);
		module_.getOrInsertFunction("printf", printfType);
	}

	llvm::AllocaInst* createEntryAlloca(llvm::Function* function,
										const std::string& name,
										llvm::Type* type) {
		llvm::IRBuilder<> entryBuilder(&function->getEntryBlock(),
									   function->getEntryBlock().begin());
		return entryBuilder.CreateAlloca(type, nullptr, name);
	}

	llvm::AllocaInst* createEntryAlloca(llvm::Function* function,
										const std::string& name, Type type) {
		return createEntryAlloca(function, name, llvmType(type));
	}

	void generateFunction(const FunctionDecl& declaration) {
		if (!declaration.body)
			return;
		llvm::Function* function = module_.getFunction(declaration.name);
		auto* entry = llvm::BasicBlock::Create(context_, "entry", function);
		builder_.SetInsertPoint(entry);
		scopes_.clear();
		scopes_.emplace_back();
		auto arg = function->arg_begin();
		for (const Parameter& parameter : declaration.parameters) {
			Type type = parseTypeName(parameter.typeName, parameter.location);
			llvm::AllocaInst* slot =
				createEntryAlloca(function, parameter.name, type);
			builder_.CreateStore(&*arg, slot);
			scopes_.back()[parameter.name] = {slot, type, parameter.typeName};
			++arg;
		}
		generateStatement(declaration.body);
		if (!hasTerminator(builder_.GetInsertBlock())) {
			const std::vector<std::string> returnTypes =
				declaration.returnTypes.empty()
					? std::vector<std::string>{declaration.returnType}
					: declaration.returnTypes;
			if (returnTypes.size() != 1) {
				throw Diagnostic(declaration.location,
								 "control reaches end of function '" +
									 declaration.name + "'");
			}
			Type returnType =
				parseTypeName(returnTypes[0], declaration.location);
			if (returnType == Type::Void) {
				builder_.CreateRetVoid();
			} else if (declaration.name == "main" && returnType == Type::Isize) {
				builder_.CreateRet(
					llvm::ConstantInt::get(llvmType(Type::Isize), 0));
			} else {
				throw Diagnostic(declaration.location,
								 "control reaches end of non-void function '" +
									 declaration.name + "'");
			}
		}
	}

	void generateStatement(const StmtPtr& statement) {
		if (hasTerminator(builder_.GetInsertBlock()))
			return;
		switch (statement->kind) {
		case Stmt::Kind::Block:
			scopes_.emplace_back();
			for (const StmtPtr& child : statement->children)
				generateStatement(child);
			scopes_.pop_back();
			return;
		case Stmt::Kind::Variable: {
			if (statement->names.size() > 1) {
				llvm::Function* function =
					builder_.GetInsertBlock()->getParent();
				std::vector<llvm::AllocaInst*> slots;
				for (size_t index = 0; index < statement->names.size();
					 ++index) {
					const Type type = parseTypeName(statement->typeNames[index],
													statement->location);
					llvm::AllocaInst* slot = createEntryAlloca(
						function, statement->names[index], type);
					scopes_.back()[statement->names[index]] = {
						slot, type, statement->typeNames[index]};
					slots.push_back(slot);
				}
				llvm::Value* aggregate =
					generateExpression(statement->expression);
				for (size_t index = 0; index < statement->names.size();
					 ++index) {
					llvm::Value* value = builder_.CreateExtractValue(
						aggregate, static_cast<unsigned>(index),
						statement->names[index]);
					builder_.CreateStore(value, slots[index]);
				}
				return;
			}
			llvm::Function* function = builder_.GetInsertBlock()->getParent();
			Type type;
			if (statement->typeName == "auto") {
				// for-in bindings: infer the element (or map key) type from the
				// collection
				auto collection =
					lookupName(statement->expression->children[0]->text,
							   statement->location);
				type = collectionElementType(collection, statement->location);
				llvm::Value* value = generateExpression(statement->expression);
				llvm::AllocaInst* slot =
					createEntryAlloca(function, statement->name, type);
				scopes_.back()[statement->name] = {slot, type,
												   statement->typeName};
				builder_.CreateStore(value, slot);
				return;
			}
			type = parseTypeName(statement->typeName, statement->location);
			if (type == Type::Array) {
				const std::vector<ExprPtr>& elements =
					statement->expression ? statement->expression->children
										  : std::vector<ExprPtr>{};
				Type elementType =
					arrayElementType(statement->typeName, statement->location);
				llvm::Type* elementLLVM = llvmType(elementType);
				llvm::Type* arrayType =
					llvm::ArrayType::get(elementLLVM, elements.size());
				llvm::AllocaInst* arraySlot =
					createEntryAlloca(function, statement->name, arrayType);
				for (size_t index = 0; index < elements.size(); ++index) {
					llvm::Value* elementPointer = builder_.CreateInBoundsGEP(
						arrayType, arraySlot,
						llvm::ArrayRef<llvm::Value*>{
							llvm::ConstantInt::get(
								llvm::Type::getInt32Ty(context_), 0),
							llvm::ConstantInt::get(
								llvm::Type::getInt32Ty(context_), index)});
					builder_.CreateStore(generateExpression(elements[index]),
										 elementPointer);
				}
				scopes_.back()[statement->name] = {arraySlot, type,
												   statement->typeName};
				return;
			}
			if (type == Type::Vector) {
				const std::vector<ExprPtr>& elements =
					statement->expression ? statement->expression->children
										  : std::vector<ExprPtr>{};
				Type elementType =
					vectorElementType(statement->typeName, statement->location);
				llvm::Type* elementLLVM		 = llvmType(elementType);
				llvm::StructType* structType = vectorStructType();
				llvm::AllocaInst* slot =
					createEntryAlloca(function, statement->name, structType);
				llvm::Value* data = callMalloc(elements.size(), elementLLVM);
				for (size_t index = 0; index < elements.size(); ++index) {
					llvm::Value* elementPointer = builder_.CreateInBoundsGEP(
						elementLLVM, data, indexValue(index));
					builder_.CreateStore(generateExpression(elements[index]),
										 elementPointer);
				}
				storeVectorFields(slot, data,
								  intConstant((uint64_t)elements.size()),
								  intConstant((uint64_t)elements.size()));
				scopes_.back()[statement->name] = {slot, type,
												   statement->typeName};
				return;
			}
			if (type == Type::Map) {
				const std::vector<ExprPtr>& mapEntries =
					statement->expression ? statement->expression->children
										  : std::vector<ExprPtr>{};
				const size_t count = mapEntries.size() / 2;
				Type valueType =
					mapValueType(statement->typeName, statement->location);
				llvm::StructType* entryType = mapEntryType(valueType);
				llvm::StructType* mapType	= mapStructType();
				llvm::AllocaInst* slot =
					createEntryAlloca(function, statement->name, mapType);
				llvm::Value* entries = callMalloc(count, entryType);
				for (size_t index = 0; index < count; ++index) {
					llvm::Value* entryPointer = builder_.CreateInBoundsGEP(
						entryType, entries, indexValue(index));
					llvm::Value* keySlot =
						builder_.CreateStructGEP(entryType, entryPointer, 0);
					llvm::Value* valueSlot =
						builder_.CreateStructGEP(entryType, entryPointer, 1);
					builder_.CreateStore(
						generateExpression(mapEntries[2 * index]), keySlot);
					builder_.CreateStore(
						generateExpression(mapEntries[2 * index + 1]),
						valueSlot);
				}
				storeMapFields(slot, entries, intConstant(count),
							   intConstant(count));
				scopes_.back()[statement->name] = {slot, type,
												   statement->typeName};
				return;
			}
			llvm::AllocaInst* slot =
				createEntryAlloca(function, statement->name, type);
			scopes_.back()[statement->name] = {slot, type, statement->typeName};
			llvm::Value* initial =
				statement->expression
					? generateExpression(statement->expression)
					: llvm::Constant::getNullValue(llvmType(type));
			if (initial->getType() != llvmType(type) && initial->getType()->isIntegerTy() && llvmType(type)->isIntegerTy()) {
				initial = builder_.CreateIntCast(initial, llvmType(type), true, "cast");
			}
			builder_.CreateStore(initial, slot);
			return;
		}
		case Stmt::Kind::Expression:
			generateExpression(statement->expression);
			return;
		case Stmt::Kind::Return: {
			std::vector<ExprPtr> values = statement->returnValues;
			if (values.empty() && statement->expression)
				values.push_back(statement->expression);
			if (values.empty()) {
				builder_.CreateRetVoid();
				return;
			}
			if (values.size() == 1) {
				builder_.CreateRet(generateExpression(values[0]));
				return;
			}
			llvm::Value* aggregate = llvm::ConstantAggregateZero::get(
				builder_.GetInsertBlock()->getParent()->getReturnType());
			for (size_t index = 0; index < values.size(); ++index) {
				llvm::Value* value = generateExpression(values[index]);
				aggregate		   = builder_.CreateInsertValue(
					aggregate, value,
					llvm::ArrayRef<unsigned>{static_cast<unsigned>(index)});
			}
			builder_.CreateRet(aggregate);
			return;
		}
		case Stmt::Kind::If:
			generateIf(statement);
			return;
		case Stmt::Kind::While:
			generateWhile(statement);
			return;
		case Stmt::Kind::For:
			generateFor(statement);
			return;
		case Stmt::Kind::Switch:
			generateSwitchStatement(statement);
			return;
		case Stmt::Kind::TupleAssign: {
			llvm::Value* aggregate = generateExpression(statement->expression);
			for (size_t index = 0; index < statement->names.size(); ++index) {
				const StoredVariable& target =
					lookupName(statement->names[index], statement->location);
				llvm::Value* value = builder_.CreateExtractValue(
					aggregate, static_cast<unsigned>(index),
					statement->names[index]);
				builder_.CreateStore(value, target.slot);
			}
			return;
		}
		}
	}

	void generateIf(const StmtPtr& statement) {
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::BasicBlock* thenBlock =
			llvm::BasicBlock::Create(context_, "if.then", function);
		llvm::BasicBlock* elseBlock =
			statement->alternative
				? llvm::BasicBlock::Create(context_, "if.else", function)
				: nullptr;
		llvm::BasicBlock* mergeBlock =
			llvm::BasicBlock::Create(context_, "if.end", function);
		llvm::Value* condition =
			toCondition(generateExpression(statement->expression));
		builder_.CreateCondBr(condition, thenBlock,
							  elseBlock ? elseBlock : mergeBlock);

		builder_.SetInsertPoint(thenBlock);
		generateStatement(statement->children[0]);
		bool thenTerminates = hasTerminator(builder_.GetInsertBlock());
		if (!thenTerminates)
			builder_.CreateBr(mergeBlock);

		bool elseTerminates = false;
		if (elseBlock) {
			builder_.SetInsertPoint(elseBlock);
			generateStatement(statement->alternative);
			elseTerminates = hasTerminator(builder_.GetInsertBlock());
			if (!elseTerminates)
				builder_.CreateBr(mergeBlock);
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
		llvm::BasicBlock* conditionBlock =
			llvm::BasicBlock::Create(context_, "while.cond", function);
		llvm::BasicBlock* bodyBlock =
			llvm::BasicBlock::Create(context_, "while.body", function);
		llvm::BasicBlock* endBlock =
			llvm::BasicBlock::Create(context_, "while.end", function);
		builder_.CreateBr(conditionBlock);
		builder_.SetInsertPoint(conditionBlock);
		builder_.CreateCondBr(
			toCondition(generateExpression(statement->expression)), bodyBlock,
			endBlock);
		builder_.SetInsertPoint(bodyBlock);
		generateStatement(statement->children[0]);
		if (!hasTerminator(builder_.GetInsertBlock()))
			builder_.CreateBr(conditionBlock);
		builder_.SetInsertPoint(endBlock);
	}

	void generateFor(const StmtPtr& statement) {
		scopes_.emplace_back();
		if (statement->initializer)
			generateStatement(statement->initializer);
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::BasicBlock* conditionBlock =
			llvm::BasicBlock::Create(context_, "for.cond", function);
		llvm::BasicBlock* bodyBlock =
			llvm::BasicBlock::Create(context_, "for.body", function);
		llvm::BasicBlock* stepBlock =
			llvm::BasicBlock::Create(context_, "for.step", function);
		llvm::BasicBlock* endBlock =
			llvm::BasicBlock::Create(context_, "for.end", function);
		builder_.CreateBr(conditionBlock);
		builder_.SetInsertPoint(conditionBlock);
		llvm::Value* condition =
			statement->expression
				? toCondition(generateExpression(statement->expression))
				: llvm::ConstantInt::getTrue(context_);
		builder_.CreateCondBr(condition, bodyBlock, endBlock);
		builder_.SetInsertPoint(bodyBlock);
		generateStatement(statement->children[0]);
		if (!hasTerminator(builder_.GetInsertBlock()))
			builder_.CreateBr(stepBlock);
		builder_.SetInsertPoint(stepBlock);
		if (statement->step)
			generateExpression(statement->step);
		if (!hasTerminator(builder_.GetInsertBlock()))
			builder_.CreateBr(conditionBlock);
		builder_.SetInsertPoint(endBlock);
		scopes_.pop_back();
	}

	static bool hasTerminator(const llvm::BasicBlock* block) {
		return !block->empty() && block->back().isTerminator();
	}

	llvm::Value* generateIfExpression(const ExprPtr& expression) {
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::BasicBlock* thenBlock =
			llvm::BasicBlock::Create(context_, "if.then.value", function);
		llvm::BasicBlock* elseBlock =
			llvm::BasicBlock::Create(context_, "if.else.value", function);
		llvm::BasicBlock* mergeBlock =
			llvm::BasicBlock::Create(context_, "if.end.value", function);
		llvm::Value* condition =
			toCondition(generateExpression(expression->children[0]));
		builder_.CreateCondBr(condition, thenBlock, elseBlock);
		builder_.SetInsertPoint(thenBlock);
		llvm::Value* thenValue	  = generateExpression(expression->children[1]);
		llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
		builder_.CreateBr(mergeBlock);
		builder_.SetInsertPoint(elseBlock);
		llvm::Value* elseValue	  = generateExpression(expression->children[2]);
		llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();
		builder_.CreateBr(mergeBlock);
		builder_.SetInsertPoint(mergeBlock);
		llvm::PHINode* result =
			builder_.CreatePHI(thenValue->getType(), 2, "if.value");
		result->addIncoming(thenValue, thenEnd);
		result->addIncoming(elseValue, elseEnd);
		return result;
	}

	llvm::Value* valueMatches(llvm::Value* subject,
							  const std::vector<ExprPtr>& values) {
		llvm::Value* matches = nullptr;
		for (const ExprPtr& value : values) {
			llvm::Value* equal;
			if (subject->getType()->isPointerTy()) {
				llvm::Value* literal = generateExpression(value);
				llvm::FunctionCallee strcmpFunction =
					module_.getOrInsertFunction(
						"strcmp", llvm::FunctionType::get(
									  llvm::Type::getInt32Ty(context_),
									  {llvm::PointerType::get(context_, 0),
									   llvm::PointerType::get(context_, 0)},
									  false));
				llvm::Value* comparison = builder_.CreateCall(
					strcmpFunction, {subject, literal}, "strcmp");
				equal = builder_.CreateICmpEQ(
					comparison,
					llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0),
					"string.case");
			} else {
				llvm::Value* literal = generateExpression(value);
				equal = builder_.CreateICmpEQ(subject, literal, "case.match");
			}
			matches =
				matches ? builder_.CreateOr(matches, equal, "case.or") : equal;
		}
		return matches;
	}

	llvm::Value* generateSwitchExpression(const ExprPtr& expression) {
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::Value* subject	 = generateExpression(expression->children[0]);
		llvm::BasicBlock* mergeBlock =
			llvm::BasicBlock::Create(context_, "switch.value.end", function);
		const ExprArm* defaultArm = nullptr;
		std::vector<const ExprArm*> valueArms;
		for (const ExprArm& arm : expression->arms) {
			if (arm.values.empty()) {
				defaultArm = &arm;
			} else {
				valueArms.push_back(&arm);
			}
		}
		std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;
		llvm::BasicBlock* defaultBlock =
			llvm::BasicBlock::Create(context_, "case.default.value", function);
		const size_t armCount = valueArms.size();
		for (size_t index = 0; index < armCount; ++index) {
			const ExprArm& arm = *valueArms[index];
			llvm::BasicBlock* armBlock =
				llvm::BasicBlock::Create(context_, "case.body.value", function);
			llvm::BasicBlock* falseTarget =
				(index + 1 < armCount)
					? llvm::BasicBlock::Create(context_, "case.next.value",
											   function)
					: defaultBlock;
			builder_.CreateCondBr(valueMatches(subject, arm.values), armBlock,
								  falseTarget);
			builder_.SetInsertPoint(armBlock);
			llvm::Value* value = generateExpression(arm.body);
			incoming.push_back({value, builder_.GetInsertBlock()});
			builder_.CreateBr(mergeBlock);
			if (index + 1 < armCount)
				builder_.SetInsertPoint(falseTarget);
		}
		builder_.SetInsertPoint(defaultBlock);
		llvm::Value* defaultValue = generateExpression(defaultArm->body);
		incoming.push_back({defaultValue, builder_.GetInsertBlock()});
		builder_.CreateBr(mergeBlock);
		builder_.SetInsertPoint(mergeBlock);
		llvm::PHINode* result = builder_.CreatePHI(
			incoming.front().first->getType(), incoming.size(), "switch.value");
		for (const auto& pair : incoming)
			result->addIncoming(pair.first, pair.second);
		return result;
	}

	void generateSwitchStatement(const StmtPtr& statement) {
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::Value* subject	 = generateExpression(statement->expression);
		llvm::BasicBlock* mergeBlock =
			llvm::BasicBlock::Create(context_, "switch.end", function);
		const SwitchArm* defaultArm = nullptr;
		std::vector<const SwitchArm*> valueArms;
		for (const SwitchArm& arm : statement->switchArms) {
			if (arm.values.empty()) {
				defaultArm = &arm;
			} else {
				valueArms.push_back(&arm);
			}
		}
		bool branched				 = false;
		bool lastArmNoNext			 = valueArms.empty();
		llvm::BasicBlock* evaluation = builder_.GetInsertBlock();
		for (size_t index = 0; index < valueArms.size(); ++index) {
			const SwitchArm& arm = *valueArms[index];
			llvm::BasicBlock* armBlock =
				llvm::BasicBlock::Create(context_, "case.body", function);
			llvm::BasicBlock* nextBlock =
				index + 1 < valueArms.size()
					? llvm::BasicBlock::Create(context_, "case.next", function)
					: nullptr;
			llvm::BasicBlock* falseTarget =
				nextBlock
					? nextBlock
					: (defaultArm ? llvm::BasicBlock::Create(
										context_, "case.default", function)
								  : llvm::BasicBlock::Create(
										context_, "case.end", function));
			builder_.CreateCondBr(valueMatches(subject, arm.values), armBlock,
								  falseTarget);
			builder_.SetInsertPoint(armBlock);
			scopes_.emplace_back();
			generateStatement(arm.body);
			scopes_.pop_back();
			if (!hasTerminator(builder_.GetInsertBlock())) {
				builder_.CreateBr(mergeBlock);
				branched = true;
			}
			if (nextBlock) {
				builder_.SetInsertPoint(nextBlock);
			}
			evaluation	  = falseTarget;
			lastArmNoNext = (nextBlock == nullptr);
		}
		if (defaultArm) {
			builder_.SetInsertPoint(evaluation);
			scopes_.emplace_back();
			generateStatement(defaultArm->body);
			scopes_.pop_back();
			if (!hasTerminator(builder_.GetInsertBlock())) {
				builder_.CreateBr(mergeBlock);
				branched = true;
			}
		} else if (valueArms.empty() || lastArmNoNext) {
			builder_.SetInsertPoint(evaluation);
			if (!hasTerminator(builder_.GetInsertBlock())) {
				builder_.CreateBr(mergeBlock);
				branched = true;
			}
		}
		if (branched) {
			builder_.SetInsertPoint(mergeBlock);
		} else {
			mergeBlock->eraseFromParent();
		}
	}

	llvm::Value* toCondition(llvm::Value* value) {
		if (value->getType()->isIntegerTy(1))
			return value;
		if (value->getType()->isIntegerTy()) {
			return builder_.CreateICmpNE(
				value, llvm::ConstantInt::get(value->getType(), 0), "truth");
		}
		return builder_.CreateFCmpONE(
			value, llvm::ConstantFP::get(value->getType(), 0.0), "truth");
	}

	llvm::Constant* stringPointer(const std::string& value,
								  const std::string& name) {
		llvm::Constant* data =
			llvm::ConstantDataArray::getString(context_, value, true);
		auto* global = new llvm::GlobalVariable(
			module_, data->getType(), true, llvm::GlobalValue::PrivateLinkage,
			data, name);
		llvm::Constant* zero =
			llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0);
		llvm::Constant* indices[] = {zero, zero};
		return llvm::ConstantExpr::getInBoundsGetElementPtr(data->getType(),
															global, indices);
	}

	StoredVariable& lookupName(const std::string& name, Location location) {
		for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
			auto variable = scope->find(name);
			if (variable != scope->end())
				return variable->second;
		}
		throw Diagnostic(location, "unknown variable '" + name + "'");
	}

	StoredVariable& lookupVariable(const ExprPtr& expression) {
		return lookupName(expression->text, expression->location);
	}

	Type collectionElementType(const StoredVariable& variable,
							   Location location) {
		if (variable.type == Type::Array)
			return arrayElementType(variable.typeName, location);
		if (variable.type == Type::Vector)
			return vectorElementType(variable.typeName, location);
		return mapValueType(variable.typeName, location);
	}

	llvm::Value* indexValue(uint64_t value) {
		return llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), value);
	}

	llvm::Value* intConstant(uint64_t value) {
		return indexValue(value);
	}

	llvm::Value* callMalloc(llvm::Value* byteCount) {
		llvm::PointerType* pointerType = llvm::PointerType::get(context_, 0);
		llvm::FunctionCallee mallocFunction = module_.getOrInsertFunction(
			"malloc",
			llvm::FunctionType::get(pointerType,
									{llvm::Type::getInt64Ty(context_)}, false));
		return builder_.CreateCall(mallocFunction, {byteCount}, "alloc");
	}

	llvm::Value* callMalloc(llvm::Value* count, llvm::Type* elementType) {
		const uint64_t elementBytes =
			module_.getDataLayout().getTypeAllocSize(elementType);
		llvm::Value* byteCount = builder_.CreateMul(
			builder_.CreateZExt(count, llvm::Type::getInt64Ty(context_)),
			llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_),
								   elementBytes),
			"alloc.bytes");
		return callMalloc(byteCount);
	}

	llvm::Value* callMalloc(uint64_t count, llvm::Type* elementType) {
		const uint64_t bytes =
			static_cast<uint64_t>(
				module_.getDataLayout().getTypeAllocSize(elementType)) *
			count;
		return callMalloc(
			llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), bytes));
	}

	void callFree(llvm::Value* pointer) {
		llvm::PointerType* pointerType	  = llvm::PointerType::get(context_, 0);
		llvm::FunctionCallee freeFunction = module_.getOrInsertFunction(
			"free", llvm::FunctionType::get(llvm::Type::getVoidTy(context_),
											{pointerType}, false));
		builder_.CreateCall(freeFunction, {pointer});
	}

	llvm::Value* vectorData(llvm::Value* slot) {
		llvm::StructType* structType = vectorStructType();
		llvm::Value* fieldPointer =
			builder_.CreateStructGEP(structType, slot, 0);
		return builder_.CreateLoad(llvm::PointerType::get(context_, 0),
								   fieldPointer, "vec.data");
	}

	llvm::Value* vectorLength(llvm::Value* slot) {
		llvm::StructType* structType = vectorStructType();
		llvm::Value* fieldPointer =
			builder_.CreateStructGEP(structType, slot, 1);
		return builder_.CreateLoad(llvm::Type::getInt32Ty(context_),
								   fieldPointer, "vec.len");
	}

	llvm::Value* vectorCapacity(llvm::Value* slot) {
		llvm::StructType* structType = vectorStructType();
		llvm::Value* fieldPointer =
			builder_.CreateStructGEP(structType, slot, 2);
		return builder_.CreateLoad(llvm::Type::getInt32Ty(context_),
								   fieldPointer, "vec.cap");
	}

	void storeVectorFields(llvm::Value* slot, llvm::Value* data,
						   llvm::Value* length, llvm::Value* capacity) {
		llvm::StructType* structType = vectorStructType();
		builder_.CreateStore(data,
							 builder_.CreateStructGEP(structType, slot, 0));
		builder_.CreateStore(length,
							 builder_.CreateStructGEP(structType, slot, 1));
		builder_.CreateStore(capacity,
							 builder_.CreateStructGEP(structType, slot, 2));
	}

	// Ensures the vector has room for one more element, growing the heap buffer
	// (doubling capacity) when needed. Afterwards callers must reload the data
	// pointer and length from the struct fields.
	void reserveVector(llvm::Value* slot, llvm::Type* elementType) {
		llvm::Value* length	  = vectorLength(slot);
		llvm::Value* capacity = vectorCapacity(slot);
		if (length->getType() != capacity->getType()) {
			capacity = builder_.CreateIntCast(capacity, length->getType(), true, "cap.cast");
		}
		llvm::Value* needed =
			builder_.CreateICmpSGE(length, capacity, "vec.grow.needed");
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::BasicBlock* growBlock =
			llvm::BasicBlock::Create(context_, "vec.grow", function);
		llvm::BasicBlock* readyBlock =
			llvm::BasicBlock::Create(context_, "vec.ready", function);
		builder_.CreateCondBr(needed, growBlock, readyBlock);
		builder_.SetInsertPoint(growBlock);
		llvm::Value* doubled =
			builder_.CreateAdd(capacity, capacity, "vec.double");
		llvm::Value* newCapacity = builder_.CreateSelect(
			builder_.CreateICmpEQ(capacity, indexValue(0), "vec.empty"),
			indexValue(4), doubled, "vec.newcap");
		llvm::Value* oldData = vectorData(slot);
		llvm::Value* newData = callMalloc(
			builder_.CreateZExt(newCapacity, llvm::Type::getInt64Ty(context_),
								"vec.bytes.count"),
			elementType);
		emitCopyLoop(newData, oldData, length, elementType, "vec.copy");
		callFree(oldData);
		storeVectorFields(slot, newData, length, newCapacity);
		builder_.CreateBr(readyBlock);
		builder_.SetInsertPoint(readyBlock);
	}

	// Emits a loop that copies `count` elements from `source` to `destination`.
	// On return the builder inserts into the loop exit block.
	void emitCopyLoop(llvm::Value* destination, llvm::Value* source,
					  llvm::Value* count, llvm::Type* elementType,
					  const std::string& prefix) {
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::BasicBlock* conditionBlock =
			llvm::BasicBlock::Create(context_, prefix + ".cond", function);
		llvm::BasicBlock* bodyBlock =
			llvm::BasicBlock::Create(context_, prefix + ".body", function);
		llvm::BasicBlock* exitBlock =
			llvm::BasicBlock::Create(context_, prefix + ".exit", function);
		llvm::BasicBlock* entryBlock = builder_.GetInsertBlock();
		builder_.CreateBr(conditionBlock);
		builder_.SetInsertPoint(conditionBlock);
		llvm::PHINode* index = builder_.CreatePHI(
			llvm::Type::getInt32Ty(context_), 2, prefix + ".i");
		index->addIncoming(indexValue(0), entryBlock);
		llvm::Value* more =
			builder_.CreateICmpSLT(index, count, prefix + ".more");
		builder_.CreateCondBr(more, bodyBlock, exitBlock);
		builder_.SetInsertPoint(bodyBlock);
		llvm::Value* sourceElement = builder_.CreateInBoundsGEP(
			elementType, source, index, prefix + ".src");
		llvm::Value* destinationElement = builder_.CreateInBoundsGEP(
			elementType, destination, index, prefix + ".dst");
		llvm::Value* copy =
			builder_.CreateLoad(elementType, sourceElement, prefix + ".value");
		builder_.CreateStore(copy, destinationElement);
		llvm::Value* next =
			builder_.CreateAdd(index, indexValue(1), prefix + ".next");
		index->addIncoming(next, builder_.GetInsertBlock());
		builder_.CreateBr(conditionBlock);
		builder_.SetInsertPoint(exitBlock);
	}

	llvm::Value* mapCount(llvm::Value* slot) {
		llvm::StructType* structType = mapStructType();
		llvm::Value* fieldPointer =
			builder_.CreateStructGEP(structType, slot, 0);
		return builder_.CreateLoad(llvm::Type::getInt32Ty(context_),
								   fieldPointer, "map.count");
	}

	llvm::Value* mapCapacity(llvm::Value* slot) {
		llvm::StructType* structType = mapStructType();
		llvm::Value* fieldPointer =
			builder_.CreateStructGEP(structType, slot, 1);
		return builder_.CreateLoad(llvm::Type::getInt32Ty(context_),
								   fieldPointer, "map.cap");
	}

	llvm::Value* mapEntries(llvm::Value* slot) {
		llvm::StructType* structType = mapStructType();
		llvm::Value* fieldPointer =
			builder_.CreateStructGEP(structType, slot, 2);
		return builder_.CreateLoad(llvm::PointerType::get(context_, 0),
								   fieldPointer, "map.entries");
	}

	void storeMapFields(llvm::Value* slot, llvm::Value* entries,
						llvm::Value* count, llvm::Value* capacity) {
		llvm::StructType* structType = mapStructType();
		builder_.CreateStore(count,
							 builder_.CreateStructGEP(structType, slot, 0));
		builder_.CreateStore(capacity,
							 builder_.CreateStructGEP(structType, slot, 1));
		builder_.CreateStore(entries,
							 builder_.CreateStructGEP(structType, slot, 2));
	}

	// Finds the entry index for `key`, or returns -1 when the key is absent.
	// Emits a linear scan and leaves the insert point in the exit block.
	llvm::Value* mapFindIndex(llvm::Value* slot, llvm::Value* key,
							  llvm::StructType* entryType) {
		llvm::Function* function	= builder_.GetInsertBlock()->getParent();
		llvm::AllocaInst* foundSlot = createEntryAlloca(
			function, "map.found", llvm::Type::getInt32Ty(context_));
		builder_.CreateStore(
			llvm::ConstantInt::getSigned(llvm::Type::getInt32Ty(context_), -1),
			foundSlot);
		llvm::BasicBlock* conditionBlock =
			llvm::BasicBlock::Create(context_, "map.scan.cond", function);
		llvm::BasicBlock* bodyBlock =
			llvm::BasicBlock::Create(context_, "map.scan.body", function);
		llvm::BasicBlock* exitBlock =
			llvm::BasicBlock::Create(context_, "map.scan.exit", function);
		llvm::Value* entries		 = mapEntries(slot);
		llvm::Value* count			 = mapCount(slot);
		llvm::BasicBlock* entryBlock = builder_.GetInsertBlock();
		builder_.CreateBr(conditionBlock);
		builder_.SetInsertPoint(conditionBlock);
		llvm::PHINode* index = builder_.CreatePHI(
			llvm::Type::getInt32Ty(context_), 2, "map.scan.i");
		index->addIncoming(indexValue(0), entryBlock);
		// continue while i < count and nothing found yet
		llvm::Value* more =
			builder_.CreateICmpSLT(index, count, "map.scan.more");
		llvm::Value* notFound = builder_.CreateICmpEQ(
			builder_.CreateLoad(llvm::Type::getInt32Ty(context_), foundSlot,
								"map.scan.found"),
			llvm::ConstantInt::getSigned(llvm::Type::getInt32Ty(context_), -1),
			"map.scan.isnew");
		llvm::Value* keepGoing =
			builder_.CreateAnd(more, notFound, "map.scan.keepgoing");
		builder_.CreateCondBr(keepGoing, bodyBlock, exitBlock);
		builder_.SetInsertPoint(bodyBlock);
		llvm::Value* entryPointer =
			builder_.CreateInBoundsGEP(entryType, entries, index, "map.entry");
		llvm::Value* entryKeySlot =
			builder_.CreateStructGEP(entryType, entryPointer, 0);
		llvm::Value* entryKey = builder_.CreateLoad(
			llvm::PointerType::get(context_, 0), entryKeySlot, "map.entrykey");
		llvm::FunctionCallee strcmpFunction = module_.getOrInsertFunction(
			"strcmp",
			llvm::FunctionType::get(llvm::Type::getInt32Ty(context_),
									{llvm::PointerType::get(context_, 0),
									 llvm::PointerType::get(context_, 0)},
									false));
		llvm::Value* compared =
			builder_.CreateCall(strcmpFunction, {key, entryKey}, "map.keycmp");
		llvm::Value* match = builder_.CreateICmpEQ(
			compared,
			llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0),
			"map.keymatch");
		llvm::BasicBlock* storeBlock =
			llvm::BasicBlock::Create(context_, "map.scan.hit", function);
		llvm::BasicBlock* stepBlock =
			llvm::BasicBlock::Create(context_, "map.scan.step", function);
		builder_.CreateCondBr(match, storeBlock, stepBlock);
		builder_.SetInsertPoint(storeBlock);
		builder_.CreateStore(index, foundSlot);
		builder_.CreateBr(stepBlock);
		builder_.SetInsertPoint(stepBlock);
		llvm::Value* next =
			builder_.CreateAdd(index, indexValue(1), "map.scan.next");
		index->addIncoming(next, builder_.GetInsertBlock());
		builder_.CreateBr(conditionBlock);
		builder_.SetInsertPoint(exitBlock);
		return builder_.CreateLoad(llvm::Type::getInt32Ty(context_), foundSlot,
								   "map.index");
	}

	// Ensures the map has room for one more entry, then returns the entry
	// pointer to fill (creating an entry when needed is up to the caller).
	// Reload count and entries from the struct fields afterwards.
	llvm::Value* reserveMapEntry(llvm::Value* slot,
								 llvm::StructType* entryType) {
		llvm::Value* count	  = mapCount(slot);
		llvm::Value* capacity = mapCapacity(slot);
		llvm::Value* needed =
			builder_.CreateICmpSGE(count, capacity, "map.grow.needed");
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::BasicBlock* growBlock =
			llvm::BasicBlock::Create(context_, "map.grow", function);
		llvm::BasicBlock* readyBlock =
			llvm::BasicBlock::Create(context_, "map.ready", function);
		builder_.CreateCondBr(needed, growBlock, readyBlock);
		builder_.SetInsertPoint(growBlock);
		llvm::Value* doubled =
			builder_.CreateAdd(capacity, capacity, "map.double");
		llvm::Value* newCapacity = builder_.CreateSelect(
			builder_.CreateICmpEQ(capacity, indexValue(0), "map.empty"),
			indexValue(4), doubled, "map.newcap");
		llvm::Value* oldEntries = mapEntries(slot);
		llvm::Value* newEntries = callMalloc(
			builder_.CreateZExt(newCapacity, llvm::Type::getInt64Ty(context_),
								"map.bytes.count"),
			entryType);
		emitCopyLoop(newEntries, oldEntries, count, entryType, "map.copy");
		callFree(oldEntries);
		storeMapFields(slot, newEntries, count, newCapacity);
		builder_.CreateBr(readyBlock);
		builder_.SetInsertPoint(readyBlock);
		llvm::Value* entries	  = mapEntries(slot);
		llvm::Value* index		  = mapCount(slot);
		llvm::Value* entryPointer = builder_.CreateInBoundsGEP(
			entryType, entries, index, "map.newentry");
		return entryPointer;
	}

	// map[m[key]] read: returns the value for key, or the zero value when
	// absent.
	llvm::Value* mapLookupValue(llvm::Value* slot, llvm::Value* key,
								Type valueType) {
		llvm::StructType* entryType = mapEntryType(valueType);
		llvm::Value* index			= mapFindIndex(slot, key, entryType);
		llvm::Value* entries		= mapEntries(slot);
		llvm::Value* present =
			builder_.CreateICmpSGE(index, indexValue(0), "map.present");
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::BasicBlock* foundBlock =
			llvm::BasicBlock::Create(context_, "map.hit", function);
		llvm::BasicBlock* missBlock =
			llvm::BasicBlock::Create(context_, "map.miss", function);
		llvm::BasicBlock* mergeBlock =
			llvm::BasicBlock::Create(context_, "map.join", function);
		builder_.CreateCondBr(present, foundBlock, missBlock);
		builder_.SetInsertPoint(foundBlock);
		llvm::Value* entryPointer =
			builder_.CreateInBoundsGEP(entryType, entries, index, "map.entry");
		llvm::Value* valueSlot =
			builder_.CreateStructGEP(entryType, entryPointer, 1);
		llvm::Value* value =
			builder_.CreateLoad(llvmType(valueType), valueSlot, "map.value");
		builder_.CreateBr(mergeBlock);
		builder_.SetInsertPoint(missBlock);
		llvm::Value* zero = llvm::Constant::getNullValue(llvmType(valueType));
		builder_.CreateBr(mergeBlock);
		builder_.SetInsertPoint(mergeBlock);
		llvm::PHINode* result =
			builder_.CreatePHI(llvmType(valueType), 2, "map.lookup");
		result->addIncoming(value, foundBlock);
		result->addIncoming(zero, missBlock);
		return result;
	}

	// map[key] = value or map.insert(key, value): overwrite or append.
	void mapAssignValue(llvm::Value* slot, llvm::Value* key, llvm::Value* value,
						Type valueType) {
		llvm::StructType* entryType = mapEntryType(valueType);
		llvm::Value* index			= mapFindIndex(slot, key, entryType);
		llvm::Value* present =
			builder_.CreateICmpSGE(index, indexValue(0), "map.present");
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::BasicBlock* overwriteBlock =
			llvm::BasicBlock::Create(context_, "map.overwrite", function);
		llvm::BasicBlock* appendBlock =
			llvm::BasicBlock::Create(context_, "map.append", function);
		llvm::BasicBlock* doneBlock =
			llvm::BasicBlock::Create(context_, "map.done", function);
		builder_.CreateCondBr(present, overwriteBlock, appendBlock);
		builder_.SetInsertPoint(overwriteBlock);
		llvm::Value* entries = mapEntries(slot);
		llvm::Value* entryPointer =
			builder_.CreateInBoundsGEP(entryType, entries, index, "map.entry");
		llvm::Value* valueSlot =
			builder_.CreateStructGEP(entryType, entryPointer, 1);
		builder_.CreateStore(value, valueSlot);
		builder_.CreateBr(doneBlock);
		builder_.SetInsertPoint(appendBlock);
		llvm::Value* newEntryPointer = reserveMapEntry(slot, entryType);
		llvm::Value* keySlot =
			builder_.CreateStructGEP(entryType, newEntryPointer, 0);
		llvm::Value* newValueSlot =
			builder_.CreateStructGEP(entryType, newEntryPointer, 1);
		builder_.CreateStore(key, keySlot);
		builder_.CreateStore(value, newValueSlot);
		llvm::Value* count = mapCount(slot);
		builder_.CreateStore(
			builder_.CreateAdd(count, indexValue(1), "map.nextcount"),
			builder_.CreateStructGEP(mapStructType(), slot, 0));
		builder_.CreateBr(doneBlock);
		builder_.SetInsertPoint(doneBlock);
	}

	llvm::Value* collectionElementAddress(const StoredVariable& variable,
										  llvm::Value* index,
										  llvm::Type* elementLLVM) {
		if (variable.type == Type::Array) {
			llvm::Type* arrayType =
				llvm::cast<llvm::AllocaInst>(variable.slot)->getAllocatedType();
			llvm::Value* zero = indexValue(0);
			if (zero->getType() != index->getType()) {
				zero = builder_.CreateIntCast(zero, index->getType(), true,
											  "zero.cast");
			}
			return builder_.CreateInBoundsGEP(
				arrayType, variable.slot,
				llvm::ArrayRef<llvm::Value*>{zero, index},
				"index.address");
		}
		llvm::Value* data = vectorData(variable.slot);
		return builder_.CreateInBoundsGEP(elementLLVM, data, index,
										  "index.address");
	}

	void emitTrapIf(llvm::Value* condition) {
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::BasicBlock* okBlock =
			llvm::BasicBlock::Create(context_, "bounds.ok", function);
		llvm::BasicBlock* failBlock =
			llvm::BasicBlock::Create(context_, "bounds.fail", function);
		builder_.CreateCondBr(condition, failBlock, okBlock);
		builder_.SetInsertPoint(failBlock);
		llvm::Function* trap = llvm::Intrinsic::getOrInsertDeclaration(
			&module_, llvm::Intrinsic::trap);
		builder_.CreateCall(trap);
		builder_.CreateUnreachable();
		builder_.SetInsertPoint(okBlock);
	}

	void emitRangeCheck(llvm::Value* index, llvm::Value* length) {
		if (index->getType() != length->getType()) {
			index = builder_.CreateIntCast(index, length->getType(), true,
										  "index.cast");
		}
		llvm::Value* tooBig =
			builder_.CreateICmpSGE(index, length, "bounds.high");
		llvm::Value* tooSmall =
			builder_.CreateICmpSLT(index, indexValue(0), "bounds.low");
		emitTrapIf(builder_.CreateOr(tooBig, tooSmall, "bounds.invalid"));
	}

	llvm::Value* readCollectionElement(const ExprPtr& expression) {
		const ExprPtr& base = expression->children[0];
		llvm::Value* index	= generateExpression(expression->children[1]);
		if (base->kind == Expr::Kind::Name) {
			auto variable = lookupVariable(base);
			if (variable.type == Type::Array) {
				Type elementType =
					collectionElementType(variable, expression->location);
				llvm::Value* address = collectionElementAddress(
					variable, index, llvmType(elementType));
				return builder_.CreateLoad(llvmType(elementType), address,
										   "index.value");
			}
			if (variable.type == Type::Vector) {
				Type elementType =
					collectionElementType(variable, expression->location);
				emitRangeCheck(index, vectorLength(variable.slot));
				llvm::Value* address = collectionElementAddress(
					variable, index, llvmType(elementType));
				return builder_.CreateLoad(llvmType(elementType), address,
										   "index.value");
			}
			if (variable.type == Type::Map) {
				if (index->getType()->isIntegerTy()) {
					// Positional key access used by the for-in desugaring for
					// maps.
					llvm::StructType* entryType = mapEntryType(
						mapValueType(variable.typeName, expression->location));
					llvm::Value* entries = mapEntries(variable.slot);
					emitRangeCheck(index, mapCount(variable.slot));
					llvm::Value* entryPointer = builder_.CreateInBoundsGEP(
						entryType, entries, index, "map.positional");
					llvm::Value* keySlot =
						builder_.CreateStructGEP(entryType, entryPointer, 0);
					return builder_.CreateLoad(
						llvm::PointerType::get(context_, 0), keySlot,
						"map.key");
				}
				Type valueType =
					collectionElementType(variable, expression->location);
				return mapLookupValue(variable.slot, index, valueType);
			}
		}
		llvm::Value* baseValue = generateExpression(base);
		llvm::Value* address =
			builder_.CreateInBoundsGEP(llvm::PointerType::get(context_, 0),
									   baseValue, index, "index.address");
		return builder_.CreateLoad(llvm::PointerType::get(context_, 0), address,
								   "index");
	}

	void storeCollectionElement(const ExprPtr& indexExpression,
								llvm::Value* value) {
		const ExprPtr& base = indexExpression->children[0];
		auto variable		= lookupVariable(base);
		if (variable.type == Type::Map) {
			Type valueType =
				collectionElementType(variable, indexExpression->location);
			mapAssignValue(variable.slot,
						   generateExpression(indexExpression->children[1]),
						   value, valueType);
			return;
		}
		if (variable.type == Type::Array || variable.type == Type::Vector) {
			Type elementType =
				collectionElementType(variable, indexExpression->location);
			llvm::Value* index =
				generateExpression(indexExpression->children[1]);
			if (variable.type == Type::Vector) {
				emitRangeCheck(index, vectorLength(variable.slot));
			}
			llvm::Value* address = collectionElementAddress(
				variable, index, llvmType(elementType));
			builder_.CreateStore(value, address);
			return;
		}
		throw Diagnostic(
			indexExpression->location,
			"index assignment is only supported for arrays, vectors, and maps");
	}

	llvm::Value* generateExpression(const ExprPtr& expression) {
		switch (expression->kind) {
		case Expr::Kind::Integer: {
			llvm::APInt value(128, expression->text, 10);
			unsigned bits = llvmType(Type::Isize)->getPrimitiveSizeInBits();
			if (bits < 128)
				value = value.trunc(bits);
			return llvm::ConstantInt::get(context_, value);
		}
		case Expr::Kind::Float:
			return llvm::ConstantFP::get(llvmType(Type::F64),
										 std::stod(expression->text));
		case Expr::Kind::String:
			return stringPointer(expression->text, "str");
		case Expr::Kind::Boolean:
			return llvm::ConstantInt::get(llvmType(Type::Bool),
										  expression->text == "true");
		case Expr::Kind::Name: {
			auto variable = lookupVariable(expression);
			if (variable.type == Type::Array || variable.type == Type::Vector ||
				variable.type == Type::Map) {
				throw Diagnostic(expression->location,
								 "collection '" + expression->text +
									 "' must be used with an index, an "
									 "iterator, or its methods");
			}
			return builder_.CreateLoad(llvmType(variable.type), variable.slot,
									   expression->text);
		}
		case Expr::Kind::ArrayLiteral:
		case Expr::Kind::MapLiteral:
			throw Diagnostic(expression->location,
							 "collection literals are only supported as "
							 "variable initializers");
		case Expr::Kind::Index:
			return readCollectionElement(expression);
		case Expr::Kind::Unary: {
			if (expression->text == "++" || expression->text == "--" ||
				expression->text == "post++" || expression->text == "post--") {
				auto variable		  = lookupVariable(expression->children[0]);
				llvm::Value* oldValue = builder_.CreateLoad(
					llvmType(variable.type), variable.slot, "increment.old");
				llvm::Value* one =
					variable.type == Type::F64
						? static_cast<llvm::Value*>(
							  llvm::ConstantFP::get(llvmType(Type::F64), 1.0))
						: static_cast<llvm::Value*>(
							  llvm::ConstantInt::get(llvmType(Type::I32), 1));
				bool decrement =
					expression->text == "--" || expression->text == "post--";
				llvm::Value* next =
					arithmetic(decrement ? "-" : "+", oldValue, one);
				builder_.CreateStore(next, variable.slot);
				return expression->text.rfind("post", 0) == 0 ? oldValue : next;
			}
			llvm::Value* value = generateExpression(expression->children[0]);
			if (expression->text == "!")
				return builder_.CreateNot(toCondition(value), "not");
			if (expression->text == "-") {
				return value->getType()->isFloatingPointTy()
						   ? builder_.CreateFNeg(value, "neg")
						   : builder_.CreateNeg(value, "neg");
			}
			return value;
		}
		case Expr::Kind::Binary:
			return generateBinary(expression);
		case Expr::Kind::If:
			return generateIfExpression(expression);
		case Expr::Kind::Switch:
			return generateSwitchExpression(expression);
		case Expr::Kind::Call:
			return generateCall(expression);
		}
		return nullptr;
	}

	llvm::Value* generateBinary(const ExprPtr& expression) {
		const std::string& op = expression->text;
		if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") {
			const ExprPtr& target = expression->children[0];
			if (target->kind == Expr::Kind::Index) {
				if (op != "=") {
					throw Diagnostic(target->location,
									 "compound assignment is not supported on "
									 "collection elements");
				}
				storeCollectionElement(
					target, generateExpression(expression->children[1]));
				return nullptr;
			}
			auto targetVariable = lookupVariable(target);
			llvm::Value* rhs	= generateExpression(expression->children[1]);
			if (op != "=") {
				llvm::Value* lhs = builder_.CreateLoad(
					llvmType(targetVariable.type), targetVariable.slot);
				rhs = arithmetic(op.substr(0, 1), lhs, rhs);
			}
			builder_.CreateStore(rhs, targetVariable.slot);
			return rhs;
		}
		llvm::Value* lhs = generateExpression(expression->children[0]);
		if (op == "&&" || op == "||") {
			llvm::Value* leftCondition = toCondition(lhs);
			llvm::Function* function   = builder_.GetInsertBlock()->getParent();
			llvm::BasicBlock* leftBlock = builder_.GetInsertBlock();
			llvm::BasicBlock* rightBlock =
				llvm::BasicBlock::Create(context_, "logic.rhs", function);
			llvm::BasicBlock* mergeBlock =
				llvm::BasicBlock::Create(context_, "logic.end", function);
			bool isAnd = op == "&&";
			builder_.CreateCondBr(leftCondition,
								  isAnd ? rightBlock : mergeBlock,
								  isAnd ? mergeBlock : rightBlock);
			builder_.SetInsertPoint(rightBlock);
			llvm::Value* rightCondition =
				toCondition(generateExpression(expression->children[1]));
			llvm::BasicBlock* rightEnd = builder_.GetInsertBlock();
			if (!hasTerminator(rightEnd))
				builder_.CreateBr(mergeBlock);
			builder_.SetInsertPoint(mergeBlock);
			llvm::PHINode* result =
				builder_.CreatePHI(llvm::Type::getInt1Ty(context_), 2, "logic");
			result->addIncoming(
				llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), !isAnd),
				leftBlock);
			result->addIncoming(rightCondition, rightEnd);
			return result;
		}
		llvm::Value* rhs = generateExpression(expression->children[1]);
		if (rhs->getType() != lhs->getType() && rhs->getType()->isIntegerTy() && lhs->getType()->isIntegerTy()) {
			rhs = builder_.CreateIntCast(rhs, lhs->getType(), true, "rhs.cast");
		}
		if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
			op == ">=") {
			bool floating = lhs->getType()->isFloatingPointTy();
			if (lhs->getType()->isPointerTy() && (op == "==" || op == "!=")) {
				llvm::FunctionCallee strcmpFunction =
					module_.getOrInsertFunction(
						"strcmp", llvm::FunctionType::get(
									  llvm::Type::getInt32Ty(context_),
									  {llvm::PointerType::get(context_, 0),
									   llvm::PointerType::get(context_, 0)},
									  false));
				llvm::Value* comparison =
					builder_.CreateCall(strcmpFunction, {lhs, rhs}, "strcmp");
				return op == "=="
						   ? builder_.CreateICmpEQ(
								 comparison,
								 llvm::ConstantInt::get(
									 llvm::Type::getInt32Ty(context_), 0),
								 "string.eq")
						   : builder_.CreateICmpNE(
								 comparison,
								 llvm::ConstantInt::get(
									 llvm::Type::getInt32Ty(context_), 0),
								 "string.ne");
			}
			if (floating) {
				if (op == "==")
					return builder_.CreateFCmpOEQ(lhs, rhs, "cmp");
				if (op == "!=")
					return builder_.CreateFCmpUNE(lhs, rhs, "cmp");
				if (op == "<")
					return builder_.CreateFCmpOLT(lhs, rhs, "cmp");
				if (op == "<=")
					return builder_.CreateFCmpOLE(lhs, rhs, "cmp");
				if (op == ">")
					return builder_.CreateFCmpOGT(lhs, rhs, "cmp");
				return builder_.CreateFCmpOGE(lhs, rhs, "cmp");
			}
			if (op == "==")
				return builder_.CreateICmpEQ(lhs, rhs, "cmp");
			if (op == "!=")
				return builder_.CreateICmpNE(lhs, rhs, "cmp");
			if (op == "<")
				return builder_.CreateICmpSLT(lhs, rhs, "cmp");
			if (op == "<=")
				return builder_.CreateICmpSLE(lhs, rhs, "cmp");
			if (op == ">")
				return builder_.CreateICmpSGT(lhs, rhs, "cmp");
			return builder_.CreateICmpSGE(lhs, rhs, "cmp");
		}
		return arithmetic(op, lhs, rhs);
	}

	llvm::Value* arithmetic(const std::string& op, llvm::Value* lhs,
							llvm::Value* rhs) {
		if (lhs->getType() != rhs->getType() && lhs->getType()->isIntegerTy() &&
			rhs->getType()->isIntegerTy()) {
			rhs = builder_.CreateIntCast(rhs, lhs->getType(), true,
										 "rhs.cast");
		}
		bool floating = lhs->getType()->isFloatingPointTy();
		if (!floating && (op == "/" || op == "%")) {
			llvm::Value* divisionByZero = builder_.CreateICmpEQ(
				rhs, llvm::ConstantInt::get(rhs->getType(), 0),
				"division.zero");
			llvm::Value* minimum = builder_.CreateICmpEQ(
				lhs,
				llvm::ConstantInt::get(lhs->getType(),
									   -(int64_t(1) << (lhs->getType()->getPrimitiveSizeInBits() - 1))),
				"division.minimum");
			llvm::Value* negativeOne = builder_.CreateICmpEQ(
				rhs, llvm::ConstantInt::getSigned(rhs->getType(), -1),
				"division.negative.one");
			llvm::Value* overflow =
				builder_.CreateAnd(minimum, negativeOne, "division.overflow");
			llvm::Value* invalid =
				builder_.CreateOr(divisionByZero, overflow, "division.invalid");
			llvm::Function* function = builder_.GetInsertBlock()->getParent();
			llvm::BasicBlock* validBlock =
				llvm::BasicBlock::Create(context_, "division.valid", function);
			llvm::BasicBlock* failureBlock = llvm::BasicBlock::Create(
				context_, "division.failure", function);
			builder_.CreateCondBr(invalid, failureBlock, validBlock);
			builder_.SetInsertPoint(failureBlock);
			llvm::Function* trap = llvm::Intrinsic::getOrInsertDeclaration(
				&module_, llvm::Intrinsic::trap);
			builder_.CreateCall(trap);
			builder_.CreateUnreachable();
			builder_.SetInsertPoint(validBlock);
		}
		if (floating) {
			if (op == "+")
				return builder_.CreateFAdd(lhs, rhs, "add");
			if (op == "-")
				return builder_.CreateFSub(lhs, rhs, "sub");
			if (op == "*")
				return builder_.CreateFMul(lhs, rhs, "mul");
			if (op == "/")
				return builder_.CreateFDiv(lhs, rhs, "div");
			if (op == "%")
				return builder_.CreateFRem(lhs, rhs, "rem");
		} else {
			if (op == "+")
				return builder_.CreateAdd(lhs, rhs, "add");
			if (op == "-")
				return builder_.CreateSub(lhs, rhs, "sub");
			if (op == "*")
				return builder_.CreateMul(lhs, rhs, "mul");
			if (op == "/")
				return builder_.CreateSDiv(lhs, rhs, "div");
			if (op == "%")
				return builder_.CreateSRem(lhs, rhs, "rem");
		}
		return nullptr;
	}

	// for (int j = length; j > index; --j) data[j] = data[j - 1];
	void emitShiftRight(llvm::Value* data, llvm::Type* elementType,
						llvm::Value* fromIndex, llvm::Value* length) {
		if (fromIndex->getType() != length->getType()) {
			fromIndex = builder_.CreateIntCast(fromIndex, length->getType(),
											   true, "index.cast");
		}
		llvm::Function* function = builder_.GetInsertBlock()->getParent();
		llvm::BasicBlock* conditionBlock =
			llvm::BasicBlock::Create(context_, "shift.cond", function);
		llvm::BasicBlock* bodyBlock =
			llvm::BasicBlock::Create(context_, "shift.body", function);
		llvm::BasicBlock* exitBlock =
			llvm::BasicBlock::Create(context_, "shift.exit", function);
		llvm::BasicBlock* entryBlock = builder_.GetInsertBlock();
		builder_.CreateBr(conditionBlock);
		builder_.SetInsertPoint(conditionBlock);
		llvm::PHINode* cursor =
			builder_.CreatePHI(llvm::Type::getInt32Ty(context_), 2, "shift.j");
		cursor->addIncoming(length, entryBlock);
		llvm::Value* keepGoing =
			builder_.CreateICmpSGT(cursor, fromIndex, "shift.more");
		builder_.CreateCondBr(keepGoing, bodyBlock, exitBlock);
		builder_.SetInsertPoint(bodyBlock);
		llvm::Value* source =
			builder_.CreateSub(cursor, indexValue(1), "shift.src");
		llvm::Value* sourceElement = builder_.CreateInBoundsGEP(
			elementType, data, source, "shift.src.addr");
		llvm::Value* destinationElement = builder_.CreateInBoundsGEP(
			elementType, data, cursor, "shift.dst.addr");
		llvm::Value* copy =
			builder_.CreateLoad(elementType, sourceElement, "shift.value");
		builder_.CreateStore(copy, destinationElement);
		llvm::Value* next =
			builder_.CreateSub(cursor, indexValue(1), "shift.next");
		cursor->addIncoming(next, builder_.GetInsertBlock());
		builder_.CreateBr(conditionBlock);
		builder_.SetInsertPoint(exitBlock);
	}

	llvm::Value* generateMethodCall(const ExprPtr& call) {
		StoredVariable& variable  = lookupName(call->receiver, call->location);
		const std::string& method = call->text;
		if (method == "size") {
			if (variable.type == Type::Array) {
				llvm::Type* allocated =
					llvm::cast<llvm::AllocaInst>(variable.slot)
						->getAllocatedType();
				return indexValue(
					llvm::cast<llvm::ArrayType>(allocated)->getNumElements());
			}
			if (variable.type == Type::Vector)
				return vectorLength(variable.slot);
			return mapCount(variable.slot);
		}
		if (method == "at") {
			const ExprPtr& argument = call->children[0];
			if (variable.type == Type::Map) {
				Type valueType =
					collectionElementType(variable, call->location);
				return mapLookupValue(variable.slot,
									  generateExpression(argument), valueType);
			}
			Type elementType = collectionElementType(variable, call->location);
			llvm::Value* index = generateExpression(argument);
			if (variable.type == Type::Vector) {
				emitRangeCheck(index, vectorLength(variable.slot));
			} else {
				llvm::Type* allocated =
					llvm::cast<llvm::AllocaInst>(variable.slot)
						->getAllocatedType();
				emitTrapIf(builder_.CreateOr(
					builder_.CreateICmpSGE(
						index,
						llvm::ConstantInt::get(
							llvm::Type::getInt32Ty(context_),
							llvm::cast<llvm::ArrayType>(allocated)
								->getNumElements()),
						"bounds.high"),
					builder_.CreateICmpSLT(index, indexValue(0),
										   "bounds.low")));
			}
			llvm::Value* address = collectionElementAddress(
				variable, index, llvmType(elementType));
			return builder_.CreateLoad(llvmType(elementType), address,
									   "at.value");
		}
		if (method == "insert" || method == "push") {
			if (variable.type == Type::Map) {
				Type valueType =
					collectionElementType(variable, call->location);
				llvm::Value* key   = generateExpression(call->children[0]);
				llvm::Value* value = generateExpression(call->children[1]);
				mapAssignValue(variable.slot, key, value, valueType);
				return nullptr;
			}
			Type elementType = collectionElementType(variable, call->location);
			llvm::Type* elementLLVM = llvmType(elementType);
			if (call->children.size() == 1) {
				llvm::Value* value = generateExpression(call->children[0]);
								reserveVector(variable.slot, elementLLVM);
				llvm::Value* slot	 = variable.slot;
				llvm::Value* data	 = vectorData(slot);
				llvm::Value* length	 = vectorLength(slot);
				llvm::Value* address = builder_.CreateInBoundsGEP(
					elementLLVM, data, length, "vec.push.addr");
				builder_.CreateStore(value, address);
				builder_.CreateStore(
					builder_.CreateAdd(length, indexValue(1), "vec.push.len"),
					builder_.CreateStructGEP(vectorStructType(), slot, 1));
				return nullptr;
			}
			llvm::Value* index = generateExpression(call->children[0]);
			llvm::Value* value = generateExpression(call->children[1]);
			// 			emitRangeCheck(
			// 				index, builder_.CreateAdd(vectorLength(variable.slot),
			// 										  indexValue(1), "vec.insert.limit"));
						reserveVector(variable.slot, elementLLVM);
			llvm::Value* slot	= variable.slot;
			llvm::Value* data	= vectorData(slot);
			llvm::Value* length = vectorLength(slot);
			emitShiftRight(data, elementLLVM, index, length);
			llvm::Value* address = builder_.CreateInBoundsGEP(
				elementLLVM, data, index, "vec.insert.addr");
			builder_.CreateStore(value, address);
			builder_.CreateStore(
				builder_.CreateAdd(length, indexValue(1), "vec.insert.len"),
				builder_.CreateStructGEP(vectorStructType(), slot, 1));
			return nullptr;
		}
		throw Diagnostic(call->location, "unknown method '" + method + "'");
	}

	llvm::Value* generateCall(const ExprPtr& expression) {
		if (!expression->receiver.empty()) {
			return generateMethodCall(expression);
		}
		if (expression->text == "assert") {
			llvm::Value* condition =
				toCondition(generateExpression(expression->children[0]));
			llvm::Function* function = builder_.GetInsertBlock()->getParent();
			llvm::BasicBlock* success =
				llvm::BasicBlock::Create(context_, "assert.ok", function);
			llvm::BasicBlock* failure =
				llvm::BasicBlock::Create(context_, "assert.fail", function);
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
				"printf", llvm::FunctionType::get(
							  llvm::Type::getInt32Ty(context_),
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
					llvm::Value* no	 = stringPointer("false", "false");
					format			 = "%s";
					arguments.push_back(
						builder_.CreateSelect(value, yes, no, "boolstr"));
				} else {
					format = "%d";
					arguments.push_back(value);
				}
				std::vector<llvm::Value*> callArguments{
					stringPointer(format, "fmt")};
				callArguments.insert(callArguments.end(), arguments.begin(),
									 arguments.end());
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
					name, llvm::FunctionType::get(result, std::move(parameters),
												  false));
			};
			llvm::Type* i32 = llvm::Type::getInt32Ty(context_);
			llvm::Type* i1	= llvm::Type::getInt1Ty(context_);
			if (expression->text == "__civic_string_length") {
				auto function =
					libc("strlen", llvm::Type::getInt64Ty(context_), {pointer});
				llvm::Value* length =
					builder_.CreateCall(function, arguments, "strlen");
				return builder_.CreateZExt(length, llvmType(Type::Isize), "string.length");
			}
			if (expression->text == "__civic_string_equals") {
				auto function = libc("strcmp", i32, {pointer, pointer});
				llvm::Value* result =
					builder_.CreateCall(function, arguments, "strcmp");
				return builder_.CreateICmpEQ(
					result, llvm::ConstantInt::get(i32, 0), "string.eq");
			}
			if (expression->text == "__civic_string_contains") {
				auto function = libc("strstr", pointer, {pointer, pointer});
				llvm::Value* result =
					builder_.CreateCall(function, arguments, "strstr");
				return builder_.CreateICmpNE(
					result,
					llvm::ConstantPointerNull::get(
						llvm::cast<llvm::PointerType>(pointer)),
					"string.contains");
			}
			auto strlenFunction =
				libc("strlen", llvm::Type::getInt64Ty(context_), {pointer});
			llvm::Value* valueLength = builder_.CreateCall(
				strlenFunction, {arguments[0]}, "value.len");
			llvm::Value* patternLength = builder_.CreateCall(
				strlenFunction, {arguments[1]}, "pattern.len");
			if (expression->text == "__civic_string_starts_with") {
				llvm::Value* lengthFits = builder_.CreateICmpULE(
					patternLength, valueLength, "prefix.fits");
				llvm::Value* compared = builder_.CreateCall(
					libc("strncmp", i32,
						 {pointer, pointer, llvm::Type::getInt64Ty(context_)}),
					{arguments[0], arguments[1], patternLength}, "strncmp");
				llvm::Value* equal = builder_.CreateICmpEQ(
					compared, llvm::ConstantInt::get(i32, 0), "prefix.matches");
				return builder_.CreateAnd(lengthFits, equal, "starts.with");
			}

			llvm::Function* parent = builder_.GetInsertBlock()->getParent();
			llvm::BasicBlock* compareBlock =
				llvm::BasicBlock::Create(context_, "suffix.compare", parent);
			llvm::BasicBlock* missBlock =
				llvm::BasicBlock::Create(context_, "suffix.miss", parent);
			llvm::BasicBlock* mergeBlock =
				llvm::BasicBlock::Create(context_, "suffix.end", parent);
			llvm::Value* lengthFits = builder_.CreateICmpULE(
				patternLength, valueLength, "suffix.fits");
			builder_.CreateCondBr(lengthFits, compareBlock, missBlock);
			builder_.SetInsertPoint(compareBlock);
			llvm::Value* offset =
				builder_.CreateSub(valueLength, patternLength, "suffix.offset");
			llvm::Value* suffixStart =
				builder_.CreateGEP(llvm::Type::getInt8Ty(context_),
								   arguments[0], offset, "suffix.start");
			llvm::Value* compared = builder_.CreateCall(
				libc("strcmp", i32, {pointer, pointer}),
				{suffixStart, arguments[1]}, "suffix.compare");
			llvm::Value* matches = builder_.CreateICmpEQ(
				compared, llvm::ConstantInt::get(i32, 0), "suffix.matches");
			llvm::BasicBlock* compareEnd = builder_.GetInsertBlock();
			builder_.CreateBr(mergeBlock);
			builder_.SetInsertPoint(missBlock);
			builder_.CreateBr(mergeBlock);
			builder_.SetInsertPoint(mergeBlock);
			llvm::PHINode* result = builder_.CreatePHI(i1, 2, "ends.with");
			result->addIncoming(matches, compareEnd);
			result->addIncoming(llvm::ConstantInt::getFalse(context_),
								missBlock);
			return result;
		}
		std::vector<llvm::Value*> arguments;
		for (const ExprPtr& argument : expression->children) {
			arguments.push_back(generateExpression(argument));
		}
		llvm::Function* function = module_.getFunction(expression->text);
		return builder_.CreateCall(
			function, arguments,
			function->getReturnType()->isVoidTy() ? "" : "call");
	}

	const Program& program_;
	llvm::LLVMContext context_;
	llvm::Module module_;
	llvm::IRBuilder<> builder_;
	std::vector<std::unordered_map<std::string, StoredVariable>> scopes_;
};
} // namespace

std::string generateLLVMIR(const Program& program,
						   const std::string& moduleName) {
	return Generator(program, moduleName).run();
}
} // namespace civic
