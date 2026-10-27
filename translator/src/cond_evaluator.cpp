#include "cond_evaluator.hpp"

#include <array>
#include <stdexcept>
#include <string_view>

namespace arm11 {

std::string ConditionEvaluator::GetConditionString(std::uint8_t cond_code) {
    static constexpr std::array<std::string_view, 16> conditions{
        "cpu.cpsr.Z",                              // EQ
        "!cpu.cpsr.Z",                             // NE
        "cpu.cpsr.C",                              // CS/HS
        "!cpu.cpsr.C",                             // CC/LO
        "cpu.cpsr.N",                              // MI
        "!cpu.cpsr.N",                             // PL
        "cpu.cpsr.V",                              // VS
        "!cpu.cpsr.V",                             // VC
        "(cpu.cpsr.C && !cpu.cpsr.Z)",               // HI
        "(!cpu.cpsr.C || cpu.cpsr.Z)",               // LS
        "(cpu.cpsr.N == cpu.cpsr.V)",                // GE
        "(cpu.cpsr.N != cpu.cpsr.V)",                // LT
        "(!cpu.cpsr.Z && (cpu.cpsr.N == cpu.cpsr.V))", // GT
        "(cpu.cpsr.Z || (cpu.cpsr.N != cpu.cpsr.V))",  // LE
        "",                                        // AL
        "false",                                   // NV/reserved in this emitter
    };
    if (cond_code >= conditions.size()) {
        throw std::invalid_argument("ARM condition code must fit in four bits");
    }
    return std::string{conditions[cond_code]};
}

} // namespace arm11
