#include "unit_publisher.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace arm11 {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kTempSuffix = ".3ds2u-tmp";
constexpr std::string_view kChunkSuffix = ".cpp";
constexpr std::size_t kIndexDigits = 4;
const std::string kRegisterMarker = "engine.RegisterBlock(0x";
const std::string kUnitCallPrefix = "    arm11::RegisterTranslatedBlocks";

std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read " + path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

fs::path DirectoryOf(const std::string& main_path) {
    const auto parent = fs::path{main_path}.parent_path();
    return parent.empty() ? fs::path{"."} : parent;
}

// Keys of every "engine.RegisterBlock(0xKEYu, ...)" line, in file order.
std::vector<std::uint32_t> ParseKeys(const std::string& text) {
    std::vector<std::uint32_t> keys;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const auto pos = line.find(kRegisterMarker);
        if (pos == std::string::npos) continue;
        keys.push_back(static_cast<std::uint32_t>(
            std::stoul(line.substr(pos + kRegisterMarker.size(), 8), nullptr, 16)));
    }
    return keys;
}

// Unit indices of every "    arm11::RegisterTranslatedBlocksNNNN(engine);" line, in file order.
std::vector<std::size_t> ParseUnitCalls(const std::string& text) {
    std::vector<std::size_t> calls;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind(kUnitCallPrefix, 0) != 0) continue;
        calls.push_back(static_cast<std::size_t>(
            std::stoul(line.substr(kUnitCallPrefix.size(), kIndexDigits))));
    }
    return calls;
}

void VerifyChunk(const UnitSetFiles& files, std::size_t index) {
    const auto keys = ParseKeys(ReadFile(TempPathFor(files.chunk_paths[index])));
    if (keys != files.expected_keys[index]) {
        throw std::runtime_error("verify failed: unit " + std::to_string(index) +
                                 " registrations differ from its plan");
    }
}

void VerifyMain(const UnitSetFiles& files) {
    const auto text = ReadFile(TempPathFor(files.main_path));
    if (text.find(kRegisterMarker) != std::string::npos) {
        throw std::runtime_error("verify failed: main file registers blocks directly");
    }
    const auto calls = ParseUnitCalls(text);
    if (calls.size() != files.chunk_paths.size()) {
        throw std::runtime_error("verify failed: unit call count " + std::to_string(calls.size()) +
                                 " differs from unit count " + std::to_string(files.chunk_paths.size()));
    }
    for (std::size_t i = 0; i < calls.size(); ++i) {
        if (calls[i] != i) throw std::runtime_error("verify failed: unit calls out of order");
    }
}

// Only a regular file (or nothing) may sit at a name this run writes or removes.
void RequireRegularOrAbsent(const std::string& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found) return;
    if (error) throw std::runtime_error("publish refused: cannot inspect " + path);
    if (!fs::is_regular_file(status)) {
        throw std::runtime_error("publish refused: not a regular file: " + path);
    }
}

bool IsRegularFile(const std::string& path) {
    std::error_code error;
    return fs::is_regular_file(fs::symlink_status(path, error));
}

// Exactly <stem>_blocks_NNNN.cpp with four digits.
bool IsChunkName(const std::string& name, const std::string& stem) {
    const std::string prefix = stem + "_blocks_";
    if (name.size() != prefix.size() + kIndexDigits + kChunkSuffix.size()) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    const auto digits_begin = static_cast<std::ptrdiff_t>(prefix.size());
    const auto digits_end = digits_begin + static_cast<std::ptrdiff_t>(kIndexDigits);
    if (name.compare(static_cast<std::size_t>(digits_end), kChunkSuffix.size(), kChunkSuffix.data()) != 0) {
        return false;
    }
    return std::all_of(name.begin() + digits_begin, name.begin() + digits_end,
                       [](char c) { return c >= '0' && c <= '9'; });
}

