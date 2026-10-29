#include "hle_services.hpp"
#include "pica200.hpp"
#include "save_manager.hpp"
#include "wiiu_memory.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>
#include <vector>
#include <vpad/input.h>

namespace {
namespace fs = std::filesystem;
using HLE = wiiu::hle::HLEServiceManager;
void Check(bool value, std::source_location at = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Save persistence test failed at line " + std::to_string(at.line()));
}
template <class Operation> void Reject(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception &) {
        rejected = true;
    }
    Check(rejected);
}
constexpr std::uint32_t ipc = 0x08000000, path_addr = 0x08000800, archive_addr = 0x08000900,
                        input = 0x08001000, output = 0x08020000;
void Request(wiiu::WiiUMemory &memory, std::initializer_list<std::uint32_t> words) {
    std::uint32_t address = ipc;
    for (const auto word : words) {
        memory.Write32(address, word);
        address += 4;
    }
}
std::uint32_t Path(wiiu::WiiUMemory &memory, const std::string &text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    memory.LoadImage(path_addr, bytes);
    return static_cast<std::uint32_t>(bytes.size());
}
std::uint64_t ArchiveIPC(HLE &hle, wiiu::WiiUMemory &memory) {
    memory.Write8(archive_addr, 0);
    Request(memory, {0x080C00C2, 4, 1, 1, (1u << 14) | 2u, archive_addr});
    Check(hle.HandleFSUserIPC(memory, ipc) == 0 && memory.Read32(ipc) == 0x080C00C0);
    return memory.Read32(ipc + 8) | (std::uint64_t{memory.Read32(ipc + 12)} << 32);
}
} // namespace

// Platform substitutes exercise the production FS services and guest memory.
void *MEMAllocFromDefaultHeapEx(std::uint32_t size, std::int32_t alignment) {
    Check(alignment == 0x1000);
    return ::operator new(size, std::align_val_t{0x1000}, std::nothrow);
}
void MEMFreeToDefaultHeap(void *pointer) { ::operator delete(pointer, std::align_val_t{0x1000}); }
void OSReport(const char *, ...) {}
void wiiu::Pica200::WriteMMIO(std::uint32_t, std::uint32_t, std::uint8_t) {
    throw std::logic_error("Unexpected graphics access in save test");
}
std::int32_t VPADRead(VPADChan, VPADStatus *, std::uint32_t, VPADReadError *error) {
    *error = VPAD_READ_NO_SAMPLES;
    return 0;
}
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution, VPADTouchData *, const VPADTouchData *) {}

