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
    // Asked once per LoadModule after relocation, before the module is visible. True lets
    // its code run; false or a throw rejects the load with "CRO module code is not translated".
    using CodePolicy = bool (*)(void* context, const CROModule& module);
    void SetCodePolicy(CodePolicy policy, void* context) noexcept;
    bool RegisterSymbol(const std::string& name, std::uint32_t address);
    [[nodiscard]] bool FindSymbol(const std::string& name, std::uint32_t& address) const;
    bool LoadModule(WiiUMemory& memory, const std::uint8_t* cro_data, std::size_t size,
                    std::uint32_t load_addr, CROModule& out_module);
    bool UnloadModule(WiiUMemory& memory, std::uint32_t load_addr);
    [[nodiscard]] const std::string& LastError() const noexcept { return error_; }
    // Name of the loaded module whose [load, load + mapped_size) contains address, else nullptr.
    // The pointer stays valid until that module is unloaded.
    [[nodiscard]] const char* FindModuleName(std::uint32_t address) const noexcept;
private:
    struct Symbol { std::uint32_t address{}, owner{}; };
    [[nodiscard]] bool AllowsCode(const CROModule& module) const;
    CodePolicy code_policy_{};
    void* code_policy_context_{};
    std::map<std::string, Symbol> symbols_;
    std::map<std::uint32_t, CROModule> modules_;
    WiiUMemory* memory_{};
    std::string error_;
};
} // namespace wiiu
