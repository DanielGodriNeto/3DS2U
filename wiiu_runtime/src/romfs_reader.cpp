#include "romfs_reader.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

namespace wiiu {
namespace {
constexpr std::uint32_t kNone = 0xFFFFFFFFu;
void Require(bool condition) { if (!condition) throw std::invalid_argument("Invalid RomFS metadata"); }
std::uint32_t LE32(std::span<const std::uint8_t> bytes, std::size_t at) {
    Require(at <= bytes.size() && bytes.size() - at >= 4);
    return std::uint32_t{bytes[at]} | (std::uint32_t{bytes[at + 1]} << 8) |
           (std::uint32_t{bytes[at + 2]} << 16) | (std::uint32_t{bytes[at + 3]} << 24);
}
std::uint64_t LE64(std::span<const std::uint8_t> bytes, std::size_t at) {
    return LE32(bytes, at) | (std::uint64_t{LE32(bytes, at + 4)} << 32);
}
bool ValidUTF16(std::u16string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (++i >= text.size() || text[i] < 0xDC00 || text[i] > 0xDFFF) return false;
        } else if (c >= 0xDC00 && c <= 0xDFFF) return false;
    }
    return true;
}
std::u16string Name(std::span<const std::uint8_t> bytes, std::size_t at, std::uint32_t length) {
    Require((length & 1u) == 0 && at <= bytes.size() && length <= bytes.size() - at);
    std::u16string result;
    for (std::size_t i = 0; i < length; i += 2)
        result.push_back(static_cast<char16_t>(bytes[at + i] | (std::uint32_t{bytes[at + i + 1]} << 8)));
    Require(ValidUTF16(result) && result.find_first_of(u"/\\") == std::u16string::npos &&
            result.find(u'\0') == std::u16string::npos && result != u"." && result != u"..");
    return result;
}
std::u16string FromUTF8(const std::string& text) {
    Require(text.size() <= 4096);
    std::u16string result;
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<std::uint8_t>(text[i++]);
        std::uint32_t value = c;
        unsigned extra = 0;
        if (c >= 0xC2 && c <= 0xDF) { value = c & 31u; extra = 1; }
        else if (c >= 0xE0 && c <= 0xEF) { value = c & 15u; extra = 2; }
        else if (c >= 0xF0 && c <= 0xF4) { value = c & 7u; extra = 3; }
        else Require(c < 0x80);
        Require(extra <= text.size() - i);
        for (unsigned n = 0; n < extra; ++n) {
            c = static_cast<std::uint8_t>(text[i++]);
            Require((c & 0xC0u) == 0x80u);
            value = (value << 6) | (c & 63u);
        }
        Require((extra != 1 || value >= 0x80) && (extra != 2 || value >= 0x800) &&
                (extra != 3 || value >= 0x10000) && value <= 0x10FFFF &&
                !(value >= 0xD800 && value <= 0xDFFF));
        if (value < 0x10000) result.push_back(static_cast<char16_t>(value));
        else {
            value -= 0x10000;
            result.push_back(static_cast<char16_t>(0xD800u + (value >> 10)));
            result.push_back(static_cast<char16_t>(0xDC00u + (value & 1023u)));
        }
    }
    return result;
}
} // namespace

void RomFSReader::Unmount() noexcept {
    image_.clear(); directories_.clear(); files_.clear();
    data_offset_ = 0;
    ++generation_;
}

