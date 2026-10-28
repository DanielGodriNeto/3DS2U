// Native TEV shader test: texture * primary color, then add a red constant.
#include "gx2_manager.hpp"
#include "tev_combiner.hpp"
#include <coreinit/debug.h>
#include <proc_ui/procui.h>
#include <whb/proc.h>
#include <array>
#include <bit>
#include <exception>
#include <stdexcept>

namespace {
std::uint32_t Release(void* context) {
    static_cast<wiiu::gx2::GX2Manager*>(context)->Shutdown(); WHBProcStopRunning(); return 0;
}
} // namespace
int main() {
    WHBProcInit(); wiiu::gx2::GX2Manager graphics; int result=0;
    try {
        if(!graphics.Initialize()) throw std::runtime_error("GX2 initialization failed");
        ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE,Release,&graphics,100);
        wiiu::TEVCombiner tev;
        tev.WriteRegister(wiiu::TEVCombiner::kBase+0x10,0x0FEF0FEF);
        tev.WriteRegister(wiiu::TEVCombiner::kBase+0x18,2);
        tev.WriteRegister(wiiu::TEVCombiner::kBase+0x64,0x00000040);
        constexpr std::array<float,54> vertices{
            -.8f,-.8f,0, .5f,1,.5f,1, 0,1,   .8f,-.8f,0, .5f,1,.5f,1, 1,1,
             .8f,.8f,0, .5f,1,.5f,1, 1,0,    -.8f,-.8f,0, .5f,1,.5f,1, 0,1,
             .8f,.8f,0, .5f,1,.5f,1, 1,0,    -.8f,.8f,0, .5f,1,.5f,1, 0,0};
        std::array<std::uint8_t,sizeof(vertices)> vertex_bytes{};
        for(unsigned i=0;i<vertices.size();++i) {
            const auto bits=std::bit_cast<std::uint32_t>(vertices[i]);
            for(unsigned b=0;b<4;++b) vertex_bytes[i*4+b]=static_cast<std::uint8_t>(bits>>(b*8));
        }
        std::array<std::uint8_t,8*8*4> texture{};
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
            const auto pixel=(y*8+x)*4;
            const bool white=((x^y)&1u)==0;
            texture[pixel]=texture[pixel+1]=white ? 255:0;
            texture[pixel+2]=texture[pixel+3]=255;
        }
        OSReport("[3DS2U TEV] Expected green/purple checkerboard on TV and DRC\n");
        while(WHBProcIsRunning()) {
            graphics.BeginFrame();
            graphics.BindTexture0(texture.data(),8,8,GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8);
            graphics.BindTEVState(tev);
            graphics.DrawTriangles(vertex_bytes.data(),6,36);
            graphics.EndFrame();
        }
    } catch(const std::exception& error) { OSReport("[3DS2U TEV] %s\n",error.what()); result=1; }
    graphics.Shutdown(); WHBProcShutdown(); return result;
}
