/**
 * @file src/pyc_parser/pyc_reader.cpp
 * @brief Top-level Python .pyc parser → BcModule.
 */

#include <memory>
#include "retdec/pyc_parser/pyc_reader.h"
#include "retdec/pyc_parser/py_marshal.h"
#include "retdec/pyc_parser/py_opcodes.h"

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_type.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace retdec {
namespace pyc_parser {

using namespace bc_module;

// ─── Opcode name → BcOpcode mapping ─────────────────────────────────────────

static const std::unordered_map<std::string, BcOpcode> kPyOpcodeMap = {
    {"POP_TOP",              BcOpcode::PYTHON_POP_TOP},
    {"ROT_TWO",              BcOpcode::PYTHON_ROT_TWO},
    {"ROT_THREE",            BcOpcode::PYTHON_ROT_THREE},
    {"ROT_FOUR",             BcOpcode::PYTHON_ROT_FOUR},
    {"ROT_N",                BcOpcode::PYTHON_ROT_N},
    {"DUP_TOP",              BcOpcode::PYTHON_DUP_TOP},
    {"DUP_TOP_TWO",          BcOpcode::PYTHON_DUP_TOP_TWO},
    {"COPY",                 BcOpcode::PYTHON_COPY},
    {"PUSH_NULL",            BcOpcode::PYTHON_PUSH_NULL},
    {"NOP",                  BcOpcode::PYTHON_NOP},
    {"RESUME",               BcOpcode::PYTHON_RESUME},
    {"CACHE",                BcOpcode::PYTHON_CACHE},
    {"UNARY_POSITIVE",       BcOpcode::PYTHON_UNARY_POSITIVE},
    {"UNARY_NEGATIVE",       BcOpcode::PYTHON_UNARY_NEGATIVE},
    {"UNARY_NOT",            BcOpcode::PYTHON_UNARY_NOT},
    {"UNARY_INVERT",         BcOpcode::PYTHON_UNARY_INVERT},
    {"BINARY_ADD",           BcOpcode::PYTHON_BINARY_ADD},
    {"BINARY_SUBTRACT",      BcOpcode::PYTHON_BINARY_SUBTRACT},
    {"BINARY_MULTIPLY",      BcOpcode::PYTHON_BINARY_MULTIPLY},
    {"BINARY_FLOOR_DIVIDE",  BcOpcode::PYTHON_BINARY_FLOOR_DIVIDE},
    {"BINARY_TRUE_DIVIDE",   BcOpcode::PYTHON_BINARY_TRUE_DIVIDE},
    {"BINARY_MODULO",        BcOpcode::PYTHON_BINARY_MODULO},
    {"BINARY_POWER",         BcOpcode::PYTHON_BINARY_POWER},
    {"BINARY_LSHIFT",        BcOpcode::PYTHON_BINARY_LSHIFT},
    {"BINARY_RSHIFT",        BcOpcode::PYTHON_BINARY_RSHIFT},
    {"BINARY_AND",           BcOpcode::PYTHON_BINARY_AND},
    {"BINARY_OR",            BcOpcode::PYTHON_BINARY_OR},
    {"BINARY_XOR",           BcOpcode::PYTHON_BINARY_XOR},
    {"BINARY_MATRIX_MULTIPLY", BcOpcode::PYTHON_BINARY_MATRIX_MULTIPLY},
    {"BINARY_SUBSCR",        BcOpcode::PYTHON_BINARY_SUBSCR},
    {"BINARY_OP",            BcOpcode::PYTHON_BINARY_OP},
    {"INPLACE_ADD",          BcOpcode::PYTHON_INPLACE_ADD},
    {"INPLACE_SUBTRACT",     BcOpcode::PYTHON_INPLACE_SUBTRACT},
    {"INPLACE_MULTIPLY",     BcOpcode::PYTHON_INPLACE_MULTIPLY},
    {"INPLACE_FLOOR_DIVIDE", BcOpcode::PYTHON_INPLACE_FLOOR_DIVIDE},
    {"INPLACE_TRUE_DIVIDE",  BcOpcode::PYTHON_INPLACE_TRUE_DIVIDE},
    {"INPLACE_MODULO",       BcOpcode::PYTHON_INPLACE_MODULO},
    {"INPLACE_POWER",        BcOpcode::PYTHON_INPLACE_POWER},
    {"INPLACE_LSHIFT",       BcOpcode::PYTHON_INPLACE_LSHIFT},
    {"INPLACE_RSHIFT",       BcOpcode::PYTHON_INPLACE_RSHIFT},
    {"INPLACE_AND",          BcOpcode::PYTHON_INPLACE_AND},
    {"INPLACE_OR",           BcOpcode::PYTHON_INPLACE_OR},
    {"INPLACE_XOR",          BcOpcode::PYTHON_INPLACE_XOR},
    {"INPLACE_MATRIX_MULTIPLY", BcOpcode::PYTHON_INPLACE_MATRIX_MULTIPLY},
    {"STORE_SUBSCR",         BcOpcode::PYTHON_STORE_SUBSCR},
    {"DELETE_SUBSCR",        BcOpcode::PYTHON_DELETE_SUBSCR},
    {"LOAD_CONST",           BcOpcode::PYTHON_LOAD_CONST},
    {"LOAD_FAST",            BcOpcode::PYTHON_LOAD_FAST},
    {"STORE_FAST",           BcOpcode::PYTHON_STORE_FAST},
    {"DELETE_FAST",          BcOpcode::PYTHON_DELETE_FAST},
    {"LOAD_FAST_CHECK",      BcOpcode::PYTHON_LOAD_FAST_CHECK},
    {"LOAD_FAST_AND_CLEAR",  BcOpcode::PYTHON_LOAD_FAST_AND_CLEAR},
    {"LOAD_NAME",            BcOpcode::PYTHON_LOAD_NAME},
    {"STORE_NAME",           BcOpcode::PYTHON_STORE_NAME},
    {"DELETE_NAME",          BcOpcode::PYTHON_DELETE_NAME},
    {"LOAD_GLOBAL",          BcOpcode::PYTHON_LOAD_GLOBAL},
    {"STORE_GLOBAL",         BcOpcode::PYTHON_STORE_GLOBAL},
    {"DELETE_GLOBAL",        BcOpcode::PYTHON_DELETE_GLOBAL},
    {"LOAD_ATTR",            BcOpcode::PYTHON_LOAD_ATTR},
    {"STORE_ATTR",           BcOpcode::PYTHON_STORE_ATTR},
    {"DELETE_ATTR",          BcOpcode::PYTHON_DELETE_ATTR},
    {"LOAD_DEREF",           BcOpcode::PYTHON_LOAD_DEREF},
    {"STORE_DEREF",          BcOpcode::PYTHON_STORE_DEREF},
    {"DELETE_DEREF",         BcOpcode::PYTHON_DELETE_DEREF},
    {"LOAD_CLOSURE",         BcOpcode::PYTHON_LOAD_CLOSURE},
    {"LOAD_CLASSDEREF",      BcOpcode::PYTHON_LOAD_CLASSDEREF},
    {"COPY_FREE_VARS",       BcOpcode::PYTHON_COPY_FREE_VARS},
    {"COMPARE_OP",           BcOpcode::PYTHON_COMPARE_OP},
    {"IS_OP",                BcOpcode::PYTHON_IS_OP},
    {"CONTAINS_OP",          BcOpcode::PYTHON_CONTAINS_OP},
    {"JUMP_FORWARD",         BcOpcode::PYTHON_JUMP_FORWARD},
    {"JUMP_ABSOLUTE",        BcOpcode::PYTHON_JUMP_ABSOLUTE},
    {"JUMP_BACKWARD",        BcOpcode::PYTHON_JUMP_BACKWARD},
    {"JUMP_BACKWARD_NO_INTERRUPT", BcOpcode::PYTHON_JUMP_BACKWARD_NO_INTERRUPT},
    {"POP_JUMP_IF_TRUE",     BcOpcode::PYTHON_POP_JUMP_IF_TRUE},
    {"POP_JUMP_IF_FALSE",    BcOpcode::PYTHON_POP_JUMP_IF_FALSE},
    {"POP_JUMP_IF_NONE",     BcOpcode::PYTHON_POP_JUMP_IF_NONE},
    {"POP_JUMP_IF_NOT_NONE", BcOpcode::PYTHON_POP_JUMP_IF_NOT_NONE},
    {"POP_JUMP_FORWARD_IF_TRUE",  BcOpcode::PYTHON_POP_JUMP_FORWARD_IF_TRUE},
    {"POP_JUMP_FORWARD_IF_FALSE", BcOpcode::PYTHON_POP_JUMP_FORWARD_IF_FALSE},
    {"POP_JUMP_FORWARD_IF_NONE",  BcOpcode::PYTHON_POP_JUMP_FORWARD_IF_NONE},
    {"POP_JUMP_FORWARD_IF_NOT_NONE", BcOpcode::PYTHON_POP_JUMP_FORWARD_IF_NOT_NONE},
    {"POP_JUMP_BACKWARD_IF_TRUE",  BcOpcode::PYTHON_POP_JUMP_BACKWARD_IF_TRUE},
    {"POP_JUMP_BACKWARD_IF_FALSE", BcOpcode::PYTHON_POP_JUMP_BACKWARD_IF_FALSE},
    {"POP_JUMP_BACKWARD_IF_NONE",  BcOpcode::PYTHON_POP_JUMP_BACKWARD_IF_NONE},
    {"POP_JUMP_BACKWARD_IF_NOT_NONE", BcOpcode::PYTHON_POP_JUMP_BACKWARD_IF_NOT_NONE},
    {"JUMP_IF_TRUE_OR_POP",  BcOpcode::PYTHON_JUMP_IF_TRUE_OR_POP},
    {"JUMP_IF_FALSE_OR_POP", BcOpcode::PYTHON_JUMP_IF_FALSE_OR_POP},
    {"FOR_ITER",             BcOpcode::PYTHON_FOR_ITER},
    {"SEND",                 BcOpcode::PYTHON_SEND},
    {"RETURN_VALUE",         BcOpcode::PYTHON_RETURN_VALUE},
    {"RETURN_CONST",         BcOpcode::PYTHON_RETURN_CONST},
    {"RETURN_GENERATOR",     BcOpcode::PYTHON_RETURN_GENERATOR},
    {"RAISE_VARARGS",        BcOpcode::PYTHON_RAISE_VARARGS},
    {"RERAISE",              BcOpcode::PYTHON_RERAISE},
    {"CALL_FUNCTION",        BcOpcode::PYTHON_CALL_FUNCTION},
    {"CALL_FUNCTION_KW",     BcOpcode::PYTHON_CALL_FUNCTION_KW},
    {"CALL_FUNCTION_EX",     BcOpcode::PYTHON_CALL_FUNCTION_EX},
    {"CALL_METHOD",          BcOpcode::PYTHON_CALL_METHOD},
    {"LOAD_METHOD",          BcOpcode::PYTHON_LOAD_METHOD},
    {"CALL",                 BcOpcode::PYTHON_CALL},
    {"PRECALL",              BcOpcode::PYTHON_PRECALL},
    {"KW_NAMES",             BcOpcode::PYTHON_KW_NAMES},
    {"PUSH_EXC_INFO",        BcOpcode::PYTHON_PUSH_EXC_INFO},
    {"CALL_INTRINSIC_1",     BcOpcode::PYTHON_CALL_INTRINSIC_1},
    {"CALL_INTRINSIC_2",     BcOpcode::PYTHON_CALL_INTRINSIC_2},
    {"MAKE_FUNCTION",        BcOpcode::PYTHON_MAKE_FUNCTION},
    {"BUILD_SLICE",          BcOpcode::PYTHON_BUILD_SLICE},
    {"BUILD_TUPLE",          BcOpcode::PYTHON_BUILD_TUPLE},
    {"BUILD_LIST",           BcOpcode::PYTHON_BUILD_LIST},
    {"BUILD_SET",            BcOpcode::PYTHON_BUILD_SET},
    {"BUILD_MAP",            BcOpcode::PYTHON_BUILD_MAP},
    {"BUILD_CONST_KEY_MAP",  BcOpcode::PYTHON_BUILD_CONST_KEY_MAP},
    {"BUILD_STRING",         BcOpcode::PYTHON_BUILD_STRING},
    {"LIST_APPEND",          BcOpcode::PYTHON_LIST_APPEND},
    {"SET_ADD",              BcOpcode::PYTHON_SET_ADD},
    {"MAP_ADD",              BcOpcode::PYTHON_MAP_ADD},
    {"LIST_EXTEND",          BcOpcode::PYTHON_LIST_EXTEND},
    {"SET_UPDATE",           BcOpcode::PYTHON_SET_UPDATE},
    {"DICT_MERGE",           BcOpcode::PYTHON_DICT_MERGE},
    {"DICT_UPDATE",          BcOpcode::PYTHON_DICT_UPDATE},
    {"UNPACK_SEQUENCE",      BcOpcode::PYTHON_UNPACK_SEQUENCE},
    {"UNPACK_EX",            BcOpcode::PYTHON_UNPACK_EX},
    {"IMPORT_NAME",          BcOpcode::PYTHON_IMPORT_NAME},
    {"IMPORT_FROM",          BcOpcode::PYTHON_IMPORT_FROM},
    {"IMPORT_STAR",          BcOpcode::PYTHON_IMPORT_STAR},
    {"POP_EXCEPT",           BcOpcode::PYTHON_POP_EXCEPT},
    {"POP_BLOCK",            BcOpcode::PYTHON_POP_BLOCK},
    {"END_FINALLY",          BcOpcode::PYTHON_END_FINALLY},
    {"SETUP_WITH",           BcOpcode::PYTHON_SETUP_WITH},
    {"BEFORE_WITH",          BcOpcode::PYTHON_BEFORE_WITH},
    {"WITH_EXCEPT_START",    BcOpcode::PYTHON_WITH_EXCEPT_START},
    {"BEFORE_ASYNC_WITH",    BcOpcode::PYTHON_BEFORE_ASYNC_WITH},
    {"END_ASYNC_FOR",        BcOpcode::PYTHON_END_ASYNC_FOR},
    {"CHECK_EXC_MATCH",      BcOpcode::PYTHON_CHECK_EXC_MATCH},
    {"CLEANUP_THROW",        BcOpcode::PYTHON_CLEANUP_THROW},
    {"STOPITERATION_ERROR",  BcOpcode::PYTHON_STOPITERATION_ERROR},
    {"YIELD_VALUE",          BcOpcode::PYTHON_YIELD_VALUE},
    {"YIELD_FROM",           BcOpcode::PYTHON_YIELD_FROM},
    {"GET_ITER",             BcOpcode::PYTHON_GET_ITER},
    {"GET_YIELD_FROM_ITER",  BcOpcode::PYTHON_GET_YIELD_FROM_ITER},
    {"GET_AWAITABLE",        BcOpcode::PYTHON_GET_AWAITABLE},
    {"GET_AITER",            BcOpcode::PYTHON_GET_AITER},
    {"GET_ANEXT",            BcOpcode::PYTHON_GET_ANEXT},
    {"ASYNC_GEN_WRAP",       BcOpcode::PYTHON_ASYNC_GEN_WRAP},
    {"MATCH_CLASS",          BcOpcode::PYTHON_MATCH_CLASS},
    {"MATCH_SEQUENCE",       BcOpcode::PYTHON_MATCH_SEQUENCE},
    {"MATCH_MAPPING",        BcOpcode::PYTHON_MATCH_MAPPING},
    {"FORMAT_VALUE",         BcOpcode::PYTHON_FORMAT_VALUE},
    {"PRINT_EXPR",           BcOpcode::PYTHON_PRINT_EXPR},
    {"LOAD_BUILD_CLASS",     BcOpcode::PYTHON_LOAD_BUILD_CLASS},
    {"SETUP_ANNOTATIONS",    BcOpcode::PYTHON_SETUP_ANNOTATIONS},
    {"LOAD_ASSERTION_ERROR", BcOpcode::PYTHON_LOAD_ASSERTION_ERROR},
    {"EXTENDED_ARG",         BcOpcode::PYTHON_EXTENDED_ARG},
};

// ─── PycReader ────────────────────────────────────────────────────────────────

PycReader::PycReader(PycReadOptions opts) : opts_(std::move(opts)) {}

// ─── readFile ─────────────────────────────────────────────────────────────────

PycReadResult PycReader::readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        PycReadResult r;
        r.error = "Cannot open file: " + path;
        return r;
    }
    std::vector<uint8_t> buf(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>{});
    return read(buf.data(), buf.size(), path);
}

