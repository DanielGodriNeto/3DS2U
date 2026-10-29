#include "key_store.hpp"
#include "container_reader.hpp"
#include <utility>
namespace extractor {
using namespace detail;
namespace {
std::string Trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{}
                                      : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
} // namespace
void KeyStore::Set(const std::string &name, const Block &key) { keys_.insert_or_assign(name, key); }
const Block *KeyStore::Find(const std::string &name) const {
    const auto it = keys_.find(name);
    return it == keys_.end() ? nullptr : &it->second;
}
Block KeyStore::Require(const std::string &name) const {
    if (const auto key = Find(name))
        return *key;
    throw std::runtime_error("Missing keys.txt entry: " + name);
}
Block KeyStore::CommonKey(std::uint8_t index) const {
    const auto suffix = std::to_string(index);
    if (const auto key = Find("common_key_" + suffix))
        return *key;
    if (index == 0)
        if (const auto key = Find("common_key"))
            return *key;
    return crypto::NormalKey(Require("keyx_3d"), Require("common_key_y_" + suffix));
}
void KeyStore::LoadFile(const std::filesystem::path &filename) {
    std::ifstream file(filename);
    Need(bool(file), "Cannot open keys.txt");
    KeyStore parsed;
    std::string line;
    std::size_t total = 0;
    while (std::getline(file, line)) {
        total += line.size();
        Need(total <= 1024u * 1024u, "keys.txt is too large");
        line = Trim(line.substr(0, line.find('#')));
        if (line.empty())
            continue;
        const auto equal = line.find('=');
        Need(equal != std::string::npos, "Malformed keys.txt entry");
        auto name = Trim(line.substr(0, equal));
        const auto value = Trim(line.substr(equal + 1));
        for (auto& c : name)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        Need(!name.empty() &&
                 name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos &&
                 value.size() == 32,
             "Invalid key name or key length in keys.txt");
        Block key{};
        const auto nibble = [](char c) -> unsigned {
            if (c >= '0' && c <= '9')
                return static_cast<unsigned>(c - '0');
            if (c >= 'a' && c <= 'f')
                return static_cast<unsigned>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F')
                return static_cast<unsigned>(c - 'A' + 10);
            throw std::runtime_error("Non-hexadecimal key in keys.txt");
        };
        for (unsigned i = 0; i < 16; ++i)
            key[i] = static_cast<std::uint8_t>((nibble(value[i * 2]) << 4) | nibble(value[i * 2 + 1]));
        Need(!parsed.Find(name), "Duplicate key name in keys.txt");
        parsed.Set(name, key);
    }
    Need(file.eof(), "Cannot read keys.txt");
    keys_ = std::move(parsed.keys_);
}
} // namespace extractor
