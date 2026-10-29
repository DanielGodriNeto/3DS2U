#include "save_manager.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>
#if defined(_WIN32)
#define NOMINMAX
#include <io.h>
#include <share.h>
#include <windows.h>
#undef CreateDirectory
#undef CreateFile
#else
#include <unistd.h>
#endif

namespace wiiu {
namespace {
namespace fs = std::filesystem;
fs::path UTF8(const std::string &text) {
    std::u8string utf8;
    for (const auto c : text)
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(c)));
    return fs::path(utf8);
}
std::string PathText(const fs::path &path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}
[[noreturn]] void Fail(SaveError error, const char *message) { throw SaveException(error, message); }
std::string Hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (unsigned i = 0; i < 16; ++i)
        text[15 - i] = digits[(value >> (i * 4)) & 15u];
    return text;
}
void CheckIO(bool success, const char *message) {
    if (success)
        return;
    if (errno == ENOENT)
        Fail(SaveError::NotFound, message);
    if (errno == EACCES || errno == EPERM)
        Fail(SaveError::AccessDenied, message);
    if (errno == EEXIST)
        Fail(SaveError::AlreadyExists, message);
    Fail(SaveError::IO, message);
}
fs::file_status Inspect(const fs::path &path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory)
        Fail(SaveError::IO, "Cannot inspect save path");
    if (fs::is_symlink(status))
        Fail(SaveError::AccessDenied, "Save paths cannot traverse symbolic links");
#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        Fail(SaveError::AccessDenied, "Save paths cannot traverse reparse points");
#endif
    if (fs::exists(status) && !fs::is_regular_file(status) && !fs::is_directory(status))
        Fail(SaveError::AccessDenied, "Save path is not a regular file or directory");
    return status;
}
void DirectoryChain(const fs::path &path, bool create) {
    auto current = path.root_path();
    for (const auto &component : path.relative_path()) {
        current /= component;
        // devoptab device prefixes are not ordinary POSIX directory components.
        if (PathText(current).back() == ':')
            continue;
        auto status = Inspect(current);
        if (!fs::exists(status) && create) {
            fs::create_directory(current);
            status = Inspect(current);
        }
        if (!fs::is_directory(status))
            Fail(SaveError::NotFound, "Save parent directory does not exist");
    }
}
fs::path GuestPath(const std::string &path) {
    if (path.empty() || path.size() > 1024 || path.find('\0') != std::string::npos ||
        path.find_first_of("\\:*?\"<>|") != std::string::npos)
        Fail(SaveError::InvalidArgument, "Invalid guest save path");
    const auto relative = path.front() == '/' ? path.substr(1) : path;
    fs::path result;
    for (const auto &component : UTF8(relative)) {
        const auto text = PathText(component);
        if (text.empty() || text == "." || text == ".." || text.back() == '.' || text.back() == ' ')
            Fail(SaveError::InvalidArgument, "Invalid guest save path component");
        for (const auto c : text)
            if (static_cast<unsigned char>(c) < 32)
                Fail(SaveError::InvalidArgument, "Control character in save path");
        auto device = text.substr(0, text.find('.'));
        for (auto &c : device)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" ||
            (device.size() == 4 && (device.substr(0, 3) == "COM" || device.substr(0, 3) == "LPT") &&
             device[3] >= '1' && device[3] <= '9'))
            Fail(SaveError::InvalidArgument, "Reserved host filename in save path");
        result /= component;
    }
    if (result.empty() || result.has_root_path())
        Fail(SaveError::InvalidArgument, "Invalid save filename");
    return result;
}
struct Close {
    void operator()(std::FILE *stream) const noexcept {
        if (stream)
            std::fclose(stream);
    }
};
using Stream = std::unique_ptr<std::FILE, Close>;
Stream Existing(const fs::path &path, bool write) {
    std::FILE *stream{};
#if defined(_WIN32)
    const auto result = _wfopen_s(&stream, path.c_str(), write ? L"r+b" : L"rb");
    if (result != 0)
        errno = result;
#else
    stream = std::fopen(path.c_str(), write ? "r+b" : "rb");
#endif
    CheckIO(stream != nullptr, "Cannot open save file");
    return Stream(stream);
}
Stream Exclusive(const fs::path &path) {
#if defined(_WIN32)
    int descriptor = -1;
    const auto opened = _wsopen_s(&descriptor, path.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                                  _SH_DENYNO, _S_IREAD | _S_IWRITE);
    if (opened != 0)
        errno = opened;
#else
    const int descriptor = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
#endif
    CheckIO(descriptor >= 0, "Cannot create save file");
#if defined(_WIN32)
    auto *stream = _fdopen(descriptor, "w+b");
    if (!stream)
        _close(descriptor);
#else
    auto *stream = fdopen(descriptor, "w+b");
    if (!stream)
        close(descriptor);
#endif
    CheckIO(stream != nullptr, "Cannot open new save stream");
    return Stream(stream);
}
std::uint64_t Length(std::FILE *stream) {
    CheckIO(std::fseek(stream, 0, SEEK_END) == 0, "Cannot seek save file");
    const auto size = std::ftell(stream);
    CheckIO(size >= 0, "Cannot determine save length");
    return static_cast<std::uint64_t>(size);
}
void Fill(std::FILE *stream, std::uint64_t until) {
    auto size = Length(stream);
    std::array<std::uint8_t, 32768> zeros{};
    while (size < until) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(until - size, zeros.size()));
        CheckIO(std::fwrite(zeros.data(), 1, count, stream) == count, "Cannot initialize save bytes");
        size += count;
    }
}
} // namespace