// ─── read ─────────────────────────────────────────────────────────────────────

PycReadResult PycReader::read(const uint8_t* data, size_t size,
                               const std::string& srcPath) {
    PycReadResult result;
    result.module = BcModule("", SourceLang::Python);

    if (!data || size < kPycHeaderSize) {
        result.error = "File too small to be a valid .pyc (need " +
                       std::to_string(kPycHeaderSize) + " bytes)";
        return result;
    }

    // 1. Read header
    uint32_t rawMagic  = static_cast<uint32_t>(data[0])
                       | (static_cast<uint32_t>(data[1]) << 8)
                       | (static_cast<uint32_t>(data[2]) << 16)
                       | (static_cast<uint32_t>(data[3]) << 24);
    uint32_t bitField  = static_cast<uint32_t>(data[4])
                       | (static_cast<uint32_t>(data[5]) << 8)
                       | (static_cast<uint32_t>(data[6]) << 16)
                       | (static_cast<uint32_t>(data[7]) << 24);

    // Remaining 8 bytes: mtime+size or source_hash
    // Not needed for decompilation; we skip them.

    auto verOpt = detectVersion(rawMagic);
    if (!verOpt) {
        std::ostringstream ss;
        ss << "Unknown Python .pyc magic: 0x" << std::hex << rawMagic;
        result.error = ss.str();
        return result;
    }
    result.version = *verOpt;

    // 2. Parse the marshal stream after the 16-byte header
    const uint8_t* marshalData = data + kPycHeaderSize;
    size_t         marshalSize = size - kPycHeaderSize;

    MarshalReader mr(marshalData, marshalSize, result.version);
    auto rootObj = mr.readObject();

    if (mr.hasError()) {
        result.error = "Marshal parse error: " + mr.error();
        return result;
    }
    if (!rootObj || !rootObj->isCode()) {
        result.error = "Root marshal object is not a code object";
        return result;
    }

    const auto& rootCode = rootObj->asCode();
    if (!rootCode) {
        result.error = "Root code object is null";
        return result;
    }

    // 3. Build module name from filename
    std::string moduleName;
    if (!rootCode->co_filename.empty()) {
        moduleName = rootCode->co_filename;
    } else if (!srcPath.empty()) {
        moduleName = srcPath;
    } else {
        moduleName = "<unknown>";
    }

    // Extract base name (strip directory and .pyc/.py extension)
    {
        size_t slash = moduleName.rfind('/');
        if (slash == std::string::npos) slash = moduleName.rfind('\\');
        if (slash != std::string::npos) moduleName = moduleName.substr(slash+1);
        for (const char* ext : {".pyc", ".pyo", ".py"}) {
            size_t dot = moduleName.rfind(ext);
            if (dot != std::string::npos)
                moduleName = moduleName.substr(0, dot);
        }
        // Strip Python's special angle-bracket names like <string>, <module>, <stdin>.
        if (moduleName.size() >= 2 &&
            moduleName.front() == '<' && moduleName.back() == '>')
            moduleName = moduleName.substr(1, moduleName.size() - 2);
    }

    result.module.setName(moduleName);

    // 4. Build BcModule from the root code object
    buildModule(*rootCode, moduleName, result);

    result.root    = rootCode;
    result.success = true;
    return result;
}

