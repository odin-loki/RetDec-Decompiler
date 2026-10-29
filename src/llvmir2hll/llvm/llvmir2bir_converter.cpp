/**
* @file src/llvmir2hll/llvm/llvmir2bir_converter.cpp
* @brief Implementation of LLVMIR2BIRConverter.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <memory>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Local.h>

#include "retdec/llvmir2hll/config/config.h"
#include "retdec/llvmir2hll/ir/expression.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/llvm/llvmir2bir_converter.h"
#include "retdec/llvmir2hll/llvm/llvmir2bir_converter/llvm_value_converter.h"
#include "retdec/llvmir2hll/llvm/llvmir2bir_converter/structure_converter.h"
#include "retdec/llvmir2hll/llvm/llvmir2bir_converter/variables_manager.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/llvmir2hll/utils/ir.h"
#include "retdec/llvmir2hll/utils/string.h"
#include "retdec/utils/io/log.h"

using namespace retdec::utils::io;

namespace retdec {
namespace llvmir2hll {

/**
* @brief Constructs a new converter.
*
* For more details, see create().
*/
LLVMIR2BIRConverter::LLVMIR2BIRConverter(llvm::Pass *basePass):
	basePass(basePass), optionStrictFPUSemantics(false),
	enableDebug(false), converter(),
	llvmModule(nullptr), resModule(), structConverter(), variablesManager() {}

/**
* @brief Creates a new converter.
*
* @param[in] basePass Pass that instantiates a concrete converter.
*
* @par Preconditions
*  - @a basePass is non-null
*/
ShPtr<LLVMIR2BIRConverter> LLVMIR2BIRConverter::create(llvm::Pass *basePass) {
	return ShPtr<LLVMIR2BIRConverter>(new LLVMIR2BIRConverter(basePass));
}

/**
* @brief Enables/disables the use of strict FPU semantics.
*
* @param[in] strict If @c true, enables the use of strict FPU semantics. If @c
*                   false, disables the use of strict FPU semantics.
*/
void LLVMIR2BIRConverter::setOptionStrictFPUSemantics(bool strict) {
	optionStrictFPUSemantics = strict;
}

/**
* @brief Converts the given LLVM module into a module in BIR.
*
* @param[in] llvmModule LLVM module to be converted.
* @param[in] moduleName Identifier of the resulting module.
* @param[in] semantics The used semantics.
* @param[in] config Configuration for the module.
* @param[in] enableDebug If @c true, debugging messages will be emitted.
*
* @par Preconditions
*  - both @a llvmModule and @a semantics are non-null
*/