int main() try {
    // Leave unique fixtures under build; never format a user's configured save root.
    const auto base = fs::absolute(
        fs::path("build") /
        ("save-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    Check(!fs::exists(base));
    constexpr std::uint64_t title = 0x0004000000123400ull;
    wiiu::SaveManager saves(title, base.string());
    wiiu::SaveManager other(title + 1, base.string());
    wiiu::WiiUMemory memory;
    HLE hle;
    hle.AttachSaveManager(saves);
    std::uint64_t archive{};
    Check(hle.FSUSER_OpenArchive(4, archive) == HLE::kFSNotMounted);
    Check(hle.FSUSER_Initialize() == 0);
    Check(hle.FSUSER_FormatThisUserSaveData() == 0);
    Check(hle.FSUSER_OpenArchive(4, archive) == 0 && archive != 0);
    Check(hle.FSUSER_CreateFile(archive, "/save01.dat", 1024) == 0);
    Check(hle.FSUSER_CreateFile(archive, "/save01.dat", 0) == HLE::kFSAlreadyExists);
    std::uint32_t file{};
    Check(hle.FSUSER_OpenFile(archive, "/save01.dat", 3, file) == 0);
    const std::array<std::uint8_t, 19> message{'3', 'D', 'S', '2', 'U',  '_',  'S',  'A',  'V', 'E',
                                               '_', 'O', 'K', 0,   0x12, 0x34, 0x80, 0xFF, 0x56};
    memory.LoadImage(input, message);
    std::uint32_t transferred{};
    Check(hle.FSFILE_Write(memory, file, 0, input, static_cast<std::uint32_t>(message.size()), 1,
                           transferred) == 0);
    Check(transferred == message.size());
    Check(hle.FSFILE_Read(memory, file, 0, output, 1024, transferred) == 0 && transferred == 1024);
    const auto *read = static_cast<const std::uint8_t *>(memory.GetPointer(output, 1024));
    Check(std::equal(message.begin(), message.end(), read));
    Check(std::all_of(read + message.size(), read + 1024, [](auto b) { return b == 0; }));
    const auto physical = fs::path(saves.GetArchivePath(4)) / "save01.dat";
    Check(fs::is_regular_file(physical) && fs::file_size(physical) == 1024);
    Check(hle.FSFILE_Close(file) == 0 && hle.FSFILE_Close(file) == HLE::kFSInvalidHandle);
    {
        std::ifstream stream(physical, std::ios::binary);
        std::array<std::uint8_t, 19> disk{};
        stream.read(reinterpret_cast<char *>(disk.data()), disk.size());
        Check(stream.good() && disk == message);
    }
    {
        wiiu::SaveManager reopened(title, base.string());
        reopened.OpenArchive(4);
        auto handle = reopened.OpenFile(4, "/save01.dat", 7);
        std::array<std::uint8_t, 19> disk{};
        Check(reopened.GetSize(handle) == 1024 &&
              reopened.ReadFile(handle, 0, disk.data(), disk.size()) == disk.size() && disk == message);
        Reject([&] { saves.ReadFile(handle, 0, disk.data(), 1); });
        reopened.CloseFile(handle);
        Reject([&] { reopened.ReadFile(handle, 0, disk.data(), 1); });
    }
    Check(hle.FSUSER_OpenFile(archive, "/save01.dat", 1, file) == 0);
    Check(hle.FSFILE_Write(memory, file, 0, input, 1, 0, transferred) == HLE::kFSAccessDenied);
    Check(hle.FSFILE_Close(file) == 0);
    Check(hle.FSUSER_CreateDirectory(archive, "/slot") == 0);
    Check(hle.FSUSER_CreateFile(archive, "/slot/big.dat", 0) == 0);
    Check(hle.FSUSER_OpenFile(archive, "/slot/big.dat", 3, file) == 0);
    std::vector<std::uint8_t> large(70000);
    for (std::size_t i = 0; i < large.size(); ++i)
        large[i] = static_cast<std::uint8_t>(i * 31u);
    memory.LoadImage(input, large);
    Check(hle.FSFILE_Write(memory, file, 17, input, 70000, 0x101, transferred) == 0 && transferred == 70000);
    Check(hle.FSFILE_Read(memory, file, 0, output, 70017, transferred) == 0 && transferred == 70017);
    read = static_cast<const std::uint8_t *>(memory.GetPointer(output, 70017));
    Check(std::all_of(read, read + 17, [](auto b) { return b == 0; }) &&
          std::equal(large.begin(), large.end(), read + 17));
    Check(hle.FSFILE_Write(memory, file, std::numeric_limits<std::uint64_t>::max(), input, 1, 0,
                           transferred) != 0);
    Check(hle.FSFILE_Write(memory, file, 0, 0x10000000, 1, 0, transferred) != 0);
    Check(hle.FSFILE_Write(memory, file, 0, 0, 0, 1, transferred) == 0 && transferred == 0);
    Check(hle.FSFILE_Close(file) == 0);
    for (const auto *path :
         {"/../escape", "/a/../b", "C:/escape", "//escape", "/a\\b", "/NUL.txt", "/trailing.", "/trailing "})
        Check(hle.FSUSER_CreateFile(archive, path, 1) == HLE::kFSInvalidArgument);
    Check(hle.FSUSER_CreateFile(archive, std::string("/a\0b", 4), 1) != 0);
    Check(hle.FSUSER_CreateFile(archive, "/missing/file", 1) != 0);
    Reject([&] { saves.OpenArchive(3); });

    // Exercise the native IPC layouts, including LE 64-bit archive handles.
    const auto ipc_archive = ArchiveIPC(hle, memory);
    const auto lo = static_cast<std::uint32_t>(ipc_archive),
               hi = static_cast<std::uint32_t>(ipc_archive >> 32);
    auto length = Path(memory, "/ipc.dat");
    Request(memory, {0x08080202, 0, lo, hi, 3, length, 0, 1024, 0, (length << 14) | 2u, path_addr});
    Check(hle.HandleFSUserIPC(memory, ipc) == 0 && memory.Read32(ipc + 4) == 0);
    Request(memory, {0x080201C2, 0, lo, hi, 3, length, 3, 0, (length << 14) | 2u, path_addr});
    Check(hle.HandleFSUserIPC(memory, ipc) == 0 && memory.Read32(ipc + 8) == 0x10);
    file = memory.Read32(ipc + 12);
    memory.LoadImage(input, message);
    const auto n = static_cast<std::uint32_t>(message.size());
    Request(memory, {0x08030102, 0, 0, n, 1, (n << 4) | 0xAu, input});
    Check(hle.HandleFSFileIPC(memory, file, ipc) == 0 && memory.Read32(ipc + 8) == n);
    Request(memory, {0x080200C2, 0, 0, n, (n << 4) | 0xCu, output});
    Check(hle.HandleFSFileIPC(memory, file, ipc) == 0 && memory.Read32(ipc + 8) == n);
    Check(std::memcmp(memory.GetPointer(output, n), message.data(), n) == 0);
    Request(memory, {0x08040000, 0});
    Check(hle.HandleFSFileIPC(memory, file, ipc) == 0 && memory.Read32(ipc + 8) == 1024);
    Request(memory, {0x08090000, 0});
    Check(hle.HandleFSFileIPC(memory, file, ipc) == 0);
    Request(memory, {0x08080000, 0});
    Check(hle.HandleFSFileIPC(memory, file, ipc) == 0);
    // Unsupported archive paths must still receive an error response packet.
    Request(memory, {0x080C00C2, 4, 2, 1, (1u << 14) | 2u, archive_addr});
    Check(hle.HandleFSUserIPC(memory, ipc) != 0 && memory.Read32(ipc) == 0x080C0040 &&
          memory.Read32(ipc + 4) != 0);
    // UTF-16LE file names are converted independently of host byte order.
    const std::array<std::uint8_t, 10> unicode{'/', 0, 'x', 0, 0xE9, 0, 'a', 0, 0, 0};
    memory.LoadImage(path_addr, unicode);
    Request(memory, {0x08080202, 0, lo, hi, 4, 10, 0, 1, 0, (10u << 14) | 2u, path_addr});
    Check(hle.HandleFSUserIPC(memory, ipc) == 0);
    const std::array<std::uint8_t, 6> cjk{'/', 0, 0x22, 0x6F, 0, 0};
    memory.LoadImage(path_addr, cjk);
    Request(memory, {0x08080202, 0, lo, hi, 4, 6, 0, 1, 0, (6u << 14) | 2u, path_addr});
    Check(hle.HandleFSUserIPC(memory, ipc) == 0);
    length = Path(memory, "/ipc.dat");
    Request(memory, {0x08030204, 0, 4, 1, 1, 3, length, 1, 0, (1u << 14) | (2u << 10) | 2u, archive_addr,
                     (length << 14) | 2u, path_addr});
    Check(hle.HandleFSUserIPC(memory, ipc) == 0);
    file = memory.Read32(ipc + 12);
    Check(hle.FSFILE_Read(memory, file, 0, output, n, transferred) == 0 && transferred == n);
    Check(std::memcmp(memory.GetPointer(output, n), message.data(), n) == 0);
    Check(hle.FSFILE_Close(file) == 0);
    Request(memory, {0x08080202, 0, lo, hi, 3, length, 0, 0, 1, (length << 14) | 2u, path_addr});
    Check(hle.HandleFSUserIPC(memory, ipc) == HLE::kFSInvalidArgument);
    memory.Write32(archive_addr, 1);
    memory.Write32(archive_addr + 4, 0x55667788);
    memory.Write32(archive_addr + 8, 0x11223344);
    Request(memory, {0x080C00C2, 6, 2, 12, (12u << 14) | 2u, archive_addr});
    Check(hle.HandleFSUserIPC(memory, ipc) == 0);
    const auto ext_archive = memory.Read32(ipc + 8) | (std::uint64_t{memory.Read32(ipc + 12)} << 32);
    Check(hle.FSUSER_CreateFile(ext_archive, "/ipc-ext.dat", 1) == 0);
    Check(fs::exists(fs::path(saves.GetArchivePath(6, 0x1122334455667788ull)) / "ipc-ext.dat"));

    // Formatting one title's SaveData must preserve ExtData and other titles.
    std::uint64_t ext{};
    Check(hle.FSUSER_OpenArchive(6, ext, 0x1122334455667788ull) == 0);
    Check(hle.FSUSER_CreateFile(ext, "/ext.dat", 4) == 0);
    other.OpenArchive(4);
    other.CreateFile(4, "/other.dat", 4);
    Check(hle.FSUSER_OpenFile(archive, "/save01.dat", 3, file) == 0);
    Request(memory, {0x080F0180, 1, 16, 16, 17, 17, 0});
    Check(hle.HandleFSUserIPC(memory, ipc) == 0);
    Check(!fs::exists(physical));
    Check(hle.FSFILE_Read(memory, file, 0, output, 1, transferred) == HLE::kFSInvalidHandle);
    Check(hle.FSUSER_CreateFile(archive, "/stale.dat", 1) == HLE::kFSInvalidHandle);
    Check(fs::exists(fs::path(saves.GetArchivePath(6, 0x1122334455667788ull)) / "ext.dat"));
    Check(fs::exists(fs::path(other.GetArchivePath(4)) / "other.dat"));
    Check(hle.FSUSER_CreateFile(ext, "/retained.dat", 1) == 0);
    // Reject links during format before removing any archive entry.
    const auto root = fs::path(saves.GetArchivePath(4));
    const auto outside = base / "outside";
    fs::create_directory(outside);
    {
        std::ofstream sentinel(outside / "sentinel");
        sentinel << "KEEP";
    }
    std::error_code error;
    fs::create_directory_symlink(outside, root / "external", error);
    if (!error) {
        Reject([&] { saves.CreateFile(4, "/external/escaped", 1); });
        Reject([&] { saves.FormatThisUserSaveData(); });
        Check(fs::exists(outside / "sentinel"));
        fs::remove(root / "external");
    } else
        std::cout << "Symlink test skipped: host does not permit creating symlinks.\n";
    // A file in place of a directory must produce an I/O failure, not false success.
    const auto blocked = base / "blocked";
    {
        std::ofstream stream(blocked);
        stream << "BLOCK";
    }
    wiiu::SaveManager unavailable(title, blocked.string());
    Reject([&] { unavailable.OpenArchive(4); });
    std::cout << "SaveData/ExtData persistence, raw bytes, IPC, isolation and path tests passed.\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
