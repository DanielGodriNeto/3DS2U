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

HLEServiceManager::HLEServiceManager() {
    service_ports_.emplace("fs:USER", ServicePort::FSUser);
    service_ports_.emplace("aoc:u", ServicePort::AocUser);
}

std::uint32_t HLEServiceManager::OpenServicePort(std::string_view name, std::uint32_t& handle) {
    handle = 0;
    if (name.empty() || name.size() > 8) return kFSInvalidArgument;
    try {
        const auto port = service_ports_.find(std::string(name));
        if (port == service_ports_.end()) return kFSNotFound;
        if (service_sessions_.size() >= 64 || next_service_handle_ >= 0x40000000u) return kFSOutOfResources;
        const auto id = next_service_handle_++;
        service_sessions_.emplace(id, port->second);
        handle = id;
        return 0;
    } catch (const std::bad_alloc&) { return kFSOutOfResources; }
}

std::uint32_t HLEServiceManager::CloseServicePort(std::uint32_t handle) {
    return service_sessions_.erase(handle) != 0 ? 0u : kFSInvalidHandle;
}

std::uint32_t HLEServiceManager::HandleServiceIPC(WiiUMemory& memory, std::uint32_t handle,
                                                std::uint32_t address) {
    const auto session = service_sessions_.find(handle);
    if (session == service_sessions_.end()) return kFSInvalidHandle;
    if (session->second == ServicePort::AocUser) return aoc_.HandleIPC(memory, address);
    return HandleFSUserIPC(memory, address);
}

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
        if (size < 0x138 || size > memory.GetSize()) return kROInvalidArgument;
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
std::uint32_t SaveResult(const SaveException& error) {
    using H=HLEServiceManager;
    switch(error.code) {
    case SaveError::InvalidArgument:return H::kFSInvalidArgument;
    case SaveError::NotFound:return H::kFSNotFound;
    case SaveError::AlreadyExists:return H::kFSAlreadyExists;
    case SaveError::AccessDenied:return H::kFSAccessDenied;
    case SaveError::InvalidHandle:return H::kFSInvalidHandle;
    case SaveError::NotSupported:return H::kFSNotSupported;
    case SaveError::OutOfResources:return H::kFSOutOfResources;
    default:return H::kFSIOError;
    }
}
template<class Operation> std::uint32_t SaveCall(Operation operation) {
    try { operation(); return 0; }
    catch(const SaveException& error) { return SaveResult(error); }
    catch(const std::bad_alloc&) { return HLEServiceManager::kFSOutOfResources; }
    catch(const std::exception&) { return HLEServiceManager::kFSIOError; }
}
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
std::uint32_t FSFailureReply(WiiUMemory& memory,std::uint32_t address,std::uint32_t result) noexcept {
    try {
        const auto command=ReadIPC(memory,address,2)[0];
        ReplyIPC(memory,address,std::array{(command&0xFFFF0000u)|0x40u,result});
    } catch(const std::exception&) {}
    return result;
}
std::string GuestText(WiiUMemory& memory,std::uint32_t type,std::uint32_t size,std::uint32_t address) {
    if(size<2 || size>4096 || (type!=3 && type!=4)) throw std::invalid_argument("Invalid FS text path");
    const auto* bytes=static_cast<const std::uint8_t*>(memory.GetPointer(address,size));
    if(type==3) {
        if(bytes[size-1]!=0 || std::find(bytes,bytes+size-1,0)!=bytes+size-1) throw std::invalid_argument("Invalid FS path terminator");
        return {reinterpret_cast<const char*>(bytes),size-1u};
    }
    if((size&1u)!=0 || bytes[size-1]!=0 || bytes[size-2]!=0) throw std::invalid_argument("Invalid UTF16 FS path");
    std::string text;
    for(std::uint32_t i=0;i<size-2;i+=2) {
        std::uint32_t c=bytes[i]|(std::uint32_t{bytes[i+1]}<<8);
        if(c>=0xD800 && c<=0xDBFF) {
            if(i+4>=size) throw std::invalid_argument("Truncated UTF16 surrogate");
            const auto low=bytes[i+2]|(std::uint32_t{bytes[i+3]}<<8);
            if(low<0xDC00 || low>0xDFFF) throw std::invalid_argument("Invalid UTF16 surrogate");
            c=0x10000u+((c-0xD800u)<<10)+(low-0xDC00u); i+=2;
        } else if(c==0 || (c>=0xDC00 && c<=0xDFFF)) throw std::invalid_argument("Invalid UTF16 code point");
        if(c<0x80) text.push_back(static_cast<char>(c));
        else if(c<0x800) { text.push_back(static_cast<char>(0xC0u|(c>>6))); text.push_back(static_cast<char>(0x80u|(c&63u))); }
        else {
            if(c>=0x10000) { text.push_back(static_cast<char>(0xF0u|(c>>18))); text.push_back(static_cast<char>(0x80u|((c>>12)&63u))); }
            else text.push_back(static_cast<char>(0xE0u|(c>>12)));
            text.push_back(static_cast<char>(0x80u|((c>>6)&63u))); text.push_back(static_cast<char>(0x80u|(c&63u)));
        }
    }
    return text;
}
std::uint64_t ArchivePath(WiiUMemory& memory,std::uint32_t id,std::uint32_t type,
                           std::uint32_t size,std::uint32_t address) {
    if(type==1 && size==1 && memory.Read8(address)==0) return 0;
    if(id==6 && type==2 && size==12) {
        (void)memory.GetPointer(address,size);
        if(memory.Read32(address)!=1) throw SaveException(SaveError::NotSupported,"Only SD ExtData is supported");
        return memory.Read32(address+4)|(std::uint64_t{memory.Read32(address+8)}<<32);
    }
    throw SaveException(SaveError::NotSupported,"Unsupported save archive low path");
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

void HLEServiceManager::AttachSaveManager(SaveManager& manager) {
    if(saves_) for(const auto& [id,file]:save_handles_) {
        (void)id; try { saves_->CloseFile(file.handle); } catch(const std::exception&) {}
    }
    save_handles_.clear(); save_archives_.clear(); saves_=&manager;
}
std::uint32_t HLEServiceManager::FSUSER_OpenArchive(std::uint32_t id,std::uint64_t& out,std::uint64_t ext) {
    out=0;
    if(!fs_initialized_ || !saves_) return kFSNotMounted;
    if(save_archives_.size()>=32 || next_archive_==std::numeric_limits<std::uint64_t>::max()) return kFSOutOfResources;
    return SaveCall([&] { saves_->OpenArchive(id,ext); const auto value=next_archive_++;
        save_archives_.emplace(value,SaveArchive{id,ext}); out=value; });
}
std::uint32_t HLEServiceManager::FSUSER_CloseArchive(std::uint64_t handle) {
    return save_archives_.erase(handle)!=0 ? 0u:kFSInvalidHandle;
}
std::uint32_t HLEServiceManager::FSUSER_FormatThisUserSaveData() {
    if(!fs_initialized_ || !saves_) return kFSNotMounted;
    const auto result=SaveCall([&] { saves_->FormatThisUserSaveData(); });
    if(result==0) {
        std::erase_if(save_handles_,[](const auto& item) { return item.second.archive==SaveManager::kSaveData; });
        std::erase_if(save_archives_,[](const auto& item) { return item.second.id==SaveManager::kSaveData; });
    }
    return result;
}
std::uint32_t HLEServiceManager::FSUSER_CreateFile(std::uint64_t archive,const std::string& path,std::uint64_t size) {
    const auto it=save_archives_.find(archive); if(!saves_ || it==save_archives_.end()) return kFSInvalidHandle;
    return SaveCall([&] { saves_->CreateFile(it->second.id,path,size,it->second.extdata); });
}
std::uint32_t HLEServiceManager::FSUSER_CreateDirectory(std::uint64_t archive,const std::string& path) {
    const auto it=save_archives_.find(archive); if(!saves_ || it==save_archives_.end()) return kFSInvalidHandle;
    return SaveCall([&] { saves_->CreateDirectory(it->second.id,path,it->second.extdata); });
}
std::uint32_t HLEServiceManager::FSUSER_OpenFile(std::uint64_t archive,const std::string& path,
    std::uint32_t flags,std::uint32_t& out) {
    out=0; const auto it=save_archives_.find(archive);
    if(!saves_ || it==save_archives_.end()) return kFSInvalidHandle;
    if(fs_handles_.size()+save_handles_.size()>=256 || next_fs_handle_==std::numeric_limits<std::uint32_t>::max())
        return kFSOutOfResources;
    return SaveCall([&] {
        auto handle=saves_->OpenFile(it->second.id,path,flags,it->second.extdata);
        const auto id=next_fs_handle_++;
        try { save_handles_.emplace(id,SaveFile{handle,it->second.id}); }
        catch(...) { saves_->CloseFile(handle); throw; }
        out=id;
    });
}
std::uint32_t HLEServiceManager::FSFILE_Write(WiiUMemory& memory,std::uint32_t id,
    std::uint64_t offset,std::uint32_t source,std::uint32_t size,std::uint32_t flags,std::uint32_t& written) {
    written=0; const auto it=save_handles_.find(id);
    if(!saves_ || it==save_handles_.end()) return fs_handles_.contains(id) ? kFSAccessDenied:kFSInvalidHandle;
    try {
        const void* bytes=size==0 ? nullptr:memory.GetPointer(source,size);
        return SaveCall([&] { written=static_cast<std::uint32_t>(saves_->WriteFile(it->second.handle,offset,bytes,size,flags)); });
    } catch(const std::exception&) { return kFSInvalidArgument; }
}
std::uint32_t HLEServiceManager::FSFILE_Flush(std::uint32_t id) {
    const auto it=save_handles_.find(id);
    if(!saves_ || it==save_handles_.end()) return kFSInvalidHandle;
    return SaveCall([&] { saves_->FlushFile(it->second.handle); });
}

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
    if (!fs_initialized_) return kFSNotMounted;
    if (request.size() < 13 || request[0] != 0x08030204u || request[1] != 0)
        return kFSInvalidArgument;
    if(request[2]==SaveManager::kSaveData || request[2]==SaveManager::kExtData) {
        if(!saves_) return kFSNotMounted;
        if(request[4]>4096 || request[6]>4096 || request[8]!=0 ||
           request[9]!=((request[4]<<14)|(2u<<10)|2u) || request[11]!=((request[6]<<14)|2u)) return kFSInvalidArgument;
        try {
            const auto ext=ArchivePath(memory,request[2],request[3],request[4],request[10]);
            const auto path=GuestText(memory,request[5],request[6],request[12]);
            std::uint64_t archive{};
            const auto opened=FSUSER_OpenArchive(request[2],archive,ext);
            if(opened!=0) return opened;
            const auto result=FSUSER_OpenFile(archive,path,request[7],out_handle);
            (void)FSUSER_CloseArchive(archive); return result;
        } catch(const SaveException& error) { return SaveResult(error); }
        catch(const std::exception&) { return kFSInvalidArgument; }
    }
    if(romfs_==nullptr) return kFSNotMounted;
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
        if (fs_handles_.size()+save_handles_.size() >= 256 || next_fs_handle_ == std::numeric_limits<std::uint32_t>::max())
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
    if(const auto save=save_handles_.find(id); save!=save_handles_.end()) {
        if(!saves_) return kFSInvalidHandle;
        if(size>SaveManager::kMaxFileSize || offset>SaveManager::kMaxFileSize-size) return kFSInvalidArgument;
        try { if(size!=0) (void)memory.GetPointer(destination,size); }
        catch(const std::exception&) { return kFSInvalidArgument; }
        return SaveCall([&] {
            std::array<std::uint8_t,32768> chunk{};
            if(size==0) (void)saves_->ReadFile(save->second.handle,offset,nullptr,0);
            while(bytes_read<size) {
                const auto count=saves_->ReadFile(save->second.handle,offset+bytes_read,chunk.data(),
                    std::min<std::size_t>(chunk.size(),size-bytes_read));
                if(count==0) break;
                memory.LoadImage(destination+bytes_read,std::span{chunk}.first(count));
                bytes_read+=static_cast<std::uint32_t>(count);
            }
        });
    }
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
    if(const auto it=save_handles_.find(id); it!=save_handles_.end()) {
        if(!saves_) return kFSInvalidHandle;
        const auto result=SaveCall([&] { saves_->CloseFile(it->second.handle); });
        save_handles_.erase(it); return result;
    }
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
        if(command==0x080C00C2u) {
            const auto r=ReadIPC(memory,address,6); std::uint64_t handle{};
            std::uint32_t result=kFSInvalidArgument;
            if(r[3]<=4096 && r[4]==((r[3]<<14)|2u)) {
                const auto ext=ArchivePath(memory,r[1],r[2],r[3],r[5]);
                result=FSUSER_OpenArchive(r[1],handle,ext);
            }
            ReplyIPC(memory,address,std::array{0x080C00C0u,result,static_cast<std::uint32_t>(handle),static_cast<std::uint32_t>(handle>>32)});
            return result;
        }
        if(command==0x080E0080u) {
            const auto r=ReadIPC(memory,address,3);
            const auto result=FSUSER_CloseArchive(r[1]|(std::uint64_t{r[2]}<<32));
            ReplyIPC(memory,address,std::array{0x080E0040u,result}); return result;
        }
        if(command==0x080F0180u) {
            const auto r=ReadIPC(memory,address,7);
            // Allocation counts and journal duplication are not modeled by the host archive.
            const auto result=r[6]<=1 ? FSUSER_FormatThisUserSaveData():kFSInvalidArgument;
            ReplyIPC(memory,address,std::array{0x080F0040u,result}); return result;
        }
        if(command==0x08080202u || command==0x08090182u || command==0x080201C2u) {
            const auto r=ReadIPC(memory,address,command==0x08080202u ? 11u:command==0x08090182u ? 9u:10u);
            const bool create=command==0x08080202u, directory=command==0x08090182u;
            const auto descriptor=create ? 9u:directory ? 7u:8u;
            std::uint32_t result=kFSInvalidArgument, handle{};
            if(r[1]==0 && r[5]<=4096 && r[descriptor]==((r[5]<<14)|2u)) {
                const auto path=GuestText(memory,r[4],r[5],r[descriptor+1]);
                const auto archive=r[2]|(std::uint64_t{r[3]}<<32);
                if(create || directory) {
                    if(r[6]!=0) result=kFSNotSupported;
                    else result=create ? FSUSER_CreateFile(archive,path,r[7]|(std::uint64_t{r[8]}<<32)):
                                         FSUSER_CreateDirectory(archive,path);
                } else result=r[7]==0 ? FSUSER_OpenFile(archive,path,r[6],handle):kFSNotSupported;
            }
            if(!create && !directory) ReplyIPC(memory,address,std::array{result==0 ? 0x08020042u:0x08020040u,result,handle ? 0x10u:0u,handle});
            else ReplyIPC(memory,address,std::array{(command&0xFFFF0000u)|0x40u,result});
            return result;
        }
        ReplyIPC(memory, address, std::array{(command & 0xFFFF0000u) | 0x40u, kFSNotSupported});
        return kFSNotSupported;
    } catch (const SaveException& error) { return FSFailureReply(memory,address,SaveResult(error)); }
    catch (const std::bad_alloc&) { return FSFailureReply(memory,address,kFSOutOfResources); }
    catch (const std::exception&) { return FSFailureReply(memory,address,kFSInvalidArgument); }
}

