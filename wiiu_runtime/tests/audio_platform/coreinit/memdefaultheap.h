#pragma once
#include <cstdint>
void* MEMAllocFromDefaultHeapEx(std::uint32_t size, std::int32_t alignment);
void MEMFreeToDefaultHeap(void* pointer);
