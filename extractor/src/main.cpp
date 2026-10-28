#include "three_dsx.hpp"
#include "romfs_reader.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <array>

namespace {
std::vector<std::uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if (!in) throw std::runtime_error("Cannot open input file");
    const auto length=in.tellg();
    if (length<0 || length>512ll*1024*1024) throw std::runtime_error("Invalid input length or exceeds 512 MiB");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    in.seekg(0);
    if (!bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if (!in) throw std::runtime_error("Could not read entire input file");
    return bytes;
}
bool Magic(std::span<const std::uint8_t> b,std::size_t at,const char* text) {
    return at<=b.size() && b.size()-at>=4 && std::equal(b.begin()+static_cast<std::ptrdiff_t>(at),b.begin()+static_cast<std::ptrdiff_t>(at+4),text);
}
void Write(const char* path,std::span<const std::uint8_t> bytes) {
    std::ofstream out;
    out.exceptions(std::ios::failbit|std::ios::badbit);
    out.open(path,std::ios::binary|std::ios::trunc);
    if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    out.close();
}
void Package(std::span<const std::uint8_t> bytes) {
    std::ofstream out;
    out.exceptions(std::ios::failbit|std::ios::badbit);
    out.open("extracted_romfs.cpp",std::ios::trunc);
    out<<"// Generated RomFS package; bytes remain in guest little-endian order.\n"
           "#include \"packaged_romfs.hpp\"\nnamespace wiiu {\n";
    if (!bytes.empty()) {
        out<<"namespace { alignas(32) const std::uint8_t image[] = {\n";
        constexpr char hex[]="0123456789ABCDEF";
        for (std::size_t i=0;i<bytes.size();++i) {
            out<<"0x"<<hex[bytes[i]>>4]<<hex[bytes[i]&15]<<',';
            if ((i&15u)==15) out<<'\n';
        }
        out<<"\n}; }\nstd::span<const std::uint8_t> GetPackagedRomFS() noexcept { return image; }\n";
    } else out<<"std::span<const std::uint8_t> GetPackagedRomFS() noexcept { return {}; }\n";
    out<<"} // namespace wiiu\n";
    out.close();
}
} // namespace

int main(int argc,char** argv) {
    if (argc!=2) { std::cerr<<"Usage: extractor <input.3dsx>\n"; return 1; }
    try {
        const auto input=std::filesystem::weakly_canonical(argv[1]);
        for (const auto* output : {"extracted.code","extracted.exheader","extracted.romfs","extracted_romfs.cpp"})
            if (input==std::filesystem::weakly_canonical(output)) throw std::runtime_error("Input aliases an extraction output");
        {
            std::ifstream probe(input,std::ios::binary);
            if (!probe) throw std::runtime_error("Cannot open input file");
            std::array<std::uint8_t,0x104> prefix{};
            probe.read(reinterpret_cast<char*>(prefix.data()),static_cast<std::streamsize>(prefix.size()));
            const auto bytes=std::span{prefix}.first(static_cast<std::size_t>(probe.gcount()));
            if (Magic(bytes,0,"NCCH") || Magic(bytes,0x100,"NCCH")) {
                std::cerr<<"[3DS2U Extractor] WARNING: Commercial NCCH/CXI extraction requires ctrtool and separately decrypted content.\n";
                return 2;
            }
        }
        const auto file=Read(input);
        const auto image=extractor::Extract(file);
        if (!image.romfs.empty()) {
            wiiu::RomFSReader validator;
            if (!validator.Mount(image.romfs)) throw std::runtime_error("Unsupported or invalid Level-3 RomFS: "+validator.GetLastError());
        }
        Write("extracted.code",image.code); Write("extracted.exheader",image.exheader);
        Write("extracted.romfs",image.romfs); Package(image.romfs);
        std::cout<<"[3DS2U Extractor] .text: "<<image.text_size<<", .rodata: "<<image.rodata_size
                 <<", .data: "<<image.data_size<<", BSS: "<<image.bss_size<<" bytes\n"
                 <<"[3DS2U Extractor] Applied "<<image.relocations<<" relocations at guest base 0x08000000\n"
                 <<"[3DS2U Extractor] RomFS: "<<image.romfs.size()<<" bytes\n"
                 <<"[3DS2U Extractor] Wrote extracted.code, extracted.exheader, extracted.romfs and extracted_romfs.cpp\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"[3DS2U Extractor] ERROR: "<<error.what()<<'\n'; return 1;
    }
}
