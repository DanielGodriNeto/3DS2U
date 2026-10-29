#include "aoc_manager.hpp"
#include "hle_services.hpp"
#include "pica200.hpp"
#include "romfs_reader.hpp"
#include "wiiu_memory.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
#include <utility>
#include <vector>
#include <vpad/input.h>

namespace {
using HLE = wiiu::hle::HLEServiceManager;
void Check(bool condition, std::source_location at = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error("AOC test failed at line " + std::to_string(at.line()));
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
void Put32(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes.at(at + i) = static_cast<std::uint8_t>(value >> (i * 8));
}
void Put64(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint64_t value) {
    Put32(bytes, at, static_cast<std::uint32_t>(value));
    Put32(bytes, at + 4, static_cast<std::uint32_t>(value >> 32));
}
using Entry = std::pair<std::u16string, std::string>;
std::vector<std::uint8_t> Image(std::initializer_list<Entry> entries) {
    constexpr auto none = 0xFFFFFFFFu;
    std::vector<std::uint8_t> records, payload;
    for (const auto &[name, contents] : entries) {
        const auto at = records.size();
        records.resize((at + 32 + name.size() * 2 + 3) & ~std::size_t{3});
        Put32(records, at, 0);
        Put32(records, at + 4, static_cast<std::uint32_t>(records.size()));
        Put64(records, at + 8, payload.size());
        Put64(records, at + 16, contents.size());
        Put32(records, at + 24, none);
        Put32(records, at + 28, static_cast<std::uint32_t>(name.size() * 2));
        for (std::size_t i = 0; i < name.size(); ++i) {
            records[at + 32 + i * 2] = static_cast<std::uint8_t>(name[i]);
            records[at + 33 + i * 2] = static_cast<std::uint8_t>(name[i] >> 8);
        }
        payload.insert(payload.end(), contents.begin(), contents.end());
    }
    if (!records.empty()) {
        std::size_t at = 0;
        for (std::size_t i = 0; i + 1 < entries.size(); ++i) {
            const auto &name = (entries.begin() + i)->first;
            at = (at + 32 + name.size() * 2 + 3) & ~std::size_t{3};
        }
        Put32(records, at + 4, none);
    }
    const auto data = (0x48u + records.size() + 15u) & ~std::size_t{15};
    std::vector<std::uint8_t> image(data + payload.size());
    const std::array<std::uint32_t, 10> header{0x28,
                                               0x28,
                                               4,
                                               0x2C,
                                               24,
                                               0x44,
                                               4,
                                               0x48,
                                               static_cast<std::uint32_t>(records.size()),
                                               static_cast<std::uint32_t>(data)};
    for (unsigned i = 0; i < header.size(); ++i)
        Put32(image, i * 4, header[i]);
    Put32(image, 0x28, 0);
    Put32(image, 0x2C, 0);
    Put32(image, 0x30, none);
    Put32(image, 0x34, none);
    Put32(image, 0x38, records.empty() ? none : 0);
    Put32(image, 0x3C, none);
    Put32(image, 0x44, records.empty() ? none : 0);
    std::copy(records.begin(), records.end(), image.begin() + 0x48);
    std::copy(payload.begin(), payload.end(), image.begin() + data);
    return image;
}
std::string Read(const wiiu::RomFSReader &reader, const wiiu::RomFSFileHandle &handle) {
    std::string contents(static_cast<std::size_t>(handle.GetSize()), '\0');
    Check(reader.ReadFile(handle, 0, contents.data(), contents.size()) == contents.size());
    return contents;
}
std::string Open(const wiiu::RomFSReader &reader, const std::string &path) {
    wiiu::RomFSFileHandle handle;
    Check(reader.OpenFile(path, handle));
    return Read(reader, handle);
}
constexpr std::uint32_t ipc = 0x08000000, output = 0x08001000;
void Request(wiiu::WiiUMemory &memory, std::initializer_list<std::uint32_t> words) {
    std::uint32_t address = ipc;
    for (const auto word : words) {
        memory.Write32(address, word);
        address += 4;
    }
}
void WriteImage(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    Check(!file.fail());
}
} // namespace

// Host substitutes retain the production FCRAM, filesystem and IPC paths.
void *MEMAllocFromDefaultHeapEx(std::uint32_t size, std::int32_t alignment) {
    Check(alignment == 0x1000);
    return ::operator new(size, std::align_val_t{0x1000}, std::nothrow);
}
void MEMFreeToDefaultHeap(void *pointer) { ::operator delete(pointer, std::align_val_t{0x1000}); }
void OSReport(const char *, ...) {}
void wiiu::Pica200::WriteMMIO(std::uint32_t, std::uint32_t, std::uint8_t) {
    throw std::logic_error("Unexpected graphics access in AOC test");
}
std::int32_t VPADRead(VPADChan, VPADStatus *, std::uint32_t, VPADReadError *error) {
    *error = VPAD_READ_NO_SAMPLES;
    return 0;
}
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution, VPADTouchData *, const VPADTouchData *) {}