std::uint32_t HLEServiceManager::HandleFSFileIPC(WiiUMemory& memory, std::uint32_t handle,
                                               std::uint32_t address) {
    try {
        const auto command = ReadIPC(memory, address, 2)[0];
        if(command==0x08030102u) {
            const auto r=ReadIPC(memory,address,7); std::uint32_t written{};
            const auto result=r[3]<=0x0FFFFFFFu && r[5]==((r[3]<<4)|0xAu) ?
                FSFILE_Write(memory,handle,r[1]|(std::uint64_t{r[2]}<<32),r[6],r[3],r[4],written):kFSInvalidArgument;
            ReplyIPC(memory,address,std::array{0x08030082u,result,written,r[5],r[6]}); return result;
        }
        if(command==0x08090000u) {
            const auto result=FSFILE_Flush(handle); ReplyIPC(memory,address,std::array{0x08090040u,result}); return result;
        }
        if(command==0x08040000u) {
            std::uint64_t size{}; std::uint32_t result=kFSInvalidHandle;
            if(const auto it=save_handles_.find(handle); saves_ && it!=save_handles_.end())
                result=SaveCall([&] { size=saves_->GetSize(it->second.handle); });
            else if(const auto rom=fs_handles_.find(handle); romfs_ && rom!=fs_handles_.end()) {
                (void)romfs_->ReadFile(rom->second,0,nullptr,0); size=rom->second.GetSize(); result=0;
            }
            ReplyIPC(memory,address,std::array{0x080400C0u,result,static_cast<std::uint32_t>(size),static_cast<std::uint32_t>(size>>32)});
            return result;
        }
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
    } catch (const std::exception&) { return FSFailureReply(memory,address,kFSInvalidArgument); }
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
    service_sessions_.clear();
    OSReport("[3DS2U HLE] srvExit: stub success\n");
    return 0;
}

} // namespace wiiu::hle
