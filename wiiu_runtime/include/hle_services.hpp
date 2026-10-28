#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include "romfs_reader.hpp"
#include "cro_loader.hpp"
#include "save_manager.hpp"

namespace wiiu { class WiiUMemory; class GSPGPU; class AudioDriver; }

namespace wiiu::hle {

// Partial HLE services; FS routing below is explicit, not a general SVC layer.
class HLEServiceManager {
public:
    // Explicit GX submission helper; the attached GPU service must outlive submissions.
    void AttachGSPGPU(GSPGPU& service) noexcept { gsp_gpu_ = &service; }
    [[nodiscard]] std::uint32_t GSPGPU_SubmitGXCommand(WiiUMemory& memory, std::uint32_t packet_address);
    [[nodiscard]] std::uint32_t srvInit();
    [[nodiscard]] std::uint32_t aptInit();
    [[nodiscard]] std::uint32_t aptMainLoop();
    [[nodiscard]] std::uint32_t aptExit();
    [[nodiscard]] std::uint32_t srvExit();

    [[nodiscard]] std::uint32_t csndInit();
    [[nodiscard]] std::uint32_t csndExit();
    [[nodiscard]] std::uint32_t dspInit();
    [[nodiscard]] std::uint32_t dspExit();
    void AttachAudioDriver(AudioDriver& driver) noexcept { audio_driver_ = &driver; }
    [[nodiscard]] std::uint32_t csndPlaySound(std::uint32_t channel, std::uint32_t address,
        std::uint32_t rate, std::uint16_t format, std::uint32_t size, bool loop);
    [[nodiscard]] std::uint32_t DSP_PlayBuffer(std::uint32_t channel, std::uint32_t address,
        std::uint32_t rate, std::uint16_t format, std::uint32_t size, bool loop);

    // Nonblocking HLE helpers; these are not libctru ABI/IPC implementations.
    [[nodiscard]] static std::uint32_t dspGetSemaphoreState() noexcept { return 1; }
    [[nodiscard]] bool dspIsChannelReady(std::uint32_t channel) const noexcept;
    [[nodiscard]] bool csndIsChannelPlaying(std::uint32_t channel) const noexcept;

    static constexpr std::uint32_t kAudioMMIOBase = 0x1EC10000u;
    static constexpr std::uint32_t kAudioMMIOEnd = 0x1EC18000u;
    // Synthetic MVP readiness/semaphore registers; other registers read zero.
    static constexpr std::uint32_t kAudioReadyRegister = kAudioMMIOBase + 4u;
    static constexpr std::uint32_t kAudioSemaphoreRegister = kAudioMMIOBase + 8u;
    [[nodiscard]] static std::uint32_t ReadAudioMMIO(std::uint32_t address, unsigned width,
                                                   AudioDriver* driver = nullptr);
    static void WriteAudioMMIO(std::uint32_t address, std::uint32_t value, unsigned width,
                               AudioDriver* driver = nullptr);

