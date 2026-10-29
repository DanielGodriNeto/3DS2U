#include "binary_loader.hpp"
#include "cia_extractor.hpp"
#include "romfs_reader.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
#include <stdexcept>

#include "container_fixtures.hpp"
using namespace extractor;
using namespace extractor::test;
namespace {
class CaptureDiagnostics {
  public:
    CaptureDiagnostics() : previous_(std::cerr.rdbuf(text.rdbuf())) {}
    ~CaptureDiagnostics() { std::cerr.rdbuf(previous_); }
    std::ostringstream text;

  private:
    std::streambuf *previous_;
};
template <class Operation> void RejectWith(Operation operation, const std::string &message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception &error) {
        rejected = true;
        Check(std::string(error.what()).find(message) != std::string::npos);
    }
    Check(rejected);
}
void TestTelemetry(const std::filesystem::path &folder) {
    CaptureDiagnostics logs;
    const ExtractionOptions ignore{true};
    CIAExtractor cia;
    const auto plain = Make(false, false);
    const auto tmd_at = plain.content_at - 0xB40;
    // Change an unused NCCH header byte, preserving all extractable data.
    auto bad = plain.cia;
    bad[plain.content_at + 0x150] ^= 1;
    RejectWith([&] { (void)cia.Extract(bad, plain.keys); }, "CIA TMD content chunk");
    const auto recovered = cia.Extract(bad, plain.keys, ignore);
    Check(recovered.code == plain.code && recovered.romfs == plain.romfs);
    Write(folder / "ignore_hash.cia", bad);
    Check(cia.ExtractFile(folder / "ignore_hash.cia", plain.keys, ignore).code == plain.code);
    // Metadata hashes are independent of both AES layers.
    bad = plain.cia;
    bad[tmd_at + 0x1E4] ^= 1;
    RejectWith([&] { (void)cia.Extract(bad, plain.keys); }, "TMD content-info table");
    Check(cia.Extract(bad, plain.keys, ignore).code == plain.code);
    bad = plain.cia;
    bad[tmd_at + 0x208] ^= 1;
    HashAt(bad, tmd_at + 0x1E4, std::span{bad}.subspan(tmd_at + 0x204, 0x900));
    RejectWith([&] { (void)cia.Extract(bad, plain.keys); }, "TMD chunk records");
    Check(cia.Extract(bad, plain.keys, ignore).code == plain.code);
    const auto ncch_extract = [&](const Buffer &bytes, ExtractionOptions options) {
        return ExtractNCCH(
            [&](std::uint64_t at, std::size_t length) {
                Check(at <= bytes.size() && length <= bytes.size() - at);
                return Buffer(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                              bytes.begin() + static_cast<std::ptrdiff_t>(at + length));
            },
            bytes.size(), plain.keys, options);
    };
    for (const auto offset : {0x160u, 0x1C0u, 0xA00u + 0x1E0u}) {
        auto ncch = plain.ncch;
        ncch[offset] ^= 1;
        if (offset > 0x200)
            HashAt(ncch, 0x1C0, std::span{ncch}.subspan(0xA00, 0x200));
        const auto label = offset == 0x160   ? "NCCH ExHeader"
                           : offset == 0x1C0 ? "NCCH ExeFS superblock"
                                             : "NCCH ExeFS .code";
        RejectWith([&] { (void)ncch_extract(ncch, {}); }, label);
        Check(ncch_extract(ncch, ignore).code == plain.code);
        bad = plain.cia;
        std::copy(ncch.begin(), ncch.end(), bad.begin() + static_cast<std::ptrdiff_t>(plain.content_at));
        Check(cia.Extract(bad, plain.keys, ignore).code == plain.code);
    }
    bad = plain.cia;
    bad[plain.content_at + 0x100] = 0;
    RejectWith([&] { (void)cia.Extract(bad, plain.keys, ignore); }, "NCCH magic missing");
    Write(folder / "missing_magic.cia", bad);
    bad = plain.cia;
    Put(bad, plain.content_at + 0x1A0, 0xFFFFFFFF);
    RejectWith([&] { (void)cia.Extract(bad, plain.keys, ignore); }, "Container range outside input");
    const auto encrypted = Make();
    Check(cia.Extract(encrypted.cia, encrypted.keys).code == encrypted.code);
    auto wrong_ncch = encrypted.keys;
    wrong_ncch.Set("keyx_2c", Block{});
    RejectWith([&] { (void)cia.Extract(encrypted.cia, wrong_ncch); }, "NCCH ExHeader");
    const auto secure = Make(true, true, false, false, 1);
    wrong_ncch = secure.keys;
    wrong_ncch.Set("keyx_25", Block{});
    RejectWith([&] { (void)cia.Extract(secure.cia, wrong_ncch); }, "NCCH ExeFS .code");
    auto wrong = encrypted.keys;
    wrong.Set("common_key_0", Block{});
    RejectWith([&] { (void)cia.Extract(encrypted.cia, wrong, ignore); }, "NCCH magic missing");
    RejectWith([&] { (void)cia.Extract(encrypted.cia, KeyStore{}, ignore); }, "Missing keys.txt entry");
    const auto text = logs.text.str();
    Check(text.find("2b7e151628aed2a6abf7158809cf4f3c") == std::string::npos);
    for (const auto *needle : {"Title ID 0x0004000001234500", "CommonKey index 0", "keyx_2c",
                               "FATAL: NCCH magic missing", "WARNING:", "Expected:", "Actual:"})
        Check(text.find(needle) != std::string::npos);
}
void TestDecryptedContent(const std::filesystem::path &folder) {
    CaptureDiagnostics logs;
    CIAExtractor cia;
    // Preserve the TMD encrypted flag, but replace the outer CBC ciphertext with NCCH bytes.
    for (const bool inner_crypto : {false, true}) {
        auto fixture = Make(true, inner_crypto);
        std::copy(fixture.ncch.begin(), fixture.ncch.end(),
                  fixture.cia.begin() + static_cast<std::ptrdiff_t>(fixture.content_at));
        KeyStore keys;
        if (inner_crypto)
            keys.Set("keyx_2c", common);
        const auto image = cia.Extract(fixture.cia, keys);
        Check(image.code == fixture.code && image.romfs == fixture.romfs);
        Write(folder / "already_decrypted.cia", fixture.cia);
        Check(cia.ExtractFile(folder / "already_decrypted.cia", keys).code == fixture.code);
        // A plaintext ticket field must not be used when outer content is already decrypted.
        const auto ticket_at = Align(Align(0x2020) + 16);
        std::copy(title_key.begin(), title_key.end(),
                  fixture.cia.begin() + static_cast<std::ptrdiff_t>(ticket_at + 0x1BF));
        Check(cia.Extract(fixture.cia, keys).code == fixture.code);
        fixture.cia[fixture.content_at + 0x150] ^= 1;
        RejectWith([&] { (void)cia.Extract(fixture.cia, keys); }, "CIA TMD content chunk");
        Check(cia.Extract(fixture.cia, keys, {true}).code == fixture.code);
    }
    Check(logs.text.str().find("CIA content is already decrypted (NoCrypto). Skipping outer AES-CBC pass.") !=
          std::string::npos);
    Check(logs.text.str().find("Ticket TitleKey AES-CBC: requesting") == std::string::npos);
}
} // namespace
int main() try {
    const auto folder =
        std::filesystem::path("build") /
        ("cia-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(std::filesystem::create_directories(folder));
    TestTelemetry(folder);
    TestDecryptedContent(folder);
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
    std::cout << "CIA CBC/NCCH CTR, secure/seed crypto, BLZ, IVFC, hashes, telemetry, override and loader "
                 "tests passed.\n";
    std::cout << "Synthetic encrypted CLI fixture: " << (folder / "encrypted.cia").string() << '\n';
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