struct SaveManager::Impl {
    struct File {
        Stream stream;
        std::uint32_t flags, archive;
    };
    fs::path base;
    std::uint64_t title{}, next = 1;
    std::shared_ptr<const std::uint8_t> owner = std::make_shared<const std::uint8_t>(std::uint8_t{0});
    std::unordered_map<std::uint64_t, File> files;
    fs::path Root(std::uint32_t id, std::uint64_t ext) const {
        if (id != kSaveData && id != kExtData)
            Fail(SaveError::NotSupported, "Unsupported save archive ID");
        if (id == kSaveData && ext != 0)
            Fail(SaveError::InvalidArgument, "SaveData cannot select an ExtData ID");
        auto root = base / Hex(title) / (id == kSaveData ? "savedata" : "extdata");
        if (id == kExtData)
            root /= Hex(ext);
        return root;
    }
    fs::path Resolve(std::uint32_t id, const std::string &guest, std::uint64_t ext) {
        const auto root = Root(id, ext);
        DirectoryChain(root, false);
        const auto path = root / GuestPath(guest);
        DirectoryChain(path.parent_path(), false);
        (void)Inspect(path);
        return path;
    }
    File &Get(const SaveFileHandle &handle) {
        if (handle.owner_.lock() != owner)
            Fail(SaveError::InvalidHandle, "Save file owner expired");
        const auto it = files.find(handle.id_);
        if (it == files.end())
            Fail(SaveError::InvalidHandle, "Save file handle is closed or stale");
        return it->second;
    }
};
SaveManager::SaveManager(std::uint64_t title, std::string path) : impl_(std::make_unique<Impl>()) {
    if (title == 0 || path.empty())
        Fail(SaveError::InvalidArgument, "Save manager requires a title ID and base path");
    impl_->title = title;
#if defined(__powerpc__) || defined(__PPC__) || defined(__WIIU__)
    impl_->base = fs::path(path).lexically_normal(); // Preserve sd:/ and fs:/ devoptab roots.
#else
    impl_->base = fs::absolute(UTF8(path)).lexically_normal();
#endif
    if (impl_->base == impl_->base.root_path())
        Fail(SaveError::InvalidArgument, "Save base cannot be a filesystem root");
}
SaveManager::~SaveManager() = default;
std::string SaveManager::GetArchivePath(std::uint32_t id, std::uint64_t ext) const {
    return PathText(impl_->Root(id, ext));
}
void SaveManager::OpenArchive(std::uint32_t id, std::uint64_t ext) {
    DirectoryChain(impl_->Root(id, ext), true);
}
void SaveManager::CreateDirectory(std::uint32_t id, const std::string &path, std::uint64_t ext) {
    const auto target = impl_->Resolve(id, path, ext);
    if (fs::exists(Inspect(target)))
        Fail(SaveError::AlreadyExists, "Save directory already exists");
    CheckIO(fs::create_directory(target), "Cannot create save directory");
}
void SaveManager::CreateFile(std::uint32_t id, const std::string &path, std::uint64_t size,
                             std::uint64_t ext) {
    if (size > kMaxFileSize)
        Fail(SaveError::InvalidArgument, "Save size exceeds portable SD seek range");
    const auto target = impl_->Resolve(id, path, ext);
    auto stream = Exclusive(target);
    try {
        Fill(stream.get(), size);
        CheckIO(std::fclose(stream.release()) == 0, "Cannot close initialized save file");
    } catch (...) {
        stream.reset();
        std::error_code ignored;
        fs::remove(target, ignored);
        throw;
    }
}
SaveFileHandle SaveManager::OpenFile(std::uint32_t id, const std::string &path, std::uint32_t flags,
                                     std::uint64_t ext) {
    if ((flags & ~7u) != 0 || (flags & 3u) == 0 || ((flags & 4u) != 0 && (flags & 2u) == 0))
        Fail(SaveError::InvalidArgument, "Invalid save open flags");
    if (impl_->files.size() >= 256 || impl_->next == std::numeric_limits<std::uint64_t>::max())
        Fail(SaveError::OutOfResources, "Save file handle limit reached");
    const auto target = impl_->Resolve(id, path, ext);
    Stream stream;
    const auto status = Inspect(target);
    if (fs::is_directory(status))
        Fail(SaveError::AccessDenied, "Cannot open a save directory as a file");
    if (!fs::exists(status) && (flags & 4u) != 0)
        stream = Exclusive(target);
    else
        stream = Existing(target, (flags & 2u) != 0);
    const auto key = impl_->next++;
    impl_->files.emplace(key, Impl::File{std::move(stream), flags, id});
    SaveFileHandle handle;
    handle.owner_ = impl_->owner;
    handle.id_ = key;
    return handle;
}
std::size_t SaveManager::ReadFile(const SaveFileHandle &handle, std::uint64_t offset, void *buffer,
                                  std::size_t size) {
    auto &file = impl_->Get(handle);
    if ((file.flags & 1u) == 0)
        Fail(SaveError::AccessDenied, "Save file is not readable");
    if ((size != 0 && !buffer) || offset > kMaxFileSize || size > kMaxFileSize - offset)
        Fail(SaveError::InvalidArgument, "Invalid save read range");
    if (size == 0)
        return 0;
    CheckIO(std::fseek(file.stream.get(), static_cast<long>(offset), SEEK_SET) == 0, "Cannot seek save read");
    std::clearerr(file.stream.get());
    const auto count = std::fread(buffer, 1, size, file.stream.get());
    CheckIO(std::ferror(file.stream.get()) == 0, "Cannot read save bytes");
    return count;
}
std::size_t SaveManager::WriteFile(const SaveFileHandle &handle, std::uint64_t offset, const void *buffer,
                                   std::size_t size, std::uint32_t flags) {
    auto &file = impl_->Get(handle);
    if ((file.flags & 2u) == 0)
        Fail(SaveError::AccessDenied, "Save file is not writable");
    if ((flags & ~0x101u) != 0 || (size != 0 && !buffer) || offset > kMaxFileSize ||
        size > kMaxFileSize - offset)
        Fail(SaveError::InvalidArgument, "Invalid save write range or flags");
    if (size != 0) {
        Fill(file.stream.get(), offset);
        CheckIO(std::fseek(file.stream.get(), static_cast<long>(offset), SEEK_SET) == 0,
                "Cannot seek save write");
        const auto *bytes = static_cast<const std::uint8_t *>(buffer);
        for (std::size_t done = 0; done < size;) {
            const auto chunk = std::min<std::size_t>(32768, size - done);
            CheckIO(std::fwrite(bytes + done, 1, chunk, file.stream.get()) == chunk,
                    "Cannot write save bytes");
            done += chunk;
        }
    }
    if ((flags & 1u) != 0)
        FlushFile(handle);
    return size;
}
std::uint64_t SaveManager::GetSize(const SaveFileHandle &handle) {
    return Length(impl_->Get(handle).stream.get());
}
void SaveManager::FlushFile(const SaveFileHandle &handle) {
    auto *stream = impl_->Get(handle).stream.get();
    CheckIO(std::fflush(stream) == 0, "Cannot flush save file");
#if defined(_WIN32)
    CheckIO(_commit(_fileno(stream)) == 0, "Cannot commit save file");
#else
    CheckIO(fsync(fileno(stream)) == 0, "Cannot synchronize save file");
#endif
}
void SaveManager::CloseFile(const SaveFileHandle &handle) {
    auto &file = impl_->Get(handle);
    auto *stream = file.stream.release();
    impl_->files.erase(handle.id_);
    CheckIO(std::fclose(stream) == 0, "Cannot close save file");
}
void SaveManager::FormatThisUserSaveData() {
    const auto root = impl_->Root(kSaveData, 0);
    DirectoryChain(root, true);
    // Verify every target before removal; never recursively follow an external link.
    std::vector<fs::path> entries;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        (void)Inspect(entry.path());
        const auto relative = entry.path().lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..")
            Fail(SaveError::AccessDenied, "Format target leaves title archive");
        entries.push_back(entry.path());
    }
    for (auto it = impl_->files.begin(); it != impl_->files.end();) {
        if (it->second.archive == kSaveData) {
            auto *stream = it->second.stream.release();
            it = impl_->files.erase(it);
            CheckIO(std::fclose(stream) == 0, "Cannot close save file before format");
        } else
            ++it;
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        (void)Inspect(*it);
        CheckIO(fs::remove(*it), "Cannot remove title save entry");
    }
}
} // namespace wiiu
