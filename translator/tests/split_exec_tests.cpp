// Host execution tests for translated output (plan tests 21-22). For each fixture the translator
// emits unsplit output and split output (--split-instructions 2). The compiler builds each variant
// with split_exec_driver.cpp under strict flags, and the two driver dumps must match byte for byte.
// argv[1]: translator executable. argv[2]: scratch work directory. argv[3]: repository root.
// argv[4]: C++ compiler command (for example g++).
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kEntry = 0x00100000u;

struct Paths {
    fs::path translator;
    fs::path work;
    fs::path root;
    std::string compiler;
};

struct Fixture {
    std::vector<std::uint32_t> words;
    int expected_exit{};    // driver exit code of the unsplit run
    bool expect_late_svc{}; // the SVC call must sit in a unit other than 0000
};

struct TestCase {
    std::string name;
    Fixture fixture;
};

void Check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

std::string Quote(const fs::path& path) { return "\"" + path.string() + "\""; }

std::string ExeSuffix() {
#ifdef _WIN32
    return ".exe";
#else
    return "";
#endif
}

// Runs a shell command line and returns its exit code. cmd.exe strips one outer quote pair, so Windows wraps the line once more.
int RunShell(const std::string& command) {
#ifdef _WIN32
    const std::string wrapped = "\"" + command + "\"";
    return std::system(wrapped.c_str());
#else
    const int status = std::system(command.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

void Append32(Bytes& bytes, std::uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(word >> (i * 8)));
}

void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8));
}

