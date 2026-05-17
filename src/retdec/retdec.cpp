/**
 * @file src/retdec/retdec.cpp
 * @brief RetDec library.
 * @copyright (c) 2019 Odin Loch Trading as Imortek
 */

#include <memory>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include <llvm/ADT/Triple.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/Analysis/RegionPass.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/CodeGen/CommandFlags.inc>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LegacyPassNameParser.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/LinkAllIR.h>
#include <llvm/MC/SubtargetFeature.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/SystemUtils.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "retdec/bin2llvmir/optimizations/decoder/decoder.h"
#include "retdec/bin2llvmir/optimizations/provider_init/provider_init.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/providers/config.h"

#include "retdec/llvmir2hll/llvmir2hll.h"

#include "retdec/config/config.h"
#include "retdec/llvmir-emul/llvmir_emul.h"
#include "retdec/utils/thread_pool.h"
#include "retdec/utils/gpu_scanner.h"
#include "retdec/retdec/retdec.h"
#include "retdec/utils/conversion.h"
#include "retdec/utils/memory.h"
#include "retdec/utils/io/log.h"

// ── Post-decompile analysis passes ───────────────────────────────────────────
#include "retdec/sort_detect/sort_detect.h"
#include "retdec/concurrency_detect/concurrency_detect.h"
#include "retdec/container_detect/container_detect.h"
#include "retdec/algo_recover/algo_recover.h"
#include "retdec/type_inference/type_inference.h"
#include "retdec/serial_detect/serial_detect.h"
#include "retdec/ipa/ipa.h"
#include "retdec/call_conv/call_conv.h"
#include "retdec/ptx_decompile/cuda_host_recover.h"

#include "llvm_to_ssa.h"
#include "retdec/ssa/ssa.h"

using namespace retdec::utils::io;

// extern llvm::cl::opt<bool> PrintAfterAll;

//==============================================================================
// disassembler
//==============================================================================

/**
 * Create an empty input module.
 */
std::unique_ptr<llvm::Module> createLlvmModule(llvm::LLVMContext& Context)
{
	llvm::SMDiagnostic Err;

	std::string c = "; ModuleID = 'test'\nsource_filename = \"test\"\n";
	auto mb = llvm::MemoryBuffer::getMemBuffer(c);
	if (mb == nullptr)
	{
		throw std::runtime_error("failed to create llvm::MemoryBuffer");
	}
	std::unique_ptr<Module> M = parseIR(mb->getMemBufferRef(), Err, Context);
	if (M == nullptr)
	{
		throw std::runtime_error("failed to create llvm::Module");
	}

	// Immediately run the verifier to catch any problems before starting up the
	// pass pipelines. Otherwise we can crash on broken code during
	// doInitialization().
	if (verifyModule(*M, &errs()))
	{
		throw std::runtime_error("created llvm::Module is broken");
	}

	return M;
}

namespace retdec {

namespace
{

bool bin2llvmirPassDiagEnabled()
{
	const char *e = std::getenv("RETDEC_BIN2LLVMIR_DIAG");
	return e != nullptr && e[0] != '\0' && e[0] != '0';
}

} // namespace

/**
 * This pass just prints phase information about other, subsequent passes.
 * In pass manager, it should be placed right before the pass which phase info
 * it is printing.
 */
class ModulePassPrinter : public llvm::ModulePass
{
	public:
		static char ID;
		std::string PhaseName;
		std::string PhaseArg;
		std::string PassName;

		static std::string LastPhase;
		inline static const std::string LlvmAggregatePhaseName = "LLVM";
		/// Wall-clock start for the next real pass (set by printer; read by @c ModulePassTimerAfter).
		static std::chrono::steady_clock::time_point passWallStartForTimedPass;

	public:
		ModulePassPrinter(
				const std::string& phaseName,
				const std::string& phaseArg)
				: llvm::ModulePass(ID)
				, PhaseName(phaseName)
				, PhaseArg(phaseArg)
				, PassName("ModulePass Printer: " + PhaseName)
		{

		}

