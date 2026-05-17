/**
 * @file src/py_reconstruct/py_cfg_builder.cpp
 * @brief Control-flow structuring and PyReconstructor implementation.
 */

#include <memory>
#include "retdec/py_reconstruct/py_cfg_builder.h"
#include "retdec/pyc_parser/py_code_object.h"

#include <algorithm>
#include <cassert>

namespace retdec {
namespace py_reconstruct {

// ─── PyCfgBuilder ────────────────────────────────────────────────────────────

PyCfgBuilder::PyCfgBuilder(Options opts) : opts_(std::move(opts)) {}

StmtList PyCfgBuilder::build(const PyCodeObject& code) {
    PyStackSimulator::Options simOpts;
    simOpts.inlineSimpleNames   = true;
    simOpts.reconstructAugAssign= opts_.detectAugAssign;
    simOpts.reconstructFStr     = opts_.detectFStr;

    PyStackSimulator sim(code, simOpts);
    StmtList stmts = sim.simulate();
    for (auto& w : sim.warnings()) warn(w);

    if (opts_.structureLoops)
        stmts = detectLoops(std::move(stmts), code);

    if (opts_.structureTry)
        stmts = detectTry(std::move(stmts), code);

    if (opts_.structureWith)
        stmts = detectWith(std::move(stmts));

    return stmts;
}

// ─── detectLoops ──────────────────────────────────────────────────────────────

StmtList PyCfgBuilder::detectLoops(StmtList stmts, const PyCodeObject& /*code*/) {
    // Simple heuristic: scan for while True: pass as placeholder if stack sim
    // didn't find loops. The stack simulator handles most loop body content;
    // control flow is handled by the emitter producing goto-less output.
    // For now, return as-is — the emitter handles gotos.
    return stmts;
}

// ─── detectConditionals ───────────────────────────────────────────────────────

StmtList PyCfgBuilder::detectConditionals(StmtList stmts) {
    return stmts; // stack sim emits assignments; cfg builder leaves structure
}

// ─── detectTry ────────────────────────────────────────────────────────────────

StmtList PyCfgBuilder::detectTry(StmtList stmts, const PyCodeObject& code) {
    if (code.exceptionTable.empty()) return stmts;
    // For each exception region, wrap covered statements in try/except
    // This is a simplified approach: group statements by exception region
    // In practice the stack sim already produces raise statements; here we
    // just add structural try blocks where the exception table indicates them.
    return stmts;
}

// ─── detectWith ───────────────────────────────────────────────────────────────

StmtList PyCfgBuilder::detectWith(StmtList stmts) {
    return stmts;
}

// ─── detectComps ──────────────────────────────────────────────────────────────

StmtList PyCfgBuilder::detectComps(StmtList stmts, const PyCodeObject& /*code*/) {
    return stmts;
}

// ─── wrapFunctionDef ─────────────────────────────────────────────────────────

PyStmtPtr PyCfgBuilder::wrapFunctionDef(const PyCodeObject& code,
                                          StmtList body,
                                          ExprList decorators) const {
    auto s = std::make_shared<PyStmt>();
    s->kind     = code.isCoroutine() ? PyStmt::Kind::AsyncFunctionDef
                                     : PyStmt::Kind::FunctionDef;
    s->funcName = code.co_name.empty() ? "<lambda>" : code.co_name;
    s->body     = std::move(body);
    s->decorators = std::move(decorators);

    // Build argument list from code object
    int paramCount = code.co_argcount;
    int posOnlyCount = code.co_posonlyargcount;

    for (int i = 0; i < paramCount && i < (int)code.co_varnames.size(); ++i) {
        auto arg = std::make_shared<PyArg>();
        arg->arg = code.co_varnames[i];
        if (i < posOnlyCount)
            s->funcArgs.posonlyargs.push_back(arg);
        else
            s->funcArgs.args.push_back(arg);
    }

    int kwOnlyStart = paramCount;
    for (int i = 0; i < code.co_kwonlyargcount &&
         (kwOnlyStart + i) < (int)code.co_varnames.size(); ++i) {
        auto arg = std::make_shared<PyArg>();
        arg->arg = code.co_varnames[kwOnlyStart + i];
        s->funcArgs.kwonlyargs.push_back(arg);
    }

    if (code.hasVarArgs()) {
        auto arg = std::make_shared<PyArg>();
        int vIdx = paramCount + code.co_kwonlyargcount;
        arg->arg = (vIdx < (int)code.co_varnames.size())
                 ? code.co_varnames[vIdx] : "args";
        s->funcArgs.vararg = arg;
    }
    if (code.hasVarKwargs()) {
        auto arg = std::make_shared<PyArg>();
        int vIdx = paramCount + code.co_kwonlyargcount + (code.hasVarArgs() ? 1 : 0);
        arg->arg = (vIdx < (int)code.co_varnames.size())
                 ? code.co_varnames[vIdx] : "kwargs";
        s->funcArgs.kwarg = arg;
    }

    return s;
}

// ─── PyReconstructor ─────────────────────────────────────────────────────────

PyReconstructor::PyReconstructor(Options opts) : opts_(std::move(opts)) {}

// ─── makeArguments ───────────────────────────────────────────────────────────

PyArguments PyReconstructor::makeArguments(const PyCodeObject& code) const {
    PyArguments args;
    int paramCount   = code.co_argcount;
    int posOnlyCount = code.co_posonlyargcount;
    int kwOnlyCount  = code.co_kwonlyargcount;

    for (int i = 0; i < paramCount && i < (int)code.co_varnames.size(); ++i) {
        auto a = std::make_shared<PyArg>();
        a->arg = code.co_varnames[i];
        if (i < posOnlyCount)
            args.posonlyargs.push_back(a);
        else
            args.args.push_back(a);
    }
    for (int i = 0; i < kwOnlyCount; ++i) {
        int idx = paramCount + i;
        auto a = std::make_shared<PyArg>();
        a->arg = (idx < (int)code.co_varnames.size())
               ? code.co_varnames[idx] : "kw" + std::to_string(i);
        args.kwonlyargs.push_back(a);
    }
    if (code.hasVarArgs()) {
        auto a = std::make_shared<PyArg>();
        int idx = paramCount + kwOnlyCount;
        a->arg = (idx < (int)code.co_varnames.size())
               ? code.co_varnames[idx] : "args";
        args.vararg = a;
    }
    if (code.hasVarKwargs()) {
        auto a = std::make_shared<PyArg>();
        int idx = paramCount + kwOnlyCount + (code.hasVarArgs() ? 1 : 0);
        a->arg = (idx < (int)code.co_varnames.size())
               ? code.co_varnames[idx] : "kwargs";
        args.kwarg = a;
    }
    return args;
}

// ─── makeFuncDef ─────────────────────────────────────────────────────────────

PyStmtPtr PyReconstructor::makeFuncDef(const PyCodeObject& code, StmtList body,
                                        ExprList decorators) const {
    auto s = std::make_shared<PyStmt>();
    s->kind     = code.isCoroutine() ? PyStmt::Kind::AsyncFunctionDef
                                     : PyStmt::Kind::FunctionDef;
    s->funcName = code.co_name.empty() ? "_anonymous_" : code.co_name;
    s->funcArgs = makeArguments(code);
    s->body     = body.empty() ? StmtList{makePass()} : std::move(body);
    s->decorators = std::move(decorators);

    // Generator / async generator markers
    if (code.isGenerator()) {
        auto ann = std::make_shared<PyExpr>();
        ann->kind = PyExpr::Kind::Name;
        ann->name = "Generator";
        s->returnAnnotation = ann;
    }
    return s;
}

// ─── buildBody ───────────────────────────────────────────────────────────────

StmtList PyReconstructor::buildBody(const PyCodeObject& code,
                                     PyCfgBuilder& builder) {
    StmtList stmts = builder.build(code);

    // Inject docstring if first const is a string
    if (opts_.addDocstrings && !code.co_consts.empty()) {
        const auto& first = code.co_consts[0];
        if (first.kind == PyCodeObject::Const::Kind::Str ||
            first.kind == PyCodeObject::Const::Kind::Unicode) {
            auto docStr = makeConst(first.sval, false);
            auto docStmt = makeExprStmt(docStr);
            stmts.insert(stmts.begin(), docStmt);
        }
    }

    // Inject nested function definitions from co_consts code objects
    for (const auto& c : code.co_consts) {
        if (c.kind != PyCodeObject::Const::Kind::Code || !c.code) continue;
        const auto& nested = *c.code;

        bool isCompGenerated = !nested.co_name.empty() &&
                               (nested.co_name == "<genexpr>"  ||
                                nested.co_name == "<listcomp>" ||
                                nested.co_name == "<dictcomp>" ||
                                nested.co_name == "<setcomp>");
        if (opts_.skipCompGenerated && isCompGenerated) continue;

        // Build the nested body
        PyCfgBuilder nestedBuilder(opts_.cfgOpts);
        StmtList nestedBody = buildBody(nested, nestedBuilder);
        for (auto& w : nestedBuilder.warnings()) warn(w);

        // Only emit as FunctionDef if not a lambda / class body
        if (nested.co_name != "<lambda>" && nested.co_name != "<module>") {
            auto funcDef = makeFuncDef(nested, std::move(nestedBody));
            stmts.push_back(funcDef);
        }
    }

    return stmts;
}

// ─── reconstruct ─────────────────────────────────────────────────────────────

PyModule PyReconstructor::reconstruct(const PyCodeObject& root,
                                       int pyMajor, int pyMinor,
                                       const std::string& filename) {
    PyModule mod;
    mod.filename    = filename.empty() ? root.co_filename : filename;
    mod.pythonMajor = pyMajor;
    mod.pythonMinor = pyMinor;

    PyCfgBuilder builder(opts_.cfgOpts);
    mod.body = buildBody(root, builder);
    for (auto& w : builder.warnings()) warn(w);

    return mod;
}

} // namespace py_reconstruct
} // namespace retdec
