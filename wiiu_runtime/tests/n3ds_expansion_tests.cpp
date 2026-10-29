#include "arm11_types.hpp"
#include "hid_manager.hpp"
#include "pica200.hpp"
#include "svc_handler.hpp"
#include "wiiu_engine.hpp"
#include "wiiu_memory.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <coreinit/thread.h>
#include <cstring>
#include <iostream>
#include <limits>
#include <malloc.h>
#include <source_location>
#include <stdexcept>
#include <vpad/input.h>

namespace {
void Check(bool value, std::source_location at = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("N3DS test failed at line " + std::to_string(at.line()));
}
template <class Operation> void Reject(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception &) {
        rejected = true;
    }
    Check(rejected);
}
void Call(arm11::CPUContext &cpu, std::uint32_t svc, std::array<std::uint32_t, 6> args) {
    for (unsigned i = 0; i < args.size(); ++i)
        cpu.r[i] = args[i];
    wiiu::SvcHandler::Execute(cpu, svc);
}
std::uint32_t LE32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    Check(offset <= bytes.size() && bytes.size() - offset >= 4);
    return bytes[offset] | (std::uint32_t{bytes[offset + 1]} << 8) |
           (std::uint32_t{bytes[offset + 2]} << 16) | (std::uint32_t{bytes[offset + 3]} << 24);
}
std::int16_t Low(std::uint32_t value) {
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}
std::int16_t High(std::uint32_t value) { return Low(value >> 16); }
VPADStatus sample{};
VPADReadError input_error = VPAD_READ_SUCCESS;
std::atomic<unsigned> workers{};
bool Worker(void *, arm11::CPUContext &cpu) {
    ++workers;
    wiiu::SvcHandler::Execute(cpu, 9);
    return false;
}
void WaitThread(wiiu::SvcHandler &kernel, arm11::CPUContext &waiter, std::uint32_t handle) {
    Call(waiter, 0x24, {handle, 0, 1000000000, 0, 0, 0});
    for (unsigned i = 0; i < 2000 && !kernel.Ready(waiter); ++i)
        OSSleepTicks(1000);
    Check(kernel.Ready(waiter) && kernel.GetWaitState(waiter) == wiiu::SvcHandler::WaitState::Signaled);
}
} // namespace

// Host doubles expose the affinity requested by the production SVC implementation.
void *MEMAllocFromDefaultHeapEx(std::uint32_t size, std::int32_t alignment) {
    return _aligned_malloc(size, static_cast<std::size_t>(alignment));
}
void MEMFreeToDefaultHeap(void *pointer) { _aligned_free(pointer); }
void OSReport(const char *, ...) {}
void wiiu::Pica200::WriteMMIO(std::uint32_t, std::uint32_t, std::uint8_t) {}
std::int32_t VPADRead(VPADChan channel, VPADStatus *status, std::uint32_t count, VPADReadError *error) {
    Check(channel == VPAD_CHAN_0 && count == 1);
    *status = sample;
    *error = input_error;
    return input_error == VPAD_READ_SUCCESS ? 1 : 0;
}
void VPADGetTPCalibratedPointEx(VPADChan, VPADTouchPadResolution, VPADTouchData *out,
                                const VPADTouchData *in) {
    *out = *in;
}

