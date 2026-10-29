#include "romfs_reader.hpp"
#include "hle_services.hpp"
#include "wiiu_memory.hpp"
#include "pica200.hpp"
#include <vpad/input.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::uint32_t none = 0xFFFFFFFFu;
void Check(bool condition) { if (!condition) throw std::runtime_error("RomFS test failed"); }
template<class Operation> void Reject(Operation operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    Check(rejected);
}
void Put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) b.at(at + i) = static_cast<std::uint8_t>(value >> (i * 8));
}
void Put64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t value) {
    Put32(b, at, static_cast<std::uint32_t>(value)); Put32(b, at + 4, static_cast<std::uint32_t>(value >> 32));
}
std::uint32_t Record(std::vector<std::uint8_t>& table, unsigned fixed, std::u16string_view name) {
    const auto start = static_cast<std::uint32_t>(table.size());
    table.resize((table.size() + fixed + name.size() * 2 + 3) & ~std::size_t{3});
    Put32(table, start + fixed - 4, static_cast<std::uint32_t>(name.size() * 2));
    for (std::size_t i = 0; i < name.size(); ++i) {
        table[start + fixed + i * 2] = static_cast<std::uint8_t>(name[i]);
        table[start + fixed + i * 2 + 1] = static_cast<std::uint8_t>(name[i] >> 8);
    }
    return start;
}
struct Fixture {
    std::vector<std::uint8_t> image;
    std::uint32_t dir_offset{}, file_offset{}, data_offset{};
};
Fixture MakeImage() {
    std::vector<std::uint8_t> dirs, files, payload;
    const auto root = Record(dirs, 24, u"");
    const auto textures = Record(dirs, 24, u"textures");
    const auto test = Record(files, 32, u"test.txt");
    const auto player = Record(files, 32, u"player.bin");
    const auto unicode = Record(files, 32, u"caf\u00E9.bin");
    const auto big = Record(files, 32, u"big.bin");
    Put32(dirs, root, 0); Put32(dirs, root + 4, none);
    Put32(dirs, root + 8, textures); Put32(dirs, root + 12, test); Put32(dirs, root + 16, textures);
    Put32(dirs, textures, 0); Put32(dirs, textures + 4, none);
    Put32(dirs, textures + 8, none); Put32(dirs, textures + 12, player); Put32(dirs, textures + 16, none);
    const auto file = [&](std::uint32_t at, std::uint32_t parent, std::uint32_t sibling,
                          std::uint32_t hash, const std::string& text) {
        Put32(files, at, parent); Put32(files, at + 4, sibling); Put32(files, at + 24, hash);
        Put64(files, at + 8, payload.size()); Put64(files, at + 16, text.size());
        payload.insert(payload.end(), text.begin(), text.end());
    };
    file(test, 0, unicode, player, "3DS2U_ROMFS_OK");
    file(player, textures, none, unicode, "PIXELS");
    file(unicode, 0, big, big, "UNICODE");
    std::string large(6000, '\0');
    for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<char>(i % 127);
    file(big, 0, none, none, large);
    Fixture fixture;
    fixture.dir_offset = 0x2C;
    const auto file_hash = fixture.dir_offset + static_cast<std::uint32_t>(dirs.size());
    fixture.file_offset = file_hash + 4;
    fixture.data_offset = (fixture.file_offset + static_cast<std::uint32_t>(files.size()) + 15u) & ~15u;
    fixture.image.resize(fixture.data_offset + payload.size());
    const std::uint32_t header[]{0x28, 0x28, 4, fixture.dir_offset, static_cast<std::uint32_t>(dirs.size()),
        file_hash, 4, fixture.file_offset, static_cast<std::uint32_t>(files.size()), fixture.data_offset};
    for (unsigned i = 0; i < 10; ++i) Put32(fixture.image, i * 4, header[i]);
    Put32(fixture.image, 0x28, 0); Put32(fixture.image, file_hash, 0);
    std::copy(dirs.begin(), dirs.end(), fixture.image.begin() + fixture.dir_offset);
    std::copy(files.begin(), files.end(), fixture.image.begin() + fixture.file_offset);
    std::copy(payload.begin(), payload.end(), fixture.image.begin() + fixture.data_offset);
    return fixture;
}
constexpr std::uint32_t ipc = 0x08000000u, path_address = 0x08000800u,
    archive_address = 0x08000900u, output_address = 0x08001000u;
void Request(wiiu::WiiUMemory& memory, std::span<const std::uint32_t> words) {
    for (std::size_t i = 0; i < words.size(); ++i) memory.Write32(ipc + static_cast<std::uint32_t>(i * 4), words[i]);
}
std::array<std::uint32_t, 13> OpenRequest(wiiu::WiiUMemory& memory, const std::string& path) {
    std::vector<std::uint8_t> bytes(path.begin(), path.end()); bytes.push_back(0);
    memory.LoadImage(path_address, bytes);
    const std::array<std::uint8_t, 1> empty{0}; memory.LoadImage(archive_address, empty);
    const auto size = static_cast<std::uint32_t>(bytes.size());
    return {0x08030204u, 0, 3, 1, 1, 3, size, 1, 0, (1u << 14) | (2u << 10) | 2u,
            archive_address, (size << 14) | 2u, path_address};
}
} // namespace