		bool runOnModule(llvm::Module &M) override
		{
			if (utils::startsWith(PhaseArg, "retdec"))
			{
				Log::phase(PhaseName);
				LastPhase = PhaseArg;
			}
			else
			{
				// aggregate LLVM
				if (LastPhase != LlvmAggregatePhaseName)
				{
					Log::phase(LlvmAggregatePhaseName);
					LastPhase = LlvmAggregatePhaseName;
				}

				// print all
				// Log::phase(PhaseName);
				// LastPhase = PhaseArg;
			}
			if (bin2llvmirPassDiagEnabled() && utils::startsWith(PhaseArg, "retdec"))
			{
				passWallStartForTimedPass = std::chrono::steady_clock::now();
			}
			return false;
		}

		llvm::StringRef getPassName() const override
		{
			return PassName.c_str();
		}

		void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
		{
			AU.setPreservesAll();
		}
};
char ModulePassPrinter::ID = 0;
std::string ModulePassPrinter::LastPhase;
std::chrono::steady_clock::time_point ModulePassPrinter::passWallStartForTimedPass{};

/**
 * Runs immediately after each real module pass; logs wall ms for @c retdec-* passes only.
 */
class ModulePassTimerAfter : public llvm::ModulePass
{
	public:
		static char ID;

		explicit ModulePassTimerAfter(std::string passArg)
				: llvm::ModulePass(ID), _passArg(std::move(passArg))
		{
		}

		bool runOnModule(llvm::Module &) override
		{
			if (!bin2llvmirPassDiagEnabled()
					|| !utils::startsWith(_passArg, "retdec"))
			{
				return false;
			}
			const auto now = std::chrono::steady_clock::now();
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					now - ModulePassPrinter::passWallStartForTimedPass).count();
			Log::info() << "[bin2llvmir-diag] pass_ms " << _passArg << "=" << ms
					<< std::endl;
			return false;
		}

		llvm::StringRef getPassName() const override
		{
			return "retdec-module-pass-timer-after";
		}

		void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
		{
			AU.setPreservesAll();
		}

	private:
		std::string _passArg;
};
char ModulePassTimerAfter::ID = 0;

common::BasicBlock fillBasicBlock(
		bin2llvmir::Config* config,
		llvm::BasicBlock& bb,
		llvm::BasicBlock& bbEnd)
{
	common::BasicBlock ret;

	ret.setStartEnd(
		bin2llvmir::AsmInstruction::getTrueBasicBlockAddress(&bb),
		bin2llvmir::AsmInstruction::getBasicBlockEndAddress(&bbEnd)
	);

	for (auto pit = pred_begin(&bb), e = pred_end(&bb); pit != e; ++pit)
	{
		// Find BB with address - there should always be some.
		// Some BBs may not have addresses - e.g. those inside
		// if-then-else instruction models.
		auto* pred = *pit;
		auto start = bin2llvmir::AsmInstruction::getTrueBasicBlockAddress(pred);
		while (start.isUndefined())
		{
			pred = pred->getPrevNode();
			assert(pred);
			start = bin2llvmir::AsmInstruction::getTrueBasicBlockAddress(pred);
		}
		ret.preds.insert(start);
	}

	for (auto sit = succ_begin(&bbEnd), e = succ_end(&bbEnd); sit != e; ++sit)
	{
		// Find BB with address - there should always be some.
		// Some BBs may not have addresses - e.g. those inside
		// if-then-else instruction models.
		auto* succ = *sit;
		auto start = bin2llvmir::AsmInstruction::getTrueBasicBlockAddress(succ);
		while (start.isUndefined())
		{
			succ = succ->getPrevNode();
			assert(succ);
			start = bin2llvmir::AsmInstruction::getTrueBasicBlockAddress(succ);
		}
		ret.succs.insert(start);
	}
	// MIPS likely delays slot hack - recognize generated pattern and
	// find all sucessors.
	// Also applicable to ARM cond call/return patterns, and other cases.
	if (bin2llvmir::AsmInstruction::getTrueBasicBlockAddress(&bbEnd).isUndefined() // no addr
			&& (++pred_begin(&bbEnd)) == pred_end(&bbEnd) // single pred
			&& bbEnd.getPrevNode() == *pred_begin(&bbEnd)) // pred right before
	{
		auto* br = llvm::dyn_cast<llvm::BranchInst>(
				(*pred_begin(&bbEnd))->getTerminator());
		if (br
				&& br->isConditional()
				&& br->getSuccessor(0) == &bbEnd
				&& bin2llvmir::AsmInstruction::getTrueBasicBlockAddress(
						br->getSuccessor(1)))
		{
			ret.succs.insert(
					bin2llvmir::AsmInstruction::getTrueBasicBlockAddress(
							br->getSuccessor(1)));
		}
	}

	auto* nextBb = bbEnd.getNextNode(); // may be nullptr
	for (auto ai = bin2llvmir::AsmInstruction(&bb);
			ai.isValid() && ai.getBasicBlock() != nextBb;
			ai = ai.getNext())
	{
		ret.instructions.push_back(ai.getCapstoneInsn());

		for (auto& i : ai)
		{
			auto call = llvm::dyn_cast<llvm::CallInst>(&i);
			if (call && call->getCalledFunction())
			{
				auto cf = call->getCalledFunction();
				auto target = bin2llvmir::AsmInstruction::getFunctionAddress(cf);
				if (target.isUndefined())
				{
					target = config->getFunctionAddress(cf);
				}
				if (target.isDefined())
				{
					auto src = ai.getAddress();
					// MIPS hack: there are delay slots on MIPS, calls/branches
					// are placed at the end of the next instruction (delay slot)
					// we need to modify reference address.
					// This assums that all references on MIPS have delays slots of
					// 4 bytes, and therefore need to be fixed, it it is not the
					// case, it will cause problems.
					//
					if (config->getConfig().architecture.isMipsOrPic32())
					{
						src -= 4;
					}

					ret.calls.emplace(
							common::BasicBlock::CallEntry{src, target});
				}
			}
		}
	}

	return ret;
}

