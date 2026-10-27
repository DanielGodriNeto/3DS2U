#include "binary_loader.hpp"

#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <utility>

namespace arm11 {

bool BinaryLoader::LoadFile(const std::string& filepath) {
    code_.clear(); // A failed load must never expose a previous executable.

    const auto fail = [&filepath](const char* message) {
        std::cerr << "BinaryLoader: " << message << ": " << filepath << '\n';
        return false;
    };

    try {
        // RAII closes the stream on every return path; ate allows size discovery.
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return fail("cannot open file");
        }

        const std::streamoff file_size = file.tellg();
        if (file_size < 0) {
            return fail("cannot determine file size");
        }

        // Check both allocation and stream limits before narrowing the size.
        std::vector<std::uint8_t> data;
        const auto size = static_cast<std::uintmax_t>(file_size);
        if (size > data.max_size() ||
            size > static_cast<std::uintmax_t>(
                       std::numeric_limits<std::streamsize>::max())) {
            return fail("file is too large");
        }

        file.seekg(0, std::ios::beg);
        if (!file) {
            return fail("cannot seek to start of file");
        }

        data.resize(static_cast<std::size_t>(size));
        if (!data.empty()) {
            const auto count = static_cast<std::streamsize>(size);
            file.read(reinterpret_cast<char*>(data.data()), count);
            if (!file || file.gcount() != count) {
                return fail("cannot read entire file");
            }
        }

        code_ = std::move(data); // Publish only a complete, successful read.
        return true;
    } catch (const std::exception& error) {
        // Includes allocation failures and stream-related exceptions.
        std::cerr << "BinaryLoader: " << error.what() << ": " << filepath << '\n';
        return false;
    }
}

} // namespace arm11