// Platform substitutes; tests use the production reader, services and FCRAM code.
void* MEMAllocFromDefaultHeapEx(std::uint32_t size, std::int32_t alignment) {
    Check(alignment == 0x1000);
    return ::operator new(size, std::align_val_t{0x1000}, std::nothrow);
}
void MEMFreeToDefaultHeap(void* pointer) { ::operator delete(pointer, std::align_val_t{0x1000}); }
void OSReport(const char*, ...) {}
void wiiu::Pica200::WriteMMIO(std::uint32_t, std::uint32_t, std::uint8_t) {
    throw std::logic_error("Unexpected graphics access in RomFS test");
}
std::int32_t VPADRead(VPADChan, VPADStatus*, std::uint32_t, VPADReadError* error) {
    *error = VPAD_READ_NO_SAMPLES; return 0;
}
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution, VPADTouchData*, const VPADTouchData*) {}

int main() {
    const auto fixture = MakeImage();
    wiiu::RomFSReader reader;
    auto source = fixture.image;
    Check(reader.Mount(source));
    source[fixture.data_offset] = 'X';
    wiiu::RomFSFileHandle file;
    Check(reader.OpenFile(std::string{"romfs:/test.txt"}, file));
    std::array<char, 64> output{};
    const auto count = reader.ReadFile(file, 0, output.data(), output.size());
    Check(std::string(output.data(), count) == "3DS2U_ROMFS_OK");
    Check(file.GetSize() == count);
    Check(reader.ReadFile(file, 0, nullptr, 0) == 0);
    Check(reader.ReadFile(file, std::numeric_limits<std::uint64_t>::max(), output.data(), 64) == 0);
    Check(reader.ReadFile(file, 6, output.data(), 5) == 5 && std::string(output.data(), 5) == "ROMFS");
    Reject([&] { (void)reader.ReadFile(file, 0, nullptr, 1); });
    Reject([&] { (void)reader.ReadFile({}, 0, output.data(), 1); });
    Check(reader.OpenFile(std::string{"romfs:/textures/player.bin"}, file));
    Check(reader.ReadFile(file, 0, output.data(), 64) == 6 && std::string(output.data(), 6) == "PIXELS");
    Check(reader.OpenFile(std::string{"/caf\xC3\xA9.bin"}, file));
    Check(reader.ReadFile(file, 0, output.data(), 64) == 7 && std::string(output.data(), 7) == "UNICODE");
    const auto stale = file;
    Check(reader.Mount(fixture.image));
    Reject([&] { (void)reader.ReadFile(stale, 0, output.data(), 1); });
    for (const std::string path : {"romfs:/missing", "romfs:/../test.txt", "romfs://test.txt", "romfs:/", "test.txt", "romfs:/test.txt/"})
        Check(!reader.OpenFile(path, file));
    Check(!reader.OpenFile(std::string{"/\xC0\xAF"}, file));
    Check(!reader.OpenFile(std::u16string_view{u"/\xD800"}, file));
    std::vector<std::uint8_t> prefixed(0x1000, 0xAB);
    prefixed.insert(prefixed.end(), fixture.image.begin(), fixture.image.end());
    Check(reader.Mount(prefixed, 0x1000));

    const auto malformed = [&](auto corrupt) {
        auto bytes = fixture.image; corrupt(bytes);
        wiiu::RomFSReader invalid;
        Check(!invalid.Mount(bytes) && !invalid.GetLastError().empty());
    };
    malformed([](auto& b) { b.resize(16); });
    malformed([](auto& b) { Put32(b, 0, 0x28000000u); });
    malformed([](auto& b) { Put32(b, 8, 0xFFFFFFFCu); });
    malformed([&](auto& b) { Put32(b, fixture.dir_offset + 8, 0); });
    malformed([&](auto& b) { Put32(b, fixture.file_offset + 4, 0); });
    malformed([&](auto& b) { Put64(b, fixture.file_offset + 8, UINT64_MAX); });
    malformed([&](auto& b) { Put32(b, fixture.file_offset + 28, 3); });
    malformed([](auto& b) { Put32(b, 0x28, 1); });

    using Services = wiiu::hle::HLEServiceManager;
    wiiu::WiiUMemory memory;
    Services services;
    services.AttachRomFS(reader);
    Request(memory, std::array{0x08010002u, 0x20u, 0u});
    Check(services.HandleFSUserIPC(memory, ipc) == 0 && memory.Read32(ipc) == 0x08010040u);
    auto request = OpenRequest(memory, "romfs:/test.txt"); Request(memory, request);
    Check(services.HandleFSUserIPC(memory, ipc) == 0);
    const auto id = memory.Read32(ipc + 12);
    Check(id != 0 && memory.Read32(ipc) == 0x08030042u && memory.Read32(ipc + 8) == 0x10u);
    std::array<std::uint8_t, 64> guard{}; guard.fill(0xCD); memory.LoadImage(output_address, guard);
    Request(memory, std::array{0x080200C2u, 0u, 0u, 64u, (64u << 4) | 0xCu, output_address});
    Check(services.HandleFSFileIPC(memory, id, ipc) == 0 && memory.Read32(ipc + 8) == 14);
    const auto* result = static_cast<const char*>(memory.GetPointer(output_address, 64));
    Check(std::string(result, 14) == "3DS2U_ROMFS_OK" && static_cast<unsigned char>(result[14]) == 0xCD);
    const auto* reply = static_cast<const std::uint8_t*>(memory.GetPointer(ipc, 4));
    Check(reply[0] == 0x82 && reply[1] == 0 && reply[2] == 2 && reply[3] == 8);
    std::uint32_t bytes_read = 99;
    Check(services.FSFILE_Read(memory, id, UINT64_MAX, output_address, 64, bytes_read) == 0 && bytes_read == 0);
    Check(services.FSFILE_Read(memory, id, 0, 0x1EC00000u, 64, bytes_read) == Services::kFSInvalidArgument);
    Request(memory, std::array{0x08080000u, 0u});
    Check(services.HandleFSFileIPC(memory, id, ipc) == 0);
    Check(services.FSFILE_Close(id) == Services::kFSInvalidHandle);
    Check(services.FSFILE_Read(memory, id, 0, output_address, 1, bytes_read) == Services::kFSInvalidHandle && bytes_read == 0);

    request = OpenRequest(memory, "/missing"); std::uint32_t handle = 99;
    Check(services.FSUSER_OpenFileDirectly(memory, request, handle) == Services::kFSNotFound && handle == 0);
    request = OpenRequest(memory, "/test.txt"); request[7] = 2;
    Check(services.FSUSER_OpenFileDirectly(memory, request, handle) == Services::kFSNotSupported);
    request[7] = 1; request[11] ^= 1u;
    Check(services.FSUSER_OpenFileDirectly(memory, request, handle) == Services::kFSInvalidArgument);
    request = OpenRequest(memory, "/big.bin");
    Check(services.FSUSER_OpenFileDirectly(memory, request, handle) == 0);
    Check(services.FSFILE_Read(memory, handle, 0, output_address, 7000, bytes_read) == 0 && bytes_read == 6000);
    const auto* large = static_cast<const std::uint8_t*>(memory.GetPointer(output_address, 6000));
    for (unsigned i = 0; i < 6000; ++i) Check(large[i] == i % 127);
    Check(reader.Mount(fixture.image));
    Check(services.FSFILE_Read(memory, handle, 0, output_address, 1, bytes_read) == Services::kFSInvalidHandle);
    services.AttachRomFS(reader);
    // UTF-16 IPC file paths are fetched bytewise, not through native PPC casts.
    request = OpenRequest(memory, "/test.txt");
    const std::u16string unicode_path = u"/caf\u00E9.bin";
    std::vector<std::uint8_t> path_bytes((unicode_path.size() + 1) * 2);
    for (std::size_t i = 0; i < unicode_path.size(); ++i) {
        path_bytes[i * 2] = static_cast<std::uint8_t>(unicode_path[i]);
        path_bytes[i * 2 + 1] = static_cast<std::uint8_t>(unicode_path[i] >> 8);
    }
    memory.LoadImage(path_address, path_bytes);
    request[5] = 4; request[6] = static_cast<std::uint32_t>(path_bytes.size());
    request[11] = (request[6] << 14) | 2u;
    Check(services.FSUSER_OpenFileDirectly(memory, request, handle) == 0);
    Check(services.FSFILE_Read(memory, handle, 0, output_address, 64, bytes_read) == 0 && bytes_read == 7);
    Check(std::memcmp(memory.GetPointer(output_address, 7), "UNICODE", 7) == 0);
    Check(services.FSFILE_Close(handle) == 0);
    request = OpenRequest(memory, "/test.txt");
    std::vector<std::uint32_t> handles;
    for (unsigned i = 0; i < 256; ++i) {
        Check(services.FSUSER_OpenFileDirectly(memory, request, handle) == 0);
        handles.push_back(handle);
    }
    Check(services.FSUSER_OpenFileDirectly(memory, request, handle) == Services::kFSOutOfResources && handle == 0);
    for (auto opened : handles) Check(services.FSFILE_Close(opened) == 0);
    Check(services.FSUSER_OpenFileDirectly(memory, request, handle) == 0 && handle > handles.back());
    Check(services.FSFILE_Close(handle) == 0);
    Request(memory, std::array{0x080200C2u, 0u, 0u, 4u, 0u, output_address});
    Check(services.HandleFSFileIPC(memory, id, ipc) == Services::kFSInvalidArgument);
    Request(memory, std::array{0x09000000u, 0u});
    Check(services.HandleFSUserIPC(memory, ipc) == Services::kFSNotSupported);
    Reject([&] { (void)reader.ReadFile({}, 0, nullptr, 0); });
    Check(services.HandleFSUserIPC(memory, 0xFFFFFFFCu) == Services::kFSInvalidArgument);
    std::cout << "RomFS header, nested/Unicode paths, bounds, handles and FS IPC tests passed.\n";
}
