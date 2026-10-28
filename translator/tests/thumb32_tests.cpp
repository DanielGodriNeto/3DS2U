#include "thumb_decoder.hpp"
#include "cpp_emitter.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value) { if (!value) throw std::runtime_error("T32 decoder regression"); }
std::uint32_t Branch(std::int32_t displacement, bool exchange = false) {
    const auto bits = static_cast<std::uint32_t>(displacement);
    const auto sign = (bits >> 24) & 1u;
    const auto j1 = 1u ^ (((bits >> 23) & 1u) ^ sign), j2 = 1u ^ (((bits >> 22) & 1u) ^ sign);
    return ((0xF000u | (sign << 10) | ((bits >> 12) & 1023u)) << 16) |
        (exchange ? 0xC000u : 0xD000u) | (j1 << 13) | (j2 << 11) | ((bits >> 1) & 2047u);
}
} // namespace

int main() {
    arm11::ThumbTranslator translator;
    const std::array<std::uint8_t, 8> bytes{0,0xF0,1,0xF8,1,0x20,0xFE,0xE7};
    const auto instructions = translator.DecodeBuffer(bytes, 0x08000FFEu);
    Check(instructions.size()==3 && instructions[0].Width()==4 && instructions[0].opcode==0xF000F801u);
    Check(instructions[1].address==0x08001002u && instructions[1].Width()==2);
    Check(translator.TranslateInstruction(0xF000F801u, 0x08000FFEu).find("cpu.r[14]") != std::string::npos);
    for (auto offset : {-16777216,-1048578,-4096,-2,0,2,4096,16777214})
        Check(arm11::ThumbTranslator::BranchTarget(Branch(offset),0x08000FFEu)==0x08001002u+static_cast<std::uint32_t>(offset));
    for (auto offset : {-16777216,-4096,0,4,16777212})
        Check(arm11::ThumbTranslator::BranchTarget(Branch(offset,true),0x08000FFEu)==0x08001000u+static_cast<std::uint32_t>(offset));
    const auto blocks = arm11::CFGBuilder{}.BuildBlocks(instructions);
    Check(blocks.size()==3 && blocks.at(0x08000FFFu).byte_size==4 && blocks.contains(0x08001005u));
    Check(arm11::CppEmitter{}.EmitTranslatedFile("build/thumb32_blocks.cpp",blocks));
    unsigned rejected=0;
    try { (void)translator.DecodeBuffer(std::span{bytes}.first(2)); } catch (const std::invalid_argument&) { ++rejected; }
    try { (void)arm11::CFGBuilder{}.BuildBlocks({{0xF04F0000u,0x1000,arm11::InstructionSet::Thumb},
                                                {0x2001,0x1002,arm11::InstructionSet::Thumb}}); }
    catch (const std::invalid_argument&) { ++rejected; }
    try { (void)arm11::CFGBuilder{}.BuildBlocks({{0xE001,0x1000,arm11::InstructionSet::Thumb},
        {0x2000,0x1002,arm11::InstructionSet::Thumb},{0xF04F0000u,0x1004,arm11::InstructionSet::Thumb}}); }
    catch (const std::invalid_argument&) { ++rejected; }
    Check(rejected==3);
    std::ofstream out("build/thumb32_execution.cpp");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << "#include \"arm11_types.hpp\"\n#include <iostream>\n";
    const std::pair<const char*, std::uint32_t> operations[]{
        {"mov",0xF04F0201u},{"mov_reg",0xEA4F0200u},{"mov_repeat",0xF04F12FFu},
        {"movw",0xF2412234u},{"movt",0xF2C12234u},
        {"add",0xEB000201u},{"adds",0xEB100201u},{"sub",0xEBA00201u},
        {"add_imm",0xF1000205u},{"sub_imm",0xF1A00205u},{"addw",0xF2000205u},
        {"lsl",0xEA4F02C0u},{"lsls",0xEA5F02C0u},{"lsr32",0xEA4F0210u},
        {"lsl_reg",0xFA00F201u},{"lsr_reg",0xFA20F201u},
        {"store",0xF8C02004u},{"load",0xF8D02004u},{"store_byte",0xF8802005u},{"load_byte",0xF8902005u},
        {"post_load",0xF8502B04u},{"pre_store",0xF8402D04u},{"reg_load",0xF8502021u},
        {"literal",0xF8DF2004u},{"load_pc",0xF8D0F000u},
        {"branch",Branch(8192)},{"exchange",Branch(4096,true)},
    };
    for (const auto& [name, raw] : operations) {
        const auto code = translator.TranslateInstruction({raw,0x08010002u,arm11::InstructionSet::Thumb});
        Check(code != "// Unhandled Thumb Instruction");
        out << "void " << name << "(arm11::CPUContext& cpu, arm11::Memory& memory) { (void)cpu; (void)memory;\n" << code << "\n}\n";
    }
    out << R"cpp(
void Check(bool value) { if (!value) throw std::runtime_error("T32 execution regression"); }
int main() {
    arm11::CPUContext cpu;
    arm11::Memory memory(4096,0x08010000u);
    cpu.cpsr={true,false,true,true};
    mov(cpu,memory); Check(cpu.r[2]==1 && cpu.cpsr.N && cpu.cpsr.V);
    mov_repeat(cpu,memory); Check(cpu.r[2]==0x00FF00FFu);
    movw(cpu,memory); Check(cpu.r[2]==0x1234);
    movt(cpu,memory); Check(cpu.r[2]==0x12341234u);
    cpu.r[0]=0xFFFFFFFFu; cpu.r[1]=1; adds(cpu,memory);
    Check(cpu.r[2]==0 && cpu.cpsr.C && cpu.cpsr.Z && !cpu.cpsr.V);
    cpu.r[0]=100; cpu.r[1]=3; add(cpu,memory); Check(cpu.r[2]==103);
    sub(cpu,memory); Check(cpu.r[2]==97);
    add_imm(cpu,memory); Check(cpu.r[2]==105);
    sub_imm(cpu,memory); Check(cpu.r[2]==95);
    addw(cpu,memory); Check(cpu.r[2]==105);
    mov_reg(cpu,memory); Check(cpu.r[2]==100);
    cpu.r[0]=0xA0000001u; lsls(cpu,memory); Check(cpu.r[2]==8 && cpu.cpsr.C);
    lsr32(cpu,memory); Check(cpu.r[2]==0);
    cpu.r[0]=7; lsl(cpu,memory); Check(cpu.r[2]==56);
    for (auto amount : {0u,1u,31u,32u,33u,255u}) {
        cpu.r[0]=0x80000001u; cpu.r[1]=amount;
        lsl_reg(cpu,memory); Check(cpu.r[2]==(amount<32 ? 0x80000001u<<amount : 0));
        lsr_reg(cpu,memory); Check(cpu.r[2]==(amount<32 ? 0x80000001u>>amount : 0));
    }
    cpu.r[0]=0x08010000u; cpu.r[2]=0x12345678u;
    store(cpu,memory); cpu.r[2]=0; load(cpu,memory); Check(cpu.r[2]==0x12345678u);
    cpu.r[2]=0xABCDEFEEu; store_byte(cpu,memory); Check(memory.Read32(cpu.r[0]+4)==0x1234EE78u);
    load_byte(cpu,memory); Check(cpu.r[2]==0xEE);
    memory.Write32(0x08010000u,0xABCD1234u); post_load(cpu,memory);
    Check(cpu.r[2]==0xABCD1234u && cpu.r[0]==0x08010004u);
    cpu.r[0]=0x08010010u; cpu.r[2]=0x87654321u; pre_store(cpu,memory);
    Check(cpu.r[0]==0x0801000Cu && memory.Read32(cpu.r[0])==0x87654321u);
    cpu.r[0]=0x08010000u; cpu.r[1]=3; reg_load(cpu,memory); Check(cpu.r[2]==0x87654321u);
    memory.Write32(0x08010008u,0x55AA55AAu); literal(cpu,memory); Check(cpu.r[2]==0x55AA55AAu);
    branch(cpu,memory); Check(cpu.r[15]==0x08012006u && cpu.r[14]==0x08010007u && cpu.thumb);
    exchange(cpu,memory); Check(cpu.r[15]==0x08011004u && cpu.r[14]==0x08010007u && !cpu.thumb);
    cpu.r[0]=0x08010000u; memory.Write32(cpu.r[0],0x08020001u);
    load_pc(cpu,memory); Check(cpu.r[15]==0x08020000u && cpu.thumb);
    std::cout << "T32 data, shifts, byte/word memory, BL/BLX and interworking execution passed.\n";
}
)cpp";
    std::cout << "Mixed-width Thumb decoding, long targets and CFG generation passed.\n";
}
