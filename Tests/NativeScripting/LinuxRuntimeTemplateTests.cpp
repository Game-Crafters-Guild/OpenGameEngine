// LinuxRuntimeTemplate: the checks a template export applies to the runtime
// directory it copies. The ELF check guards against packaging a Mach-O or PE
// Player into a Linux game; the payload list is shared by the engine's
// template copy and the Steam Deck exporter's template cache.

#include "Engine/Build/LinuxRuntimeTemplate.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using GameEngine::IsLinuxX64Elf;
using GameEngine::IsRuntimeTemplateGamePayload;

namespace
{

// The first 20 bytes of an ELF header: e_ident then e_type, e_machine.
using ElfPrefix = std::array<std::uint8_t, 20>;

ElfPrefix LinuxX64Prefix()
{
    ElfPrefix header{};
    header[0] = 0x7f;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = 2;  // ELFCLASS64
    header[5] = 1;  // ELFDATA2LSB
    header[18] = 62; // EM_X86_64, low byte
    header[19] = 0;
    return header;
}

class LinuxRuntimeTemplateTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        m_Root = fs::temp_directory_path() / ("ge-runtime-template-" + std::to_string(id));
        fs::create_directories(m_Root);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    fs::path Write(const char* name, const std::uint8_t* bytes, std::size_t size)
    {
        const fs::path path = m_Root / name;
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes),
                                                    static_cast<std::streamsize>(size));
        return path;
    }

    fs::path Write(const char* name, const ElfPrefix& header)
    {
        return Write(name, header.data(), header.size());
    }

    fs::path m_Root;
};

} // namespace

TEST_F(LinuxRuntimeTemplateTests, AcceptsLinuxX64Elf)
{
    EXPECT_TRUE(IsLinuxX64Elf(Write("Player", LinuxX64Prefix())));
}

TEST_F(LinuxRuntimeTemplateTests, RejectsOtherArchitecturesAndFormats)
{
    ElfPrefix arm64 = LinuxX64Prefix();
    arm64[18] = 183; // EM_AARCH64
    EXPECT_FALSE(IsLinuxX64Elf(Write("arm64", arm64)));

    ElfPrefix elf32 = LinuxX64Prefix();
    elf32[4] = 1; // ELFCLASS32
    EXPECT_FALSE(IsLinuxX64Elf(Write("elf32", elf32)));

    ElfPrefix bigEndian = LinuxX64Prefix();
    bigEndian[5] = 2; // ELFDATA2MSB
    EXPECT_FALSE(IsLinuxX64Elf(Write("bigEndian", bigEndian)));

    ElfPrefix highMachineByte = LinuxX64Prefix();
    highMachineByte[19] = 1;
    EXPECT_FALSE(IsLinuxX64Elf(Write("highMachineByte", highMachineByte)));

    ElfPrefix portableExecutable{};
    portableExecutable[0] = 'M';
    portableExecutable[1] = 'Z';
    EXPECT_FALSE(IsLinuxX64Elf(Write("Player.exe", portableExecutable)));
}

TEST_F(LinuxRuntimeTemplateTests, RejectsTruncatedAndMissingFiles)
{
    const ElfPrefix header = LinuxX64Prefix();
    EXPECT_FALSE(IsLinuxX64Elf(Write("truncated", header.data(), header.size() - 1)));
    EXPECT_FALSE(IsLinuxX64Elf(m_Root / "missing"));
}

TEST(LinuxRuntimeTemplatePayloadTests, GameFilesArePayload)
{
    for (const char* name : {"Assets", "Packages", "NativeScripts", "game.config", ".assetmanifest",
                             "AssetDatabase.assetdb", "GameEngine.Scripts.dll", "GameEngine.Scripts.native.so",
                             "player_assets.json", "build-steamdeck.log", "steamdeck-player.tar.gz"})
    {
        EXPECT_TRUE(IsRuntimeTemplateGamePayload(fs::path("template") / name)) << name;
    }
}

TEST(LinuxRuntimeTemplatePayloadTests, ComparesNamesCaseInsensitively)
{
    EXPECT_TRUE(IsRuntimeTemplateGamePayload("ASSETS"));
    EXPECT_TRUE(IsRuntimeTemplateGamePayload("Game.Config"));
}

TEST(LinuxRuntimeTemplatePayloadTests, RuntimeFilesAreKept)
{
    for (const char* name : {"Player", "libEngine.so", "launch.sh", "Managed", "runtime-template.json"})
    {
        EXPECT_FALSE(IsRuntimeTemplateGamePayload(fs::path("template") / name)) << name;
    }
}
