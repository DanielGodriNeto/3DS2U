#pragma once
#include <array>
#include <cstdint>

namespace wiiu::gx2::shaders {

// Minimal Latte CF programs, serialized LE before GPU use. No SDK shader binary
// or runtime GLSL compiler is required. R1=position, R2=color, R3=clip position.
// Four DOT4 groups compute R3 = row-major mat4(c0..c3) * R1.
inline constexpr auto vertex = [] {
    std::array<std::uint32_t, 40> words{};
    words[0] = 0; words[1] = 0x09800000u; // CALL_FS, no barrier
    words[2] = 4; words[3] = 0xA03C0000u; // ALU: address 4, 16 instructions, barrier
    words[4] = 0xC001A03Cu; words[5] = 0x94000688u; // POS0 = R3
    words[6] = 0xC0014000u; words[7] = 0x14200688u; // PARAM0 = R2, end program
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto offset = 8u + (row * 4u + lane) * 2u;
            words[offset] = 1u | (lane << 10) | ((256u + row) << 13) |
                            (lane << 23) | (lane == 3 ? 0x80000000u : 0u);
            // Reduction uses all four lanes; only the row's destination lane writes.
            words[offset + 1] = (0x50u << 7) | (3u << 21) | (lane << 29) |
                                (lane == row ? 0x10u : 0u);
        }
    }
    return words;
}();
// Legacy register-mode reference; runtime selects the block-mode TEV programs below.
inline constexpr std::array<std::uint32_t, 2> pixel{
    0xC0000000u, 0x94200688u, // EXP_DONE PIX0, R0.xyzw, end program
};

// Textured input R3=UV, so the transformed position moves to R4.
inline constexpr auto textured_vertex = [] {
    std::array<std::uint32_t, 42> words{};
    for (unsigned i = 0; i < 8; ++i) words[i] = vertex[i];
    words[2] = 5; // ALU follows five CF instructions.
    words[4] = 0xC002203Cu; // POS0 = R4
    words[7] = 0x13800688u; // EXP PARAM0 color; PARAM1 completes parameter exports.
    words[8] = 0xC001C001u; words[9] = 0x14200B08u; // PARAM1 = R3.xy01, end
    for (unsigned i = 8; i < 40; ++i) {
        words[i + 2] = vertex[i];
        if ((i & 1u) != 0) words[i + 2] = (words[i + 2] & ~(0x7Fu << 21)) | (4u << 21);
    }
    return words;
}();

// PS inputs R0=color, R1=UV. Sample unit 0 into R2, multiply, export R0.
inline constexpr auto textured_pixel = [] {
    std::array<std::uint32_t, 20> words{};
    words[0] = 4; words[1] = 0x80800000u; // TEX clause, one sample
    words[2] = 6; words[3] = 0xA00C0000u; // ALU clause, four multiplies
    words[4] = 0xC0000000u; words[5] = 0x94200688u; // PIX0 = R0, end
    words[8] = 0x00010010u; // SAMPLE resource 0, coordinates R1
    words[9] = 0xF00D1002u; // R2.xyzw, normalized coordinates
    words[10] = 0xB0800000u; // sampler 0, source xy01
    for (unsigned lane = 0; lane < 4; ++lane) {
        words[12 + lane * 2] = (lane << 10) | (2u << 13) | (lane << 23) |
                               (lane == 3 ? 0x80000000u : 0u);
        words[13 + lane * 2] = (2u << 7) | 0x10u | (lane << 29); // MUL_IEEE R0, R0, R2
    }
    return words;
}();

// Uniform-block variants keep the existing matrix math, using VS binding 1.
template<std::size_t N>
constexpr auto BlockVertex(std::array<std::uint32_t,N> words,unsigned first_alu) {
    words[2]|=(1u<<22)|(1u<<30); // KCACHE bank 1, lock one 16-vec4 line.
    for(unsigned i=0;i<16;++i) {
        auto& low=words[first_alu+i*2];
        low=(low&~(0x1FFu<<13))|((128u+i/4u)<<13);
    }
    return words;
}
inline constexpr auto block_vertex=BlockVertex(vertex,8);
inline constexpr auto textured_block_vertex=BlockVertex(textured_vertex,10);

