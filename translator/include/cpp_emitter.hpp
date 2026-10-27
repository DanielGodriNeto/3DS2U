#pragma once

#include "cfg_builder.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace arm11 {

class CppEmitter {
public:
    // Writes WiiUMemory block functions and global InitDispatcher(WiiUEngine&).
    // Returns false and reports I/O errors to std::cerr; output may be partial.
    // Instruction semantics are limited to those of the current translators.
    [[nodiscard]] bool EmitTranslatedFile(
        const std::string& output_filename,
        const std::unordered_map<std::uint32_t, BasicBlock>& cfg) const;
};

} // namespace arm11
