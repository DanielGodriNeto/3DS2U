#pragma once
#include <mutex>
struct OSMutex { std::recursive_mutex value; };
inline void OSInitMutex(OSMutex*) {}
inline void OSLockMutex(OSMutex* mutex) { mutex->value.lock(); }
inline void OSUnlockMutex(OSMutex* mutex) { mutex->value.unlock(); }
