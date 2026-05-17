#include <algorithm>
/**
 * @file src/java_emitter/java_expr_emitter.cpp
 * @brief Java expression emitter implementation.
 */

#include "retdec/java_emitter/java_expr_emitter.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace retdec {
namespace java_emitter {

using namespace bc_module;
using namespace jvm_reconstruct;

// ─── ExprContext ─────────────────────────────────────────────────────────────

ExprContext::ExprContext(const BcMethod& m,
                          const ReconstructResult& r,
                          const JavaTypePrinter& tp)
    : method(m), recon(r), tyPrinter(tp) {
    for (const auto& lv : m.locals) {
        localNames[lv.index] = lv.name;
        localTypes[lv.index] = lv.type;
    }
}

// ─── Precedence table ─────────────────────────────────────────────────────────

int JavaExprEmitter::precedenceOf(BcOpcode op) {
    switch (op) {
        // Postfix (highest)
        case BcOpcode::ArrayLoad:       return 15;
        // Unary
        case BcOpcode::Neg: case BcOpcode::FNeg: return 14;
        // Multiplicative
        case BcOpcode::Mul: case BcOpcode::FMul:
        case BcOpcode::Div: case BcOpcode::FDiv:
        case BcOpcode::Rem: case BcOpcode::FRem: return 12;
        // Additive
        case BcOpcode::Add: case BcOpcode::FAdd:
        case BcOpcode::Sub: case BcOpcode::FSub: return 11;
        // Shift
        case BcOpcode::Shl: case BcOpcode::Shr: case BcOpcode::UShr: return 10;
        // Relational
        case BcOpcode::CmpLt: case BcOpcode::CmpGe:
        case BcOpcode::CmpGt: case BcOpcode::CmpLe:
        case BcOpcode::Instanceof:                 return 9;
        // Equality
        case BcOpcode::CmpEq: case BcOpcode::CmpNe: return 8;
        // Bitwise
        case BcOpcode::And: return 7;
        case BcOpcode::Xor: return 6;
        case BcOpcode::Or:  return 5;
        // Logical
        case BcOpcode::LAnd: return 4;
        case BcOpcode::LOr:  return 3;
        // Conditional/assignment (lowest)
        default: return 1;
    }
}

std::string JavaExprEmitter::opSymbol(BcOpcode op) {
    switch (op) {
        case BcOpcode::Add:   case BcOpcode::FAdd: return "+";
        case BcOpcode::Sub:   case BcOpcode::FSub: return "-";
        case BcOpcode::Mul:   case BcOpcode::FMul: return "*";
        case BcOpcode::Div:   case BcOpcode::FDiv: return "/";
        case BcOpcode::Rem:   case BcOpcode::FRem: return "%";
        case BcOpcode::Neg:   case BcOpcode::FNeg: return "-";
        case BcOpcode::Shl:  return "<<";
        case BcOpcode::Shr:  return ">>";
        case BcOpcode::UShr: return ">>>";
        case BcOpcode::And:  return "&";
        case BcOpcode::Or:   return "|";
        case BcOpcode::Xor:  return "^";
        case BcOpcode::LAnd: return "&&";
        case BcOpcode::LOr:  return "||";
        case BcOpcode::CmpEq: return "==";
        case BcOpcode::CmpNe: return "!=";
        case BcOpcode::CmpLt: return "<";
        case BcOpcode::CmpGe: return ">=";
        case BcOpcode::CmpGt: return ">";
        case BcOpcode::CmpLe: return "<=";
        case BcOpcode::FCmpL:
        case BcOpcode::FCmpG: return "/* fcmp */";
        default: return "?";
    }
}

// ─── Parenthesisation ────────────────────────────────────────────────────────

std::string JavaExprEmitter::paren(const ExprNode& expr, int required) {
    if (expr.precedence < required)
        return "(" + expr.text + ")";
    return expr.text;
}

// ─── Literal emission ─────────────────────────────────────────────────────────

std::string JavaExprEmitter::emitLiteral(const BcInstruction& insn) {
    if (insn.operands.empty()) {
        // No-operand push opcodes.
        switch (insn.opcode) {
            case BcOpcode::PushTrue:  return "true";
            case BcOpcode::PushFalse: return "false";
            case BcOpcode::PushNull:  return "null";
            default: return "0";
        }
    }
    if (auto* iv = std::get_if<BcIntOperand>(&insn.operands[0])) {
        if (insn.opcode == BcOpcode::PushLong)
            return std::to_string(iv->value) + "L";
        return std::to_string(iv->value);
    }
    if (auto* fv = std::get_if<BcFloatOperand>(&insn.operands[0])) {
        std::ostringstream os;
        if (insn.opcode == BcOpcode::PushFloat) {
            os << std::setprecision(8) << fv->value << "f";
        } else {
            os << std::setprecision(17) << fv->value;
        }
        std::string s = os.str();
        // Ensure there's a decimal point for double literals.
        if (insn.opcode == BcOpcode::PushDouble &&
            s.find('.') == std::string::npos &&
            s.find('e') == std::string::npos)
            s += ".0";
        return s;
    }
    if (auto* sv = std::get_if<BcStringOperand>(&insn.operands[0])) {
        // Escape the string.
        std::string out = "\"";
        for (char c : sv->value) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                        out += "\\u" + [&] {
                            std::ostringstream hex;
                            hex << std::hex << std::setw(4) << std::setfill('0')
                                << static_cast<unsigned>(static_cast<unsigned char>(c));
                            return hex.str();
                        }();
                    else
                        out += c;
            }
        }
        out += "\"";
        return out;
    }
    return "/* literal */0";
}

