#include "three_dsx.hpp"
#include "binary_loader.hpp"
#include "romfs_reader.hpp"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

namespace {
using U32=std::uint32_t;
void Check(bool value,std::source_location at=std::source_location::current()) {
    if (!value) throw std::runtime_error("Extractor test failed at line "+std::to_string(at.line()));
}
void Put(std::vector<std::uint8_t>& b,U32 at,U32 value) {
    for (unsigned i=0;i<4;++i) b.at(at+i)=static_cast<std::uint8_t>(value>>(8u*i));
}
U32 Word(std::span<const std::uint8_t> b,U32 at) {
    return U32{b[at]}|(U32{b[at+1]}<<8)|(U32{b[at+2]}<<16)|(U32{b[at+3]}<<24);
}
std::vector<std::uint8_t> RomFS() {
    const std::string message="3DS2U_ROMFS_OK";
    std::vector<std::uint8_t> b(0x80+message.size());
    const std::array<U32,10> header{0x28,0x28,4,0x2C,24,0x44,4,0x48,48,0x80};
    for (U32 i=0;i<header.size();++i) Put(b,i*4,header[i]);
    Put(b,0x2C+4,0xFFFFFFFF); Put(b,0x2C+8,0xFFFFFFFF); Put(b,0x2C+16,0xFFFFFFFF);
    Put(b,0x48+4,0xFFFFFFFF); Put(b,0x48+16,static_cast<U32>(message.size()));
    Put(b,0x48+24,0xFFFFFFFF); Put(b,0x48+28,16);
    const std::string name="test.txt";
    for (U32 i=0;i<name.size();++i) b[0x48+32+i*2]=static_cast<std::uint8_t>(name[i]);
    std::memcpy(b.data()+0x80,message.data(),message.size()); return b;
}
std::vector<std::uint8_t> Fixture() {
    std::vector<std::uint8_t> b(104);
    Put(b,0,0x58534433); b[4]=0x2C; b[6]=8;
    Put(b,16,8); Put(b,20,8); Put(b,24,16); Put(b,28,8); Put(b,40,104);
    Put(b,44+8,1); Put(b,44+16,1); Put(b,44+20,1);
    Put(b,68,0xE3A00000); Put(b,72,0xEAFFFFFE);
    Put(b,76,0x2008); Put(b,80,0xAABBCCDD);
    Put(b,84,0x10001000); Put(b,88,1);
    Put(b,92,0x00010000); Put(b,96,0x00010001); Put(b,100,0x00010000);
    const auto romfs=RomFS(); b.insert(b.end(),romfs.begin(),romfs.end()); return b;
}
void Save(const char* path,std::span<const std::uint8_t> b) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc); out.exceptions(std::ios::badbit|std::ios::failbit);
    out.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size())); out.close();
}
} // namespace

int main() try {
    auto file=Fixture(); const auto image=extractor::Extract(file);
    Check(image.code.size()==0x3000 && image.relocations==3 && image.bss_size==8);
    Check(Word(image.code,0x1000)==0x08002008 && Word(image.code,0x2000)==0x7FFFF000);
    Check(Word(image.code,0x2004)==0x08000001 && Word(image.code,8)==0);
    Save("build/extractor_fixture.3dsx",file);
    Save("build/extractor_test.code",image.code); Save("build/extractor_test.exheader",image.exheader);
    arm11::BinaryLoader loader;
    Check(loader.LoadFile("build/extractor_test.code","build/extractor_test.exheader"));
    Check(loader.GetTextBuffer().size()==8 && loader.GetInstructionAddresses().size()==2);
    Check(Word(loader.GetGuestBytes(0x08002008,4),0)==0 && loader.GetBssSize()==8);
    wiiu::RomFSReader reader; Check(reader.Mount(image.romfs)); wiiu::RomFSFileHandle handle;
    Check(reader.OpenFile("romfs:/test.txt",handle)); std::array<char,64> text{};
    const auto read=reader.ReadFile(handle,0,text.data(),text.size());
    Check(std::string(text.data(),read)=="3DS2U_ROMFS_OK");
    for (unsigned fault=0;fault<10;++fault) {
        auto broken=file;
        if (fault==0) broken.resize(31);
        if (fault==1) broken[4]=0x21;
        if (fault==2) broken[6]=4;
        if (fault==3) Put(broken,28,20);
        if (fault==4) Put(broken,16,0xFFFFFFFF);
        if (fault==5) Put(broken,52,0xFFFFFFFF);
        if (fault==6) Put(broken,92,0xFFFF0000);
        if (fault==7) Put(broken,76,0x30000000);
        if (fault==8) Put(broken,40,68);
        if (fault==9) broken.resize(103);
        bool rejected=false; try { (void)extractor::Extract(broken); } catch (const std::exception&) { rejected=true; }
        Check(rejected);
    }
    // An absent filesystem must produce an empty package, never reuse old assets.
    auto no_romfs=file; Put(no_romfs,40,0); no_romfs.resize(104);
    Check(extractor::Extract(no_romfs).romfs.empty()); Save("build/extractor_no_romfs.3dsx",no_romfs);
    std::vector<std::uint8_t> ncch(0x200); Put(ncch,0x100,0x4843434E); Save("build/extractor_ncch.cxi",ncch);
    std::cout<<"3DSX section layout, ABS/REL31 relocation, BSS, RomFS and malformed input tests passed.\n";
} catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