common::Function fillFunction(
		bin2llvmir::Config* config,
		llvm::Function& f)
{
	common::Function ret(
			bin2llvmir::AsmInstruction::getFunctionAddress(&f),
			bin2llvmir::AsmInstruction::getFunctionEndAddress(&f),
			f.getName()
	);

	for (llvm::BasicBlock& bb : f)
	{
		// There are more BBs in LLVM IR than we created in control-flow
		// decoding - e.g. BBs inside instructions that behave like
		// if-then-else created by capstone2llvmir.
		if (bin2llvmir::AsmInstruction::getTrueBasicBlockAddress(&bb).isUndefined())
		{
			continue;
		}

		llvm::BasicBlock* bbEnd = &bb;
		while (bbEnd->getNextNode())
		{
			// Next has address -- is a proper BB.
			//
			if (bin2llvmir::AsmInstruction::getTrueBasicBlockAddress(
					bbEnd->getNextNode()).isDefined())
			{
				break;
			}
			else
			{
				bbEnd = bbEnd->getNextNode();
			}
		}

		ret.basicBlocks.emplace(
				fillBasicBlock(config, bb, *bbEnd));
	}

	for (auto* u : f.users())
	{
		if (auto* i = llvm::dyn_cast<llvm::Instruction>(u))
		{
			if (auto ai = bin2llvmir::AsmInstruction(i))
			{
				auto addr = ai.getAddress();
				// MIPS hack: there are delay slots on MIPS, calls/branches
				// are placed at the end of the next instruction (delay slot)
				// we need to modify reference address.
				// This assums that all references on MIPS have delays slots of
				// 4 bytes, and therefore need to be fixed, it it is not the
				// case, it will cause problems.
				//
				if (config->getConfig().architecture.isMipsOrPic32())
				{
					addr -= 4;
				}
				ret.codeReferences.insert(addr);
			}
		}
	}

	return ret;
}