// ─── Constructor ─────────────────────────────────────────────────────────────

JavaExprEmitter::JavaExprEmitter(ExprContext& ctx)
    : ctx_(ctx) {}

// ─── Method call emission ─────────────────────────────────────────────────────

std::string JavaExprEmitter::emitMethodCall(const BcInstruction& insn,
                                              std::vector<ExprNode>& stack,
                                              bool isConstructorCall) {
    if (insn.operands.empty()) return "/* bad call */";
    auto* mref = std::get_if<BcMethodRef>(&insn.operands[0]);
    if (!mref) return "/* bad call operand */";

    // Collect arguments (right-to-left from stack).
    size_t numArgs = mref->descriptor.params.size();
    bool hasThis = (insn.opcode == BcOpcode::InvokeVirtual ||
                    insn.opcode == BcOpcode::InvokeInterface ||
                    insn.opcode == BcOpcode::InvokeSpecial ||
                    insn.opcode == BcOpcode::Callvirt);

    size_t total = numArgs + (hasThis ? 1 : 0);
    std::vector<std::string> args;
    for (size_t i = 0; i < total && !stack.empty(); ++i) {
        args.push_back(stack.back().text);
        stack.pop_back();
    }
    std::reverse(args.begin(), args.end());

    std::string receiver = hasThis && !args.empty() ? args[0] : "";
    std::vector<std::string> callArgs(
        hasThis ? args.begin() + 1 : args.begin(), args.end());

    // Build argument list.
    std::string argList;
    for (size_t i = 0; i < callArgs.size(); ++i) {
        if (i) argList += ", ";
        argList += callArgs[i];
    }

    // Simple class name from owner.
    std::string ownerFq = mref->owner;
    for (char& c : ownerFq) if (c == '/') c = '.';
    std::string ownerSimple = ctx_.tyPrinter.printNoImport(types::Class(ownerFq));

    if (isConstructorCall) {
        // new ClassName(args)
        return "new " + ownerSimple + "(" + argList + ")";
    }

    std::string method = mref->name;
    if (insn.opcode == BcOpcode::InvokeStatic || insn.opcode == BcOpcode::Call) {
        return ownerSimple + "." + method + "(" + argList + ")";
    }

    if (receiver.empty()) receiver = "this";
    return receiver + "." + method + "(" + argList + ")";
}

// ─── Field access emission ────────────────────────────────────────────────────