struct TEVProgram {
    std::array<std::uint32_t,2048> words{};
    unsigned word_count{};
};
namespace tev_isa {
struct Builder {
    TEVProgram program;
    unsigned cf{}, body=64, start{}, cache{};
    constexpr void Begin(unsigned cache_start) {
        cache=cache_start; start=body;
        program.words[cf*2]=(body/2)|(2u<<30); // PS block 0, two cache lines.
        program.words[cf*2+1]=0xA0000000u|((cache_start/16u)<<2);
    }
    constexpr unsigned K(unsigned index) const { return 128u+index-cache; }
    constexpr void End() {
        program.words[cf*2+1]|=((body-start)/2u-1u)<<18; ++cf;
    }
    constexpr void Op(unsigned opcode,unsigned dst,unsigned lane,unsigned a,unsigned ac,
                      unsigned b,unsigned bc,bool last,bool clamp=false) {
        program.words[body++]=a|(ac<<10)|(b<<13)|(bc<<23)|(last ? 0x80000000u:0u);
        program.words[body++]=(opcode<<7)|0x10u|(dst<<21)|(lane<<29)|(clamp ? 0x80000000u:0u);
    }
    constexpr void Vector(unsigned op,unsigned dst,unsigned a,unsigned b,unsigned bc=4,bool clamp=false) {
        for(unsigned c=0;c<4;++c) Op(op,dst,c,a,c,b,bc==4 ? c:bc,c==3,clamp);
    }
    constexpr void Select(unsigned stage,unsigned operand,bool alpha) {
        const auto weights=K(stage*11+operand*4+(alpha ? 1u:0u));
        const auto constant=K(stage*11+8);
        const auto dst=alpha ? 7u:5u;
        const auto first=alpha ? 3u:0u;
        for(unsigned c=first;c<4;++c) Op(2,dst,c,0,c,weights,0,c==3);
        for(unsigned term=1;term<4;++term) {
            const auto src=term==1 ? 2u : term==2 ? constant : 3u;
            for(unsigned c=first;c<4;++c) Op(2,6,c,src,c,weights,term,c==3);
            for(unsigned c=first;c<4;++c) Op(0,dst,c,dst,c,6,c,c==3);
        }
    }
    constexpr void Operand(unsigned stage,unsigned operand) {
        Begin((stage*11u/16u)*16u);
        Select(stage,operand,false); Select(stage,operand,true);
        const auto rgb=K(stage*11+operand*4+2), alpha=rgb+1;
        for(unsigned c=0;c<4;++c) {
            if(c<3) Op(2,6,c,5,c,rgb,0,false);
            else Op(0x19,6,c,7,3,248,0,true);
        }
        for(unsigned c=0;c<3;++c) Op(2,7,c,5,3,rgb,1,c==2);
        for(unsigned c=0;c<4;++c) Op(0,6,c,6,c,c==3 ? 248u:7u,c==3 ? 0u:c,c==3);
        for(unsigned c=0;c<4;++c) Op(2,6,c,6,c,c==3 ? alpha:rgb,c==3 ? 0u:2u,c==3);
        for(unsigned c=0;c<4;++c) Op(0,operand==0 ? 4u:8u,c,6,c,c==3 ? alpha:rgb,c==3 ? 1u:3u,c==3);
        End();
    }
    constexpr void Combine(unsigned stage) {
        Begin((stage*11u/16u)*16u);
        const auto rgb=K(stage*11+9), alpha=rgb+1;
        for(unsigned c=0;c<4;++c) Op(2,9,c,4,c,c==3 ? alpha:rgb,0,c==3);
        for(unsigned c=0;c<4;++c) Op(2,10,c,8,c,c==3 ? alpha:rgb,1,c==3);
        Vector(0,9,9,10); Vector(2,10,4,8);
        for(unsigned c=0;c<4;++c) Op(2,10,c,10,c,c==3 ? alpha:rgb,2,c==3);
        Vector(0,3,9,10,4,true); End();
    }
};
constexpr TEVProgram Build(bool textured) {
    Builder b{};
    if(textured) {
        b.program.words[b.cf*2]=b.body/2; b.program.words[b.cf*2+1]=0x80800000u; ++b.cf;
        b.program.words[b.body++]=0x00010010u;
        b.program.words[b.body++]=0xF00D1002u;
        b.program.words[b.body++]=0xB0800000u; b.program.words[b.body++]=0;
    }
    b.Begin(0);
    if(!textured) for(unsigned c=0;c<4;++c) b.Op(0x19,2,c,249,0,248,0,c==3);
    b.Vector(0x19,3,0,248,0); b.End();
    for(unsigned stage=0;stage<6;++stage) {
        b.Operand(stage,0); b.Operand(stage,1); b.Combine(stage);
    }
    b.program.words[b.cf*2]=0xC0018000u; // EXP_DONE PIX0 = R3.xyzw.
    b.program.words[b.cf*2+1]=0x94200688u;
    b.program.word_count=b.body;
    return b.program;
}
} // namespace tev_isa
// Constant evaluation produces one fixed machine-code program per input layout.
inline constexpr auto tev_pixel=tev_isa::Build(false);
inline constexpr auto textured_tev_pixel=tev_isa::Build(true);
static_assert(tev_pixel.word_count<tev_pixel.words.size());

} // namespace wiiu::gx2::shaders
