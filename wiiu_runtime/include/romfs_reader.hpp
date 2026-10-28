#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wiiu {
class RomFSReader;
struct RomFSFileHandle {
    [[nodiscard]] std::uint64_t GetSize() const noexcept { return size_; }
private:
    friend class RomFSReader;
    const RomFSReader* owner_{};
    std::uint64_t generation_{}, offset_{}, size_{};
};

// Owns a copy of an unencrypted Level-3 RomFS image, with offsets relative to it.
class RomFSReader {
public:
    RomFSReader() = default;
    RomFSReader(const RomFSReader&) = delete;
    RomFSReader& operator=(const RomFSReader&) = delete;
    [[nodiscard]] bool Mount(std::span<const std::uint8_t> image, std::size_t level3_offset = 0);
    void Unmount() noexcept;
    [[nodiscard]] bool OpenFile(const std::string& path, RomFSFileHandle& out_handle) const;
    [[nodiscard]] bool OpenFile(std::u16string_view path, RomFSFileHandle& out_handle) const;
    [[nodiscard]] std::size_t ReadFile(const RomFSFileHandle& handle, std::uint64_t offset,
                                      void* buffer, std::size_t size) const;
    [[nodiscard]] const std::string& GetLastError() const noexcept { return error_; }
private:
    struct Directory {
        std::uint32_t parent, sibling, child_dir, child_file, hash;
        std::u16string name;
    };
    struct File {
        std::uint32_t parent, sibling, hash;
        std::uint64_t offset, size;
        std::u16string name;
    };
    std::vector<std::uint8_t> image_;
    std::unordered_map<std::uint32_t, Directory> directories_;
    std::unordered_map<std::uint32_t, File> files_;
    std::uint32_t data_offset_{};
    std::uint64_t generation_{};
    std::string error_;
};
} // namespace wiiu