    // MVP error results, not a complete catalog of Nintendo FS result codes.
    static constexpr std::uint32_t kFSInvalidArgument = 0xD9000001u;
    static constexpr std::uint32_t kFSNotFound = 0xD9000002u;
    static constexpr std::uint32_t kFSInvalidHandle = 0xD9000003u;
    static constexpr std::uint32_t kFSNotMounted = 0xD9000004u;
    static constexpr std::uint32_t kFSNotSupported = 0xD9000005u;
    static constexpr std::uint32_t kFSOutOfResources = 0xD9000006u;
    static constexpr std::uint32_t kFSIOError=0xD9000007u;
    static constexpr std::uint32_t kFSAlreadyExists=0xD9000008u;
    static constexpr std::uint32_t kFSAccessDenied=0xD9000009u;
    // SaveManager carries the current title identity and must outlive FS calls.
    void AttachSaveManager(SaveManager& manager);
    [[nodiscard]] std::uint32_t FSUSER_OpenArchive(std::uint32_t archive_id,std::uint64_t& handle,
                                                 std::uint64_t extdata_id=0);
    [[nodiscard]] std::uint32_t FSUSER_CloseArchive(std::uint64_t handle);
    [[nodiscard]] std::uint32_t FSUSER_FormatThisUserSaveData();
    [[nodiscard]] std::uint32_t FSUSER_CreateFile(std::uint64_t archive,const std::string& path,std::uint64_t size);
    [[nodiscard]] std::uint32_t FSUSER_CreateDirectory(std::uint64_t archive,const std::string& path);
    [[nodiscard]] std::uint32_t FSUSER_OpenFile(std::uint64_t archive,const std::string& path,
                                              std::uint32_t flags,std::uint32_t& handle);
    [[nodiscard]] std::uint32_t FSFILE_Write(WiiUMemory& memory,std::uint32_t handle,
        std::uint64_t offset,std::uint32_t source,std::uint32_t size,std::uint32_t flags,std::uint32_t& written);
    [[nodiscard]] std::uint32_t FSFILE_Flush(std::uint32_t handle);
    // Reader must outlive the manager's FS calls; replacing it closes all handles.
    void AttachRomFS(RomFSReader& reader) noexcept;
    [[nodiscard]] std::uint32_t FSUSER_Initialize();
    [[nodiscard]] std::uint32_t FSUSER_OpenFileDirectly(WiiUMemory& memory,
        std::span<const std::uint32_t> request, std::uint32_t& out_handle);
    [[nodiscard]] std::uint32_t FSFILE_Read(WiiUMemory& memory, std::uint32_t handle,
        std::uint64_t offset, std::uint32_t output_address, std::uint32_t size,
        std::uint32_t& bytes_read);
    [[nodiscard]] std::uint32_t FSFILE_Close(std::uint32_t handle);
    // Command addresses point to LE guest IPC buffers; file handles select sessions.
    [[nodiscard]] std::uint32_t HandleFSUserIPC(WiiUMemory& memory, std::uint32_t command_address);
    [[nodiscard]] std::uint32_t HandleFSFileIPC(WiiUMemory& memory, std::uint32_t handle,
                                              std::uint32_t command_address);
    // Explicit RO HLE calls; service-session IPC and signature verification are deferred.
    static constexpr std::uint32_t kROInvalidArgument = 0xD9010001u;
    static constexpr std::uint32_t kROLoadFailed = 0xD9010002u;
    static constexpr std::uint32_t kROOutOfResources = 0xD9010003u;
    [[nodiscard]] std::uint32_t RO_RegisterModule(WiiUMemory& memory,
        std::uint32_t address, std::uint32_t size);
    [[nodiscard]] std::uint32_t RO_LoadModule(WiiUMemory& memory,
        std::uint32_t source_address, std::uint32_t size, std::uint32_t load_address,
        CROModule& out_module);
    [[nodiscard]] std::uint32_t RO_UnloadModule(WiiUMemory& memory, std::uint32_t load_address);
    [[nodiscard]] CROLoader& GetCROLoader() noexcept { return cro_loader_; }
    [[nodiscard]] std::span<const std::uint8_t> RegisteredROHashes(std::uint32_t address) const noexcept;
private:
    struct SaveArchive { std::uint32_t id; std::uint64_t extdata; };
    struct SaveFile { SaveFileHandle handle; std::uint32_t archive; };
    SaveManager* saves_{};
    std::unordered_map<std::uint64_t,SaveArchive> save_archives_;
    std::unordered_map<std::uint32_t,SaveFile> save_handles_;
    std::uint64_t next_archive_=0x5341564500000001ull;
    AudioDriver* audio_driver_{};
    GSPGPU* gsp_gpu_{};
    CROLoader cro_loader_;
    std::map<std::uint32_t, std::vector<std::uint8_t>> ro_hashes_;
    RomFSReader* romfs_{};
    std::unordered_map<std::uint32_t, RomFSFileHandle> fs_handles_;
    std::uint32_t next_fs_handle_ = 0x40000000u;
    bool fs_initialized_{};
};

} // namespace wiiu::hle