void fillFunctions(
		llvm::Module& module,
		retdec::common::FunctionSet* fs)
{
	if (fs == nullptr)
	{
		return;
	}

	auto* config = bin2llvmir::ConfigProvider::getConfig(&module);
	if (config == nullptr)
	{
		return;
	}

	for (llvm::Function& f : module.functions())
	{
		if (f.isDeclaration()
			|| f.empty()
			|| bin2llvmir::AsmInstruction::getFunctionAddress(&f).isUndefined())
		{
			auto sa = config->getFunctionAddress(&f);
			if (sa.isDefined())
			{
				fs->emplace(common::Function(sa, sa, f.getName()));
			}
			continue;
		}

		fs->emplace(fillFunction(config, f));
	}
}

LlvmModuleContextPair disassemble(
		const std::string& inputPath,
		retdec::common::FunctionSet* fs)
{
	auto context = std::make_unique<llvm::LLVMContext>();
	auto module = createLlvmModule(*context);

	config::Config c;
	c.parameters.setInputFile(inputPath);

	// Create a PassManager to hold and optimize the collection of passes we
	// are about to build.
	llvm::legacy::PassManager pm;

	if (bin2llvmirPassDiagEnabled())
	{
		pm.add(new ModulePassPrinter(
				"Providers initialization",
				"retdec-provider-init"));
		pm.add(new bin2llvmir::ProviderInitialization(&c));
		pm.add(new ModulePassTimerAfter("retdec-provider-init"));
		pm.add(new ModulePassPrinter(
				"Input binary to LLVM IR decoding",
				"retdec-decoder"));
		pm.add(new bin2llvmir::Decoder());
		pm.add(new ModulePassTimerAfter("retdec-decoder"));
	}
	else
	{
		pm.add(new bin2llvmir::ProviderInitialization(&c));
		pm.add(new bin2llvmir::Decoder());
	}

	const auto disasmT0 = std::chrono::steady_clock::now();
	pm.run(*module);
	if (bin2llvmirPassDiagEnabled())
	{
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - disasmT0).count();
		Log::info() << "[bin2llvmir-diag] disassemble_pipeline_wall_ms=" << ms
				<< std::endl;
	}

	fillFunctions(*module, fs);

	return LlvmModuleContextPair{std::move(module), std::move(context)};
}

//==============================================================================
// decompiler
//==============================================================================

/**
 * Call a bunch of LLVM initialization functions, same as the original opt.
 */
llvm::PassRegistry& initializeLlvmPasses()
{
	// Initialize passes
	llvm::PassRegistry& Registry = *llvm::PassRegistry::getPassRegistry();
	initializeCore(Registry);
	initializeScalarOpts(Registry);
	initializeIPO(Registry);
	initializeAnalysis(Registry);
	initializeTransformUtils(Registry);
	initializeInstCombine(Registry);
	initializeTarget(Registry);
	return Registry;
}

/**
 * Add the pass to the pass manager - no verification.
 */
static inline void addPass(
		legacy::PassManagerBase& PM,
		Pass* P,
		const PassInfo* PI)
{
	PM.add(new ModulePassPrinter(
			PI->getPassName().str(),
			PI->getPassArgument().str()
	));
	PM.add(P);
	PM.add(new ModulePassTimerAfter(PI->getPassArgument().str()));

// if (!PI->isAnalysis())
// PM.add(P->createPrinterPass(
// 		dbgs(),
// 		("*** IR Dump After " + P->getPassName() + " ***").str()
// ));

}


void setLogsFrom(const retdec::config::Parameters& params)
{
	auto logFile = params.getLogFile();
	auto errFile = params.getErrFile();
	auto verbose = params.isVerboseOutput();

	Logger::Ptr outLog = nullptr;

	outLog.reset(
		logFile.empty()
			? new Logger(std::cout, verbose)
			: new FileLogger(logFile, verbose)
	);

	Log::set(Log::Type::Info, std::move(outLog));

	if (!errFile.empty()) {
		Log::set(Log::Type::Error, Logger::Ptr(new FileLogger(errFile)));
	}
}

