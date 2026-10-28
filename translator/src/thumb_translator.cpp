#include "thumb_decoder.hpp"
#include "alu_translator.hpp"
#include "cond_evaluator.hpp"
#include "mem_translator.hpp"

#include <limits>
#include <stdexcept>

namespace arm11 {
namespace {

std::string Reg(unsigned r) { return "cpu.r[" + std::to_string(r) + "]"; }

// Wide unsigned addition implements both carry and NOT-borrow without signed UB.
std::string Arithmetic(const std::string& lhs, const std::string& rhs,
                       bool subtract, bool carry, const std::string& destination) {
    std::string code = "{\n    const std::uint32_t lhs = " + lhs + ";\n"
                       "    const std::uint32_t rhs = " + rhs + ";\n"
                       "    const std::uint32_t carry = " +
                       (carry ? std::string{"cpu.cpsr.C ? 1u : 0u"} :
                                std::string{subtract ? "1u" : "0u"}) + ";\n"
                       "    const std::uint64_t wide = std::uint64_t{lhs} + " +
                       (subtract ? "std::uint64_t{rhs ^ 0xFFFFFFFFu}" : "std::uint64_t{rhs}") +
                       " + carry;\n"
                       "    const std::uint32_t result = static_cast<std::uint32_t>(wide);\n"
                       "    cpu.UpdateNZ(result);\n"
                       "    cpu.cpsr.C = (wide >> 32) != 0;\n"
                       "    cpu.cpsr.V = ((" +
                       (subtract ? std::string{"(lhs ^ rhs)"} : std::string{"~(lhs ^ rhs)"}) +
                       " & (lhs ^ result)) >> 31) != 0;\n";
    if (!destination.empty()) code += "    " + destination + " = result;\n";
    return code + "}";
}

std::string Shift(unsigned op, unsigned rd, unsigned rm) {
    std::string code = "{\n    const std::uint32_t value = " + Reg(rd) + ";\n"
                       "    const unsigned amount = " + Reg(rm) + " & 0xFFu;\n"
                       "    std::uint32_t result = value;\n"
                       "    if (amount != 0) {\n";
    if (op == 2 || op == 3) {
        const bool left = op == 2;
        code += "        if (amount < 32) {\n"
                "            cpu.cpsr.C = ((value >> " +
                std::string{left ? "(32u - amount)" : "(amount - 1u)"} + " ) & 1u) != 0;\n"
                "            result = value " + (left ? "<<" : ">>") + " amount;\n"
                "        } else {\n"
                "            cpu.cpsr.C = amount == 32 && " +
                (left ? "(value & 1u) != 0" : "(value >> 31) != 0") + ";\n"
                "            result = 0;\n        }\n";
    } else if (op == 4) {
        code += "        const bool negative = (value >> 31) != 0;\n"
                "        if (amount < 32) {\n"
                "            cpu.cpsr.C = ((value >> (amount - 1u)) & 1u) != 0;\n"
                "            result = (value >> amount) | (negative ? (0xFFFFFFFFu << (32u - amount)) : 0u);\n"
                "        } else {\n"
                "            cpu.cpsr.C = negative;\n"
                "            result = negative ? 0xFFFFFFFFu : 0u;\n        }\n";
    } else {
        code += "        const unsigned rotate = amount & 31u;\n"
                "        result = rotate == 0 ? value : (value >> rotate) | (value << (32u - rotate));\n"
                "        cpu.cpsr.C = (result >> 31) != 0;\n";
    }
    return code + "    }\n    cpu.UpdateNZ(result);\n    " + Reg(rd) + " = result;\n}";
}

} // namespace

std::vector<Instruction> ThumbTranslator::DecodeBuffer(
    std::span<const std::uint8_t> buffer, std::uint32_t base_address) const {
    if ((base_address & 1u) != 0 || (buffer.size() & 1u) != 0) {
        throw std::invalid_argument("Thumb code requires aligned, complete 2-byte instructions");
    }
    if (!buffer.empty() && buffer.size() - 1 >
        std::numeric_limits<std::uint32_t>::max() - base_address) {
        throw std::out_of_range("Thumb code exceeds the guest address space");
    }
    std::vector<Instruction> instructions;
    instructions.reserve(buffer.size() / 2);
    for (std::size_t offset = 0; offset < buffer.size(); offset += 2) {
        const auto op = std::uint32_t{buffer[offset]} | (std::uint32_t{buffer[offset + 1]} << 8);
        if ((op & 0xF800u) >= 0xE800u) {
            throw std::invalid_argument("T32/Thumb long-branch encodings are outside the T16 subset");
        }
        instructions.push_back({op, base_address + static_cast<std::uint32_t>(offset), InstructionSet::Thumb});
    }
    return instructions;
}

std::string ThumbTranslator::TranslateInstruction(std::uint16_t opcode,
                                                  std::uint32_t current_pc) const {
    const std::uint32_t op = opcode;
    if ((current_pc & 1u) != 0) throw std::invalid_argument("Thumb PC must be halfword aligned");
    if ((op & 0xE000u) == 0x2000u) {
        constexpr unsigned alu[] = {13, 10, 4, 2};
        const auto rd = (op >> 8) & 7u;
        const auto arm = 0xE2100000u | (alu[(op >> 11) & 3u] << 21) |
                         (rd << 16) | (rd << 12) | (op & 0xFFu);
        return ALUTranslator{}.TranslateInstruction({arm, current_pc});
    }
    if ((op & 0xFC00u) == 0x4000u) {
        const auto alu = (op >> 6) & 15u;
        const auto rd = op & 7u;
        const auto rm = (op >> 3) & 7u;
        if (alu == 2 || alu == 3 || alu == 4 || alu == 7) return Shift(alu, rd, rm);
        if (alu == 5 || alu == 6) return Arithmetic(Reg(rd), Reg(rm), alu == 6, true, Reg(rd));
        if (alu == 9) return Arithmetic("0u", Reg(rm), true, false, Reg(rd));
        if (alu == 10 || alu == 11) return Arithmetic(Reg(rd), Reg(rm), alu == 10, false, "");
        std::string expression;
        switch (alu) {
        case 0: case 8: expression = Reg(rd) + " & " + Reg(rm); break;
        case 1: expression = Reg(rd) + " ^ " + Reg(rm); break;
        case 12: expression = Reg(rd) + " | " + Reg(rm); break;
        case 13: expression = Reg(rd) + " * " + Reg(rm); break;
        case 14: expression = Reg(rd) + " & ~" + Reg(rm); break;
        case 15: expression = "~" + Reg(rm); break;
        default: return "// Unhandled Thumb Instruction";
        }
        std::string code = "{\n    const std::uint32_t result = " + expression + ";\n"
                           "    cpu.UpdateNZ(result);\n";
        if (alu != 8) code += "    " + Reg(rd) + " = result;\n";
        return code + "}";
    }
    if ((op & 0xE000u) == 0x6000u && (op & 0x1000u) == 0) {
        const auto arm = ((op & 0x800u) != 0 ? 0xE5900000u : 0xE5800000u) |
                         (((op >> 3) & 7u) << 16) | ((op & 7u) << 12) |
                         (((op >> 6) & 31u) << 2);
        return MemoryTranslator{}.TranslateInstruction({arm, current_pc});
    }
    if ((op & 0xFE00u) == 0xB400u || (op & 0xFE00u) == 0xBC00u) {
        const bool pop = (op & 0x800u) != 0;
        const auto list = (op & 0xFFu) | ((op & 0x100u) != 0 ? (pop ? 0x8000u : 0x4000u) : 0u);
        const auto code = MemoryTranslator{}.TranslateInstruction(
            {(pop ? 0xE8BD0000u : 0xE92D0000u) | list, current_pc});
        return code == "// Unhandled Instruction" ? "// Unhandled Thumb Instruction" : code;
    }
    if (const auto target = BranchTarget(op, current_pc)) {
        const auto branch = "cpu.r[15] = " + std::to_string(*target) + "u;\nreturn;";
        if (!IsConditionalBranch(op)) return branch;
        return "if (" + ConditionEvaluator::GetConditionString(static_cast<std::uint8_t>((op >> 8) & 15u)) +
               ") {\n    cpu.r[15] = " + std::to_string(*target) + "u;\n    return;\n}";
    }
    if (IsExchange(op)) {
        const auto rm = (op >> 3) & 15u;
        std::string code = "{\n    const std::uint32_t target = " +
            (rm == 15 ? std::to_string(current_pc + 4u) + "u" : Reg(rm)) + ";\n";
        if ((op & 0x80u) != 0) code += "    cpu.r[14] = " + std::to_string((current_pc + 2u) | 1u) + "u;\n";
        return code + "    cpu.BranchExchange(target);\n    return;\n}";
    }
    return "// Unhandled Thumb Instruction";
}

} // namespace arm11
