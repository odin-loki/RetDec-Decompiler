/**
 * @file src/cli_parser/cli_reader.cpp
 * @brief Top-level .NET CLI reader — produces a BcModule from a PE assembly.
 */

#include <unordered_map>
#include <memory>
#include "retdec/cli_parser/cli_reader.h"

#include "retdec/utils/byte_order.h"
#include "retdec/utils/text_transcode.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace retdec {
namespace cli_parser {

// ─── CLIReader ────────────────────────────────────────────────────────────────

CLIReader::CLIReader(const CliReadOptions& opts): opts_(opts) {}

CliReadResult CLIReader::readFile(const std::string& path)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f.is_open())
	{
		CliReadResult r;
		r.error = "Cannot open file: " + path;
		return r;
	}
	auto sz = static_cast<size_t>(f.tellg());
	f.seekg(0);
	std::vector<uint8_t> buf(sz);
	f.read(reinterpret_cast<char*>(buf.data()), sz);
	std::string name = path;
	auto slash = name.find_last_of("/\\");
	if (slash != std::string::npos) name = name.substr(slash + 1);
	return read(buf.data(), sz, name);
}

CliReadResult CLIReader::read(const uint8_t* data, size_t size, const std::string& name)
{
	CliReadResult result;

	// Reset state
	fileData_ = data;
	fileSize_ = size;
	typeDefNames_.clear();
	typeRefNames_.clear();
	nestedMap_.clear();

	// Phase 1: PE parsing
	if (!pe_.open(data, size))
	{
		result.error = "PE parse failed: " + pe_.error();
		return result;
	}
	if (!pe_.hasCLI())
	{
		result.error = "Not a .NET assembly (no CLI directory)";
		return result;
	}

	// Phase 2: Build heap set
	auto tildeData = pe_.streamBytes("#~");
	auto strData = pe_.streamBytes("#Strings");
	auto usData = pe_.streamBytes("#US");
	auto guidData = pe_.streamBytes("#GUID");
	auto blobData = pe_.streamBytes("#Blob");

	// HeapSizes comes from the #~ stream header byte 6
	uint8_t heapSizes = (tildeData.size() >= 7) ? tildeData[6] : 0;

	heaps_ = std::make_unique<CliHeaps>(strData, usData, guidData, blobData, heapSizes);

	// Phase 3: Metadata tables
	tables_ = std::make_unique<MetadataTables>();
	if (!tables_->parse(tildeData, *heaps_))
	{
		result.error = "Metadata table parse failed: " + tables_->error();
		return result;
	}

	// Phase 4a: Build NestedClass map.
	//
	// This used to be Phase 6, after buildTypeNames() -- which is the only
	// thing that reads it. nestedMap_ was empty every time buildTypeNames ran,
	// so its "fix up nested type names (Outer+Inner)" loop had nothing to
	// iterate and no nested type ever got its enclosing type's name.
	uint32_t numNested = tables_->rowCount(TableId::NestedClass);
	for (uint32_t i = 1; i <= numNested; ++i)
	{
		auto row = tables_->nestedClass(i);
		nestedMap_.emplace_back(row.nestedClass, row.enclosingClass);
	}

	// Phase 4b: Build type name cache
	if (!buildTypeNames())
	{
		result.error = "Failed to build type name cache";
		return result;
	}

	// Phase 5: Build sig decoder + CIL lifter
	sigDecoder_ = std::make_unique<CliSigDecoder>(this);
	if (opts_.decodeCIL) cilLifter_ = std::make_unique<CILLifter>(this);

	// Phase 7: Determine assembly name
	std::string asmName = name;
	if (tables_->rowCount(TableId::Assembly) >= 1)
	{
		auto asmRow = tables_->assembly(1);
		std::string fromMeta = heaps_->strings.get(asmRow.name);
		if (!fromMeta.empty()) asmName = fromMeta;
	}

	// Phase 8: Build BcModule
	BcModule module(asmName, SourceLang::CSharp);
	module.setName(asmName);
	if (tables_->rowCount(TableId::Assembly) >= 1)
	{
		auto asmRow = tables_->assembly(1);
		module.version = std::to_string(asmRow.majorVersion) + "." + std::to_string(asmRow.minorVersion) + "."
					   + std::to_string(asmRow.buildNumber) + "." + std::to_string(asmRow.revisionNumber);
		if (opts_.decodeCustomAttrs)
		{
			MetadataToken parent{static_cast<uint8_t>(TableId::Assembly), 1};
			module.moduleAnnotations = customAttributes(parent);
		}
	}
	for (const auto& n: typeRefNames_)
	{
		if (n.empty() || n == "<unknown>") continue;
		module.addExternalRef(n, n);
	}

	uint32_t numTypeDefs = tables_->rowCount(TableId::TypeDef);
	result.typeDefCount = numTypeDefs;

	// Skip TypeDef index 1 = <Module> (it's a pseudo-class for module-level methods)
	for (uint32_t ti = 2; ti <= numTypeDefs; ++ti)
	{
		auto row = tables_->typeDef(ti);
		std::string className = typeDefName(ti);

		// Skip compiler-generated types
		if (opts_.skipSynthetic)
		{
			// Types starting with '<' are compiler generated
			std::string simpleName = heaps_->strings.get(row.name);
			if (!simpleName.empty() && simpleName[0] == '<') continue;
		}

		// Skip private types if requested
		if (opts_.skipPrivate)
		{
			uint32_t vis = row.flags & TypeAttributes::VisibilityMask;
			if (vis == TypeAttributes::NotPublic || vis == TypeAttributes::NestedPrivate) continue;
		}

		++result.typeDefCount;

		try
		{
			BcClass cls = buildClass(ti, result);
			module.addClass(std::move(cls));
		}
		catch (const std::exception&)
		{
			++result.parseErrorCount;
		}
	}

	result.module = std::move(module);
	result.success = true;
	return result;
}

