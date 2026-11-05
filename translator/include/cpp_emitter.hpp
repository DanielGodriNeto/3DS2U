#pragma once

#include "cfg_builder.hpp"
#include "progress_sink.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace arm11 {

class BinaryLoader;

// Unit budget for EmitTranslatedUnits. Blocks count by instructions.size().
struct UnitOptions {
    std::uint32_t max_instructions_per_unit{};
};

class CppEmitter {
public:
    // Writes WiiUMemory block functions and global InitDispatcher(WiiUEngine&).
    // Returns false and reports I/O errors to std::cerr; output may be partial.
    // With an image, the entry must have exactly one ARM or Thumb block; otherwise
    // it returns false before opening output_filename.
    // Instruction semantics are limited to those of the current translators.
    [[nodiscard]] bool EmitTranslatedFile(
        const std::string& output_filename,
        const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
        const BinaryLoader* image = nullptr) const;

    // Splits the output into bounded translation units.
    // Files published: main_output_path (includes, image data, InitDispatcher) and
    // one <stem>_blocks_NNNN.cpp per unit in the same directory, where stem is the
    // main file name without a final ".cpp" and NNNN is %04u starting at 0000.
    // Blocks are packed in ascending key order: a unit takes the next block while
    // its instruction count stays within options.max_instructions_per_unit, and a
    // block is never split (an oversized block gets its own unit).
    // Determinism: output depends only on cfg, image, and the budget.
    // Staging: every file is first written as <name>.3ds2u-tmp, read back and checked
    // against its plan, then published (identical content leaves the final untouched).
    // Stale <stem>_blocks_NNNN.cpp regular files not in this set are removed; any other
    // match or non-regular final refuses the run before anything is changed.
    // Failure: returns false and reports "CppEmitter: <reason>" to std::cerr.
    // A budget of 0 or more than 10000 units, a CFG key mismatch, and a missing or
    // ambiguous image entry are rejected before any file is opened. A failed run removes
    // its staged files; a rename failure removes the finals it replaced. Stale removal
    // is not undone.
    // Progress: an empty sink prints nothing. Otherwise the sink receives plan-units,
    // emit-units, emit-dispatcher, verify-registration, publish, and complete events,
    // or one failed event naming the phase that failed.
    [[nodiscard]] bool EmitTranslatedUnits(
        const std::string& main_output_path,
        const std::unordered_map<std::uint32_t, BasicBlock>& cfg,
        const BinaryLoader* image,
        UnitOptions options,
        const ProgressSink& progress) const;
};

} // namespace arm11