bool decompile(retdec::config::Config& config, std::string* outString)
{
	setLogsFrom(config.parameters);

	Log::phase("Initialization");
	auto& passRegistry = initializeLlvmPasses();

	// limitMaximalMemoryIfRequested(params);
	// PrintAfterAll = true;

	auto context = std::make_unique<llvm::LLVMContext>();
	auto module = createLlvmModule(*context);

	// Create a PassManager to hold and optimize the collection of passes we
	// are about to build.
	llvm::legacy::PassManager pm;

	// Without this LLVM does more opts than we would like it to.
	// e.g. printf() call -> puts() call
	//
	// Add an appropriate TargetLibraryInfo pass for the module's triple.
	Triple ModuleTriple(module->getTargetTriple());
	TargetLibraryInfoImpl TLII(ModuleTriple);
	// The -disable-simplify-libcalls flag actually disables all builtin optzns.
	TLII.disableAllFunctions();
	pm.add(new TargetLibraryInfoWrapperPass(TLII));

	for (auto& p : config.parameters.llvmPasses)
	{
		if (auto* info = passRegistry.getPassInfo(p))
		{
			auto* pass = info->createPass();
			addPass(pm, pass, info);

			if (info->getTypeInfo() == &bin2llvmir::ProviderInitialization::ID)
			{
				auto* p = static_cast<bin2llvmir::ProviderInitialization*>(pass);
				p->setConfig(&config);
			}
			if (info->getTypeInfo() == &llvmir2hll::LlvmIr2Hll::ID)
			{
				auto* p = static_cast<llvmir2hll::LlvmIr2Hll*>(pass);
				p->setConfig(&config);
				p->setOutputString(outString);
			}
		}
		else
		{
			throw std::runtime_error("cannot create pass: " + p);
		}
	}

	// Now that we have all of the passes ready, run them.
	const auto pipelineT0 = std::chrono::steady_clock::now();
	pm.run(*module);
	if (bin2llvmirPassDiagEnabled())
	{
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - pipelineT0).count();
		Log::info() << "[bin2llvmir-diag] pipeline_wall_ms=" << ms << std::endl;
	}

	// ── Post-decompile analysis passes ────────────────────────────────────────
	// Build a lightweight retdec::ssa::SSAModule from the decoded LLVM IR so
	// the analysis passes (concurrency, sort, OCL, IPA, …) can run.
	{
		Log::phase("Post-pipeline analysis");
		const auto analysisT0 = std::chrono::steady_clock::now();

		auto ssaMod = buildSsaModule(*module);
		if (ssaMod && !ssaMod->functions.empty())
		{
			// Collect a flat const-pointer list once — reused by all module passes.
			std::vector<const ssa::SSAFunction*> fnPtrs;
			fnPtrs.reserve(ssaMod->functions.size());
			for (const auto& fp : ssaMod->functions)
				if (fp) fnPtrs.push_back(fp.get());

			// --- 1. Calling-convention inference (must run before IPA) ---
			// Use the batch runAll() method to build a ccMap for all functions.
			std::unordered_map<std::string, call_conv::CallingConvention> ccMap;
			{
				call_conv::CallConvPass ccPass;
				ccMap = ccPass.runAll(fnPtrs);
			}

			// --- 2. IPA (call graph + summary propagation) ---
			ipa::IpaResult ipaResult;
			{
				ipa::IpaPass ipaPass;
				ipaResult = ipaPass.run(fnPtrs, ccMap);
				if (!ipaResult.inlineCandidates.empty())
					Log::info() << "[analysis] IPA: "
					            << ipaResult.inlineCandidates.size()
					            << " inline candidate(s), "
					            << ipaResult.globals.size()
					            << " global(s) typed" << std::endl;
			}

			// --- 3. Type inference (per function, seeded from IPA summaries) ---
			{
				// Convert an IPA ParamInfo → AbiSeedInfo::ParamSeed.
				auto makeParamSeed =
				    [](uint32_t i, const ipa::FunctionSummary::ParamInfo& pi)
				{
					type_inference::AbiSeedInfo::ParamSeed ps;
					ps.ssaId      = ssa::kInvalidValue; // resolved per-function
					ps.paramIndex = static_cast<uint8_t>(i);
					ps.type.kind  = pi.isFp
					    ? type_inference::TypeKind::Float
					    : pi.isPointer
					        ? type_inference::TypeKind::Pointer
					        : type_inference::TypeKind::Integer;
					ps.type.width = pi.width;
					return ps;
				};

				for (const auto* fn : fnPtrs)
				{
					type_inference::TypeInferencePass tiPass;
					type_inference::TypeInferencePass::Config tiCfg;

					auto it = ipaResult.summaries.find(fn->name());
					if (it != ipaResult.summaries.end())
					{
						const auto& sum = it->second;
						for (std::size_t i = 0; i < sum.params.size(); ++i)
							tiCfg.abiSeed.params.push_back(
							    makeParamSeed(static_cast<uint32_t>(i), sum.params[i]));

						if (!sum.isVoid)
						{
							type_inference::IrType retTy;
							retTy.kind  = sum.retIsFp
							    ? type_inference::TypeKind::Float
							    : sum.retIsPtr
							        ? type_inference::TypeKind::Pointer
							        : type_inference::TypeKind::Integer;
							retTy.width = sum.retWidth;
							tiCfg.abiSeed.retVal = type_inference::AbiSeedInfo::ReturnSeed{
							    ssa::kInvalidValue, retTy};
						}
					}

					tiPass.run(*fn, tiCfg);
				}
			}

			// --- 4. Concurrency / threading detection ---
			{
				concurrency_detect::ConcurrencyDetector cd;
				auto cm = cd.analyseModule(*ssaMod);
				if (cm.isMT)
					Log::info() << "[analysis] concurrency detected: "
					            << cm.threads.size() << " thread(s), "
					            << cm.locks.size() << " lock(s), "
					            << cm.atomics.size() << " atomic(s)" << std::endl;
			}

			// --- 5. Container detection (STL containers, linked lists, trees) ---
			{
				container_detect::ContainerDetector cdet;
				auto cmap = cdet.analyseModule(fnPtrs);
				if (!cmap.empty())
					Log::info() << "[analysis] containers detected in "
					            << cmap.size() << " function(s)" << std::endl;
			}

			// --- 6. Algorithm detection (<algorithm> patterns) ---
			{
				algo_recover::AlgorithmDetector adet;
				auto amap = adet.detectModule(fnPtrs);
				if (!amap.empty())
					Log::info() << "[analysis] <algorithm> patterns detected in "
					            << amap.size() << " function(s)" << std::endl;
			}

			// --- 7. Sorting algorithm detection ---
			{
				sort_detect::SortDetector sd;
				auto dm = sd.analyseModule(fnPtrs);
				if (!dm.empty())
					Log::info() << "[analysis] sorting algorithms detected in "
					            << dm.size() << " function(s)" << std::endl;
			}

			// --- 8. Serial / wire-protocol detection (protobuf, flatbuffers, …) ---
			{
				serial_detect::SerialDetector sdet;
				auto smap = sdet.analyseModule(fnPtrs);
				if (!smap.empty())
					Log::info() << "[analysis] serial protocols detected in "
					            << smap.size() << " function(s)" << std::endl;
			}

			// --- 9. OpenCL host-side recovery ---
			{
				ptx_decompile::OclHostRecovery ocl;
				auto om = ocl.analyseModule(*ssaMod);
				if (om.hasOpenCL)
				{
					ptx_decompile::OclHostEmitter emitter;
					Log::info() << "[analysis] OpenCL host code recovered:\n"
					            << emitter.emit(om) << std::endl;
				}
			}
		}

		if (bin2llvmirPassDiagEnabled())
		{
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - analysisT0).count();
			Log::info() << "[analysis-diag] post_pipeline_analysis_wall_ms=" << ms
			            << std::endl;
		}
	}

	return EXIT_SUCCESS;
}

