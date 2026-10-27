#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace arm11 {

// Owns the exact file bytes; ELF parsing and executable mapping are separate.
class BinaryLoader {
public:
    // Returns false and logs to std::cerr on failure, leaving an empty buffer.
    // Empty files are valid and produce an empty buffer.
    [[nodiscard]] bool LoadFile(const std::string& filepath);

    // Borrowed view: invalidated by LoadFile(), destruction, or owner assignment.
    [[nodiscard]] std::span<const std::uint8_t> GetCodeBuffer() const noexcept {
        return code_;
    }

private:
    std::vector<std::uint8_t> code_;
};

} // namespace arm11