// ─── buildModule ─────────────────────────────────────────────────────────────

void PycReader::buildModule(const PyCodeObject& root,
                             const std::string& moduleName,
                             PycReadResult& result) {
    // One BcClass per Python module (the module-level code object)
    BcClass cls;
    cls.name        = moduleName;
    cls.fqName      = moduleName;
    cls.packageName = "";
    cls.access      = BcAccess::Public;

    emitCodeObject(root, cls, result, moduleName);

    result.module.addClass(std::move(cls));
}

// ─── emitCodeObject ──────────────────────────────────────────────────────────

void PycReader::emitCodeObject(const PyCodeObject& code,
                                BcClass& cls,
                                PycReadResult& result,
                                const std::string& parentQual) {
    ++result.totalCodeObjects;

    // Skip compiler-generated comprehensions / genexprs if configured
    bool isCompGenerated = !code.co_name.empty() &&
                           (code.co_name == "<genexpr>"  ||
                            code.co_name == "<listcomp>" ||
                            code.co_name == "<dictcomp>" ||
                            code.co_name == "<setcomp>");
    if (opts_.skipCompGenerated && isCompGenerated) {
        ++result.skippedMethods;
        return;
    }

    std::string qualName = code.co_qualname.empty()
                         ? (parentQual.empty() ? code.co_name
                                               : parentQual + "." + code.co_name)
                         : code.co_qualname;

    BcMethod method = buildMethod(code, qualName);

    if (opts_.buildCFG) {
        buildCFG(code, method);
        result.totalInstructions +=
            static_cast<int>(method.cfg.blockCount() > 0
                ? method.cfg.blocks().size() : 0);
    }

    cls.methods.push_back(std::move(method));
    ++result.parsedMethods;

    // Recurse into nested code objects in co_consts
    if (opts_.recurseNested) {
        for (const auto& c : code.co_consts) {
            if (c.kind == PyCodeObject::Const::Kind::Code && c.code) {
                emitCodeObject(*c.code, cls, result, qualName);
            }
        }
    }
}

