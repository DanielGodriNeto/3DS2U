#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wiiu {
class WiiUMemory;

struct CROSegment {
    std::uint32_t address{}, size{}, type{}; // Code=0, rodata=1, data=2, BSS=3.
};

struct CROModule {
    std::string name;
    std::uint32_t load_address{}, mapped_size{};
    std::vector<CROSegment> segments;
    std::map<std::string, std::uint32_t> exports;
    std::vector<std::uint32_t> indexed_exports;
    std::set<std::uint32_t> dependencies;
    [[nodiscard]] bool IsExecutable(std::uint32_t address) const noexcept;
};

// Unfixed CRO0 images only. Guest bytes are data to PPC, never native code.
// Serialize all calls with the engine execution lock; memory must outlive modules.
class CROLoader {
public:
    bool RegisterSymbol(const std::string& name, std::uint32_t address);
    [[nodiscard]] bool FindSymbol(const std::string& name, std::uint32_t& address) const;
    bool LoadModule(WiiUMemory& memory, const std::uint8_t* cro_data, std::size_t size,
                    std::uint32_t load_addr, CROModule& out_module);
    bool UnloadModule(WiiUMemory& memory, std::uint32_t load_addr);
    [[nodiscard]] const std::string& LastError() const noexcept { return error_; }
private:
    struct Symbol { std::uint32_t address{}, owner{}; };
    std::map<std::string, Symbol> symbols_;
    std::map<std::uint32_t, CROModule> modules_;
    WiiUMemory* memory_{};
    std::string error_;
};
} // namespace wiiu
