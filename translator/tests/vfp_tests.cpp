#include "vfp_translator.hpp"
#include "cpp_emitter.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
    const arm11::VFPTranslator translator;
    const std::pair<const char*, std::uint32_t> operations[]{
        {"add",0xEE301A20u}, {"sub",0xEE301A60u}, {"mul",0xEE201A20u}, {"divide",0xEE801A20u},
        {"absolute",0xEEB01AC0u}, {"negative",0xEEB11A40u}, {"root",0xEEB11AC0u},
        {"dadd",0xEE310B02u}, {"dsub",0xEE310B42u}, {"dmul",0xEE210B02u}, {"ddivide",0xEE810B02u},
        {"dabsolute",0xEEB00BC1u}, {"dnegative",0xEEB10B41u}, {"droot",0xEEB10BC1u},
        {"from_int",0xEEB81AC0u}, {"to_int",0xEEBD1AC0u}, {"to_int_round",0xEEBD1A40u},
        {"widen",0xEEB70AC0u}, {"narrow",0xEEB71BC0u}, {"narrow_odd",0xEEF70BC0u},
        {"to_core",0xEE100A90u}, {"from_core",0xEE000A90u},
        {"read_fpscr",0xEEF10A10u}, {"read_flags",0xEEF1FA10u}, {"write_fpscr",0xEEE10A10u},
        {"load_s",0xED901A01u}, {"store_s",0xED801A01u}, {"load_d",0xED900B02u}, {"store_d",0xED800B02u},
        {"load_many",0xECB01A04u}, {"store_many",0xECA01A04u},
        {"load_dmany",0xECB00B04u}, {"store_dmany",0xECA00B04u},
        {"push",0xED2D0B04u}, {"pop",0xECBD0B04u},
    };
    std::ofstream out("build/vfp_execution.cpp");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << "#include \"arm11_types.hpp\"\n#include <iostream>\n";
    std::vector<arm11::Instruction> instructions;
    for (const auto& [name, raw] : operations) {
        const auto code = translator.TranslateInstruction({raw, 0x08000000u});
        if (code == "// Unhandled VFP Instruction") throw std::runtime_error(name);
        out << "void " << name << "(arm11::CPUContext& cpu, arm11::Memory& memory) { (void)cpu; (void)memory;\n" << code << "\n}\n";
        instructions.push_back({raw, 0x08000000u + static_cast<std::uint32_t>(instructions.size() * 4)});
    }
    bool rejected = false;
    try { (void)translator.TranslateInstruction({0xEE710B02u, 0}); } catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("D16 must be rejected for VFPv2");
    if (!arm11::CppEmitter{}.EmitTranslatedFile("build/vfp_blocks.cpp", arm11::CFGBuilder{}.BuildBlocks(instructions)))
        throw std::runtime_error("VFP emitter integration failed");
    out << R"cpp(
