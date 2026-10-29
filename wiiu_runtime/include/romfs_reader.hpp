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
    std::size_t layer_{};
};

// Owns validated Level-3 images. Later mounts override files in earlier layers.
class RomFSReader {
public:
    RomFSReader() = default;
    RomFSReader(const RomFSReader&) = delete;
    RomFSReader& operator=(const RomFSReader&) = delete;
    [[nodiscard]] bool Mount(std::span<const std::uint8_t> image, std::size_t level3_offset = 0);
    [[nodiscard]] bool MountLayer(std::span<const std::uint8_t> image, std::size_t level3_offset = 0);
    [[nodiscard]] bool MountFile(const std::string& filename, std::size_t level3_offset = 0);
    [[nodiscard]] bool MountLayerFile(const std::string& filename, std::size_t level3_offset = 0);
    [[nodiscard]] std::size_t GetMountCount() const noexcept { return layers_.size(); }
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
    struct Layer {
        std::vector<std::uint8_t> image;
        std::unordered_map<std::uint32_t, Directory> directories;
        std::unordered_map<std::uint32_t, File> files;
        std::uint32_t data_offset{};
    };
    [[nodiscard]] bool OpenInLayer(std::u16string_view path, std::size_t layer,
                                   RomFSFileHandle& handle) const;
    std::vector<Layer> layers_;
    std::uint64_t generation_{};
    std::string error_;
};
} // namespace wiiu
