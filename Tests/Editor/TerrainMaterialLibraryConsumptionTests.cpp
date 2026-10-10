// The chain between a bound library and the records the surface shades from. Every link had
// coverage; the JOIN had none, which is how a terrain could carry a correct materialLibrary GUID,
// hot-reload the file on edit, and still shade from its legacy per-layer fields forever.
//
// The load is driven through the real AssetManager rather than a stub: the failure this covers
// lives in whether the library ever becomes RESIDENT for the extraction-side lookup, which a
// hand-placed asset would assert away.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "Assets/TextureAsset.h" // TextureColorSpace enumerators (classification assertions)
#include "Assets/TextureCook.h"  // TextureCookUsage enumerators
#include "Components/Terrain/Terrain.h"
#include "Core/Engine.h"
#include "Terrain/TerrainMaterialRecord.h"
#include "TerrainECS/TerrainMaterialAuthoring.h"
#include "TerrainECS/TerrainMaterialLibraryCache.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::GUID;
using GameEngine::TerrainMaterialEntry;
using GameEngine::TerrainMaterialLibraryAsset;
using GameEngine::TerrainECS::ClassifyTerrainTexture;
using GameEngine::TerrainECS::FindTerrainRoleMaterial;
using GameEngine::TerrainECS::TerrainMaterialLibraryCache;
using GameEngine::TerrainECS::TerrainTextureDeclarer;
using GameEngine::TerrainECS::TerrainTextureKind;
namespace Components = GameEngine::Components;
namespace Terrain = GameEngine::Terrain;

