#include "tev_combiner.hpp"
#include "tev_shader_interpreter.hpp"
#include "pica200.hpp"
#include "gx2_manager.hpp"
#include "wiiu_memory.hpp"
#include "gsp_gpu.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <source_location>
#include <string>

namespace {
using TEV=wiiu::TEVCombiner;
using Color=TEV::Color;
void Check(bool value,std::source_location at=std::source_location::current()) {
    if(!value) throw std::runtime_error("TEV test failed at line "+std::to_string(at.line()));
}
void Near(Color a,Color b) { for(unsigned i=0;i<4;++i) Check(std::abs(a[i]-b[i])<0.00001f); }
template<class F> void Reject(F f) {
    bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } Check(rejected);
}
Color Reference(const TEV& tev,Color primary,Color texture) {
    Color previous=primary;
    for(unsigned stage=0;stage<6;++stage) {
        const auto s=tev.GetStage(stage);
        const auto select=[&](unsigned id) {
            if(id==0) return primary;
            if(id==3) return texture;
            if(id==15) return previous;
            Color constant{};
            for(unsigned c=0;c<4;++c) constant[c]=static_cast<float>((s.constant>>(c*8))&255u)/255.0f;
            return constant;
        };
        const auto operation=[](float a,float b,unsigned op) {
            switch(op) { case 0:return a; case 1:return a*b; case 2:return a+b; case 5:return a-b; }
            throw std::runtime_error("Unsupported reference operation");
        };
        std::array<Color,2> operands{};
        for(unsigned n=0;n<2;++n) {
            const auto rgb=select(s.rgb_sources[n]), alpha=select(s.alpha_sources[n]);
            for(unsigned c=0;c<3;++c) {
                const auto mod=s.rgb_modifiers[n];
                const auto value=mod>=2 ? rgb[3]:rgb[c]; operands[n][c]=(mod&1u) ? 1.0f-value:value;
            }
            operands[n][3]=s.alpha_modifiers[n] ? 1.0f-alpha[3]:alpha[3];
        }
        for(unsigned c=0;c<4;++c) previous[c]=std::clamp(operation(operands[0][c],operands[1][c],
            c==3 ? s.alpha_operator:s.rgb_operator)*static_cast<float>(1u<<(c==3 ? s.alpha_scale:s.rgb_scale)),0.0f,1.0f);
    }
    return previous;
}
} // namespace

int main() try {
    TEV tev;
    const auto initial=tev.GetStage(0);
    Check(initial.rgb_sources[0]==3 && initial.rgb_sources[1]==0 && initial.rgb_operator==1);
    Check(initial.alpha_sources[0]==3 && initial.alpha_operator==1);
    const auto block=tev.BuildUniformBlock();
    Check(block.stages[0].rows[0]==Color{0,1,0,0});
    Check(block.stages[0].rows[4]==Color{1,0,0,0});
    Check(block.stages[0].rows[2]==Color{1,0,1,0});
    Check(block.stages[0].rows[9]==Color{0,0,1,0});
    Check((reinterpret_cast<std::uintptr_t>(&block)&255u)==0);
    const Color primary{0.5f,0.25f,0.75f,1}, texture{0.2f,0.4f,0.8f,0.5f};
    const Color product{0.1f,0.1f,0.6f,0.5f};
    Near(tev.Evaluate(primary,texture),product);
    Near(tev_test::Run(wiiu::gx2::shaders::textured_tev_pixel,block,primary,texture),product);
    Near(tev_test::Run(wiiu::gx2::shaders::tev_pixel,block,primary,texture),primary);
    Check(((wiiu::gx2::shaders::block_vertex[2]>>22)&15u)==1);
    Check(((wiiu::gx2::shaders::block_vertex[8]>>13)&511u)==128);
    // Independent math vs actual emitted machine code, including different RGB/alpha sources.
    std::mt19937 random(0x3D52u);
    constexpr std::array<unsigned,4> sources{0,3,14,15}, ops{0,1,2,5};
    for(unsigned sample=0;sample<400;++sample) {
        TEV state;
        for(unsigned stage=0;stage<6;++stage) {
            std::uint32_t src{},mod{};
            for(unsigned n=0;n<3;++n) {
                src|=sources[random()%4]<<(n*4); src|=sources[random()%4]<<(16+n*4);
                mod|=(random()%4)<<(n*4); mod|=(random()%2)<<(12+n*4);
            }
            state.WriteRegister(TEV::kBase+stage*16,src);
            state.WriteRegister(TEV::kBase+stage*16+4,mod);
            state.WriteRegister(TEV::kBase+stage*16+8,ops[random()%4]|(ops[random()%4]<<16));
            state.WriteRegister(TEV::kBase+stage*16+12,(random()%3)|((random()%3)<<16));
            state.WriteRegister(TEV::kBase+0x60+stage*4,random());
        }
        Color a{},b{};
        for(unsigned c=0;c<4;++c) { a[c]=static_cast<float>(random()%256)/255; b[c]=static_cast<float>(random()%256)/255; }
        const auto uniforms=state.BuildUniformBlock();
        Near(state.Evaluate(a,b),Reference(state,a,b));
        Near(tev_test::Run(wiiu::gx2::shaders::textured_tev_pixel,uniforms,a,b),Reference(state,a,b));
        Near(tev_test::Run(wiiu::gx2::shaders::tev_pixel,uniforms,a,b),Reference(state,a,Color{1,1,1,1}));
    }
    tev.WriteRegister(TEV::kBase+0x60,0x12345678);
    tev.WriteRegister(TEV::kBase+0x60,0xAA0000BB,9);
    Check(tev.ReadRegister(TEV::kBase+0x60)==0xAA3456BB);
    Reject([&] { tev.WriteRegister(TEV::kBase+1,0); });
    tev.WriteRegister(TEV::kBase,4); Reject([&] { (void)tev.BuildUniformBlock(); });
    // Deferred draws retain their own constants rather than the final register state.
    wiiu::gx2::GX2Manager graphics; wiiu::Pica200 pica(graphics); wiiu::WiiUMemory memory(pica);
    pica.WriteMMIO(wiiu::Pica200::kVertexBufferRegister,0x08001000);
    pica.WriteMMIO(wiiu::Pica200::kVertexCountRegister,3);
    pica.BeginCommandList(memory);
    pica.WriteMMIO(TEV::kBase+0x60,0xFF0000FF); pica.WriteMMIO(wiiu::Pica200::kDrawRegister,1);
    pica.WriteMMIO(TEV::kBase+0x60,0xFF00FF00); pica.WriteMMIO(wiiu::Pica200::kDrawRegister,1);
    Check(pica.EndCommandList());
    Check(graphics.tev_history.size()==2);
    Check(graphics.tev_history[0].stages[0].rows[8]==Color{1,0,0,1});
    Check(graphics.tev_history[1].stages[0].rows[8]==Color{0,1,0,1});
    pica.BeginCommandList(memory);
    pica.WriteMMIO(wiiu::Pica200::kDrawRegister,1);
    pica.WriteMMIO(TEV::kBase+8,6); pica.WriteMMIO(wiiu::Pica200::kDrawRegister,1);
    Reject([&] { (void)pica.EndCommandList(); }); pica.CancelCommandList(); Check(graphics.draws==2);
    std::cout<<"TEV uniform encoding, emitted shader execution, modifiers, six-stage math and draw snapshots passed.\n";
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
