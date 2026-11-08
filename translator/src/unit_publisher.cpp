#include "unit_publisher.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <streambuf>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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

bool IsRegularFile(const std::string& path);

// Staged files are read back only while they are still regular files (never a swapped-in link).
std::string ReadStaged(const std::string& final_path) {
    const auto staged = TempPathFor(final_path);
    if (!IsRegularFile(staged)) {
        throw std::runtime_error("verify failed: staged file is not a regular file: " + staged);
    }
    return ReadFile(staged);
}

void VerifyChunk(const UnitSetFiles& files, std::size_t index) {
    const auto keys = ParseKeys(ReadStaged(files.chunk_paths[index]));
    if (keys != files.expected_keys[index]) {
        throw std::runtime_error("verify failed: unit " + std::to_string(index) +
                                 " registrations differ from its plan");
    }
}

void VerifyMain(const UnitSetFiles& files) {
    const auto text = ReadStaged(files.main_path);
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
    std::error_code scan_error;
    fs::directory_iterator it(DirectoryOf(files.main_path), scan_error);
    if (scan_error) return stale; // a missing or unreadable directory fails at the first staged write
    for (const auto& entry : it) {
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
    if (!IsRegularFile(TempPathFor(final_path))) {
        throw std::runtime_error("publish refused: staged file is not a regular file: " + TempPathFor(final_path));
    }
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

// The staged file must still be the regular file this run created, and the final must still be
// a regular file or absent, immediately before the rename.
void RenameStaged(const std::string& final_path) {
    if (!IsRegularFile(TempPathFor(final_path))) {
        throw std::runtime_error("publish refused: staged file is not a regular file: " + TempPathFor(final_path));
    }
    RequireRegularOrAbsent(final_path);
    std::error_code error;
    fs::rename(TempPathFor(final_path), final_path, error);
    if (error) {
        throw std::runtime_error("publish failed: cannot rename " + TempPathFor(final_path) + " to " +
                                 final_path + ": " + error.message());
    }
}

void RemoveStale(const std::string& path) {
    RequireRegularOrAbsent(path);
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
    for (const auto& target : targets) RemoveRegularFile(TempPathFor(target.final_path));
}

// Opens path for writing and fails if anything already exists there, a link included.
std::FILE* OpenExclusive(const std::string& path) {
#ifdef _WIN32
    int fd = -1;
    const auto wide = fs::path{path}.wstring();
    const auto code = _wsopen_s(&fd, wide.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _SH_DENYWR,
                                _S_IREAD | _S_IWRITE);
    if (code != 0) errno = code;
    if (code != 0) return nullptr;
    auto* file = _fdopen(fd, "wb");
    if (file == nullptr) _close(fd);
    return file;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return nullptr;
    auto* file = ::fdopen(fd, "wb");
    if (file == nullptr) ::close(fd);
    return file;
#endif
}

// Buffered ostream sink over a FILE*. Write errors surface as stream failure.
class FileBuf : public std::streambuf {
public:
    explicit FileBuf(std::FILE* file) : file_{file} {}
    bool Flush() { return sync() == 0; }

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        const auto written = std::fwrite(data, 1, static_cast<std::size_t>(count), file_);
        return static_cast<std::streamsize>(written);
    }
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
        const unsigned char byte = static_cast<unsigned char>(ch);
        return std::fwrite(&byte, 1, 1, file_) == 1 ? ch : traits_type::eof();
    }
    int sync() override { return std::fflush(file_) == 0 ? 0 : -1; }

private:
    std::FILE* file_;
};

struct FileCloser {
    void operator()(std::FILE* file) const { if (file != nullptr) std::fclose(file); }
};

// Leftover staging file of a crashed run: only a regular file may be replaced.
std::FILE* CreateStaged(const std::string& staged_path) {
    if (auto* file = OpenExclusive(staged_path)) return file;
    const int open_errno = errno;
    if (open_errno != EEXIST) {
        throw std::runtime_error("staging failed: cannot create " + staged_path + ": " + std::strerror(open_errno));
    }
    if (!IsRegularFile(staged_path)) {
        throw std::runtime_error("staging refused: path exists and is not a regular file: " + staged_path);
    }
    RemoveRegularFile(staged_path);
    if (auto* file = OpenExclusive(staged_path)) return file;
    throw std::runtime_error("staging refused: cannot create exclusively: " + staged_path);
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
    for (const auto& path : staged_) RemoveRegularFile(path);
}

std::string TempFileSet::Track(const std::string& final_path) {
    staged_.push_back(TempPathFor(final_path));
    return staged_.back();
}

void WriteStagedFile(const std::string& staged_path, const std::function<void(std::ostream&)>& write) {
    std::unique_ptr<std::FILE, FileCloser> file{CreateStaged(staged_path)};
    FileBuf buffer{file.get()};
    std::ostream out{&buffer};
    out.exceptions(std::ios::failbit | std::ios::badbit);
    write(out);
    out.flush();
    if (std::fclose(file.release()) != 0) throw std::runtime_error("cannot close " + staged_path);
}

void CheckPublishable(const UnitSetFiles& files) {
    for (const auto& chunk : files.chunk_paths) RequireRegularOrAbsent(chunk);
    RequireRegularOrAbsent(files.main_path);
    FindStaleChunks(files);
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
    CheckPublishable(files);
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
