#include "three_dsx.hpp"
#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace extractor {
namespace {
using U32 = std::uint32_t;
constexpr U32 base = 0x08000000u, capacity = 128u * 1024u * 1024u;
void Range(std::span<const std::uint8_t> b, std::size_t at, std::size_t size) {
    if (at > b.size() || size > b.size() - at) throw std::runtime_error("Truncated 3DSX range");
}
U32 LE16(std::span<const std::uint8_t> b, std::size_t at) {
    Range(b,at,2); return U32{b[at]} | (U32{b[at+1]} << 8);
}
U32 LE32(std::span<const std::uint8_t> b, std::size_t at) {
    return LE16(b,at) | (LE16(b,at+2) << 16);
}
void Put(std::vector<std::uint8_t>& b, U32 at, U32 value) {
    Range(b,at,4);
    for (unsigned i=0;i<4;++i) b[at+i]=static_cast<std::uint8_t>(value >> (8u*i));
}
U32 Page(U32 size) {
    if (size > capacity) throw std::runtime_error("3DSX section exceeds FCRAM");
    return (size+0xFFFu)&~0xFFFu;
}
} // namespace

Image Extract(std::span<const std::uint8_t> file) {
    Range(file,0,0x20);
    if (LE32(file,0)!=0x58534433u) throw std::runtime_error("Expected 3DSX magic");
    const auto header=LE16(file,4), relocation_header=LE16(file,6);
    if (header<0x20 || (header>0x20 && header<0x2C) || relocation_header<8 ||
        relocation_header>64 || (relocation_header&3u)!=0 || LE32(file,8)!=0 || LE32(file,12)!=0)
        throw std::runtime_error("Unsupported 3DSX version, flags or header sizes");
    Range(file,0,header);
    Image image;
    image.text_size=LE32(file,16); image.rodata_size=LE32(file,20);
    const auto data_total=LE32(file,24); image.bss_size=LE32(file,28);
    if (image.text_size==0 || image.bss_size>data_total ||
        ((image.text_size|image.rodata_size|data_total|image.bss_size)&3u)!=0)
        throw std::runtime_error("Invalid 3DSX segment sizes/alignment");
    image.data_size=data_total-image.bss_size;
    const std::array<U32,3> initialized{image.text_size,image.rodata_size,image.data_size};
    const std::array<U32,3> mapped{Page(image.text_size),Page(image.rodata_size),Page(data_total)};
    const std::uint64_t total=std::uint64_t{mapped[0]}+mapped[1]+mapped[2];
    if (total>capacity) throw std::runtime_error("3DSX image exceeds 128 MiB FCRAM");
    const std::array<U32,3> offsets{0,mapped[0],mapped[0]+mapped[1]};
    const auto table_count=relocation_header/4;
    Range(file,header,static_cast<std::size_t>(relocation_header)*3);
    std::size_t cursor=header+static_cast<std::size_t>(relocation_header)*3;
    image.code.resize(static_cast<std::size_t>(offsets[2])+Page(image.data_size),0);
    for (unsigned segment=0;segment<3;++segment) {
        Range(file,cursor,initialized[segment]);
        std::copy_n(file.begin()+static_cast<std::ptrdiff_t>(cursor),initialized[segment],image.code.begin()+offsets[segment]);
        cursor+=initialized[segment];
    }
    std::set<U32> patched;
    for (U32 segment=0;segment<3;++segment) for (U32 kind=0;kind<table_count;++kind) {
        const auto count=LE32(file,header+segment*relocation_header+kind*4u);
        if (kind>=2 && count!=0) throw std::runtime_error("Unsupported 3DSX relocation table kind");
        if (count>(file.size()-cursor)/4) throw std::runtime_error("Truncated 3DSX relocation table");
        U32 word=0;
        for (U32 record=0;record<count;++record) {
            const auto skip=LE16(file,cursor), run=LE16(file,cursor+2); cursor+=4;
            if (word>mapped[segment]/4 || skip>mapped[segment]/4-word)
                throw std::runtime_error("3DSX relocation skip outside segment");
            word+=skip;
            if (run!=0 && (word>initialized[segment]/4 || run>initialized[segment]/4-word))
                throw std::runtime_error("3DSX relocation patch outside initialized segment");
            for (U32 n=0;n<run;++n,++word) {
                const auto place=offsets[segment]+word*4;
                if (!patched.insert(place).second) throw std::runtime_error("Duplicate 3DSX relocation");
                const auto encoded=LE32(image.code,place), subtype=encoded>>28;
                const auto target=encoded&0x0FFFFFFFu;
                if (target>total || (kind==0 && subtype!=0) || (kind==1 && subtype>1))
                    throw std::runtime_error("Invalid 3DSX relocation target/subtype");
                const auto value=kind==0 ? base+target : target-place;
                Put(image.code,place,kind==1 && subtype==1 ? value&0x7FFFFFFFu : value);
                ++image.relocations;
            }
        }
    }
    if (header>=0x2C) {
        const auto smdh=LE32(file,0x20), smdh_size=LE32(file,0x24), romfs=LE32(file,0x28);
        if (smdh_size!=0) {
            if (smdh<cursor) throw std::runtime_error("SMDH overlaps 3DSX executable");
            Range(file,smdh,smdh_size);
        }
        if (romfs!=0) {
            if ((romfs&3u)!=0 || romfs<cursor || romfs>=file.size() ||
                (smdh_size!=0 && romfs<std::uint64_t{smdh}+smdh_size))
                throw std::runtime_error("Invalid appended RomFS offset");
            if (file.size()-romfs>64u*1024u*1024u) throw std::runtime_error("Embedded RomFS exceeds 64 MiB package limit");
            image.romfs.assign(file.begin()+romfs,file.end());
        }
    }
    // Synthetic CodeSetInfo only: not a signed/executable NCCH security header.
    image.exheader.resize(0x400,0);
    for (U32 i=0;i<3;++i) {
        const auto descriptor=0x10u+i*0x10u;
        Put(image.exheader,descriptor,base+offsets[i]);
        Put(image.exheader,descriptor+4,mapped[i]/0x1000u);
        Put(image.exheader,descriptor+8,initialized[i]);
    }
    Put(image.exheader,0x3C,image.bss_size);
    return image;
}
} // namespace extractor