// ─── buildMethod ─────────────────────────────────────────────────────────────

BcMethod PycReader::buildMethod(const PyCodeObject& code,
                                 const std::string& qualName) const {
    BcMethod m;
    m.name = code.co_name;
    if (m.name.empty()) m.name = "<module>";

    m.isConstructor = (m.name == "__init__");
    m.isStaticInit  = (m.name == "<module>");
    m.isAbstract    = false;
    m.isNative      = false;
    m.access        = BcAccess::Public;

    // Return type: always generic Python object
    m.descriptor.returnType = std::make_shared<BcType>(types::ClrObject());

    // Parameters: first co_argcount entries of co_varnames
    int paramCount = code.co_argcount;
    if (code.hasVarArgs())   ++paramCount; // *args
    if (code.hasVarKwargs()) ++paramCount; // **kwargs

    for (int i = 0; i < paramCount && i < (int)code.co_varnames.size(); ++i) {
        m.paramNames.push_back(code.co_varnames[i]);
        m.descriptor.params.push_back(std::make_shared<BcType>(types::ClrObject()));
    }

    // Local variables (everything after the parameters)
    for (size_t i = static_cast<size_t>(paramCount);
         i < code.co_varnames.size(); ++i) {
        BcLocalVar lv;
        lv.name    = code.co_varnames[i];
        lv.type    = types::ClrObject();
        lv.isParam = false;
        m.locals.push_back(std::move(lv));
    }

    // Free variables
    for (const auto& fv : code.co_freevars) {
        BcLocalVar lv;
        lv.name = fv;
        lv.type = types::ClrObject();
        m.locals.push_back(std::move(lv));
    }

    // Flags to BcAccess
    if (code.isGenerator())
        m.access = m.access | BcAccess::Bridge; // generator marker
    if (code.isCoroutine())
        m.access = m.access | BcAccess::Synchronized; // async marker (reuse)

    m.maxStack  = static_cast<uint16_t>(code.co_stacksize);
    m.maxLocals = static_cast<uint16_t>(code.co_nlocals);

    return m;
}