std::string JavaExprEmitter::emitFieldAccess(const BcInstruction& insn,
                                               std::vector<ExprNode>& stack,
                                               bool isPut) {
    if (insn.operands.empty()) return "/* bad field */";
    auto* fref = std::get_if<BcFieldRef>(&insn.operands[0]);
    if (!fref) return "/* bad field operand */";

    std::string ownerFq = fref->owner;
    for (char& c : ownerFq) if (c == '/') c = '.';
    std::string ownerSimple = ctx_.tyPrinter.printNoImport(types::Class(ownerFq));

    if (fref->isStatic) {
        std::string access = ownerSimple + "." + fref->name;
        if (isPut && !stack.empty()) {
            std::string val = stack.back().text; stack.pop_back();
            return access + " = " + val;
        }
        return access;
    }

    // Instance field.
    std::string obj = "this";
    if (!stack.empty() && !isPut) {
        obj = stack.back().text; stack.pop_back();
    } else if (isPut && stack.size() >= 2) {
        std::string val = stack.back().text; stack.pop_back();
        obj = stack.back().text; stack.pop_back();
        return obj + "." + fref->name + " = " + val;
    }
    return obj + "." + fref->name;
}

// ─── Array creation emission ──────────────────────────────────────────────────

std::string JavaExprEmitter::emitNewArray(const BcInstruction& insn,
                                           std::vector<ExprNode>& stack) {
    // Size on stack.
    std::string size = "0";
    if (!stack.empty()) { size = stack.back().text; stack.pop_back(); }

    // Determine element type.
    std::string elemType = "Object";
    if (!insn.operands.empty()) {
        if (auto* t = std::get_if<BcTypeOperand>(&insn.operands[0]))
            elemType = ctx_.tyPrinter.print(t->type);
    }
    return "new " + elemType + "[" + size + "]";
}

// ─── Lambda and method-reference emission ─────────────────────────────────────

std::string JavaExprEmitter::emitMethodRef(const LambdaPattern& pat) {
    // Method reference: Owner::method
    std::string ownerFq = pat.implMethod.owner;
    for (char& c : ownerFq) if (c == '/') c = '.';
    std::string ownerSimple = ctx_.tyPrinter.printNoImport(types::Class(ownerFq));
    return ownerSimple + "::" + pat.implMethod.name;
}

std::string JavaExprEmitter::emitLambda(const LambdaPattern& pat,
                                          const BcClass& /*ownerClass*/) {
    if (pat.kind == LambdaKind::MethodReference)
        return emitMethodRef(pat);

    // Lambda: (params) -> body
    // We emit a compact form using the implementation method descriptor.
    const BcFuncType& sig = pat.implMethod.descriptor;
    std::string params;
    for (size_t i = 0; i < sig.params.size(); ++i) {
        if (i) params += ", ";
        // Use type-inferred param names.
        if (i < ctx_.method.locals.size())
            params += ctx_.method.locals[i].name;
        else
            params += "p" + std::to_string(i);
    }
    if (sig.params.size() == 1) {
        // Single-param lambdas don't need parens if param is simple.
        return params + " -> { /* lambda body */ }";
    }
    return "(" + params + ") -> { /* lambda body */ }";
}

// ─── String concatenation ─────────────────────────────────────────────────────

std::string JavaExprEmitter::emitStringConcat(const StringConcatPattern& /*pat*/,
                                               const std::vector<ExprNode>& parts) {
    if (parts.empty()) return "\"\"";
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += " + ";
        out += parts[i].text;
    }
    return out;
}

// ─── Main instruction emitter ─────────────────────────────────────────────────

