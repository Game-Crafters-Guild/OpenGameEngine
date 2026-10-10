// The cook usage a material bind writes onto a texture asset (assets.texture.usage), which the
// import cook compresses by. A material document's textures are an unordered map, so the tag
// must not depend on which slot binds first. TextureService::DeclareTextureClassification runs
// the same tag write as a slot bind (BindMaterialTextureRef and UpdateMaterialTextures hand it
// TextureCookUsageForMaterialSlot); it is the entry point used here because it needs no GPU
// device and starts no texture load of its own.

#include "Assets/AssetManager.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/TextureService.h"
#include "EngineLogCapture.h"
#include "Logger/LogSink.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{
namespace fs = std::filesystem;

// Raises a flag when a line containing the needle is logged, so a test can act at a moment only
// the log marks, such as a cook that has started.
class LogLineFlagSink final : public Logger::LogSink
{
  public:
    LogLineFlagSink(std::atomic<bool>* flag, std::string needle) : m_Flag(flag), m_Needle(std::move(needle)) {}

    void Write(const Logger::LogMessage& message) override
    {
        if (std::string_view(message.Message).find(m_Needle) != std::string_view::npos)
            m_Flag->store(true);
    }
    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    Logger::String GetName() const override { return "LogLineFlagSink"; }

  private:
    std::atomic<bool>* m_Flag;
    std::string m_Needle;
};

class TextureCookUsageTag : public testing::Test
{
  protected:
    inline static fs::path s_Workspace;
    Engine::Renderer::TextureService m_Textures;
    TextureGpuTranscodeTarget m_PreviousTranscodeTarget = TextureAsset::GetGpuTranscodeTarget();

    void TearDown() override { TextureAsset::SetGpuTranscodeTarget(m_PreviousTranscodeTarget); }

    static void SetUpTestSuite()
    {
        s_Workspace = fs::temp_directory_path() / ("ge-cook-usage-tag-" + GUID::Generate().ToString());
        fs::create_directories(s_Workspace / "Assets");
        auto& engine = EngineCore::GetInstance();
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = s_Workspace.string();
        config.EnableEditor = false;
        ASSERT_TRUE(engine.Initialize(config));
        // As in the editor, the only host that writes these tags: an async reload's payload is
        // adopted by the asset hot-reload drain.
        engine.GetAssetManager().SetHotReloadEnabled(true);
    }

    static void TearDownTestSuite()
    {
        EngineCore::GetInstance().Shutdown();
        std::error_code error;
        fs::remove_all(s_Workspace, error);
    }

    static AssetManager& Assets() { return EngineCore::GetInstance().GetAssetManager(); }

    // A fresh square RGBA source in the project with no usage tag: R, G and B differ per texel,
    // as they do in an occlusion-roughness-metallic map.
    static fs::path WriteTexture(GUID& guid, uint32 size = 4)
    {
        const fs::path path = s_Workspace / "Assets" / (GUID::Generate().ToString() + ".tga");
        std::vector<uint8> bytes(18, 0);
        bytes[2] = 2;
        bytes[12] = uint8(size & 0xff);
        bytes[13] = uint8(size >> 8);
        bytes[14] = uint8(size & 0xff);
        bytes[15] = uint8(size >> 8);
        bytes[16] = 32;
        bytes[17] = 0x28;
        bytes.reserve(bytes.size() + size_t(size) * size * 4);
        for (uint32 y = 0; y < size; ++y)
            for (uint32 x = 0; x < size; ++x)
                bytes.insert(bytes.end(), {uint8(x * 7 + y * 3), uint8(x ^ y), uint8((x * y) >> 5), uint8(255)});
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        const auto registered = Assets().GetRegistry().RegisterAssetByPath(path);
        guid = registered.IsOk() ? registered.Value() : GUID::Null();
        return path;
    }

    void Bind(const GUID& texture, const char* slot)
    {
        const TextureCookUsage usage = TextureCookUsageForMaterialSlot(HashStringId(slot));
        m_Textures.DeclareTextureClassification(
            texture, IsLinearTextureUsage(usage) ? TextureColorSpace::Linear : TextureColorSpace::Unknown,
            usage);
    }

    static std::string StoredUsage(const fs::path& path)
    {
        std::string value;
        Assets().GetRegistry().TryGetMetaValue(path, kTextureUsageMetaKey, value);
        return value;
    }

    // What the import cook encodes the texture to under its stored settings, on a device and
    // build that can block-compress.
    static TextureCookOutput CookOutput(const fs::path& path)
    {
        TextureCookInputs inputs;
        std::string error;
        EXPECT_TRUE(ResolveTextureCookInputs(".tga", [&](const char* key, std::string& value) {
            return Assets().GetRegistry().TryGetMetaValue(path, key, value);
        }, inputs, error)) << error;
        return ResolveTextureCookOutput(inputs.Settings, false, true, true);
    }

    static TextureFormat ResidentFormat(const SharedPtr<Asset>& asset)
    {
        const auto* texture = dynamic_cast<const TextureAsset*>(asset.get());
        return texture ? texture->GetFormat() : TextureFormat::Unknown;
    }

    static bool PumpUntil(const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!done())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            Assets().Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
};