// ─── liftInstruction ────────────────────────────────────────────────────────

void PycReader::liftInstruction(uint8_t rawOp, int32_t arg,
                                 uint32_t offset,
                                 const PyCodeObject& code,
                                 BcInstruction& out) const {
    OpcodeInfo info = opcodeInfo(rawOp, code.version);

    // Map opcode name → BcOpcode
    auto it = kPyOpcodeMap.find(info.name);
    BcOpcode bcOp = (it != kPyOpcodeMap.end())
                  ? it->second
                  : BcOpcode::PYTHON_UNKNOWN;
    out.opcode = bcOp;
    out.offset = offset;

    // Line number from table
    out.line = static_cast<uint32_t>(
        std::max(0, code.lineAt(offset)));

    // Operand encoding based on opcode semantics
    BcOperand operand;

    switch (bcOp) {
    case BcOpcode::PYTHON_LOAD_CONST: {
        if (arg >= 0 && static_cast<size_t>(arg) < code.co_consts.size()) {
            const auto& c = code.co_consts[arg];
            if (c.kind == PyCodeObject::Const::Kind::Int) {
                operand = BcIntOperand{static_cast<int64_t>(c.ival)};
            } else if (c.kind == PyCodeObject::Const::Kind::Str ||
                       c.kind == PyCodeObject::Const::Kind::Unicode) {
                operand = BcStringOperand{c.sval};
            } else if (c.kind == PyCodeObject::Const::Kind::Float) {
                operand = BcFloatOperand{c.fval};
            } else {
                operand = BcIntOperand{static_cast<int64_t>(arg)};
            }
        } else {
            operand = BcIntOperand{static_cast<int64_t>(arg)};
        }
        break;
    }
    case BcOpcode::PYTHON_LOAD_NAME:
    case BcOpcode::PYTHON_STORE_NAME:
    case BcOpcode::PYTHON_DELETE_NAME:
    case BcOpcode::PYTHON_LOAD_GLOBAL:
    case BcOpcode::PYTHON_STORE_GLOBAL:
    case BcOpcode::PYTHON_DELETE_GLOBAL:
    case BcOpcode::PYTHON_IMPORT_FROM:
    case BcOpcode::PYTHON_IMPORT_NAME: {
        if (arg >= 0 && static_cast<size_t>(arg) < code.co_names.size())
            operand = BcStringOperand{code.co_names[arg]};
        else
            operand = BcIntOperand{static_cast<int64_t>(arg)};
        break;
    }
    case BcOpcode::PYTHON_LOAD_FAST:
    case BcOpcode::PYTHON_STORE_FAST:
    case BcOpcode::PYTHON_DELETE_FAST:
    case BcOpcode::PYTHON_LOAD_FAST_CHECK:
    case BcOpcode::PYTHON_LOAD_FAST_AND_CLEAR: {
        if (arg >= 0 && static_cast<size_t>(arg) < code.co_varnames.size())
            operand = BcStringOperand{code.co_varnames[arg]};
        else
            operand = BcIntOperand{static_cast<int64_t>(arg)};
        break;
    }
    case BcOpcode::PYTHON_LOAD_ATTR:
    case BcOpcode::PYTHON_STORE_ATTR:
    case BcOpcode::PYTHON_DELETE_ATTR:
    case BcOpcode::PYTHON_LOAD_METHOD: {
        if (arg >= 0 && static_cast<size_t>(arg) < code.co_names.size())
            operand = BcStringOperand{code.co_names[arg]};
        else
            operand = BcIntOperand{static_cast<int64_t>(arg)};
        break;
    }
    case BcOpcode::PYTHON_LOAD_DEREF:
    case BcOpcode::PYTHON_STORE_DEREF:
    case BcOpcode::PYTHON_LOAD_CLOSURE:
    case BcOpcode::PYTHON_LOAD_CLASSDEREF: {
        // cellvars come first, then freevars
        size_t ci = static_cast<size_t>(arg);
        if (ci < code.co_cellvars.size())
            operand = BcStringOperand{code.co_cellvars[ci]};
        else {
            ci -= code.co_cellvars.size();
            if (ci < code.co_freevars.size())
                operand = BcStringOperand{code.co_freevars[ci]};
            else
                operand = BcIntOperand{static_cast<int64_t>(arg)};
        }
        break;
    }
    // Jumps: encode target as int64
    case BcOpcode::PYTHON_JUMP_FORWARD:
    case BcOpcode::PYTHON_JUMP_ABSOLUTE:
    case BcOpcode::PYTHON_JUMP_BACKWARD:
    case BcOpcode::PYTHON_JUMP_BACKWARD_NO_INTERRUPT:
    case BcOpcode::PYTHON_POP_JUMP_IF_TRUE:
    case BcOpcode::PYTHON_POP_JUMP_IF_FALSE:
    case BcOpcode::PYTHON_POP_JUMP_IF_NONE:
    case BcOpcode::PYTHON_POP_JUMP_IF_NOT_NONE:
    case BcOpcode::PYTHON_POP_JUMP_FORWARD_IF_TRUE:
    case BcOpcode::PYTHON_POP_JUMP_FORWARD_IF_FALSE:
    case BcOpcode::PYTHON_POP_JUMP_FORWARD_IF_NONE:
    case BcOpcode::PYTHON_POP_JUMP_FORWARD_IF_NOT_NONE:
    case BcOpcode::PYTHON_POP_JUMP_BACKWARD_IF_TRUE:
    case BcOpcode::PYTHON_POP_JUMP_BACKWARD_IF_FALSE:
    case BcOpcode::PYTHON_POP_JUMP_BACKWARD_IF_NONE:
    case BcOpcode::PYTHON_POP_JUMP_BACKWARD_IF_NOT_NONE:
    case BcOpcode::PYTHON_JUMP_IF_TRUE_OR_POP:
    case BcOpcode::PYTHON_JUMP_IF_FALSE_OR_POP:
    case BcOpcode::PYTHON_FOR_ITER:
    case BcOpcode::PYTHON_SEND: {
        // Compute absolute target
        int32_t target = arg;
        if (info.kind == OpcodeKind::JumpForward)
            target = static_cast<int32_t>(offset) + 2 + arg * 2;
        else if (info.kind == OpcodeKind::JumpBackward)
            target = static_cast<int32_t>(offset) + 2 - arg * 2;
        else if (!code.version.atLeast(3, 11))
            target = arg; // absolute offset
        else
            target = static_cast<int32_t>(offset) + 2 + arg * 2;
        operand = BcIntOperand{static_cast<int64_t>(target)};
        break;
    }
    default:
        operand = BcIntOperand{static_cast<int64_t>(arg)};
        break;
    }

    out.operands.clear();
    out.operands.push_back(std::move(operand));
}