void Check(bool condition) { if (!condition) throw std::runtime_error("VFP execution regression"); }
int main() {
    arm11::CPUContext cpu;
    arm11::Memory memory(4096, 0x08010000u);
    for (unsigned i = 1; i < 1000; ++i) {
        const float a = static_cast<float>(i) * 0.125f, b = static_cast<float>(i + 1) * 0.25f;
        cpu.SetS(0,a); cpu.SetS(1,b);
        add(cpu,memory); Check(cpu.GetS(2) == a+b);
        sub(cpu,memory); Check(cpu.GetS(2) == a-b);
        mul(cpu,memory); Check(cpu.GetS(2) == a*b);
        divide(cpu,memory); Check(std::abs(cpu.GetS(2)-a/b) < 0.000001f);
        root(cpu,memory); Check(cpu.GetS(2) == std::sqrt(a));
        cpu.SetD(1,static_cast<double>(a)); cpu.SetD(2,static_cast<double>(b));
        dadd(cpu,memory); Check(cpu.GetD(0) == static_cast<double>(a)+b);
        dsub(cpu,memory); Check(cpu.GetD(0) == static_cast<double>(a)-b);
        dmul(cpu,memory); Check(cpu.GetD(0) == static_cast<double>(a)*b);
        ddivide(cpu,memory); Check(std::abs(cpu.GetD(0)-static_cast<double>(a)/b) < 1e-15);
        droot(cpu,memory); Check(cpu.GetD(0) == std::sqrt(static_cast<double>(a)));
    }
    cpu.SetS(0,-2.0f); absolute(cpu,memory); Check(cpu.GetS(2)==2.0f);
    negative(cpu,memory); Check(cpu.GetS(2)==2.0f);
    cpu.SetD(1,-4.0); dabsolute(cpu,memory); Check(cpu.GetD(0)==4.0);
    dnegative(cpu,memory); Check(cpu.GetD(0)==4.0);
    cpu.SetD(0,std::bit_cast<double>(std::uint64_t{0x1122334455667788ull}));
    Check(cpu.vfp_bits[0]==0x55667788u && cpu.vfp_bits[1]==0x11223344u);
    cpu.SetS(1,1.0f); Check((std::bit_cast<std::uint64_t>(cpu.GetD(0)) >> 32)==0x3F800000u);
    cpu.vfp_bits[0]=std::bit_cast<std::uint32_t>(std::int32_t{-123});
    from_int(cpu,memory); Check(cpu.GetS(2)==-123.0f);
    cpu.SetS(0,-12.75f); to_int(cpu,memory); Check(std::bit_cast<std::int32_t>(cpu.vfp_bits[2])==-12);
    cpu.fpscr=1u<<22; cpu.SetS(0,1.25f); to_int_round(cpu,memory); Check(cpu.vfp_bits[2]==2);
    cpu.fpscr=0; cpu.SetS(0,1.5f); widen(cpu,memory); Check(cpu.GetD(0)==1.5);
    cpu.SetD(0,2.5); narrow(cpu,memory); Check(cpu.GetS(2)==2.5f);
    cpu.SetD(0,2.5); narrow_odd(cpu,memory); Check(cpu.GetS(1)==2.5f);
    cpu.fpscr=0; cpu.SetS(0,std::numeric_limits<float>::quiet_NaN());
    to_int(cpu,memory); Check(cpu.vfp_bits[2]==0 && (cpu.fpscr&1)!=0);
    for (const float input : {2147483648.0f, std::numeric_limits<float>::infinity()}) {
        cpu.fpscr=0; cpu.SetS(0,input); to_int(cpu,memory);
        Check(cpu.vfp_bits[2]==0x7FFFFFFFu && (cpu.fpscr&1)!=0);
    }
    for (const float input : {-4294967296.0f, -std::numeric_limits<float>::infinity()}) {
        cpu.fpscr=0; cpu.SetS(0,input); to_int(cpu,memory);
        Check(cpu.vfp_bits[2]==0x80000000u && (cpu.fpscr&1)!=0);
    }
    cpu.fpscr=0; cpu.SetS(0,1); cpu.SetS(1,0); divide(cpu,memory);
    Check(std::isinf(cpu.GetS(2)) && (cpu.fpscr&2)!=0);
    cpu.fpscr=0; cpu.SetS(0,-1); root(cpu,memory); Check(std::isnan(cpu.GetS(2)) && (cpu.fpscr&1)!=0);
    cpu.fpscr=0; cpu.vfp_bits[1]=0x7FA12345u; to_core(cpu,memory); Check(cpu.r[0]==0x7FA12345u);
    cpu.r[0]=0x12345678; from_core(cpu,memory); Check(cpu.vfp_bits[1]==0x12345678);
    cpu.r[0]=0xA0000000u; write_fpscr(cpu,memory); Check(cpu.fpscr==cpu.r[0]);
    cpu.r[0]=0; read_fpscr(cpu,memory); Check(cpu.r[0]==0xA0000000u);
    read_flags(cpu,memory); Check(cpu.cpsr.N && !cpu.cpsr.Z && cpu.cpsr.C && !cpu.cpsr.V);
    cpu.r[0]=0x08010000u; cpu.vfp_bits[2]=0x7FA12345u;
    store_s(cpu,memory); cpu.vfp_bits[2]=0; load_s(cpu,memory); Check(cpu.vfp_bits[2]==0x7FA12345u);
    cpu.SetD(0,3.25); store_d(cpu,memory); cpu.SetD(0,0); load_d(cpu,memory); Check(cpu.GetD(0)==3.25);
    for (unsigned i=0;i<4;++i) cpu.vfp_bits[2+i]=0x10203040u+i;
    store_many(cpu,memory); Check(cpu.r[0]==0x08010010u);
    cpu.r[0]=0x08010000u; cpu.vfp_bits[2]=0; load_many(cpu,memory); Check(cpu.vfp_bits[2]==0x10203040u && cpu.r[0]==0x08010010u);
    cpu.r[0]=0x08010000u; cpu.SetD(0,5.5); cpu.SetD(1,-7.25); store_dmany(cpu,memory);
    cpu.r[0]=0x08010000u; cpu.SetD(0,0); cpu.SetD(1,0); load_dmany(cpu,memory);
    Check(cpu.GetD(0)==5.5 && cpu.GetD(1)==-7.25);
    cpu.r[13]=0x08010800u; push(cpu,memory); Check(cpu.r[13]==0x080107F0u);
    cpu.SetD(0,0); pop(cpu,memory); Check(cpu.r[13]==0x08010800u && cpu.GetD(0)==5.5);
    const int saved=std::fegetround(); cpu.fpscr=1u<<22; cpu.vfp_bits[0]=16777217u;
    from_int(cpu,memory); Check(cpu.GetS(2)==16777218.0f && std::fegetround()==saved);
    cpu.fpscr=1u<<24; cpu.vfp_bits[0]=1; cpu.SetS(1,0); add(cpu,memory);
    Check(cpu.GetS(2)==0 && (cpu.fpscr&128)!=0);
    cpu.fpscr=1u<<24; cpu.SetS(0,std::numeric_limits<float>::min()); cpu.SetS(1,0.5f); mul(cpu,memory);
    Check(cpu.GetS(2)==0 && (cpu.fpscr&8)!=0 && (cpu.fpscr&16)==0);
    std::cout << "VFP arithmetic, aliases, conversions, FPSCR, rounding and memory execution passed.\n";
}
)cpp";
    std::cout << "VFP translation and emitter generation passed.\n";
}