int main() try {
    constexpr std::uint32_t huge = 200u * 1024u * 1024u;
    {
        wiiu::WiiUMemory memory(true);
        Check(memory.IsNew3DS() && memory.GetSize() == 256u * 1024u * 1024u &&
              memory.GetVirtualEnd() == 0x18000000);
        Check(reinterpret_cast<std::uintptr_t>(memory.GetPointer(memory.kVirtualBase)) % 0x1000 == 0);
        memory.ReserveImageRange(memory.kVirtualBase, 0x2000);
        wiiu::SvcHandler kernel(memory, Worker);
        arm11::CPUContext cpu{}, waiter{};
        cpu.svc_handler = waiter.svc_handler = &kernel;
        Call(cpu, 1, {3, 0, 0, huge, 3, 0});
        Check(cpu.r[0] == 0 && cpu.r[1] == 0x08002000 && (cpu.r[1] & 0xFFFu) == 0);
        const auto heap = cpu.r[1];
        Check(std::uint64_t{heap} + huge <= memory.GetVirtualEnd());
        Check(memory.Read32(heap) == 0 && memory.Read32(heap + huge - 4) == 0);
        memory.Write32(heap + huge - 4, 0x1234ABCD);
        Check(memory.Read32(heap + huge - 4) == 0x1234ABCD);
        const auto *raw = static_cast<const std::uint8_t *>(memory.GetPointer(heap + huge - 4, 4));
        Check(raw[0] == 0xCD && raw[1] == 0xAB && raw[2] == 0x34 && raw[3] == 0x12);
        Check(memory.FindFreeRange(huge) == 0);
        Check(!memory.ReserveDynamicRange(heap + 0x1000, 0x1000));
        Check(!memory.ReserveDynamicRange(0x17FFF000, 0x2000));
        Reject([&] { (void)memory.GetPointer(0x17FFFFFF, 2); });
        Reject([&] { memory.Write32(0x18000000, 0); });
        Call(cpu, 1, {3, 0, 0, 0xFFFFFFFF, 3, 0});
        Check(cpu.r[0] != 0);
        Call(cpu, 1, {1, heap, 0, huge, 0, 0});
        Check(cpu.r[0] == 0);
        Check(memory.Read32(heap + huge - 4) == 0 && memory.FindFreeRange(huge) == heap);

        // Test every supported explicit core and the existing negative affinity modes.
        for (const auto processor : {0u, 1u, 2u, 0xFFFFFFFFu, 0xFFFFFFFEu}) {
            const auto calls = svc_test_platform::affinity_calls;
            Call(cpu, 8, {48, 0x08000000, 0, 0x08002000, processor, 0});
            Check(cpu.r[0] == 0 && cpu.r[1] != 0);
            Check(svc_test_platform::affinity == (processor < 3 ? 1u << processor : 7u));
            Check(svc_test_platform::affinity_calls == calls + 1 && svc_test_platform::stack_aligned);
            WaitThread(kernel, waiter, cpu.r[1]);
        }
        const auto resumed = svc_test_platform::resumed.load();
        Call(cpu, 8, {48, 0x08000000, 0, 0x08002000, 3, 0});
        Check(cpu.r[0] == wiiu::SvcHandler::kInvalidArgument && svc_test_platform::resumed.load() == resumed);
        svc_test_platform::fail_affinity = true;
        Call(cpu, 8, {48, 0x08000000, 0, 0x08002000, 2, 0});
        Check(cpu.r[0] == 0 && svc_test_platform::affinity == 4);
        WaitThread(kernel, waiter, cpu.r[1]);
        svc_test_platform::fail_affinity = false;
        kernel.Shutdown();
        Check(workers.load() == 6);
        Check(svc_test_platform::created.load() == svc_test_platform::resumed.load());

        wiiu::HIDManager hid(true);
        memory.SetHIDManager(hid);
        sample.hold = sample.trigger = VPAD_BUTTON_A | VPAD_BUTTON_B | VPAD_BUTTON_X | VPAD_BUTTON_Y |
                                       VPAD_BUTTON_PLUS | VPAD_BUTTON_MINUS | VPAD_BUTTON_L | VPAD_BUTTON_R |
                                       VPAD_BUTTON_RIGHT | VPAD_BUTTON_LEFT | VPAD_BUTTON_UP |
                                       VPAD_BUTTON_DOWN | VPAD_BUTTON_ZL | VPAD_BUTTON_ZR;
        sample.leftStick = {-1.0f, 0.5f};
        sample.rightStick = {0.5f, -1.0f};
        sample.tpNormal = {853, 479, 1, VPAD_VALID};
        hid.Update();
        auto shared = hid.GetSharedMemory(), irrst = hid.GetIRRSTSharedMemory();
        const auto index = LE32(shared, 0x10), ir_index = LE32(irrst, 0x10);
        Check(index < 8 && ir_index == index);
        const auto pad = 0x28u + index * 16u, ir = 0x18u + index * 16u;
        constexpr std::uint32_t extended = (1u << 14) | (1u << 15) | (1u << 24) | (1u << 27);
        Check(LE32(shared, pad) == (0xFFFu | (1u << 29) | (1u << 30)));
        Check(LE32(irrst, ir) == extended && LE32(irrst, ir + 4) == extended);
        Check(Low(LE32(shared, pad + 12)) == -156 && High(LE32(shared, pad + 12)) == 78);
        Check(Low(LE32(irrst, ir + 12)) == 78 && High(LE32(irrst, ir + 12)) == -156);
        Check(memory.Read32(hid.kIRRSTBase + ir) == extended);
        Check(memory.Read16(hid.kIRRSTBase + ir + 12) == 78);
        Check(std::bit_cast<std::int16_t>(memory.Read16(hid.kIRRSTBase + ir + 14)) == -156);
        Check(memory.Read8(hid.kIRRSTBase + ir + 14) == 0x64 &&
              memory.Read8(hid.kIRRSTBase + ir + 15) == 0xFF);
        Check(memory.Read32(hid.kBase + 32) == extended);
        const auto touch = 0xC8u + LE32(shared, 0xB8) * 8u;
        Check(LE32(shared, touch) == (319u | (239u << 16)) && LE32(shared, touch + 4) == 1);
        // A service may publish the same LE snapshot at its guest-selected mapping.
        memory.LoadImage(0x08001000, shared);
        Check(memory.Read32(0x08001000 + pad) == LE32(shared, pad));
        const auto old_index = index;
        input_error = VPAD_READ_NO_SAMPLES;
        hid.Update();
        Check(LE32(shared, 0x10) == old_index && memory.Read32(hid.kBase + 4) == 0);
        input_error = VPAD_READ_SUCCESS;
        sample = {};
        hid.Update();
        const auto released = 0x18u + LE32(irrst, 0x10) * 16u;
        Check(LE32(irrst, released) == 0 && LE32(irrst, released + 8) == extended);
        sample.rightStick = {2.0f, -2.0f};
        hid.Update();
        auto axes = LE32(irrst, 0x18u + LE32(irrst, 0x10) * 16u + 12);
        Check(Low(axes) == 156 && High(axes) == -156);
        sample.rightStick = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
        hid.Update();
        axes = LE32(irrst, 0x18u + LE32(irrst, 0x10) * 16u + 12);
        Check(axes == 0);
        for (unsigned i = 0; i < 16; ++i)
            hid.Update();
        Check(LE32(shared, 0x10) < 8 && LE32(irrst, 0x10) < 8);
        Reject([&] { (void)memory.Read32(hid.kIRRSTBase + hid.kIRRSTSize - 1); });
    }
    {
        wiiu::WiiUMemory old;
        wiiu::SvcHandler kernel(old, Worker);
        arm11::CPUContext cpu{};
        cpu.svc_handler = &kernel;
        Check(!old.IsNew3DS() && old.GetSize() == 128u * 1024u * 1024u);
        Call(cpu, 1, {3, 0, 0, huge, 3, 0});
        Check(cpu.r[0] != 0);
        Call(cpu, 8, {48, 0x08000000, 0, 0x08002000, 2, 0});
        Check(cpu.r[0] == wiiu::SvcHandler::kInvalidArgument);
        Reject([&] { (void)old.Read32(0x10000000); });
        wiiu::HIDManager hid;
        sample = {};
        sample.hold = VPAD_BUTTON_ZL | VPAD_BUTTON_ZR;
        sample.rightStick = {1.0f, 1.0f};
        hid.Update();
        Check(hid.ReadRegister(hid.kBase) == 0 && hid.ReadRegister(hid.kBase + 28) == 0);
        Check(LE32(hid.GetIRRSTSharedMemory(), 0x18) == 0);
    }
    {
        wiiu::WiiUEngine engine(true);
        Check(engine.GetMemory().IsNew3DS());
        sample = {};
        sample.hold = sample.trigger = VPAD_BUTTON_ZL;
        engine.RegisterBlock(0x08000000, [](arm11::CPUContext &cpu, wiiu::WiiUMemory &memory) {
            cpu.r[0] = memory.Read32(wiiu::HIDManager::kBase);
            cpu.svc_yield = true;
        });
        engine.Run(0x08000000);
        Check((engine.GetCPU().r[0] & (1u << 14)) != 0);
    }
    std::cout << "N3DS 200 MiB heap, HID/IRRST layout, axis mapping and Core 2 affinity tests passed.\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