// ─── ITypeNameResolver ────────────────────────────────────────────────────────

std::string CLIReader::typeDefName(uint32_t idx) const
{
	if (idx == 0 || idx > typeDefNames_.size()) return "<unknown>";
	return typeDefNames_[idx - 1];
}

std::string CLIReader::typeRefName(uint32_t idx) const
{
	if (idx == 0 || idx > typeRefNames_.size()) return "<unknown>";
	return typeRefNames_[idx - 1];
}

BcType CLIReader::typeSpecType(uint32_t idx) const
{
	if (!tables_ || !sigDecoder_) return types::ClrObject();
	uint32_t numSpecs = tables_->rowCount(TableId::TypeSpec);
	if (idx == 0 || idx > numSpecs) return types::ClrObject();
	auto row = tables_->typeSpec(idx);
	auto blob = heaps_->blobs.get(row.signature);
	auto ct = sigDecoder_->decodeTypeSpec(blob);
	if (!ct) return types::ClrObject();
	return ct->base;
}

// ─── buildTypeNames ───────────────────────────────────────────────────────────

bool CLIReader::buildTypeNames()
{
	uint32_t numTypeDefs = tables_->rowCount(TableId::TypeDef);
	typeDefNames_.resize(numTypeDefs);

	for (uint32_t i = 1; i <= numTypeDefs; ++i)
	{
		auto row = tables_->typeDef(i);
		std::string ns = heaps_->strings.get(row.ns);
		std::string name = heaps_->strings.get(row.name);
		typeDefNames_[i - 1] = makeClrName(ns, name);
	}

	// Fix up nested type names (Outer+Inner).
	//
	// ECMA-335 II.22.32 does not order the NestedClass rows, and a nested type
	// can itself enclose another, so the enclosing type's own name may not be
	// final yet. Resolve each chain from its outermost type instead of reading
	// whatever typeDefNames_ happens to hold.
	std::unordered_map<uint32_t, uint32_t> enclosingOf;
	for (const auto& [nested, enclosing]: nestedMap_)
	{
		if (nested == 0 || nested > numTypeDefs) continue;
		if (enclosing == 0 || enclosing > numTypeDefs) continue;
		if (nested == enclosing) continue; // a type cannot enclose itself
		enclosingOf[nested] = enclosing;
	}

	// A malformed table can name a cycle; the chain length is bounded by the
	// number of type definitions, so anything longer is one.
	for (const auto& [nested, enclosing]: enclosingOf)
	{
		std::vector<uint32_t> chain;
		uint32_t cur = nested;
		bool cyclic = false;
		while (true)
		{
			chain.push_back(cur);
			auto it = enclosingOf.find(cur);
			if (it == enclosingOf.end()) break;
			cur = it->second;
			if (chain.size() > numTypeDefs)
			{
				cyclic = true;
				break;
			}
		}
		if (cyclic) continue;
		(void)enclosing;

		// chain is innermost..outermost; the outermost keeps its namespaced
		// name and each step inwards appends "+Name".
		std::string full = typeDefNames_[chain.back() - 1];
		for (std::size_t k = chain.size(); k-- > 1;)
		{
			auto row = tables_->typeDef(chain[k - 1]);
			full += "+" + heaps_->strings.get(row.name);
		}
		typeDefNames_[nested - 1] = full;
	}

	// TypeRef names
	uint32_t numTypeRefs = tables_->rowCount(TableId::TypeRef);
	typeRefNames_.resize(numTypeRefs);
	for (uint32_t i = 1; i <= numTypeRefs; ++i)
	{
		auto row = tables_->typeRef(i);
		std::string ns = heaps_->strings.get(row.ns);
		std::string name = heaps_->strings.get(row.name);
		typeRefNames_[i - 1] = makeClrName(ns, name);
	}

	return true;
}

std::string CLIReader::makeClrName(const std::string& ns, const std::string& name) const
{
	if (ns.empty()) return name;
	return ns + "." + name;
}

// ─── buildClass ───────────────────────────────────────────────────────────────

namespace {

/// The half-open row range a metadata list column describes, clamped to a table
/// that actually has @a rows rows.
///
/// ECMA-335 II.22: a TypeDef's FieldList and MethodList, and a MethodDef's
/// ParamList, are the first row that entity owns, and the run ends where the
/// NEXT entity's column begins. Both endpoints are raw uint32 column values
/// decoded straight out of the #~ stream. The format requires them to be
/// monotonically non-decreasing and to index inside the target table; nothing
/// in the file makes them so.
///
/// They were used as raw loop bounds. Every iteration of those loops allocates
/// -- buildField returns a BcField that is push_back'd, buildMethod likewise,
/// and with decodeCustomAttrs on (the default) each field also scans the
/// Constant table three times -- and the row accessors return a default row for
/// an out-of-range index, so the loop never stops early. It just manufactures
/// empty objects. Measured, from a 1,024-byte assembly whose third TypeDef row
/// declares FieldList and MethodList at 0xFFFF over tables of one row each:
///
///     before:  fields=65534 methods=65534   peak RSS 86,348 KB
///     after:   fields=0     methods=0       peak RSS  3,880 KB
///
/// (a well-formed assembly of the same size peaks at 3,812 KB).
///
/// A MethodDef's ParamList is the same column shape one level down, bounded by
/// the next MethodDef's. That one costs work rather than memory -- an
/// out-of-range Param row decodes as a default whose Sequence is 0, which the
/// loop skips, so it manufactures nothing and simply walks rows that are not
/// there. Measured over 200 reads of the same 1 KB assembly, with the field and
/// method clamps in place both times so the difference is this loop alone:
/// 0 ms clamped against 15 ms unclamped.
///
/// Rows are 1-based, so the first row is 1 and one past the last is `rows + 1`;
/// the sum is formed in 64 bits because `rows` is a uint32 out of the file and
/// `rows + 1` wraps at 0xFFFFFFFF. Both ends are held inside [1, rows + 1]: a
/// column naming row 0 is as out of spec as one naming row 0xFFFF, and it costs
/// a manufactured row rather than 65533 of them, which is exactly why the
/// cheaper end of this defect is the one that would have survived review.
struct RowRange
{
	std::uint32_t begin = 0;
	std::uint32_t end = 0;
};

RowRange clampRowRange(std::uint32_t begin, std::uint32_t end, std::uint32_t rows)
{
	const std::uint64_t limit = static_cast<std::uint64_t>(rows) + 1;
	const auto hold = [limit](std::uint32_t v) {
		return static_cast<std::uint32_t>(std::min<std::uint64_t>(std::max<std::uint64_t>(v, 1), limit));
	};
	RowRange r;
	r.begin = hold(begin);
	r.end = hold(end);
	// A range that runs backwards is not a range. The format forbids it; the
	// bytes do not.
	if (r.end < r.begin) r.end = r.begin;
	return r;
}

} // namespace

