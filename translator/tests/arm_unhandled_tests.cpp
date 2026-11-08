#include "cpp_emitter.hpp"
#include "cfg_builder.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string EmitSingle(const char* path, std::uint32_t word, std::uint32_t address) {
    const std::vector<arm11::Instruction> instructions{{word, address}};
    if (!arm11::CppEmitter{}.EmitTranslatedFile(path, arm11::CFGBuilder{}.BuildBlocks(instructions)))
        throw std::runtime_error("emit failed");
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Returns the full line that contains the position, without its terminator.
std::string LineAt(const std::string& text, std::size_t at) {
    const auto start = text.rfind('\n', at);
    const auto begin = start == std::string::npos ? std::size_t{0} : start + 1;
    const auto end = text.find('\n', at);
    return text.substr(begin, end - begin);
}

std::string PreviousLine(const std::string& text, std::size_t at) {
    const auto start = text.rfind('\n', at);
    return LineAt(text, start - 1);
}

std::string NextLine(const std::string& text, std::size_t at) {
    const auto end = text.find('\n', at);
    return LineAt(text, end + 1);
}

// An unhandled ARM word must become a runtime trap, never a silent comment.
// Conditional words put the trap inside the condition guard; cond 0xF words run it unconditionally.
void CheckTrapped(const std::string& name, const std::string& text, const std::string& address,
                  bool conditional) {
    const std::string trap = "throw std::runtime_error(\"unsupported ARM instruction at 0x" + address + "\");";
    const auto at = text.find(trap);
    if (at == std::string::npos) throw std::runtime_error(name + ": no trap emitted");
    if (text.find("Unhandled") != std::string::npos)
        throw std::runtime_error(name + ": silent comment emitted");
    const auto previous = PreviousLine(text, at);
    const bool guarded = previous.find("if (") != std::string::npos;
    if (guarded != conditional)
        throw std::runtime_error(name + ": trap guard does not match the instruction condition");
    if (conditional && previous.find("if (cpu.cpsr.Z) {") == std::string::npos)
        throw std::runtime_error(name + ": trap is not under the EQ guard");
    if (conditional && NextLine(text, at) != "    }")
        throw std::runtime_error(name + ": trap is not the only statement under the guard");
}

// A translatable word must keep its translation and must not trap.
void CheckTranslated(const std::string& name, const std::string& text, const std::string& expected) {
    if (text.find("throw") != std::string::npos)
        throw std::runtime_error(name + ": translatable instruction traps");
    if (text.find(expected) == std::string::npos)
        throw std::runtime_error(name + ": translation missing: " + expected);
}
} // namespace

int main() {
    const auto unconditional = EmitSingle("build/arm_unhandled_unconditional.cpp", 0xE8BD0000u, 0x08000000u);
    CheckTrapped("unconditional", unconditional, "08000000", false);

    const auto relocated = EmitSingle("build/arm_unhandled_address.cpp", 0xE8BD0000u, 0x08000010u);
    CheckTrapped("address", relocated, "08000010", false);

    const auto conditional = EmitSingle("build/arm_unhandled_conditional.cpp", 0x08BD0000u, 0x08000000u);
    CheckTrapped("conditional", conditional, "08000000", true);

    const auto never = EmitSingle("build/arm_unhandled_never.cpp", 0xF8BD0000u, 0x08000000u);
    CheckTrapped("cond-F", never, "08000000", false);

    CheckTranslated("mov", EmitSingle("build/arm_unhandled_mov.cpp", 0xE3A00001u, 0x08000000u),
                    "cpu.r[0] = 0x1u;");
    CheckTranslated("ldr", EmitSingle("build/arm_unhandled_ldr.cpp", 0xE5910000u, 0x08000000u),
                    "memory.Read32(adjusted)");
    return 0;
}
