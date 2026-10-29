#include "binary_loader.hpp"
#include "cia_extractor.hpp"
#include "romfs_reader.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "container_fixtures.hpp"
using namespace extractor;
using namespace extractor::test;
int main() try {
    const auto folder =
        std::filesystem::path("build") /
        ("cia-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(std::filesystem::create_directories(folder));
    CIAExtractor extractor;
    for (const auto &options : {std::array<unsigned, 6>{1, 1, 0, 0, 0, 2},
                                {1, 1, 1, 0, 1, 2},
                                {1, 1, 0, 1, 1, 2},
                                {1, 1, 0, 0, 0xA, 2},
                                {1, 1, 0, 0, 0xB, 2},
                                {1, 1, 0, 0, 0, 1},
                                {0, 0, 0, 0, 0, 2}}) {
        auto fixture = Make(options[0] != 0, options[1] != 0, options[2] != 0, options[3] != 0,
                            static_cast<std::uint8_t>(options[4]), static_cast<std::uint16_t>(options[5]));
        const auto image = extractor.Extract(fixture.cia, fixture.keys);
        Check(image.code == fixture.code && image.romfs == fixture.romfs && image.exheader.size() == 0x400 &&
              (image.exheader[13] & 1) == 0);
        wiiu::RomFSReader reader;
        Check(reader.Mount(image.romfs));
        wiiu::RomFSFileHandle file;
        Check(reader.OpenFile(std::string{"/test.txt"}, file));
        std::array<char, 32> text{};
        Check(reader.ReadFile(file, 0, text.data(), text.size()) == 14 &&
              std::string(text.data(), 14) == "3DS2U_ROMFS_OK");
        Write(folder / "synthetic.cia", fixture.cia);
        Write(folder / "image.code", image.code);
        Write(folder / "image.exheader", image.exheader);
        Check(extractor.ExtractFile(folder / "synthetic.cia", fixture.keys).code == image.code);
        arm11::BinaryLoader loader;
        Check(loader.LoadFile((folder / "image.code").string(), (folder / "image.exheader").string()));
        if (options[0]) {
            KeyStore missing;
            Reject([&] { (void)extractor.Extract(fixture.cia, missing); });
            auto bad = fixture.keys;
            Block zero{};
            bad.Set("common_key_0", zero);
            Reject([&] { (void)extractor.Extract(fixture.cia, bad); });
        }
        fixture.cia[fixture.content_at + 0x200] ^= 1;
        Reject([&] { (void)extractor.Extract(fixture.cia, fixture.keys); });
    }
    auto fixture = Make();
    Write(folder / "encrypted.cia", fixture.cia);
    {
        std::ofstream keys(folder / "keys.txt");
        keys << "common_key_0=2b7e151628aed2a6abf7158809cf4f3c\nkeyx_2c=2b7e151628aed2a6abf7158809cf4f3c\n";
    }
    KeyStore loaded;
    loaded.LoadFile(folder / "keys.txt");
    Check(loaded.CommonKey(0) == common);
    Check(extractor.ExtractFile(folder / "encrypted.cia", loaded).code == fixture.code);
    Reject([&] { (void)extractor.Extract(std::span{fixture.cia}.first(31), loaded); });
    auto bad = fixture.cia;
    bad[0x20 + content_index / 8] = 0;
    Reject([&] { (void)extractor.Extract(bad, loaded); });
    Check(DecompressCodeLZSS(Compressed()).size() == 28);
    bad = Compressed();
    bad.back() = 0xFF;
    Reject([&] { (void)DecompressCodeLZSS(bad); });
    bad = Compressed();
    bad[10] = 0x80;
    Reject([&] { (void)DecompressCodeLZSS(bad); });
    std::cout << "CIA CBC/NCCH CTR, secure/seed crypto, BLZ, IVFC, hashes and loader tests passed.\n";
    std::cout << "Synthetic encrypted CLI fixture: " << (folder / "encrypted.cia").string() << '\n';
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