BcClass CLIReader::buildClass(uint32_t typeDefIdx, CliReadResult& result) const
{
	auto row = tables_->typeDef(typeDefIdx);
	BcClass cls;
	cls.fqName = typeDefName(typeDefIdx);
	// Simple name = after last '.' or '+'
	cls.name = cls.fqName;
	auto dot = cls.fqName.find_last_of(".+");
	if (dot != std::string::npos) cls.name = cls.fqName.substr(dot + 1);
	// Package name
	auto dot2 = cls.fqName.find_last_of('.');
	if (dot2 != std::string::npos) cls.packageName = cls.fqName.substr(0, dot2);

	// Access flags
	cls.access = typeDefAccess(row.flags);

	// Nested type → outer class name
	if (opts_.decodeNestedClasses && isNested(typeDefIdx))
	{
		uint32_t enc = enclosingClass(typeDefIdx);
		if (enc != 0) cls.outerClass = typeDefName(enc);
	}

	// is interface / abstract / sealed
	cls.isInterface = (row.flags & TypeAttributes::ClassSemanticsMask) == TypeAttributes::Interface;
	cls.isAbstract = (row.flags & TypeAttributes::Abstract) != 0;

	// Superclass
	if (row.extends.valid())
	{
		auto sup = resolveExtends(row.extends);
		if (sup) cls.superClass = *sup;
	}
	if (cls.superClass)
	{
		std::string sn = cls.superClass->isRef() ? cls.superClass->ref().className : cls.superClass->toString();
		if (sn == "System.Enum") cls.isEnum = true;
	}

	// Interfaces
	cls.interfaces = resolveInterfaces(typeDefIdx);

	// Generic parameters
	if (opts_.decodeGenericParams)
	{
		auto gps = genericParams(typeDefIdx, false);
		for (const auto& gp: gps)
			cls.typeParams.push_back(gp);
	}

	// Fields
	uint32_t numTypeDefs = tables_->rowCount(TableId::TypeDef);
	uint32_t fieldEnd;
	if (typeDefIdx < numTypeDefs)
		fieldEnd = tables_->typeDef(typeDefIdx + 1).fieldList;
	else
		fieldEnd = tables_->rowCount(TableId::Field) + 1;

	const auto fieldRange = clampRowRange(row.fieldList, fieldEnd, tables_->rowCount(TableId::Field));
	for (uint32_t fi = fieldRange.begin; fi < fieldRange.end; ++fi)
	{
		try
		{
			auto f = buildField(fi);
			cls.fields.push_back(std::move(f));
		}
		catch (const std::exception&)
		{
			++result.parseErrorCount;
		}
	}
	result.fieldCount += static_cast<uint32_t>(cls.fields.size());

	if (cls.isEnum)
	{
		for (const auto& f: cls.fields)
		{
			if (f.name == "value__") continue;
			if (hasFlag(f.access, BcAccess::Static)) cls.enumConstants.push_back(f.name);
		}
	}

	// Methods
	uint32_t methodEnd;
	if (typeDefIdx < numTypeDefs)
		methodEnd = tables_->typeDef(typeDefIdx + 1).methodList;
	else
		methodEnd = tables_->rowCount(TableId::MethodDef) + 1;

	const auto methodRange = clampRowRange(row.methodList, methodEnd, tables_->rowCount(TableId::MethodDef));
	for (uint32_t mi = methodRange.begin; mi < methodRange.end; ++mi)
	{
		try
		{
			auto m = buildMethod(mi, result);
			cls.methods.push_back(std::move(m));
		}
		catch (const std::exception&)
		{
			++result.parseErrorCount;
		}
	}
	result.methodDefCount += static_cast<uint32_t>(cls.methods.size());

	// Custom attributes
	if (opts_.decodeCustomAttrs)
	{
		MetadataToken parent{static_cast<uint8_t>(TableId::TypeDef), typeDefIdx};
		cls.annotations = customAttributes(parent);
	}

	return cls;
}