namespace {

/**
 * Returns lane @a idx of the integer vector @a vec as a scalar built from the
 * scalar the vector came from, or nullptr when the lane cannot be traced.
 */
llvm::Value *integerVectorLane(llvm::Value *vec, unsigned idx,
		llvm::IRBuilder<> &irb, unsigned depth = 0) {
	auto *vt = llvm::dyn_cast<llvm::FixedVectorType>(vec->getType());
	if (!vt || depth > 8 || idx >= vt->getNumElements()) {
		return nullptr;
	}
	auto *et = vt->getElementType();
	// SSE scalar code keeps doubles/floats in i128 registers and updates
	// lane 0 through insertelement; lanes of FP vectors are traced the same
	// way and reinterpreted as the element type.
	if (!et->isIntegerTy() && !et->isFloatingPointTy()) {
		return nullptr;
	}
	unsigned eb = et->getPrimitiveSizeInBits().getFixedValue();
	auto *it = llvm::IntegerType::get(vec->getContext(), eb);
	if (auto *c = llvm::dyn_cast<llvm::Constant>(vec)) {
		return c->getAggregateElement(idx);
	}
	if (auto *bc = llvm::dyn_cast<llvm::BitCastInst>(vec)) {
		llvm::Value *x = bc->getOperand(0);
		if (!x->getType()->isIntegerTy()) {
			return nullptr;
		}
		unsigned lo = idx * eb;
		if (auto *z = llvm::dyn_cast<llvm::ZExtInst>(x)) {
			unsigned w = z->getOperand(0)->getType()->getIntegerBitWidth();
			if (lo >= w) {
				return llvm::Constant::getNullValue(et);
			}
			if (lo + eb <= w) {
				x = z->getOperand(0);
			}
		}
		if (lo) {
			x = irb.CreateLShr(x, lo);
		}
		x = irb.CreateTrunc(x, it);
		return et->isIntegerTy() ? x : irb.CreateBitCast(x, et);
	}
	if (auto *sv = llvm::dyn_cast<llvm::ShuffleVectorInst>(vec)) {
		int m = sv->getMaskValue(idx);
		if (m < 0) {
			return llvm::UndefValue::get(et);
		}
		unsigned n = llvm::cast<llvm::FixedVectorType>(
			sv->getOperand(0)->getType())->getNumElements();
		return unsigned(m) < n
			? integerVectorLane(sv->getOperand(0), m, irb, depth + 1)
			: integerVectorLane(sv->getOperand(1), m - n, irb, depth + 1);
	}
	if (auto *ie = llvm::dyn_cast<llvm::InsertElementInst>(vec)) {
		auto *ci = llvm::dyn_cast<llvm::ConstantInt>(ie->getOperand(2));
		if (!ci) {
			return nullptr;
		}
		return ci->getZExtValue() == idx ? ie->getOperand(1)
			: integerVectorLane(ie->getOperand(0), idx, irb, depth + 1);
	}
	return nullptr;
}

/**
 * Splits a stack slot of a fixed vector type that is only loaded and stored
 * whole (reg2mem's copy of an SSE register kept across a loop) into one slot
 * per lane. A store writes each lane it can trace; a load rebuilds the
 * vector from the lane slots, so a later lane read sees the scalar stored
 * by the previous iteration instead of the stale base vector the BIR's
 * insertelement approximation would keep.
 */
void scalarizeVectorSlots(llvm::Function &f) {
	if (f.isDeclaration()) {
		return;
	}
	std::vector<llvm::AllocaInst *> slots;
	for (auto &i : f.getEntryBlock()) {
		auto *a = llvm::dyn_cast<llvm::AllocaInst>(&i);
		if (!a || a->isArrayAllocation()) {
			continue;
		}
		auto *vt = llvm::dyn_cast<llvm::FixedVectorType>(a->getAllocatedType());
		if (!vt || vt->getNumElements() > 16
				|| !(vt->getElementType()->isIntegerTy()
					|| vt->getElementType()->isFloatingPointTy())) {
			continue;
		}
		bool simple = true;
		for (auto *u : a->users()) {
			if (auto *l = llvm::dyn_cast<llvm::LoadInst>(u)) {
				simple &= l->getType() == vt && !l->isVolatile();
			} else if (auto *st = llvm::dyn_cast<llvm::StoreInst>(u)) {
				simple &= st->getPointerOperand() == a
					&& st->getValueOperand()->getType() == vt
					&& !st->isVolatile();
			} else {
				simple = false;
			}
		}
		if (simple) {
			slots.push_back(a);
		}
	}
	for (auto *a : slots) {
		auto *vt = llvm::cast<llvm::FixedVectorType>(a->getAllocatedType());
		auto *et = vt->getElementType();
		unsigned n = vt->getNumElements();
		std::vector<llvm::AllocaInst *> lanes;
		llvm::IRBuilder<> eb(a);
		for (unsigned l = 0; l < n; ++l) {
			lanes.push_back(eb.CreateAlloca(et, nullptr,
				a->getName() + ".lane" + llvm::Twine(l)));
		}
		std::vector<llvm::Instruction *> users;
		for (auto *u : a->users()) {
			users.push_back(llvm::cast<llvm::Instruction>(u));
		}
		for (auto *u : users) {
			llvm::IRBuilder<> irb(u);
			if (auto *st = llvm::dyn_cast<llvm::StoreInst>(u)) {
				auto *v = st->getValueOperand();
				for (unsigned l = 0; l < n; ++l) {
					llvm::Value *x = integerVectorLane(v, l, irb);
					if (!x || x->getType() != et) {
						x = irb.CreateExtractElement(v, irb.getInt64(l));
					}
					irb.CreateStore(x, lanes[l]);
				}
			} else {
				llvm::Value *v = llvm::PoisonValue::get(vt);
				for (unsigned l = 0; l < n; ++l) {
					v = irb.CreateInsertElement(v,
						irb.CreateLoad(et, lanes[l]), irb.getInt64(l));
				}
				u->replaceAllUsesWith(v);
			}
		}
		for (auto *u : users) {
			u->eraseFromParent();
		}
		a->eraseFromParent();
	}
}

} // anonymous namespace

