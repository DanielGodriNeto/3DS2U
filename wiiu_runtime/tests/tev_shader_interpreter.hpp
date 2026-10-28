#pragma once
#include "tev_combiner.hpp"
#include "default_shaders.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace tev_test {
// Executes the actual emitted OP2/CF words with VLIW group read-before-write semantics.
inline wiiu::TEVCombiner::Color Run(const wiiu::gx2::shaders::TEVProgram& shader,
    const wiiu::TEVCombiner::UniformBlock& uniform,wiiu::TEVCombiner::Color primary,
    wiiu::TEVCombiner::Color texture) {
    using Color=wiiu::TEVCombiner::Color;
    std::array<Color,11> registers{}; registers[0]=primary;
    const auto require=[](bool ok) { if(!ok) throw std::runtime_error("Invalid emitted TEV machine code"); };
    for(unsigned cf=0;cf<32;++cf) {
        const auto low=shader.words[cf*2], high=shader.words[cf*2+1];
        if((high&0x30000000u)==0x20000000u) {
            const auto address=low&0x3FFFFFu, count=((high>>18)&127u)+1;
            const auto cache=((high>>2)&255u)*16u;
            require((low>>30)==2 && ((low>>22)&15u)==0 && address*2u+count*2u<=shader.word_count);
            auto pending=registers;
            const auto source=[&](unsigned sel,unsigned channel) {
                if(sel<11) return registers[sel][channel];
                if(sel>=128 && sel<160) {
                    const auto index=cache+sel-128;
                    require(index<66);
                    return uniform.stages[index/11].rows[index%11][channel];
                }
                if(sel==248) return 0.0f;
                if(sel==249) return 1.0f;
                throw std::runtime_error("Unexpected TEV shader source selector");
            };
            bool closed=true;
            for(unsigned i=0;i<count;++i) {
                const auto a=shader.words[(address+i)*2], b=shader.words[(address+i)*2+1];
                const auto op=(b>>7)&0x7FFu, dst=(b>>21)&127u, channel=(b>>29)&3u;
                require(dst<11 && (b&16u)!=0 && (a&0x26001200u)==0);
                const auto x=source(a&511u,(a>>10)&3u), y=source((a>>13)&511u,(a>>23)&3u);
                float value{};
                if(op==0) value=x+y;
                else if(op==2) value=x*y;
                else if(op==0x19) value=x;
                else throw std::runtime_error("Unexpected TEV shader opcode");
                if((b&0x80000000u)!=0) value=std::clamp(value,0.0f,1.0f);
                pending[dst][channel]=value;
                closed=(a>>31)!=0;
                if(closed) { registers=pending; pending=registers; }
            }
            require(closed);
        } else if(((high>>23)&127u)==1) {
            require(low*2u+4u<=shader.word_count);
            require((shader.words[low*2]&31u)==16 && (shader.words[low*2+1]&127u)==2);
            registers[2]=texture;
        } else if(((high>>23)&127u)==40) {
            require((high&(1u<<21))!=0 && ((low>>15)&127u)==3);
            return registers[3];
        } else throw std::runtime_error("Unexpected TEV shader control flow");
    }
    throw std::runtime_error("TEV shader missing final export");
}
} // namespace tev_test