namespace
{

// A four-slot library whose grass role is unmistakably NOT the built-in green, so a record that
// came from the built-in palette or from the legacy per-layer fields cannot be mistaken for one
// that came from the file.
constexpr float kProbeGrassR = 1.0f;
constexpr float kProbeGrassG = 0.05f;
constexpr float kProbeGrassB = 0.05f;

// Role 0 carries a full PBR texture set so the classification the terrain owes the texture system
// is observable; the other roles stay albedo-only, which is what an unmigrated library looks like.
const GUID kProbeAlbedoTexture("a1b2c3d4-0000-4000-8000-000000000001");
const GUID kProbeNormalTexture("a1b2c3d4-0000-4000-8000-000000000002");
const GUID kProbeOrmTexture("a1b2c3d4-0000-4000-8000-000000000003");

std::vector<TerrainMaterialEntry> MakeProbeEntries()
{
    std::vector<TerrainMaterialEntry> entries;
    for (std::uint32_t role = 0; role < Terrain::kTerrainLayerRoleCount; ++role)
    {
        TerrainMaterialEntry e{};
        e.SlotId = static_cast<std::uint8_t>(role);
        e.Name = "Probe " + std::to_string(role);
        e.AlbedoR = kProbeGrassR;
        e.AlbedoG = kProbeGrassG;
        e.AlbedoB = kProbeGrassB;
        if (role == 0)
        {
            e.AlbedoTexture = kProbeAlbedoTexture;
            e.NormalTexture = kProbeNormalTexture;
            e.OrmTexture = kProbeOrmTexture;
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

// Stands in for the texture system, recording what the cache declares about each texture. The
// real declarer routes the same (guid, kind) pairs into TextureService::DeclareTextureClassification.
class DeclarationRecorder
{
public:
    TerrainTextureDeclarer Declarer()
    {
        return [this](const GUID& guid, TerrainTextureKind kind)
        { m_Declared.emplace_back(guid, kind); };
    }

    std::size_t Count() const { return m_Declared.size(); }
    void Clear() { m_Declared.clear(); }

    // How many times this exact texture was declared as this exact kind. Zero for a texture that
    // was never declared, which is the state that uploads a normal map as sRGB.
    std::size_t CountOf(const GUID& guid, TerrainTextureKind kind) const
    {
        std::size_t n = 0;
        for (const auto& [g, k] : m_Declared)
            if (g == guid && k == kind)
                ++n;
        return n;
    }

private:
    std::vector<std::pair<GUID, TerrainTextureKind>> m_Declared;
};

class TerrainMaterialLibraryConsumption : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void SetUp() override
    {
        EngineCore& engine = EngineCore::GetInstance();
        ASSERT_TRUE(engine.IsInitialized());

        // Inside the asset root: a file outside it registers with a session-local GUID, which
        // would make this test measure a different failure than the one it is for.
        m_Path = engine.GetAssetManager().GetAssetRoot() /
                 ("ge_matlib_consumption_" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  ".terrainmatlib");

        {
            std::ofstream out(m_Path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(out.is_open()) << m_Path.string();
            out << TerrainMaterialLibraryAsset::MakeDocumentText(MakeProbeEntries());
        }

        ASSERT_TRUE(engine.GetAssetManager().GetRegistry().RegisterAsset(m_Path));
        m_Guid = engine.GetAssetManager().GetRegistry().GetAssetGUID(m_Path);
        ASSERT_FALSE(m_Guid.IsNull()) << "the library did not register at all";

        // Instrument check: the whole test is about whether a library the registry knows becomes
        // reachable to extraction. A registry that never typed it would make the miss below look
        // like the defect under test when it was really a fixture problem.
        GameEngine::AssetMetadata md{};
        ASSERT_TRUE(engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(m_Guid, md));
        ASSERT_EQ(md.Type, GameEngine::AssetType::TerrainMaterialLibrary);
    }

    void TearDown() override
    {
        // Unregister BEFORE deleting: the asset root is watched, so a file removed while its
        // asset is still resident leaves an auto-reload queued that fires during a later test --
        // and its log line lands in the middle of gtest's summary, splitting "[  PASSED  ] N".
        // A gate that parses that line would read a corrupted count.
        EngineCore& engine = EngineCore::GetInstance();
        if (engine.IsInitialized())
        {
            engine.GetAssetManager().UnregisterLoadedAsset(m_Guid);
            std::error_code ec;
            std::filesystem::remove(m_Path, ec);
            for (int i = 0; i < 5; ++i)
                engine.GetAssetManager().Update();
            return;
        }
        std::error_code ec;
        std::filesystem::remove(m_Path, ec);
    }

    // Resolves the library the way extraction does, pumping the asset manager between attempts so
    // an asynchronous load has the frames it needs. Extraction gets one call per frame; this is
    // the same call, given a generous but finite number of them.
    const std::vector<TerrainMaterialEntry>* ResolveWithPumping(TerrainMaterialLibraryCache& cache,
                                                                int maxFrames = 600)
    {
        EngineCore& engine = EngineCore::GetInstance();
        for (int frame = 0; frame < maxFrames; ++frame)
        {
            if (const auto* entries = cache.Get(m_Guid, m_Recorder.Declarer()))
                return entries;
            engine.GetAssetManager().Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return nullptr;
    }

    std::filesystem::path m_Path;
    GUID m_Guid;
    DeclarationRecorder m_Recorder;
};

// The join under test: a bound GUID must become entries extraction can read. Failing here means
// every terrain in the project shades from its legacy fields no matter what its library says.
TEST_F(TerrainMaterialLibraryConsumption, ABoundLibraryBecomesResidentAndResolvesItsEntries)
{
    TerrainMaterialLibraryCache cache;

    const std::vector<TerrainMaterialEntry>* entries = ResolveWithPumping(cache);
    ASSERT_NE(entries, nullptr)
        << "the library never became resident, so extraction shades from the legacy fields";
    ASSERT_EQ(entries->size(), Terrain::kTerrainLayerRoleCount);
}

// The step after residency: a terrain's default role slots must find those entries. A library
// that resolves but whose slots match nothing yields the BUILT-IN palette, whose grass role is
// green — visually identical to the legacy path, and the reason the live symptom was ambiguous.
TEST_F(TerrainMaterialLibraryConsumption, DefaultRoleSlotsResolveToTheLibrarysOwnMaterials)
{
    TerrainMaterialLibraryCache cache;
    const std::vector<TerrainMaterialEntry>* entries = ResolveWithPumping(cache);
    ASSERT_NE(entries, nullptr);

    Components::Terrain terrain{};
    terrain.MaterialLibraryGuid.Set(m_Guid);

    for (std::uint32_t role = 0; role < Terrain::kTerrainLayerRoleCount; ++role)
    {
        const TerrainMaterialEntry* entry = FindTerrainRoleMaterial(terrain, entries, role);
        ASSERT_NE(entry, nullptr) << "role " << role << " resolved to no material";
        EXPECT_FLOAT_EQ(entry->AlbedoR, kProbeGrassR);
        EXPECT_FLOAT_EQ(entry->AlbedoG, kProbeGrassG);
        EXPECT_FLOAT_EQ(entry->AlbedoB, kProbeGrassB);
    }
}

// The order extraction actually meets at scene load: the terrain carries its library GUID the
// moment the scene text parses, while the project's asset scan is still running, so the FIRST
// resolve can land before the registry knows the file. That first attempt must not be the only
// attempt — a library that becomes registered a frame later has to be picked up, or the terrain
// shades from its legacy fields for the rest of the session with no error anywhere.
TEST_F(TerrainMaterialLibraryConsumption, AResolveThatLosesTheScanRaceIsRetriedNotLatched)
{
    EngineCore& engine = EngineCore::GetInstance();
    TerrainMaterialLibraryCache cache;

    // Ask for a GUID nothing has registered yet — the scan-race position.
    const GUID unregistered("c0ffee00-1111-4222-8333-444444444444");
    EXPECT_EQ(cache.Get(unregistered, m_Recorder.Declarer()), nullptr);
    for (int i = 0; i < 5; ++i)
        engine.GetAssetManager().Update();

    // The library now exists under that identity, exactly as a completing scan would leave it.
    const std::filesystem::path racePath =
        engine.GetAssetManager().GetAssetRoot() /
        ("ge_matlib_scanrace_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".terrainmatlib");
    {
        std::ofstream out(racePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << TerrainMaterialLibraryAsset::MakeDocumentText(MakeProbeEntries());
    }
    GameEngine::AssetMetadata md{};
    md.Guid = unregistered;
    md.Path = racePath;
    md.Type = GameEngine::AssetType::TerrainMaterialLibrary;
    md.Name = racePath.stem().string();
    md.Extension = ".terrainmatlib";
    ASSERT_TRUE(engine.GetAssetManager().GetRegistry().RegisterAssetMetadata(md));

    const std::vector<TerrainMaterialEntry>* entries = nullptr;
    for (int frame = 0; frame < 600 && !entries; ++frame)
    {
        entries = cache.Get(unregistered, m_Recorder.Declarer());
        engine.GetAssetManager().Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::error_code ec;
    std::filesystem::remove(racePath, ec);

    ASSERT_NE(entries, nullptr)
        << "the first failed resolve latched: the library registered a frame later is never "
           "requested again, so the terrain shades from its legacy fields forever";
    EXPECT_EQ(entries->size(), Terrain::kTerrainLayerRoleCount);
}

// Editing a bound library must reach the surface. The cache holds a COPY of the parsed entries,
// so nothing about the asset reloading makes the cached copy stale on its own — the reload event
// is the only thing that drops it. This drives the real dispatcher event the editor's file-watch
// reload raises, so a subscription that filtered the wrong type or never attached would show up
// as an edit that changes the file and nothing on screen.
TEST_F(TerrainMaterialLibraryConsumption, AReloadedLibraryReplacesTheCachedParse)
{
    EngineCore& engine = EngineCore::GetInstance();
    TerrainMaterialLibraryCache cache;

    const std::vector<TerrainMaterialEntry>* before = ResolveWithPumping(cache);
    ASSERT_NE(before, nullptr);
    ASSERT_FALSE(before->empty());
    ASSERT_FLOAT_EQ((*before)[0].AlbedoR, kProbeGrassR);

    // The authored edit: a different tint on disk, then the asset re-reads it.
    constexpr float kEditedR = 0.125f;
    {
        std::vector<TerrainMaterialEntry> edited = MakeProbeEntries();
        for (auto& e : edited)
            e.AlbedoR = kEditedR;
        std::ofstream out(m_Path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << TerrainMaterialLibraryAsset::MakeDocumentText(edited);
    }
    ASSERT_EQ(engine.GetAssetManager().ReloadAssetNow(m_Guid), GameEngine::ReloadOutcome::Reloaded)
        << "the library was not resident, so this test would prove nothing about reloading";

    // The event the file-watch path raises on the main thread; the cache's invalidator is what
    // must be listening for it.
    engine.GetAssetManager().GetEventDispatcher().DispatchEvent(GameEngine::AssetEvents::AssetReloaded(
        m_Guid, GameEngine::AssetType::TerrainMaterialLibrary, m_Path.string()));

    const std::vector<TerrainMaterialEntry>* after = ResolveWithPumping(cache);
    ASSERT_NE(after, nullptr);
    ASSERT_FALSE(after->empty());
    EXPECT_FLOAT_EQ((*after)[0].AlbedoR, kEditedR)
        << "the edit reached the file and the asset, but the cache kept serving the old parse";
}

// SESSION-B SHAPE. An asset OBJECT can be registered without ever having been loaded --
// construction and Load() are separate steps. Such an object is non-null and reports the right
// AssetType, but carries no materials.
//
// Resolving it is worse than failing to resolve it: the empty entry is CACHED, and the cache is
// consulted ahead of the asset manager on every later call, so the terrain never asks again, never
// warns, and shades every role from the built-in palette -- whose grass role is green. That is a
// bound, correct, freshly authored library rendering as though it did not exist, permanently.
TEST_F(TerrainMaterialLibraryConsumption, AnUnpopulatedAssetObjectIsNotTreatedAsResolved)
{
    EngineCore& engine = EngineCore::GetInstance();

    // Registered but never loaded: what a construct-then-register path leaves behind.
    auto unloaded = std::make_shared<TerrainMaterialLibraryAsset>(m_Guid, m_Path);
    ASSERT_NE(unloaded->GetState(), GameEngine::AssetState::Loaded);
    ASSERT_TRUE(unloaded->GetMaterials().empty());
    engine.GetAssetManager().RegisterLoadedAsset(m_Guid, unloaded);

    TerrainMaterialLibraryCache cache;

    EXPECT_EQ(cache.Get(m_Guid, m_Recorder.Declarer()), nullptr)
        << "an unloaded asset object was accepted as a resolved library; its empty material list "
           "is now cached and every role falls back to the built-in palette forever";

    // And it must recover once the object really holds the file's materials.
    ASSERT_TRUE(unloaded->Load());
    const std::vector<TerrainMaterialEntry>* entries = ResolveWithPumping(cache);
    ASSERT_NE(entries, nullptr) << "the cache never recovered after the library really loaded";
    ASSERT_EQ(entries->size(), Terrain::kTerrainLayerRoleCount);
    EXPECT_FLOAT_EQ((*entries)[0].AlbedoR, kProbeGrassR);
}

// SESSION-A SHAPE. Asset::Reload() unloads THEN loads the new bytes: between them the asset is still
// registered, still the right type, and holds NO materials. Extraction runs on a JobSystem worker
// while reloads run on the main thread, so a resolve can land inside that window -- and one sample
// there poisons the cache permanently, because the cached empty list is returned ahead of the
// asset on every later call and only another reload event can drop it.
TEST_F(TerrainMaterialLibraryConsumption, AResolveDuringTheReloadWindowDoesNotPoisonTheCache)
{
    EngineCore& engine = EngineCore::GetInstance();
    TerrainMaterialLibraryCache cache;

    const std::vector<TerrainMaterialEntry>* before = ResolveWithPumping(cache);
    ASSERT_NE(before, nullptr);
    ASSERT_FLOAT_EQ((*before)[0].AlbedoR, kProbeGrassR);

    auto asset = engine.GetAssetManager().GetAsset(m_Guid);
    ASSERT_NE(asset, nullptr);

    // The reload event drops the cached parse, exactly as the file-watch path does...
    engine.GetAssetManager().GetEventDispatcher().DispatchEvent(GameEngine::AssetEvents::AssetReloaded(
        m_Guid, GameEngine::AssetType::TerrainMaterialLibrary, m_Path.string()));
    // ...and the resolve lands mid-Reload, payload torn down and not yet rebuilt.
    asset->Unload();
    ASSERT_TRUE(static_cast<TerrainMaterialLibraryAsset*>(asset.get())->GetMaterials().empty());

    EXPECT_EQ(cache.Get(m_Guid, m_Recorder.Declarer()), nullptr)
        << "the torn-down reload window was cached as a resolved-but-empty library";

    // Reload finishes; the terrain must come back to its authored materials.
    ASSERT_TRUE(asset->Load());
    const std::vector<TerrainMaterialEntry>* after = ResolveWithPumping(cache);
    ASSERT_NE(after, nullptr);
    ASSERT_EQ(after->size(), Terrain::kTerrainLayerRoleCount);
    EXPECT_FLOAT_EQ((*after)[0].AlbedoR, kProbeGrassR);
}

// The classification a terrain material texture carries. Nothing else can derive it: the terrain
// resolves textures by GUID and never builds a Material, so the material slot-name ladder that
// classifies every other texture in the engine never sees these. Left undeclared, a normal map
// takes the extension guess (sRGB) and every decoded tangent vector is gamma-corrupted.
TEST(TerrainTextureClassification, DataMapsAreLinearAndColourKeepsItsGuess)
{
    const auto normal = ClassifyTerrainTexture(TerrainTextureKind::Normal);
    EXPECT_EQ(normal.ColorSpace, GameEngine::TextureColorSpace::Linear);
    EXPECT_EQ(normal.Usage, GameEngine::TextureCookUsage::Normal) << "normals cook to BC5";

    const auto orm = ClassifyTerrainTexture(TerrainTextureKind::Orm);
    EXPECT_EQ(orm.ColorSpace, GameEngine::TextureColorSpace::Linear);
    EXPECT_EQ(orm.Usage, GameEngine::TextureCookUsage::Packed) << "packed data cooks to BC7";

    // Albedo declares NO color space: its extension guess is already right for an LDR source (sRGB)
    // AND an HDR one (linear), so pinning sRGB here would mis-tag the HDR case.
    const auto albedo = ClassifyTerrainTexture(TerrainTextureKind::Albedo);
    EXPECT_EQ(albedo.ColorSpace, GameEngine::TextureColorSpace::Unknown);
    EXPECT_EQ(albedo.Usage, GameEngine::TextureCookUsage::Color);
}

// Adopting a parse is the last moment before the caller resolves those same GUIDs to bindless
// indices, so every authored texture must be declared by the time the entries come back. A miss
// here is the gamma-corrupt upload: the resolve that follows uploads the map under the guess.
TEST_F(TerrainMaterialLibraryConsumption, AdoptingAParseDeclaresEveryAuthoredTextureWithItsKind)
{
    TerrainMaterialLibraryCache cache;

    const std::vector<TerrainMaterialEntry>* entries = ResolveWithPumping(cache);
    ASSERT_NE(entries, nullptr);

    EXPECT_EQ(m_Recorder.CountOf(kProbeNormalTexture, TerrainTextureKind::Normal), 1u)
        << "the normal map was never classified — it uploads sRGB and decodes to garbage vectors";
    EXPECT_EQ(m_Recorder.CountOf(kProbeOrmTexture, TerrainTextureKind::Orm), 1u)
        << "the ORM map was never classified — AO/roughness upload gamma-corrupted";
    EXPECT_EQ(m_Recorder.CountOf(kProbeAlbedoTexture, TerrainTextureKind::Albedo), 1u);

    // Only the authored references, and never under a kind they do not hold: a normal map declared
    // as albedo would be the same bug wearing the fix's clothes.
    EXPECT_EQ(m_Recorder.Count(), 3u);
    EXPECT_EQ(m_Recorder.CountOf(kProbeNormalTexture, TerrainTextureKind::Albedo), 0u);
}

// Declaration rides the PARSE, not the resolve. Extraction calls Get() every frame for every
// terrain; doing the AssetDatabase lookups there would put metadata reads and string work in a
// per-frame path on an ECS worker for a result that cannot change between parses.
TEST_F(TerrainMaterialLibraryConsumption, TexturesAreDeclaredOncePerParseNotPerResolve)
{
    TerrainMaterialLibraryCache cache;

    ASSERT_NE(ResolveWithPumping(cache), nullptr);
    const std::size_t afterFirst = m_Recorder.Count();
    ASSERT_EQ(afterFirst, 3u);

    for (int frame = 0; frame < 30; ++frame)
        ASSERT_NE(cache.Get(m_Guid, m_Recorder.Declarer()), nullptr);

    EXPECT_EQ(m_Recorder.Count(), afterFirst)
        << "a cache HIT re-declared; extraction would pay this on every terrain, every frame";
}

// An edit that swaps which textures a material references has to re-declare, or the new map
// uploads under the guess while the old one keeps the tag it no longer needs.
TEST_F(TerrainMaterialLibraryConsumption, AReloadRedeclaresTheEditedDocumentsTextures)
{
    EngineCore& engine = EngineCore::GetInstance();
    TerrainMaterialLibraryCache cache;

    ASSERT_NE(ResolveWithPumping(cache), nullptr);
    ASSERT_EQ(m_Recorder.Count(), 3u);
    m_Recorder.Clear();

    // The authored edit: role 0 points its normal slot at a different map.
    const GUID replacementNormal("a1b2c3d4-0000-4000-8000-00000000000f");
    {
        std::vector<TerrainMaterialEntry> edited = MakeProbeEntries();
        edited[0].NormalTexture = replacementNormal;
        std::ofstream out(m_Path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << TerrainMaterialLibraryAsset::MakeDocumentText(edited);
    }
    ASSERT_EQ(engine.GetAssetManager().ReloadAssetNow(m_Guid), GameEngine::ReloadOutcome::Reloaded)
        << "the library was not resident, so this test would prove nothing about reloading";
    engine.GetAssetManager().GetEventDispatcher().DispatchEvent(GameEngine::AssetEvents::AssetReloaded(
        m_Guid, GameEngine::AssetType::TerrainMaterialLibrary, m_Path.string()));

    const std::vector<TerrainMaterialEntry>* after = ResolveWithPumping(cache);
    ASSERT_NE(after, nullptr);
    ASSERT_FALSE(after->empty());
    ASSERT_EQ((*after)[0].NormalTexture, replacementNormal) << "the edit never reached the parse";

    EXPECT_EQ(m_Recorder.CountOf(replacementNormal, TerrainTextureKind::Normal), 1u)
        << "the replacement normal map was never classified after the reload";
}

} // namespace
