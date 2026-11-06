// Host CLI tests for --split-instructions (plan tests 19-20). Runs the built translator.
// argv[1]: translator executable. argv[2]: scratch work directory.
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

void Check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
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

// Decrypted ExHeader with the entry text at 0x00100000 (same layout as split_units_tests.cpp).
Bytes MakeExHeader(std::uint32_t text_size) {
    constexpr std::uint32_t kBase = 0x00100000u;
    Bytes header(0x400, 0);
    Put32(header, 0x10, kBase);
    Put32(header, 0x14, (text_size + 0xFFFu) / 0x1000u);
    Put32(header, 0x18, text_size);
    return header;
}

// Seven ALU instructions and a self branch: one block, no SVC, entry at the start.
Bytes CliCode() {
    const std::vector<std::uint32_t> words = {0xE3A00001u, 0xE2800001u, 0xE2800001u, 0xE2800001u,
                                              0xE2800001u, 0xE2800001u, 0xE2800001u, 0xEAFFFFFEu};
    Bytes code;
    for (const auto word : words) Append32(code, word);
    return code;
}

// Runs the translator with args, stdout and stderr captured in log_path. Returns the exit code.
int RunTranslator(const fs::path& translator, const std::string& args, const fs::path& log_path) {
    std::string command = "\"" + translator.string() + "\" " + args + " > \"" + log_path.string() + "\" 2>&1";
#ifdef _WIN32
    // cmd.exe strips one outer pair of quotes, so wrap the whole command line in one more pair.
    command = "\"" + command + "\"";
    return std::system(command.c_str());
#else
    const int status = std::system(command.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

struct Fixture {
    fs::path translator;
    fs::path input;
    fs::path exheader;
    fs::path work;
};

Fixture MakeFixture(const fs::path& translator, const fs::path& work) {
    fs::remove_all(work);
    fs::create_directories(work);
    Fixture fixture{translator, work / "image.code", work / "image.exheader", work};
    const auto code = CliCode();
    SaveBytes(fixture.input, code);
    SaveBytes(fixture.exheader, MakeExHeader(static_cast<std::uint32_t>(code.size())));
    return fixture;
}

std::string CommonArgs(const Fixture& fixture, const fs::path& output) {
    return "\"" + fixture.input.string() + "\" \"" + output.string() + "\" --exheader \"" +
           fixture.exheader.string() + "\"";
}

// Test 19: a bad or repeated budget exits 1, prints no success line, and writes no output.
void SplitBudgetRejectedWithoutOutput(const Fixture& fixture) {
    struct BadCase {
        std::string options;
        std::string reason;
    };
    const std::vector<BadCase> cases = {
        {"--split-instructions 0", "--split-instructions requires N >= 1"},
        {"--split-instructions 2 --split-instructions 3", "Unknown, incomplete, or repeated CLI option"},
        {"--split-instructions", "Unknown, incomplete, or repeated CLI option"},
        {"--split-instructions -1", "Invalid mode-range number"},
    };
    for (const auto& bad : cases) {
        const auto out = fixture.work / "rejected" / "translated_game.cpp";
        fs::create_directories(out.parent_path());
        const auto log = fixture.work / "rejected.log";
        const int code = RunTranslator(fixture.translator, CommonArgs(fixture, out) + " " + bad.options, log);
        const auto text = ReadText(log);
        Check(code == 1, "exit " + std::to_string(code) + " for: " + bad.options);
        Check(text.find("Translation complete") == std::string::npos, "success line for: " + bad.options);
        Check(text.find(bad.reason) != std::string::npos, "missing reason for: " + bad.options + ": " + text);
        Check(!fs::exists(out), "output written for: " + bad.options);
    }
}

// Test 20: a valid budget publishes units and reports completion; no option keeps the single file.
void SplitBudgetPublishesUnits(const Fixture& fixture) {
    const auto split_out = fixture.work / "split" / "translated_game.cpp";
    fs::create_directories(split_out.parent_path());
    const auto split_log = fixture.work / "split.log";
    Check(RunTranslator(fixture.translator, CommonArgs(fixture, split_out) + " --split-instructions 2",
                        split_log) == 0, "split run failed: " + ReadText(split_log));
    const auto split_text = ReadText(split_log);
    Check(split_text.find("Translation complete") != std::string::npos, "split run lacks success line");
    Check(split_text.find("phase=complete") != std::string::npos, "split run lacks complete progress");
    Check(fs::exists(split_out.parent_path() / "translated_game_blocks_0000.cpp"), "unit chunk missing");
    Check(ReadText(split_out).find("RegisterTranslatedBlocks0000") != std::string::npos,
          "main file does not call unit 0000");

    const auto plain_out = fixture.work / "plain" / "translated_game.cpp";
    fs::create_directories(plain_out.parent_path());
    const auto plain_log = fixture.work / "plain.log";
    Check(RunTranslator(fixture.translator, CommonArgs(fixture, plain_out), plain_log) == 0,
          "unsplit run failed: " + ReadText(plain_log));
    Check(ReadText(plain_out).find("RegisterTranslatedBlocks") == std::string::npos,
          "unsplit output calls units");
    Check(ReadText(plain_log).find("[3DS2U] progress") == std::string::npos, "unsplit run printed progress");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: split_cli_tests <translator> <work-dir>\n";
        return 2;
    }
    const fs::path translator = argv[1];
    const fs::path work = argv[2];
    const auto fixture = MakeFixture(translator, work / "cli");
    using TestFn = void (*)(const Fixture&);
    const std::vector<std::pair<std::string, TestFn>> tests = {
        {"SplitBudgetRejectedWithoutOutput", SplitBudgetRejectedWithoutOutput},
        {"SplitBudgetPublishesUnits", SplitBudgetPublishesUnits},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test(fixture);
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (failures == 0 ? "Split CLI tests passed.\n" : "Split CLI tests FAILED.\n");
    return failures == 0 ? 0 : 1;
}