std::string JavaExprEmitter::emitInsn(const BcInstruction& insn,
                                       std::vector<ExprNode>& stack) {
    auto push = [&](std::string text, int prec = 0, bool sideEff = false) {
        stack.push_back({ExprKind::Literal, std::move(text), prec, sideEff});
        return std::string{};
    };

    auto pop = [&]() -> ExprNode {
        if (stack.empty()) return {ExprKind::Literal, "/* stack underflow */", 0, false};
        ExprNode n = stack.back(); stack.pop_back(); return n;
    };

    switch (insn.opcode) {
        // ── Constants ──────────────────────────────────────────────────────────
        case BcOpcode::PushInt:
        case BcOpcode::PushLong:
        case BcOpcode::PushFloat:
        case BcOpcode::PushDouble:
        case BcOpcode::PushString:
        case BcOpcode::PushTrue:
        case BcOpcode::PushFalse:
        case BcOpcode::PushNull:
            return push(emitLiteral(insn), 15);

        case BcOpcode::LoadClass:
            if (!insn.operands.empty()) {
                if (auto* t = std::get_if<BcTypeOperand>(&insn.operands[0])) {
                    return push(ctx_.tyPrinter.print(t->type) + ".class", 15);
                }
            }
            return push("Object.class", 15);

        // ── Local variable load ────────────────────────────────────────────────
        case BcOpcode::LoadLocal: {
            uint32_t idx = 0;
            if (!insn.operands.empty()) {
                if (auto* lop = std::get_if<BcLocalOperand>(&insn.operands[0]))
                    idx = lop->index;
            }
            auto it = ctx_.localNames.find(idx);
            std::string name = (it != ctx_.localNames.end()) ? it->second
                                                               : "v" + std::to_string(idx);
            return push(name, 15);
        }

        // ── Local variable store → statement ───────────────────────────────────
        case BcOpcode::StoreLocal: {
            uint32_t idx = 0;
            if (!insn.operands.empty()) {
                if (auto* lop = std::get_if<BcLocalOperand>(&insn.operands[0]))
                    idx = lop->index;
            }
            auto it = ctx_.localNames.find(idx);
            std::string name = (it != ctx_.localNames.end()) ? it->second
                                                               : "v" + std::to_string(idx);
            ExprNode val = pop();
            return name + " = " + val.text;
        }

        // ── Arithmetic / bitwise ───────────────────────────────────────────────
        case BcOpcode::Add: case BcOpcode::FAdd:
        case BcOpcode::Sub: case BcOpcode::FSub:
        case BcOpcode::Mul: case BcOpcode::FMul:
        case BcOpcode::Div: case BcOpcode::FDiv:
        case BcOpcode::Rem: case BcOpcode::FRem:
        case BcOpcode::Shl: case BcOpcode::Shr: case BcOpcode::UShr:
        case BcOpcode::And: case BcOpcode::Or:  case BcOpcode::Xor:
        case BcOpcode::LAnd: case BcOpcode::LOr:
        case BcOpcode::CmpEq: case BcOpcode::CmpNe:
        case BcOpcode::CmpLt: case BcOpcode::CmpGe:
        case BcOpcode::CmpGt: case BcOpcode::CmpLe: {
            int prec = precedenceOf(insn.opcode);
            std::string sym = opSymbol(insn.opcode);
            ExprNode rhs = pop();
            ExprNode lhs = pop();
            std::string expr = paren(lhs, prec) + " " + sym + " " + paren(rhs, prec + 1);
            return push(expr, prec);
        }

        case BcOpcode::Neg: case BcOpcode::FNeg: {
            ExprNode operand = pop();
            return push("-" + paren(operand, 14), 14);
        }

        // ── Type conversions ───────────────────────────────────────────────────
        case BcOpcode::I2L: { ExprNode e = pop(); return push("(long)" + paren(e, 13), 13); }
        case BcOpcode::I2F: { ExprNode e = pop(); return push("(float)" + paren(e, 13), 13); }
        case BcOpcode::I2D: { ExprNode e = pop(); return push("(double)" + paren(e, 13), 13); }
        case BcOpcode::L2I: { ExprNode e = pop(); return push("(int)" + paren(e, 13), 13); }
        case BcOpcode::L2F: { ExprNode e = pop(); return push("(float)" + paren(e, 13), 13); }
        case BcOpcode::L2D: { ExprNode e = pop(); return push("(double)" + paren(e, 13), 13); }
        case BcOpcode::F2D: { ExprNode e = pop(); return push("(double)" + paren(e, 13), 13); }
        case BcOpcode::F2I: { ExprNode e = pop(); return push("(int)" + paren(e, 13), 13); }
        case BcOpcode::F2L: { ExprNode e = pop(); return push("(long)" + paren(e, 13), 13); }
        case BcOpcode::D2I: { ExprNode e = pop(); return push("(int)" + paren(e, 13), 13); }
        case BcOpcode::D2L: { ExprNode e = pop(); return push("(long)" + paren(e, 13), 13); }
        case BcOpcode::D2F: { ExprNode e = pop(); return push("(float)" + paren(e, 13), 13); }
        case BcOpcode::I2B: { ExprNode e = pop(); return push("(byte)" + paren(e, 13), 13); }
        case BcOpcode::I2C: { ExprNode e = pop(); return push("(char)" + paren(e, 13), 13); }
        case BcOpcode::I2S: { ExprNode e = pop(); return push("(short)" + paren(e, 13), 13); }

        // ── Cast / instanceof ──────────────────────────────────────────────────
        case BcOpcode::CheckCast: {
            if (insn.operands.empty()) return {};
            auto* t = std::get_if<BcTypeOperand>(&insn.operands[0]);
            if (!t) return {};
            ExprNode e = pop();
            std::string castType = ctx_.tyPrinter.print(t->type);
            return push("(" + castType + ")" + paren(e, 13), 13);
        }
        case BcOpcode::Instanceof: {
            if (insn.operands.empty()) return {};
            auto* t = std::get_if<BcTypeOperand>(&insn.operands[0]);
            if (!t) return {};
            ExprNode e = pop();
            std::string iofType = ctx_.tyPrinter.print(t->type);
            return push(paren(e, 9) + " instanceof " + iofType, 9);
        }

        // ── Object creation ────────────────────────────────────────────────────
        case BcOpcode::New: {
            // Just push the type for now; constructor call follows.
            if (!insn.operands.empty()) {
                if (auto* t = std::get_if<BcTypeOperand>(&insn.operands[0]))
                    return push("/* new " + ctx_.tyPrinter.printNoImport(t->type) + " */", 15);
            }
            return push("/* new Object */", 15);
        }
        case BcOpcode::NewArray:
        case BcOpcode::MultiNewArray:
            return push(emitNewArray(insn, stack), 15);

        // ── Array access ───────────────────────────────────────────────────────
        case BcOpcode::ArrayLoad: {
            ExprNode idx  = pop();
            ExprNode arr  = pop();
            return push(paren(arr, 15) + "[" + idx.text + "]", 15);
        }
        case BcOpcode::ArrayStore: {
            ExprNode val  = pop();
            ExprNode idx  = pop();
            ExprNode arr  = pop();
            return arr.text + "[" + idx.text + "] = " + val.text;
        }
        case BcOpcode::ArrayLength: {
            ExprNode arr = pop();
            return push(paren(arr, 15) + ".length", 15);
        }

        // ── Field access ───────────────────────────────────────────────────────
        case BcOpcode::GetField:
            return push(emitFieldAccess(insn, stack, false), 15, false);
        case BcOpcode::GetStatic:
            return push(emitFieldAccess(insn, stack, false), 15, false);
        case BcOpcode::PutField:
        case BcOpcode::PutStatic:
            return emitFieldAccess(insn, stack, true);

        // ── Method invocation ──────────────────────────────────────────────────
        case BcOpcode::InvokeSpecial: {
            // If it's <init>, emit as constructor.
            bool isCtor = false;
            if (!insn.operands.empty()) {
                if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
                    isCtor = (m->name == "<init>");
            }
            std::string expr = emitMethodCall(insn, stack, isCtor);
            // Check if it returns void.
            bool returnsVoid = true;
            if (!insn.operands.empty()) {
                if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
                    returnsVoid = !m->descriptor.returnType ||
                                  m->descriptor.returnType->isVoid();
            }
            if (returnsVoid) return expr;
            return push(expr, 1, true);
        }
        case BcOpcode::InvokeVirtual:
        case BcOpcode::InvokeInterface:
        case BcOpcode::Callvirt:
        case BcOpcode::InvokeStatic:
        case BcOpcode::Call: {
            std::string expr = emitMethodCall(insn, stack, false);
            bool returnsVoid = true;
            if (!insn.operands.empty()) {
                if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
                    returnsVoid = !m->descriptor.returnType ||
                                  m->descriptor.returnType->isVoid();
            }
            if (returnsVoid) return expr;
            return push(expr, 1, true);
        }
        case BcOpcode::InvokeDynamic: {
            // Check for lambda/method-ref pattern.
            // The PatternLifter will have annotated these — just emit a placeholder.
            if (!insn.operands.empty()) {
                if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0])) {
                    // Lambda metafactory.
                    if (m->owner.find("LambdaMetafactory") != std::string::npos) {
                        return push("/* lambda */", 15);
                    }
                    // String concat factory.
                    if (m->name == "makeConcatWithConstants" ||
                        m->name == "makeConcat") {
                        // Collect all args from stack.
                        size_t numParts = m->descriptor.params.size();
                        std::vector<ExprNode> parts;
                        for (size_t i = 0; i < numParts && !stack.empty(); ++i) {
                            parts.push_back(stack.back());
                            stack.pop_back();
                        }
                        std::reverse(parts.begin(), parts.end());
                        std::string concat;
                        for (size_t i = 0; i < parts.size(); ++i) {
                            if (i) concat += " + ";
                            concat += parts[i].text;
                        }
                        return push(concat.empty() ? "\"\"" : concat, 11);
                    }
                }
            }
            return push("/* invokedynamic */", 15);
        }

        // ── Dup ────────────────────────────────────────────────────────────────
        case BcOpcode::Dup: {
            if (!stack.empty()) stack.push_back(stack.back());
            return {};
        }
        case BcOpcode::DupX1: case BcOpcode::DupX2:
        case BcOpcode::Dup2: case BcOpcode::Dup2X1: case BcOpcode::Dup2X2:
            if (!stack.empty()) stack.push_back(stack.back());
            return {};

        // ── Swap ───────────────────────────────────────────────────────────────
        case BcOpcode::Swap: {
            if (stack.size() >= 2) {
                std::swap(stack[stack.size()-1], stack[stack.size()-2]);
            }
            return {};
        }

        // ── Return ─────────────────────────────────────────────────────────────
        case BcOpcode::Return:
            return "return";
        case BcOpcode::ReturnValue: {
            ExprNode val = pop();
            return "return " + val.text;
        }
        case BcOpcode::Throw: {
            ExprNode val = pop();
            return "throw " + val.text;
        }

        // ── Control flow (handled at statement level) ───────────────────────────
        case BcOpcode::Goto:
        case BcOpcode::IfTrue:
        case BcOpcode::IfFalse:
        case BcOpcode::IfEq: case BcOpcode::IfNe:
        case BcOpcode::IfLt: case BcOpcode::IfGe:
        case BcOpcode::IfGt: case BcOpcode::IfLe:
        case BcOpcode::TableSwitch:
        case BcOpcode::LookupSwitch:
            return {}; // handled by stmt emitter

        // ── Comparison → boolean (JVM fcmpl/fcmpg/lcmp) ────────────────────────
        case BcOpcode::FCmpL: case BcOpcode::FCmpG: {
            ExprNode rhs = pop(); ExprNode lhs = pop();
            return push("Float.compare(" + lhs.text + ", " + rhs.text + ")", 15);
        }
        case BcOpcode::LCmp: {
            ExprNode rhs = pop(); ExprNode lhs = pop();
            return push("Long.compare(" + lhs.text + ", " + rhs.text + ")", 15);
        }

        // ── Null check ─────────────────────────────────────────────────────────
        case BcOpcode::IsNull: {
            ExprNode e = pop();
            return push(paren(e, 8) + " == null", 8);
        }
        case BcOpcode::IsNotNull: {
            ExprNode e = pop();
            return push(paren(e, 8) + " != null", 8);
        }

        // ── CLR-specific ───────────────────────────────────────────────────────
        case BcOpcode::Ldstr: {
            if (!insn.operands.empty()) {
                if (auto* sv = std::get_if<BcStringOperand>(&insn.operands[0])) {
                    return push("\"" + sv->value + "\"", 15);
                }
            }
            return push("\"\"", 15);
        }
        case BcOpcode::Box: {
            ExprNode e = pop();
            return push(e.text, e.precedence); // boxing is implicit in Java source
        }
        case BcOpcode::Unbox: {
            ExprNode e = pop();
            // For Java output, unboxing is implicit; just keep the expression.
            return push(e.text, e.precedence);
        }

        // ── Monitor enter/exit ────────────────────────────────────────────────
        case BcOpcode::MonitorEnter:
        case BcOpcode::MonitorExit:
            return {}; // handled at statement level

        // ── Anything else (nop, etc.) ─────────────────────────────────────────
        default:
            return {};
    }
}

} // namespace java_emitter
} // namespace retdec
