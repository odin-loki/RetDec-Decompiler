/**
 * @file src/sass_decode/sass_lifter.cpp
 * @brief CUDA-C emission from decoded SASS subset + optional PTX payload.
 */

#include "retdec/sass_decode/sass_lifter.h"

#include "retdec/ptx_decompile/ptx_lifter.h"
#include "retdec/sass_decode/cubin_loader.h"
#include "retdec/sass_decode/sass_decoder.h"

#include <cstdio>
#include <set>
#include <sstream>

namespace retdec {
namespace sass_decode {
namespace {

const char* ptrTypedef(uint32_t bits)
{
	return bits == 32 ? "uint32_t" : "uint64_t";
}

std::string regName(uint8_t r)
{
	if (r == kRegZero)
	{
		return "0";
	}
	std::ostringstream os;
	os << "r" << unsigned(r);
	return os.str();
}

std::string predGuard(const SassInstr& in)
{
	if (in.pred == kPredTrue)
	{
		return {};
	}
	std::ostringstream os;
	os << "if (p" << unsigned(in.pred) << ") ";
	return os.str();
}

void collectRegs(const SassKernel& k, std::set<unsigned>& regs, std::set<unsigned>& preds)
{
	for (const auto& in : k.instrs)
	{
		if (!in.decoded)
		{
			continue;
		}
		auto add = [&](uint8_t r) {
			if (r != kRegZero)
			{
				regs.insert(r);
			}
		};
		add(in.dest);
		add(in.src0);
		add(in.src1);
		add(in.src2);
		if (in.pred != kPredTrue)
		{
			preds.insert(in.pred);
		}
	}
}

std::string hex64(uint64_t v)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
	return buf;
}

std::string liftInstr(const SassInstr& in, uint32_t pointerBits)
{
	const std::string g = predGuard(in);
	std::ostringstream os;
	os << "    ";
	if (!in.decoded)
	{
		os << "/* sass " << hex64(in.pc) << "  " << hex64(in.word0);
		if (in.size >= 16)
		{
			os << "  " << hex64(in.word1);
		}
		os << " */";
		return os.str();
	}

	switch (in.opcode)
	{
	case SassOpcode::Nop:
		os << g << "; /* NOP */";
		break;
	case SassOpcode::Exit:
		os << g << "return;";
		break;
	case SassOpcode::Bra:
		os << g << "goto L_bra_" << hex64(in.pc) << "; /* BRA; target bits not in NVIDIA ISA docs */";
		break;
	case SassOpcode::Iadd3:
		os << g << regName(in.dest) << " = " << regName(in.src0) << " + " << regName(in.src1) << " + "
		   << regName(in.src2) << ";";
		break;
	case SassOpcode::Fadd:
		os << g << "*(float*)&" << regName(in.dest) << " = *(float*)&" << regName(in.src0) << " + *(float*)&"
		   << regName(in.src1) << ";";
		break;
	case SassOpcode::Imad:
		os << g << regName(in.dest) << " = " << regName(in.src0) << " * " << regName(in.src1) << " + "
		   << regName(in.src2) << ";";
		break;
	case SassOpcode::Mov:
		os << g << regName(in.dest) << " = " << regName(in.src0) << "; /* MOV */";
		break;
	case SassOpcode::Ldg:
		os << g << regName(in.dest) << " = *(" << ptrTypedef(pointerBits) << "*)(uintptr_t)" << regName(in.src0)
		   << "; /* LDG */";
		break;
	case SassOpcode::Stg:
		os << g << "*(" << ptrTypedef(pointerBits) << "*)(uintptr_t)" << regName(in.src0) << " = " << regName(in.src1)
		   << "; /* STG */";
		break;
	case SassOpcode::S2r:
		os << g << regName(in.dest)
		   << " = threadIdx.x; /* S2R; SR selector bits are not in NVIDIA ISA docs */";
		break;
	case SassOpcode::Ldc:
		os << g << regName(in.dest) << " = /* LDC constant */ 0;";
		break;
	default:
		os << "/* UNKNOWN */";
		break;
	}
	return os.str();
}

} // namespace

std::string SassLifter::liftKernel(const SassKernel& kernel, const SassModule& mod) const
{
	std::ostringstream os;
	os << "__global__ void " << (kernel.name.empty() ? "kernel" : kernel.name) << "(void) {\n";
	std::set<unsigned> regs, preds;
	collectRegs(kernel, regs, preds);
	if (!preds.empty())
	{
		os << "    int";
		bool first = true;
		for (unsigned p : preds)
		{
			os << (first ? " " : ", ") << "p" << p;
			first = false;
		}
		os << ";\n";
	}
	if (!regs.empty())
	{
		os << "    unsigned int";
		bool first = true;
		for (unsigned r : regs)
		{
			os << (first ? " " : ", ") << "r" << r;
			first = false;
		}
		os << ";\n";
	}
	os << "    /* " << smName(mod.sm) << ", " << mod.pointerBits << "-bit GPU pointers, "
	   << sassInstrSize(mod.sm ? mod.sm : 80) << "-byte SASS words */\n";
	for (const auto& in : kernel.instrs)
	{
		os << liftInstr(in, mod.pointerBits) << "\n";
	}
	os << "}\n";
	return os.str();
}

std::string SassLifter::liftModule(const SassModule& mod) const
{
	std::ostringstream os;
	os << "/* RetDec SASS subset lift — NOT Production. SASS is not in Capstone.\n";
	os << " * " << smName(mod.sm) << ", ELF" << (mod.elf64 ? "64" : "32") << ", EM_CUDA=" << unsigned(mod.eMachine)
	   << ", GPU pointers=" << mod.pointerBits << "-bit.\n";
	os << " * nvdisasm is not used (CUDA EULA). Unknown opcodes are raw hex comments.\n";
	os << " */\n";
	if (!mod.error.empty())
	{
		os << "/* error: " << mod.error << " */\n";
		return os.str();
	}
	for (const auto& w : mod.warnings)
	{
		os << "/* warning: " << w << " */\n";
	}
	for (const auto& k : mod.kernels)
	{
		os << liftKernel(k, mod) << "\n";
	}
	if (!mod.ptxText.empty())
	{
		os << "/* ---- embedded PTX (not SASS) ---- */\n";
		ptx_decompile::PtxParser parser;
		auto ptx = parser.parse(mod.ptxText);
		if (!parser.lastError().empty())
		{
			os << "/* PTX parse: " << parser.lastError() << " */\n";
		}
		else
		{
			ptx_decompile::PtxLifter ptxLift;
			os << ptxLift.liftModule(ptx);
		}
	}
	return os.str();
}

std::string decompileCudaBinary(const uint8_t* data, size_t size, const SassConfig& cfg)
{
	SassModule mod = loadCudaBinary(data, size, cfg);
	if (mod.ok() || !mod.kernels.empty() || !mod.ptxText.empty())
	{
		SassDecoder dec(mod.sm ? mod.sm : 80, mod.pointerBits);
		dec.decodeModule(mod);
	}
	return SassLifter{}.liftModule(mod);
}

} // namespace sass_decode
} // namespace retdec