int main() try {
    const auto base = Image({{u"base.bin", "BASE_ONLY"}, {u"shared.bin", "BASE_VERSION"}});
    auto dlc = Image({{u"dlc.bin", "DLC_ONLY"}, {u"shared.bin", "DLC_VERSION"}});
    wiiu::RomFSReader reader;
    Check(reader.Mount(base) && reader.GetMountCount() == 1);
    wiiu::RomFSFileHandle old;
    Check(reader.OpenFile(std::string{"/shared.bin"}, old));
    Check(reader.MountLayer(dlc) && reader.GetMountCount() == 2);
    dlc.back() = 0;
    Check(Open(reader, "romfs:/base.bin") == "BASE_ONLY");
    Check(Open(reader, "romfs:/dlc.bin") == "DLC_ONLY");
    Check(Open(reader, "romfs:/shared.bin") == "DLC_VERSION");
    Check(Read(reader, old) == "BASE_VERSION");
    Check(reader.OpenFile(std::u16string_view{u"/shared.bin"}, old) && Read(reader, old) == "DLC_VERSION");
    Check(!reader.MountLayer(std::array<std::uint8_t, 4>{}) && reader.GetMountCount() == 2);
    Check(!reader.GetLastError().empty() && Read(reader, old) == "DLC_VERSION");
    for (unsigned i = 0; i < 12; ++i)
        Check(reader.MountLayer(Image({})));
    Check(Read(reader, old) == "DLC_VERSION" && Open(reader, "/base.bin") == "BASE_ONLY");
    Check(reader.MountLayer(Image({{u"shared.bin", "THIRD_LAYER"}})));
    Check(Open(reader, "/shared.bin") == "THIRD_LAYER");
    Check(reader.MountLayer(Image({{u"shared.bin", ""}})) && Open(reader, "/shared.bin").empty());
    Check(Read(reader, old) == "DLC_VERSION");
    reader.Unmount();
    Reject([&] { (void)Read(reader, old); });
    Check(reader.GetMountCount() == 0 && !reader.OpenFile(std::string{"/base.bin"}, old));

    const auto folder =
        std::filesystem::path("build") /
        ("aoc-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(std::filesystem::create_directories(folder));
    WriteImage(folder / "base.romfs", base);
    const auto dlc_file = Image({{u"dlc.bin", "DLC_ONLY"}, {u"shared.bin", "DLC_VERSION"}});
    WriteImage(folder / "dlc.romfs", dlc_file);
    Check(reader.MountFile((folder / "base.romfs").string()));
    Check(reader.MountLayerFile((folder / "dlc.romfs").string()));
    Check(Open(reader, "/shared.bin") == "DLC_VERSION" && Open(reader, "/base.bin") == "BASE_ONLY");
    Check(reader.OpenFile(std::string{"/shared.bin"}, old));
    Check(!reader.MountLayerFile((folder / "missing.romfs").string()) && reader.GetMountCount() == 2);
    Check(Read(reader, old) == "DLC_VERSION");
    Check(!reader.MountFile((folder / "missing.romfs").string()) && reader.GetMountCount() == 0);
    Reject([&] { (void)Read(reader, old); });
    Check(reader.Mount(base) && reader.MountLayer(dlc_file));

    wiiu::WiiUMemory memory;
    HLE services;
    std::uint32_t session{}, missing = 99;
    Check(services.OpenServicePort("missing", missing) == HLE::kFSNotFound && missing == 0);
    Check(services.OpenServicePort("aoc:u", session) == 0 && session != 0);
    auto &aoc = services.GetAocManager();
    Request(memory, {0x00020000});
    Check(services.HandleServiceIPC(memory, session, ipc) == 0 && memory.Read32(ipc) == 0x00020080 &&
          memory.Read32(ipc + 8) == 0);
    const std::array<std::uint64_t, 3> titles{0x0004008C87654321ull, 0x0004008C12345678ull,
                                              0x0004008C12345678ull};
    aoc.SetInstalledDLCs(0x0004000012345600ull, titles);
    Check(aoc.CountAddOnContent() == 2 && aoc.GetInstalledTitleIDs()[0] == titles[1]);
    Request(memory, {0x00020000});
    Check(services.HandleServiceIPC(memory, session, ipc) == 0 && memory.Read32(ipc + 8) == 2);
    std::array<std::uint8_t, 64> guard;
    guard.fill(0xCD);
    memory.LoadImage(output, guard);
    Request(memory, {0x00030042, 4, (32u << 4) | 0xCu, output});
    Check(services.HandleServiceIPC(memory, session, ipc) == 0 && memory.Read32(ipc) == 0x00030082 &&
          memory.Read32(ipc + 8) == 2);
    const auto *bytes = static_cast<const std::uint8_t *>(memory.GetPointer(output, 64));
    const std::array<std::uint8_t, 8> first{0x78, 0x56, 0x34, 0x12, 0x8C, 0, 4, 0};
    Check(std::equal(first.begin(), first.end(), bytes));
    Check(memory.Read32(output + 8) == 0x87654321 && memory.Read32(output + 12) == 0x0004008C);
    Check(std::all_of(bytes + 16, bytes + 64, [](auto b) { return b == 0xCD; }));
    Request(memory, {0x00030042, 1, (8u << 4) | 0xCu, output});
    Check(aoc.HandleIPC(memory, ipc) == 0 && memory.Read32(ipc + 8) == 1);
    Request(memory, {0x00030042, 0, 0xC, 0});
    Check(aoc.HandleIPC(memory, ipc) == 0 && memory.Read32(ipc + 8) == 0);
    memory.LoadImage(output, guard);
    for (const auto descriptor : {(16u << 4) | 0xAu, (8u << 4) | 0xCu}) {
        Request(memory, {0x00030042, 2, descriptor, output});
        Check(aoc.HandleIPC(memory, ipc) == wiiu::AocManager::kInvalidArgument &&
              memory.Read32(ipc) == 0x00030040);
        Check(std::memcmp(memory.GetPointer(output, 64), guard.data(), 64) == 0);
    }
    for (const auto bad_output : {0x10000000u, output + 1, ipc + 4}) {
        Request(memory, {0x00030042, 2, (16u << 4) | 0xCu, bad_output});
        Check(aoc.HandleIPC(memory, ipc) == wiiu::AocManager::kInvalidArgument);
    }
    Request(memory, {0x00030042, 0xFFFFFFFF, 0xC, output});
    Check(aoc.HandleIPC(memory, ipc) == wiiu::AocManager::kInvalidArgument);
    Check(aoc.HandleIPC(memory, ipc + 1) == wiiu::AocManager::kInvalidArgument);
    memory.Write32(0x0FFFFFFC, 0x00020000);
    Check(aoc.HandleIPC(memory, 0x0FFFFFFC) == wiiu::AocManager::kInvalidArgument &&
          memory.Read32(0x0FFFFFFC) == 0x00020000);
    Request(memory, {0x00040000});
    Check(aoc.HandleIPC(memory, ipc) == wiiu::AocManager::kNotSupported);
    Reject([&] { aoc.SetInstalledDLCs(0, titles); });
    const std::array<std::uint64_t, 1> zero{0};
    Reject([&] { aoc.SetInstalledDLCs(1, zero); });
    Check(aoc.CountAddOnContent() == 2);
    const std::array<std::uint64_t, 1> other{0x0004008C00005678ull};
    aoc.SetInstalledDLCs(0x0004000000005600ull, other);
    Check(aoc.CountAddOnContent() == 1 && aoc.GetGameTitleID() == 0x0004000000005600ull);

    // The same service table must retain fs:USER routing into the layered reader.
    services.AttachRomFS(reader);
    std::uint32_t fs_session{};
    Check(services.OpenServicePort("fs:USER", fs_session) == 0);
    Request(memory, {0x08010002, 0x20, 0});
    Check(services.HandleServiceIPC(memory, fs_session, ipc) == 0);
    constexpr std::uint32_t path_address = output + 128, archive_address = output + 256;
    const std::array<std::uint8_t, 12> name{'/', 's', 'h', 'a', 'r', 'e', 'd', '.', 'b', 'i', 'n', 0};
    memory.LoadImage(path_address, name);
    memory.Write8(archive_address, 0);
    const std::array<std::uint32_t, 13> request{
        0x08030204,       0,           3, 1, 1, 3, 12, 1, 0, (1u << 14) | (2u << 10) | 2u, archive_address,
        (12u << 14) | 2u, path_address};
    for (std::uint32_t i = 0; i < request.size(); ++i)
        memory.Write32(ipc + i * 4, request[i]);
    Check(services.HandleServiceIPC(memory, fs_session, ipc) == 0);
    const auto file = memory.Read32(ipc + 12);
    std::uint32_t read{};
    Check(services.FSFILE_Read(memory, file, 0, output, 64, read) == 0 && read == 11);
    Check(std::memcmp(memory.GetPointer(output, 11), "DLC_VERSION", 11) == 0);
    Check(services.FSFILE_Close(file) == 0);
    Check(services.CloseServicePort(session) == 0 &&
          services.HandleServiceIPC(memory, session, ipc) == HLE::kFSInvalidHandle);
    Check(services.OpenServicePort("aoc:u", missing) == 0 && missing > session);
    Check(services.srvExit() == 0 &&
          services.HandleServiceIPC(memory, missing, ipc) == HLE::kFSInvalidHandle);
    std::cout << "AOC count/list, service routing, LE buffers and layered RomFS tests passed.\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