/**
 * Rewrites lane reads of integer vectors (extractelement, and bitcasts of a
 * vector back to an integer) into shifts and truncations of the scalars the
 * lanes came from. The BIR has no vector types, and its converter would
 * otherwise read every lane as lane 0 and every shuffle as its first operand,
 * so a paired 32-bit load compared lane 0 with itself and lost the swap.
 */
void LLVMIR2BIRConverter::lowerIntegerVectorLanes(llvm::Module &m) {
	for (auto &f : m) {
		scalarizeVectorSlots(f);
		std::vector<llvm::Instruction *> work;
		for (auto &bb : f) {
			for (auto &i : bb) {
				if (llvm::isa<llvm::ExtractElementInst>(i)
						|| (llvm::isa<llvm::BitCastInst>(i)
							&& i.getType()->isIntegerTy()
							&& i.getOperand(0)->getType()->isVectorTy())) {
					work.push_back(&i);
				}
			}
		}
		for (auto *i : work) {
			llvm::IRBuilder<> irb(i);
			llvm::Value *r = nullptr;
			if (auto *ee = llvm::dyn_cast<llvm::ExtractElementInst>(i)) {
				auto *ci = llvm::dyn_cast<llvm::ConstantInt>(ee->getIndexOperand());
				if (ci) {
					r = integerVectorLane(ee->getVectorOperand(), ci->getZExtValue(), irb);
				}
			} else {
				auto *vt = llvm::dyn_cast<llvm::FixedVectorType>(i->getOperand(0)->getType());
				auto *vet = vt ? vt->getElementType() : nullptr;
				if (vt && (vet->isIntegerTy() || vet->isFloatingPointTy())
						&& !llvm::isa<llvm::BitCastInst>(i->getOperand(0))) {
					unsigned eb = vet->getPrimitiveSizeInBits().getFixedValue();
					auto *it = i->getType();
					for (unsigned l = 0; l < vt->getNumElements(); ++l) {
						auto *lane = integerVectorLane(i->getOperand(0), l, irb);
						if (!lane) {
							r = nullptr;
							break;
						}
						if (lane->getType()->isFloatingPointTy()) {
							lane = irb.CreateBitCast(lane,
								llvm::IntegerType::get(i->getContext(), eb));
						}
						llvm::Value *part = irb.CreateZExt(lane, it);
						if (l) {
							part = irb.CreateShl(part, l * eb);
						}
						r = r ? irb.CreateOr(r, part) : part;
					}
				}
			}
			if (r && r->getType() == i->getType()) {
				i->replaceAllUsesWith(r);
				llvm::RecursivelyDeleteTriviallyDeadInstructions(i);
			}
		}
	}
}

