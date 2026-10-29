#include "packaged_romfs.hpp"
#include "romfs_reader.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
int main() {
    wiiu::RomFSReader reader;
    wiiu::RomFSFileHandle handle;
    std::array<char,64> bytes{};
    if (!reader.Mount(wiiu::GetPackagedRomFS()) || !reader.OpenFile("romfs:/test.txt",handle)) return 1;
    const auto size=reader.ReadFile(handle,0,bytes.data(),bytes.size());
    if (std::string(bytes.data(),size)!="3DS2U_ROMFS_OK") return 1;
    std::cout<<"Generated C++ RomFS package mounted and read successfully.\n";
}