void SaveBytes(const fs::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Check(file.is_open(), "cannot write " + path.string());
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// Decrypted ExHeader with the entry text at kEntry (same layout as split_cli_tests.cpp).
Bytes MakeExHeader(std::uint32_t text_size) {
    Bytes header(0x400, 0);
    Put32(header, 0x10, kEntry);
    Put32(header, 0x14, (text_size + 0xFFFu) / 0x1000u);
    Put32(header, 0x18, text_size);
    return header;
}

// Unconditional ARM branch at `from` to `to` (forward or self), encoded as B with imm24.
std::uint32_t BranchWord(std::uint32_t from, std::uint32_t to) {
    return 0xEA000000u | (((to - from - 8u) >> 2) & 0xFFFFFFu);
}

fs::path WriteFixture(const Paths& paths, const std::string& name, const std::vector<std::uint32_t>& words) {
    const auto dir = paths.work / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    Bytes code;
    for (const auto word : words) Append32(code, word);
    SaveBytes(dir / "image.code", code);
    SaveBytes(dir / "image.exheader", MakeExHeader(static_cast<std::uint32_t>(code.size())));
    return dir;
}

// Compiles every generated .cpp in out_dir with the driver under strict flags.
void BuildDriver(const Paths& paths, const fs::path& out_dir, const fs::path& driver) {
    std::vector<fs::path> sources;
    for (const auto& entry : fs::directory_iterator(out_dir)) {
        if (entry.path().extension() == ".cpp") sources.push_back(entry.path());
    }
    std::sort(sources.begin(), sources.end());
    sources.push_back(paths.root / "translator" / "tests" / "split_exec_driver.cpp");
    const auto support = paths.root / "translator" / "tests" / "support";
    const auto include = paths.root / "translator" / "include";
    std::string command = Quote(paths.compiler) + " -std=c++20 -Wall -Wextra -Werror -I" +
                          Quote(support) + " -I" + Quote(include);
    for (const auto& source : sources) command += " " + Quote(source);
    const auto log = out_dir / "compile.log";
    command += " -o " + Quote(driver) + " > " + Quote(log) + " 2>&1";
    Check(RunShell(command) == 0, "driver build failed: " + ReadText(log));
}

// Translates one variant, builds it with the driver, runs it, and returns the driver exit code.
int ExecuteVariant(const Paths& paths, const fs::path& dir, const std::string& variant, const std::string& options) {
    const auto out_dir = dir / variant;
    fs::create_directories(out_dir);
    const auto output = out_dir / "translated_game.cpp";
    const auto translate_log = out_dir / "translate.log";
    const std::string translate = Quote(paths.translator) + " " + Quote(dir / "image.code") + " " +
                                  Quote(output) + " --exheader " + Quote(dir / "image.exheader") + options +
                                  " > " + Quote(translate_log) + " 2>&1";
    Check(RunShell(translate) == 0, "translation failed for " + variant + ": " + ReadText(translate_log));
    const auto driver = out_dir / ("driver_bin" + ExeSuffix());
    BuildDriver(paths, out_dir, driver);
    const auto dump = out_dir / "dump.txt";
    return RunShell(Quote(driver) + " > " + Quote(dump));
}

// The SVC call must appear in a split unit other than 0000, and the driver must have logged it.
void CheckLateSvc(const fs::path& split_dir) {
    const std::string call = "wiiu::SvcHandler::Execute(cpu, 0x";
    bool in_first_unit = false;
    bool in_later_unit = false;
    for (const auto& entry : fs::directory_iterator(split_dir)) {
        if (entry.path().extension() != ".cpp") continue;
        if (ReadText(entry.path()).find(call) == std::string::npos) continue;
        if (entry.path().filename().string().find("_blocks_0000.cpp") != std::string::npos) {
            in_first_unit = true;
        } else {
            in_later_unit = true;
        }
    }
    Check(in_later_unit && !in_first_unit, "SVC call is not in a late unit");
    Check(ReadText(split_dir / "dump.txt").find("svc=1\n") != std::string::npos, "SVC call was not executed");
}

void RunExecTest(const Paths& paths, const TestCase& test) {
    const auto dir = WriteFixture(paths, test.name, test.fixture.words);
    const int plain_exit = ExecuteVariant(paths, dir, "plain", "");
    const int split_exit = ExecuteVariant(paths, dir, "split", " --split-instructions 2");
    Check(plain_exit == test.fixture.expected_exit,
          "unsplit driver exit " + std::to_string(plain_exit) + ", expected " +
              std::to_string(test.fixture.expected_exit));
    Check(split_exit == plain_exit, "split driver exit " + std::to_string(split_exit) +
                                        " differs from unsplit " + std::to_string(plain_exit));
    Check(ReadText(dir / "plain" / "dump.txt") == ReadText(dir / "split" / "dump.txt"),
          "dumps differ: compare " + (dir / "plain" / "dump.txt").string() + " with " +
              (dir / "split" / "dump.txt").string());
    if (test.fixture.expect_late_svc) CheckLateSvc(dir / "split");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: split_exec_tests <translator> <work-dir> <repo-root> <compiler>\n";
        return 2;
    }
    const Paths paths{argv[1], argv[2], argv[3], argv[4]};
    fs::create_directories(paths.work);
    const std::vector<TestCase> tests = {
        // Blocks: [MOV, ADD, B over a dead MOV] [ADD, ADD, B self]. Budget 2 gives three units.
        {"ExecutionMatchesUnsplit",
         {{0xE3A00001u, 0xE2801002u, BranchWord(kEntry + 0x08u, kEntry + 0x10u), 0xE3A02009u,
           0xE2833004u, 0xE2844001u, BranchWord(kEntry + 0x18u, kEntry + 0x18u)},
          0, false}},
        // The SVC block sits after the first unit and must be executed in both variants.
        {"SvcInLateUnitMatchesUnsplit",
         {{0xE3A00001u, 0xE2801002u, BranchWord(kEntry + 0x08u, kEntry + 0x10u), 0xE3A02009u,
           0xE2833004u, 0xEF000001u},
          0, true}},
        // A self-branch changes no register, so the driver must exit 1 in both variants.
        {"IdleLoopExitsOne", {{BranchWord(kEntry, kEntry)}, 1, false}},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            RunExecTest(paths, test);
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (failures == 0 ? "Split exec tests passed.\n" : "Split exec tests FAILED.\n");
    return failures == 0 ? 0 : 1;
}