ShPtr<Module> LLVMIR2BIRConverter::convert(llvm::Module *llvmModule,
		const std::string &moduleName, ShPtr<Semantics> semantics,
		ShPtr<Config> config, bool enableDebug) {
	PRECONDITION_NON_NULL(llvmModule);
	PRECONDITION_NON_NULL(semantics);

	this->llvmModule = llvmModule;
	lowerIntegerVectorLanes(*llvmModule);
	this->enableDebug = enableDebug;
	resModule = std::make_shared<Module>(llvmModule, moduleName, semantics,
		config);
	variablesManager = std::make_shared<VariablesManager>(resModule);
	converter = LLVMValueConverter::create(resModule, variablesManager);
	structConverter = std::make_unique<StructureConverter>(basePass, converter, resModule);

	converter->setOptionStrictFPUSemantics(optionStrictFPUSemantics);

	convertAndAddFuncsDeclarations();
	convertAndAddGlobalVariables();
	convertFuncsBodies();
	makeIdentifiersValid();

	return resModule;
}

/**
* @brief Determines if given LLVM global variable @a var is external.
*/
bool LLVMIR2BIRConverter::isExternal(const llvm::GlobalVariable &var) const {
	// Only local linkage global variables (which are private and internal)
	// are internal. Others are external.
	return !var.hasLocalLinkage();
}

/**
* @brief Determines whether the given LLVM global variable @a globVar should be
*        converted and added into the resulting module.
*/
bool LLVMIR2BIRConverter::shouldBeConvertedAndAdded(
		const llvm::GlobalVariable &globVar) const {
	return !converter->storesStringLiteral(globVar);
}

/**
* @brief Converts the given LLVM global variable @a globVar into a variable in BIR.
*/
ShPtr<Variable> LLVMIR2BIRConverter::convertGlobalVariable(
		llvm::GlobalVariable &globVar) const {
	auto var = converter->convertValueToVariable(&globVar);
	if (isExternal(globVar)) {
		var->markAsExternal();
	}

	auto a = resModule->getConfig()->getAddressForGlobalVar(globVar.getName().str());
	var->setAddress(a);

	return var;
}

/**
* @brief Converts initializer of the given LLVM global variable @a globVar into
*        an expression in BIR.
*/
ShPtr<Expression> LLVMIR2BIRConverter::convertGlobalVariableInitializer(
		llvm::GlobalVariable &globVar) const {
	if (globVar.hasInitializer()) {
		return converter->convertConstantToExpression(
			globVar.getInitializer());
	}

	return nullptr;
}

/**
* @brief Converts all global variables of the input LLVM module and stores them
*        into the resulting module.
*/
void LLVMIR2BIRConverter::convertAndAddGlobalVariables() {
	if (enableDebug) {
		Log::phase("converting global variables", Log::SubPhase);
	}

	for (auto &globVar: llvmModule->globals()) {
		if (shouldBeConvertedAndAdded(globVar)) {
			auto variable = convertGlobalVariable(globVar);
			auto initializer = convertGlobalVariableInitializer(globVar);
			resModule->addGlobalVar(variable, initializer);
			variablesManager->addGlobalValVarPair(&globVar, variable);
		}
	}
}

/**
* @brief Converts parameters of the given LLVM function @a func into a list of
*        function parameters in BIR.
*/
VarVector LLVMIR2BIRConverter::convertFuncParams(llvm::Function &func) {
	VarVector params;
	for (auto &arg: func.args()) {
		params.push_back(converter->convertValueToVariable(&arg));
	}

	return params;
}

/**
* @brief Converts a declaration of the given LLVM function @a func into
*        a function declaration in BIR.
*/
ShPtr<Function> LLVMIR2BIRConverter::convertFuncDeclaration(
		llvm::Function &func) {
	// Clear local variables before conversion.
	variablesManager->reset();

	auto retType = converter->convertType(func.getReturnType());
	auto params = convertFuncParams(func);

	auto birFunc = Function::create(resModule, retType, func.getName().str(), params);
	variablesManager->addGlobalValVarPair(&func, birFunc->getAsVar());
	birFunc->setVarArg(func.isVarArg());
	return birFunc;
}

