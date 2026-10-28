#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace wiiu {
enum class SaveError {
    InvalidArgument,
    NotFound,
    AlreadyExists,
    AccessDenied,
    IO,
    InvalidHandle,
    NotSupported,
    OutOfResources
};
class SaveException : public std::runtime_error {
  public:
    SaveException(SaveError error, const char *message) : std::runtime_error(message), code(error) {}
    SaveError code;
};
class SaveManager;
struct SaveFileHandle {
    explicit operator bool() const noexcept { return id_ != 0 && !owner_.expired(); }

  private:
    friend class SaveManager;
    std::weak_ptr<const std::uint8_t> owner_;
    std::uint64_t id_{};
};

// Serialize with the FS service/engine lock. The SD device must already be mounted.
class SaveManager {
  public:
    static constexpr const char *kDefaultBase = "sd:/wiiu/apps/3ds2u/saves/";
    static constexpr std::uint32_t kSaveData = 4, kExtData = 6;
    static constexpr std::uint64_t kMaxFileSize = 0x7FFFFFFFu;
    explicit SaveManager(std::uint64_t title_id, std::string base_path = kDefaultBase);
    ~SaveManager();
    SaveManager(const SaveManager &) = delete;
    SaveManager &operator=(const SaveManager &) = delete;
    void OpenArchive(std::uint32_t archive_id, std::uint64_t extdata_id = 0);
    void FormatThisUserSaveData();
    void CreateDirectory(std::uint32_t archive_id, const std::string &path, std::uint64_t extdata_id = 0);
    void CreateFile(std::uint32_t archive_id, const std::string &path, std::uint64_t size,
                    std::uint64_t extdata_id = 0);
    [[nodiscard]] SaveFileHandle OpenFile(std::uint32_t archive_id, const std::string &path,
                                          std::uint32_t open_flags, std::uint64_t extdata_id = 0);
    std::size_t ReadFile(const SaveFileHandle &handle, std::uint64_t offset, void *buffer, std::size_t size);
    std::size_t WriteFile(const SaveFileHandle &handle, std::uint64_t offset, const void *buffer,
                          std::size_t size, std::uint32_t flush_flags);
    [[nodiscard]] std::uint64_t GetSize(const SaveFileHandle &handle);
    void FlushFile(const SaveFileHandle &handle);
    void CloseFile(const SaveFileHandle &handle);
    [[nodiscard]] std::string GetArchivePath(std::uint32_t archive_id, std::uint64_t extdata_id = 0) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace wiiu