bool RomFSReader::Mount(std::span<const std::uint8_t> image, std::size_t level3_offset) {
    Unmount();
    error_.clear();
    try {
        Require(level3_offset <= image.size());
        // Copy before interpreting metadata so caller buffers may be released safely.
        const auto source = image.subspan(level3_offset);
        image_.assign(source.begin(), source.end());
        const std::span<const std::uint8_t> bytes{image_};
        Require(LE32(bytes, 0) == 0x28u);
        data_offset_ = LE32(bytes, 0x24);
        Require(data_offset_ >= 0x28 && data_offset_ <= bytes.size());
        struct Range { std::uint32_t offset, size; };
        std::array<Range, 4> tables{};
        for (std::size_t i = 0; i < tables.size(); ++i) {
            tables[i] = {LE32(bytes, 4 + i * 8), LE32(bytes, 8 + i * 8)};
            const auto [offset, size] = tables[i];
            Require(offset >= 0x28 && offset <= data_offset_ && size <= data_offset_ - offset &&
                    ((offset | size) & 3u) == 0);
            for (std::size_t j = 0; j < i; ++j)
                if (size != 0 && tables[j].size != 0)
                    Require(std::uint64_t{offset} + size <= tables[j].offset ||
                            std::uint64_t{tables[j].offset} + tables[j].size <= offset);
        }
        Require(tables[0].size >= 4 && tables[2].size >= 4 && tables[1].size >= 24);
        const auto dirs = bytes.subspan(tables[1].offset, tables[1].size);
        for (std::size_t at = 0; at < dirs.size();) {
            Require(dirs.size() - at >= 24);
            const auto name_length = LE32(dirs, at + 20);
            auto name = Name(dirs, at + 24, name_length);
            Require(at == 0 ? name.empty() : !name.empty());
            directories_.emplace(static_cast<std::uint32_t>(at), Directory{
                LE32(dirs, at), LE32(dirs, at + 4), LE32(dirs, at + 8),
                LE32(dirs, at + 12), LE32(dirs, at + 16), std::move(name)});
            const auto length = (std::uint64_t{24} + name_length + 3u) & ~std::uint64_t{3};
            Require(length <= dirs.size() - at);
            at += static_cast<std::size_t>(length);
        }
        const auto files = bytes.subspan(tables[3].offset, tables[3].size);
        for (std::size_t at = 0; at < files.size();) {
            Require(files.size() - at >= 32);
            const auto name_length = LE32(files, at + 28);
            auto name = Name(files, at + 32, name_length);
            Require(!name.empty());
            const auto offset = LE64(files, at + 8), size = LE64(files, at + 16);
            Require(offset <= bytes.size() - data_offset_ && size <= bytes.size() - data_offset_ - offset);
            files_.emplace(static_cast<std::uint32_t>(at), File{LE32(files, at), LE32(files, at + 4),
                LE32(files, at + 24), offset, size, std::move(name)});
            const auto length = (std::uint64_t{32} + name_length + 3u) & ~std::uint64_t{3};
            Require(length <= files.size() - at);
            at += static_cast<std::size_t>(length);
        }
        const auto dir_ref = [&](auto value) { return value == kNone || directories_.contains(value); };
        const auto file_ref = [&](auto value) { return value == kNone || files_.contains(value); };
        for (const auto& [offset, dir] : directories_) {
            (void)offset;
            Require(dir_ref(dir.parent) && dir_ref(dir.sibling) && dir_ref(dir.child_dir) &&
                    file_ref(dir.child_file) && dir_ref(dir.hash));
        }
        for (const auto& [offset, file] : files_) {
            (void)offset;
            Require(directories_.contains(file.parent) && file_ref(file.sibling) && file_ref(file.hash));
        }
        for (unsigned table : {0u, 2u})
            for (std::size_t at = 0; at < tables[table].size; at += 4) {
                const auto ref = LE32(bytes, tables[table].offset + at);
                Require(table == 0 ? dir_ref(ref) : file_ref(ref));
            }
        Require(directories_.at(0).parent == 0 && directories_.at(0).sibling == kNone);
        std::unordered_set<std::uint32_t> seen_dirs{0}, seen_files;
        std::vector<std::uint32_t> pending{0};
        while (!pending.empty()) {
            const auto parent = pending.back(); pending.pop_back();
            const auto& dir = directories_.at(parent);
            std::unordered_set<std::u16string> names;
            for (auto child = dir.child_dir; child != kNone; child = directories_.at(child).sibling) {
                const auto& record = directories_.at(child);
                Require(record.parent == parent && seen_dirs.insert(child).second && names.insert(record.name).second);
                pending.push_back(child);
            }
            for (auto child = dir.child_file; child != kNone; child = files_.at(child).sibling) {
                const auto& record = files_.at(child);
                Require(record.parent == parent && seen_files.insert(child).second && names.insert(record.name).second);
            }
        }
        Require(seen_dirs.size() == directories_.size() && seen_files.size() == files_.size());
        return true;
    } catch (const std::exception& error) {
        Unmount();
        error_ = error.what();
        return false;
    }
}

bool RomFSReader::OpenFile(const std::string& path, RomFSFileHandle& handle) const {
    handle = {};
    try { return OpenFile(std::u16string_view{FromUTF8(path)}, handle); }
    catch (const std::exception&) { return false; }
}

bool RomFSReader::OpenFile(std::u16string_view path, RomFSFileHandle& handle) const {
    handle = {};
    if (directories_.empty() || path.size() > 2048 || !ValidUTF16(path)) return false;
    if (path.starts_with(u"romfs:")) path.remove_prefix(6);
    if (path.empty() || path.front() != u'/') return false;
    path.remove_prefix(1);
    std::uint32_t directory = 0;
    while (!path.empty()) {
        const auto slash = path.find(u'/');
        const auto component = path.substr(0, slash);
        if (component.empty() || component == u"." || component == u"..") return false;
        if (slash == std::u16string_view::npos) {
            for (auto file = directories_.at(directory).child_file; file != kNone; file = files_.at(file).sibling) {
                const auto& record = files_.at(file);
                if (record.name == component) {
                    handle.owner_ = this; handle.generation_ = generation_;
                    handle.offset_ = data_offset_ + record.offset; handle.size_ = record.size;
                    return true;
                }
            }
            return false;
        }
        bool found = false;
        for (auto child = directories_.at(directory).child_dir; child != kNone; child = directories_.at(child).sibling)
            if (directories_.at(child).name == component) { directory = child; found = true; break; }
        if (!found) return false;
        path.remove_prefix(slash + 1);
    }
    return false;
}

std::size_t RomFSReader::ReadFile(const RomFSFileHandle& handle, std::uint64_t offset,
                                 void* buffer, std::size_t size) const {
    if (handle.owner_ != this || handle.generation_ != generation_ || image_.empty())
        throw std::invalid_argument("Invalid or stale RomFS file handle");
    if (offset >= handle.size_ || size == 0) return 0;
    if (buffer == nullptr) throw std::invalid_argument("Null RomFS read buffer");
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size, handle.size_ - offset));
    std::memcpy(buffer, image_.data() + static_cast<std::size_t>(handle.offset_ + offset), count);
    return count;
}
} // namespace wiiu