LlvmModuleContextPair decompileToLlvmIr(
		retdec::config::Config& config,
		const std::string& stopBeforePass)
{
	setLogsFrom(config.parameters);

	Log::phase("Initialization");
	auto& passRegistry = initializeLlvmPasses();

	auto context = std::make_unique<llvm::LLVMContext>();
	auto module = createLlvmModule(*context);

	llvm::legacy::PassManager pm;

	Triple ModuleTriple(module->getTargetTriple());
	TargetLibraryInfoImpl TLII(ModuleTriple);
	TLII.disableAllFunctions();
	pm.add(new TargetLibraryInfoWrapperPass(TLII));

	for (auto& p : config.parameters.llvmPasses)
	{
		if (p == stopBeforePass)
			break;

		if (auto* info = passRegistry.getPassInfo(p))
		{
			auto* pass = info->createPass();
			addPass(pm, pass, info);

			if (info->getTypeInfo() == &bin2llvmir::ProviderInitialization::ID)
			{
				auto* pi = static_cast<bin2llvmir::ProviderInitialization*>(pass);
				pi->setConfig(&config);
			}
		}
		else
		{
			throw std::runtime_error("cannot create pass: " + p);
		}
	}

	const auto pipelineT0 = std::chrono::steady_clock::now();
	pm.run(*module);
	if (bin2llvmirPassDiagEnabled())
	{
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - pipelineT0).count();
		Log::info() << "[bin2llvmir-diag] pipeline_wall_ms=" << ms << std::endl;
	}

	return LlvmModuleContextPair{std::move(module), std::move(context)};
}

