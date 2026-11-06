// Writes case_<n>.cpp (from WriteRomFSSource) and driver_<n>.cpp (independent
// decimal oracle) into argv[1]. A shell step compiles and runs each pair.
#include "romfs_source.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

void WriteCase(const std::string& dir, int n, const std::vector<std::uint8_t>& bytes) {
    std::ostringstream generated;
    extractor::WriteRomFSSource(generated, bytes);
    const auto text = generated.str();
    if (!bytes.empty()) {
        Check(text.find("0x") == std::string::npos, "array-style output");
        Check(text.find("sizeof(image) - 1") != std::string::npos, "span must exclude the NUL");
    }
    std::ofstream(dir + "/case_" + std::to_string(n) + ".cpp") << text;

    std::ofstream driver(dir + "/driver_" + std::to_string(n) + ".cpp");
    driver << "#include \"packaged_romfs.hpp\"\n#include <cstdint>\n"
              "static const std::uint8_t expected[] = {0";
    for (const auto b : bytes) driver << ',' << static_cast<unsigned>(b);
    driver << "};\nint main() {\n    const auto got = wiiu::GetPackagedRomFS();\n"
              "    if (got.size() != " << bytes.size() << "u) return 1;\n"
              "    for (std::size_t i = 0; i < got.size(); ++i) if (got[i] != expected[i + 1]) return 2;\n"
              "    return 0;\n}\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "usage: romfs_source_tests <work-dir>");
        const std::string dir = argv[1];
        int n = 0;
        std::uint32_t state = 12345;
        for (const std::size_t size : {0, 1, 15, 16, 17, 63, 64, 65, 128, 4097}) {
            std::vector<std::uint8_t> bytes(size);
            for (auto& b : bytes) { state = state * 1664525u + 1013904223u; b = static_cast<std::uint8_t>(state >> 24); }
            WriteCase(dir, n++, bytes);
        }
        std::vector<std::uint8_t> all(256), desc(256);
        for (std::size_t i = 0; i < 256; ++i) { all[i] = static_cast<std::uint8_t>(i); desc[i] = static_cast<std::uint8_t>(255 - i); }
        WriteCase(dir, n++, all);
        WriteCase(dir, n++, desc);
        std::cout << "cases=" << n << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
