#include "hle_services.hpp"
#include "wiiu_memory.hpp"
#include "audio_driver.hpp"

#include <coreinit/debug.h>

#include <cinttypes>
#include <stdexcept>
#include <array>
#include <algorithm>
#include <limits>
#include <new>

namespace wiiu::hle {

std::span<const std::uint8_t> HLEServiceManager::RegisteredROHashes(std::uint32_t address) const noexcept {
    const auto it = ro_hashes_.find(address);
    return it == ro_hashes_.end() ? std::span<const std::uint8_t>{} : it->second;
}

std::uint32_t HLEServiceManager::RO_RegisterModule(WiiUMemory& memory,
    std::uint32_t address, std::uint32_t size) {
    try {
        if (size < 0x80 || size > 4u * 1024u * 1024u || ro_hashes_.size() >= 32)
            return kROInvalidArgument;
        (void)memory.GetPointer(address, size);
        std::uint32_t offset{}, count{};
        if (memory.Read32(address) == 0x30525243u) {
            if (size < 0x360 || memory.Read32(address + 8) != 0 || memory.Read32(address + 12) != 0 ||
                memory.Read32(address + 0x344) != size) return kROInvalidArgument;
            offset = memory.Read32(address + 0x350);
            count = memory.Read32(address + 0x354);
            if (offset < 0x360 || offset > size || count == 0 || count > (size - offset) / 32u)
                return kROInvalidArgument;
        } else if (size >= 0x138 && memory.Read32(address + 0x80) == 0x304F5243u) {
            offset = 0; count = 4;
        } else return kROInvalidArgument;
        const auto* first = static_cast<const std::uint8_t*>(memory.GetPointer(address + offset, count * 32u));
        std::vector<std::uint8_t> hashes(first, first + count * 32u);
        ro_hashes_.insert_or_assign(address, std::move(hashes));
        OSReport("[3DS2U RO] Registered hash metadata (verification deferred)\n");
        return 0;
    } catch (const std::bad_alloc&) { return kROOutOfResources; }
      catch (const std::exception&) { return kROInvalidArgument; }
}

std::uint32_t HLEServiceManager::RO_LoadModule(WiiUMemory& memory,
    std::uint32_t source, std::uint32_t size, std::uint32_t destination, CROModule& out) {
    try {
        if (size < 0x138 || size > WiiUMemory::kFCRAMSize) return kROInvalidArgument;
        const auto* bytes = static_cast<const std::uint8_t*>(memory.GetPointer(source, size));
        if (!cro_loader_.LoadModule(memory, bytes, size, destination, out)) {
            OSReport("[3DS2U RO] Load failed: %s\n", cro_loader_.LastError().c_str());
            return kROLoadFailed;
        }
        OSReport("[3DS2U RO] Loaded %s at %08" PRIX32 "\n", out.name.c_str(), destination);
        return 0;
    } catch (const std::exception&) { return kROInvalidArgument; }
}

std::uint32_t HLEServiceManager::RO_UnloadModule(WiiUMemory& memory, std::uint32_t address) {
    if (!cro_loader_.UnloadModule(memory, address)) return kROLoadFailed;
    OSReport("[3DS2U RO] Unloaded module at %08" PRIX32 "\n", address);
    return 0;
}

namespace {
std::array<std::uint32_t, 13> ReadIPC(WiiUMemory& memory, std::uint32_t address, unsigned words) {
    if ((address & 3u) != 0) throw std::invalid_argument("Unaligned FS IPC address");
    (void)memory.GetPointer(address, static_cast<std::size_t>(words) * 4u);
    std::array<std::uint32_t, 13> request{};
    for (unsigned i = 0; i < words; ++i) request[i] = memory.Read32(address + i * 4u);
    return request;
}
void ReplyIPC(WiiUMemory& memory, std::uint32_t address, std::span<const std::uint32_t> reply) {
    (void)memory.GetPointer(address, reply.size_bytes());
    for (std::size_t i = 0; i < reply.size(); ++i)
        memory.Write32(address + static_cast<std::uint32_t>(i * 4u), reply[i]);
}
void ValidateAudioAccess(std::uint32_t address, unsigned width) {
    if ((width != 2 && width != 4) || address < HLEServiceManager::kAudioMMIOBase ||
        address >= HLEServiceManager::kAudioMMIOEnd ||
        width > HLEServiceManager::kAudioMMIOEnd - address ||
        (address & (width - 1u)) != 0) {
        throw std::out_of_range("Invalid audio MMIO access");
    }
}
} // namespace

void HLEServiceManager::AttachRomFS(RomFSReader& reader) noexcept {
    fs_handles_.clear();
    romfs_ = &reader;
}

std::uint32_t HLEServiceManager::FSUSER_Initialize() {
    fs_initialized_ = true;
    OSReport("[3DS2U HLE] FSUSER_Initialize: success\n");
    return 0;
}

std::uint32_t HLEServiceManager::FSUSER_OpenFileDirectly(WiiUMemory& memory,
    std::span<const std::uint32_t> request, std::uint32_t& out_handle) {
    out_handle = 0;
    if (!fs_initialized_ || romfs_ == nullptr) return kFSNotMounted;
    if (request.size() < 13 || request[0] != 0x08030204u || request[1] != 0)
        return kFSInvalidArgument;
    // Current-process RomFS archive, empty archive path, read-only open.
    if (request[2] != 3 || request[3] != 1 || request[7] != 1 || request[8] != 0)
        return kFSNotSupported;
    const auto size = request[6];
    if (request[4] != 1 || size < 2 || size > 4096 ||
        request[9] != ((1u << 14) | (2u << 10) | 2u) || request[11] != ((size << 14) | 2u))
        return kFSInvalidArgument;
    if (request[5] != 3 && request[5] != 4) return kFSNotSupported;
    try {
        const auto* archive = static_cast<const std::uint8_t*>(memory.GetPointer(request[10], 1));
        if (*archive != 0) return kFSInvalidArgument;
        const auto* bytes = static_cast<const std::uint8_t*>(memory.GetPointer(request[12], size));
        RomFSFileHandle handle;
        bool opened = false;
        if (request[5] == 3) {
            if (bytes[size - 1] != 0 || std::find(bytes, bytes + size - 1, 0) != bytes + size - 1)
                return kFSInvalidArgument;
            opened = romfs_->OpenFile(std::string{reinterpret_cast<const char*>(bytes), size - 1u}, handle);
        } else {
            if ((size & 1u) != 0 || bytes[size - 1] != 0 || bytes[size - 2] != 0)
                return kFSInvalidArgument;
            std::u16string path;
            for (std::uint32_t i = 0; i < size - 2u; i += 2) {
                const auto c = static_cast<char16_t>(bytes[i] | (std::uint32_t{bytes[i + 1]} << 8));
                if (c == 0) return kFSInvalidArgument;
                path.push_back(c);
            }
            opened = romfs_->OpenFile(std::u16string_view{path}, handle);
        }
        if (!opened) return kFSNotFound;
        if (fs_handles_.size() >= 256 || next_fs_handle_ == std::numeric_limits<std::uint32_t>::max())
            return kFSOutOfResources;
        const auto id = next_fs_handle_;
        fs_handles_.emplace(id, handle);
        ++next_fs_handle_;
        out_handle = id;
        return 0;
    } catch (const std::bad_alloc&) { return kFSOutOfResources; }
    catch (const std::exception&) { return kFSInvalidArgument; }
}

std::uint32_t HLEServiceManager::FSFILE_Read(WiiUMemory& memory, std::uint32_t id,
    std::uint64_t offset, std::uint32_t destination, std::uint32_t size, std::uint32_t& bytes_read) {
    bytes_read = 0;
    const auto it = fs_handles_.find(id);
    if (romfs_ == nullptr || it == fs_handles_.end()) return kFSInvalidHandle;
    try {
        // Validate generation even for zero-byte/EOF reads after remounting.
        (void)romfs_->ReadFile(it->second, offset, nullptr, 0);
    } catch (const std::exception&) { return kFSInvalidHandle; }
    try {
        if (size != 0) (void)memory.GetPointer(destination, size);
        std::array<std::uint8_t, 4096> chunk{};
        while (bytes_read < size) {
            const auto count = romfs_->ReadFile(it->second, offset + bytes_read, chunk.data(),
                std::min<std::size_t>(chunk.size(), size - bytes_read));
            if (count == 0) break;
            memory.LoadImage(destination + bytes_read, std::span{chunk}.first(count));
            bytes_read += static_cast<std::uint32_t>(count);
        }
        return 0;
    } catch (const std::exception&) { bytes_read = 0; return kFSInvalidArgument; }
}

std::uint32_t HLEServiceManager::FSFILE_Close(std::uint32_t id) {
    return fs_handles_.erase(id) != 0 ? 0u : kFSInvalidHandle;
}

std::uint32_t HLEServiceManager::HandleFSUserIPC(WiiUMemory& memory, std::uint32_t address) {
    try {
        const auto command = ReadIPC(memory, address, 2)[0];
        if (command == 0x08010002u) {
            const auto request = ReadIPC(memory, address, 3);
            const auto result = request[1] == 0x20u ? FSUSER_Initialize() : kFSInvalidArgument;
            ReplyIPC(memory, address, std::array{0x08010040u, result});
            return result;
        }
        if (command == 0x08030204u) {
            const auto request = ReadIPC(memory, address, 13);
            std::uint32_t handle = 0;
            const auto result = FSUSER_OpenFileDirectly(memory, request, handle);
            ReplyIPC(memory, address, std::array{result == 0 ? 0x08030042u : 0x08030040u,
                                                result, result == 0 ? 0x10u : 0u, handle});
            return result;
        }
        ReplyIPC(memory, address, std::array{(command & 0xFFFF0000u) | 0x40u, kFSNotSupported});
        return kFSNotSupported;
    } catch (const std::bad_alloc&) { return kFSOutOfResources; }
    catch (const std::exception&) { return kFSInvalidArgument; }
}

std::uint32_t HLEServiceManager::HandleFSFileIPC(WiiUMemory& memory, std::uint32_t handle,
                                               std::uint32_t address) {
    try {
        const auto command = ReadIPC(memory, address, 2)[0];
        if (command == 0x080200C2u) {
            const auto request = ReadIPC(memory, address, 6);
            std::uint32_t bytes_read = 0;
            const auto descriptor = (request[3] << 4) | 0xCu;
            const auto result = request[3] <= 0x0FFFFFFFu && request[4] == descriptor ?
                FSFILE_Read(memory, handle, request[1] | (std::uint64_t{request[2]} << 32),
                            request[5], request[3], bytes_read) : kFSInvalidArgument;
            ReplyIPC(memory, address, std::array{0x08020082u, result, bytes_read, request[4], request[5]});
            return result;
        }
        if (command == 0x08080000u) {
            const auto result = FSFILE_Close(handle);
            ReplyIPC(memory, address, std::array{0x08080040u, result});
            return result;
        }
        ReplyIPC(memory, address, std::array{(command & 0xFFFF0000u) | 0x40u, kFSNotSupported});
        return kFSNotSupported;
    } catch (const std::exception&) { return kFSInvalidArgument; }
}

std::uint32_t HLEServiceManager::csndInit() {
    if (audio_driver_ != nullptr) return audio_driver_->Initialize() ? 0u : 0xD9030001u;
    OSReport("[OSConsole] [3DS2U HLE] csndInit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::csndExit() {
    if (audio_driver_ != nullptr) { audio_driver_->StopAll(); OSReport("[3DS2U Audio] CSND channels stopped\n"); return 0; }
    OSReport("[OSConsole] [3DS2U HLE] csndExit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::dspInit() {
    if (audio_driver_ != nullptr) return audio_driver_->Initialize() ? 0u : 0xD9030001u;
    OSReport("[OSConsole] [3DS2U HLE] dspInit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::dspExit() {
    if (audio_driver_ != nullptr) { audio_driver_->StopAll(); OSReport("[3DS2U Audio] DSP channels stopped\n"); return 0; }
    OSReport("[OSConsole] [3DS2U HLE] dspExit: stub success\n");
    return 0;
}

std::uint32_t HLEServiceManager::csndPlaySound(std::uint32_t channel, std::uint32_t address,
    std::uint32_t rate, std::uint16_t format, std::uint32_t size, bool loop) {
    if (audio_driver_ == nullptr) return 0xD9030002u;
    try { audio_driver_->PlayVoice(channel, address, rate, format, size, loop); return 0; }
    catch (const std::exception& error) {
        OSReport("[3DS2U Audio] Playback rejected: %s\n", error.what()); return 0xD9030001u;
    }
}
std::uint32_t HLEServiceManager::DSP_PlayBuffer(std::uint32_t channel, std::uint32_t address,
    std::uint32_t rate, std::uint16_t format, std::uint32_t size, bool loop) {
    return csndPlaySound(channel, address, rate, format, size, loop);
}
bool HLEServiceManager::csndIsChannelPlaying(std::uint32_t channel) const noexcept {
    return audio_driver_ != nullptr && channel < AudioDriver::kVoiceCount && audio_driver_->IsPlaying(channel);
}
bool HLEServiceManager::dspIsChannelReady(std::uint32_t channel) const noexcept {
    return audio_driver_ == nullptr || (channel < AudioDriver::kVoiceCount && !audio_driver_->IsPlaying(channel));
}

std::uint32_t HLEServiceManager::ReadAudioMMIO(std::uint32_t address, unsigned width, AudioDriver* driver) {
    ValidateAudioAccess(address, width);
    std::uint32_t active_value{};
    if (driver != nullptr && driver->ReadMMIO(address, width, active_value)) return active_value;
    const auto word_address = address & ~3u;
    const std::uint32_t value =
        (word_address == kAudioReadyRegister || word_address == kAudioSemaphoreRegister) ? 1u : 0u;
    // MMIO returns a numeric guest value; select halfwords in guest LE order.
    return (value >> ((address & 3u) * 8u)) & (width == 2 ? 0xFFFFu : 0xFFFFFFFFu);
}

void HLEServiceManager::WriteAudioMMIO(std::uint32_t address, std::uint32_t value, unsigned width, AudioDriver* driver) {
    ValidateAudioAccess(address, width);
    if (driver != nullptr && driver->WriteMMIO(address, value, width)) return;
    OSReport("[OSConsole] [3DS2U HLE] audio MMIO Write%u 0x%08" PRIX32
             " = 0x%08" PRIX32 ": stub no-op\n", width * 8u, address, value);
}

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