/**
* @brief Updates the given LLVM function @a func from declaration to definition.
*/
void LLVMIR2BIRConverter::updateFuncToDefinition(llvm::Function &func) {
	auto name = func.getName().str();
	if (enableDebug) {
		Log::phase("converting function " + name, Log::SubPhase);
	}

	auto birFunc = resModule->getFuncByName(name);
	if (birFunc) {
		// Clear local variables before conversion.
		variablesManager->reset();

		birFunc->setParams(convertFuncParams(func));
		birFunc->setBody(structConverter->convertFuncBody(func));
		birFunc->setLocalVars(variablesManager->getLocalVars());

		generateVarDefinitions(birFunc);
	}
}

/**
* @brief Sorts local variables set @a vars alphabetically by name.
*/
VarVector LLVMIR2BIRConverter::sortLocalVars(const VarSet &vars) const {
	VarVector varVector(vars.begin(), vars.end());
	sortByName(varVector);
	return varVector;
}

/**
* @brief Generates variable definition statements at the beginning of @a func.
*/
void LLVMIR2BIRConverter::generateVarDefinitions(ShPtr<Function> func) const {
	auto vars = sortLocalVars(func->getLocalVars());
	for (auto i = vars.crbegin(), e = vars.crend(); i != e; ++i) {
		Address a = (*i)->getAddress();
		if (a.isUndefined()) a = func->getStartAddress();
		func->getBody()->prependStatement(
			VarDefStmt::create(*i, nullptr, nullptr, a));
	}
}

/**
* @brief Determines whether the given LLVM function @a func should be converted
*        and added into the resulting module.
*/
bool LLVMIR2BIRConverter::shouldBeConvertedAndAdded(
		const llvm::Function &func) const {
	// Do not convert 'available_externally' functions, because they have
	// definitions outside module.
	return !func.hasAvailableExternallyLinkage();
}

/**
* @brief Converts all functions declarations of the input LLVM module and stores
*        them into the resulting module.
*/
void LLVMIR2BIRConverter::convertAndAddFuncsDeclarations() {
	for (auto &func: llvmModule->functions()) {
		if (shouldBeConvertedAndAdded(func)) {
			resModule->addFunc(convertFuncDeclaration(func));
		}
	}
}

/**
* @brief Goes through all functions definitions of the input LLVM module and
*        converts their bodies and stores them into the resulting module.
*/
void LLVMIR2BIRConverter::convertFuncsBodies() {
	for (auto &func: llvmModule->functions()) {
		if (!func.isDeclaration() && shouldBeConvertedAndAdded(func)) {
			updateFuncToDefinition(func);
		}
	}
}

/**
* @brief Makes all identifiers valid by replacing invalid characters with valid
*        characters.
*/
void LLVMIR2BIRConverter::makeIdentifiersValid() {
	makeGlobVarsIdentifiersValid();
	makeFuncsIdentifiersValid();
}

/**
* @brief Makes all identifiers of the global variables valid.
*/
void LLVMIR2BIRConverter::makeGlobVarsIdentifiersValid() {
	for (auto &globVar: resModule->getGlobalVars()) {
		globVar->setName(makeIdentifierValid(globVar->getName()));
	}
}

/**
* @brief Makes all identifiers of the functions valid (function names, parameters
*        and local variables).
*/
void LLVMIR2BIRConverter::makeFuncsIdentifiersValid() {
	for (auto i = resModule->func_begin(), e = resModule->func_end(); i != e; ++i) {
		makeFuncIdentifiersValid(*i);
	}
}

/**
* @brief Makes all identifiers of the given function @a func valid (function
*        name, parameters and local variables).
*/
void LLVMIR2BIRConverter::makeFuncIdentifiersValid(ShPtr<Function> func) const {
	func->setName(makeIdentifierValid(func->getName()));
	makeFuncVariablesValid(func);
}

/**
* @brief Makes all identifiers of the local variables and parameters in the
*        given function @a func valid.
*/
void LLVMIR2BIRConverter::makeFuncVariablesValid(ShPtr<Function> func) const {
	for (auto &var: func->getLocalVars(true)) {
		var->setName(makeIdentifierValid(var->getName()));
	}
}

} // namespace llvmir2hll
} // namespace retdec
