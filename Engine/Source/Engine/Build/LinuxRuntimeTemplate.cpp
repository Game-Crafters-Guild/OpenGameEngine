#include "Engine/Build/LinuxRuntimeTemplate.h"

#include "Types/StringUtils.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

namespace GameEngine {

namespace {

// ELF identification and e_machine, from the System V ABI.
constexpr std::array<std::uint8_t, 4> kElfMagic = {0x7f, 'E', 'L', 'F'};
constexpr std::size_t kElfClassOffset = 4;
constexpr std::uint8_t kElfClass64 = 2;
constexpr std::size_t kElfDataOffset = 5;
constexpr std::uint8_t kElfDataLittleEndian = 1;
constexpr std::size_t kElfMachineOffset = 18; // 16-bit, in the file's byte order
constexpr std::uint16_t kElfMachineX86_64 = 62;
constexpr std::size_t kElfHeaderBytesRead = kElfMachineOffset + 2;

// Lower-case names; see IsRuntimeTemplateGamePayload.
constexpr std::array<std::string_view, 11> kGamePayloadNames = {
    "assets",
    "packages",
    "nativescripts",
    "game.config",
    ".assetmanifest",
    "assetdatabase.assetdb",
    "gameengine.scripts.dll",
    "gameengine.scripts.native.so",
    "player_assets.json",
    "build-steamdeck.log",
    "steamdeck-player.tar.gz",
};

} // namespace

bool IsLinuxX64Elf(const std::filesystem::path& path)
{
    std::array<std::uint8_t, kElfHeaderBytesRead> header{};
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!file)
        return false;
    for (std::size_t i = 0; i < kElfMagic.size(); ++i)
    {
        if (header[i] != kElfMagic[i])
            return false;
    }
    const std::uint16_t machine =
        static_cast<std::uint16_t>(header[kElfMachineOffset] | (header[kElfMachineOffset + 1] << 8));
    return header[kElfClassOffset] == kElfClass64 && header[kElfDataOffset] == kElfDataLittleEndian &&
           machine == kElfMachineX86_64;
}

bool IsRuntimeTemplateGamePayload(const std::filesystem::path& entry)
{
    const std::string name = ToLowerAscii(entry.filename().string());
    for (const std::string_view payload : kGamePayloadNames)
    {
        if (name == payload)
            return true;
    }
    return false;
}

bool IsRuntimeTemplateDebugSymbol(const std::filesystem::path& entry)
{
    const auto extension = ToLowerAscii(entry.extension().string());
    return ToLowerAscii(entry.filename().string()) == ".debug" || extension == ".pdb" ||
           extension == ".debug" || extension == ".dwp" || extension == ".dsym";
}

} // namespace GameEngine
