#include "hle_services.hpp"

#include <coreinit/debug.h>

namespace wiiu::hle {

std::uint32_t HLEServiceManager::srvInit() {
    OSReport("[3DS2U HLE] srvInit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::aptInit() {
    OSReport("[3DS2U HLE] aptInit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::aptMainLoop() {
    OSReport("[3DS2U HLE] aptMainLoop: stub continue\n");
    return 1;
}

std::uint32_t HLEServiceManager::aptExit() {
    OSReport("[3DS2U HLE] aptExit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::srvExit() {
    OSReport("[3DS2U HLE] srvExit: stub success\n");
    return 0;
}

} // namespace wiiu::hle
