#include "packaged_romfs.hpp"
namespace wiiu {
__attribute__((weak)) std::span<const std::uint8_t> GetPackagedRomFS() noexcept { return {}; }
}