// The game's defect: an occlusion-roughness-metallic map bound to aoMap and metallicRoughnessMap
// cooked to single-channel BC4 whenever the aoMap bind ran first.
TEST_F(TextureCookUsageTag, OneTextureOnOcclusionAndMetallicRoughnessSlotsCooksPackedInEitherOrder)
{
    const char* orders[2][2] = {{"aoMap", "metallicRoughnessMap"}, {"metallicRoughnessMap", "aoMap"}};
    for (const auto& order : orders)
    {
        SCOPED_TRACE(std::string(order[0]) + " first");
        GUID texture;
        const fs::path path = WriteTexture(texture);
        ASSERT_FALSE(texture.IsNull());
        Bind(texture, order[0]);
        Bind(texture, order[1]);
        EXPECT_EQ(StoredUsage(path), "packed");
        EXPECT_EQ(CookOutput(path), TextureCookOutput::BC7);
    }
}

// A widened tag changes the cook key, so the texture already loaded under the narrow tag has to
// be decoded again; the bind asks the asset manager for that reload. The reload stays in flight
// until an Update adopts it, and none runs between the bind and the check.
TEST_F(TextureCookUsageTag, WideningATagReloadsTheLoadedTexture)
{
    GUID texture;
    const fs::path path = WriteTexture(texture);
    ASSERT_FALSE(texture.IsNull());
    Bind(texture, "aoMap");
    ASSERT_EQ(StoredUsage(path), "mask");
    const AssetLoadHandle load = Assets().LoadAsset(texture, [](Result<SharedPtr<Asset>, AssetError>) {});
    ASSERT_TRUE(PumpUntil([&] { return load.IsComplete(); }));
    const SharedPtr<Asset> loaded = load.GetResult();
    ASSERT_NE(loaded, nullptr);
    ASSERT_FALSE(Assets().IsReloadInFlight(texture));

    Bind(texture, "metallicRoughnessMap");
    EXPECT_EQ(StoredUsage(path), "packed");
    EXPECT_TRUE(Assets().IsReloadInFlight(texture));
}

// A colour texture a data slot also binds: no one cooked artefact serves both, so the tag the
// texture carries stays, whether a bind or the texture inspector wrote it, and the conflict is
// reported once however often it is bound again.
TEST_F(TextureCookUsageTag, ColourTagAndDataSlotKeepTheTagAndWarnOnce)
{
    GUID texture;
    const fs::path path = WriteTexture(texture);
    ASSERT_FALSE(texture.IsNull());
    ASSERT_TRUE(Assets().GetRegistry().SetMetaValue(path, kTextureUsageMetaKey, "color"));

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("TextureCookUsageTag log-capture self test");
        Bind(texture, "metallicRoughnessMap");
        Bind(texture, "metallicRoughnessMap");
        Bind(texture, "aoMap");
        Logger::Log::Flush();
    }
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "log-capture self test"), 1u);
    EXPECT_EQ(StoredUsage(path), "color");
    EXPECT_EQ(TestLog::CountLinesContaining(lines, path.filename().string()), 1u);
    const std::string warning = TestLog::FirstLineContaining(lines, path.filename().string());
    EXPECT_NE(warning.find("'color'"), std::string::npos) << warning;
    EXPECT_NE(warning.find("'packed'"), std::string::npos) << warning;
    EXPECT_NE(warning.find("texture inspector"), std::string::npos) << warning;
}

// A texture's first load cooks under the usage the texture has when that cook starts. A bind
// that tags the texture while the cook runs (a second material binding it as a scene loads)
// re-cooks it once the load lands, instead of leaving the untagged cook resident. The source is
// large enough that its cook is still running when the bind lands; a missed window fails the
// test rather than passing it.
TEST_F(TextureCookUsageTag, ATagWrittenWhileTheFirstLoadCooksReachesTheLoadedTexture)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "BC encoder unavailable";
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    GUID texture;
    const fs::path path = WriteTexture(texture, 2048);
    ASSERT_FALSE(texture.IsNull());
    ASSERT_EQ(StoredUsage(path), "");

    AssetLoadHandle load;
    {
        std::vector<std::string> lines;
        TestLog::ScopedEngineLogCapture capture(&lines);
        std::atomic<bool> cookStarted{false};
        Logger::Log::AddSink(
            std::make_unique<LogLineFlagSink>(&cookStarted, path.stem().string() + " -> Uncompressed started"));
        load = Assets().LoadAsset(texture, [](Result<SharedPtr<Asset>, AssetError>) {});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!cookStarted.load() && !load.IsComplete() && std::chrono::steady_clock::now() < deadline)
        {
            Logger::Log::Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(cookStarted.load()) << "the first load's cook start was never logged";
        ASSERT_FALSE(load.IsComplete()) << "the first load finished before the bind; the window was missed";
        Bind(texture, "aoMap");
        ASSERT_EQ(StoredUsage(path), "mask");
    }
    ASSERT_TRUE(PumpUntil([&] { return load.IsComplete(); }));
    const SharedPtr<Asset> loaded = load.GetResult();
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(PumpUntil([&] { return ResidentFormat(loaded) == TextureFormat::BC4; }))
        << "resident format " << static_cast<int>(ResidentFormat(loaded)) << ", stored usage " << StoredUsage(path);
}

} // namespace