BcField CLIReader::buildField(uint32_t fieldIdx) const
{
	auto row = tables_->field(fieldIdx);
	BcField f;
	f.name = heaps_->strings.get(row.name);
	f.access = fieldAccess(row.flags);

	auto blob = heaps_->blobs.get(row.signature);
	if (!blob.empty())
	{
		auto ct = sigDecoder_->decodeField(blob);
		if (ct) f.type = ct->base;
	}
	f.constantIntValue = fieldConstantInt(fieldIdx);
	f.constantFltValue = fieldConstantFloat(fieldIdx);
	f.constantStrValue = fieldConstantString(fieldIdx);
	if (opts_.decodeCustomAttrs)
	{
		MetadataToken parent{static_cast<uint8_t>(TableId::Field), fieldIdx};
		f.annotations = customAttributes(parent);
	}
	uint32_t nFm = tables_->rowCount(TableId::FieldMarshal);
	for (uint32_t i = 1; i <= nFm; ++i)
	{
		auto fm = tables_->fieldMarshal(i);
		if (fm.parent.table != static_cast<uint8_t>(TableId::Field) || fm.parent.index != fieldIdx) continue;
		BcAnnotation ma;
		ma.typeName = "System.Runtime.InteropServices.MarshalAsAttribute";
		auto blob = heaps_->blobs.get(fm.nativeType);
		if (!blob.empty())
		{
			static const char* digits = "0123456789ABCDEF";
			std::string hex;
			hex.reserve(blob.size() * 2);
			for (uint8_t b: blob)
			{
				hex.push_back(digits[b >> 4]);
				hex.push_back(digits[b & 0x0F]);
			}
			BcAnnotationValue ev;
			ev.kind = BcAnnotationValue::Kind::String;
			ev.stringValue = std::move(hex);
			ma.elements["NativeType"] = std::move(ev);
		}
		f.annotations.push_back(std::move(ma));
		break;
	}
	return f;
}