// Regular chunk-named files that this run does not write. A non-regular match refuses the run.
std::vector<std::string> FindStaleChunks(const UnitSetFiles& files) {
    std::set<std::string> wanted;
    for (const auto& path : files.chunk_paths) wanted.insert(fs::path{path}.filename().string());
    std::vector<std::string> stale;
    for (const auto& entry : fs::directory_iterator(DirectoryOf(files.main_path))) {
        const auto name = entry.path().filename().string();
        if (!IsChunkName(name, files.chunk_stem) || wanted.contains(name)) continue;
        RequireRegularOrAbsent(entry.path().string());
        stale.push_back(entry.path().string());
    }
    return stale;
}

// True when the final must be replaced, that is when it is absent or its bytes differ.
bool ContentDiffers(const std::string& final_path) {
    if (!IsRegularFile(final_path)) return true;
    return ReadFile(final_path) != ReadFile(TempPathFor(final_path));
}

void RemoveIfPresent(const std::string& path) {
    std::error_code error;
    fs::remove(path, error);
}

// Rollback removes only regular files; a directory or link that appeared is left alone.
void RemoveRegularFile(const std::string& path) {
    if (IsRegularFile(path)) RemoveIfPresent(path);
}

void RenameStaged(const std::string& final_path) {
    std::error_code error;
    fs::rename(TempPathFor(final_path), final_path, error);
    if (error) {
        throw std::runtime_error("publish failed: cannot rename " + TempPathFor(final_path) + " to " +
                                 final_path + ": " + error.message());
    }
}

void RemoveStale(const std::string& path) {
    std::error_code error;
    fs::remove(path, error);
    if (error) throw std::runtime_error("publish failed: cannot remove stale file " + path + ": " + error.message());
}

struct Target {
    std::string final_path;
    bool replace{};
};

// Renames in order and returns the finals this run replaced. The caller rolls them back on failure.
void ReplaceTargets(const std::vector<Target>& targets, std::vector<std::string>& replaced) {
    for (const auto& target : targets) {
        if (!target.replace) continue;
        RenameStaged(target.final_path);
        replaced.push_back(target.final_path);
    }
}

// Removes staged files of unchanged outputs, and every staged file on rollback.
void DropStaged(const std::vector<Target>& targets) {
    for (const auto& target : targets) RemoveIfPresent(TempPathFor(target.final_path));
}

std::vector<Target> PlanTargets(const UnitSetFiles& files) {
    std::vector<Target> targets;
    for (const auto& chunk : files.chunk_paths) targets.push_back({chunk, ContentDiffers(chunk)});
    targets.push_back({files.main_path, ContentDiffers(files.main_path)});
    return targets;
}

} // namespace

std::string TempPathFor(const std::string& final_path) {
    return final_path + std::string{kTempSuffix};
}

TempFileSet::~TempFileSet() {
    for (const auto& path : staged_) RemoveIfPresent(path);
}

std::string TempFileSet::Track(const std::string& final_path) {
    staged_.push_back(TempPathFor(final_path));
    return staged_.back();
}

void VerifyStagedFiles(const UnitSetFiles& files) {
    if (files.expected_keys.size() != files.chunk_paths.size()) {
        throw std::runtime_error("verify failed: plan lists " + std::to_string(files.expected_keys.size()) +
                                 " unit key sets for " + std::to_string(files.chunk_paths.size()) + " units");
    }
    for (std::size_t i = 0; i < files.chunk_paths.size(); ++i) VerifyChunk(files, i);
    VerifyMain(files);
}

void PublishStagedFiles(const UnitSetFiles& files) {
    for (const auto& chunk : files.chunk_paths) RequireRegularOrAbsent(chunk);
    RequireRegularOrAbsent(files.main_path);
    const auto stale = FindStaleChunks(files);
    auto targets = PlanTargets(files);
    std::vector<std::string> replaced;
    try {
        for (const auto& path : stale) RemoveStale(path);
        ReplaceTargets(targets, replaced);
    } catch (...) {
        for (const auto& path : replaced) RemoveRegularFile(path);
        DropStaged(targets);
        throw;
    }
    DropStaged(targets);
}

} // namespace arm11
