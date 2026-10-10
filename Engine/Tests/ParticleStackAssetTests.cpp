// The particle stack asset: the authoring and cooked forms it loads, what it keeps when a load or
// an edit is refused, and the build step that ships it cooked (CookStagedAssets, step 13e).

#include "Assets/AssetRegistry.h"
#include "Assets/ParserRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/StagedAssetCook.h"
#include "Engine/Build/TexturePackageCook.h"
#include "Particles/Assets/ParticleStackAsset.h"
#include "Particles/ParticleStackAuthoring.h"
#include "Particles/ParticleStackBinary.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Particles;

namespace
{
constexpr const char* kStack =
    R"({"version":1,"entryPhase":1,"lifetime":3,"phases":[{"id":1,"label":"Sparks","processors":[)"
    R"({"type":"emitBurst","id":2,"stage":"emission","parameters":{"count":20}},)"
    R"({"type":"drag","id":3,"stage":"update"}]}]})";

std::filesystem::path TempFile(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

void WriteText(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

std::vector<uint8> ReadBytes(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

TEST(ParticleStackAsset, LoadsTheAuthoringAndTheCookedForm)
{
    const auto file = TempFile("ge_stack_forms.particlestack");
    WriteText(file, kStack);
    ParticleStackAsset authored(GUID::Generate(), file);
    ASSERT_TRUE(authored.Load());
    ASSERT_NE(authored.Compiled(), nullptr);
    EXPECT_EQ(authored.Document()->Phases.front().Label, "Sparks");

    std::string error;
    ASSERT_TRUE(ParticleStackAsset::CookFile(file, error)) << error;
    EXPECT_TRUE(IsParticleStackBinary(ReadBytes(file)));
    ParticleStackAsset cooked(GUID::Generate(), file);
    ASSERT_TRUE(cooked.Load());
    EXPECT_EQ(SerializeParticleStack(*cooked.Document(), -1), SerializeParticleStack(*authored.Document(), -1));

    // Cooking a cooked file changes nothing.
    const auto bytes = ReadBytes(file);
    ASSERT_TRUE(ParticleStackAsset::CookFile(file, error)) << error;
    EXPECT_EQ(ReadBytes(file), bytes);
    std::filesystem::remove(file);
}

TEST(ParticleStackAsset, AFileThatIsNoStackCooksToAnErrorAndStaysAsItWas)
{
    const auto file = TempFile("ge_stack_broken.particlestack");
    WriteText(file, "{broken}");
    std::string error;
    EXPECT_FALSE(ParticleStackAsset::CookFile(file, error));
    EXPECT_FALSE(error.empty());
    const auto bytes = ReadBytes(file);
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "{broken}");
    ParticleStackAsset asset(GUID::Generate(), file);
    EXPECT_FALSE(asset.Load());
    EXPECT_TRUE(asset.HasFailed());
    EXPECT_EQ(asset.Compiled(), nullptr);
    EXPECT_FALSE(asset.Diagnostics().empty());
    std::filesystem::remove(file);
}

TEST(ParticleStackAsset, ARefusedEditKeepsTheStackAndSaysWhy)
{
    ParticleStackAsset asset(GUID::Generate(), TempFile("ge_stack_edit.particlestack"));
    const std::string text = kStack;
    ASSERT_TRUE(asset.LoadFromData({text.begin(), text.end()}));
    const auto running = asset.Compiled();

    auto invalid = *asset.Document();
    invalid.EntryPhase = 99;
    EXPECT_FALSE(asset.SetDocument(invalid));
    EXPECT_EQ(asset.Compiled(), running);
    EXPECT_FALSE(asset.Diagnostics().empty());

    auto edited = *asset.Document();
    edited.Lifetime = 7.0f;
    ASSERT_TRUE(asset.SetDocument(edited));
    EXPECT_NE(asset.Compiled(), running);
    EXPECT_FLOAT_EQ(asset.Compiled()->Lifetime, 7.0f);
    EXPECT_TRUE(asset.Diagnostics().empty());
}

namespace
{
// A staged content root the way the build leaves it before step 13e: project assets under
// Assets/, the manifest naming each by its source path and its path under the root.
struct StagedContent
{
    std::filesystem::path Root = TempFile("ge_staged_content");
    AssetManifest Manifest;

    StagedContent()
    {
        std::filesystem::remove_all(Root);
    }
    ~StagedContent()
    {
        std::filesystem::remove_all(Root);
    }

    std::filesystem::path Stage(const std::string& relative, const std::string& text)
    {
        const auto file = Root / relative;
        std::filesystem::create_directories(file.parent_path());
        WriteText(file, text);
        AssetManifestEntry entry;
        entry.guid = GUID::Generate();
        entry.sourcePath = std::filesystem::path("C:/Project") / relative;
        entry.outputPath = relative;
        Manifest.entries.push_back(entry);
        return file;
    }
};

bool NeverCancelled()
{
    return false;
}
} // namespace

TEST(StagedAssetCook, StacksShipCookedAndOtherAssetsShipAsStaged)
{
    ParserRegistry parsers;
    ASSERT_TRUE(parsers.Initialize());
    StagedContent content;
    const auto stack = content.Stage("Assets/Particles/Sparks.particlestack", kStack);
    const std::string materialText = R"({"version":1,"surfaceShader":"Surfaces/standard_pbr.glsl"})";
    const auto material = content.Stage("Assets/Materials/Plain.material", materialText);
    const auto notes = content.Stage("Assets/Notes/readme.unknownextension", "notes");

    std::string error;
    ASSERT_TRUE(CookStagedAssets(content.Root, content.Manifest, parsers, NeverCancelled, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(IsParticleStackBinary(ReadBytes(stack)));
    ParticleStackAsset shipped(GUID::Generate(), stack);
    ASSERT_TRUE(shipped.Load());
    EXPECT_EQ(shipped.Document()->Phases.front().Label, "Sparks");
    const auto materialBytes = ReadBytes(material);
    EXPECT_EQ(std::string(materialBytes.begin(), materialBytes.end()), materialText);
    const auto noteBytes = ReadBytes(notes);
    EXPECT_EQ(std::string(noteBytes.begin(), noteBytes.end()), "notes");
}

// The step the build runs over its staged content ships a stack cooked, beside a texture bake that has
// nothing to bake: the build's call, not only the stack cook inside it.
TEST(StagedAssetCook, TheBuildsContentCookShipsAStackCooked)
{
    ParserRegistry parsers;
    ASSERT_TRUE(parsers.Initialize());
    AssetRegistry registry;
    StagedContent content;
    const auto stack = content.Stage("Assets/Particles/Sparks.particlestack", kStack);

    TexturePackageCookStats textureStats;
    std::string error;
    ASSERT_TRUE(CookStagedContent(content.Root, content.Manifest, registry, parsers, TextureCookEncodeQuality::QuickBC7,
                                  /*textureWorkers=*/nullptr, NeverCancelled, textureStats, error))
        << error;
    EXPECT_TRUE(IsParticleStackBinary(ReadBytes(stack)));
    EXPECT_EQ(textureStats.Textures, 0u);
}

TEST(StagedAssetCook, AStackThatCannotCookFailsTheBuildNamingIt)
{
    ParserRegistry parsers;
    ASSERT_TRUE(parsers.Initialize());
    StagedContent content;
    const auto broken = content.Stage("Assets/Particles/Broken.particlestack", "{broken}");

    std::string error;
    EXPECT_FALSE(CookStagedAssets(content.Root, content.Manifest, parsers, NeverCancelled, error));
    EXPECT_NE(error.find("Assets/Particles/Broken.particlestack: "), std::string::npos) << error;
    const auto bytes = ReadBytes(broken);
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "{broken}");
}

TEST(StagedAssetCook, ACancelledBuildStopsBeforeTheNextAsset)
{
    ParserRegistry parsers;
    ASSERT_TRUE(parsers.Initialize());
    StagedContent content;
    const auto stack = content.Stage("Assets/Particles/Sparks.particlestack", kStack);

    std::string error;
    EXPECT_FALSE(CookStagedAssets(content.Root, content.Manifest, parsers, []
                                  { return true; }, error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(IsParticleStackBinary(ReadBytes(stack)));
}