BcMethod CLIReader::buildMethod(uint32_t methodDefIdx, CliReadResult& result) const
{
	auto row = tables_->methodDef(methodDefIdx);
	BcMethod m;
	m.name = heaps_->strings.get(row.name);
	m.access = methodDefAccess(row.flags);
	m.isAbstract = (row.flags & MethodAttributes::Abstract) != 0;
	m.isNative = (row.flags & MethodAttributes::PInvokeImpl) != 0;
	m.isConstructor = (m.name == ".ctor");
	m.isStaticInit = (m.name == ".cctor");

	// Signature
	auto sigBlob = heaps_->blobs.get(row.signature);
	if (!sigBlob.empty())
	{
		auto sig = sigDecoder_->decodeMethod(sigBlob);
		if (sig)
		{
			m.descriptor = sigDecoder_->toBcFuncType(*sig);
			// Generic method params
			if (sig->isGeneric && opts_.decodeGenericParams)
			{
				auto gps = genericParams(methodDefIdx, true);
				for (const auto& gp: gps)
					m.typeParams.push_back(gp);
			}
		}
	}

	// Parameter names from Param table
	uint32_t numMethodDefs = tables_->rowCount(TableId::MethodDef);
	uint32_t paramEnd;
	if (methodDefIdx < numMethodDefs)
		paramEnd = tables_->methodDef(methodDefIdx + 1).paramList;
	else
		paramEnd = tables_->rowCount(TableId::Param) + 1;

	const auto paramRange = clampRowRange(row.paramList, paramEnd, tables_->rowCount(TableId::Param));
	for (uint32_t pi = paramRange.begin; pi < paramRange.end; ++pi)
	{
		auto prow = tables_->param(pi);
		if (prow.sequence == 0) continue; // sequence 0 = return type param
		std::string pname = heaps_->strings.get(prow.name);
		if (!pname.empty() && prow.sequence - 1 < m.descriptor.params.size())
			m.paramNames.push_back(pname);
		else
			m.paramNames.push_back("p" + std::to_string(prow.sequence));
		if (opts_.decodeCustomAttrs && prow.sequence > 0)
		{
			size_t pidx = static_cast<size_t>(prow.sequence - 1);
			if (m.paramAnnotations.size() <= pidx) m.paramAnnotations.resize(pidx + 1);
			MetadataToken parent{static_cast<uint8_t>(TableId::Param), pi};
			auto anns = customAttributes(parent);
			m.paramAnnotations[pidx].insert(m.paramAnnotations[pidx].end(), anns.begin(), anns.end());
		}
	}

	// CIL method body
	if (opts_.decodeCIL && row.rva != 0 && cilLifter_)
	{
		auto bodySpan = pe_.rvaToSpan(row.rva);
		if (!bodySpan.empty())
		{
			CILMethodHeader hdr;
			m.cfg = cilLifter_->lift(bodySpan, hdr);
			m.maxStack = hdr.maxStack;

			// Local variable types from LocalVarSig
			if (hdr.localVarSigTok != 0)
			{
				MetadataToken tok = MetadataToken::fromRaw(hdr.localVarSigTok);
				if (tok.table == static_cast<uint8_t>(TableId::StandAloneSig))
				{
					auto sasRow = tables_->standAloneSig(tok.index);
					auto lvBlob = heaps_->blobs.get(sasRow.signature);
					auto localSig = sigDecoder_->decodeLocalVar(lvBlob);
					if (localSig)
					{
						uint32_t localIdx = 0;
						for (const auto& lv: localSig->locals)
						{
							BcLocalVar var;
							var.index = localIdx++;
							var.name = "loc" + std::to_string(var.index);
							var.type = lv.base;
							m.locals.push_back(var);
						}
					}
				}
			}
		}
	}

	if (opts_.decodeCustomAttrs)
	{
		MetadataToken parent{static_cast<uint8_t>(TableId::MethodDef), methodDefIdx};
		m.annotations = customAttributes(parent);
	}

	if (row.flags & MethodAttributes::PInvokeImpl)
	{
		uint32_t nImpl = tables_->rowCount(TableId::ImplMap);
		for (uint32_t i = 1; i <= nImpl; ++i)
		{
			auto im = tables_->implMap(i);
			if (im.memberForwarded.table != static_cast<uint8_t>(TableId::MethodDef)
				|| im.memberForwarded.index != methodDefIdx)
				continue;
			m.access = m.access | BcAccess::Extern;
			BcAnnotation dll;
			dll.typeName = "System.Runtime.InteropServices.DllImportAttribute";
			std::string entry = heaps_->strings.get(im.importName);
			if (!entry.empty())
			{
				BcAnnotationValue ev;
				ev.kind = BcAnnotationValue::Kind::String;
				ev.stringValue = entry;
				dll.elements["EntryPoint"] = std::move(ev);
			}
			if (im.importScope != 0)
			{
				auto mr = tables_->moduleRef(im.importScope);
				std::string dllName = heaps_->strings.get(mr.name);
				if (!dllName.empty())
				{
					BcAnnotationValue dv;
					dv.kind = BcAnnotationValue::Kind::String;
					dv.stringValue = dllName;
					dll.elements["Value"] = std::move(dv);
				}
			}
			m.annotations.push_back(std::move(dll));
			break;
		}
	}

	return m;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

std::optional<BcType> CLIReader::resolveExtends(MetadataToken tok) const
{
	if (!tok.valid()) return std::nullopt;
	std::string name;
	if (tok.table == static_cast<uint8_t>(TableId::TypeDef))
		name = typeDefName(tok.index);
	else if (tok.table == static_cast<uint8_t>(TableId::TypeRef))
		name = typeRefName(tok.index);
	else if (tok.table == static_cast<uint8_t>(TableId::TypeSpec))
		return typeSpecType(tok.index);
	else
		return std::nullopt;

	// Don't include implicit System.Object inheritance
	if (name == "System.Object") return std::nullopt;
	return CliSigDecoder::clrNameToType(name);
}

std::vector<BcType> CLIReader::resolveInterfaces(uint32_t typeDefIdx) const
{
	std::vector<BcType> result;
	uint32_t n = tables_->rowCount(TableId::InterfaceImpl);
	for (uint32_t i = 1; i <= n; ++i)
	{
		auto row = tables_->interfaceImpl(i);
		if (row.clazz != typeDefIdx) continue;
		if (!row.interface_.valid()) continue;

		std::string name;
		if (row.interface_.table == static_cast<uint8_t>(TableId::TypeDef))
			name = typeDefName(row.interface_.index);
		else if (row.interface_.table == static_cast<uint8_t>(TableId::TypeRef))
			name = typeRefName(row.interface_.index);
		else
			continue;

		result.push_back(CliSigDecoder::clrNameToType(name));
	}
	return result;
}

std::vector<std::string> CLIReader::genericParams(uint32_t owner, bool isMethod) const
{
	std::vector<std::string> result;
	uint32_t n = tables_->rowCount(TableId::GenericParam);
	uint8_t expectedTable =
		isMethod ? static_cast<uint8_t>(TableId::MethodDef) : static_cast<uint8_t>(TableId::TypeDef);

	std::vector<std::pair<uint16_t, std::string>> params;
	for (uint32_t i = 1; i <= n; ++i)
	{
		auto row = tables_->genericParam(i);
		if (row.owner.table == expectedTable && row.owner.index == owner)
		{
			std::string pname = heaps_->strings.get(row.name);
			if (pname.empty()) pname = "T" + std::to_string(row.number);
			std::string bounds;
			uint32_t nc = tables_->rowCount(TableId::GenericParamConstraint);
			for (uint32_t c = 1; c <= nc; ++c)
			{
				auto cr = tables_->genericParamConstraint(c);
				if (cr.owner != i) continue;
				std::string tn;
				if (cr.constraint.table == static_cast<uint8_t>(TableId::TypeDef))
					tn = typeDefName(cr.constraint.index);
				else if (cr.constraint.table == static_cast<uint8_t>(TableId::TypeRef))
					tn = typeRefName(cr.constraint.index);
				else if (cr.constraint.table == static_cast<uint8_t>(TableId::TypeSpec))
					tn = typeSpecType(cr.constraint.index).toString();
				if (tn.empty() || tn == "<unknown>") continue;
				if (!bounds.empty()) bounds += ", ";
				bounds += tn;
			}
			if (!bounds.empty()) pname += " : " + bounds;
			params.emplace_back(row.number, pname);
		}
	}

	std::sort(params.begin(), params.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	for (const auto& [num, name]: params)
		result.push_back(name);

	return result;
}

std::vector<BcAnnotation> CLIReader::customAttributes(MetadataToken parent) const
{
	std::vector<BcAnnotation> result;
	if (!opts_.decodeCustomAttrs) return result;

	uint32_t n = tables_->rowCount(TableId::CustomAttribute);
	for (uint32_t i = 1; i <= n; ++i)
	{
		auto row = tables_->customAttribute(i);
		// Check if this attribute is attached to our parent
		// The parent is encoded as a HasCustomAttribute coded token
		// We stored it as table/index in fields[0]/[1]
		if (row.parent.table != parent.table || row.parent.index != parent.index) continue;

		BcAnnotation ann;
		// Resolve type name from CustomAttributeType
		if (row.type.valid())
		{
			if (row.type.table == static_cast<uint8_t>(TableId::MethodDef))
			{
				// Constructor MethodDef → find owning TypeDef
				ann.typeName = "<ctor>";
			}
			else if (row.type.table == static_cast<uint8_t>(TableId::MemberRef))
			{
				auto mref = tables_->memberRef(row.type.index);
				if (mref.clazz.table == static_cast<uint8_t>(TableId::TypeRef))
					ann.typeName = typeRefName(mref.clazz.index);
				else
					ann.typeName = "<attr>";
			}
		}

		result.push_back(std::move(ann));
	}
	return result;
}

// ─── Access flag mapping ──────────────────────────────────────────────────────

BcAccess CLIReader::typeDefAccess(uint32_t flags)
{
	uint32_t vis = flags & TypeAttributes::VisibilityMask;
	BcAccess a = BcAccess::Internal;
	switch (vis)
	{
	case TypeAttributes::Public:
	case TypeAttributes::NestedPublic: a = BcAccess::Public; break;
	case TypeAttributes::NestedFamily:
	case TypeAttributes::NestedFamORAssem: a = BcAccess::Protected; break;
	case TypeAttributes::NestedPrivate: a = BcAccess::Private; break;
	case TypeAttributes::NestedAssembly:
	case TypeAttributes::NestedFamANDAssem: a = BcAccess::Internal; break;
	default: a = BcAccess::Internal; break;
	}
	if (flags & TypeAttributes::Abstract) a = a | BcAccess::Abstract;
	if (flags & TypeAttributes::Sealed) a = a | BcAccess::Sealed;
	return a;
}

BcAccess CLIReader::methodDefAccess(uint16_t flags)
{
	uint16_t acc = flags & MethodAttributes::MemberAccessMask;
	BcAccess a = BcAccess::Private;
	switch (acc)
	{
	case MethodAttributes::Public: a = BcAccess::Public; break;
	case MethodAttributes::Family:
	case MethodAttributes::FamORAssem: a = BcAccess::Protected; break;
	case MethodAttributes::Private: a = BcAccess::Private; break;
	case MethodAttributes::Assem: a = BcAccess::Internal; break;
	default: a = BcAccess::Private; break;
	}
	if (flags & MethodAttributes::Static) a = a | BcAccess::Static;
	if (flags & MethodAttributes::Final) a = a | BcAccess::Final;
	if (flags & MethodAttributes::Virtual)
	{
		if (flags & MethodAttributes::NewSlot)
			a = a | BcAccess::Virtual;
		else
		{
			a = a | BcAccess::Override;
			if (flags & MethodAttributes::Final) a = a | BcAccess::Sealed;
		}
	}
	if (flags & MethodAttributes::Abstract) a = a | BcAccess::Abstract;
	if (flags & MethodAttributes::PInvokeImpl) a = a | BcAccess::Extern;
	return a;
}

BcAccess CLIReader::fieldAccess(uint16_t flags)
{
	uint16_t acc = flags & 0x0007; // FieldAttributes MemberAccessMask
	BcAccess a = BcAccess::Private;
	switch (acc)
	{
	case 0x0006: a = BcAccess::Public; break;
	case 0x0004: a = BcAccess::Protected; break;
	case 0x0001: a = BcAccess::Private; break;
	case 0x0003: a = BcAccess::Internal; break;
	default: a = BcAccess::Private; break;
	}
	if (flags & 0x0010) a = a | BcAccess::Static;   // FieldAttributes.Static
	if (flags & 0x0020) a = a | BcAccess::Readonly; // FieldAttributes.InitOnly
	if (flags & 0x0020) a = a | BcAccess::Final;
	return a;
}

bool CLIReader::isNested(uint32_t typeDefIdx) const
{
	for (const auto& [n, e]: nestedMap_)
		if (n == typeDefIdx) return true;
	return false;
}

uint32_t CLIReader::enclosingClass(uint32_t typeDefIdx) const
{
	for (const auto& [n, e]: nestedMap_)
		if (n == typeDefIdx) return e;
	return 0;
}

static std::optional<ConstantRow> findFieldConstantRow(const MetadataTables& tables, uint32_t fieldIdx)
{
	uint32_t n = tables.rowCount(TableId::Constant);
	for (uint32_t i = 1; i <= n; ++i)
	{
		auto row = tables.constant(i);
		if (row.parent.table == static_cast<uint8_t>(TableId::Field) && row.parent.index == fieldIdx) return row;
	}
	return std::nullopt;
}

// Widths of the fixed-size #Blob constant encodings, ECMA-335 II.23.1.16
// (ELEMENT_TYPE_*). They are the byte counts the element type itself names, not
// sizeof() of whatever C++ type happens to hold the result.
static constexpr size_t kU1Bytes = 1; ///< BOOLEAN, I1, U1
static constexpr size_t kU2Bytes = 2; ///< CHAR, I2, U2
static constexpr size_t kU4Bytes = 4; ///< I4, U4
static constexpr size_t kU8Bytes = 8; ///< I8, U8

/// Bytes one UTF-16 code unit occupies in a #Blob string constant
/// (ECMA-335 II.23.1.16 ELEMENT_TYPE_STRING: "a UTF-16 string").
static constexpr size_t kUtf16BytesPerUnit = 2;

/// The low @p n bytes of @p b, little-endian, read as a two's-complement value.
///
/// Was a hand-rolled accumulate whose two halves disagreed about the bound: the
/// loop stopped at `i < n && i < b.size()`, and the sign test on the next line
/// read `b[n - 1]` with no reference to b.size() at all. ESBMC, with the blob
/// malloc'd at exactly its symbolic length: blen = 1, n = 4 -- the loop reads
/// b[0] and stops, then the sign test reads b[3], two bytes past the end of a
/// one-byte heap object ("dereference failure: array bounds violated: heap
/// object"). Every call site below used to pre-check `blob.size() >= n`, which
/// is why nothing in this tree reached that read; the trap was that the guard
/// lived at the call sites rather than in the function that needs it, so the
/// next `case` added to the switch inherited the hazard rather than the check.
/// Those pre-checks are gone now: the arms below call straight in and this
/// function refuses, so the guard cannot be forgotten and the truncated-blob
/// tests exercise it rather than the call site's copy of it.
///
/// byteorder::readLE refuses the whole read unless n is in 1..8 AND
/// bounds::rangeFits(0, b.size(), n) holds, so there is one bound and it is the
/// one the read uses; byteorder::signExtendFrom then does the extension in
/// unsigned arithmetic (the `~0ull << (8 * n)` here was fine only because n < 8
/// was tested first -- at n = 8 that shift is undefined).
static std::optional<int64_t> readSignedLE(std::span<const uint8_t> b, size_t n)
{
	uint64_t u = 0;
	if (n > utils::byteorder::kMaxBytes) return std::nullopt;
	if (!utils::byteorder::readLE(b.data(), b.size(), 0, static_cast<unsigned>(n), u)) return std::nullopt;
	return utils::byteorder::signExtendFrom(u, static_cast<unsigned>(n) * utils::byteorder::kBitsPerByte);
}

/// The low @p n bytes of @p b, little-endian, read as an unsigned value.
///
/// The U2 arm below was `blob[0] | (blob[1] << 8)` and the U4 arm was a
/// `std::memcpy` into a uint32_t. The memcpy is host-endian: ECMA-335 II.22.9
/// says a #Blob constant is stored little-endian, so on a big-endian host it
/// returned the byte-swapped constant. Both now go through the same kernel read
/// as the signed arms, so the width, the bound and the byte order are stated
/// once each.
///
/// Which arms call this and which call readSignedLE is the whole of the
/// signed/unsigned question, so it is decided per ElementType at the switch and
/// nowhere else. byteorder::zeroExtendFrom is the counterpart the kernel keeps
/// beside signExtendFrom precisely so that the choice has to be made by name.
static std::optional<int64_t> readUnsignedLE(std::span<const uint8_t> b, size_t n)
{
	uint64_t u = 0;
	if (n > utils::byteorder::kMaxBytes) return std::nullopt;
	if (!utils::byteorder::readLE(b.data(), b.size(), 0, static_cast<unsigned>(n), u)) return std::nullopt;
	return static_cast<int64_t>(
		utils::byteorder::zeroExtendFrom(u, static_cast<unsigned>(n) * utils::byteorder::kBitsPerByte));
}

// Every arm below is now "this many bytes, this extension direction", and
// nothing else. Two things moved to get there.
//
// The BOUND. Each arm used to open with its own `blob.size() >= n` before
// calling, so the width was written twice per arm and the check was the call
// site's rather than the read's. readSignedLE/readUnsignedLE refuse through
// byteorder::readLE, whose rangeFits(0, size, n) is the same test against the
// same n it then reads -- so a truncated #Blob yields no value because the read
// declined it, and a `case` added here inherits the check instead of having to
// remember it. That is the difference the readSignedLE comment above describes:
// the ESBMC witness for the old hand-rolled body (a blob shorter than n, whose
// sign test then indexed b[n - 1] past the end) was unreachable only because
// three call sites happened to guard it.
//
// The EXTENSION DIRECTION. It is now stated by which function the arm calls,
// once per arm, rather than inferred from which other type the arm shares a
// label with.
std::optional<int64_t> CLIReader::fieldConstantInt(uint32_t fieldIdx) const
{
	if (!tables_ || !heaps_) return std::nullopt;
	auto row = findFieldConstantRow(*tables_, fieldIdx);
	if (!row) return std::nullopt;
	auto blob = heaps_->blobs.get(row->value);
	switch (static_cast<ElementType>(row->type))
	{
	case ElementType::Boolean:
		// ECMA-335 I.12.1 lists bool with the unsigned built-in types, and
		// II.23.1.16 gives it one byte: false is the zero byte and true is any
		// other. It used to share the I1 arm, which sign-extends, so a #Blob
		// byte of 0xFF -- which a hostile or damaged assembly supplies freely,
		// even though a C# compiler emits only 0x00 and 0x01 -- reported the
		// constant as -1. A bool has no negative value to report.
		return readUnsignedLE(blob, kU1Bytes);
	case ElementType::U1:
		// ECMA-335 II.23.1.16 kElementTypeU1: one byte, unsigned.
		return readUnsignedLE(blob, kU1Bytes);
	case ElementType::I1:
		// ECMA-335 II.23.1.16 kElementTypeI1: one byte, signed.
		return readSignedLE(blob, kU1Bytes);
	case ElementType::I2:
		// ECMA-335 II.23.1.16 kElementTypeI2: two little-endian bytes, signed.
		return readSignedLE(blob, kU2Bytes);
	case ElementType::Char:
	case ElementType::U2:
		// ECMA-335 II.23.1.16 kElementTypeU2: two little-endian bytes,
		// unsigned; ELEMENT_TYPE_CHAR is a UTF-16 code unit, which is also two
		// bytes and also unsigned. Char used to share the I2 arm above, which
		// sign-extends: the code unit U+FFFF (#Blob bytes FF FF) decoded as -1
		// rather than 65535, and every unit at or above U+8000 came out
		// negative. That is the bug class byteorder::zeroExtendFrom exists to
		// settle -- its own header names dex_class_parser.cpp:92 routing the
		// DEX VALUE_CHAR, unsigned by that specification too, through the
		// sign-extending path.
		return readUnsignedLE(blob, kU2Bytes);
	case ElementType::I4:
		// ECMA-335 II.23.1.16 kElementTypeI4: four little-endian bytes, signed.
		return readSignedLE(blob, kU4Bytes);
	case ElementType::U4:
		// ECMA-335 II.23.1.16 kElementTypeU4: four little-endian bytes,
		// unsigned.
		return readUnsignedLE(blob, kU4Bytes);
	case ElementType::I8:
	case ElementType::U8:
		// ECMA-335 II.23.1.16: I8 and U8 are eight little-endian bytes. They
		// share an arm because at eight bytes the two extensions agree: there
		// is nothing above bit 63 to fill, so signExtendFrom and zeroExtendFrom
		// both hand back the bits unchanged and the int64_t carries the same
		// pattern either way. A U8 above INT64_MAX therefore still reads
		// negative here -- that is the return type's limit, not this arm's.
		return readSignedLE(blob, kU8Bytes);
	default: return std::nullopt;
	}
}

/// The IEEE-754 value @p b holds in its first sizeof(Float) bytes, read
/// little-endian.
///
/// Both float arms below were `std::memcpy(&f, blob.data(), sizeof f)` straight
/// out of the blob. That is a host-endian read of a little-endian datum:
/// ECMA-335 II.22.9 stores a Constant's value in the #Blob little-endian, so on
/// a big-endian host the four bytes of 1.0f -- 00 00 80 3F -- were reassembled
/// as the pattern 0x0000803F, which is a denormal of about 4.6e-41, rather than
/// as 0x3F800000. It is the same construct, for the same reason, as the memcpy
/// the U4 integer arm above no longer uses; the float half of this decoder was
/// simply left behind when the integer half was routed.
///
/// byteorder::readLE assembles the pattern with explicit per-byte shifts, so
/// the answer does not depend on the host, and it refuses unless the whole
/// range is inside the blob -- so the width is stated once, here, rather than
/// as a `blob.size() >= n` at each call site.
///
/// The memcpy that remains is not a byte-order decision: it copies an integer
/// the host already holds in host order into a float of the same width, which
/// is the C++17 spelling of std::bit_cast. Bits is the exact-width unsigned
/// type, never the 64-bit accumulator, because copying sizeof(Float) bytes out
/// of a uint64_t would take the high half on a big-endian host and reintroduce
/// exactly the bug this removes.
template <typename Float>
static std::optional<Float> readIeeeLE(std::span<const uint8_t> b)
{
	static_assert(
		std::numeric_limits<Float>::is_iec559,
		"a #Blob R4/R8 constant is an IEEE-754 pattern, so the host "
		"type it is copied into has to be one too");
	static_assert(
		sizeof(Float) == kU4Bytes || sizeof(Float) == kU8Bytes, "ECMA-335 II.23.1.16 gives R4 four bytes and R8 eight");
	using Bits = std::conditional_t<sizeof(Float) == kU4Bytes, uint32_t, uint64_t>;
	static_assert(sizeof(Bits) == sizeof(Float), "bit pattern must be as wide");

	uint64_t acc = 0;
	if (!utils::byteorder::readLE(b.data(), b.size(), 0, static_cast<unsigned>(sizeof(Float)), acc))
		return std::nullopt;
	const Bits bits = static_cast<Bits>(acc);
	Float v = 0;
	std::memcpy(&v, &bits, sizeof(Float));
	return v;
}

std::optional<double> CLIReader::fieldConstantFloat(uint32_t fieldIdx) const
{
	if (!tables_ || !heaps_) return std::nullopt;
	auto row = findFieldConstantRow(*tables_, fieldIdx);
	if (!row) return std::nullopt;
	auto blob = heaps_->blobs.get(row->value);
	switch (static_cast<ElementType>(row->type))
	{
	case ElementType::R4:
		// ECMA-335 II.23.1.16 kElementTypeR4: four little-endian bytes.
		if (auto f = readIeeeLE<float>(blob)) return static_cast<double>(*f);
		break;
	case ElementType::R8:
		// ECMA-335 II.23.1.16 kElementTypeR8: eight little-endian bytes.
		if (auto d = readIeeeLE<double>(blob)) return *d;
		break;
	default: break;
	}
	return std::nullopt;
}

std::optional<std::string> CLIReader::fieldConstantString(uint32_t fieldIdx) const
{
	if (!tables_ || !heaps_) return std::nullopt;
	auto row = findFieldConstantRow(*tables_, fieldIdx);
	if (!row || static_cast<ElementType>(row->type) != ElementType::String) return std::nullopt;
	auto blob = heaps_->blobs.get(row->value);
	if (blob.empty()) return std::string{};

	// The loop this replaces took the three-byte `else` arm for every code unit
	// at or above 0x800, surrogates included. A surrogate is not a scalar value
	// and has no UTF-8 encoding: ESBMC's witness cu = 0xD800 came out as
	// ED A0 80, which no UTF-8 decoder accepts. Worse, a surrogate PAIR -- the
	// entire reason UTF-16 has surrogates -- came out as two such ill-formed
	// sequences rather than as the one character it denotes, so the .NET string
	// constant "\U0001F600" (units D83D DE00, the bytes 3D D8 00 DE in the
	// #Blob) decompiled as the six bytes ED A0 BD ED B8 80 instead of the four
	// bytes F0 9F 98 80.
	//
	// txt::utf16leToUtf8 combines a well-formed pair, replaces a lone surrogate
	// with U+FFFD, and is proved never to emit an ED A0..BF lead
	// (proof_encode_utf8_is_well_formed, proof_utf16_never_emits_a_surrogate).
	// It is also proved byte-identical to this loop on every input carrying no
	// surrogate unit (proof_the_kernel_matches_dotnet_off_the_surrogates), so
	// routing here changes the output only where it was already ill-formed.
	//
	// The trailing odd byte is dropped exactly as `i + 1 < blob.size()` dropped
	// it: the kernel's unit count is `inBytes >> 1`, so the highest index it
	// touches is 2*units - 1 <= blob.size() - 1.
	const size_t units = blob.size() / kUtf16BytesPerUnit;
	// utf8CapacityForUtf16 checks the 3-bytes-per-unit product with
	// bounds::mulFits and yields 0 when it would wrap, which is the sizing step
	// the hand-rolled loop never had to do because it push_back'd instead.
	const size_t cap = utils::txt::utf8CapacityForUtf16(units);
	if (cap == 0) return std::string{};

	std::string out(cap, '\0');
	const size_t written = utils::txt::utf16leToUtf8(blob.data(), blob.size(), out.data(), out.size());
	out.resize(written);
	return out;
}

} // namespace cli_parser
} // namespace retdec