namespace {

bool emulationUnpackDiagEnabled()
{
	const char *e = std::getenv("RETDEC_EMULATION_UNPACK_DIAG");
	return e != nullptr && e[0] != '\0' && e[0] != '0';
}

} // namespace

bool tryEmulationUnpacking(
		retdec::config::Config& config,
		const std::string& outputPath)
{
	const bool diag = emulationUnpackDiagEnabled();

	if (diag)
	{
		const std::string &inPath = config.parameters.getInputFile();
		Log::info() << "emulation unpack: input_path='" << inPath << "'." << std::endl;
		if (!inPath.empty())
		{
			std::ifstream ifs(inPath, std::ios::binary | std::ios::ate);
			if (ifs)
			{
				const auto sz = static_cast<std::size_t>(ifs.tellg());
				Log::info() << "emulation unpack: input_bytes=" << sz << "." << std::endl;
			}
			else
			{
				Log::info() << "emulation unpack: could not stat input file for size."
						<< std::endl;
			}
		}
	}

	try
	{
		auto pair = decompileToLlvmIr(config, "retdec-llvmir2hll");
		if (!pair.module || !pair.context)
		{
			if (diag)
			{
				Log::info() << "emulation unpack: LLVM IR generation failed (no module)."
						<< std::endl;
			}
			return false;
		}

		llvm::Module* module = pair.module.get();
		auto* bin2llvmirConfig = bin2llvmir::ConfigProvider::getConfig(module);
		if (!bin2llvmirConfig)
		{
			if (diag)
			{
				Log::info() << "emulation unpack: bin2llvmir config not available on module."
						<< std::endl;
			}
			return false;
		}

		auto ep = config.parameters.getEntryPoint();
		if (ep.isUndefined())
			ep = config.parameters.getMainAddress();
		if (ep.isUndefined())
		{
			if (diag)
			{
				Log::info() << "emulation unpack: no entry point or main address."
						<< std::endl;
			}
			return false;
		}

		llvm::Function* entryFunc = bin2llvmirConfig->getLlvmFunction(ep);
		if (!entryFunc || entryFunc->isDeclaration() || entryFunc->empty())
		{
			if (diag)
			{
				Log::info() << "emulation unpack: entry LLVM function missing, external, "
						"or empty at "
						<< retdec::utils::intToHexString(ep.getValue(), true) << "."
						<< std::endl;
			}
			return false;
		}

		if (diag)
		{
			Log::info() << "emulation unpack: entry_rva="
					<< retdec::utils::intToHexString(ep.getValue(), true)
					<< " entry_llvm_name='" << entryFunc->getName().str() << "'."
					<< std::endl;
		}

		retdec::llvmir_emul::LlvmIrEmulator emu(module);
		emu.runFunction(entryFunc, {});

		auto storedAddrs = emu.getStoredMemory();
		if (storedAddrs.empty())
		{
			if (diag)
			{
				Log::info() << "emulation unpack: emulator performed no stores to tracked "
						"memory."
						<< std::endl;
			}
			return false;
		}

		uint64_t minAddr = UINT64_MAX;
		uint64_t maxAddr = 0;
		for (uint64_t addr : storedAddrs)
		{
			minAddr = std::min(minAddr, addr);
			maxAddr = std::max(maxAddr, addr + 7);
		}

		const size_t maxDumpSize = 16 * 1024 * 1024;
		size_t rangeSize = (maxAddr >= minAddr) ? (maxAddr - minAddr + 1) : 0;
		if (rangeSize == 0 || rangeSize > maxDumpSize)
		{
			if (diag)
			{
				Log::info() << "emulation unpack: dumped range size " << rangeSize
						<< " is empty or over " << maxDumpSize << " byte cap."
						<< std::endl;
			}
			return false;
		}

		std::ofstream out(outputPath, std::ios::binary);
		if (!out)
		{
			if (diag)
			{
				Log::info() << "emulation unpack: cannot open output file '" << outputPath
						<< "'." << std::endl;
			}
			return false;
		}

		std::vector<uint8_t> buf(rangeSize, 0);
		for (uint64_t addr : storedAddrs)
		{
			if (addr >= minAddr && addr + 8 <= minAddr + rangeSize)
			{
				auto gv = emu.getMemoryValue(addr);
				uint64_t val = gv.IntVal.getZExtValue();
				for (unsigned i = 0; i < 8; ++i)
					buf[addr - minAddr + i] = static_cast<uint8_t>(val >> (i * 8));
			}
		}

		out.write(reinterpret_cast<const char*>(buf.data()), buf.size());
		out.close();
		if (!out.good())
		{
			if (diag)
			{
				Log::info() << "emulation unpack: write to '" << outputPath
						<< "' failed." << std::endl;
			}
			return false;
		}

		// Set entry point and section VMA for raw loader (unpacked code at minAddr)
		config.parameters.setEntryPoint(retdec::common::Address(minAddr));
		config.parameters.setSectionVMA(retdec::common::Address(minAddr));

		if (diag)
		{
			Log::info() << "emulation unpack: success store_sites=" << storedAddrs.size()
					<< " dump_range_bytes=" << rangeSize << " min_addr="
					<< retdec::utils::intToHexString(minAddr, true) << " max_addr="
					<< retdec::utils::intToHexString(maxAddr, true) << " output_path='"
					<< outputPath << "'." << std::endl;
		}

		Log::info() << "Emulation unpack: wrote " << rangeSize << " bytes to '"
				<< outputPath << "' (image base "
				<< retdec::utils::intToHexString(minAddr, true) << ")." << std::endl;
		return true;
	}
	catch (const std::exception&)
	{
		if (diag)
		{
			Log::info() << "emulation unpack: exception during LLVM IR build or emulation."
					<< std::endl;
		}
		return false;
	}
}

} // namespace retdec

//==============================================================================
// parallel batch decompiler
//==============================================================================

/**
 * @brief Decompile multiple binaries in parallel using a thread pool.
 */
namespace retdec {

std::vector<bool> parallelBatchDecompile(
        std::vector<retdec::config::Config>& configs,
        std::size_t numJobs)
{
    const std::size_t n = configs.size();
    std::vector<bool> results(n, false);
    if (n == 0) return results;

    if (numJobs == 0 || numJobs > n) {
        numJobs = std::min(
            n,
            static_cast<std::size_t>(std::thread::hardware_concurrency())
        );
    }
    if (numJobs == 0) numJobs = 1;

    retdec::utils::ThreadPool pool(numJobs);
    std::vector<std::future<bool>> futures;
    futures.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        futures.push_back(pool.submit([&configs, i]() -> bool {
            return decompile(configs[i], nullptr);
        }));
    }

    for (std::size_t i = 0; i < n; ++i) {
        try {
            results[i] = futures[i].get();
        } catch (const std::exception& e) {
            Log::error() << "parallelBatchDecompile: job " << i
                         << " threw: " << e.what() << std::endl;
            results[i] = false;
        }
    }
    return results;
}

} // namespace retdec
