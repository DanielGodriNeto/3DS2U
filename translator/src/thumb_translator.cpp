#include "thumb_decoder.hpp"
#include "alu_translator.hpp"
#include "cond_evaluator.hpp"
#include "mem_translator.hpp"

#include <limits>
#include <stdexcept>
#include <bit>

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
    for (std::size_t offset = 0; offset < buffer.size();) {
        auto op = std::uint32_t{buffer[offset]} | (std::uint32_t{buffer[offset + 1]} << 8);
        const auto width = IsWidePrefix(op) ? 4u : 2u;
        if (width == 4) {
            if (buffer.size() - offset < 4) throw std::invalid_argument("Truncated T32 instruction");
            op = (op << 16) | buffer[offset + 2] | (std::uint32_t{buffer[offset + 3]} << 8);
        }
        instructions.push_back({op, base_address + static_cast<std::uint32_t>(offset), InstructionSet::Thumb});
        offset += width;
    }
    return instructions;
}

std::string ThumbTranslator::TranslateInstruction(const Instruction& inst) const {
    if (inst.opcode <= 0xFFFFu) return TranslateInstruction(inst.opcode, inst.address);
    const auto op = inst.opcode, first = op >> 16, second = op & 0xFFFFu;
    constexpr auto unhandled = "// Unhandled Thumb Instruction";
    if (inst.state != InstructionSet::Thumb || (inst.address & 1u) != 0) return unhandled;
    if (const auto target = BranchTarget(op, inst.address)) {
        std::string result;
        if ((second & 0xC000u) == 0xC000u)
            result += "cpu.r[14] = " + std::to_string((inst.address + 4u) | 1u) + "u;\n";
        return result + "cpu.BranchExchange(" + std::to_string(*target | (IsBLXImmediate(op) ? 0u : 1u)) + "u);\nreturn;";
    }
    const auto rd = (second >> 8) & 15u, rn = first & 15u, rm = second & 15u;
    const bool set_flags = (first & 16u) != 0;
    const auto arithmetic = [&](const std::string& rhs, bool subtract) {
        if (rd == 15 || rn == 15) return std::string{unhandled};
        return set_flags ? Arithmetic(Reg(rn), rhs, subtract, false, Reg(rd)) :
            Reg(rd) + " = " + Reg(rn) + (subtract ? " - " : " + ") + rhs + ";";
    };
    const auto move = [&](const std::string& value, const std::string& carry) {
        if (rd >= 15) return std::string{unhandled};
        return "{\n    const std::uint32_t value = " + value + ";\n" +
            (set_flags ? "    const bool carry = " + carry + ";\n    cpu.UpdateNZ(value);\n    cpu.cpsr.C = carry;\n" : "") +
            "    " + Reg(rd) + " = value;\n}";
    };
    if (((first & 0xFFE0u) == 0xFA00u || (first & 0xFFE0u) == 0xFA20u) && (second & 0xF0F0u) == 0xF000u) {
        if (rd >= 15 || rn >= 13 || rm >= 13) return unhandled;
        const bool left = (first & 0x20u) == 0;
        return "{\n    const std::uint32_t source = " + Reg(rn) + ";\n    const unsigned amount = " + Reg(rm) +
            " & 255u;\n    std::uint32_t result = source;\n    bool carry = cpu.cpsr.C;\n"
            "    if (amount != 0) {\n        if (amount < 32) {\n            result = source " + (left ? "<<" : ">>") +
            " amount;\n            carry = ((source >> " + (left ? "(32u - amount)" : "(amount - 1u)") +
            ") & 1u) != 0;\n        } else {\n            result = 0;\n            carry = amount == 32 && " +
            (left ? "(source & 1u) != 0" : "(source >> 31) != 0") + ";\n        }\n    }\n" +
            (set_flags ? "    cpu.UpdateNZ(result);\n    cpu.cpsr.C = carry;\n" : "    (void)carry;\n") +
            "    " + Reg(rd) + " = result;\n}";
    }
    if ((second & 0x8000u) == 0 && ((first & 0xFFE0u) == 0xEA40u ||
        (first & 0xFFE0u) == 0xEB00u || (first & 0xFFE0u) == 0xEBA0u)) {
        if (rm >= 13) return unhandled;
        const auto kind = (second >> 4) & 3u;
        auto amount = (((second >> 12) & 7u) << 2) | ((second >> 6) & 3u);
        if (kind > 1) return unhandled;
        if (kind == 1 && amount == 0) amount = 32;
        std::string value = Reg(rm), carry = "cpu.cpsr.C";
        if (amount != 0) {
            carry = "((" + Reg(rm) + " >> " + std::to_string(kind == 0 ? 32u - amount : amount - 1u) + "u) & 1u) != 0";
            value = amount == 32 ? "0u" : "(" + Reg(rm) + (kind == 0 ? " << " : " >> ") + std::to_string(amount) + "u)";
        }
        if ((first & 0xFFE0u) == 0xEA40u) return rn == 15 ? move(value, carry) : unhandled;
        return arithmetic(value, (first & 0xFFE0u) == 0xEBA0u);
    }
    const auto immediate = ((first & 0x400u) << 1) | ((second >> 12) & 7u) << 8 | (second & 255u);
    if ((second & 0x8000u) == 0 && ((first & 0xFBE0u) == 0xF040u ||
        (first & 0xFBE0u) == 0xF100u || (first & 0xFBE0u) == 0xF1A0u)) {
        std::uint32_t expanded = immediate & 255u;
        bool rotated = false;
        if ((immediate & 0xC00u) == 0) {
            const auto replication = (immediate >> 8) & 3u;
            if (replication != 0 && expanded == 0) return unhandled;
            if (replication == 1) expanded |= expanded << 16;
            if (replication == 2) expanded = (expanded << 8) | (expanded << 24);
            if (replication == 3) expanded *= 0x01010101u;
        } else {
            expanded = std::rotr(0x80u | (immediate & 127u), static_cast<int>((immediate >> 7) & 31u));
            rotated = true;
        }
        const auto value = std::to_string(expanded) + "u";
        if ((first & 0xFBE0u) == 0xF040u)
            return rn == 15 ? move(value, rotated ? ((expanded >> 31) != 0 ? "true" : "false") : "cpu.cpsr.C") : unhandled;
        return arithmetic(value, (first & 0xFBE0u) == 0xF1A0u);
    }
    if ((second & 0x8000u) == 0 && ((first & 0xFBF0u) == 0xF200u || (first & 0xFBF0u) == 0xF2A0u)) {
        if (rd == 15) return unhandled;
        const auto base = rn == 15 ? std::to_string((inst.address + 4u) & ~3u) + "u" : Reg(rn);
        return Reg(rd) + " = " + base + ((first & 0xA0u) != 0 ? " - " : " + ") + std::to_string(immediate) + "u;";
    }
    if ((second & 0x8000u) == 0 && ((first & 0xFBF0u) == 0xF240u || (first & 0xFBF0u) == 0xF2C0u)) {
        if (rd >= 15) return unhandled;
        const auto value = (rn << 12) | immediate;
        return Reg(rd) + " = " + ((first & 0x80u) != 0 ? "(" + Reg(rd) + " & 0xFFFFu) | " + std::to_string(value << 16) : std::to_string(value)) + "u;";
    }
    const auto head = first & 0xFFF0u;
    const bool positive = head == 0xF8D0u || head == 0xF8C0u || head == 0xF890u || head == 0xF880u;
    const bool other = head == 0xF850u || head == 0xF840u || head == 0xF810u || head == 0xF800u;
    if (positive || other) {
        const bool load = (first & 16u) != 0, byte = (first & 0x40u) == 0;
        const auto rt = second >> 12;
        if (rt == 15 && (!load || byte)) return unhandled;
        bool pre = true, up = true, wb = false;
        std::string offset = std::to_string(second & 0xFFFu) + "u";
        if (rn == 15) {
            if (!load) return unhandled;
            up = (first & 0x80u) != 0;
        } else if (!positive) {
            if ((second & 0x800u) != 0) {
                pre = (second & 0x400u) != 0; up = (second & 0x200u) != 0; wb = (second & 0x100u) != 0;
                if ((!pre && !wb) || (wb && rn == rt)) return unhandled;
                offset = std::to_string(second & 255u) + "u";
            } else {
                if ((second & 0xFC0u) != 0 || rm >= 13) return unhandled;
                offset = "(" + Reg(rm) + " << " + std::to_string((second >> 4) & 3u) + "u)";
            }
        }
        const auto base = rn == 15 ? std::to_string((inst.address + 4u) & ~3u) + "u" : Reg(rn);
        std::string code = "{\n    const std::uint32_t base = " + base + ";\n    const std::uint32_t adjusted = base " +
            (up ? "+ " : "- ") + offset + ";\n";
        if (!load) code += "    const std::uint32_t value = " + Reg(rt) + ";\n";
        if (wb && pre) code += "    " + Reg(rn) + " = adjusted;\n";
        const auto address = pre ? "adjusted" : "base";
        if (load) code += "    const std::uint32_t value = memory.Read" + std::string{byte ? "8(" : "32("} + address + ");\n";
        else code += "    memory.Write" + std::string{byte ? "8(" : "32("} + address + ", " +
            (byte ? "static_cast<std::uint8_t>(value)" : "value") + ");\n";
        if (load && rt != 15) code += "    " + Reg(rt) + " = value;\n";
        if (wb && !pre) code += "    " + Reg(rn) + " = adjusted;\n";
        if (load && rt == 15) code += "    cpu.BranchExchange(value);\n    return;\n";
        return code + "}";
    }
    return unhandled;
}

std::string ThumbTranslator::TranslateInstruction(std::uint32_t opcode,
                                                  std::uint32_t current_pc) const {
    if (opcode > 0xFFFFu) return TranslateInstruction({opcode, current_pc, InstructionSet::Thumb});
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
