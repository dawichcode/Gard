#include "bytecode/bytecode.h"
#include <cstring>

namespace gard {
namespace bytecode {

uint16_t BytecodeModule::addConstantInt(int32_t val) {
    ConstantEntry entry;
    entry.tag = ConstantTag::Integer;
    entry.intVal = val;
    constantPool.push_back(entry);
    return static_cast<uint16_t>(constantPool.size() - 1);
}

uint16_t BytecodeModule::addConstantLong(int64_t val) {
    ConstantEntry entry;
    entry.tag = ConstantTag::Long;
    entry.longVal = val;
    constantPool.push_back(entry);
    return static_cast<uint16_t>(constantPool.size() - 1);
}

uint16_t BytecodeModule::addConstantFloat(float val) {
    ConstantEntry entry;
    entry.tag = ConstantTag::Float;
    entry.floatVal = val;
    constantPool.push_back(entry);
    return static_cast<uint16_t>(constantPool.size() - 1);
}

uint16_t BytecodeModule::addConstantDouble(double val) {
    ConstantEntry entry;
    entry.tag = ConstantTag::Double;
    entry.doubleVal = val;
    constantPool.push_back(entry);
    return static_cast<uint16_t>(constantPool.size() - 1);
}

uint16_t BytecodeModule::addConstantString(const std::string& val) {
    // Check if already exists
    for (uint16_t i = 0; i < constantPool.size(); i++) {
        if (constantPool[i].tag == ConstantTag::String && constantPool[i].strVal == val) {
            return i;
        }
    }
    ConstantEntry entry;
    entry.tag = ConstantTag::String;
    entry.strVal = val;
    constantPool.push_back(entry);
    return static_cast<uint16_t>(constantPool.size() - 1);
}

// --- Serialization ---

std::vector<uint8_t> BytecodeModule::serialize() const {
    std::vector<uint8_t> data;

    // Magic
    data.push_back(MAGIC[0]);
    data.push_back(MAGIC[1]);
    data.push_back(MAGIC[2]);
    data.push_back(MAGIC[3]);

    // Version
    data.push_back(static_cast<uint8_t>(versionMajor >> 8));
    data.push_back(static_cast<uint8_t>(versionMajor & 0xFF));
    data.push_back(static_cast<uint8_t>(versionMinor >> 8));
    data.push_back(static_cast<uint8_t>(versionMinor & 0xFF));

    // Flags
    data.push_back(static_cast<uint8_t>(flags >> 8));
    data.push_back(static_cast<uint8_t>(flags & 0xFF));

    // Constant pool size
    uint32_t cpSize = static_cast<uint32_t>(constantPool.size());
    data.push_back(static_cast<uint8_t>((cpSize >> 24) & 0xFF));
    data.push_back(static_cast<uint8_t>((cpSize >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((cpSize >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>(cpSize & 0xFF));

    // Function count
    uint16_t fnCount = static_cast<uint16_t>(functions.size());
    data.push_back(static_cast<uint8_t>(fnCount >> 8));
    data.push_back(static_cast<uint8_t>(fnCount & 0xFF));

    // Entry function
    data.push_back(static_cast<uint8_t>(entryFunction >> 8));
    data.push_back(static_cast<uint8_t>(entryFunction & 0xFF));

    // Code size
    uint32_t codeSize = static_cast<uint32_t>(code.size());
    data.push_back(static_cast<uint8_t>((codeSize >> 24) & 0xFF));
    data.push_back(static_cast<uint8_t>((codeSize >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((codeSize >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>(codeSize & 0xFF));

    // Constant pool entries
    for (auto& entry : constantPool) {
        data.push_back(static_cast<uint8_t>(entry.tag));
        switch (entry.tag) {
            case ConstantTag::Integer: {
                int32_t v = entry.intVal;
                data.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
                data.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
                data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                data.push_back(static_cast<uint8_t>(v & 0xFF));
                break;
            }
            case ConstantTag::Long: {
                int64_t v = entry.longVal;
                for (int i = 7; i >= 0; i--) {
                    data.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
                }
                break;
            }
            case ConstantTag::Float: {
                uint32_t bits;
                std::memcpy(&bits, &entry.floatVal, 4);
                data.push_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
                data.push_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
                data.push_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
                data.push_back(static_cast<uint8_t>(bits & 0xFF));
                break;
            }
            case ConstantTag::Double: {
                uint64_t bits;
                std::memcpy(&bits, &entry.doubleVal, 8);
                for (int i = 7; i >= 0; i--) {
                    data.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
                }
                break;
            }
            case ConstantTag::String:
            case ConstantTag::Class:
            case ConstantTag::Method: {
                uint16_t len = static_cast<uint16_t>(entry.strVal.size());
                data.push_back(static_cast<uint8_t>(len >> 8));
                data.push_back(static_cast<uint8_t>(len & 0xFF));
                for (char c : entry.strVal) {
                    data.push_back(static_cast<uint8_t>(c));
                }
                break;
            }
        }
    }

    // Function table
    for (auto& fn : functions) {
        // Name (length-prefixed)
        uint16_t nameLen = static_cast<uint16_t>(fn.name.size());
        data.push_back(static_cast<uint8_t>(nameLen >> 8));
        data.push_back(static_cast<uint8_t>(nameLen & 0xFF));
        for (char c : fn.name) data.push_back(static_cast<uint8_t>(c));
        // Params, locals, maxStack
        data.push_back(static_cast<uint8_t>(fn.paramCount >> 8));
        data.push_back(static_cast<uint8_t>(fn.paramCount & 0xFF));
        data.push_back(static_cast<uint8_t>(fn.localCount >> 8));
        data.push_back(static_cast<uint8_t>(fn.localCount & 0xFF));
        data.push_back(static_cast<uint8_t>(fn.maxStack >> 8));
        data.push_back(static_cast<uint8_t>(fn.maxStack & 0xFF));
        // Code offset and length
        data.push_back(static_cast<uint8_t>((fn.codeOffset >> 24) & 0xFF));
        data.push_back(static_cast<uint8_t>((fn.codeOffset >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((fn.codeOffset >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>(fn.codeOffset & 0xFF));
        data.push_back(static_cast<uint8_t>((fn.codeLength >> 24) & 0xFF));
        data.push_back(static_cast<uint8_t>((fn.codeLength >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((fn.codeLength >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>(fn.codeLength & 0xFF));
        // Flags
        data.push_back(fn.isAsync ? 1 : 0);
    }

    // Code section
    data.insert(data.end(), code.begin(), code.end());

    // Debug symbols
    uint32_t dbgCount = static_cast<uint32_t>(debugSymbols.size());
    data.push_back(static_cast<uint8_t>((dbgCount >> 24) & 0xFF));
    data.push_back(static_cast<uint8_t>((dbgCount >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((dbgCount >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>(dbgCount & 0xFF));
    for (auto& sym : debugSymbols) {
        data.push_back(static_cast<uint8_t>((sym.pcOffset >> 24) & 0xFF));
        data.push_back(static_cast<uint8_t>((sym.pcOffset >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((sym.pcOffset >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>(sym.pcOffset & 0xFF));
        data.push_back(static_cast<uint8_t>(sym.line >> 8));
        data.push_back(static_cast<uint8_t>(sym.line & 0xFF));
        data.push_back(static_cast<uint8_t>(sym.column >> 8));
        data.push_back(static_cast<uint8_t>(sym.column & 0xFF));
    }

    return data;
}

BytecodeModule BytecodeModule::deserialize(const std::vector<uint8_t>& data) {
    BytecodeModule mod;
    if (data.size() < 22) return mod; // minimum header size

    // Verify magic
    if (data[0] != 'G' || data[1] != 'A' || data[2] != 'R' || data[3] != 'D') {
        return mod;
    }

    // Read header (simplified — full impl would parse all sections)
    mod.versionMajor = (data[4] << 8) | data[5];
    mod.versionMinor = (data[6] << 8) | data[7];
    mod.flags = (data[8] << 8) | data[9];

    return mod;
}

// --- Verification ---

bool BytecodeModule::verify(std::vector<std::string>& errors) const {
    bool valid = true;

    // Check entry function exists
    if (entryFunction >= 0 && entryFunction >= static_cast<int16_t>(functions.size())) {
        errors.push_back("Entry function index out of bounds");
        valid = false;
    }

    // Check each function
    for (size_t i = 0; i < functions.size(); i++) {
        auto& fn = functions[i];
        if (fn.codeOffset + fn.codeLength > code.size()) {
            errors.push_back("Function '" + fn.name + "' code extends beyond code section");
            valid = false;
        }

        // Verify stack balance (simplified)
        // Walk through instructions and track stack depth
        int stackDepth = 0;
        uint32_t pc = fn.codeOffset;
        uint32_t end = fn.codeOffset + fn.codeLength;

        while (pc < end && pc < code.size()) {
            OpCode op = static_cast<OpCode>(code[pc]);
            pc++;

            switch (op) {
                // Push operations (+1)
                case OpCode::CONST_NULL:
                case OpCode::CONST_TRUE:
                case OpCode::CONST_FALSE:
                case OpCode::LOAD_LOCAL:
                case OpCode::LOAD_GLOBAL:
                case OpCode::LOAD_CAPTURE:
                case OpCode::DUP:
                    stackDepth++;
                    break;

                case OpCode::CONST_I32: pc += 4; stackDepth++; break;
                case OpCode::CONST_I64: pc += 8; stackDepth++; break;
                case OpCode::CONST_F32: pc += 4; stackDepth++; break;
                case OpCode::CONST_F64: pc += 8; stackDepth++; break;
                case OpCode::CONST_STR: pc += 2; stackDepth++; break;

                // Pop operations (-1)
                case OpCode::POP:
                case OpCode::STORE_LOCAL:
                case OpCode::STORE_GLOBAL:
                case OpCode::RETURN:
                case OpCode::PRINT:
                    stackDepth--;
                    break;

                // Binary ops: pop 2, push 1 (-1 net)
                case OpCode::ADD_I: case OpCode::SUB_I: case OpCode::MUL_I:
                case OpCode::DIV_I: case OpCode::MOD_I:
                case OpCode::ADD_F: case OpCode::SUB_F: case OpCode::MUL_F:
                case OpCode::DIV_F:
                case OpCode::BIT_AND: case OpCode::BIT_OR: case OpCode::BIT_XOR:
                case OpCode::SHL: case OpCode::SHR: case OpCode::USHR:
                case OpCode::CMP_EQ: case OpCode::CMP_NE:
                case OpCode::CMP_LT: case OpCode::CMP_GT:
                case OpCode::CMP_LE: case OpCode::CMP_GE:
                case OpCode::LOG_AND: case OpCode::LOG_OR:
                case OpCode::CONCAT:
                case OpCode::GET_ELEM:
                    stackDepth--;
                    break;

                // SET_ELEM: pop 3 (obj, idx, val)
                case OpCode::SET_ELEM:
                    stackDepth -= 3;
                    break;

                // Control flow
                case OpCode::JUMP: pc += 2; break;
                case OpCode::JUMP_IF: pc += 2; stackDepth--; break;
                case OpCode::JUMP_IFNOT: pc += 2; stackDepth--; break;

                // Calls
                case OpCode::CALL: pc += 3; break; // func_idx(2) + argc(1)
                case OpCode::CALL_VIRTUAL: pc += 3; break;

                // Objects
                case OpCode::NEW_OBJ: pc += 2; stackDepth++; break;
                case OpCode::GET_FIELD: pc += 2; break; // pop obj, push field (net 0)
                case OpCode::SET_FIELD: pc += 2; stackDepth -= 2; break;
                case OpCode::NEW_ARRAY: pc += 2; stackDepth++; break;
                case OpCode::NEW_MAP: stackDepth++; break;

                case OpCode::LINE: pc += 2; break;

                default: break;
            }

            if (stackDepth < 0) {
                errors.push_back("Stack underflow in function '" + fn.name + "' at offset " + std::to_string(pc));
                valid = false;
                break;
            }
        }
    }

    return valid;
}

} // namespace bytecode
} // namespace gard