// ─── findLeaders ─────────────────────────────────────────────────────────────

std::vector<uint32_t> PycReader::findLeaders(const PyCodeObject& code) const {
    std::set<uint32_t> leaders;
    leaders.insert(0);

    const auto& bytecode = code.co_code;
    const bool is311 = code.version.atLeast(3, 11);
    const uint8_t haveArg = haveArgument(code.version);

    size_t pos = 0;
    int32_t extArg = 0;

    while (pos < bytecode.size()) {
        uint8_t op = bytecode[pos];
        int32_t  arg = 0;
        size_t   instrSize = 2; // always 2 bytes in wordcode

        if (pos + 1 < bytecode.size())
            arg = bytecode[pos+1];
        arg |= extArg << 8;

        bool hasArg = is311 ? true : (op >= haveArg);

        if (op == 90 && !is311) { // EXTENDED_ARG in 3.8-3.10
            extArg = (extArg | arg) << 8;
            pos += instrSize;
            continue;
        }
        if (is311 && op == 144) { // EXTENDED_ARG in 3.11+
            extArg = (extArg | arg) << 8;
            pos += instrSize;
            continue;
        }
        extArg = 0;

        uint32_t nextOff = static_cast<uint32_t>(pos + instrSize);

        OpcodeInfo info = opcodeInfo(op, code.version);
        if (info.isJump() && hasArg) {
            int32_t target = arg;
            if (info.kind == OpcodeKind::JumpForward)
                target = static_cast<int32_t>(pos) + static_cast<int32_t>(instrSize)
                       + arg * 2;
            else if (info.kind == OpcodeKind::JumpBackward)
                target = static_cast<int32_t>(pos) + static_cast<int32_t>(instrSize)
                       - arg * 2;

            if (target >= 0 && static_cast<size_t>(target) < bytecode.size())
                leaders.insert(static_cast<uint32_t>(target));
            // Fall-through is a leader too (conditional jumps)
            if (info.kind != OpcodeKind::JumpAbsolute &&
                info.kind != OpcodeKind::Jump &&
                nextOff < bytecode.size())
                leaders.insert(nextOff);
            leaders.insert(nextOff); // always add fall-through
        } else if (info.isReturn() || info.kind == OpcodeKind::Raise) {
            // After return/raise, the next instruction starts a new block
            if (nextOff < bytecode.size())
                leaders.insert(nextOff);
        }

        pos += instrSize;
    }

    // Exception table entries add leaders at handler targets (3.11+)
    for (const auto& ee : code.exceptionTable) {
        leaders.insert(ee.target);
        leaders.insert(ee.start);
    }

    return std::vector<uint32_t>(leaders.begin(), leaders.end());
}

