#pragma once
// forge::script compact stack bytecode: compiler, verifier and VM.
#include "interp.hpp"

namespace forge::script {

enum class Opc : std::uint8_t {
    Const, Nil, True, False, Pop, Dup,
    LoadLocal, StoreLocal, LoadGlobal, StoreGlobal, DefineGlobal, DefineConst,
    Binary, Unary, Jump, JumpIfFalse, JumpIfFalseKeep, JumpIfTrueKeep, JumpIfNotNilKeep,
    Call, GetField, OptField, SetField, GetIndex, SetIndex, MakeList, MakeMap, Closure, Interp,
    Iter, IterNext, IterNext2, Return, ReturnNil, CheckType, ExecNode, Is, Spawn, Await, Shell, Throw, Line, Super,
    Copy, EvalNode, ClearLocal,
};

struct Instr { Opc op; std::int32_t a = 0, b = 0; };

struct Chunk {
    std::string name, file;
    std::vector<Instr> code;
    std::vector<int> lines;                         // per instruction
    std::vector<Value> consts;                      // ints, floats, strings (strings frozen)
    std::vector<std::string> names;                 // globals and field names
    std::vector<NodeP> nodes;                       // delegated statements, lambdas, shells, types
    int nslots = 0;
};

std::shared_ptr<const Chunk> compile(const FnDecl& fd);
void verify(const Chunk& c);                        // throws std::runtime_error on malformed bytecode
Value vm_run(Interp& in, const Chunk& c, const Ref<Env>& env);
std::string disassemble(const Chunk& c);

} // namespace forge::script
