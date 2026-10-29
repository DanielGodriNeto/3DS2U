#include "cro_loader.hpp"
#include "wiiu_memory.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <iterator>

namespace wiiu {
namespace {
using U32 = std::uint32_t;
using Bytes = std::span<const std::uint8_t>;
[[noreturn]] void Fail(const char* message) { throw std::runtime_error(message); }
void Range(Bytes b, std::size_t at, std::size_t size) {
    if (at > b.size() || size > b.size() - at) Fail("CRO range outside image");
}
U32 Word(Bytes b, std::size_t at) {
    Range(b, at, 4);
    return U32{b[at]} | (U32{b[at + 1]} << 8) | (U32{b[at + 2]} << 16) |
           (U32{b[at + 3]} << 24);
}
void Put(std::vector<std::uint8_t>& b, U32 at, U32 value) {
    Range(b, at, 4);
    for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
}
U32 Page(U32 n) {
    if (n > WiiUMemory::kNewFCRAMSize) Fail("CRO image too large");
    return (n + 0xFFFu) & ~0xFFFu;
}
struct Table { U32 offset{}, count{}, width{}; };
struct Parser {
    Bytes bytes;
    std::array<U32, 46> h{};
    std::array<Table, 17> tables{};
    explicit Parser(Bytes input) : bytes(input) {
        Range(bytes, 0, 0x138);
        for (U32 i = 0; i < h.size(); ++i) h[i] = Word(bytes, 0x80u + i * 4u);
        if (h[0] != 0x304F5243u) Fail("Expected CRO0 magic at offset 0x80");
        if (h[4] < 0x138 || h[4] > bytes.size() || h[4] > WiiUMemory::kNewFCRAMSize)
            Fail("Invalid CRO file size");
        bytes = bytes.first(h[4]);
        if (h[2] != 0 || h[3] != 0 || h[7] != 0) Fail("CRO is already linked or invalid");
        constexpr std::array<U32, 17> widths{1,1,1,12,8,4,1,8,20,12,8,8,8,1,8,12,12};
        for (U32 i = 0; i < widths.size(); ++i) {
            auto& t = tables[i];
            t = {h[12 + i * 2], h[13 + i * 2], widths[i]};
            if (t.count != 0 && (t.offset < 0x138 || t.count > bytes.size() / t.width))
                Fail("Invalid CRO table");
            Range(bytes, t.offset, static_cast<std::size_t>(t.count) * t.width);
        }
        if (h[19] == 0 || h[19] > 16) Fail("Invalid CRO segment count");
        if (h[41] != 0 || h[45] != 0) Fail("Static CRS relocations are unsupported");
    }
    U32 Entry(unsigned table, U32 index) const {
        const auto& t = tables.at(table);
        if (index >= t.count) Fail("CRO table index out of range");
        return t.offset + index * t.width;
    }
    std::string String(U32 at, unsigned table) const {
        const auto& t = tables.at(table);
        if (at < t.offset || at - t.offset >= t.count) Fail("CRO string outside string table");
        std::string name;
        const auto end = t.offset + t.count;
        for (; at < end && bytes[at] != 0; ++at) {
            if (name.size() == 4096) Fail("CRO symbol name too long");
            name.push_back(static_cast<char>(bytes[at]));
        }
        if (at == end || name.empty()) Fail("Invalid CRO string");
        return name;
    }
};
U32 Tag(const CROModule& module, U32 tag, U32 width = 1) {
    const auto index = tag & 15u, offset = tag >> 4;
    if (index >= module.segments.size()) Fail("Invalid CRO segment tag");
    const auto& segment = module.segments[index];
    if (offset > segment.size || width > segment.size - offset) Fail("CRO segment tag outside segment");
    return segment.address + offset;
}

// CRO addends are explicit signed RELA addends: branch displacement is S+A-P.
// Thus a direct ARM call normally has A=-8, and a Thumb call A=-4.
void Relocate(std::vector<std::uint8_t>& image, U32 base, U32 place,
              unsigned type, U32 symbol, U32 addend) {
    const U32 at = place - base;
    const U32 old = Word(image, at);
    if (type == 0) return;
    if (type == 2 || type == 38 || type == 41) {
        // TARGET1/TARGET2 use the absolute-address policy of this runtime.
        if ((place & 3u) != 0) Fail("Unaligned CRO word relocation");
        Put(image, at, symbol + addend); return;
    }
    if (type == 3) { Put(image, at, symbol + addend - place); return; }
    if (type == 42) {
        const auto relative = static_cast<std::int64_t>(symbol) +
            std::bit_cast<std::int32_t>(addend) - place;
        if ((place & 3u) != 0 || relative < -1073741824 || relative > 1073741823)
            Fail("CRO PREL31 relocation out of range");
        Put(image, at, (old & 0x80000000u) | (static_cast<U32>(relative) & 0x7FFFFFFFu));
        return;
    }
    const auto displacement = static_cast<std::int64_t>(symbol & ~1u) +
        std::bit_cast<std::int32_t>(addend) - place;
    if (type == 28 || type == 29) {
        if ((place & 3u) != 0 || (old & 0x0E000000u) != 0x0A000000u)
            Fail("Invalid ARM branch relocation opcode");
        const bool was_blx = (old >> 28) == 15u;
        if ((type == 28 && !was_blx && (old & 0xFF000000u) != 0xEB000000u) ||
            (type == 29 && was_blx)) Fail("Invalid ARM call/jump relocation opcode");
        const bool thumb = (symbol & 1u) != 0;
        if (thumb && type != 28)
            Fail("ARM jump requires an interworking veneer");
        if ((displacement & (thumb ? 1 : 3)) != 0 || displacement < -33554432 ||
            displacement > (thumb ? 33554430 : 33554428)) Fail("ARM branch relocation out of range");
        const auto bits = static_cast<U32>(displacement);
        Put(image, at, (thumb ? 0xFA000000u | ((bits & 2u) << 23) :
                         was_blx ? 0xEB000000u : old & 0xFF000000u) |
                         ((bits >> 2) & 0x00FFFFFFu));
        return;
    }
    if (type == 10) {
        const U32 first = old & 0xFFFFu, second = old >> 16;
        if ((place & 1u) != 0 || (first & 0xF800u) != 0xF000u ||
            (second & 0xC000u) != 0xC000u ||
            ((second & 0x1000u) == 0 && (second & 1u) != 0)) Fail("Invalid Thumb call relocation opcode");
        const bool thumb = (symbol & 1u) != 0;
        // BLX uses Align(P+4,4), so account for a halfword-aligned place.
        const auto delta = displacement + (thumb ? 0 : static_cast<std::int64_t>(place & 2u));
        if ((delta & (thumb ? 1 : 3)) != 0 || delta < -16777216 || delta > 16777214)
            Fail("Thumb branch relocation out of range");
        const auto bits = static_cast<U32>(delta);
        const auto s = (bits >> 24) & 1u;
        const auto j1 = (~((bits >> 23) ^ s)) & 1u;
        const auto j2 = (~((bits >> 22) ^ s)) & 1u;
        const auto hi = 0xF000u | (s << 10) | ((bits >> 12) & 0x3FFu);
        const auto lo = (thumb ? 0xD000u : 0xC000u) | (j1 << 13) | (j2 << 11) |
                        ((bits >> 1) & 0x7FFu);
        Put(image, at, hi | (lo << 16)); return;
    }
    Fail("Unsupported CRO relocation type");
}
} // namespace

bool CROModule::IsExecutable(std::uint32_t address) const noexcept {
    address &= ~1u;
    for (const auto& segment : segments)
        if (segment.type == 0 && address >= segment.address && address - segment.address < segment.size)
            return true;
    return false;
}

bool CROLoader::RegisterSymbol(const std::string& name, U32 address) {
    if (name.empty()) return false;
    return symbols_.emplace(name, Symbol{address, 0}).second;
}
bool CROLoader::FindSymbol(const std::string& name, U32& address) const {
    const auto it = symbols_.find(name);
    if (it == symbols_.end()) return false;
    address = it->second.address; return true;
}

bool CROLoader::LoadModule(WiiUMemory& memory, const std::uint8_t* data, std::size_t size,
                           U32 base, CROModule& out) {
    try {
        if (data == nullptr || size > memory.GetSize() ||
            (memory_ != nullptr && memory_ != &memory)) Fail("Invalid CRO input or memory owner");
        const Parser p(Bytes(data, size));
        CROModule module;
        module.load_address = base;
        module.name = p.String(p.h[16], 2);
        for (const auto& [address, loaded] : modules_) {
            (void)address;
            if (loaded.name == module.name) Fail("CRO module already loaded");
        }
        const auto bss_start = Page(p.h[4]);
        if (p.h[5] > memory.GetSize() - bss_start) Fail("CRO BSS too large");
        module.mapped_size = Page(bss_start + p.h[5]);
        if ((base & 0xFFFu) != 0 || base < memory.GetImageEnd() ||
            base > memory.GetVirtualEnd() ||
            module.mapped_size > memory.GetVirtualEnd() - base)
            Fail("CRO mapping outside free FCRAM");
        U32 bss_used = 0;
        for (U32 i = 0; i < p.h[19]; ++i) {
            const auto at = p.Entry(3, i);
            const auto offset = Word(p.bytes, at), length = Word(p.bytes, at + 4), type = Word(p.bytes, at + 8);
            if (type > 3) Fail("Invalid CRO segment type");
            U32 mapped = offset;
            if (type == 3) {
                if (length > p.h[5] - bss_used) Fail("CRO BSS exceeds header size");
                mapped = bss_start + bss_used; bss_used += length;
            } else {
                Range(p.bytes, offset, length);
                if (length != 0 && offset < 0x138) Fail("CRO segment overlaps header");
                for (const auto& prior : module.segments)
                    if (prior.type != 3 && length != 0 && prior.size != 0 &&
                        base + offset < prior.address + prior.size && prior.address < base + offset + length)
                        Fail("Overlapping CRO segments");
            }
            module.segments.push_back({base + mapped, length, type});
        }
        for (unsigned kind : {0u, 2u}) {
            const auto offset = p.h[kind == 0 ? 12 : 14];
            const auto length = p.h[kind == 0 ? 13 : 15];
            if (length != 0 && std::none_of(module.segments.begin(), module.segments.end(),
                [&](const CROSegment& segment) {
                    return segment.type == kind && segment.address == base + offset && segment.size == length;
                })) Fail("CRO code/data descriptor disagrees with segment table");
        }
        std::vector<std::uint8_t> image(module.mapped_size, 0);
        std::copy(p.bytes.begin(), p.bytes.end(), image.begin());
        auto symbols = symbols_;
        for (U32 i = 0; i < p.h[21]; ++i) {
            const auto at = p.Entry(4, i);
            auto name = p.String(Word(p.bytes, at), 6);
            const auto value = Tag(module, Word(p.bytes, at + 4));
            if (!symbols.emplace(name, Symbol{value, base}).second) Fail("Duplicate CRO export");
            module.exports.emplace(std::move(name), value);
        }
        for (U32 i = 0; i < p.h[23]; ++i)
            module.indexed_exports.push_back(Tag(module, Word(p.bytes, p.Entry(5, i))));
        std::set<U32> visited;
        std::set<U32> patches;
        const auto patch = [&](U32 target, unsigned type, U32 symbol, U32 addend) {
            if (type != 0) {
                auto next = patches.lower_bound(target);
                if ((next != patches.end() && *next < target + 4u) ||
                    (next != patches.begin() && *std::prev(next) + 4u > target))
                    Fail("Overlapping CRO relocation patches");
                patches.insert(target);
            }
            Relocate(image, base, target, type, symbol, addend);
        };
        const auto batch = [&](U32 at, U32 symbol, U32 owner) {
            const auto& table = p.tables[9];
            if (at < table.offset || (at - table.offset) % 12u != 0) Fail("Invalid CRO relocation batch");
            for (;;) {
                if (at - table.offset >= table.count * 12u || !visited.insert(at).second)
                    Fail("Unterminated or duplicate CRO relocation batch");
                const auto target = Tag(module, Word(p.bytes, at), 4);
                if (p.bytes[at + 5] > 1 || p.bytes[at + 6] != 0) Fail("Invalid CRO relocation flags");
                patch(target, p.bytes[at + 4], symbol, Word(p.bytes, at + 8));
                image[at + 6] = 1;
                if (p.bytes[at + 5] != 0) break;
                at += 12;
            }
            if (owner != 0 && owner != base) module.dependencies.insert(owner);
        };
        for (U32 i = 0; i < p.h[33]; ++i) {
            const auto at = p.Entry(10, i);
            const auto name = p.String(Word(p.bytes, at), 13);
            const auto found = symbols.find(name);
            if (found == symbols.end()) Fail("Unresolved CRO named import");
            batch(Word(p.bytes, at + 4), found->second.address, found->second.owner);
        }
        // Module imports select indexed exports or segment-tag anonymous exports.
        std::set<U32> indexed, anonymous;
        for (U32 i = 0; i < p.h[29]; ++i) {
            const auto at = p.Entry(8, i);
            const auto name = p.String(Word(p.bytes, at), 13);
            const CROModule* provider = nullptr;
            for (const auto& [address, loaded] : modules_) {
                (void)address;
                if (loaded.name == name) provider = &loaded;
            }
            if (provider == nullptr) Fail("Unresolved CRO module import");
            for (unsigned kind = 0; kind < 2; ++kind) {
                const auto start = Word(p.bytes, at + 4 + kind * 8);
                const auto count = Word(p.bytes, at + 8 + kind * 8);
                const auto& table = p.tables[11 + kind];
                if (count != 0 && (start < table.offset || (start - table.offset) % 8u != 0 ||
                    (start - table.offset) / 8u > table.count ||
                    count > table.count - (start - table.offset) / 8u)) Fail("Invalid CRO module import slice");
                for (U32 n = 0; n < count; ++n) {
                    const auto entry = start + n * 8u;
                    if (!(kind == 0 ? indexed : anonymous).insert(entry).second) Fail("Duplicate CRO import slice");
                    const auto value = Word(p.bytes, entry);
                    if (kind == 0 && value >= provider->indexed_exports.size()) Fail("Invalid CRO indexed import");
                    batch(Word(p.bytes, entry + 4), kind == 0 ? provider->indexed_exports[value] : Tag(*provider, value),
                          provider->load_address);
                }
            }
        }
        if (visited.size() != p.h[31] || indexed.size() != p.h[35] || anonymous.size() != p.h[37])
            Fail("Unreferenced CRO imports or relocations");
        for (U32 i = 0; i < p.h[43]; ++i) {
            const auto at = p.Entry(15, i);
            const auto index = p.bytes[at + 5];
            if (index >= module.segments.size()) Fail("Invalid CRO internal relocation symbol");
            patch(Tag(module, Word(p.bytes, at), 4), p.bytes[at + 4],
                     module.segments[index].address, Word(p.bytes, at + 8));
        }
        // Build all throwing metadata before committing any guest bytes.
        auto modules = modules_;
        if (!modules.emplace(base, module).second) Fail("CRO address already loaded");
        CROModule result = module;
        if (!memory.ReserveDynamicRange(base, module.mapped_size)) Fail("CRO mapping overlaps an allocation");
        memory.LoadImage(base, image);
        modules_.swap(modules); symbols_.swap(symbols); memory_ = &memory;
        out = std::move(result); error_.clear(); return true;
    } catch (const std::exception& e) { error_ = e.what(); return false; }
}

bool CROLoader::UnloadModule(WiiUMemory& memory, U32 base) {
    const auto it = modules_.find(base);
    if (memory_ != &memory || it == modules_.end()) { error_ = "CRO module not loaded"; return false; }
    for (const auto& [address, module] : modules_) {
        (void)address;
        if (module.dependencies.contains(base)) { error_ = "CRO module still imported"; return false; }
    }
    memory.ClearRange(base, it->second.mapped_size);
    if (!memory.ReleaseDynamicRange(base, it->second.mapped_size)) {
        error_ = "CRO reservation missing"; return false;
    }
    for (const auto& [name, value] : it->second.exports) { (void)value; symbols_.erase(name); }
    modules_.erase(it); error_.clear(); return true;
}
} // namespace wiiu