// ─── buildCFG ─────────────────────────────────────────────────────────────────

void PycReader::buildCFG(const PyCodeObject& code, BcMethod& method) const {
    const auto& bytecode = code.co_code;
    if (bytecode.empty()) return;

    auto leaders = findLeaders(code);
    std::sort(leaders.begin(), leaders.end());

    // Map offset → block index
    std::unordered_map<uint32_t, size_t> offsetToBlock;
    for (size_t i = 0; i < leaders.size(); ++i)
        offsetToBlock[leaders[i]] = i;

    // Create blocks
    std::vector<BcBasicBlock*> blocks;
    blocks.reserve(leaders.size());
    for (size_t i = 0; i < leaders.size(); ++i) {
        BcBasicBlock& bb = method.cfg.addBlock();
        bb.id = static_cast<uint32_t>(i);
        blocks.push_back(&bb);
    }

    // Decode instructions and fill blocks
    const bool is311 = code.version.atLeast(3, 11);
    const uint8_t haveArg = haveArgument(code.version);
    size_t pos   = 0;
    int32_t extArg = 0;
    size_t   curBlockIdx = 0;

    while (pos < bytecode.size()) {
        uint8_t op = bytecode[pos];
        int32_t  arg = (pos + 1 < bytecode.size()) ? bytecode[pos+1] : 0;

        // Check if we've entered a new block
        {
            auto it = offsetToBlock.find(static_cast<uint32_t>(pos));
            if (it != offsetToBlock.end() && it->second != curBlockIdx) {
                curBlockIdx = it->second;
                extArg = 0;
            }
        }

        // Handle EXTENDED_ARG
        if ((!is311 && op == 90) || (is311 && op == 144)) {
            extArg = (extArg | arg) << 8;
            pos += 2;
            continue;
        }

        int32_t fullArg = (extArg | arg);
        extArg = 0;

        BcInstruction insn;
        liftInstruction(op, fullArg, static_cast<uint32_t>(pos), code, insn);
        blocks[curBlockIdx]->instrs.push_back(std::move(insn));

        pos += 2;

        // Wire successors after the last instruction of the block
        if (pos < bytecode.size()) {
            auto nextIt = offsetToBlock.find(static_cast<uint32_t>(pos));
            if (nextIt != offsetToBlock.end() && nextIt->second != curBlockIdx) {
                // Fall-through edge
                OpcodeInfo info = opcodeInfo(op, code.version);
                if (!info.isReturn() && info.kind != OpcodeKind::Raise) {
                    blocks[curBlockIdx]->succs.push_back(
                        static_cast<uint32_t>(nextIt->second));
                }
            }
        }
    }

    // Add jump successor edges from jump instructions
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        BcBasicBlock* bb = blocks[bi];
        if (bb->instrs.empty()) continue;
        const BcInstruction& last = bb->instrs.back();

        OpcodeInfo info = opcodeInfo(
            static_cast<uint8_t>(0), // lookup by opcode name not needed here
            code.version);

        // Resolve jump targets from operand
        if (!last.operands.empty() && std::holds_alternative<BcIntOperand>(last.operands[0])) {
            int64_t target = std::get<BcIntOperand>(last.operands[0]).value;
            auto it = offsetToBlock.find(static_cast<uint32_t>(target));
            if (it != offsetToBlock.end()) {
                uint32_t targetBlock = static_cast<uint32_t>(it->second);
                // Add if not already there
                bool already = false;
                for (auto s : bb->succs) if (s == targetBlock) { already = true; break; }
                if (!already) bb->succs.push_back(targetBlock);
            }
        }
    }

    // Exception handler blocks
    for (const auto& ee : code.exceptionTable) {
        auto it = offsetToBlock.find(ee.target);
        if (it == offsetToBlock.end()) continue;
        BcExceptionHandler eh;
        eh.startOffset  = ee.start;
        eh.endOffset    = ee.end;
        eh.handlerBlock = static_cast<uint32_t>(it->second);
        // catchType unknown at this stage
        method.cfg.addExceptionHandler(std::move(eh));
    }
}

// ─── constType / nameType ────────────────────────────────────────────────────

BcType PycReader::constType(const PyCodeObject::Const& c) const {
    switch (c.kind) {
    case PyCodeObject::Const::Kind::Int:   return types::Long();
    case PyCodeObject::Const::Kind::Float: return types::Double();
    case PyCodeObject::Const::Kind::Bytes: return types::ClrObject(); // bytes
    case PyCodeObject::Const::Kind::Str:
    case PyCodeObject::Const::Kind::Unicode: return types::ClrString();
    default: return types::ClrObject();
    }
}

BcType PycReader::nameType(const std::string&) const {
    return types::ClrObject();
}

} // namespace pyc_parser
} // namespace retdec
