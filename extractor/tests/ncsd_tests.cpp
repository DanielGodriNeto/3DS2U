#include "container_fixtures.hpp"
#include "ncsd_extractor.hpp"
#include "romfs_reader.hpp"

namespace {
using namespace extractor;
using namespace extractor::test;
Buffer Cart(const Buffer &ncch, std::size_t offset = 0x4000) {
    Buffer cart(offset + ncch.size());
    std::copy_n("NCSD", 4, cart.begin() + 0x100);
    Put(cart, 0x104, (cart.size() + 0x20000) / 0x200);
    Put(cart, 0x108, 0x0102030405060708ull, 8);
    Put(cart, 0x120, offset / 0x200);
    Put(cart, 0x124, ncch.size() / 0x200);
    Put(cart, 0x190, 0x0102030405060708ull, 8);
    std::copy(ncch.begin(), ncch.end(), cart.begin() + static_cast<std::ptrdiff_t>(offset));
    return cart;
}
KeyStore NCCHKeys(const KeyStore &all) {
    KeyStore keys;
    for (const auto name :
         {"keyx_2c", "keyx_25", "keyx_18", "keyx_1b", "seed_0004000001234500", "ncch_fixed_system_key"})
        if (const auto key = all.Find(name))
            keys.Set(name, *key);
    Check(!keys.Find("common_key_0"));
    return keys;
}
void Validate(const Image &image, const Fixture &fixture) {
    Check(image.code == fixture.code && image.romfs == fixture.romfs && image.exheader.size() == 0x400);
    Check((image.exheader[13] & 1u) == 0);
    wiiu::RomFSReader reader;
    Check(reader.Mount(image.romfs));
    wiiu::RomFSFileHandle handle;
    Check(reader.OpenFile(std::string{"/test.txt"}, handle));
    std::array<char, 14> bytes{};
    Check(reader.ReadFile(handle, 0, bytes.data(), bytes.size()) == 14);
    Check(std::string(bytes.data(), bytes.size()) == "3DS2U_ROMFS_OK");
}
} // namespace
int main() try {
    NCSDExtractor extractor;
    const auto folder =
        std::filesystem::path("build") /
        ("ncsd-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(std::filesystem::create_directories(folder));
    for (const auto &flags : {std::array<unsigned, 6>{1, 0, 0, 0, 0, 2},
                              {1, 1, 0, 0, 1, 2},
                              {1, 0, 1, 0, 1, 2},
                              {1, 0, 0, 0, 0xA, 2},
                              {1, 0, 0, 0, 0xB, 2},
                              {1, 0, 0, 0, 0, 1},
                              {0, 0, 0, 0, 0, 2}}) {
        const auto fixture = Make(false, flags[0] != 0, flags[1] != 0, flags[2] != 0,
                                  static_cast<std::uint8_t>(flags[4]), static_cast<std::uint16_t>(flags[5]));
        const auto keys = NCCHKeys(fixture.keys);
        for (const auto offset : {0x4000u, 0x8000u}) {
            auto cart = Cart(fixture.ncch, offset);
            Validate(extractor.Extract(cart, keys), fixture);
            Write(folder / "fixture.3ds", cart);
            Validate(extractor.ExtractFile(folder / "fixture.3ds", keys), fixture);
            Reject([&] { (void)extractor.Extract(std::span{cart}.first(cart.size() - 1), keys); });
        }
    }
    // Fixed application partitions automatically use the zero key, never a CIA TitleKey.
    const auto fixed = Make(false, true, true, false, 0, 2, true);
    const KeyStore empty;
    const auto fixed_cart = Cart(fixed.ncch);
    Validate(extractor.Extract(fixed_cart, empty), fixed);
    Write(folder / "zero_fixed.3ds", fixed_cart);
    Write(folder / "zero_fixed.cci", fixed_cart);
    const auto system = Make(false, true, false, false, 0, 2, true, true);
    Validate(extractor.Extract(Cart(system.ncch), NCCHKeys(system.keys)), system);
    Reject([&] { (void)extractor.Extract(Cart(system.ncch), empty); });
    const auto standard = Make(false);
    Reject([&] { (void)extractor.Extract(Cart(standard.ncch), empty); });
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        auto bad = fixed_cart;
        switch (mutation) {
        case 0:
            bad[0x100] = 0;
            break;
        case 1:
            Put(bad, 0x120, 0);
            break;
        case 2:
            Put(bad, 0x124, 0);
            break;
        case 3:
            Put(bad, 0x128, 0x20);
            Put(bad, 0x12C, 1);
            break;
        case 4:
            Put(bad, 0x104, 0x21);
            break;
        case 5:
            bad[0x110] = 1;
            break;
        case 6:
            bad[0x190] ^= 1;
            break;
        }
        Reject([&] { (void)extractor.Extract(bad, empty); });
    }
    // Another partition may exist, but extraction must remain confined to partition 0.
    auto multi = fixed_cart;
    multi.resize(multi.size() + 0x200);
    Put(multi, 0x128, fixed_cart.size() / 0x200);
    Put(multi, 0x12C, 1);
    Validate(extractor.Extract(multi, empty), fixed);
    auto corrupt = fixed_cart;
    corrupt[0x4000 + 0xA00 + 0x200] ^= 1;
    Reject([&] { (void)extractor.Extract(corrupt, empty); });
    Reject([&] { (void)ExtractNCCH([](std::uint64_t, std::size_t) { return Buffer{}; }, 0x2000, empty); });
    std::cout
        << "NCSD partition bounds, trimmed dumps, NCCH secure/seed/fixed crypto and RomFS tests passed.\n";
    std::cout << "Synthetic zero-fixed CLI fixtures: " << folder.string() << '\n';
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
