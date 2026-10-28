#pragma once
#include <cstdint>
#include <utility>
#include <vector>
namespace ax_mock { inline std::vector<std::pair<void*, std::uint32_t>> stores; }
inline void DCStoreRange(void* address, std::uint32_t size) { ax_mock::stores.emplace_back(address, size); }
