#include "tev_combiner.hpp"
#include <algorithm>
#include <stdexcept>

namespace wiiu {
namespace {
TEVCombiner::Color Weights(std::uint32_t source) {
    switch(source) {
    case 0: return {1,0,0,0};
    case 3: return {0,1,0,0};
    case 14: return {0,0,1,0};
    case 15: return {0,0,0,1};
    default: throw std::invalid_argument("Unsupported TEV source");
    }
}
TEVCombiner::Color Operator(std::uint32_t op,std::uint32_t scale) {
    TEVCombiner::Color c{};
    switch(op) {
    case 0: c={1,0,0,0}; break;
    case 1: c={0,0,1,0}; break;
    case 2: c={1,1,0,0}; break;
    case 5: c={1,-1,0,0}; break;
    default: throw std::invalid_argument("Unsupported TEV operator");
    }
    if(scale>2) throw std::invalid_argument("Unsupported TEV scale");
    for(auto& component:c) component*=static_cast<float>(1u<<scale);
    return c;
}
} // namespace
TEVCombiner::TEVCombiner() noexcept {
    for(unsigned stage=0;stage<kStageCount;++stage) {
        registers_[stage*4]=stage==0 ? 0x0F030F03u : 0x0FFF0FFFu;
        registers_[stage*4+2]=stage==0 ? 0x00010001u : 0u;
        registers_[24+stage]=0xFFFFFFFFu;
    }
}
bool TEVCombiner::WriteRegister(std::uint32_t address,std::uint32_t value,std::uint8_t mask) {
    if(address<kBase || address>=kEnd) return false;
    if((address&3u)!=0 || mask>15) throw std::invalid_argument("Invalid TEV register write");
    auto& old=registers_[(address-kBase)/4];
    for(unsigned b=0;b<4;++b) if((mask&(1u<<b))!=0) {
        const auto bits=0xFFu<<(b*8); old=(old&~bits)|(value&bits);
    }
    return true;
}
std::uint32_t TEVCombiner::ReadRegister(std::uint32_t address) const noexcept {
    return address>=kBase && address<kEnd && (address&3u)==0 ? registers_[(address-kBase)/4] : 0;
}
TEVCombiner::Stage TEVCombiner::GetStage(unsigned index) const {
    if(index>=kStageCount) throw std::out_of_range("Invalid TEV stage");
    Stage stage;
    const auto source=registers_[index*4], modifier=registers_[index*4+1];
    for(unsigned n=0;n<3;++n) {
        stage.rgb_sources[n]=(source>>(n*4))&15u;
        stage.alpha_sources[n]=(source>>(16+n*4))&15u;
        stage.rgb_modifiers[n]=(modifier>>(n*4))&15u;
        stage.alpha_modifiers[n]=(modifier>>(12+n*4))&7u;
    }
    stage.rgb_operator=registers_[index*4+2]&15u;
    stage.alpha_operator=(registers_[index*4+2]>>16)&15u;
    stage.constant=registers_[24+index];
    stage.rgb_scale=registers_[index*4+3]&3u;
    stage.alpha_scale=(registers_[index*4+3]>>16)&3u;
    return stage;
}
TEVCombiner::UniformBlock TEVCombiner::BuildUniformBlock() const {
    UniformBlock block{};
    for(unsigned i=0;i<kStageCount;++i) {
        const auto s=GetStage(i); auto& r=block.stages[i].rows;
        r[9]=Operator(s.rgb_operator,s.rgb_scale); r[10]=Operator(s.alpha_operator,s.alpha_scale);
        for(unsigned operand=0;operand<2;++operand) {
            const auto at=operand*4;
            if(operand==0 || s.rgb_operator!=0) {
                r[at]=Weights(s.rgb_sources[operand]);
                const auto m=s.rgb_modifiers[operand];
                if(m>3) throw std::invalid_argument("Unsupported TEV RGB modifier");
                r[at+2]={m<2 ? 1.0f:0.0f,m>=2 ? 1.0f:0.0f,(m&1u) ? -1.0f:1.0f,(m&1u) ? 1.0f:0.0f};
            }
            if(operand==0 || s.alpha_operator!=0) {
                r[at+1]=Weights(s.alpha_sources[operand]);
                const auto m=s.alpha_modifiers[operand];
                if(m>1) throw std::invalid_argument("Unsupported TEV alpha modifier");
                r[at+3]={m ? -1.0f:1.0f,m ? 1.0f:0.0f,0,0};
            }
        }
        for(unsigned c=0;c<4;++c) r[8][c]=static_cast<float>((s.constant>>(c*8))&255u)/255.0f;
    }
    return block;
}
TEVCombiner::Color TEVCombiner::Evaluate(Color primary,Color texture) const {
    const auto block=BuildUniformBlock(); Color previous=primary;
    for(const auto& stage:block.stages) {
        const auto& r=stage.rows;
        const std::array<Color,4> sources{primary,texture,r[8],previous};
        std::array<Color,2> operand{};
        for(unsigned n=0;n<2;++n) {
            Color rgb{},alpha{};
            for(unsigned s=0;s<4;++s) for(unsigned c=0;c<4;++c) {
                rgb[c]+=sources[s][c]*r[n*4][s]; alpha[c]+=sources[s][c]*r[n*4+1][s];
            }
            for(unsigned c=0;c<3;++c)
                operand[n][c]=(rgb[c]*r[n*4+2][0]+rgb[3]*r[n*4+2][1])*r[n*4+2][2]+r[n*4+2][3];
            operand[n][3]=alpha[3]*r[n*4+3][0]+r[n*4+3][1];
        }
        for(unsigned c=0;c<4;++c) {
            const auto& op=r[c==3 ? 10u:9u];
            previous[c]=std::clamp(operand[0][c]*op[0]+operand[1][c]*op[1]+operand[0][c]*operand[1][c]*op[2],0.0f,1.0f);
        }
    }
    return previous;
}
} // namespace wiiu
