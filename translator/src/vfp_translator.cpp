#include "vfp_translator.hpp"
#include <stdexcept>

namespace arm11 {
namespace {
std::string R(unsigned r) { return "cpu.r[" + std::to_string(r) + "]"; }
std::string Bits(unsigned s) { return "cpu.vfp_bits[" + std::to_string(s) + "]"; }
unsigned Register(unsigned field, unsigned extension, bool dp) {
    const auto index = dp ? field + extension * 16u : field * 2u + extension;
    if (dp && index >= 16) throw std::runtime_error("ARM11 VFPv2 has only D0-D15");
    return index;
}
std::string Get(unsigned reg, bool dp) { return "cpu.VFPInput(cpu.Get" + std::string{dp ? "D(" : "S("} + std::to_string(reg) + "))"; }
std::string Set(unsigned reg, bool dp, const std::string& value) {
    return "cpu.Set" + std::string{dp ? "D(" : "S("} + std::to_string(reg) + ", cpu.VFPOutput(" + value + "));";
}
std::string Operation(const std::string& body) { return "cpu.VFPOperation([&] { " + body + " });"; }
} // namespace

std::string VFPTranslator::TranslateInstruction(const Instruction& inst) const {
    const auto op = inst.opcode;
    constexpr auto unhandled = "// Unhandled VFP Instruction";
    if (!IsEncoding(op) || inst.state != InstructionSet::ARM || (op >> 28) == 15) return unhandled;
    const bool dp = (op & 0x100u) != 0;
    if ((op & 0x0FEF0FFFu) == 0x0EE10A10u) {
        const auto rt = (op >> 12) & 15u;
        const bool read = (op & (1u << 20)) != 0;
        if (rt == 15) {
            if (!read) return unhandled;
            return "cpu.cpsr.N = (cpu.fpscr >> 31) != 0;\ncpu.cpsr.Z = ((cpu.fpscr >> 30) & 1u) != 0;\n"
                   "cpu.cpsr.C = ((cpu.fpscr >> 29) & 1u) != 0;\ncpu.cpsr.V = ((cpu.fpscr >> 28) & 1u) != 0;";
        }
        return read ? R(rt) + " = cpu.fpscr;" : "cpu.fpscr = " + R(rt) + ";";
    }
    if ((op & 0x0FE00F7Fu) == 0x0E000A10u) {
        const auto rt = (op >> 12) & 15u;
        if (rt == 15) return unhandled;
        const auto s = ((op >> 16) & 15u) * 2u + ((op >> 7) & 1u);
        return (op & (1u << 20)) != 0 ? R(rt) + " = " + Bits(s) + ";" : Bits(s) + " = " + R(rt) + ";";
    }
    const bool precision_conversion = (op & 0x0FBF0ED0u) == 0x0EB70AC0u;
    const auto vd = Register((op >> 12) & 15u, (op >> 22) & 1u, precision_conversion ? !dp : dp);
    if ((op & 0x0E000E00u) == 0x0C000A00u) {
        const bool pre = (op & (1u << 24)) != 0, up = (op & (1u << 23)) != 0;
        const bool wb = (op & (1u << 21)) != 0, load = (op & (1u << 20)) != 0;
        const auto rn = (op >> 16) & 15u, words = op & 255u;
        const bool single = pre && !wb;
        if (!single && !((!pre && up) || (pre && !up && wb))) return unhandled;
        if (!single && (rn == 15 || words == 0 || (dp && (words & 1u) != 0))) return unhandled;
        const auto start = dp ? vd * 2u : vd;
        const auto count = single ? (dp ? 2u : 1u) : words;
        if (start + count > 32) return unhandled;
        const auto base = rn == 15 ? std::to_string((inst.address + 8u) & ~3u) + "u" : R(rn);
        std::string code = "{\n    const std::uint32_t base = " + base + ";\n    std::uint32_t address = base";
        if (single || !up) code += (up ? " + " : " - ") + std::to_string(words * 4u) + "u";
        code += ";\n";
        for (unsigned i = 0; i < count; ++i) {
            code += load ? "    " + Bits(start + i) + " = memory.Read32(address);\n" :
                           "    memory.Write32(address, " + Bits(start + i) + ");\n";
            code += "    address += 4u;\n";
        }
        if (!single && wb) code += "    " + R(rn) + " = base " + (up ? "+ " : "- ") + std::to_string(words * 4u) + "u;\n";
        return code + "}";
    }
    if ((op & 0x0F000E10u) != 0x0E000A00u) return unhandled;
    const auto vm = Register(op & 15u, (op >> 5) & 1u, dp);
    const auto operation = (op >> 20) & 0xBu;
    if (operation == 2 || operation == 3 || (operation == 8 && (op & 0x40u) == 0)) {
        if (operation == 2 && (op & 0x40u) != 0) return unhandled;
        const auto vn = Register((op >> 16) & 15u, (op >> 7) & 1u, dp);
        const char* symbol = operation == 2 ? " * " : operation == 8 ? " / " : (op & 0x40u) != 0 ? " - " : " + ";
        return Operation(Set(vd, dp, Get(vn, dp) + symbol + Get(vm, dp)));
    }
    if (operation != 0xB || (op & 0x40u) == 0) return unhandled;
    const auto unary = (op >> 16) & 15u;
    const bool bit7 = (op & 0x80u) != 0;
    if (unary <= 1) {
        if (unary == 0 || !bit7) {
            // Register move/ABS/NEG affect raw signs, not NaN payloads or FPSCR.
            const auto mask = dp ? "0x8000000000000000ull" : "0x80000000u";
            const auto type = dp ? "std::uint64_t" : "std::uint32_t";
            const auto value = "std::bit_cast<" + std::string{type} + ">(cpu.Get" + (dp ? "D(" : "S(") + std::to_string(vm) + "))";
            const auto expression = unary == 0 ? (bit7 ? value + " & ~" + mask : value) : value + " ^ " + mask;
            return "cpu.Set" + std::string{dp ? "D(" : "S("} + std::to_string(vd) + ", std::bit_cast<" + (dp ? "double" : "float") + ">(" + expression + "));";
        }
        return Operation(Set(vd, dp, "std::sqrt(" + Get(vm, dp) + ")"));
    }
    if (unary == 7 && bit7) {
        const auto destination = Register((op >> 12) & 15u, (op >> 22) & 1u, !dp);
        return Operation(Set(destination, !dp, "static_cast<" + std::string{dp ? "float" : "double"} + ">(" + Get(vm, dp) + ")"));
    }
    if (unary == 8 && bit7 && !dp) {
        const auto source = (op & 15u) * 2u + ((op >> 5) & 1u);
        return Operation(Set(vd, false, "static_cast<float>(std::bit_cast<std::int32_t>(" + Bits(source) + "))"));
    }
    if (unary == 13 && !dp)
        return Operation(Bits(vd) + " = cpu.VFPToS32(" + Get(vm, false) + ", " + (bit7 ? "true" : "false") + ");");
    return unhandled;
}
} // namespace arm11
