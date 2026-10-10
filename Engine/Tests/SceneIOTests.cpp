#include "Particles/ParticleStackAuthoring.h"
#include <gtest/gtest.h>

#include "EngineLogCapture.h"

#include "Scene/ReflectionSceneSchema.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"

#include "AssetCore/GUID.h"
#include "Assets/AssetDependencyExtractor.h"
#include "Assets/Packages/ProjectPackagesManifest.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkySunDrive.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Engine/Rendering/MeshNameRegistry.h"
#include "Components/Rendering/ParticleRenderer.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Rendering/PostProcessEffects/AmbientOcclusionEffect.h"
#include "Components/Rendering/PostProcessEffects/AtmosphericCloudLayer.h"
#include "Components/Rendering/PostProcessEffects/VolumetricClouds.h"
#include "Components/Rendering/PostProcessEffects/BloomEffect.h"
#include "Components/Rendering/PostProcessEffects/ChromaticAberrationEffect.h"
#include "Components/Rendering/PostProcessEffects/ColorFilterEffect.h"
#include "Components/Rendering/PostProcessEffects/ColorGradeEffect.h"
#include "Components/Rendering/PostProcessEffects/ContrastAdaptiveSharpenEffect.h"
#include "Components/Rendering/PostProcessEffects/CrtEffect.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"
#include "Components/Rendering/PostProcessEffects/DebandEffect.h"
#include "Components/Rendering/PostProcessEffects/DepthOfFieldEffect.h"
#include "Components/Rendering/PostProcessEffects/ExposureAdjustmentEffect.h"
#include "Components/Rendering/PostProcessEffects/FastBlurEffect.h"
#include "Components/Rendering/PostProcessEffects/FilmSimulationEffect.h"
#include "Components/Rendering/PostProcessEffects/HeatDistortionEffect.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessEffects/ScreenSpaceReflectionsEffect.h"
#include "Components/Rendering/PostProcessEffects/VolumetricFogEffect.h"
#include "Components/Rendering/PostProcessEffects/VignetteEffect.h"
#include "Components/Rendering/PostProcessEffects/ShadowSettingsEffect.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffectPresets.h"
#include "Components/Rendering/AmbientLight.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Spline/SplineWall.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Terrain/Terrain.h"
#include "Terrain/TerrainTypes.h" // kMaxTerrainMaterialLayers (the paint-layer clamp bound)
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Transform.h"
#include "Components/Video/VideoTextureComponent.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/UnresolvedComponentStore.h"
#include "Logger/Logger.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainGrass/Scene/TerrainGrassSceneSchemas.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "StagedTestPaths.h"

using namespace GameEngine;

namespace
{
// Example of a user-defined component + schema registered outside Engine libraries.
struct Health
{
    float value = 0.0f;
};

struct DebugMarker
{
    float value = 0.0f;
};

struct MatUser
{
    AssetType type = AssetType::Unknown;
    char guid[64]{}; // GUID string (null-terminated)
};

class HealthSchema final : public Scene::ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Health"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const Scene::SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* h = world.GetComponent<Health>(entity);
        if (!h)
            return;
        outLines.push_back(std::string("Health.value = ") + std::to_string(h->value));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const Scene::SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        if (property != "value")
        {
            if (outError)
                *outError = "Unknown Health property";
            return false;
        }
        float v = 0.0f;
        if (!Scene::ParseFloat(value, v))
        {
            if (outError)
                *outError = "Health.value must be a number";
            return false;
        }
        world.AddComponentImmediate(entity, Health{v});
        return true;
    }
};

class DebugMarkerSchema final : public Scene::ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "DebugMarker"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const Scene::SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* d = world.GetComponent<DebugMarker>(entity);
        if (!d)
            return;
        outLines.push_back(std::string("DebugMarker.value = ") + std::to_string(d->value));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const Scene::SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        if (property != "value")
        {
            if (outError)
                *outError = "Unknown DebugMarker property";
            return false;
        }
        float v = 0.0f;
        if (!Scene::ParseFloat(value, v))
        {
            if (outError)
                *outError = "DebugMarker.value must be a number";
            return false;
        }
        world.AddComponentImmediate(entity, DebugMarker{v});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<DebugMarker>(entity);
        return true;
    }
};

GE_REGISTER_SCENE_SCHEMA(HealthSchema)
GE_REGISTER_SCENE_SCHEMA(DebugMarkerSchema)

class MatUserSchema final : public Scene::ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "MatUser"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const Scene::SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* m = world.GetComponent<MatUser>(entity);
        if (!m)
            return;
        if (m->guid[0] == '\0')
            return;
        outLines.push_back(std::string("MatUser.material = &{") + std::string(m->guid) + "}");
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const Scene::SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        if (property != "material")
        {
            if (outError)
                *outError = "Unknown MatUser property";
            return false;
        }

        Scene::SceneValue v{};
        std::string perr;
        if (!Scene::ParseValue(value, v, &perr))
        {
            if (outError)
                *outError = perr;
            return false;
        }

        if (v.Kind != Scene::SceneValueKind::ResourceRef)
        {
            if (outError)
                *outError = "MatUser.material expects a #resource reference";
            return false;
        }

        AssetReference ref{};
        std::string rerr;
        if (!Scene::TryResolveResourceIdToAssetReference(ctx, v.StringValue, ref, &rerr))
        {
            if (outError)
                *outError = rerr;
            return false;
        }

        MatUser mu{};
        mu.type = ref.type;
        std::memset(mu.guid, 0, sizeof(mu.guid));
        const std::string gs = ref.guid.ToString();
        const size_t n = std::min(gs.size(), sizeof(mu.guid) - 1);
        std::memcpy(mu.guid, gs.data(), n);
        mu.guid[n] = '\0';
        world.AddComponentImmediate(entity, mu);
        return true;
    }
};

GE_REGISTER_SCENE_SCHEMA(MatUserSchema)

// V2 component+schema that exercises FormatAssetReferenceForSave/TryResolveAssetReference.
// Stored as POD so it satisfies the ECS Component concept.
struct MatUserV2
{
    char guid[64]{};  // null-terminated GUID string ("" when unset)
    char path[256]{}; // null-terminated path string ("" when unset)
};

class MatUserV2Schema final : public Scene::ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "MatUserV2"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const Scene::SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* m = world.GetComponent<MatUserV2>(entity);
        if (!m)
            return;
        const bool hasGuid = m->guid[0] != '\0';
        const bool hasPath = m->path[0] != '\0';
        if (!hasGuid && !hasPath)
            return;
        GUID g = GUID::Null();
        if (hasGuid)
        {
            try
            {
                g = GUID(std::string(m->guid));
            }
            catch (...)
            {
            }
        }
        outLines.push_back(std::string("MatUserV2.mat = ") +
                           Scene::FormatAssetReferenceForSave(ctx, g, std::string_view(m->path)));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const Scene::SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        if (property != "mat")
        {
            if (outError)
                *outError = "Unknown MatUserV2 property";
            return false;
        }
        Scene::SceneValue sv{};
        if (!Scene::ParseValue(value, sv, outError))
            return false;

        AssetReference ref{};
        if (!Scene::TryResolveAssetReference(ctx, sv, AssetType::Material, ref, outError))
            return false;

        MatUserV2 mu{};
        if (!ref.guid.IsNull())
        {
            const std::string gs = ref.guid.ToString();
            const size_t n = std::min(gs.size(), sizeof(mu.guid) - 1);
            std::memcpy(mu.guid, gs.data(), n);
            mu.guid[n] = '\0';
        }
        if (!ref.path.empty())
        {
            const size_t n = std::min(ref.path.size(), sizeof(mu.path) - 1);
            std::memcpy(mu.path, ref.path.data(), n);
            mu.path[n] = '\0';
        }
        world.AddComponentImmediate(entity, mu);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<MatUserV2>(entity);
        return true;
    }

    void EnumerateAssetReferences(const ECS::World& world,
                                  ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override
    {
        const auto* m = world.GetComponent<MatUserV2>(entity);
        if (!m || !visitor || m->guid[0] == '\0')
            return;
        GUID g = GUID::Null();
        try
        {
            g = GUID(std::string(m->guid));
        }
        catch (...)
        {
        }
        if (!g.IsNull())
        {
            const std::string_view path = m->path[0] != '\0' ? std::string_view(m->path) : std::string_view{};
            visitor(g, AssetType::Material, path, "mat");
        }
    }
};

GE_REGISTER_SCENE_SCHEMA(MatUserV2Schema)

// Test resolver: in-memory map of (guid <-> path) pairs.
// Set up in the test, then passed to Save/LoadOptions.
class FakeAssetResolver final : public Scene::ISceneAssetResolver
{
  public:
    void SetRoot(std::filesystem::path root) { m_Root = std::move(root); }
    // Mirrors the editor resolver: AssetRegistry::RegisterAssetByPath fails for
    // paths that aren't on disk, so GetOrCreateAssetGuid returns Null for them.
    void SetMintRequiresFileOnDisk(bool require) { m_MintRequiresFileOnDisk = require; }
    // A further mount searched after the primary root, in registration order —
    // the editor mount, for a scene shipped with the editor.
    void AddMountRoot(std::filesystem::path root) { m_MountRoots.push_back(std::move(root)); }
    void Bind(const GUID& g, const std::filesystem::path& relPath, AssetType t)
    {
        const auto abs = (m_Root / relPath).lexically_normal();
        m_GuidToPath[g] = {abs, t};
        m_PathToGuid[abs.lexically_normal().generic_string()] = {g, t};
    }
    void Unbind(const GUID& g)
    {
        auto it = m_GuidToPath.find(g);
        if (it == m_GuidToPath.end())
            return;
        m_PathToGuid.erase(it->second.first.lexically_normal().generic_string());
        m_GuidToPath.erase(it);
    }
    int MintCalls() const { return m_MintCalls; }
    // Mirrors the registry's journaled redirects (rename heals): ResolveGuid
    // chases these, records or not.
    void AddRedirect(const GUID& from, const GUID& to) { m_Redirects[from] = to; }

    std::filesystem::path GetAssetRoot() const override { return m_Root; }
    // Mirrors AssetManager::ResolveAssetPath: the primary root is the project mount,
    // the first root holding the file wins, and nothing found anchors to the primary.
    std::filesystem::path ResolveAssetPath(const std::filesystem::path& authoredPath) const override
    {
        if (authoredPath.is_absolute())
            return authoredPath;
        if (m_Root.empty())
            return {};
        const auto primary = (m_Root / authoredPath).lexically_normal();
        if (std::filesystem::exists(primary))
            return primary;
        for (const auto& root : m_MountRoots)
        {
            const auto candidate = (root / authoredPath).lexically_normal();
            if (std::filesystem::exists(candidate))
                return candidate;
        }
        return primary;
    }
    GUID ResolveGuid(const GUID& g) const override
    {
        GUID resolved = g;
        for (int depth = 0; depth < 8; ++depth)
        {
            auto it = m_Redirects.find(resolved);
            if (it == m_Redirects.end() || it->second == resolved)
                break;
            resolved = it->second;
        }
        return resolved;
    }
    GUID GetOrCreateAssetGuid(const std::filesystem::path& abs) override
    {
        ++m_MintCalls;
        if (m_MintRequiresFileOnDisk && !std::filesystem::exists(abs))
            return GUID::Null();
        const auto key = abs.lexically_normal().generic_string();
        auto it = m_PathToGuid.find(key);
        if (it != m_PathToGuid.end())
            return it->second.first;
        // Mint a deterministic GUID (NFC + lowercase) and bind it.
        const GUID g = GUID::Derive(GUID::Null(), key);
        m_GuidToPath[g] = {abs.lexically_normal(), AssetType::Unknown};
        m_PathToGuid[key] = {g, AssetType::Unknown};
        return g;
    }
    bool TryGetPathAndType(const GUID& g, std::filesystem::path& outPath, AssetType& outType) const override
    {
        auto it = m_GuidToPath.find(g);
        if (it == m_GuidToPath.end())
            return false;
        outPath = it->second.first;
        outType = it->second.second;
        return true;
    }
    bool TryGetGuidAndType(const std::filesystem::path& abs, GUID& outGuid, AssetType& outType) const override
    {
        const auto key = abs.lexically_normal().generic_string();
        auto it = m_PathToGuid.find(key);
        if (it == m_PathToGuid.end())
            return false;
        outGuid = it->second.first;
        outType = it->second.second;
        return true;
    }

  private:
    std::filesystem::path m_Root;
    std::unordered_map<GUID, std::pair<std::filesystem::path, AssetType>> m_GuidToPath;
    std::unordered_map<std::string, std::pair<GUID, AssetType>> m_PathToGuid;
    std::unordered_map<GUID, GUID> m_Redirects;
    std::vector<std::filesystem::path> m_MountRoots;
    mutable int m_MintCalls = 0;
    bool m_MintRequiresFileOnDisk = false;
};

static std::filesystem::path MakeTempPath(const char* filename)
{
    const auto dir = std::filesystem::temp_directory_path() / "GameEngine_SceneIOTests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / filename).lexically_normal();
}

// Staged fixture mirror root (StageTestAssets); never climbs into the source tree.
static std::filesystem::path ResolveStagedFixturePath(const std::filesystem::path& relativePath)
{
    return (GameEngine::TestPaths::StagedRoot() / relativePath).lexically_normal();
}

static bool WriteFile(const std::filesystem::path& p, const std::string& text)
{
    std::ofstream f(p, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f.is_open())
        return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

static bool ReadFile(const std::filesystem::path& p, std::string& out)
{
    std::ifstream f(p, std::ios::in | std::ios::binary);
    if (!f.is_open())
        return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::string s;
    if (size > 0)
        s.resize(static_cast<size_t>(size));
    if (size > 0)
        f.read(s.data(), size);
    out = std::move(s);
    return true;
}

static ECS::EntityHandle FindByTag(ECS::World& world, const std::string& tag)
{
    auto arches = world.GetAllArchetypes();
    for (auto* a : arches)
    {
        if (!a)
            continue;
        for (const auto& h : a->CollectEntities())
        {
            if (!h.IsValid() || !world.IsValid(h))
                continue;
            if (auto* t = world.GetComponent<Components::SceneEntityTag>(h))
            {
                if (t->View() == tag)
                    return h;
            }
        }
    }
    return ECS::EntityHandle::Invalid();
}
} // namespace

TEST(SceneIO, Load_EntityWithoutName_GetsNameFromSceneId)
{
    const auto scenePath = MakeTempPath("noname.scene");
    const std::string src =
        "[scene name=\"NoName\" version=1]\n"
        "\n"
        "[entity id=\"main_camera\"]\n"
        "Transform.position = (0, 3, -10)\n"
        "Transform.rotation = (0, 0, 0, 1)\n"
        "Transform.scale = (1, 1, 1)\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    auto h = FindByTag(world, "main_camera");
    ASSERT_TRUE(h.IsValid());
    auto* n = world.GetComponent<Components::Name>(h);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->View(), "main_camera");
}

// A raw component write (GE_ECSABI_SetComponentBytes) can fill all 64 bytes of a
// Name or SceneEntityTag with no terminator. Saving must write exactly those 64
// bytes, not run on into the next entity's value in the same chunk column.
TEST(SceneIO, Save_NameAndTagWithNoTerminatorWriteOnlyTheirOwnBytes)
{
    const auto scenePath = MakeTempPath("name_no_terminator.scene");

    ECS::World world;
    const ECS::EntityHandle full = world.CreateEntity();
    const ECS::EntityHandle neighbour = world.CreateEntity();
    ASSERT_TRUE(full.IsValid());
    ASSERT_TRUE(neighbour.IsValid());
    Components::SceneEntityTag fullTag{};
    std::memset(fullTag.value, 'f', sizeof(fullTag.value));
    world.AddComponentImmediate(full, fullTag);
    Components::SceneEntityTag neighbourTag{};
    std::memcpy(neighbourTag.value, "neighbour_tag", 14);
    world.AddComponentImmediate(neighbour, neighbourTag);
    Components::Name fullName{};
    std::memset(fullName.value, 'a', sizeof(fullName.value));
    world.AddComponentImmediate(full, fullName);
    Components::Name neighbourName{};
    std::memcpy(neighbourName.value, "Neighbour", 10);
    world.AddComponentImmediate(neighbour, neighbourName);

    const std::string fullTagText(sizeof(fullTag.value), 'f');
    const std::string fullNameText(sizeof(fullName.value), 'a');
    EXPECT_EQ(world.GetComponent<Components::SceneEntityTag>(full)->View(), fullTagText);
    EXPECT_EQ(world.GetComponent<Components::Name>(full)->View(), fullNameText);

    ASSERT_TRUE(Scene::SaveSceneToFile(world, scenePath, Scene::SaveOptions{}));
    std::string body;
    ASSERT_TRUE(ReadFile(scenePath, body));
    EXPECT_NE(body.find("[entity id=\"" + fullTagText + "\""), std::string::npos) << body;
    EXPECT_EQ(body.find("fneighbour_tag"), std::string::npos) << body;
    EXPECT_NE(body.find("Name.value = \"" + fullNameText + "\"\n"), std::string::npos) << body;
    EXPECT_EQ(body.find("aNeighbour"), std::string::npos) << body;
}

TEST(SceneIO, Load_VideoTexture_MigratesLegacyUniformName)
{
    const auto scenePath = MakeTempPath("videotex_legacy.scene");
    const std::string src =
        "[scene name=\"VideoLegacy\" version=1]\n"
        "\n"
        "[entity id=\"video_plane\"]\n"
        "Transform.position = (0, 0, 0)\n"
        "Transform.rotation = (0, 0, 0, 1)\n"
        "Transform.scale = (1, 1, 1)\n"
        "VideoTextureComponent.videoPath = \"videos/clip.mp4\"\n"
        "VideoTextureComponent.uniformName = \"uBaseColor\"\n"
        "VideoTextureComponent.loop = true\n"
        "VideoTextureComponent.playOnStart = true\n"
        "VideoTextureComponent.playbackSpeed = 1\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    auto h = FindByTag(world, "video_plane");
    ASSERT_TRUE(h.IsValid());
    auto* c = world.GetComponent<Components::VideoTextureComponent>(h);
    ASSERT_NE(c, nullptr);
    // Pre-migration scenes carry "uBaseColor", which resolves to no material
    // texture slot; loading migrates it to the canonical base-color slot name.
    EXPECT_STREQ(c->uniformName, "albedoMap");
    EXPECT_STREQ(c->videoPath, "videos/clip.mp4");
}

// videoPath sits directly before uniformName in the component. A path that
// fills all 512 bytes with no terminator must save as exactly those bytes,
// not run on into the slot name.
TEST(SceneIO, Save_VideoPathWithNoTerminatorWritesOnlyItsOwnBytes)
{
    const auto scenePath = MakeTempPath("videotex_no_terminator.scene");

    ECS::World world;
    const ECS::EntityHandle e = world.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    world.AddComponentImmediate(e, Components::SceneEntityTag{{'v', '\0'}});
    Components::VideoTextureComponent video{};
    std::memset(video.videoPath, 'v', sizeof(video.videoPath));
    world.AddComponentImmediate(e, video);

    const std::string fullPath(sizeof(video.videoPath), 'v');
    EXPECT_EQ(world.GetComponent<Components::VideoTextureComponent>(e)->VideoPath(), fullPath);

    ASSERT_TRUE(Scene::SaveSceneToFile(world, scenePath, Scene::SaveOptions{}));
    std::string body;
    ASSERT_TRUE(ReadFile(scenePath, body));
    EXPECT_NE(body.find("VideoTextureComponent.videoPath = \"" + fullPath + "\"\n"), std::string::npos) << body;
    EXPECT_EQ(body.find("valbedoMap"), std::string::npos) << body;
}

TEST(SceneIO, Load_AutoNumericEntityId_DoesNotCopyToName)
{
    const auto scenePath = MakeTempPath("autoid.scene");
    const std::string src =
        "[scene name=\"Auto\" version=1]\n"
        "\n"
        "[entity id=\"e_1048576\"]\n"
        "Transform.position = (0, 0, 0)\n"
        "Transform.rotation = (0, 0, 0, 1)\n"
        "Transform.scale = (1, 1, 1)\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    auto h = FindByTag(world, "e_1048576");
    ASSERT_TRUE(h.IsValid());
    EXPECT_EQ(world.GetComponent<Components::Name>(h), nullptr);
}

TEST(SceneIO, Load_NoAddReflectedComponentFromScene)
{
    const auto scenePath = MakeTempPath("no_add_reflected_component.scene");
    const std::string src =
        "[scene name=\"NoAdd\" version=1]\n"
        "\n"
        "[entity id=\"animated_node\"]\n"
        "AnimatedNodeRef.nodeIndex = 7\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    auto h = FindByTag(world, "animated_node");
    ASSERT_TRUE(h.IsValid());
    const auto* node = world.GetComponent<Components::AnimatedNodeRef>(h);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->nodeIndex, 7u);
}

TEST(SceneIO, LoadBasicScene_EntitiesAndParenting)
{
    const auto scenePath = MakeTempPath("basic.scene");
    const std::string src =
        "[scene name=\"Basic\" version=1]\n"
        "\n"
        "[entity id=\"root\"]\n"
        "Name.value = \"Root\"\n"
        "Transform.position = (1, 2, 3)\n"
        "\n"
        "[entity id=\"child\" parent=\"root\"]\n"
        "Name.value = \"Child\"\n"
        "Transform.position = (0, 0, 1)\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));

    auto root = FindByTag(world, "root");
    auto child = FindByTag(world, "child");
    ASSERT_TRUE(root.IsValid());
    ASSERT_TRUE(child.IsValid());

    {
        auto* n = world.GetComponent<Components::Name>(root);
        ASSERT_NE(n, nullptr);
        EXPECT_EQ(n->View(), "Root");
        auto* t = world.GetComponent<Components::Transform>(root);
        ASSERT_NE(t, nullptr);
        auto p = t->GetPosition();
        EXPECT_FLOAT_EQ(p.x, 1.0f);
        EXPECT_FLOAT_EQ(p.y, 2.0f);
        EXPECT_FLOAT_EQ(p.z, 3.0f);
    }
    {
        auto* n = world.GetComponent<Components::Name>(child);
        ASSERT_NE(n, nullptr);
        EXPECT_EQ(n->View(), "Child");
        auto* p = world.GetComponent<Components::Parent>(child);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->parent.id, root.id);
    }
}

// The switch state of whichever of the two components the tagged entity carries.
static void ExpectComponentsEnabled(ECS::World& world, const char* tag, bool meshEnabled, bool probeEnabled)
{
    const ECS::EntityHandle e = FindByTag(world, tag);
    ASSERT_TRUE(e.IsValid()) << tag;
    const ECS::Entity entity(&world, e);
    if (world.HasComponent<Components::MeshRenderer>(e))
        EXPECT_EQ(entity.IsEnabled<Components::MeshRenderer>(), meshEnabled) << tag;
    if (world.HasComponent<Components::ReflectionProbe>(e))
        EXPECT_EQ(entity.IsEnabled<Components::ReflectionProbe>(), probeEnabled) << tag;
}

// The switch line reads case-insensitively ("MeshRenderer.enabled", "ReflectionProbe.Enabled")
// and "= true" leaves the component on. The writer emits the line only for a component that is
// off.
TEST(SceneIO, SaveThenLoad_ComponentSwitchLinesReadEitherSpellingAndSaveOnlyOff)
{
    const auto scenePath = MakeTempPath("component_enabled.scene");
    const std::string src =
        "[scene name=\"ComponentEnabled\" version=1]\n"
        "\n"
        "[entity id=\"meshOff\"]\n"
        "MeshRenderer.enabled = false\n"
        "\n"
        "[entity id=\"meshOn\"]\n"
        "MeshRenderer.enabled = true\n"
        "\n"
        "[entity id=\"probeOff\"]\n"
        "ReflectionProbe.Enabled = false\n"
        "\n"
        "[entity id=\"probeOn\"]\n"
        "ReflectionProbe.CaptureResolution = 128\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w1;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w1, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    ExpectComponentsEnabled(w1, "meshOff", false, true);
    ExpectComponentsEnabled(w1, "meshOn", true, true);
    ExpectComponentsEnabled(w1, "probeOff", true, false);
    ExpectComponentsEnabled(w1, "probeOn", true, true);
    EXPECT_EQ(w1.Query<ECS::Read<Components::MeshRenderer>>().Count(), 1u)
        << "the switched-off renderer is not visited";

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("MeshRenderer.enabled = false"), std::string::npos) << saved;
    EXPECT_NE(saved.find("ReflectionProbe.enabled = false"), std::string::npos) << saved;
    EXPECT_EQ(saved.find("MeshRenderer.enabled = true"), std::string::npos)
        << "a component that is on writes no switch line\n" << saved;
    EXPECT_EQ(saved.find("ReflectionProbe.enabled = true"), std::string::npos) << saved;
    EXPECT_EQ(saved.find("ComponentDisabled"), std::string::npos)
        << "the switch is written as the component's own line, never as a component of its own\n" << saved;

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    ExpectComponentsEnabled(w2, "meshOff", false, true);
    ExpectComponentsEnabled(w2, "meshOn", true, true);
    ExpectComponentsEnabled(w2, "probeOff", true, false);
    ExpectComponentsEnabled(w2, "probeOn", true, true);
}

namespace
{
Components::Name MakeName(std::string_view text)
{
    Components::Name name{};
    std::snprintf(name.value, sizeof(name.value), "%.*s", static_cast<int>(text.size()), text.data());
    return name;
}

// Whether a token names one of the types the save machinery treats as scene structure rather
// than as a component of an entity: a subscene or blueprint instance root becomes its own file
// section, and Name is the entity's own label the probes below are found by.
bool IsStructuralToken(std::string_view token)
{
    return token == "SceneSubsceneInstance" || token == "SceneBlueprintInstance" || token == "Name";
}

struct SwitchProbe
{
    std::string Token;
    ECS::ComponentTypeId TypeId = 0;
};

// Adds one entity named after the token, carrying a default component of the type, switched
// off. A type that does not switch through its tag, or is scene structure, adds nothing; one
// that neither its schema nor the component factory default-constructs goes to `notCreatable`.
void AddSwitchedOffProbe(ECS::World& world, std::string_view tokenView, ECS::ComponentTypeId typeId,
                         const Scene::ISceneComponentSchema* schema, std::vector<SwitchProbe>& probes,
                         std::vector<std::string>& notCreatable)
{
    const std::string token(tokenView);
    if (IsStructuralToken(token) || !ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
        return;
    const ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, MakeName(token));
    std::string err;
    const bool created =
        (schema && schema->AddDefault(world, e, &err)) || ECS::ComponentFactory::Create(world, e, typeId);
    if (!created)
    {
        notCreatable.push_back(token);
        world.DestroyEntityImmediate(e);
        return;
    }
    ASSERT_TRUE(world.SetComponentEnabledImmediate(e, typeId, false)) << token;
    probes.push_back({token, typeId});
}
} // namespace

// Every component that switches through its ECS::ComponentDisabled tag
// (ComponentRegistry::SwitchesThroughDisabledTag) keeps a switch-off across save and load,
// whatever writes it: the scene writer emits "Token.enabled = false" for a switched-off component
// of any type and the loader applies it after the block, so no hand-written schema has to know
// about the switch. One entity per registered type, default-constructed through its schema or
// the component factory, all switched off, saved once and loaded once. A type that keeps its own
// Enabled field (a terrain effect) is not in the set: its field is its own line.
TEST(SceneIO, EverySwitchableComponentKeepsItsSwitchOffAcrossSaveAndLoad)
{
    ECS::World w1;
    std::vector<SwitchProbe> probes;
    std::vector<std::string> notCreatable;
    for (const Scene::ISceneComponentSchema* schema : Scene::SceneSchemaRegistry::GetAllSorted())
        AddSwitchedOffProbe(w1, schema->GetComponentName(),
                            Scene::SceneSchemaRegistry::ComponentTypeForSchema(*schema), schema, probes,
                            notCreatable);
    for (const ECS::ComponentTypeId typeId : ECS::ComponentFactory::DefaultByteTypes())
        if (const Scene::ISceneComponentSchema* fallback =
                Scene::SceneSchemaRegistry::ReflectionSchemaForUnhandledType(typeId))
            AddSwitchedOffProbe(w1, fallback->GetComponentName(), typeId, nullptr, probes, notCreatable);
    ASSERT_FALSE(probes.empty());

    // The reviewed red cases and their controls are in the set, so a broken enumeration cannot
    // pass silently; a terrain effect keeps its own field and is not.
    const auto covered = [&](std::string_view token) {
        return std::any_of(probes.begin(), probes.end(), [&](const SwitchProbe& p) { return p.Token == token; });
    };
    for (const char* seed : {"AmbientLight", "LODGroup", "Light", "ReflectionProbe", "MeshRenderer", "PhysicsBody"})
        EXPECT_TRUE(covered(seed)) << seed;
    EXPECT_FALSE(covered("TerrainFlattenEffect"));
    EXPECT_FALSE(ECS::ComponentRegistry::SwitchesThroughDisabledTag(
        ECS::GetComponentTypeId<Components::TerrainFlattenEffect>()));
    for (const std::string& token : notCreatable)
        std::printf("[SceneIO] no schema or factory default-constructs %s; it is not round-tripped here\n",
                    token.c_str());

    const auto scenePath = MakeTempPath("every_switch_off.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{})) << Scene::GetLastSceneIOError().message;
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_EQ(saved.find("ComponentDisabled"), std::string::npos)
        << "the switch is the component's own line, never a component of its own";
    for (const SwitchProbe& p : probes)
        EXPECT_NE(saved.find(p.Token + ".enabled = false"), std::string::npos) << p.Token << " wrote no switch line";

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    std::unordered_map<std::string, ECS::EntityHandle> byName;
    auto names = w2.Query<ECS::Read<Components::Name>>();
    names.IncludeDisabled();
    names.Each([&](ECS::EntityHandle h, const Components::Name& name) { byName[std::string(name.value)] = h; });
    for (const SwitchProbe& p : probes)
    {
        const auto it = byName.find(p.Token);
        ASSERT_NE(it, byName.end()) << p.Token;
        EXPECT_TRUE(w2.HasComponent(it->second, p.TypeId)) << p.Token << " did not load";
        EXPECT_FALSE(w2.IsComponentEnabled(it->second, p.TypeId)) << p.Token << " lost its switch-off";
    }
}

TEST(SceneIO, SaveThenLoad_OceanPresetFieldIdentities)
{
    const auto scenePath = MakeTempPath("ocean_preset_fields.scene");
    ECS::World originalWorld;
    const auto entity = originalWorld.CreateEntity();
    originalWorld.AddComponentImmediate(entity, Components::SceneEntityTag{{'o', 'c', 'e', 'a', 'n', '\0'}});
    Components::OceanPresetBinding binding{};
    binding.Overrides[0].FieldIdentifier = HashStringId("Surface.FoamRoughness");
    binding.Overrides[2].FieldIdentifier = HashStringId("Renderer.GravityMultiplier");
    binding.Overrides[511].FieldIdentifier = HashStringId("Spectrum.WindSpeed");
    originalWorld.AddComponentImmediate(entity, binding);
    ASSERT_TRUE(Scene::SaveSceneToFile(originalWorld, scenePath, Scene::SaveOptions{}));

    ECS::World loadedWorld;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loadedWorld, scenePath,
                                        Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto loadedEntity = FindByTag(loadedWorld, "ocean");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* loaded = loadedWorld.GetComponent<Components::OceanPresetBinding>(loadedEntity);
    ASSERT_NE(loaded, nullptr);
    for (size_t index = 0; index < std::size(binding.Overrides); ++index)
        EXPECT_EQ(loaded->Overrides[index].FieldIdentifier, binding.Overrides[index].FieldIdentifier);
}

// The ocean's shallow clarity window saves and loads as authored values, and a
// scene written without the two lines loads the defaults the shader used as
// literals (8 m of path, floor 0.22).
TEST(SceneIO, SaveThenLoad_OceanShallowClarityWindow)
{
    const auto scenePath = MakeTempPath("ocean_shallow_clarity.scene");
    ECS::World originalWorld;
    const auto entity = originalWorld.CreateEntity();
    originalWorld.AddComponentImmediate(entity, Components::SceneEntityTag{{'o', 'c', 'e', 'a', 'n', '\0'}});
    Components::OceanSurface surface{};
    surface.ShallowClarityDistance = 2.5f;
    surface.ShallowClarityFloor = 0.05f;
    originalWorld.AddComponentImmediate(entity, surface);
    ASSERT_TRUE(Scene::SaveSceneToFile(originalWorld, scenePath, Scene::SaveOptions{}))
        << Scene::GetLastSceneIOError().message;

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("OceanSurface.ShallowClarityDistance"), std::string::npos);
    EXPECT_NE(saved.find("OceanSurface.ShallowClarityFloor"), std::string::npos);

    ECS::World loadedWorld;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loadedWorld, scenePath,
                                        Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    const auto loadedEntity = FindByTag(loadedWorld, "ocean");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* loaded = loadedWorld.GetComponent<Components::OceanSurface>(loadedEntity);
    ASSERT_NE(loaded, nullptr);
    EXPECT_FLOAT_EQ(loaded->ShallowClarityDistance, 2.5f);
    EXPECT_FLOAT_EQ(loaded->ShallowClarityFloor, 0.05f);

    std::istringstream lines(saved);
    std::string withoutWindow;
    for (std::string line; std::getline(lines, line);)
    {
        if (line.find("ShallowClarity") == std::string::npos)
            withoutWindow += line + "\n";
    }
    const auto unauthoredPath = MakeTempPath("ocean_shallow_clarity_unauthored.scene");
    ASSERT_TRUE(WriteFile(unauthoredPath, withoutWindow));
    ECS::World unauthoredWorld;
    ASSERT_TRUE(Scene::LoadSceneFromFile(unauthoredWorld, unauthoredPath,
                                        Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    const auto unauthoredEntity = FindByTag(unauthoredWorld, "ocean");
    ASSERT_TRUE(unauthoredEntity.IsValid());
    const auto* unauthored = unauthoredWorld.GetComponent<Components::OceanSurface>(unauthoredEntity);
    ASSERT_NE(unauthored, nullptr);
    EXPECT_FLOAT_EQ(unauthored->ShallowClarityDistance, 8.0f);
    EXPECT_FLOAT_EQ(unauthored->ShallowClarityFloor, 0.22f);
}

TEST(SceneIO, SaveThenLoad_RoundTripCoreComponents)
{
    const auto scenePath = MakeTempPath("roundtrip.scene");

    ECS::World w1;
    const ECS::EntityHandle root = w1.CreateEntity();
    const ECS::EntityHandle child = w1.CreateEntity();
    ASSERT_TRUE(root.IsValid());
    ASSERT_TRUE(child.IsValid());

    w1.AddComponentImmediate(root, Components::SceneEntityTag{{'r', 'o', 'o', 't', '\0'}});
    w1.AddComponentImmediate(child, Components::SceneEntityTag{{'c', 'h', 'i', 'l', 'd', '\0'}});

    Components::Name rn{};
    std::memset(rn.value, 0, sizeof(rn.value));
    std::memcpy(rn.value, "Root", 4);
    w1.AddComponentImmediate(root, rn);

    Components::Parent cp{};
    cp.parent = root;
    w1.AddComponentImmediate(child, cp);

    Components::Transform rt = Components::Transform::FromTRS(
        Mathematics::Vector3{3, 4, 5},
        Mathematics::Quaternion{},
        Mathematics::Vector3{1, 1, 1});
    w1.AddComponentImmediate(root, rt);

    Scene::SaveOptions so{};
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, so));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    auto r2 = FindByTag(w2, "root");
    auto c2 = FindByTag(w2, "child");
    ASSERT_TRUE(r2.IsValid());
    ASSERT_TRUE(c2.IsValid());

    auto* p2 = w2.GetComponent<Components::Parent>(c2);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p2->parent.id, r2.id);
}

// Earth-scale authoring: a WorldSectorCoord-tagged entity must round-trip through
// scene save/load with the sector ints AND the sector-local floats bit-exact —
// otherwise a planetary-placed mesh reloads at the wrong spot. WorldSectorCoord
// serializes via the reflection schema (no hand-written schema); this pins that
// path. Identity rotation + unit scale keep the Transform decompose/recompose
// exact so the local translation comparison is truly bit-for-bit.
TEST(SceneIO, SaveThenLoad_RoundTripWorldSectorCoord)
{
    const auto scenePath = MakeTempPath("roundtrip_sector.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'t', 'a', 'g', 'g', 'e', 'd', '\0'}});

    const Mathematics::Vector3 local{100.5f, 2.25f, -100.75f};
    w1.AddComponentImmediate(e, Components::Transform::FromTRS(
                                    local, Mathematics::Quaternion{}, Mathematics::Vector3{1, 1, 1}));

    const Components::WorldSectorCoord sector{3595, -1200, 3595};
    w1.AddComponentImmediate(e, sector);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto e2 = FindByTag(w2, "tagged");
    ASSERT_TRUE(e2.IsValid());

    const auto* sc2 = w2.GetComponent<Components::WorldSectorCoord>(e2);
    ASSERT_NE(sc2, nullptr);
    EXPECT_EQ(sc2->x, sector.x);
    EXPECT_EQ(sc2->y, sector.y);
    EXPECT_EQ(sc2->z, sector.z);

    const auto* t2 = w2.GetComponent<Components::Transform>(e2);
    ASSERT_NE(t2, nullptr);
    const Mathematics::Vector3 loadedLocal = t2->GetPosition();
    EXPECT_EQ(loadedLocal.x, local.x);
    EXPECT_EQ(loadedLocal.y, local.y);
    EXPECT_EQ(loadedLocal.z, local.z);
}

namespace
{
// A Transform stores a float matrix: a save decomposes it and a load composes it again. A float
// matrix cannot carry the low bits of a small quaternion component, so scale may settle within an
// ulp and each rotation component within 2^-23 of an earlier save, but must not walk further.
constexpr float kRoundTripScaleUlps = 2.0f;
constexpr float kRoundTripRotationBound = 0x1p-23f;

float UlpOf(float value)
{
    const float magnitude = std::fabs(value);
    return std::nextafter(magnitude, std::numeric_limits<float>::infinity()) - magnitude;
}

void ExpectSameTransformWithinRoundTripBound(const Components::Transform& actual,
                                             const Components::Transform& expected)
{
    const Mathematics::Vector3 actualPosition = actual.GetPosition();
    const Mathematics::Vector3 expectedPosition = expected.GetPosition();
    EXPECT_EQ(actualPosition.x, expectedPosition.x);
    EXPECT_EQ(actualPosition.y, expectedPosition.y);
    EXPECT_EQ(actualPosition.z, expectedPosition.z);

    const Mathematics::Vector3 actualScale = actual.GetScale();
    const Mathematics::Vector3 expectedScale = expected.GetScale();
    EXPECT_NEAR(actualScale.x, expectedScale.x, kRoundTripScaleUlps * UlpOf(expectedScale.x));
    EXPECT_NEAR(actualScale.y, expectedScale.y, kRoundTripScaleUlps * UlpOf(expectedScale.y));
    EXPECT_NEAR(actualScale.z, expectedScale.z, kRoundTripScaleUlps * UlpOf(expectedScale.z));

    // Componentwise, sign included: q and -q are the same rotation, but a save that flips the sign
    // rewrites every rotation line.
    const auto& a = actual.GetRotation().GetGLM();
    const auto& e = expected.GetRotation().GetGLM();
    EXPECT_NEAR(a.x, e.x, kRoundTripRotationBound);
    EXPECT_NEAR(a.y, e.y, kRoundTripRotationBound);
    EXPECT_NEAR(a.z, e.z, kRoundTripRotationBound);
    EXPECT_NEAR(a.w, e.w, kRoundTripRotationBound);
}
} // namespace

// Saving a loaded scene again must reproduce the first save. Both poses have a non-uniform scale. The
// first has a rotation near a half-turn, where a decomposition that is not the inverse of the
// composition moves the scale furthest from one cycle to the next. The second leaves the bound when
// only the composition evaluates in double and the decomposition stays in float.
TEST(SceneIO, SaveThenLoad_RepeatedRoundTripsKeepNonUnitScaleAndRotation)
{
    struct Pose
    {
        Mathematics::Vector3 position;
        Mathematics::Quaternion rotation; // (w, x, y, z)
        Mathematics::Vector3 scale;
    };
    const Pose poses[] = {
        {{-99.48147f, -50.564533f, -21.628498f},
         {-0.50013775f, -0.4000374f, -0.5477298f, 0.5383534f},
         {1.383391f, 17.767937f, 13.4303055f}},
        {{77.1688614f, 8.67422867f, -90.5138245f},
         {0.267857671f, -0.593588769f, 0.57331115f, -0.497211158f},
         {5.69263887f, 19.9550362f, 7.46109962f}},
    };

    for (size_t poseIndex = 0; poseIndex < std::size(poses); ++poseIndex)
    {
        SCOPED_TRACE(testing::Message() << "pose " << poseIndex);
        const Pose& pose = poses[poseIndex];
        const auto scenePath = MakeTempPath("roundtrip_transform.scene");
        {
            ECS::World authored;
            const ECS::EntityHandle e = authored.CreateEntity();
            ASSERT_TRUE(e.IsValid());
            authored.AddComponentImmediate(e, Components::SceneEntityTag{{'p', 'o', 's', 'e', 'd', '\0'}});
            authored.AddComponentImmediate(
                e, Components::Transform::FromTRS(pose.position, pose.rotation, pose.scale));
            ASSERT_TRUE(Scene::SaveSceneToFile(authored, scenePath, Scene::SaveOptions{}));
        }

        constexpr int kTrips = 100;
        Components::Transform firstLoad{};
        for (int trip = 1; trip <= kTrips; ++trip)
        {
            SCOPED_TRACE(trip);
            ECS::World world;
            ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
            const auto e = FindByTag(world, "posed");
            ASSERT_TRUE(e.IsValid());
            const auto* loaded = world.GetComponent<Components::Transform>(e);
            ASSERT_NE(loaded, nullptr);
            if (trip == 1)
                firstLoad = *loaded;
            else
                ExpectSameTransformWithinRoundTripBound(*loaded, firstLoad);
            ASSERT_TRUE(Scene::SaveSceneToFile(world, scenePath, Scene::SaveOptions{}));
        }
    }
}

// An authored rotation that is not unit length is a rotation by q / |q|: its norm must not leak into
// the scale. The quaternion below is the unit quaternion (-0.4000374, -0.5477298, 0.5383534,
// -0.50013775) scaled by 1.0513.
TEST(SceneIO, Load_NonUnitQuaternionRotatesWithoutScaling)
{
    const auto scenePath = MakeTempPath("non_unit_quaternion.scene");
    const std::string src =
        "[scene name=\"NonUnitQuaternion\" version=1]\n"
        "\n"
        "[entity id=\"posed\"]\n"
        "Transform.position = (-99.48147, -50.564533, -21.628498)\n"
        "Transform.rotation = (-0.42055935, -0.5758284, 0.56597096, -0.52579486)\n"
        "Transform.scale = (1.383391, 17.767937, 13.4303055)\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto e = FindByTag(world, "posed");
    ASSERT_TRUE(e.IsValid());
    const auto* loaded = world.GetComponent<Components::Transform>(e);
    ASSERT_NE(loaded, nullptr);

    const Components::Transform unit = Components::Transform::FromTRS(
        Mathematics::Vector3{-99.48147f, -50.564533f, -21.628498f},
        Mathematics::Quaternion{-0.50013775f, -0.4000374f, -0.5477298f, 0.5383534f},
        Mathematics::Vector3{1.383391f, 17.767937f, 13.4303055f});
    ExpectSameTransformWithinRoundTripBound(*loaded, unit);
}

namespace
{
// A distinct, non-null ModelRef per ordinal for SplinePlacement pool contents.
Components::ModelRef PoolTestRef(std::uint8_t ordinal)
{
    GUID::Data data{};
    data[0] = ordinal;
    data[15] = 0x5A;
    return Components::ModelRef(GUID(data));
}

// The same, for the recipes' single (non-array) override-material slot.
Components::MaterialRef MaterialTestRef(std::uint8_t ordinal)
{
    GUID::Data data{};
    data[0] = ordinal;
    data[15] = 0x6B;
    return Components::MaterialRef(GUID(data));
}
} // namespace

// SplinePlacement is the first component whose recipe carries AssetGuid ARRAY
// fields (the piece pools). The reflection schema serializes them as one
// "<Name><index>" line per non-null element; this pins that a fully non-default
// recipe — pools included — survives a real file round-trip byte-exact.
TEST(SceneIO, SaveThenLoad_RoundTripSplinePlacementPools)
{
    const auto scenePath = MakeTempPath("roundtrip_spline_placement.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'r', 'e', 'c', 'i', 'p', 'e', '\0'}});

    Components::SplinePlacement authored{};
    authored.StraightPool[0] = PoolTestRef(1);
    authored.StraightPool[1] = PoolTestRef(2);
    authored.StraightPool[2] = PoolTestRef(3);
    authored.CurvePool[0] = PoolTestRef(4);
    authored.ScatterPool[0] = PoolTestRef(5);
    authored.ScatterPool[1] = PoolTestRef(6);
    authored.Spacing = 3.25f;
    authored.Fit = Components::SplinePlacementFit::FixedPitch;
    authored.ConformMode = Components::SplinePlacementConform::Height;
    authored.LateralOffset = -0.75f;
    authored.SlopeBlend = 0.25f;
    authored.MaxTiltDegrees = 52.5f;
    authored.Seed = 77u;
    authored.SeamShearMaxDegrees = 4.5f;
    // Every per-station variation knob carries a non-default here. The
    // memberwise compare below only catches a field that fails to serialize if
    // that field DIFFERS from its default — otherwise a recipe would reload as
    // an evenly-spaced strip and this test would still be green.
    authored.ConformTarget = Components::SplineConformTarget::TerrainOnly;
    authored.PlantMode = Components::SplinePlantMode::PivotPlane;
    authored.SpacingJitterMetres = 0.45f;
    authored.YawJitterDegrees = 6.5f;
    authored.LateralJitterMetres = 0.15f;
    authored.DropoutChance = 0.125f;
    authored.EndTaperMetres = 3.5f;
    authored.OverrideMaterial = MaterialTestRef(9);
    w1.AddComponentImmediate(e, authored);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto e2 = FindByTag(w2, "recipe");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::SplinePlacement>(e2);
    ASSERT_NE(loaded, nullptr);

    // Targeted slot checks first for a readable failure...
    for (std::uint32_t i = 0; i < Components::kSplinePoolCapacity; ++i)
    {
        EXPECT_EQ(loaded->StraightPool[i], authored.StraightPool[i]) << "StraightPool[" << i << "]";
        EXPECT_EQ(loaded->CurvePool[i], authored.CurvePool[i]) << "CurvePool[" << i << "]";
        EXPECT_EQ(loaded->ScatterPool[i], authored.ScatterPool[i]) << "ScatterPool[" << i << "]";
    }
    EXPECT_EQ(loaded->OverrideMaterial, authored.OverrideMaterial);
    EXPECT_EQ(loaded->PlantMode, authored.PlantMode)
        << "a path authored pivot-planted loads back base-anchored, which lifts every tile by its "
           "pivot depth — the whole slab thickness on the measured kits";
    // ...then the whole recipe (memberwise ==), which also covers every scalar.
    EXPECT_TRUE(*loaded == authored);
}

// The load-path half of "the default is the old behaviour": every scene authored
// before PlantMode existed has no PlantMode line at all, and must come back
// base-anchored. A default flipped in the header would re-bed every authored path
// in the project on next open, silently and with no diff to point at.
//
// The absence is produced rather than assumed: the field is authored non-default,
// saved, and its line then deleted from the file — so a serializer that stopped
// writing the key would fail the ASSERT below rather than make this test vacuous.
TEST(SceneIO, Load_SplinePlacementWithoutPlantModeKeyStaysBaseAnchored)
{
    const auto scenePath = MakeTempPath("spline_placement_plantmode_absent.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'p', 'l', 'n', 't', '\0'}});

    Components::SplinePlacement authored{};
    authored.StraightPool[0] = PoolTestRef(1);
    authored.Spacing = 3.25f;
    authored.Seed = 77u;
    authored.PlantMode = Components::SplinePlantMode::PivotPlane;
    w1.AddComponentImmediate(e, authored);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string text;
    {
        std::ifstream in(scenePath, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // Matched by KEY, not by the whole line: reflected enums serialize by
    // enumerator name ("SplinePlacement.Fit = FitToLength"), and pinning the
    // spelling of the value here would make this test fail for the wrong reason
    // the day an enumerator is renamed.
    const std::string plantKey = "SplinePlacement.PlantMode";
    const auto pos = text.find(plantKey);
    ASSERT_NE(pos, std::string::npos)
        << "PlantMode does not serialize at all, so this test cannot tell an absent key from an "
           "unwritten one; the file reads:\n"
        << text;

    const auto lineEnd = text.find('\n', pos);
    text.erase(pos, lineEnd == std::string::npos ? std::string::npos : lineEnd - pos);
    {
        std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
        out << text;
    }

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto e2 = FindByTag(w2, "plnt");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::SplinePlacement>(e2);
    ASSERT_NE(loaded, nullptr);

    EXPECT_EQ(loaded->PlantMode, Components::SplinePlantMode::BoundsMin)
        << "a pre-PlantMode scene loads pivot-planted, so every authored path in the project sinks "
           "by its pivot depth the moment it is opened";
    // The rest of the recipe is untouched by the deletion: this pins that the
    // absent key resolves to the default, not that the load bailed out early.
    EXPECT_EQ(loaded->StraightPool[0], authored.StraightPool[0]);
    EXPECT_FLOAT_EQ(loaded->Spacing, authored.Spacing);
    EXPECT_EQ(loaded->Seed, authored.Seed);
}

// A hole (valid, empty, valid) must round-trip slot-exact: null elements omit
// their line and load back as empty, so the serializer preserves the authored
// layout — the ACTIVE-prefix rule that ignores post-hole entries lives in
// selection/validation, never in the file format.
TEST(SceneIO, SaveThenLoad_SplinePlacementPoolHoleSurvives)
{
    const auto scenePath = MakeTempPath("roundtrip_spline_placement_hole.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'h', 'o', 'l', 'e', 'y', '\0'}});

    Components::SplinePlacement authored{};
    authored.StraightPool[0] = PoolTestRef(1);
    // StraightPool[1] deliberately empty.
    authored.StraightPool[2] = PoolTestRef(3);
    w1.AddComponentImmediate(e, authored);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto e2 = FindByTag(w2, "holey");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::SplinePlacement>(e2);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->StraightPool[0], authored.StraightPool[0]);
    EXPECT_TRUE(loaded->StraightPool[1].IsNull());
    EXPECT_EQ(loaded->StraightPool[2], authored.StraightPool[2]);
}

// Element-key dialect of the AssetGuid-array reflection path: a valid
// "<name><index>" key applies to that slot; an out-of-range index and the
// retired pre-pool "MeshGuid" key are tolerated skips (warn + keep defaults),
// never load failures — the migration behavior a pre-P1 scene gets.
TEST(SceneIO, ReflectionSchema_SplinePlacementElementKeys)
{
    const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("SplinePlacement");
    ASSERT_NE(typeId, 0u);
    const Scene::ReflectionSceneSchema schema(typeId, "SplinePlacement");

    ECS::World w;
    const ECS::EntityHandle e = w.CreateEntity();
    ASSERT_TRUE(e.IsValid());

    Scene::SceneLoadContext loadCtx{};
    std::string err;

    // Valid element key (parser-lowercased form) applies to slot 1.
    ASSERT_TRUE(schema.ApplyProperty(
        w, e, loadCtx, "straightpool1",
        "[guid=\"000000aa-0000-0000-0000-000000000001\"]", &err)) << err;
    const auto* comp = w.GetComponent<Components::SplinePlacement>(e);
    ASSERT_NE(comp, nullptr);
    EXPECT_TRUE(comp->StraightPool[0].IsNull());
    EXPECT_FALSE(comp->StraightPool[1].IsNull());

    // Out-of-range index: tolerated skip, nothing written.
    ASSERT_TRUE(schema.ApplyProperty(
        w, e, loadCtx, "straightpool9",
        "[guid=\"000000aa-0000-0000-0000-000000000002\"]", &err)) << err;
    comp = w.GetComponent<Components::SplinePlacement>(e);
    ASSERT_NE(comp, nullptr);
    for (std::uint32_t i = 0; i < Components::kSplinePoolCapacity; ++i)
    {
        if (i != 1u)
            EXPECT_TRUE(comp->StraightPool[i].IsNull()) << "slot " << i;
    }

    // The pre-pool single-slot key from a pre-P1 scene: tolerated skip.
    ASSERT_TRUE(schema.ApplyProperty(
        w, e, loadCtx, "meshguid",
        "[guid=\"000000aa-0000-0000-0000-000000000003\"]", &err)) << err;
}

// SplineFence carries five pools, the span-override table AND the enum fields
// the fence recipe needs. Every authored value below differs from its default,
// so a field that failed to serialize would load back as its default and fail
// the comparison.
TEST(SceneIO, SaveThenLoad_RoundTripSplineFence)
{
    const auto scenePath = MakeTempPath("roundtrip_spline_fence.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'f', 'e', 'n', 'c', 'e', '\0'}});

    Components::SplineFence authored{};
    authored.PostPool[0] = PoolTestRef(11);
    authored.PostPool[1] = PoolTestRef(12);
    authored.SpanPool[0] = PoolTestRef(21);
    authored.SpanPool[1] = PoolTestRef(22);
    authored.SpanPool[Components::kSplineFencePoolCapacity - 1] = PoolTestRef(27);
    authored.GatePool[0] = PoolTestRef(31);
    authored.CrestPool[0] = PoolTestRef(41);
    authored.CrestPool[1] = PoolTestRef(42);
    authored.CrestPool[Components::kSplineFencePoolCapacity - 1] = PoolTestRef(47);
    authored.CapPool[0] = PoolTestRef(51);
    authored.CapPool[1] = PoolTestRef(52);
    authored.PostPitch = 3.125f;
    authored.CrestPitch = 2.75f;
    authored.ConformMode = Components::SplinePlacementConform::Height;
    authored.ConformTarget = Components::SplineConformTarget::TerrainOnly;
    authored.SlopeBlend = 0.375f;
    authored.SpanMaxStretch = 1.5f;
    authored.PlantMode = Components::SplinePlantMode::BoundsMin;
    authored.SpanGrade = Components::SplineSpanGrade::Stepped;
    authored.Seed = 4242u;
    authored.OverrideMaterial = MaterialTestRef(19);
    // A gate and a pinned piece, in slots that are not the first, with an empty
    // slot between them: the table must come back slot-exact.
    authored.Overrides[1] = {3u, 2u, Components::SplineSpanOverrideKind::Gate, 1u};
    authored.Overrides[Components::kSplineFenceMaxSpanOverrides - 1u] = {
        7u, 0u, Components::SplineSpanOverrideKind::ExplicitPiece, 4u};
    w1.AddComponentImmediate(e, authored);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto e2 = FindByTag(w2, "fence");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::SplineFence>(e2);
    ASSERT_NE(loaded, nullptr);

    // Targeted checks first for a readable failure...
    for (std::uint32_t i = 0; i < Components::kSplineFencePoolCapacity; ++i)
    {
        EXPECT_EQ(loaded->PostPool[i], authored.PostPool[i]) << "PostPool[" << i << "]";
        EXPECT_EQ(loaded->SpanPool[i], authored.SpanPool[i]) << "SpanPool[" << i << "]";
        EXPECT_EQ(loaded->GatePool[i], authored.GatePool[i]) << "GatePool[" << i << "]";
        EXPECT_EQ(loaded->CrestPool[i], authored.CrestPool[i]) << "CrestPool[" << i << "]";
        EXPECT_EQ(loaded->CapPool[i], authored.CapPool[i]) << "CapPool[" << i << "]";
    }
    EXPECT_FLOAT_EQ(loaded->PostPitch, authored.PostPitch);
    EXPECT_FLOAT_EQ(loaded->CrestPitch, authored.CrestPitch);
    EXPECT_FLOAT_EQ(loaded->SlopeBlend, authored.SlopeBlend);
    EXPECT_FLOAT_EQ(loaded->SpanMaxStretch, authored.SpanMaxStretch);
    EXPECT_EQ(loaded->ConformMode, authored.ConformMode);
    EXPECT_EQ(loaded->ConformTarget, authored.ConformTarget)
        << "a fence authored terrain-only loads back conforming to the whole scene, so the "
           "stations would re-place onto whatever scenery overhangs the run";
    EXPECT_EQ(loaded->PlantMode, authored.PlantMode);
    EXPECT_EQ(loaded->SpanGrade, authored.SpanGrade);
    EXPECT_EQ(loaded->Seed, authored.Seed);
    EXPECT_EQ(loaded->OverrideMaterial, authored.OverrideMaterial);
    for (std::uint32_t i = 0; i < Components::kSplineFenceMaxSpanOverrides; ++i)
        EXPECT_EQ(loaded->Overrides[i], authored.Overrides[i]) << "Overrides[" << i << "]";
    // ...then the whole recipe (memberwise ==).
    EXPECT_TRUE(*loaded == authored);
}

// Every span grade round-trips, not just the one the test above happens to
// author. The name table an enum field serializes through is emitted by the
// component scanner off the header, so an enumerator it failed to pick up would
// not fail to build or to save — the scene would write a name the loader cannot
// match and the fence would come back at the DEFAULT grade, which is a
// different fence in a screenshot and nothing anywhere else.
TEST(SceneIO, SaveThenLoad_RoundTripEverySpanGrade)
{
    for (const Components::SplineSpanGrade grade :
         {Components::SplineSpanGrade::Racked, Components::SplineSpanGrade::Stepped,
          Components::SplineSpanGrade::Sheared})
    {
        const auto scenePath = MakeTempPath("roundtrip_span_grade.scene");

        ECS::World w1;
        const ECS::EntityHandle e = w1.CreateEntity();
        ASSERT_TRUE(e.IsValid());
        w1.AddComponentImmediate(e, Components::SceneEntityTag{{'f', 'e', 'n', 'c', 'e', '\0'}});
        Components::SplineFence authored{};
        authored.SpanGrade = grade;
        w1.AddComponentImmediate(e, authored);

        ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

        ECS::World w2;
        ASSERT_TRUE(
            Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
        const auto e2 = FindByTag(w2, "fence");
        ASSERT_TRUE(e2.IsValid());
        const auto* loaded = w2.GetComponent<Components::SplineFence>(e2);
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->SpanGrade, grade)
            << "grade " << static_cast<int>(grade) << " did not survive the scene file";
    }
}

// SplineExtrude's ring shape answers to three authored controls beyond the
// profile — which source sizes a ring (WidthMode), how far the measured search
// may reach (MaxHalfWidth), and how its open ends stop (EndTaperMetres) — and a
// recipe that renders from a measurement is only reproducible if all of them
// survive the file. Every value below differs from its default, so a field that
// failed to serialize would load back as its default and fail here rather than
// silently reverting a scene to channel widths and square ends on next open.
TEST(SceneIO, SaveThenLoad_RoundTripSplineExtrude)
{
    const auto scenePath = MakeTempPath("roundtrip_spline_extrude.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'e', 'x', 't', 'r', '\0'}});

    Components::SplineExtrude authored{};
    authored.Profile = Components::SplineExtrudeProfile::Crown;
    authored.Width = 4.5f;
    authored.EdgeDrop = 0.4f;
    authored.EdgeInset = 0.55f;
    authored.CrownRise = 0.31f;
    authored.ShoulderWidth = 1.4f;
    authored.ShoulderDrop = 0.62f;
    authored.WidthScale = Components::SplineExtrudeWidthScale::Uniform;
    authored.WidthMode = Components::SplineExtrudeWidthMode::FitToBanks;
    authored.MaxHalfWidth = 21.5f;
    authored.SeaLevelFloor = -12.5f;
    authored.EndTaperMetres = 6.25f;
    authored.ConformMode = Components::SplinePlacementConform::None;
    authored.ConformTarget = Components::SplineConformTarget::TerrainOnly;
    authored.LateralOffset = -1.75f;
    authored.VerticalOffset = 0.08f;
    authored.TilesPerMetreU = 0.35f;
    authored.TilesPerMetreV = 2.5f;
    authored.Material = MaterialTestRef(19);
    authored.CastShadows = false;
    authored.ReceiveShadows = false;
    w1.AddComponentImmediate(e, authored);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto e2 = FindByTag(w2, "extr");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::SplineExtrude>(e2);
    ASSERT_NE(loaded, nullptr);

    // The ring-shape controls first, so a readable failure names the field
    // rather than pointing at a memberwise ==.
    EXPECT_EQ(loaded->WidthMode, authored.WidthMode)
        << "WidthMode must survive, or a fitted river silently reverts to channel widths";
    EXPECT_FLOAT_EQ(loaded->MaxHalfWidth, authored.MaxHalfWidth);
    EXPECT_FLOAT_EQ(loaded->EndTaperMetres, authored.EndTaperMetres)
        << "EndTaperMetres must survive, or a tapered run reloads with square ends";

    EXPECT_EQ(loaded->Profile, authored.Profile);
    EXPECT_EQ(loaded->WidthScale, authored.WidthScale);
    EXPECT_EQ(loaded->ConformMode, authored.ConformMode);
    EXPECT_EQ(loaded->ConformTarget, authored.ConformTarget);
    EXPECT_FLOAT_EQ(loaded->Width, authored.Width);
    EXPECT_FLOAT_EQ(loaded->EdgeDrop, authored.EdgeDrop);
    EXPECT_FLOAT_EQ(loaded->EdgeInset, authored.EdgeInset);
    EXPECT_FLOAT_EQ(loaded->CrownRise, authored.CrownRise);
    EXPECT_FLOAT_EQ(loaded->ShoulderWidth, authored.ShoulderWidth);
    EXPECT_FLOAT_EQ(loaded->ShoulderDrop, authored.ShoulderDrop);
    EXPECT_FLOAT_EQ(loaded->LateralOffset, authored.LateralOffset);
    EXPECT_FLOAT_EQ(loaded->VerticalOffset, authored.VerticalOffset);
    EXPECT_FLOAT_EQ(loaded->TilesPerMetreU, authored.TilesPerMetreU);
    EXPECT_FLOAT_EQ(loaded->TilesPerMetreV, authored.TilesPerMetreV);
    EXPECT_EQ(loaded->Material, authored.Material);
    EXPECT_FALSE(loaded->CastShadows);
    EXPECT_FALSE(loaded->ReceiveShadows);
    // ...then the whole recipe (memberwise ==).
    EXPECT_TRUE(*loaded == authored);
}

// Every SplineWall field survives the scene file. Each value differs from its
// default, so a field that failed to serialize would load back as its default
// and fail here rather than silently rebuilding a different wall on next open.
TEST(SceneIO, SaveThenLoad_RoundTripSplineWall)
{
    const auto scenePath = MakeTempPath("roundtrip_spline_wall.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'w', 'a', 'l', 'l', '\0'}});

    Components::SplineWall authored{};
    authored.Thickness = 1.35f;
    authored.Height = 4.75f;
    authored.Grade = Components::SplineWallGrade::Stepped;
    authored.Corner = Components::SplineWallCorner::Round;
    authored.Material = MaterialTestRef(23);
    authored.ConformMode = Components::SplinePlacementConform::None;
    authored.ConformTarget = Components::SplineConformTarget::TerrainOnly;
    authored.CastShadows = false;
    authored.ReceiveShadows = false;
    w1.AddComponentImmediate(e, authored);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto e2 = FindByTag(w2, "wall");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::SplineWall>(e2);
    ASSERT_NE(loaded, nullptr);
    EXPECT_FLOAT_EQ(loaded->Thickness, authored.Thickness);
    EXPECT_FLOAT_EQ(loaded->Height, authored.Height);
    EXPECT_EQ(loaded->Grade, authored.Grade);
    EXPECT_EQ(loaded->Corner, authored.Corner);
    EXPECT_EQ(loaded->Material, authored.Material);
    EXPECT_EQ(loaded->ConformMode, authored.ConformMode);
    EXPECT_EQ(loaded->ConformTarget, authored.ConformTarget);
    EXPECT_FALSE(loaded->CastShadows);
    EXPECT_FALSE(loaded->ReceiveShadows);
    EXPECT_TRUE(*loaded == authored);
}

// The override material is a SINGLE (non-array) AssetRef, unlike the pools —
// a different serializer path, so it gets its own pin at both ends of its
// domain. Unset is the load-bearing half: an empty GUID must come back empty
// rather than being written as a line the resolver then heals into some
// arbitrary asset, which would retexture every recipe that set no override.
TEST(SceneIO, SaveThenLoad_SplineRecipeOverrideMaterialSetAndUnset)
{
    const auto scenePath = MakeTempPath("roundtrip_spline_override_material.scene");

    ECS::World w1;
    const ECS::EntityHandle set = w1.CreateEntity();
    ASSERT_TRUE(set.IsValid());
    w1.AddComponentImmediate(set, Components::SceneEntityTag{{'s', 'e', 't', '\0'}});
    Components::SplinePlacement placementSet{};
    placementSet.StraightPool[0] = PoolTestRef(1);
    placementSet.OverrideMaterial = MaterialTestRef(41);
    w1.AddComponentImmediate(set, placementSet);
    Components::SplineFence fenceSet{};
    fenceSet.SpanPool[0] = PoolTestRef(2);
    fenceSet.OverrideMaterial = MaterialTestRef(42);
    w1.AddComponentImmediate(set, fenceSet);

    const ECS::EntityHandle unset = w1.CreateEntity();
    ASSERT_TRUE(unset.IsValid());
    w1.AddComponentImmediate(unset, Components::SceneEntityTag{{'u', 'n', 's', 'e', 't', '\0'}});
    Components::SplinePlacement placementUnset{};
    placementUnset.StraightPool[0] = PoolTestRef(3);
    w1.AddComponentImmediate(unset, placementUnset);
    Components::SplineFence fenceUnset{};
    fenceUnset.SpanPool[0] = PoolTestRef(4);
    w1.AddComponentImmediate(unset, fenceUnset);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto setLoaded = FindByTag(w2, "set");
    ASSERT_TRUE(setLoaded.IsValid());
    const auto* placementBack = w2.GetComponent<Components::SplinePlacement>(setLoaded);
    ASSERT_NE(placementBack, nullptr);
    EXPECT_EQ(placementBack->OverrideMaterial, MaterialTestRef(41));
    const auto* fenceBack = w2.GetComponent<Components::SplineFence>(setLoaded);
    ASSERT_NE(fenceBack, nullptr);
    EXPECT_EQ(fenceBack->OverrideMaterial, MaterialTestRef(42));

    const auto unsetLoaded = FindByTag(w2, "unset");
    ASSERT_TRUE(unsetLoaded.IsValid());
    const auto* placementNone = w2.GetComponent<Components::SplinePlacement>(unsetLoaded);
    ASSERT_NE(placementNone, nullptr);
    EXPECT_TRUE(placementNone->OverrideMaterial.IsNull());
    const auto* fenceNone = w2.GetComponent<Components::SplineFence>(unsetLoaded);
    ASSERT_NE(fenceNone, nullptr);
    EXPECT_TRUE(fenceNone->OverrideMaterial.IsNull());
}

// A gate is written as one line per member of its override slot, and an
// untouched table writes nothing: a fence with no override saves exactly the
// lines it saved before the table serialized.
TEST(SceneIO, SaveThenLoad_SplineFenceOverrideLinesAndUntouchedTable)
{
    const auto scenePath = MakeTempPath("roundtrip_spline_fence_overrides.scene");

    ECS::World w1;
    const ECS::EntityHandle gated = w1.CreateEntity();
    ASSERT_TRUE(gated.IsValid());
    w1.AddComponentImmediate(gated, Components::SceneEntityTag{{'o', 'v', 'r', '\0'}});
    Components::SplineFence withGate{};
    withGate.SpanPool[0] = PoolTestRef(51);
    withGate.Overrides[0] = {2u, 1u, Components::SplineSpanOverrideKind::Gate, 0u};
    w1.AddComponentImmediate(gated, withGate);

    const ECS::EntityHandle plain = w1.CreateEntity();
    ASSERT_TRUE(plain.IsValid());
    w1.AddComponentImmediate(plain, Components::SceneEntityTag{{'p', 'l', 'n', '\0'}});
    Components::SplineFence withoutOverrides{};
    withoutOverrides.SpanPool[0] = PoolTestRef(52);
    w1.AddComponentImmediate(plain, withoutOverrides);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::ifstream file(scenePath);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("SplineFence.Overrides0.PointIndex = 2"), std::string::npos) << text;
    EXPECT_NE(text.find("SplineFence.Overrides0.SpanOrdinal = 1"), std::string::npos) << text;
    EXPECT_NE(text.find("SplineFence.Overrides0.Kind = Gate"), std::string::npos) << text;
    EXPECT_NE(text.find("SplineFence.Overrides0.PoolSlot = 0"), std::string::npos) << text;
    std::size_t overrideLines = 0;
    for (std::size_t at = text.find("SplineFence.Overrides"); at != std::string::npos;
         at = text.find("SplineFence.Overrides", at + 1))
        ++overrideLines;
    EXPECT_EQ(overrideLines, 4u) << "only the gated fence's one filled slot writes lines:\n" << text;

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto* loadedGate = w2.GetComponent<Components::SplineFence>(FindByTag(w2, "ovr"));
    const auto* loadedPlain = w2.GetComponent<Components::SplineFence>(FindByTag(w2, "pln"));
    ASSERT_NE(loadedGate, nullptr);
    ASSERT_NE(loadedPlain, nullptr);
    EXPECT_TRUE(*loadedGate == withGate);
    EXPECT_TRUE(*loadedPlain == withoutOverrides);
}

// A pool hole must round-trip slot-exact for the fence too: null elements omit
// their line and load back empty, so the file preserves the authored layout and
// the ACTIVE-prefix rule stays in selection, never in the format.
TEST(SceneIO, SaveThenLoad_SplineFencePoolHoleSurvives)
{
    const auto scenePath = MakeTempPath("roundtrip_spline_fence_hole.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'h', 'o', 'l', 'e', 'd', '\0'}});

    Components::SplineFence authored{};
    authored.SpanPool[0] = PoolTestRef(41);
    // SpanPool[1] deliberately empty.
    authored.SpanPool[2] = PoolTestRef(43);
    w1.AddComponentImmediate(e, authored);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto e2 = FindByTag(w2, "holed");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::SplineFence>(e2);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->SpanPool[0], authored.SpanPool[0]);
    EXPECT_TRUE(loaded->SpanPool[1].IsNull());
    EXPECT_EQ(loaded->SpanPool[2], authored.SpanPool[2]);
}

namespace
{
Components::SceneEntityTag MakeSceneTag(const char* value)
{
    Components::SceneEntityTag tag{};
    std::memset(tag.value, 0, sizeof(tag.value));
    std::strncpy(tag.value, value, sizeof(tag.value) - 1);
    return tag;
}
} // namespace

// A RuntimeOnlyEntity-tagged entity (e.g. an HLOD proxy) is engine-created runtime state:
// SaveSceneToFile must drop the whole entity, mirroring the blueprint-instance exclusion,
// while untagged entities round-trip untouched.
TEST(SceneIO, SaveThenLoad_RuntimeOnlyEntityExcludedFromScene)
{
    const auto scenePath = MakeTempPath("runtime_only_excluded.scene");

    ECS::World w1;
    const ECS::EntityHandle keeper = w1.CreateEntity();
    const ECS::EntityHandle proxy = w1.CreateEntity();
    ASSERT_TRUE(keeper.IsValid());
    ASSERT_TRUE(proxy.IsValid());

    w1.AddComponentImmediate(keeper, MakeSceneTag("keeper"));
    w1.AddComponentImmediate(keeper, Components::Transform::FromTRS(
        Mathematics::Vector3{1, 2, 3}, Mathematics::Quaternion{}, Mathematics::Vector3{1, 1, 1}));

    w1.AddComponentImmediate(proxy, MakeSceneTag("hlod_proxy"));
    w1.AddComponentImmediate(proxy, Components::RuntimeOnlyEntity{});
    w1.AddComponentImmediate(proxy, Components::Transform::FromTRS(
        Mathematics::Vector3{9, 9, 9}, Mathematics::Quaternion{}, Mathematics::Vector3{1, 1, 1}));

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    // The untagged (normal) entity survives the round-trip intact...
    const ECS::EntityHandle keeper2 = FindByTag(w2, "keeper");
    ASSERT_TRUE(keeper2.IsValid());
    // ...and the runtime-only entity is entirely absent from the reloaded scene.
    EXPECT_FALSE(FindByTag(w2, "hlod_proxy").IsValid());

    int runtimeOnlyCount = 0;
    for (auto* arch : w2.GetAllArchetypes())
    {
        if (!arch)
            continue;
        for (const auto& h : arch->CollectEntities())
        {
            if (h.IsValid() && w2.IsValid(h) && w2.HasComponent<Components::RuntimeOnlyEntity>(h))
                ++runtimeOnlyCount;
        }
    }
    EXPECT_EQ(runtimeOnlyCount, 0);
}

// The runtime-only entity's scene id and the RuntimeOnlyEntity component name must never
// appear in the serialized text, while the normal entity's id must.
TEST(SceneIO, Save_RuntimeOnlyEntityAndTagAbsentFromOutput)
{
    const auto scenePath = MakeTempPath("runtime_only_output.scene");

    ECS::World w1;
    const ECS::EntityHandle keeper = w1.CreateEntity();
    const ECS::EntityHandle proxy = w1.CreateEntity();
    ASSERT_TRUE(keeper.IsValid());
    ASSERT_TRUE(proxy.IsValid());

    w1.AddComponentImmediate(keeper, MakeSceneTag("keeper"));
    w1.AddComponentImmediate(proxy, MakeSceneTag("hlod_proxy"));
    w1.AddComponentImmediate(proxy, Components::RuntimeOnlyEntity{});

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string text;
    ASSERT_TRUE(ReadFile(scenePath, text));

    EXPECT_NE(text.find("keeper"), std::string::npos);
    EXPECT_EQ(text.find("hlod_proxy"), std::string::npos);
    EXPECT_EQ(text.find("RuntimeOnlyEntity"), std::string::npos);
}

// Duplicating a tagged entity keeps the tag, so a cloned proxy stays runtime-only (and
// stays out of saves). CloneEntity is the primitive under editor duplicate/undo.
TEST(SceneIO, CloneEntity_KeepsRuntimeOnlyTag)
{
    ECS::World world;
    const ECS::EntityHandle src = world.CreateEntity();
    ASSERT_TRUE(src.IsValid());
    world.AddComponentImmediate(src, Components::RuntimeOnlyEntity{});

    const ECS::EntityHandle clone = world.CloneEntity(src);
    ASSERT_TRUE(clone.IsValid());
    EXPECT_NE(clone.id, src.id);
    EXPECT_TRUE(world.HasComponent<Components::RuntimeOnlyEntity>(clone));
}

namespace
{
// A placer that IS saved, carrying runtime-only children that are not — the shape
// the spline placement and fence controllers build (recipe entity + generated
// pieces parented under it).
struct PlacerWithGeneratedChildren
{
    ECS::EntityHandle placer{};
    std::vector<ECS::EntityHandle> pieces;
};

PlacerWithGeneratedChildren MakePlacerWithGeneratedChildren(ECS::World& world, int pieceCount)
{
    PlacerWithGeneratedChildren out;
    out.placer = world.CreateEntity();
    world.AddComponentImmediate(out.placer, MakeSceneTag("placer"));
    world.AddComponentImmediate(out.placer, Components::Transform::FromTRS(
        Mathematics::Vector3{4, 0, 6}, Mathematics::Quaternion{}, Mathematics::Vector3{1, 1, 1}));

    for (int i = 0; i < pieceCount; ++i)
    {
        const ECS::EntityHandle piece = world.CreateEntity();
        world.AddComponentImmediate(piece, MakeSceneTag(("piece_" + std::to_string(i)).c_str()));
        world.AddComponentImmediate(piece, Components::RuntimeOnlyEntity{});
        world.AddComponentImmediate(piece, Components::Parent{out.placer});
        world.AddComponentImmediate(piece, Components::Transform::FromTRS(
            Mathematics::Vector3{static_cast<float>(i), 0, 0}, Mathematics::Quaternion{},
            Mathematics::Vector3{1, 1, 1}));
        out.pieces.push_back(piece);
    }
    return out;
}

int CountRuntimeOnlyEntities(ECS::World& world)
{
    int count = 0;
    for (auto* arch : world.GetAllArchetypes())
    {
        if (!arch)
            continue;
        for (const auto& h : arch->CollectEntities())
        {
            if (h.IsValid() && world.IsValid(h) && world.HasComponent<Components::RuntimeOnlyEntity>(h))
                ++count;
        }
    }
    return count;
}
} // namespace

// Generated pieces are CHILDREN of an entity that is itself saved. The exclusion has to
// survive that: the placer must round-trip, every piece must be absent, and the file must
// carry no reference to a piece — the parent link lives on the child, so an excluded child
// takes its own reference with it and the placer never names one.
TEST(SceneIO, SaveThenLoad_RuntimeOnlyChildrenOfSavedParentExcluded)
{
    const auto scenePath = MakeTempPath("runtime_only_children.scene");

    ECS::World w1;
    const auto built = MakePlacerWithGeneratedChildren(w1, 3);
    ASSERT_TRUE(built.placer.IsValid());

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string text;
    ASSERT_TRUE(ReadFile(scenePath, text));
    EXPECT_NE(text.find("placer"), std::string::npos) << text;
    EXPECT_EQ(text.find("piece_"), std::string::npos) << text;
    EXPECT_EQ(text.find("RuntimeOnlyEntity"), std::string::npos) << text;
    // No entity block may claim a piece as its parent, and no piece may claim the placer.
    EXPECT_EQ(text.find("parent=\"piece_"), std::string::npos) << text;
    EXPECT_EQ(text.find("parent=\"placer\""), std::string::npos) << text;

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const ECS::EntityHandle placer2 = FindByTag(w2, "placer");
    EXPECT_TRUE(placer2.IsValid());
    EXPECT_FALSE(FindByTag(w2, "piece_0").IsValid());
    EXPECT_FALSE(FindByTag(w2, "piece_1").IsValid());
    EXPECT_FALSE(FindByTag(w2, "piece_2").IsValid());
    EXPECT_EQ(CountRuntimeOnlyEntities(w2), 0);
}

// The exclusion is built unconditionally, so it must be APPLIED unconditionally. The
// flattening save arm (both preserve-* options off) writes entity blocks through a
// separate loop; a tagged entity must be dropped there too, or a caller that flattens
// blueprints silently bakes every generated piece into the file.
TEST(SceneIO, Save_RuntimeOnlyChildrenExcludedWhenPreserveOptionsAreOff)
{
    const auto scenePath = MakeTempPath("runtime_only_children_flat.scene");

    ECS::World w1;
    const auto built = MakePlacerWithGeneratedChildren(w1, 3);
    ASSERT_TRUE(built.placer.IsValid());

    Scene::SaveOptions opts;
    opts.preserveBlueprintInstances = false;
    opts.preserveSubscenes = false;
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, opts));

    std::string text;
    ASSERT_TRUE(ReadFile(scenePath, text));
    EXPECT_NE(text.find("placer"), std::string::npos) << text;
    EXPECT_EQ(text.find("piece_"), std::string::npos) << text;
    EXPECT_EQ(text.find("RuntimeOnlyEntity"), std::string::npos) << text;
    EXPECT_EQ(text.find("parent=\"placer\""), std::string::npos) << text;

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    EXPECT_TRUE(FindByTag(w2, "placer").IsValid());
    EXPECT_EQ(CountRuntimeOnlyEntities(w2), 0);
}

// An authored entity parented UNDER a generated one is the corruption case: the piece is
// excluded, so `parent=` would name no block in the file, and the loader rejects a missing
// parent — the scene stops opening, and the editor reports that silently. The save must
// refuse instead, in BOTH emission arms, whatever created the edge.
TEST(SceneIO, Save_RefusesAnAuthoredEntityParentedUnderAGeneratedOne)
{
    const auto scenePath = MakeTempPath("authored_under_generated.scene");

    ECS::World w1;
    const auto built = MakePlacerWithGeneratedChildren(w1, 2);
    const ECS::EntityHandle stray = w1.CreateEntity();
    w1.AddComponentImmediate(stray, MakeSceneTag("stray"));
    w1.AddComponentImmediate(stray, Components::Parent{built.pieces[0]});
    w1.AddComponentImmediate(stray, Components::Transform{});

    EXPECT_FALSE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    Scene::SaveOptions flat;
    flat.preserveBlueprintInstances = false;
    flat.preserveSubscenes = false;
    EXPECT_FALSE(Scene::SaveSceneToFile(w1, scenePath, flat));

    // Re-parenting the stray onto the placer — what the error message tells the user
    // to do — saves, and the file round-trips.
    w1.AddComponentImmediate(stray, Components::Parent{built.placer});
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    EXPECT_TRUE(FindByTag(w2, "stray").IsValid());
}

namespace
{
// The serialized HierarchyOrder line for one tagged entity, or "" if absent.
std::string SavedHierarchyOrderLine(const std::string& sceneText, const std::string& tag)
{
    const std::string header = "[entity id=\"" + tag + "\"";
    const size_t start = sceneText.find(header);
    if (start == std::string::npos)
        return {};
    const size_t end = sceneText.find("\n[", start + 1);
    const std::string block = sceneText.substr(start, end == std::string::npos ? end : end - start);
    const size_t orderAt = block.find("HierarchyOrder.order");
    if (orderAt == std::string::npos)
        return {};
    const size_t lineEnd = block.find('\n', orderAt);
    return block.substr(orderAt, lineEnd == std::string::npos ? lineEnd : lineEnd - orderAt);
}
} // namespace

// Sibling order is re-densified per parent group before writing. Runtime-only entities must
// not take part: they share a parent with authored siblings, so counting them makes an
// authored entity's saved order depend on how many pieces a generator happened to emit —
// the file churns on a change the user never made and cannot see.
TEST(SceneIO, Save_AuthoredSiblingOrderIsIndependentOfGeneratedPieceCount)
{
    const auto pathFew = MakeTempPath("sibling_order_few.scene");
    const auto pathMany = MakeTempPath("sibling_order_many.scene");

    const auto saveWithPieceCount = [](const std::filesystem::path& path, int pieceCount)
    {
        ECS::World w;
        const auto built = MakePlacerWithGeneratedChildren(w, pieceCount);
        const ECS::EntityHandle authored = w.CreateEntity();
        w.AddComponentImmediate(authored, MakeSceneTag("authored_child"));
        w.AddComponentImmediate(authored, Components::Parent{built.placer});
        w.AddComponentImmediate(authored, Components::Transform{});
        EXPECT_TRUE(Scene::SaveSceneToFile(w, path, Scene::SaveOptions{}));

        std::string text;
        EXPECT_TRUE(ReadFile(path, text));
        return text;
    };

    const std::string few = saveWithPieceCount(pathFew, 3);
    const std::string many = saveWithPieceCount(pathMany, 9);

    const std::string orderFew = SavedHierarchyOrderLine(few, "authored_child");
    const std::string orderMany = SavedHierarchyOrderLine(many, "authored_child");
    ASSERT_FALSE(orderFew.empty()) << few;
    EXPECT_EQ(orderFew, orderMany) << "few:\n" << few << "\nmany:\n" << many;
}

// Duplicating an entity copies its SceneEntityTag, so two live entities can share the same scene id.
// Saving must reassign the collision (EnsureTags) — otherwise the loader rejects the file with
// "Duplicate entity id" and the scene won't open.
TEST(SceneIO, SaveThenLoad_DuplicateEntityTagsAreMadeUnique)
{
    const auto scenePath = MakeTempPath("duplicate_tags.scene");

    ECS::World w1;
    const ECS::EntityHandle a = w1.CreateEntity();
    const ECS::EntityHandle b = w1.CreateEntity();
    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(b.IsValid());

    // Both entities carry the identical scene id, exactly as a clone of the original would.
    w1.AddComponentImmediate(a, Components::SceneEntityTag{{'p', 'o', 'i', 'n', 't', '_', 'l', 'i', 'g', 'h', 't', '\0'}});
    w1.AddComponentImmediate(b, Components::SceneEntityTag{{'p', 'o', 'i', 'n', 't', '_', 'l', 'i', 'g', 'h', 't', '\0'}});

    Components::Name na{};
    std::memset(na.value, 0, sizeof(na.value));
    std::memcpy(na.value, "Point Light", 11);
    w1.AddComponentImmediate(a, na);

    Components::Name nb{};
    std::memset(nb.value, 0, sizeof(nb.value));
    std::memcpy(nb.value, "Point Light (1)", 15);
    w1.AddComponentImmediate(b, nb);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    // The saved scene must pass validation and load both entities.
    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    // One entity keeps "point_light"; the collision was reassigned a distinct id.
    EXPECT_TRUE(FindByTag(w2, "point_light").IsValid());

    int tagged = 0;
    std::string firstTag;
    std::string secondTag;
    for (auto* arch : w2.GetAllArchetypes())
    {
        if (!arch)
            continue;
        for (const auto& h : arch->CollectEntities())
        {
            if (!h.IsValid() || !w2.IsValid(h))
                continue;
            if (auto* t = w2.GetComponent<Components::SceneEntityTag>(h))
            {
                if (t->View().empty())
                    continue;
                if (tagged == 0)
                    firstTag = t->View();
                else if (tagged == 1)
                    secondTag = t->View();
                ++tagged;
            }
        }
    }
    EXPECT_EQ(tagged, 2);
    EXPECT_NE(firstTag, secondTag);
}

// The physical-light authoring fields (unit + colour temperature) must round-trip through a save/load
// so the inspector's choices persist; omitting them (pre-physical-units scenes) must default cleanly.
TEST(SceneIO, SaveThenLoad_RoundTripLightPhysicalUnits)
{
    const auto scenePath = MakeTempPath("light_physical.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'l', 'i', 'g', 'h', 't', '\0'}});

    Components::Light l{};
    l.Type = Components::LightType::Spot;
    l.Intensity = 1600.0f;
    l.IntensityUnit = Components::LightUnit::Lumen;
    l.UseColorTemperature = true;
    l.ColorTemperature = 5600.0f;
    w1.AddComponentImmediate(e, l);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "light");
    ASSERT_TRUE(e2.IsValid());
    auto* l2 = w2.GetComponent<Components::Light>(e2);
    ASSERT_NE(l2, nullptr);
    EXPECT_EQ(l2->IntensityUnit, Components::LightUnit::Lumen);
    EXPECT_TRUE(l2->UseColorTemperature);
    EXPECT_FLOAT_EQ(l2->ColorTemperature, 5600.0f);
    EXPECT_FLOAT_EQ(l2->Intensity, 1600.0f);
}

// The directional light's angular diameter drives PCSS penumbra width, so an
// authored value that silently reverted to the solar default on load would
// change every soft shadow in the scene.
TEST(SceneIO, SaveThenLoad_RoundTripLightAngularDiameter)
{
    const auto scenePath = MakeTempPath("light_angular_diameter.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'l', 'i', 'g', 'h', 't', '\0'}});

    Components::Light l{};
    l.Type = Components::LightType::Directional;
    l.ShadowAngularDiameter = 4.25f;
    w1.AddComponentImmediate(e, l);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "light");
    ASSERT_TRUE(e2.IsValid());
    auto* l2 = w2.GetComponent<Components::Light>(e2);
    ASSERT_NE(l2, nullptr);
    EXPECT_FLOAT_EQ(l2->ShadowAngularDiameter, 4.25f);
}

// Scenes authored before the field existed carry no value and must land on the
// physical solar disc, not on zero (a legal but very different look: perfectly
// collimated, hard shadows). Softer suns are an authored opt-in, never the
// default.
TEST(SceneIO, LightAngularDiameterDefaultsToSolarDisc)
{
    Components::Light l{};
    EXPECT_FLOAT_EQ(l.ShadowAngularDiameter, 0.53f);
}

// Exposure lives on the Camera (the sensor). The exposure-control fields (mode + Manual EV100 +
// compensation + the physical sensor knobs) must persist so a physically-exposed look survives
// save/load.
TEST(SceneIO, SaveThenLoad_RoundTripCameraExposure)
{
    const auto scenePath = MakeTempPath("camera_exposure.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'c', 'a', 'm', '\0'}});
    Components::Camera c{};
    c.ExposureControl = Components::ExposureMode::Physical;
    c.ManualExposureEV = 12.5f;
    c.ExposureCompensation = -1.5f;
    c.Aperture = 2.8f;
    c.ApertureBladeCount = 9;
    c.ApertureRoundness = 0.35f;
    c.ApertureRotation = 27.5f;
    c.AnamorphicSqueeze = 2.0f;
    c.FocusDebugMode = 1;
    c.FocusDebugAlpha = 0.7f;
    c.ShutterTime = 0.004f;
    c.Iso = 400.0f;
    w1.AddComponentImmediate(e, c);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "cam");
    ASSERT_TRUE(e2.IsValid());
    auto* c2 = w2.GetComponent<Components::Camera>(e2);
    ASSERT_NE(c2, nullptr);
    EXPECT_EQ(c2->ExposureControl, Components::ExposureMode::Physical);
    EXPECT_FLOAT_EQ(c2->ManualExposureEV, 12.5f);
    EXPECT_FLOAT_EQ(c2->ExposureCompensation, -1.5f);
    EXPECT_FLOAT_EQ(c2->Aperture, 2.8f);
    EXPECT_EQ(c2->ApertureBladeCount, 9u);
    EXPECT_FLOAT_EQ(c2->ApertureRoundness, 0.35f);
    EXPECT_FLOAT_EQ(c2->ApertureRotation, 27.5f);
    EXPECT_FLOAT_EQ(c2->AnamorphicSqueeze, 2.0f);
    EXPECT_EQ(c2->FocusDebugMode, 1);
    EXPECT_FLOAT_EQ(c2->FocusDebugAlpha, 0.7f);
    EXPECT_FLOAT_EQ(c2->ShutterTime, 0.004f);
    EXPECT_FLOAT_EQ(c2->Iso, 400.0f);
}

TEST(SceneIO, Load_CameraSensorPresetAlsoSetsSensorHeight)
{
    const auto scenePath = MakeTempPath("camera_sensor_preset.scene");
    const auto preset = Components::CameraSensorPreset::Super8;
    const std::string src =
        "[scene name=\"CameraPreset\" version=1]\n\n"
        "[entity id=\"cam\"]\n"
        "Camera.sensorPreset = " + std::to_string(static_cast<uint32>(preset)) + "\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(
        world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto entity = FindByTag(world, "cam");
    ASSERT_TRUE(entity.IsValid());
    const auto* camera = world.GetComponent<Components::Camera>(entity);
    ASSERT_NE(camera, nullptr);
    EXPECT_EQ(camera->SensorPreset, preset);
    EXPECT_FLOAT_EQ(camera->SensorHeightMm,
                    Components::CameraSensorPresetHeightMm(preset));
}

// Auto exposure keys the metered average to 18 % grey and moves it only through exposure
// compensation: the camera has no separate key field, so a save never writes one and a scene that
// still names it loads with that one property skipped (delete the line to fix the scene).
TEST(SceneIO, CameraHasNoAutoExposureKeyProperty)
{
    const auto savedPath = MakeTempPath("camera_no_key_saved.scene");
    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'c', 'a', 'm', '\0'}});
    w1.AddComponentImmediate(e, Components::Camera{});
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, savedPath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_NE(saved.find("Camera.autoExposureMinEv"), std::string::npos) << saved;
    EXPECT_EQ(saved.find("autoExposureKey"), std::string::npos) << saved;

    const auto authoredPath = MakeTempPath("camera_no_key_authored.scene");
    ASSERT_TRUE(WriteFile(authoredPath,
                          "[scene name=\"CameraKey\" version=1]\n\n"
                          "[entity id=\"cam\"]\n"
                          "Camera.autoExposureMaxEv = 16\n"
                          "Camera.autoExposureKey = 0.18\n"));
    ECS::World w2;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, authoredPath, opts));
    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].component, "Camera");
    EXPECT_EQ(degradation.skips[0].field, "autoExposureKey");
    const auto cam = FindByTag(w2, "cam");
    ASSERT_TRUE(cam.IsValid());
    const auto* camera = w2.GetComponent<Components::Camera>(cam);
    ASSERT_NE(camera, nullptr) << "the other Camera fields still apply";
    EXPECT_FLOAT_EQ(camera->AutoExposureMaxEv, 16.0f);
}

// The ambient gradient tint (a 3-color diffuse-irradiance wash over the baked irradiance)
// must survive save/load so an authored Synty-style ambient look is not lost on scene reload.
TEST(SceneIO, SaveThenLoad_RoundTripSkyAmbientGradientTint)
{
    const auto scenePath = MakeTempPath("sky_ambient_tint.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'s', 'k', 'y', '\0'}});

    Components::SkyEnvironment s{};
    s.AmbientTintSky[0] = 0.62f;    s.AmbientTintSky[1] = 0.30f;    s.AmbientTintSky[2] = 0.85f;    // purple
    s.AmbientTintEquator[0] = 0.90f; s.AmbientTintEquator[1] = 0.20f; s.AmbientTintEquator[2] = 0.55f; // magenta
    s.AmbientTintGround[0] = 0.10f;  s.AmbientTintGround[1] = 0.70f;  s.AmbientTintGround[2] = 0.80f;  // cyan
    w1.AddComponentImmediate(e, s);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "sky");
    ASSERT_TRUE(e2.IsValid());
    auto* s2 = w2.GetComponent<Components::SkyEnvironment>(e2);
    ASSERT_NE(s2, nullptr);
    EXPECT_FLOAT_EQ(s2->AmbientTintSky[0], 0.62f);
    EXPECT_FLOAT_EQ(s2->AmbientTintSky[1], 0.30f);
    EXPECT_FLOAT_EQ(s2->AmbientTintSky[2], 0.85f);
    EXPECT_FLOAT_EQ(s2->AmbientTintEquator[0], 0.90f);
    EXPECT_FLOAT_EQ(s2->AmbientTintEquator[1], 0.20f);
    EXPECT_FLOAT_EQ(s2->AmbientTintEquator[2], 0.55f);
    EXPECT_FLOAT_EQ(s2->AmbientTintGround[0], 0.10f);
    EXPECT_FLOAT_EQ(s2->AmbientTintGround[1], 0.70f);
    EXPECT_FLOAT_EQ(s2->AmbientTintGround[2], 0.80f);
}

// The stylistic sun size must survive save/load, and the 2D-only size has to stay a SEPARATE
// value: they are different quantities (a solid angle vs an NDC screen fraction) and the scene
// text carries both, so a parser that folded one into the other would silently resize a sky.
TEST(SceneIO, SaveThenLoad_RoundTripSkySunSize)
{
    const auto scenePath = MakeTempPath("sky_sun_size.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'s', 'u', 'n', '\0'}});

    Components::SkyEnvironment s{};
    s.SunSize = 2.75f;
    s.SunSize2D = 0.6f;
    w1.AddComponentImmediate(e, s);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "sun");
    ASSERT_TRUE(e2.IsValid());
    auto* s2 = w2.GetComponent<Components::SkyEnvironment>(e2);
    ASSERT_NE(s2, nullptr);
    EXPECT_FLOAT_EQ(s2->SunSize, 2.75f);
    EXPECT_FLOAT_EQ(s2->SunSize2D, 0.6f);
}

// A scene authored before the lever existed carries no SunSize line, and must load as the
// physical sun rather than as a zero-initialised (invisible) one. Every shipped scene is in
// exactly this state until it is next re-saved.
TEST(SceneIO, Load_SkyWithoutSunSizeKeepsThePhysicalSun)
{
    const auto scenePath = MakeTempPath("sky_sun_size_absent.scene");
    const std::string src =
        "[scene name=\"NoSunSize\" version=1]\n"
        "\n"
        "[entity id=\"sky\"]\n"
        "SkyEnvironment.Enabled = true\n"
        "SkyEnvironment.SunSize2D = 1\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto loadedEntity = FindByTag(w, "sky");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* sky = w.GetComponent<Components::SkyEnvironment>(loadedEntity);
    ASSERT_NE(sky, nullptr);
    EXPECT_FLOAT_EQ(sky->SunSize, 1.0f);
}

// The parser bounds the value the same way the system and the inspector do, so hand-edited
// scene text cannot hand the renderer a radius outside the authoring range.
TEST(SceneIO, Load_SkySunSizeIsClampedToTheAuthoringRange)
{
    const auto scenePath = MakeTempPath("sky_sun_size_clamped.scene");
    const std::string src =
        "[scene name=\"HugeSun\" version=1]\n"
        "\n"
        "[entity id=\"sky\"]\n"
        "SkyEnvironment.Enabled = true\n"
        "SkyEnvironment.SunSize = 500\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto loadedEntity = FindByTag(w, "sky");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* sky = w.GetComponent<Components::SkyEnvironment>(loadedEntity);
    ASSERT_NE(sky, nullptr);
    EXPECT_FLOAT_EQ(sky->SunSize, Components::kSunSizeMax);
}

// The sun path's site survives save and load; a scene written before it existed loads at the
// equator on the March equinox, which is the previous sky; hand-edited values outside the
// meaningful range are brought back into it rather than handed to the solar maths; and a value that
// is not finite (1e39 overflows a float to infinity) falls back to the default latitude 0 rather
// than being clamped to a pole.
TEST(SceneIO, SaveThenLoad_RoundTripSkySunPathAndSanitizes)
{
    const auto scenePath = MakeTempPath("sky_sun_path.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'s', 'k', 'y', '\0'}});
    Components::SkyEnvironment s{};
    s.Latitude = -33.9f;
    s.DayOfYear = 355;
    s.NorthHeading = 123.5f;
    w1.AddComponentImmediate(e, s);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto e2 = FindByTag(w2, "sky");
    ASSERT_TRUE(e2.IsValid());
    const auto* s2 = w2.GetComponent<Components::SkyEnvironment>(e2);
    ASSERT_NE(s2, nullptr);
    EXPECT_FLOAT_EQ(s2->Latitude, -33.9f);
    EXPECT_EQ(s2->DayOfYear, 355);
    EXPECT_FLOAT_EQ(s2->NorthHeading, 123.5f);

    struct Case
    {
        const char* Lines;
        float Latitude;
        int32_t Day;
        float North = 0.0f;
    };
    for (const Case c : {Case{"", 0.0f, Rendering::kMarchEquinoxDay},
                         Case{"SkyEnvironment.Latitude = 200\nSkyEnvironment.DayOfYear = 0\n", 90.0f, 1},
                         Case{"SkyEnvironment.Latitude = -91.5\nSkyEnvironment.DayOfYear = 999\n", -90.0f, 365},
                         Case{"SkyEnvironment.Latitude = 1e39\n", 0.0f, Rendering::kMarchEquinoxDay},
                         Case{"SkyEnvironment.NorthHeading = -90\n", 0.0f, Rendering::kMarchEquinoxDay, 270.0f},
                         Case{"SkyEnvironment.NorthHeading = 720.5\n", 0.0f, Rendering::kMarchEquinoxDay, 0.5f},
                         Case{"SkyEnvironment.NorthHeading = 1e39\n", 0.0f, Rendering::kMarchEquinoxDay, 0.0f}})
    {
        const auto path = MakeTempPath("sky_sun_path_sanitised.scene");
        ASSERT_TRUE(WriteFile(path, std::string("[scene name=\"SunPath\" version=1]\n\n[entity id=\"sky\"]\n"
                                                "SkyEnvironment.Enabled = true\n") +
                                        c.Lines));
        ECS::World w;
        ASSERT_TRUE(Scene::LoadSceneFromFile(w, path, Scene::LoadOptions{Scene::LoadMode::Replace})) << c.Lines;
        const auto* sky = w.GetComponent<Components::SkyEnvironment>(FindByTag(w, "sky"));
        ASSERT_NE(sky, nullptr) << c.Lines;
        EXPECT_FLOAT_EQ(sky->Latitude, c.Latitude) << c.Lines;
        EXPECT_EQ(sky->DayOfYear, c.Day) << c.Lines;
        EXPECT_FLOAT_EQ(sky->NorthHeading, c.North) << c.Lines;
    }
}

// The Sun path choice and the Custom path's fields survive save and load, and so do the Earth
// fields while Custom is chosen: switching is lossless across a save too. Hand-edited values are
// brought into range, and a SunPath token this build does not know loads as Earth rather than
// failing the scene.
TEST(SceneIO, SkyCustomSunPathRoundTripsAndSanitizes)
{
    const auto scenePath = MakeTempPath("sky_custom_sun_path.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'s', 'k', 'y', '\0'}});
    Components::SkyEnvironment s{};
    s.SunPath = Components::SkySunPathKind::Custom;
    s.Latitude = 51.5f;
    s.DayOfYear = 172;
    s.NorthHeading = 15.0f;
    s.CustomAxisHeading = 250.0f;
    s.CustomAxisAltitude = 70.0f;
    s.CustomNoonHeight = 25.0f;
    w1.AddComponentImmediate(e, s);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto* s2 = w2.GetComponent<Components::SkyEnvironment>(FindByTag(w2, "sky"));
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->SunPath, Components::SkySunPathKind::Custom);
    EXPECT_FLOAT_EQ(s2->Latitude, 51.5f);
    EXPECT_EQ(s2->DayOfYear, 172);
    EXPECT_FLOAT_EQ(s2->NorthHeading, 15.0f);
    EXPECT_FLOAT_EQ(s2->CustomAxisHeading, 250.0f);
    EXPECT_FLOAT_EQ(s2->CustomAxisAltitude, 70.0f);
    EXPECT_FLOAT_EQ(s2->CustomNoonHeight, 25.0f);

    struct Case
    {
        const char* Lines;
        Components::SkySunPathKind Path;
        float AxisHeading;
        float AxisAltitude;
        float NoonHeight;
    };
    const Case cases[] = {
        {"", Components::SkySunPathKind::Earth, 0.0f, 0.0f, 90.0f},
        {"SkyEnvironment.SunPath = custom\n", Components::SkySunPathKind::Custom, 0.0f, 0.0f, 90.0f},
        {"SkyEnvironment.SunPath = \"Custom\"\n", Components::SkySunPathKind::Custom, 0.0f, 0.0f, 90.0f},
        {"SkyEnvironment.SunPath = Martian\n", Components::SkySunPathKind::Earth, 0.0f, 0.0f, 90.0f},
        // A noon height under the horizon is stored as written: the clamp that depends on the axis is
        // applied at use, and an axis tilted up reaches it.
        {"SkyEnvironment.CustomAxisHeading = -30\nSkyEnvironment.CustomAxisAltitude = 120\n"
         "SkyEnvironment.CustomNoonHeight = -5\n",
         Components::SkySunPathKind::Earth, 330.0f, 90.0f, -5.0f},
        {"SkyEnvironment.CustomAxisHeading = 1e39\nSkyEnvironment.CustomAxisAltitude = 1e39\n"
         "SkyEnvironment.CustomNoonHeight = 1e39\n",
         Components::SkySunPathKind::Earth, 0.0f, 0.0f, 90.0f},
        {"SkyEnvironment.CustomNoonHeight = 400\n", Components::SkySunPathKind::Earth, 0.0f, 0.0f, 270.0f},
        {"SkyEnvironment.CustomNoonHeight = -100\n", Components::SkySunPathKind::Earth, 0.0f, 0.0f, -90.0f},
    };
    for (const Case& c : cases)
    {
        const auto path = MakeTempPath("sky_custom_sun_path_sanitised.scene");
        ASSERT_TRUE(WriteFile(path, std::string("[scene name=\"SunPath\" version=1]\n\n[entity id=\"sky\"]\n"
                                                "SkyEnvironment.Enabled = true\n") +
                                        c.Lines));
        ECS::World w;
        ASSERT_TRUE(Scene::LoadSceneFromFile(w, path, Scene::LoadOptions{Scene::LoadMode::Replace})) << c.Lines;
        const auto* sky = w.GetComponent<Components::SkyEnvironment>(FindByTag(w, "sky"));
        ASSERT_NE(sky, nullptr) << c.Lines;
        EXPECT_EQ(sky->SunPath, c.Path) << c.Lines;
        EXPECT_FLOAT_EQ(sky->CustomAxisHeading, c.AxisHeading) << c.Lines;
        EXPECT_FLOAT_EQ(sky->CustomAxisAltitude, c.AxisAltitude) << c.Lines;
        EXPECT_FLOAT_EQ(sky->CustomNoonHeight, c.NoonHeight) << c.Lines;
    }
}

TEST(SceneIO, SaveThenLoad_RoundTripSkyGradientMode)
{
    const auto scenePath = MakeTempPath("sky_gradient_mode.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'g', 'r', 'a', 'd', '\0'}});

    Components::SkyEnvironment s{};
    s.Mode = Components::SkyMode::Gradient;
    s.GradientSkyTopColor[0] = 0.10f;    s.GradientSkyTopColor[1] = 0.35f;    s.GradientSkyTopColor[2] = 0.90f;
    s.GradientSkyHorizonColor[0] = 0.85f; s.GradientSkyHorizonColor[1] = 0.55f; s.GradientSkyHorizonColor[2] = 0.40f;
    s.GradientSkyBottomColor[0] = 0.05f;  s.GradientSkyBottomColor[1] = 0.06f;  s.GradientSkyBottomColor[2] = 0.08f;
    s.GradientSkyIntensity = 6500.0f;
    w1.AddComponentImmediate(e, s);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "grad");
    ASSERT_TRUE(e2.IsValid());
    auto* s2 = w2.GetComponent<Components::SkyEnvironment>(e2);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->Mode, Components::SkyMode::Gradient);
    EXPECT_FLOAT_EQ(s2->GradientSkyTopColor[0], 0.10f);
    EXPECT_FLOAT_EQ(s2->GradientSkyTopColor[1], 0.35f);
    EXPECT_FLOAT_EQ(s2->GradientSkyTopColor[2], 0.90f);
    EXPECT_FLOAT_EQ(s2->GradientSkyHorizonColor[0], 0.85f);
    EXPECT_FLOAT_EQ(s2->GradientSkyHorizonColor[1], 0.55f);
    EXPECT_FLOAT_EQ(s2->GradientSkyHorizonColor[2], 0.40f);
    EXPECT_FLOAT_EQ(s2->GradientSkyBottomColor[0], 0.05f);
    EXPECT_FLOAT_EQ(s2->GradientSkyBottomColor[1], 0.06f);
    EXPECT_FLOAT_EQ(s2->GradientSkyBottomColor[2], 0.08f);
    EXPECT_FLOAT_EQ(s2->GradientSkyIntensity, 6500.0f);
}

// The opt-in AmbientLight floor (mode enum + flat/gradient colors + intensity + affect-specular)
// must survive save/load so an authored night-fill is not lost on reload.
TEST(SceneIO, SaveThenLoad_RoundTripAmbientLightGradient)
{
    const auto scenePath = MakeTempPath("ambient_light_gradient.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'a', 'm', 'b', '\0'}});

    Components::AmbientLight a{};
    a.Mode = Components::AmbientLightMode::Gradient;
    a.SkyColor[0] = 0.20f;     a.SkyColor[1] = 0.12f;     a.SkyColor[2] = 0.35f;   // purple
    a.EquatorColor[0] = 0.30f; a.EquatorColor[1] = 0.10f; a.EquatorColor[2] = 0.28f;
    a.GroundColor[0] = 0.04f;  a.GroundColor[1] = 0.03f;  a.GroundColor[2] = 0.06f;
    a.Intensity = 60.0f;
    a.AffectSpecular = true;
    w1.AddComponentImmediate(e, a);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "amb");
    ASSERT_TRUE(e2.IsValid());
    auto* a2 = w2.GetComponent<Components::AmbientLight>(e2);
    ASSERT_NE(a2, nullptr);
    EXPECT_EQ(a2->Mode, Components::AmbientLightMode::Gradient);
    EXPECT_FLOAT_EQ(a2->SkyColor[0], 0.20f);
    EXPECT_FLOAT_EQ(a2->SkyColor[1], 0.12f);
    EXPECT_FLOAT_EQ(a2->SkyColor[2], 0.35f);
    EXPECT_FLOAT_EQ(a2->EquatorColor[0], 0.30f);
    EXPECT_FLOAT_EQ(a2->GroundColor[2], 0.06f);
    EXPECT_FLOAT_EQ(a2->Intensity, 60.0f);
    EXPECT_TRUE(a2->AffectSpecular);
}

// Flat mode + defaults for the fields the gradient test does not exercise.
TEST(SceneIO, SaveThenLoad_RoundTripAmbientLightFlat)
{
    const auto scenePath = MakeTempPath("ambient_light_flat.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'a', 'm', 'f', '\0'}});

    Components::AmbientLight a{};
    a.Mode = Components::AmbientLightMode::Flat;
    a.Color[0] = 0.5f; a.Color[1] = 0.4f; a.Color[2] = 0.7f;
    a.Intensity = 120.0f;
    a.AffectSpecular = false;
    w1.AddComponentImmediate(e, a);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "amf");
    ASSERT_TRUE(e2.IsValid());
    auto* a2 = w2.GetComponent<Components::AmbientLight>(e2);
    ASSERT_NE(a2, nullptr);
    EXPECT_EQ(a2->Mode, Components::AmbientLightMode::Flat);
    EXPECT_FLOAT_EQ(a2->Color[0], 0.5f);
    EXPECT_FLOAT_EQ(a2->Color[1], 0.4f);
    EXPECT_FLOAT_EQ(a2->Color[2], 0.7f);
    EXPECT_FLOAT_EQ(a2->Intensity, 120.0f);
    EXPECT_FALSE(a2->AffectSpecular);
}

// Opt-in on the SAVE side too: a world with no AmbientLight writes ZERO AmbientLight lines, so
// untouched scenes cannot silently grow (or default-in) an ambient floor on resave.
TEST(SceneIO, Save_WithoutAmbientLight_EmitsNoAmbientLightLines)
{
    const auto scenePath = MakeTempPath("ambient_light_optin.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'a', 'm', 'n', '\0'}});
    w1.AddComponentImmediate(e, Components::Transform{});

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string text;
    ASSERT_TRUE(ReadFile(scenePath, text));
    EXPECT_EQ(text.find("AmbientLight."), std::string::npos)
        << "component-less scene must not serialize any AmbientLight properties";
}

// Volume exposure modifiers (ExposureAdjustmentEffect) stack on the camera instead of owning
// exposure; the authored compensation and opt-in narrow-only clamps must survive save/load.
TEST(SceneIO, SaveThenLoad_RoundTripExposureAdjustmentEffect)
{
    const auto scenePath = MakeTempPath("exposure_adjust.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'v', 'o', 'l', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    Components::ExposureAdjustmentEffect adjust{};
    adjust.Compensation = -1.5f;
    adjust.ClampMin = true;
    adjust.MinEv = 7.5f;
    adjust.ClampMax = true;
    adjust.MaxEv = 12.0f;
    w1.AddComponentImmediate(e, adjust);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "vol");
    ASSERT_TRUE(e2.IsValid());
    auto* a2 = w2.GetComponent<Components::ExposureAdjustmentEffect>(e2);
    ASSERT_NE(a2, nullptr);
    EXPECT_TRUE(a2->Enabled);
    EXPECT_FLOAT_EQ(a2->Compensation, -1.5f);
    EXPECT_TRUE(a2->ClampMin);
    EXPECT_FLOAT_EQ(a2->MinEv, 7.5f);
    EXPECT_TRUE(a2->ClampMax);
    EXPECT_FLOAT_EQ(a2->MaxEv, 12.0f);
}

TEST(SceneIO, SaveThenLoad_RoundTripIctcpChromaCompression)
{
    const auto scenePath = MakeTempPath("ictcp_chroma_compression.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'i', 'c', 't', '\0'}});
    Components::PostProcessVolume volume{};
    volume.IctcpChromaCompression = 0.65f;
    w1.AddComponentImmediate(e, volume);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "ict");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::PostProcessVolume>(e2);
    ASSERT_NE(loaded, nullptr);
    EXPECT_FLOAT_EQ(loaded->IctcpChromaCompression, 0.65f);
}

// VignetteEffect round-trips every authored field (URP-exact optical falloff parameters).
TEST(SceneIO, SaveThenLoad_RoundTripVignetteEffect)
{
    const auto scenePath = MakeTempPath("vignette.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'v', 'i', 'g', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    Components::VignetteEffect vig{};
    vig.Intensity = 0.2f;
    vig.Smoothness = 0.2f;
    vig.Rounded = true;
    vig.StackOrder = 3;
    vig.Color[0] = 0.1f;
    vig.Color[1] = 0.0f;
    vig.Color[2] = 0.2f;
    w1.AddComponentImmediate(e, vig);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "vig");
    ASSERT_TRUE(e2.IsValid());
    auto* v2 = w2.GetComponent<Components::VignetteEffect>(e2);
    ASSERT_NE(v2, nullptr);
    EXPECT_TRUE(v2->Enabled);
    EXPECT_FLOAT_EQ(v2->Intensity, 0.2f);
    EXPECT_FLOAT_EQ(v2->Smoothness, 0.2f);
    EXPECT_TRUE(v2->Rounded);
    EXPECT_EQ(v2->StackOrder, 3);
    EXPECT_FLOAT_EQ(v2->Color[0], 0.1f);
    EXPECT_FLOAT_EQ(v2->Color[1], 0.0f);
    EXPECT_FLOAT_EQ(v2->Color[2], 0.2f);
}

TEST(SceneIO, ShadowDistanceFadeAuthoredRangeRoundTrips)
{
    const auto source = MakeTempPath("shadow_fade_authored.scene");
    const auto saved = MakeTempPath("shadow_fade_saved.scene");
    struct Case { const char* Input; const char* Expected; };
    const Case cases[] = {{"0", "0"}, {"0.25", "0.25"}, {"-0.5", "0"}, {"1.5", "0.5"}};
    for (const Case& test : cases)
    {
        SCOPED_TRACE(test.Input);
        ASSERT_TRUE(WriteFile(source,
            std::string("[scene name=\"Shadow Fade\" version=1]\n\n[entity id=\"shadow\"]\n") +
            "PostProcessVolume.isGlobal = true\nShadowSettingsEffect.enabled = true\n" +
            "ShadowSettingsEffect.distanceFadeFraction = " + test.Input + "\n"));
        ECS::World world;
        ASSERT_TRUE(Scene::LoadSceneFromFile(world, source, Scene::LoadOptions{Scene::LoadMode::Replace}));
        ASSERT_TRUE(Scene::SaveSceneToFile(world, saved, Scene::SaveOptions{}));
        std::string text;
        ASSERT_TRUE(ReadFile(saved, text));
        EXPECT_NE(text.find(std::string("ShadowSettingsEffect.distanceFadeFraction = ") + test.Expected + "\n"),
                  std::string::npos) << text;
    }
}

// ShadowSettingsEffect round-trips its authored fields (per-volume directional shadow override);
// DistanceFadeFraction and its clamp are pinned by ShadowDistanceFadeAuthoredRangeRoundTrips.
TEST(SceneIO, SaveThenLoad_RoundTripShadowSettingsEffect)
{
    const auto scenePath = MakeTempPath("shadow_settings.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'s', 'h', 'd', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    Components::ShadowSettingsEffect shadow{};
    shadow.Enabled = true;
    shadow.Mode = Components::DirectionalShadowMode::RayTraced;
    shadow.RayTracedQuality = Components::RayTracedShadowQuality::Quality;
    shadow.Filter = Components::DirectionalShadowFilter::MSM4;
    shadow.MaxShadowDistance = 50.0f;
    shadow.SplitLambda = 0.45f;
    shadow.DepthBias = 0.0002f;
    shadow.NormalBias = 0.5f;
    shadow.ScreenSpaceShadows = true;
    shadow.ScreenSpaceShadowThickness = 0.012f;
    w1.AddComponentImmediate(e, shadow);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "shd");
    ASSERT_TRUE(e2.IsValid());
    auto* s2 = w2.GetComponent<Components::ShadowSettingsEffect>(e2);
    ASSERT_NE(s2, nullptr);
    EXPECT_TRUE(s2->Enabled);
    EXPECT_EQ(s2->Mode, Components::DirectionalShadowMode::RayTraced);
    EXPECT_EQ(s2->RayTracedQuality, Components::RayTracedShadowQuality::Quality);
    EXPECT_EQ(s2->Filter, Components::DirectionalShadowFilter::MSM4);
    EXPECT_FLOAT_EQ(s2->MaxShadowDistance, 50.0f);
    EXPECT_FLOAT_EQ(s2->SplitLambda, 0.45f);
    EXPECT_FLOAT_EQ(s2->DepthBias, 0.0002f);
    EXPECT_FLOAT_EQ(s2->NormalBias, 0.5f);
    EXPECT_TRUE(s2->ScreenSpaceShadows);
    EXPECT_FLOAT_EQ(s2->ScreenSpaceShadowThickness, 0.012f);
}

// A scene predating ShadowSettingsEffect.mode (no mode line) loads to Cascades,
// and a mode value from a build newer than this one falls back to Cascades —
// the only mode with full caster coverage.
TEST(SceneIO, Load_ShadowSettingsEffectModeDefaultsAndFutureFallback)
{
    const auto scenePath = MakeTempPath("shadow_settings_mode.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'s', 'h', 'm', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    Components::ShadowSettingsEffect shadow{};
    shadow.Mode = Components::DirectionalShadowMode::RayTraced;
    w1.AddComponentImmediate(e, shadow);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    // Rewrite the mode line: once absent (legacy scene), once from the future.
    std::string text;
    {
        std::ifstream in(scenePath, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string modeLine = "ShadowSettingsEffect.mode = 1";
    const auto pos = text.find(modeLine);
    ASSERT_NE(pos, std::string::npos);
    const auto rewriteScene = [&scenePath](const std::string& body)
    {
        std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
        out << body;
    };

    std::string legacy = text;
    legacy.erase(pos, modeLine.size());
    rewriteScene(legacy);
    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto* s2 = w2.GetComponent<Components::ShadowSettingsEffect>(FindByTag(w2, "shm"));
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->Mode, Components::DirectionalShadowMode::Cascades);

    std::string future = text;
    future.replace(pos, modeLine.size(), "ShadowSettingsEffect.mode = 99");
    rewriteScene(future);
    ECS::World w3;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w3, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto* s3 = w3.GetComponent<Components::ShadowSettingsEffect>(FindByTag(w3, "shm"));
    ASSERT_NE(s3, nullptr);
    EXPECT_EQ(s3->Mode, Components::DirectionalShadowMode::Cascades);
}

// DebandEffect round-trips both fields — notably Enabled=false, the state that
// distinguishes "volume explicitly turns the deband off" from "no component,
// baseline applies" (the terminal deband is on by default like tonemapping).
TEST(SceneIO, SaveThenLoad_RoundTripDebandEffect)
{
    const auto scenePath = MakeTempPath("deband.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'d', 'e', 'b', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    Components::DebandEffect deband{};
    deband.Enabled = false;
    deband.ThresholdLsb = 3.5f;
    w1.AddComponentImmediate(e, deband);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "deb");
    ASSERT_TRUE(e2.IsValid());
    auto* d2 = w2.GetComponent<Components::DebandEffect>(e2);
    ASSERT_NE(d2, nullptr);
    EXPECT_FALSE(d2->Enabled);
    EXPECT_FLOAT_EQ(d2->ThresholdLsb, 3.5f);
}

// A scene whose DebandEffect predates thresholdLsb (no such line) loads the
// component with the reviewed 6-LSB default, and an out-of-range hand-edited
// threshold clamps to the extraction/env range instead of poisoning the gate.
TEST(SceneIO, Load_DebandEffectThresholdDefaultsAndClamps)
{
    const auto scenePath = MakeTempPath("deband_defaults.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'d', 'b', 'd', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    Components::DebandEffect deband{};
    deband.ThresholdLsb = 3.5f;
    w1.AddComponentImmediate(e, deband);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string text;
    {
        std::ifstream in(scenePath, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string thresholdLine = "DebandEffect.thresholdLsb = 3.5";
    const auto pos = text.find(thresholdLine);
    ASSERT_NE(pos, std::string::npos);
    const auto rewriteScene = [&scenePath](const std::string& body)
    {
        std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
        out << body;
    };

    std::string legacy = text;
    legacy.erase(pos, thresholdLine.size());
    rewriteScene(legacy);
    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto* d2 = w2.GetComponent<Components::DebandEffect>(FindByTag(w2, "dbd"));
    ASSERT_NE(d2, nullptr);
    EXPECT_TRUE(d2->Enabled);
    EXPECT_FLOAT_EQ(d2->ThresholdLsb, 6.0f);

    std::string outOfRange = text;
    outOfRange.replace(pos, thresholdLine.size(), "DebandEffect.thresholdLsb = 99");
    rewriteScene(outOfRange);
    ECS::World w3;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w3, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto* d3 = w3.GetComponent<Components::DebandEffect>(FindByTag(w3, "dbd"));
    ASSERT_NE(d3, nullptr);
    EXPECT_FLOAT_EQ(d3->ThresholdLsb, 16.0f);
}

// Bloom pyramid and SE Natural Bloom package controls round-trip, and a scene
// predating Scatter loads to the neutral 0.5 default (the historical fixed blend).
TEST(SceneIO, SaveThenLoad_RoundTripBloomControls)
{
    const auto scenePath = MakeTempPath("bloom_scatter.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'b', 'l', 'm', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    Components::BloomEffect bloom{};
    bloom.ScatteringAmount = 0.35f;
    bloom.AntiFlicker = false;
    bloom.Tint[0] = 0.2f;
    bloom.Tint[1] = 0.4f;
    bloom.Tint[2] = 0.8f;
    bloom.Radius = 6.25f;
    bloom.Octaves = 8;
    bloom.Scatter = 0.95f;
    bloom.DepthVeilEnabled = true;
    bloom.DepthVeilIntensity = 1.75f;
    bloom.DepthVeilStart = 12.0f;
    bloom.DepthVeilEnd = 640.0f;
    bloom.DepthVeilTint[0] = 0.3f;
    bloom.DepthVeilTint[1] = 0.6f;
    bloom.DepthVeilTint[2] = 0.9f;
    bloom.LensDirtEnabled = true;
    bloom.LensDirtVignette = true;
    bloom.LensDirtVignetteIntensity = 0.64f;
    bloom.LensDirtVignetteRadius = 0.31f;
    bloom.LensDirtVignetteSmoothness = 0.37f;
    bloom.LensDirtVignetteRounded = true;
    bloom.LensDirtVignetteColor[0] = 0.25f;
    bloom.LensDirtVignetteColor[1] = 0.5f;
    bloom.LensDirtVignetteColor[2] = 0.75f;
    bloom.LensDirtIntensity = 42.0f;
    bloom.LensDirtScatter = 0.73f;
    constexpr std::string_view kCustomDirtGuid = "77aca023-ed7b-4a39-a1d0-c72c0875e634";
    constexpr std::string_view kLegacyBuiltInDirtGuid = "a8fa27ad-08b8-42b7-9a67-3504f6910034";
    bloom.LensDirtTexture.Set(GUID(std::string(kCustomDirtGuid)));
    w1.AddComponentImmediate(e, bloom);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "blm");
    ASSERT_TRUE(e2.IsValid());
    auto* b2 = w2.GetComponent<Components::BloomEffect>(e2);
    ASSERT_NE(b2, nullptr);
    EXPECT_FLOAT_EQ(b2->ScatteringAmount, 0.35f);
    EXPECT_FALSE(b2->AntiFlicker);
    EXPECT_FLOAT_EQ(b2->Tint[0], 0.2f);
    EXPECT_FLOAT_EQ(b2->Tint[1], 0.4f);
    EXPECT_FLOAT_EQ(b2->Tint[2], 0.8f);
    EXPECT_FLOAT_EQ(b2->Radius, 6.25f);
    EXPECT_EQ(b2->Octaves, 8);
    EXPECT_FLOAT_EQ(b2->Scatter, 0.95f);
    EXPECT_TRUE(b2->DepthVeilEnabled);
    EXPECT_FLOAT_EQ(b2->DepthVeilIntensity, 1.75f);
    EXPECT_FLOAT_EQ(b2->DepthVeilStart, 12.0f);
    EXPECT_FLOAT_EQ(b2->DepthVeilEnd, 640.0f);
    EXPECT_FLOAT_EQ(b2->DepthVeilTint[0], 0.3f);
    EXPECT_FLOAT_EQ(b2->DepthVeilTint[1], 0.6f);
    EXPECT_FLOAT_EQ(b2->DepthVeilTint[2], 0.9f);
    EXPECT_TRUE(b2->LensDirtEnabled);
    EXPECT_TRUE(b2->LensDirtVignette);
    EXPECT_FLOAT_EQ(b2->LensDirtVignetteIntensity, 0.64f);
    EXPECT_FLOAT_EQ(b2->LensDirtVignetteRadius, 0.31f);
    EXPECT_FLOAT_EQ(b2->LensDirtVignetteSmoothness, 0.37f);
    EXPECT_TRUE(b2->LensDirtVignetteRounded);
    EXPECT_FLOAT_EQ(b2->LensDirtVignetteColor[0], 0.25f);
    EXPECT_FLOAT_EQ(b2->LensDirtVignetteColor[1], 0.5f);
    EXPECT_FLOAT_EQ(b2->LensDirtVignetteColor[2], 0.75f);
    EXPECT_FLOAT_EQ(b2->LensDirtIntensity, 10.0f);
    EXPECT_FLOAT_EQ(b2->LensDirtScatter, 0.73f);
    EXPECT_EQ(b2->LensDirtTexture.ToGuid(), bloom.LensDirtTexture.ToGuid());

    // A scene saved before the scatter, Anti Flicker, and lens-dirt toggle
    // fields receives their compatibility defaults.
    {
        std::ifstream in(scenePath, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const size_t dirtGuidPos = body.find(kCustomDirtGuid);
        ASSERT_NE(dirtGuidPos, std::string::npos);
        body.replace(dirtGuidPos, kCustomDirtGuid.size(), kLegacyBuiltInDirtGuid);
        std::istringstream iss(body);
        std::ostringstream oss;
        for (std::string line; std::getline(iss, line);)
            if (line.find("BloomEffect.scatter") == std::string::npos &&
                line.find("BloomEffect.depthVeil") == std::string::npos &&
                line.find("BloomEffect.antiFlicker") == std::string::npos &&
                line.find("BloomEffect.tint") == std::string::npos &&
                line.find("BloomEffect.lensDirtEnabled") == std::string::npos &&
                line.find("BloomEffect.lensDirtVignette") == std::string::npos)
                oss << line << "\n";
        std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
        out << oss.str();
    }
    ECS::World w3;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w3, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto* b3 = w3.GetComponent<Components::BloomEffect>(FindByTag(w3, "blm"));
    ASSERT_NE(b3, nullptr);
    EXPECT_FLOAT_EQ(b3->ScatteringAmount, 0.0f);
    EXPECT_FLOAT_EQ(b3->Scatter, 0.5f);
    EXPECT_FALSE(b3->DepthVeilEnabled);
    EXPECT_FLOAT_EQ(b3->DepthVeilIntensity, 1.0f);
    EXPECT_FLOAT_EQ(b3->DepthVeilStart, 25.0f);
    EXPECT_FLOAT_EQ(b3->DepthVeilEnd, 500.0f);
    EXPECT_FLOAT_EQ(b3->DepthVeilTint[0], 1.0f);
    EXPECT_FLOAT_EQ(b3->DepthVeilTint[1], 1.0f);
    EXPECT_FLOAT_EQ(b3->DepthVeilTint[2], 1.0f);
    EXPECT_TRUE(b3->AntiFlicker);
    EXPECT_FLOAT_EQ(b3->Tint[0], 1.0f);
    EXPECT_FLOAT_EQ(b3->Tint[1], 1.0f);
    EXPECT_FLOAT_EQ(b3->Tint[2], 1.0f);
    EXPECT_FALSE(b3->LensDirtEnabled);
    EXPECT_FALSE(b3->LensDirtVignette);
    EXPECT_FLOAT_EQ(b3->LensDirtVignetteIntensity, 1.0f);
    EXPECT_FLOAT_EQ(b3->LensDirtVignetteRadius, 0.25f);
    EXPECT_FLOAT_EQ(b3->LensDirtVignetteSmoothness, 0.2f);
    EXPECT_FALSE(b3->LensDirtVignetteRounded);
    EXPECT_FLOAT_EQ(b3->LensDirtVignetteColor[0], 1.0f);
    EXPECT_FLOAT_EQ(b3->LensDirtVignetteColor[1], 1.0f);
    EXPECT_FLOAT_EQ(b3->LensDirtVignetteColor[2], 1.0f);
    EXPECT_TRUE(b3->LensDirtTexture.IsNull());
}

// The CubeLutEffect scene schema round-trips the LDR-stack .cube grade the Unity converter emits
// (enabled/stackOrder/intensity/inputEncoding/textureFormat + the .cube AssetRef). Without a
// resolver the GUID recovers as a degraded reference, which is enough to prove the schema binds
// every field.
TEST(SceneIO, SaveThenLoad_RoundTripCubeLutEffect)
{
    const auto scenePath = MakeTempPath("cube_lut.scene");

    const GUID lutGuid = GUID::Generate();
    ASSERT_FALSE(lutGuid.IsNull());

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'l', 'u', 't', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    Components::CubeLutEffect lut{};
    lut.Enabled = true;
    lut.StackOrder = 2;
    lut.Intensity = 0.75f;
    lut.InputEncoding = static_cast<uint32>(Components::CubeLutInputEncoding::ArriLogC3);
    lut.TextureFormat = static_cast<uint32>(Components::CubeLutTextureFormat::R16G16B16A16_FLOAT);
    lut.LutAssetGuid.Set(lutGuid);
    w1.AddComponentImmediate(e, lut);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "lut");
    ASSERT_TRUE(e2.IsValid());
    auto* l2 = w2.GetComponent<Components::CubeLutEffect>(e2);
    ASSERT_NE(l2, nullptr);
    EXPECT_TRUE(l2->Enabled);
    EXPECT_EQ(l2->StackOrder, 2);
    EXPECT_FLOAT_EQ(l2->Intensity, 0.75f);
    EXPECT_EQ(l2->InputEncoding, static_cast<uint32>(Components::CubeLutInputEncoding::ArriLogC3));
    EXPECT_EQ(l2->TextureFormat, static_cast<uint32>(Components::CubeLutTextureFormat::R16G16B16A16_FLOAT));
    EXPECT_EQ(l2->LutAssetGuid.ToGuid(), lutGuid);
}

TEST(SceneIO, SaveThenLoad_RoundTripAtmosphericCloudOpacity)
{
    const auto scenePath = MakeTempPath("atmospheric_cloud.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'c', 'l', 'o', 'u', 'd', '\0'}});

    Components::AtmosphericCloudLayer cloud{};
    cloud.CloudColor[0] = 2.0f;
    cloud.CloudColor[1] = 2.0f;
    cloud.CloudColor[2] = 2.0f;
    cloud.Opacity = 4.5f;
    w1.AddComponentImmediate(e, cloud);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}));
    const ECS::EntityHandle e2 = FindByTag(w2, "cloud");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::AtmosphericCloudLayer>(e2);
    ASSERT_NE(loaded, nullptr);
    EXPECT_FLOAT_EQ(loaded->CloudColor[0], 2.0f);
    EXPECT_FLOAT_EQ(loaded->Opacity, 4.5f);
}

TEST(SceneIO, SaveThenLoad_RoundTripVolumetricClouds)
{
    const auto scenePath = MakeTempPath("volumetric_clouds.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'v', 'c', 'l', 'd', '\0'}});

    Components::VolumetricClouds clouds{};
    clouds.Enabled = true;
    clouds.Radius = 7250.0f;
    clouds.Altitude = 1250.0f;
    clouds.Thickness = 480.0f;
    clouds.NumStepsLight = 12;
    clouds.StepSize = 7.5f;
    clouds.DensityMultiplier = 1.6f;
    clouds.DensityOffset = -3.25f;
    clouds.ShapeOffset[0] = 10.0f;
    clouds.ShapeOffset[1] = -20.0f;
    clouds.ShapeOffset[2] = 30.0f;
    clouds.ShapeNoiseWeights[0] = 1.0f;
    clouds.ShapeNoiseWeights[1] = 0.625f;
    clouds.ShapeNoiseWeights[2] = 0.25f;
    clouds.ShapeNoiseWeights[3] = 0.125f;
    clouds.DetailNoiseWeights[2] = 0.75f;
    clouds.ForwardScattering = 0.5f;
    clouds.HistoryWeight = 0.9f;
    w1.AddComponentImmediate(e, clouds);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}));
    const ECS::EntityHandle e2 = FindByTag(w2, "vcld");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::VolumetricClouds>(e2);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->Enabled);
    EXPECT_FLOAT_EQ(loaded->Radius, 7250.0f);
    EXPECT_FLOAT_EQ(loaded->Altitude, 1250.0f);
    EXPECT_FLOAT_EQ(loaded->Thickness, 480.0f);
    EXPECT_EQ(loaded->NumStepsLight, 12);
    EXPECT_FLOAT_EQ(loaded->StepSize, 7.5f);
    EXPECT_FLOAT_EQ(loaded->DensityMultiplier, 1.6f);
    EXPECT_FLOAT_EQ(loaded->DensityOffset, -3.25f);
    EXPECT_FLOAT_EQ(loaded->ShapeOffset[1], -20.0f);
    EXPECT_FLOAT_EQ(loaded->ShapeNoiseWeights[1], 0.625f);
    EXPECT_FLOAT_EQ(loaded->ShapeNoiseWeights[3], 0.125f);
    EXPECT_FLOAT_EQ(loaded->DetailNoiseWeights[2], 0.75f);
    EXPECT_FLOAT_EQ(loaded->ForwardScattering, 0.5f);
    EXPECT_FLOAT_EQ(loaded->HistoryWeight, 0.9f);
}

// Enabled=false is the state that gates the whole effect off, and it is the
// one value whose default is true — a round-trip that silently restored the
// default would re-enable a disabled effect on every scene load.
TEST(SceneIO, SaveThenLoad_RoundTripVolumetricCloudsDisabled)
{
    const auto scenePath = MakeTempPath("volumetric_clouds_off.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'v', 'c', 'o', 'f', 'f', '\0'}});

    Components::VolumetricClouds clouds{};
    clouds.Enabled = false;
    clouds.DensityMultiplier = 1.25f;
    w1.AddComponentImmediate(e, clouds);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}));
    const ECS::EntityHandle e2 = FindByTag(w2, "vcoff");
    ASSERT_TRUE(e2.IsValid());
    const auto* loaded = w2.GetComponent<Components::VolumetricClouds>(e2);
    ASSERT_NE(loaded, nullptr);
    EXPECT_FALSE(loaded->Enabled);
    EXPECT_FLOAT_EQ(loaded->DensityMultiplier, 1.25f);
}

// A recordless GUID that a redirect moved must load as the redirect's target,
// not the authored GUID: derived subasset identities (embedded clips, bridge
// materials) never have registry records, so after a container rename the
// cascade redirect is the only route from the persisted GUID to the identity
// the reloaded container re-derives (TryResolveAssetReference Path 3;
// reconcile design §7, B2 slice).
TEST(SceneIO, AssetReferenceRecordlessGuidFollowsRedirect)
{
    FakeAssetResolver resolver;
    resolver.SetRoot(std::filesystem::temp_directory_path());
    const GUID oldModel = GUID::Derive(GUID::Null(), "b2/model_old");
    const GUID newModel = GUID::Derive(GUID::Null(), "b2/model_new");
    const GUID oldClip = GUID::Derive(oldModel, "embedded:0");
    const GUID newClip = GUID::Derive(newModel, "embedded:0");
    resolver.AddRedirect(oldClip, newClip); // cascade redirect; neither side has a record

    Scene::SceneLoadContext ctx{};
    ctx.Resolver = &resolver;

    Scene::SceneValue sv{};
    sv.Kind = Scene::SceneValueKind::String;
    sv.StringValue = oldClip.ToString();

    AssetReference ref{};
    std::string err;
    ASSERT_TRUE(Scene::TryResolveAssetReference(ctx, sv, AssetType::Animation, ref, &err)) << err;
    EXPECT_EQ(ref.guid, newClip) << "degraded fallback dropped the redirect chase";

    // Control: an unredirected recordless GUID degrades to itself.
    Scene::SceneValue sv2{};
    sv2.Kind = Scene::SceneValueKind::String;
    sv2.StringValue = oldModel.ToString();
    AssetReference ref2{};
    ASSERT_TRUE(Scene::TryResolveAssetReference(ctx, sv2, AssetType::Unknown, ref2, &err)) << err;
    EXPECT_EQ(ref2.guid, oldModel);
}

namespace
{
// Scene text in the exact shape the Unity converter authors: a path-only
// lutAsset ref ([path="..." guid=""]) whose relative path SceneIO resolves
// against the ASSET ROOT (TryResolveAssetReference Path 2).
std::string MakeAuthoredCubeLutScene(const char* lutPath)
{
    return std::string("[scene name=\"x\" version=1]\n\n"
                       "[entity id=\"lut\"]\n"
                       "CubeLutEffect.enabled = true\n"
                       "CubeLutEffect.stackOrder = 0\n"
                       "CubeLutEffect.intensity = 1\n"
                       "CubeLutEffect.inputEncoding = 0\n"
                       "CubeLutEffect.lutAsset = [path=\"") +
           lutPath + "\" guid=\"\"]\n";
}
} // namespace

// Authored form: the .cube exists under the asset root, so the loader mints a
// GUID for it via the resolver and the component comes back bound.
TEST(SceneIO, LoadCubeLutEffect_AuthoredRelativePath_ResolvesAgainstAssetRoot)
{
    const auto root = std::filesystem::temp_directory_path() / "ge_cubelut_authored";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const auto scenePath = root / "main.scene";
    const auto lutAbs = (root / "demo_smh_lut.cube").lexically_normal();
    ASSERT_TRUE(WriteFile(lutAbs, "TITLE \"t\"\nLUT_3D_SIZE 2\n"));
    ASSERT_TRUE(WriteFile(scenePath, MakeAuthoredCubeLutScene("demo_smh_lut.cube")));

    FakeAssetResolver resolver;
    resolver.SetRoot(root);
    resolver.SetMintRequiresFileOnDisk(true);

    Scene::LoadOptions opts;
    opts.mode = Scene::LoadMode::Replace;
    opts.assetResolver = &resolver;
    opts.assetRootOverride = root;

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, opts));
    auto e = FindByTag(w, "lut");
    ASSERT_TRUE(e.IsValid());
    auto* c = w.GetComponent<Components::CubeLutEffect>(e);
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->Enabled);
    ASSERT_FALSE(c->LutAssetGuid.ToGuid().IsNull());

    // The stored GUID is the one minted for <assetRoot>/demo_smh_lut.cube.
    GUID minted = GUID::Null();
    AssetType mintedType = AssetType::Unknown;
    ASSERT_TRUE(resolver.TryGetGuidAndType(lutAbs, minted, mintedType));
    EXPECT_EQ(c->LutAssetGuid.ToGuid(), minted);

    std::filesystem::remove_all(root, ec);
}

// Authored form, path misses: no file under <assetRoot>/<path> means the resolver
// can't bind a GUID (GetOrCreateAssetGuid returns Null, as the registry-backed
// editor resolver does for paths not on disk), and with no authored GUID either
// the property cannot resolve. This is the exact failure a project-root-relative
// "assets/X.cube" produced: it resolved to <assetRoot>/assets/X.cube, which doesn't exist.
//
// One unresolvable asset reference no longer costs the caller the scene: the property is skipped and
// censused, and everything else in the file loads.
TEST(SceneIO, LoadCubeLutEffect_AuthoredPathMissing_DegradesWithResolveError)
{
    const auto root = std::filesystem::temp_directory_path() / "ge_cubelut_missing";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const auto scenePath = root / "main.scene";
    ASSERT_TRUE(WriteFile(scenePath, MakeAuthoredCubeLutScene("assets/demo_smh_lut.cube")));

    FakeAssetResolver resolver;
    resolver.SetRoot(root);
    resolver.SetMintRequiresFileOnDisk(true);

    Scene::LoadOptions opts;
    opts.mode = Scene::LoadMode::Replace;
    opts.assetResolver = &resolver;
    opts.assetRootOverride = root;

    ECS::World w;
    Scene::SceneLoadDegradation degradation;
    opts.outDegradation = &degradation;
    EXPECT_TRUE(Scene::LoadSceneFromFile(w, scenePath, opts));

    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].component, "CubeLutEffect");
    EXPECT_EQ(degradation.skips[0].field, "lutAsset");
    EXPECT_NE(degradation.skips[0].message.find("cannot resolve"), std::string::npos)
        << degradation.skips[0].message;

    std::filesystem::remove_all(root, ec);
}

// A scene shipped on a secondary mount (the editor's, for MaterialLookdev.scene)
// names its assets by mount-relative path. Those bind wherever a mount supplies
// them, in the order every unprefixed asset path resolves: project first, then
// the other mounts — so an editor-mount asset binds, and a project asset at the
// same relative path still wins over the editor's copy.
TEST(SceneIO, LoadFromSecondaryMount_PathRefsResolveProjectFirstThenOtherMounts)
{
    const auto root = std::filesystem::temp_directory_path() / "ge_mount_order_refs";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto projectRoot = root / "project";
    const auto editorRoot = root / "editor";
    std::filesystem::create_directories(projectRoot / "Luts", ec);
    std::filesystem::create_directories(editorRoot / "Luts", ec);
    std::filesystem::create_directories(editorRoot / "Examples" / "Luts", ec);
    std::filesystem::create_directories(editorRoot / "Examples" / "Scenes", ec);

    const char* lut = "TITLE \"t\"\nLUT_3D_SIZE 2\n";
    const auto editorOnly = (editorRoot / "Examples" / "Luts" / "editor_only.cube").lexically_normal();
    const auto projectOnly = (projectRoot / "Luts" / "project_only.cube").lexically_normal();
    const auto shadowedProject = (projectRoot / "Luts" / "shadowed.cube").lexically_normal();
    const auto shadowedEditor = (editorRoot / "Luts" / "shadowed.cube").lexically_normal();
    ASSERT_TRUE(WriteFile(editorOnly, lut));
    ASSERT_TRUE(WriteFile(projectOnly, lut));
    ASSERT_TRUE(WriteFile(shadowedProject, lut));
    ASSERT_TRUE(WriteFile(shadowedEditor, lut));

    const auto scenePath = editorRoot / "Examples" / "Scenes" / "lookdev.scene";
    ASSERT_TRUE(WriteFile(scenePath,
                          "[scene name=\"x\" version=1]\n\n"
                          "[entity id=\"on_editor\"]\n"
                          "CubeLutEffect.lutAsset = [path=\"Examples/Luts/editor_only.cube\"]\n\n"
                          "[entity id=\"on_project\"]\n"
                          "CubeLutEffect.lutAsset = [path=\"Luts/project_only.cube\"]\n\n"
                          "[entity id=\"shadowed\"]\n"
                          "CubeLutEffect.lutAsset = [path=\"Luts/shadowed.cube\"]\n"));

    FakeAssetResolver resolver;
    resolver.SetRoot(projectRoot);
    resolver.AddMountRoot(editorRoot);
    resolver.SetMintRequiresFileOnDisk(true);

    Scene::LoadOptions opts;
    opts.mode = Scene::LoadMode::Replace;
    opts.assetResolver = &resolver;
    opts.assetRootOverride = projectRoot;
    Scene::SceneLoadDegradation degradation;
    opts.outDegradation = &degradation;

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, opts));
    EXPECT_TRUE(degradation.skips.empty())
        << degradation.skips.size() << " ref(s) skipped, first: "
        << (degradation.skips.empty() ? std::string{} : degradation.skips[0].message);

    const auto boundTo = [&](const char* tag) -> GUID
    {
        const auto e = FindByTag(w, tag);
        if (!e.IsValid())
            return GUID::Null();
        const auto* c = w.GetComponent<Components::CubeLutEffect>(e);
        return c ? c->LutAssetGuid.ToGuid() : GUID::Null();
    };
    const auto mintedFor = [&](const std::filesystem::path& abs) -> GUID
    {
        GUID g = GUID::Null();
        AssetType t = AssetType::Unknown;
        return resolver.TryGetGuidAndType(abs, g, t) ? g : GUID::Null();
    };

    EXPECT_FALSE(boundTo("on_editor").IsNull());
    EXPECT_EQ(boundTo("on_editor"), mintedFor(editorOnly));
    EXPECT_EQ(boundTo("on_project"), mintedFor(projectOnly));
    EXPECT_EQ(boundTo("shadowed"), mintedFor(shadowedProject));
    EXPECT_TRUE(mintedFor(shadowedEditor).IsNull()) << "the editor copy of a shadowed path must not be minted";

    std::filesystem::remove_all(root, ec);
}

// [resource] paths follow the same mount order: a blueprint shipped next to an
// editor-mount scene instantiates from the editor mount.
TEST(SceneIO, LoadFromSecondaryMount_ResourcePathResolvesOnThatMount)
{
    const auto root = std::filesystem::temp_directory_path() / "ge_mount_order_resource";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto projectRoot = root / "project";
    const auto editorRoot = root / "editor";
    std::filesystem::create_directories(projectRoot, ec);
    std::filesystem::create_directories(editorRoot / "Examples" / "Blueprints", ec);
    std::filesystem::create_directories(editorRoot / "Examples" / "Scenes", ec);

    ASSERT_TRUE(WriteFile(editorRoot / "Examples" / "Blueprints" / "bp.blueprint",
                          "[blueprint name=\"BP\" version=1]\n\n"
                          "[entity id=\"root\"]\n"
                          "Name.value = \"BP\"\n"));
    const auto scenePath = editorRoot / "Examples" / "Scenes" / "with_bp.scene";
    ASSERT_TRUE(WriteFile(scenePath,
                          "[scene name=\"S\" version=1]\n"
                          "[resource id=\"bp\" path=\"Examples/Blueprints/bp.blueprint\"]\n\n"
                          "[blueprint id=\"inst\" source=\"bp\"]\n"
                          "Transform.position = (1, 0, 0)\n"));

    FakeAssetResolver resolver;
    resolver.SetRoot(projectRoot);
    resolver.AddMountRoot(editorRoot);

    Scene::LoadOptions opts;
    opts.mode = Scene::LoadMode::Replace;
    opts.assetResolver = &resolver;
    opts.assetRootOverride = projectRoot;

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, opts));
    EXPECT_TRUE(FindByTag(w, "inst").IsValid());

    std::filesystem::remove_all(root, ec);
}

namespace
{
// A blueprint instance whose source file does not exist: the scene parses and validates cleanly (the
// [resource] id it names IS declared, which is all ValidateDocument checks), and the read fails
// inside LoadParsedIntoWorld — after World::Clear.
//
// This used to be an unknown component property. It cannot be any more: component and field errors
// are now collected and skipped rather than aborting the load (see the degraded-load tests below), so
// exercising the post-clear failure path requires a failure the loader genuinely cannot continue
// past. The entity comes first, so the abort still strands it in the world.
const char* const kSceneFailingAfterInstantiateBegins =
    "[scene name=\"PartialLoad\" version=1]\n"
    "[resource id=\"bp\" path=\"no_such_blueprint.blueprint\"]\n"
    "\n"
    "[entity id=\"good_one\"]\n"
    "Transform.position = (1, 2, 3)\n"
    "\n"
    "[blueprint id=\"bad_one\" source=\"bp\"]\n";
} // namespace

// The failure that strands a half-loaded world says so. Nothing else in the API
// can tell a caller whether the world it is holding survived: the return is a
// bare false, and every phase before the clear fails the same way.
TEST(SceneIO, ReplaceLoadFailingAfterClear_ReportsTheWorldWasCleared)
{
    const auto scenePath = MakeTempPath("partial_after_clear.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneFailingAfterInstantiateBegins));

    // The outgoing scene, tagged so its survival is checkable by identity. A
    // handle would not do: Clear restarts entity versions, so a stale handle can
    // pass IsValid against an unrelated incoming entity.
    ECS::World world;
    const ECS::EntityHandle outgoing = world.CreateEntity();
    world.AddComponentImmediate(outgoing, Components::SceneEntityTag{"outgoing_scene_entity"});
    world.ProcessCommands();
    ASSERT_TRUE(FindByTag(world, "outgoing_scene_entity").IsValid());

    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto& err = Scene::GetLastSceneIOError();
    EXPECT_TRUE(err.worldCleared)
        << "the clear ran before the instantiate failed, so the caller's scene is gone: " << err.message;
    EXPECT_NE(err.message.find("Blueprint file not found"), std::string::npos) << err.message;

    // The damage the flag is claiming, asserted rather than assumed: the
    // outgoing scene is gone, and what replaced it is a half-applied incoming
    // one — the entity instantiated before the abort is complete, and the
    // blueprint instance the load aborted on never arrived at all.
    EXPECT_FALSE(FindByTag(world, "outgoing_scene_entity").IsValid())
        << "the previous scene must be gone — that is what makes this failure different";
    const ECS::EntityHandle good = FindByTag(world, "good_one");
    ASSERT_TRUE(good.IsValid());
    EXPECT_NE(world.GetComponent<Components::Transform>(good), nullptr);
    EXPECT_FALSE(FindByTag(world, "bad_one").IsValid())
        << "the blueprint instance the load aborted on must not be in the world";
}

// A missing blueprint is an error IN THE SCENE: the line the error names is a line of the scene file,
// so the file must be the scene, not the blueprint path that does not exist. The message carries
// what the line alone cannot: which file is missing, the resource that names it, and what to do.
TEST(SceneIO, MissingBlueprintIsReportedAtTheSceneLineWithThePathAndTheFix)
{
    const auto scenePath = MakeTempPath("missing_blueprint.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneFailingAfterInstantiateBegins));

    // With no asset root the resource resolves next to the scene, so the scene's directory stands in
    // for the project: the path the loader searched is spelled relative to it.
    Scene::LoadOptions options{Scene::LoadMode::Replace};
    options.projectRoot = scenePath.parent_path();
    ECS::World world;
    ASSERT_FALSE(Scene::LoadSceneFromFile(world, scenePath, options));

    const auto& err = Scene::GetLastSceneIOError();
    EXPECT_EQ(err.file.lexically_normal(), scenePath.lexically_normal())
        << "line " << err.line << " is a line of the scene, so the file is the scene: " << err.file;
    EXPECT_EQ(err.line, 7) << "the [blueprint] instance the load stopped on";
    EXPECT_NE(err.message.find("Blueprint file not found at no_such_blueprint.blueprint."), std::string::npos)
        << "where the loader looked, relative to the project: " << err.message;
    EXPECT_EQ(err.message.find(scenePath.parent_path().generic_string()), std::string::npos)
        << "the absolute path belongs in the log, not the message: " << err.message;
    EXPECT_NE(err.message.find("The blueprint on line 7 uses resource \"bp\", which line 2 declares."),
              std::string::npos)
        << "both lines involved, the instance and the declaration: " << err.message;
    EXPECT_NE(err.message.find("Restore the file, change line 2, or remove the blueprint on line 7"),
              std::string::npos)
        << "the cause alone leaves the user to guess the fix: " << err.message;
}

// Outside the project, or with no project named, a relative spelling would point nowhere: the
// searched path stays absolute.
TEST(SceneIO, MissingBlueprintOutsideTheProjectNamesTheAbsolutePath)
{
    const auto scenePath = MakeTempPath("missing_blueprint_no_project.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneFailingAfterInstantiateBegins));

    ECS::World world;
    ASSERT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto& err = Scene::GetLastSceneIOError();
    const std::string searched = (scenePath.parent_path() / "no_such_blueprint.blueprint").lexically_normal().generic_string();
    EXPECT_NE(err.message.find("Blueprint file not found at " + searched + "."), std::string::npos)
        << err.message;
}

// Both entities of a duplicate id are named, and the fix is stated.
TEST(SceneIO, DuplicateEntityIdNamesBothLinesAndTheFix)
{
    const auto scenePath = MakeTempPath("duplicate_ids.scene");
    ASSERT_TRUE(WriteFile(scenePath,
                          "[scene name=\"Dupes\" version=1]\n"
                          "\n"
                          "[entity id=\"same\"]\n"
                          "Transform.position = (0, 0, 0)\n"
                          "\n"
                          "[entity id=\"same\"]\n"
                          "Transform.position = (1, 1, 1)\n"));

    ECS::World world;
    ASSERT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto& err = Scene::GetLastSceneIOError();
    EXPECT_EQ(err.line, 6);
    EXPECT_NE(err.message.find("Duplicate entity id \"same\". Line 3 already uses this id."),
              std::string::npos)
        << err.message;
    EXPECT_NE(err.message.find("Give one of the two entities a different id."), std::string::npos)
        << err.message;
}

// The mirror case, and the reason the flag cannot just be "mode == Replace":
// a failure raised before the clear leaves the world untouched.
//
// The cleared failure that runs first is the arm, not the setup. The flag is
// false by default and a pre-clear failure returns before anything assigns it,
// so against a fresh error record this EXPECT_FALSE holds whether the flag is
// computed or merely never written. Only a stale true carried in from the load
// before it separates those, which is what makes the per-load reset a tested
// contract rather than an implementation detail.
TEST(SceneIO, ReplaceLoadFailingBeforeClear_LeavesTheWorldAndSaysSo)
{
    const auto clearedPath = MakeTempPath("arms_stale_cleared_flag.scene");
    ASSERT_TRUE(WriteFile(clearedPath, kSceneFailingAfterInstantiateBegins));

    const auto scenePath = MakeTempPath("fails_before_clear.scene");
    // Duplicate entity ids fail ValidateDocument, which runs before World::Clear.
    ASSERT_TRUE(WriteFile(scenePath,
                          "[scene name=\"Dupes\" version=1]\n"
                          "\n"
                          "[entity id=\"same\"]\n"
                          "Transform.position = (0, 0, 0)\n"
                          "\n"
                          "[entity id=\"same\"]\n"
                          "Transform.position = (1, 1, 1)\n"));

    ECS::World world;
    world.CreateEntity();
    world.ProcessCommands();

    ASSERT_FALSE(Scene::LoadSceneFromFile(world, clearedPath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    ASSERT_TRUE(Scene::GetLastSceneIOError().worldCleared);

    const std::size_t before = world.GetEntityCount();

    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto& err = Scene::GetLastSceneIOError();
    EXPECT_FALSE(err.worldCleared)
        << "this load never reached the clear, so the previous load's true must not survive into it: "
        << err.message;
    EXPECT_EQ(world.GetEntityCount(), before) << "a pre-clear failure must not touch the world";
}

// An additive load never clears, whatever it does to its own entities.
TEST(SceneIO, AdditiveLoadFailing_NeverReportsTheWorldWasCleared)
{
    const auto scenePath = MakeTempPath("partial_additive.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneFailingAfterInstantiateBegins));

    ECS::World world;
    world.CreateEntity();
    world.ProcessCommands();

    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Additive}));
    EXPECT_FALSE(Scene::GetLastSceneIOError().worldCleared);
}

// The flag is per-load state, not sticky: a success after a cleared failure must
// not leave the next reader believing the world is a wreck.
TEST(SceneIO, WorldClearedFlagResetsOnTheNextLoad)
{
    const auto badPath = MakeTempPath("sticky_bad.scene");
    ASSERT_TRUE(WriteFile(badPath, kSceneFailingAfterInstantiateBegins));
    const auto goodPath = MakeTempPath("sticky_good.scene");
    ASSERT_TRUE(WriteFile(goodPath,
                          "[scene name=\"Fine\" version=1]\n"
                          "\n"
                          "[entity id=\"only\"]\n"
                          "Transform.position = (0, 0, 0)\n"));

    ECS::World world;
    ASSERT_FALSE(Scene::LoadSceneFromFile(world, badPath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    ASSERT_TRUE(Scene::GetLastSceneIOError().worldCleared);

    ASSERT_TRUE(Scene::LoadSceneFromFile(world, goodPath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    EXPECT_FALSE(Scene::GetLastSceneIOError().worldCleared);
}

// ---------------------------------------------------------------------------
// Degraded loads: a scene this build cannot fully apply still opens.
//
// The canonical case is a stale-codegen binary meeting a scene authored against
// newer reflection tables — an enumerator the running build has never heard of.
// That used to abort the entire load, costing the user every entity in the file
// to save them from one bad word. These pin the replacement contract: the scene
// loads, the damage is enumerated, and the authored text survives a re-save.
// ---------------------------------------------------------------------------
namespace
{
// The canonical stale-codegen case, exactly. SplineFence.SpanGrade is a reflected enum field whose
// enumerator NAMES live in this binary's reflection tables (Racked/Stepped/Sheared), so
// "Cantilevered" is what a scene saved by a NEWER build looks like to an older one. The codec
// rejects it, and the old behaviour was to abort the entire load over that one word.
//
// One bad assignment, with a good field after it on the same component and a wholly good sibling
// entity, so "the load kept going" is provable rather than assumed.
const char* const kSceneWithUnknownEnumValue =
    "[scene name=\"Degraded\" version=1]\n"
    "\n"
    "[entity id=\"fence\"]\n"
    "Transform.position = (1, 2, 3)\n"
    "SplineFence.SpanGrade = Cantilevered\n"
    "SplineFence.Seed = 4242\n"
    "\n"
    "[entity id=\"bystander\"]\n"
    "Transform.position = (9, 9, 9)\n";

// A run saved while SplineExtrude still had a Rectangle profile. A wall is its own recipe now.
const char* const kSceneWithRetiredRectangleExtrude =
    "[scene name=\"RetiredRectangle\" version=1]\n"
    "\n"
    "[entity id=\"rampart\"]\n"
    "SplineExtrude.Profile = Rectangle\n"
    "SplineExtrude.Width = 0.8\n";

// Light is a HAND-WRITTEN schema, not the reflection one, and it rejects a malformed value through
// its own parser. Tolerating a bad value must not be a property of the reflection schema alone.
const char* const kSceneWithBadHandWrittenValue =
    "[scene name=\"DegradedHandWritten\" version=1]\n"
    "\n"
    "[entity id=\"lamp\"]\n"
    "Light.enabled = true\n"
    "Light.type = NotANumber\n"
    "Light.intensity = 7\n";
} // namespace

// The load succeeds and keeps everything it could apply. Only the one bad assignment is missing —
// not the component, not the entity, and above all not the rest of the scene.
TEST(SceneIO, UnknownEnumValueDegradesTheLoadInsteadOfAbortingIt)
{
    const auto scenePath = MakeTempPath("degraded_enum.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;

    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts))
        << "one unreadable enumerator must not cost the caller the whole scene";

    const ECS::EntityHandle fence = FindByTag(world, "fence");
    const ECS::EntityHandle bystander = FindByTag(world, "bystander");
    ASSERT_TRUE(fence.IsValid());
    ASSERT_TRUE(bystander.IsValid()) << "entities after the bad one must still load";

    // Every entity in the file arrived: components skip, entities do not.
    EXPECT_NE(world.GetComponent<Components::Transform>(fence), nullptr);
    EXPECT_NE(world.GetComponent<Components::Transform>(bystander), nullptr);

    // The component carrying the bad field is present, with its GOOD fields applied. The grouped
    // apply parses into a scratch buffer it discards on the first bad value, so without the
    // per-property retry this component would exist holding nothing but defaults.
    const auto* spline = world.GetComponent<Components::SplineFence>(fence);
    ASSERT_NE(spline, nullptr) << "the component itself must survive one bad field";
    EXPECT_EQ(spline->Seed, 4242u) << "a field authored AFTER the bad one must still be applied";

    ASSERT_TRUE(degradation.IsDegraded());
    ASSERT_EQ(degradation.skips.size(), 1u) << "exactly the one bad assignment, not the component";
    const Scene::SceneLoadSkip& skip = degradation.skips[0];
    EXPECT_EQ(skip.entityId, "fence");
    EXPECT_EQ(skip.component, "SplineFence");
    EXPECT_EQ(skip.field, "SpanGrade") << "the census echoes the AUTHORED spelling";
    EXPECT_NE(skip.message.find("Cantilevered"), std::string::npos)
        << "the message must name the value that could not be read: " << skip.message;
    EXPECT_EQ(skip.line, 5) << "the census points at the offending line";
    EXPECT_TRUE(skip.preserved) << "a known field's authored text has a line to survive in";
}

// A scene saved with a Rectangle extrude still opens once the profile is retired: the run loads
// with its other fields, the Profile line is reported by name as a value this build cannot read,
// and its text is kept so a save does not erase it. Nothing converts it into a wall.
TEST(SceneIO, ARetiredRectangleExtrudeLoadsAndReportsItsProfile)
{
    const auto scenePath = MakeTempPath("retired_rectangle_extrude.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithRetiredRectangleExtrude));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));

    const ECS::EntityHandle rampart = FindByTag(world, "rampart");
    ASSERT_TRUE(rampart.IsValid());
    const auto* extrude = world.GetComponent<Components::SplineExtrude>(rampart);
    ASSERT_NE(extrude, nullptr);
    EXPECT_FLOAT_EQ(extrude->Width, 0.8f);
    EXPECT_EQ(extrude->Profile, Components::SplineExtrudeProfile::Bevel);
    EXPECT_EQ(world.GetComponent<Components::SplineWall>(rampart), nullptr);

    ASSERT_EQ(degradation.skips.size(), 1u);
    const Scene::SceneLoadSkip& skip = degradation.skips[0];
    EXPECT_EQ(skip.component, "SplineExtrude");
    EXPECT_EQ(skip.field, "Profile");
    EXPECT_NE(skip.message.find("Rectangle"), std::string::npos) << skip.message;
    EXPECT_TRUE(skip.preserved);
}

// The same contract through a hand-written schema, which parses values with its own code rather than
// the reflected field codecs.
TEST(SceneIO, BadValueOnAHandWrittenSchemaAlsoDegradesInsteadOfAborting)
{
    const auto scenePath = MakeTempPath("degraded_handwritten.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithBadHandWrittenValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;

    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));

    const ECS::EntityHandle lamp = FindByTag(world, "lamp");
    ASSERT_TRUE(lamp.IsValid());
    const auto* light = world.GetComponent<Components::Light>(lamp);
    ASSERT_NE(light, nullptr);
    EXPECT_FLOAT_EQ(light->Intensity, 7.0f)
        << "a field authored AFTER the bad one must still be applied";

    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].component, "Light");
    EXPECT_EQ(degradation.skips[0].field, "type");
}

// A clean scene reports no degradation — the census must not cry wolf, or the save guard it feeds
// becomes noise the user learns to click through.
TEST(SceneIO, CleanLoadReportsNoDegradation)
{
    const auto scenePath = MakeTempPath("degraded_none.scene");
    ASSERT_TRUE(WriteFile(scenePath,
                          "[scene name=\"Fine\" version=1]\n"
                          "\n"
                          "[entity id=\"only\"]\n"
                          "Transform.position = (0, 0, 0)\n"
                          "Light.intensity = 2\n"));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;

    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    EXPECT_FALSE(degradation.IsDegraded());
    EXPECT_EQ(degradation.skips.size(), 0u);
    EXPECT_EQ(degradation.DroppedCount(), 0u);
}

// The point of preserving: load a scene this build cannot fully read, save it, and the authored text
// is still there. Without it, opening a scene in a stale build and pressing Ctrl+S silently rewrites
// the user's data to whatever the field defaulted to.
TEST(SceneIO, DegradedSceneRoundTripsThePreservedTextThroughSave)
{
    const auto scenePath = MakeTempPath("degraded_roundtrip.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    ASSERT_EQ(degradation.skips.size(), 1u);
    ASSERT_TRUE(degradation.skips[0].preserved);

    const auto savedPath = MakeTempPath("degraded_roundtrip_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, savedPath));

    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_NE(saved.find("SplineFence.SpanGrade = Cantilevered"), std::string::npos)
        << "the authored text must be written back, not the default it fell back to:\n"
        << saved;

    // Substituted into the field's own line, not appended: the parser coalesces repeated keys
    // last-write-wins, so a second Light.type line would shadow the real one forever.
    std::size_t typeLines = 0;
    for (std::size_t at = saved.find("SplineFence.SpanGrade"); at != std::string::npos;
         at = saved.find("SplineFence.SpanGrade", at + 1))
        ++typeLines;
    EXPECT_EQ(typeLines, 1u) << "exactly one SplineFence.SpanGrade line:\n" << saved;

    // And it survives the next load the same way, so the file is stable under repeated open/save
    // rather than degrading a little further each time.
    ECS::World reloaded;
    Scene::SceneLoadDegradation again;
    Scene::LoadOptions reopen{Scene::LoadMode::Replace};
    reopen.outDegradation = &again;
    ASSERT_TRUE(Scene::LoadSceneFromFile(reloaded, savedPath, reopen));
    ASSERT_EQ(again.skips.size(), 1u);
    EXPECT_EQ(again.skips[0].field, "SpanGrade");
    EXPECT_TRUE(again.skips[0].preserved);
}

// The preserved text is a fallback for a value nobody has replaced — not a veto over the user. Once
// the field is written for real, that value is the truth and the stale authored text must not be
// reinstated over it on the next save.
TEST(SceneIO, WritingThePreservedFieldSupersedesTheAuthoredText)
{
    const auto scenePath = MakeTempPath("degraded_superseded.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));

    // There has to BE an override for the write to supersede, or the absence asserted below is
    // equally what a dead preservation path produces.
    ASSERT_EQ(degradation.skips.size(), 1u);
    ASSERT_TRUE(degradation.skips[0].preserved);

    const ECS::EntityHandle fence = FindByTag(world, "fence");
    ASSERT_TRUE(fence.IsValid());
    auto* spline = world.GetComponentForWrite<Components::SplineFence>(fence);
    ASSERT_NE(spline, nullptr);
    spline->SpanGrade = Components::SplineSpanGrade::Stepped;

    const auto savedPath = MakeTempPath("degraded_superseded_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, savedPath));

    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_EQ(saved.find("Cantilevered"), std::string::npos)
        << "a real write to the field must win over the preserved text:\n"
        << saved;

    // Positive control. Absence of the authored text is also what a completely dead preservation
    // path produces, so without this the assertion above cannot tell "supersession worked" from
    // "nothing was ever preserved" — and a mutation that silently accepts every field passes it.
    EXPECT_NE(saved.find("SplineFence.SpanGrade = Stepped"), std::string::npos)
        << "the written value itself must reach the file:\n"
        << saved;
}

// The accepted residual, pinned so it is a decision rather than a surprise. A raw write that sets
// the field to exactly the bytes it fell back to is invisible to a field-scoped comparison, so the
// authored text still wins the save. Closing this would mean a broader signal, and every broader
// signal is tripped by edits that never touched this field (adding a component to the entity,
// editing a sibling field) — which retires live overrides and loses real data.
//
// Benign as it stands: substituting over a value identical to the fallback leaves the world exactly
// where treating that write as a no-op would. A user who genuinely means to keep the fallback says
// so through the inspector's discard action, which the next test covers.
TEST(SceneIO, WritingTheFallbackValueItselfIsNotVisibleToTheByteComparison)
{
    const auto scenePath = MakeTempPath("degraded_accept_fallback.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    ASSERT_EQ(degradation.skips.size(), 1u);
    ASSERT_TRUE(degradation.skips[0].preserved);

    const ECS::EntityHandle fence = FindByTag(world, "fence");
    ASSERT_TRUE(fence.IsValid());
    const auto* loaded = world.GetComponent<Components::SplineFence>(fence);
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->SpanGrade, Components::SplineSpanGrade::Racked)
        << "the rejected enumerator must have fallen back to the default this test then rewrites";

    auto* spline = world.GetComponentForWrite<Components::SplineFence>(fence);
    ASSERT_NE(spline, nullptr);
    spline->SpanGrade = Components::SplineSpanGrade::Racked; // same bytes back

    const auto savedPath = MakeTempPath("degraded_accept_fallback_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, savedPath));

    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_NE(saved.find("SplineFence.SpanGrade = Cantilevered"), std::string::npos)
        << "documented residual: a write of the fallback value cannot be distinguished from no "
           "write at all, and the authored text still round-trips:\n"
        << saved;
}

// Discarding is how a user says "the fallback is what I want" in a way the save can act on.
TEST(SceneIO, DiscardingThePreservedFieldLetsTheLiveValueReachTheFile)
{
    const auto scenePath = MakeTempPath("degraded_discard.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    ASSERT_EQ(degradation.skips.size(), 1u);
    ASSERT_TRUE(degradation.skips[0].preserved);

    const ECS::EntityHandle fence = FindByTag(world, "fence");
    ASSERT_TRUE(fence.IsValid());

    const ECS::ComponentTypeId splineType =
        ECS::ComponentFieldRegistry::FindByName("splinefence");
    ASSERT_NE(splineType, 0u);
    EXPECT_TRUE(world.GetUnresolvedComponents().DiscardField(fence, splineType, "SpanGrade"))
        << "the entry the census reported must be the one the discard finds";

    const auto savedPath = MakeTempPath("degraded_discard_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, savedPath));

    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_EQ(saved.find("Cantilevered"), std::string::npos)
        << "a discarded override must not come back:\n"
        << saved;
    EXPECT_NE(saved.find("SplineFence.SpanGrade = Racked"), std::string::npos)
        << "and the live value is what lands in the file:\n"
        << saved;

    // The census is the LOAD's record and cannot see a discard, so the flag it captured still
    // reads true. Every surface that reports on what a SAVE will do has to ask the store instead,
    // or it goes on offering to write back text that is no longer there.
    EXPECT_TRUE(degradation.skips[0].preserved)
        << "the load-time record is a record; it is not expected to mutate";
    EXPECT_FALSE(Scene::SkipIsOutstanding(world, degradation.skips[0]))
        << "but the live question — will a save still write this authored text — is now no";
    EXPECT_EQ(Scene::OutstandingSkipCount(world, degradation), 0u);
}

// Destroying the entity takes the component instances the overrides describe with it, and the
// handle goes back into circulation — so the overrides must go too, or the next entity to be
// handed that index inherits a dead scene's authored text.
TEST(SceneIO, DestroyingTheEntityErasesItsPreservedFields)
{
    const auto scenePath = MakeTempPath("degraded_destroy.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    ASSERT_EQ(degradation.skips.size(), 1u);
    ASSERT_TRUE(degradation.skips[0].preserved);

    const ECS::EntityHandle fence = FindByTag(world, "fence");
    ASSERT_TRUE(fence.IsValid());
    ASSERT_NE(world.GetUnresolvedComponents().FieldsFor(fence), nullptr);

    world.DestroyEntityImmediate(fence);

    EXPECT_EQ(world.GetUnresolvedComponents().FieldsFor(fence), nullptr)
        << "the handle is back in freeIndices; its overrides must not outlive it";
    EXPECT_FALSE(Scene::SkipIsOutstanding(world, degradation.skips[0]))
        << "and nothing a save writes can carry that text any more";
}

// The editor's delete is undo/redo PARKING, not a release: the same EntityHandle comes back on
// undo. Erasing the overrides here would lose the authored text across a delete-then-undo — the
// exact loss the side table exists to prevent — so this destroy is exempt.
TEST(SceneIO, DeleteThenUndoKeepsTheAuthoredText)
{
    const auto scenePath = MakeTempPath("degraded_delete_undo.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    ASSERT_EQ(degradation.skips.size(), 1u);
    ASSERT_TRUE(degradation.skips[0].preserved);

    const ECS::EntityHandle fence = FindByTag(world, "fence");
    ASSERT_TRUE(fence.IsValid());

    // What DeleteEntitiesCommand does: snapshot the component bytes, destroy preserving the
    // handle, then revive and replay the bytes.
    ECS::Archetype* archetype = world.GetEntityArchetype(fence);
    ASSERT_NE(archetype, nullptr);
    std::vector<std::pair<ECS::ComponentTypeId, std::vector<std::uint8_t>>> snapshot;
    for (const auto typeId : archetype->GetSignature().GetComponents())
    {
        std::vector<std::uint8_t> bytes;
        if (world.CaptureComponentBytes(fence, typeId, bytes))
            snapshot.emplace_back(typeId, std::move(bytes));
    }
    ASSERT_FALSE(snapshot.empty());

    world.DestroyEntityImmediatePreserveHandle(fence);

    ASSERT_NE(world.GetUnresolvedComponents().FieldsFor(fence), nullptr)
        << "parking the handle for undo must not drop the authored text";

    ASSERT_TRUE(world.ReviveEntityImmediatePreserveHandle(fence));
    for (const auto& [typeId, bytes] : snapshot)
        ASSERT_TRUE(world.ApplyComponentBytesImmediate(fence, typeId, bytes));

    const auto savedPath = MakeTempPath("degraded_delete_undo_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, savedPath));

    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_NE(saved.find("SplineFence.SpanGrade = Cantilevered"), std::string::npos)
        << "the authored text has to survive the whole delete-then-undo round trip:\n"
        << saved;
    EXPECT_TRUE(Scene::SkipIsOutstanding(world, degradation.skips[0]))
        << "and the census must still say a save writes it back";
}

// Removing the component destroys the instance the override describes; a later re-add is a NEW
// component holding defaults, and the previous load's text must not reappear on it.
TEST(SceneIO, RemovingTheComponentRetiresItsPreservedFields)
{
    const auto scenePath = MakeTempPath("degraded_component_remove.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    ASSERT_EQ(degradation.skips.size(), 1u);

    const ECS::EntityHandle fence = FindByTag(world, "fence");
    ASSERT_TRUE(fence.IsValid());
    ASSERT_TRUE(Scene::SkipIsOutstanding(world, degradation.skips[0]));

    world.RemoveComponentImmediate<Components::SplineFence>(fence);

    EXPECT_FALSE(Scene::SkipIsOutstanding(world, degradation.skips[0]))
        << "the component the override described is gone; a save cannot write it back";
}

// The filter's safety property. Only PRESERVED rows can retire — a row whose text was never kept
// describes text still sitting in the file that this build cannot read, and a save still drops it.
// Deriving that one from the store would delete every real data-loss warning the census carries.
TEST(SceneIO, ASkipThatWasNeverPreservedIsAlwaysOutstanding)
{
    ECS::World world; // deliberately empty: nothing in the store can vouch for this row

    Scene::SceneLoadSkip dropped{};
    dropped.entityId = "lamp";
    dropped.component = "Light";
    dropped.field = "type";
    dropped.preserved = false;

    EXPECT_TRUE(Scene::SkipIsOutstanding(world, dropped));

    Scene::SceneLoadDegradation census;
    census.skips.push_back(dropped);
    EXPECT_EQ(Scene::OutstandingSkipCount(world, census), 1u);
    EXPECT_EQ(census.DroppedCount(), 1u);
}

// v2's killer #1, as a standing test. The editor adds components to scene entities after every load
// (WorldTransform, among others). That relocates the row without touching the preserved component's
// bytes, so the override must survive it. A column-version signal died here on every single load.
TEST(SceneIO, AddingAnotherComponentToTheEntityPreservesTheAuthoredText)
{
    const auto scenePath = MakeTempPath("degraded_component_add.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    ASSERT_EQ(degradation.skips.size(), 1u);
    ASSERT_TRUE(degradation.skips[0].preserved);

    const ECS::EntityHandle fence = FindByTag(world, "fence");
    ASSERT_TRUE(fence.IsValid());

    // Exactly what the editor does post-load: a component the scene never mentioned.
    world.AddComponentImmediate(fence, Components::WorldTransform{});
    world.ProcessCommands();

    const auto savedPath = MakeTempPath("degraded_component_add_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, savedPath));

    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_NE(saved.find("SplineFence.SpanGrade = Cantilevered"), std::string::npos)
        << "an archetype move is not an edit of this field:\n"
        << saved;
}

// B2, as a standing test. Editing ANY other field of the SAME component must not retire this
// field's override. A per-component signal cannot tell these apart at all — field identity is not
// in it — so this is the case that decides between a component-scoped and a field-scoped test.
TEST(SceneIO, EditingASiblingFieldOfTheSameComponentPreservesTheAuthoredText)
{
    const auto scenePath = MakeTempPath("degraded_sibling_edit.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    ASSERT_EQ(degradation.skips.size(), 1u);
    ASSERT_TRUE(degradation.skips[0].preserved);

    const ECS::EntityHandle fence = FindByTag(world, "fence");
    ASSERT_TRUE(fence.IsValid());

    auto* spline = world.GetComponentForWrite<Components::SplineFence>(fence);
    ASSERT_NE(spline, nullptr);
    spline->Seed = 777; // a different field of the same component

    const auto savedPath = MakeTempPath("degraded_sibling_edit_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, savedPath));

    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_NE(saved.find("SplineFence.SpanGrade = Cantilevered"), std::string::npos)
        << "editing Seed says nothing about SpanGrade:\n"
        << saved;
    EXPECT_NE(saved.find("SplineFence.Seed = 777"), std::string::npos)
        << "and the sibling edit itself must reach the file:\n"
        << saved;
}

// A preserved field describes one component instance. Remove the component and the instance — and
// its override — is gone; a later re-add is a NEW component holding defaults, and the previous
// load's authored text must not reappear on it.
TEST(SceneIO, RemovingAndReAddingTheComponentDropsThePreservedText)
{
    const auto scenePath = MakeTempPath("degraded_remove_readd.scene");
    ASSERT_TRUE(WriteFile(scenePath, kSceneWithUnknownEnumValue));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));

    // An override must exist for the remove to have something to take with it.
    ASSERT_EQ(degradation.skips.size(), 1u);
    ASSERT_TRUE(degradation.skips[0].preserved);

    const ECS::EntityHandle fence = FindByTag(world, "fence");
    ASSERT_TRUE(fence.IsValid());
    ASSERT_NE(world.GetComponent<Components::SplineFence>(fence), nullptr);

    world.RemoveComponentImmediate<Components::SplineFence>(fence);
    ASSERT_EQ(world.GetComponent<Components::SplineFence>(fence), nullptr);
    world.AddComponentImmediate(fence, Components::SplineFence{});

    const auto savedPath = MakeTempPath("degraded_remove_readd_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, savedPath));

    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_EQ(saved.find("Cantilevered"), std::string::npos)
        << "the override belonged to a component instance that no longer exists:\n"
        << saved;
    EXPECT_NE(saved.find("SplineFence.SpanGrade = Racked"), std::string::npos)
        << "the re-added component's own default is what must be written:\n"
        << saved;
}

// A load can degrade AND then fail after World::Clear. The world it leaves behind holds the
// half-applied incoming scene — including the field overrides applied before the abort — and a save
// of that world re-emits their authored text. Returning false without the census would hand the
// caller preserved content it was never told about, so the degraded-save guard never arms for a
// scene that is carrying exactly the data the guard exists to protect.
TEST(SceneIO, DegradedLoadThatThenFailsAfterClearStillReportsItsCensus)
{
    const auto scenePath = MakeTempPath("degrade_then_fail.scene");
    ASSERT_TRUE(WriteFile(scenePath,
                          "[scene name=\"DegradeThenFail\" version=1]\n"
                          "[resource id=\"bp\" path=\"no_such_blueprint.blueprint\"]\n"
                          "\n"
                          "[entity id=\"fence\"]\n"
                          "SplineFence.SpanGrade = Cantilevered\n"
                          "SplineFence.Seed = 4242\n"
                          "\n"
                          "[blueprint id=\"bad_one\" source=\"bp\"]\n"));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;

    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, opts));
    ASSERT_TRUE(Scene::GetLastSceneIOError().worldCleared)
        << "this test is only meaningful if the failure lands after the clear";

    ASSERT_EQ(degradation.skips.size(), 1u)
        << "the census gathered before the abort must still reach the caller";
    EXPECT_EQ(degradation.skips[0].component, "SplineFence");
    EXPECT_EQ(degradation.skips[0].field, "SpanGrade");
    EXPECT_TRUE(degradation.skips[0].preserved);

    // What the census is warning about, asserted rather than assumed: the world really is carrying
    // the authored text, and a save really does write it back.
    const auto savedPath = MakeTempPath("degrade_then_fail_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, savedPath));
    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_NE(saved.find("SplineFence.SpanGrade = Cantilevered"), std::string::npos)
        << "the adopted world carries the preserved text the census names:\n"
        << saved;
}

// World::Clear drops field overrides along with preserved components. Entity indices are recycled, so
// an override that outlived its scene would reattach to an unrelated entity in the next one.
TEST(SceneIO, ReopeningAnotherSceneDropsTheFirstScenesFieldOverrides)
{
    const auto degradedPath = MakeTempPath("degraded_first.scene");
    ASSERT_TRUE(WriteFile(degradedPath, kSceneWithUnknownEnumValue));
    const auto cleanPath = MakeTempPath("degraded_second.scene");
    ASSERT_TRUE(WriteFile(cleanPath,
                          "[scene name=\"Second\" version=1]\n"
                          "\n"
                          "[entity id=\"fence\"]\n"
                          "SplineFence.Seed = 7\n"));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, degradedPath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    Scene::SceneLoadDegradation second;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &second;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, cleanPath, opts));
    EXPECT_FALSE(second.IsDegraded()) << "the second scene is clean";

    const auto savedPath = MakeTempPath("degraded_second_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, savedPath));
    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_EQ(saved.find("Cantilevered"), std::string::npos)
        << "the previous scene's override must not survive into this one:\n"
        << saved;
    EXPECT_NE(saved.find("SplineFence.Seed = 7"), std::string::npos)
        << "the second scene's own data must be what got saved:\n"
        << saved;

    // Positive control. The assertion above also passes when preservation is dead everywhere, so
    // prove the mechanism is alive in this binary: reopening a degraded scene still round-trips.
    Scene::SceneLoadDegradation third;
    Scene::LoadOptions reopen{Scene::LoadMode::Replace};
    reopen.outDegradation = &third;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, degradedPath, reopen));
    ASSERT_EQ(third.skips.size(), 1u);

    const auto reopenedPath = MakeTempPath("degraded_third_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(world, reopenedPath));
    std::string reopened;
    ASSERT_TRUE(ReadFile(reopenedPath, reopened));
    EXPECT_NE(reopened.find("SplineFence.SpanGrade = Cantilevered"), std::string::npos)
        << "preservation itself must still work — otherwise the absence asserted above proves "
           "nothing:\n"
        << reopened;
}

// A pre-cutover scene whose PostProcessVolume authored exposure compensation / adaptation clamps
// revives that intent as an ExposureAdjustmentEffect; keys left at the legacy defaults stay
// dropped so untouched volumes gain no component field. Built by saving a real scene and
// injecting the legacy keys the old serializer wrote, so it exercises the on-disk format.
TEST(SceneIO, LoadLegacyVolumeExposure_RevivesAsExposureAdjustment)
{
    const auto scenePath = MakeTempPath("volume_legacy_exposure.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'p', 'p', 'v', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    // Inject the legacy exposure keys right after the volume's first property line. Compensation
    // and max EV are authored; min EV sits at the legacy default (4) and must stay dropped.
    {
        std::ifstream in(scenePath, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::istringstream iss(body);
        std::ostringstream oss;
        bool injected = false;
        for (std::string line; std::getline(iss, line);)
        {
            oss << line << "\n";
            if (!injected && line.find("PostProcessVolume.") != std::string::npos)
            {
                oss << "PostProcessVolume.exposureCompensation = -2\n";
                oss << "PostProcessVolume.autoExposureMinEv = 4\n";
                oss << "PostProcessVolume.autoExposureMaxEv = 12\n";
                oss << "PostProcessVolume.manualExposureEV = 9\n"; // owning-semantics key: dropped
                injected = true;
            }
        }
        ASSERT_TRUE(injected);
        std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
        out << oss.str();
    }

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "ppv");
    ASSERT_TRUE(e2.IsValid());
    ASSERT_NE(w2.GetComponent<Components::PostProcessVolume>(e2), nullptr);
    auto* a2 = w2.GetComponent<Components::ExposureAdjustmentEffect>(e2);
    ASSERT_NE(a2, nullptr);
    EXPECT_FLOAT_EQ(a2->Compensation, -2.0f);
    EXPECT_FALSE(a2->ClampMin); // legacy default min stays dropped
    EXPECT_TRUE(a2->ClampMax);
    EXPECT_FLOAT_EQ(a2->MaxEv, 12.0f);
}

// ---------------------------------------------------------------------------
// The plain-schema collapse gate. A hand-written post-process effect schema may be
// deleted only when the synthesized ReflectionSceneSchema provably (a)
// serializes the SAME property set — key names equal under the format's
// case-insensitive matching, value text byte-identical — and (b) loads the
// hand-written line form to an identical component (no key loss, no clamp or
// enum-semantics drift). CrtEffect and ExposureAdjustmentEffect pass; the
// other effect schemas keep hand-written overrides because the generic
// fallback does not reproduce their semantics: split-vector keys
// (colorR/G/B — the reflection pass skips float[3] fields entirely),
// load-time clamps (Deband/AO/ChromaticAberration/DoF/CAS/FastBlur/
// HeatDistortion/ShadowSettings), enum future-value fallbacks
// (ShadowSettings mode, FilmSimulation grainMode/gateMask), and legacy key
// renames (ChromaticAberration longitudinal/coma).

// Split "Component.key = value" lines into (lowercased key, exact value)
// pairs. Lowercasing mirrors the .scene parser, which lowercases property
// names before schema dispatch (SceneIO.cpp SplitComponentProperty).
static std::vector<std::pair<std::string, std::string>> ParseSchemaLines(
    std::string_view componentName, const std::vector<std::string>& lines)
{
    std::vector<std::pair<std::string, std::string>> out;
    const std::string prefix = std::string(componentName) + ".";
    for (const std::string& line : lines)
    {
        EXPECT_EQ(line.rfind(prefix, 0), 0u) << line;
        const auto eq = line.find(" = ");
        EXPECT_NE(eq, std::string::npos) << line;
        if (line.rfind(prefix, 0) != 0 || eq == std::string::npos)
            continue;
        std::string key = line.substr(prefix.size(), eq - prefix.size());
        for (char& ch : key)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        out.emplace_back(std::move(key), line.substr(eq + 3));
    }
    return out;
}

template <typename TComponent, typename ExpectEqualFn>
static void ExpectReflectionSchemaParity(const char* componentName, const TComponent& authored,
                                         ExpectEqualFn&& expectEqual)
{
    const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName(componentName);
    ASSERT_NE(typeId, 0u);

    ECS::World w1;
    const ECS::EntityHandle e1 = w1.CreateEntity();
    ASSERT_TRUE(e1.IsValid());
    w1.AddComponentImmediate(e1, authored);

    // Pre-collapse this is the hand-written schema (the deletion gate); after
    // the collapse Find synthesizes the reflection schema, and the test keeps
    // guarding against a re-registered hand schema drifting from it.
    const Scene::ISceneComponentSchema* registered = Scene::SceneSchemaRegistry::Find(componentName);
    ASSERT_NE(registered, nullptr);
    const Scene::ReflectionSceneSchema reflection(typeId, componentName);

    Scene::SceneSaveContext saveCtx{};
    std::vector<std::string> registeredLines;
    registered->Serialize(w1, e1, saveCtx, registeredLines);
    std::vector<std::string> reflectionLines;
    reflection.Serialize(w1, e1, saveCtx, reflectionLines);

    // (a) Identical property sets. Line order may differ (hand schemas author
    // their own order, reflection follows the field table); the format is
    // order-insensitive for distinct keys, so compare sorted.
    const auto registeredProps = ParseSchemaLines(componentName, registeredLines);
    auto sortedRegistered = registeredProps;
    auto sortedReflection = ParseSchemaLines(componentName, reflectionLines);
    std::sort(sortedRegistered.begin(), sortedRegistered.end());
    std::sort(sortedReflection.begin(), sortedReflection.end());
    EXPECT_EQ(sortedRegistered, sortedReflection);

    // (b) The registered schema's line form loads through the reflection
    // schema to an identical component: an existing scene keeps its state.
    ECS::World w2;
    const ECS::EntityHandle e2 = w2.CreateEntity();
    ASSERT_TRUE(e2.IsValid());
    std::vector<std::pair<std::string_view, std::string_view>> props;
    props.reserve(registeredProps.size());
    for (const auto& [key, value] : registeredProps)
        props.emplace_back(key, value);
    Scene::SceneLoadContext loadCtx{};
    std::string err;
    ASSERT_TRUE(reflection.ApplyProperties(w2, e2, loadCtx, props, &err, nullptr)) << err;
    const auto* loaded = w2.GetComponent<TComponent>(e2);
    ASSERT_NE(loaded, nullptr);
    expectEqual(authored, *loaded);
}

TEST(SceneIO, PostProcessSchemaCollapse_CrtEffectReflectionParity)
{
    Components::CrtEffect crt{};
    crt.Enabled = false;
    crt.StackOrder = 5;
    crt.Intensity = 0.75f;
    crt.Curvature = 0.15f;
    crt.Scanlines = 0.3f;
    crt.Vignette = 0.4f;
    crt.Aberration = 0.005f;
    crt.Softness = 0.25f;
    crt.ExposureCompensation = false;
    crt.EmulatedResolutionDiv = 4.0f;
    ExpectReflectionSchemaParity("CrtEffect", crt,
                                 [](const Components::CrtEffect& a, const Components::CrtEffect& b)
                                 {
                                     EXPECT_EQ(a.Enabled, b.Enabled);
                                     EXPECT_EQ(a.StackOrder, b.StackOrder);
                                     EXPECT_FLOAT_EQ(a.Intensity, b.Intensity);
                                     EXPECT_FLOAT_EQ(a.Curvature, b.Curvature);
                                     EXPECT_FLOAT_EQ(a.Scanlines, b.Scanlines);
                                     EXPECT_FLOAT_EQ(a.Vignette, b.Vignette);
                                     EXPECT_FLOAT_EQ(a.Aberration, b.Aberration);
                                     EXPECT_FLOAT_EQ(a.Softness, b.Softness);
                                     EXPECT_EQ(a.ExposureCompensation, b.ExposureCompensation);
                                     EXPECT_FLOAT_EQ(a.EmulatedResolutionDiv, b.EmulatedResolutionDiv);
                                 });
}

TEST(SceneIO, PostProcessSchemaCollapse_ExposureAdjustmentEffectReflectionParity)
{
    Components::ExposureAdjustmentEffect adjust{};
    adjust.Enabled = false;
    adjust.Compensation = -1.25f;
    adjust.ClampMin = true;
    adjust.MinEv = 6.5f;
    adjust.ClampMax = true;
    adjust.MaxEv = 13.0f;
    ExpectReflectionSchemaParity(
        "ExposureAdjustmentEffect", adjust,
        [](const Components::ExposureAdjustmentEffect& a, const Components::ExposureAdjustmentEffect& b)
        {
            EXPECT_EQ(a.Enabled, b.Enabled);
            EXPECT_FLOAT_EQ(a.Compensation, b.Compensation);
            EXPECT_EQ(a.ClampMin, b.ClampMin);
            EXPECT_FLOAT_EQ(a.MinEv, b.MinEv);
            EXPECT_EQ(a.ClampMax, b.ClampMax);
            EXPECT_FLOAT_EQ(a.MaxEv, b.MaxEv);
        });
}

// --- PP-ARCH Phase 1: EffectDescriptor-driven collapse of the schemas Phase 0
// could not touch. Each parity test below locks a hand schema's on-disk format
// (split keys, clamps, enum-as-int, renames) against the metadata-aware
// reflection path BEFORE the hand schema is deleted, and keeps guarding the
// synthesized schema afterwards.

TEST(SceneIO, PostProcessSchemaCollapse_AmbientOcclusionEffectReflectionParity)
{
    Components::AmbientOcclusionEffect ao{};
    ao.Enabled = false;
    ao.Intensity = 1.5f;
    ao.Radius = 2.25f;
    ao.Thickness = 0.75f;
    ExpectReflectionSchemaParity(
        "AmbientOcclusionEffect", ao,
        [](const Components::AmbientOcclusionEffect& a, const Components::AmbientOcclusionEffect& b)
        {
            EXPECT_EQ(a.Enabled, b.Enabled);
            EXPECT_FLOAT_EQ(a.Intensity, b.Intensity);
            EXPECT_FLOAT_EQ(a.Radius, b.Radius);
            EXPECT_FLOAT_EQ(a.Thickness, b.Thickness);
        });
}

TEST(SceneIO, PostProcessSchemaCollapse_DebandEffectReflectionParity)
{
    Components::DebandEffect deband{};
    deband.Enabled = false;
    deband.ThresholdLsb = 4.5f;
    ExpectReflectionSchemaParity("DebandEffect", deband,
                                 [](const Components::DebandEffect& a, const Components::DebandEffect& b)
                                 {
                                     EXPECT_EQ(a.Enabled, b.Enabled);
                                     EXPECT_FLOAT_EQ(a.ThresholdLsb, b.ThresholdLsb);
                                 });
}

TEST(SceneIO, PostProcessSchemaCollapse_ContrastAdaptiveSharpenEffectReflectionParity)
{
    Components::ContrastAdaptiveSharpenEffect cas{};
    cas.Enabled = false;
    cas.Strength = 0.65f;
    cas.StackOrder = 3;
    ExpectReflectionSchemaParity(
        "ContrastAdaptiveSharpenEffect", cas,
        [](const Components::ContrastAdaptiveSharpenEffect& a, const Components::ContrastAdaptiveSharpenEffect& b)
        {
            EXPECT_EQ(a.Enabled, b.Enabled);
            EXPECT_FLOAT_EQ(a.Strength, b.Strength);
            EXPECT_EQ(a.StackOrder, b.StackOrder);
        });
}

TEST(SceneIO, PostProcessSchemaCollapse_FastBlurEffectReflectionParity)
{
    Components::FastBlurEffect blur{};
    blur.Enabled = false;
    blur.Intensity = 0.35f;
    blur.FocusDistance = 18.5f;
    blur.FocusRange = 4.25f;
    blur.MaxRadius = 12.5f;
    blur.NearBlur = false;
    ExpectReflectionSchemaParity("FastBlurEffect", blur,
                                 [](const Components::FastBlurEffect& a, const Components::FastBlurEffect& b)
                                 {
                                     EXPECT_EQ(a.Enabled, b.Enabled);
                                     EXPECT_FLOAT_EQ(a.Intensity, b.Intensity);
                                     EXPECT_FLOAT_EQ(a.FocusDistance, b.FocusDistance);
                                     EXPECT_FLOAT_EQ(a.FocusRange, b.FocusRange);
                                     EXPECT_FLOAT_EQ(a.MaxRadius, b.MaxRadius);
                                     EXPECT_EQ(a.NearBlur, b.NearBlur);
                                 });
}

TEST(SceneIO, PostProcessSchemaCollapse_HeatDistortionEffectReflectionParity)
{
    Components::HeatDistortionEffect heat{};
    heat.Enabled = false;
    heat.Strength = 5.5f;
    heat.Speed = -0.75f; // unclamped field: negative must survive
    heat.Scale = 8.25f;
    heat.MaskStrength = 1.5f;
    heat.DistanceStart = 0.125f;
    heat.DistanceEnd = 0.5f;
    heat.DirectionalFalloff = 2.5f;
    heat.UseAbsoluteY = false;
    heat.Softness = 1.75f;
    ExpectReflectionSchemaParity(
        "HeatDistortionEffect", heat,
        [](const Components::HeatDistortionEffect& a, const Components::HeatDistortionEffect& b)
        {
            EXPECT_EQ(a.Enabled, b.Enabled);
            EXPECT_FLOAT_EQ(a.Strength, b.Strength);
            EXPECT_FLOAT_EQ(a.Speed, b.Speed);
            EXPECT_FLOAT_EQ(a.Scale, b.Scale);
            EXPECT_FLOAT_EQ(a.MaskStrength, b.MaskStrength);
            EXPECT_FLOAT_EQ(a.DistanceStart, b.DistanceStart);
            EXPECT_FLOAT_EQ(a.DistanceEnd, b.DistanceEnd);
            EXPECT_FLOAT_EQ(a.DirectionalFalloff, b.DirectionalFalloff);
            EXPECT_EQ(a.UseAbsoluteY, b.UseAbsoluteY);
            EXPECT_FLOAT_EQ(a.Softness, b.Softness);
        });
}

// First split-vector migration: Color[3] must serialize as colorR/colorG/colorB
// scalar lines (the reflection pass previously skipped float[3] fields
// entirely) and load each element back into place.
TEST(SceneIO, PostProcessSchemaCollapse_VignetteEffectReflectionParity)
{
    Components::VignetteEffect vig{};
    vig.Enabled = false;
    vig.StackOrder = 4;
    vig.Intensity = 0.45f;
    vig.Smoothness = 0.75f;
    vig.Rounded = true;
    vig.Color[0] = 0.125f;
    vig.Color[1] = 0.25f;
    vig.Color[2] = 0.5f;
    ExpectReflectionSchemaParity("VignetteEffect", vig,
                                 [](const Components::VignetteEffect& a, const Components::VignetteEffect& b)
                                 {
                                     EXPECT_EQ(a.Enabled, b.Enabled);
                                     EXPECT_EQ(a.StackOrder, b.StackOrder);
                                     EXPECT_FLOAT_EQ(a.Intensity, b.Intensity);
                                     EXPECT_FLOAT_EQ(a.Smoothness, b.Smoothness);
                                     EXPECT_EQ(a.Rounded, b.Rounded);
                                     EXPECT_FLOAT_EQ(a.Color[0], b.Color[0]);
                                     EXPECT_FLOAT_EQ(a.Color[1], b.Color[1]);
                                     EXPECT_FLOAT_EQ(a.Color[2], b.Color[2]);
                                 });
}

// Enum-as-int contract: the hand schema serialized blendMode as its underlying
// integer; the descriptor's EnumAsInt keeps that on-disk form (the generic enum
// codec would write the enumerator name and shift the format).
TEST(SceneIO, PostProcessSchemaCollapse_ColorFilterEffectReflectionParity)
{
    Components::ColorFilterEffect filter{};
    filter.Enabled = false;
    filter.StackOrder = 2;
    filter.BlendMode = Components::ColorFilterBlendMode::Screen;
    filter.Color[0] = 0.25f;
    filter.Color[1] = 0.5f;
    filter.Color[2] = 0.75f;
    filter.Intensity = 0.8f;
    ExpectReflectionSchemaParity(
        "ColorFilterEffect", filter,
        [](const Components::ColorFilterEffect& a, const Components::ColorFilterEffect& b)
        {
            EXPECT_EQ(a.Enabled, b.Enabled);
            EXPECT_EQ(a.StackOrder, b.StackOrder);
            EXPECT_EQ(a.BlendMode, b.BlendMode);
            EXPECT_FLOAT_EQ(a.Color[0], b.Color[0]);
            EXPECT_FLOAT_EQ(a.Color[1], b.Color[1]);
            EXPECT_FLOAT_EQ(a.Color[2], b.Color[2]);
            EXPECT_FLOAT_EQ(a.Intensity, b.Intensity);
        });
}

TEST(SceneIO, PostProcessSchemaCollapse_ColorGradeEffectReflectionParity)
{
    Components::ColorGradeEffect grade{};
    grade.Enabled = false;
    grade.ShadowsColor[0] = 0.05f;
    grade.ShadowsColor[2] = -0.04f;
    grade.ShadowsLightness = -0.1f;
    grade.MidtonesColor[1] = 0.02f;
    grade.MidtonesLightness = 0.05f;
    grade.HighlightsColor[0] = -0.03f;
    grade.HighlightsColor[1] = 0.01f;
    grade.HighlightsLightness = 0.2f;
    grade.Contrast = 1.15f;
    grade.Saturation = 0.9f;
    grade.HueShift = -35.0f;
    grade.Temperature = 22.0f;
    grade.Tint = -14.0f;
    grade.GradeInLog = false;
    grade.ShadowsStart = 0.05f;
    grade.ShadowsEnd = 0.35f;
    grade.HighlightsStart = 0.55f;
    grade.HighlightsEnd = 0.85f;
    ExpectReflectionSchemaParity(
        "ColorGradeEffect", grade,
        [](const Components::ColorGradeEffect& a, const Components::ColorGradeEffect& b)
        {
            EXPECT_EQ(a.Enabled, b.Enabled);
            for (int i = 0; i < 3; ++i)
            {
                EXPECT_FLOAT_EQ(a.ShadowsColor[i], b.ShadowsColor[i]);
                EXPECT_FLOAT_EQ(a.MidtonesColor[i], b.MidtonesColor[i]);
                EXPECT_FLOAT_EQ(a.HighlightsColor[i], b.HighlightsColor[i]);
            }
            EXPECT_FLOAT_EQ(a.ShadowsLightness, b.ShadowsLightness);
            EXPECT_FLOAT_EQ(a.MidtonesLightness, b.MidtonesLightness);
            EXPECT_FLOAT_EQ(a.HighlightsLightness, b.HighlightsLightness);
            EXPECT_FLOAT_EQ(a.Contrast, b.Contrast);
            EXPECT_FLOAT_EQ(a.Saturation, b.Saturation);
            EXPECT_FLOAT_EQ(a.HueShift, b.HueShift);
            EXPECT_FLOAT_EQ(a.Temperature, b.Temperature);
            EXPECT_FLOAT_EQ(a.Tint, b.Tint);
            EXPECT_EQ(a.GradeInLog, b.GradeInLog);
            EXPECT_FLOAT_EQ(a.ShadowsStart, b.ShadowsStart);
            EXPECT_FLOAT_EQ(a.ShadowsEnd, b.ShadowsEnd);
            EXPECT_FLOAT_EQ(a.HighlightsStart, b.HighlightsStart);
            EXPECT_FLOAT_EQ(a.HighlightsEnd, b.HighlightsEnd);
        });
}

// Legacy key renames: the on-disk keys are "longitudinal"/"coma" while the
// reflected fields are LongitudinalIntensity/ComaIntensity — the descriptor's
// SerializedKey carries the mapping both ways.
TEST(SceneIO, PostProcessSchemaCollapse_ChromaticAberrationEffectReflectionParity)
{
    Components::ChromaticAberrationEffect ca{};
    ca.Enabled = false;
    ca.Intensity = 2.5f;
    ca.StartOffset = 0.5f;
    ca.Saturation = 1.5f;
    ca.LongitudinalIntensity = 3.25f;
    ca.ComaIntensity = 16.5f;
    ExpectReflectionSchemaParity(
        "ChromaticAberrationEffect", ca,
        [](const Components::ChromaticAberrationEffect& a, const Components::ChromaticAberrationEffect& b)
        {
            EXPECT_EQ(a.Enabled, b.Enabled);
            EXPECT_FLOAT_EQ(a.Intensity, b.Intensity);
            EXPECT_FLOAT_EQ(a.StartOffset, b.StartOffset);
            EXPECT_FLOAT_EQ(a.Saturation, b.Saturation);
            EXPECT_FLOAT_EQ(a.LongitudinalIntensity, b.LongitudinalIntensity);
            EXPECT_FLOAT_EQ(a.ComaIntensity, b.ComaIntensity);
        });
}

// DepthOfField serializes only enabled/maxRadius/samplingQuality — the legacy
// DebugMode/DebugAlpha fields are load-only (SkipSerialize) so the property
// set matches the hand schema, while older scenes carrying them still load.
TEST(SceneIO, PostProcessSchemaCollapse_DepthOfFieldEffectReflectionParity)
{
    Components::DepthOfFieldEffect dof{};
    dof.Enabled = false;
    dof.MaxRadius = 24.5f;
    dof.SamplingQuality = Components::DofSamplingQuality::Quality;
    ExpectReflectionSchemaParity(
        "DepthOfFieldEffect", dof,
        [](const Components::DepthOfFieldEffect& a, const Components::DepthOfFieldEffect& b)
        {
            EXPECT_EQ(a.Enabled, b.Enabled);
            EXPECT_FLOAT_EQ(a.MaxRadius, b.MaxRadius);
            EXPECT_EQ(a.SamplingQuality, b.SamplingQuality);
        });
}

// SSSR never had a hand-written schema to compare against, so parity is pinned
// against LITERAL on-disk text instead: the key spelling the reflection path
// derives (lowerCamel), the enum's integer form, and the load clamps that the
// deleted-by-design hand schema would otherwise have enforced.
TEST(SceneIO, ScreenSpaceReflectionsEffectSerializesLiteralKeysAndClampsOnLoad)
{
    using SSSR = Components::ScreenSpaceReflectionsEffect;
    const auto scenePath = MakeTempPath("sssr_keys.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'s', 's', 'r', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    SSSR authored{};
    authored.Enabled = true;
    authored.Intensity = 0.85f;
    authored.MaxDistance = 250.0f;
    authored.Thickness = 0.5f;
    authored.EdgeFade = 0.25f;
    authored.MaxSteps = 96;
    authored.SampleQuality = Components::SssrSampleQuality::High;
    authored.MultiBounce = true;
    w1.AddComponentImmediate(e, authored);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string text;
    {
        std::ifstream in(scenePath, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // Exact keys: a drift here silently orphans every scene already on disk.
    EXPECT_NE(text.find("ScreenSpaceReflectionsEffect.enabled = true"), std::string::npos);
    EXPECT_NE(text.find("ScreenSpaceReflectionsEffect.intensity = 0.85"), std::string::npos);
    EXPECT_NE(text.find("ScreenSpaceReflectionsEffect.maxDistance = 250"), std::string::npos);
    EXPECT_NE(text.find("ScreenSpaceReflectionsEffect.thickness = 0.5"), std::string::npos);
    EXPECT_NE(text.find("ScreenSpaceReflectionsEffect.edgeFade = 0.25"), std::string::npos);
    EXPECT_NE(text.find("ScreenSpaceReflectionsEffect.maxSteps = 96"), std::string::npos);
    EXPECT_NE(text.find("ScreenSpaceReflectionsEffect.multiBounce = true"), std::string::npos);
    // EnumAsInt: the integer, never the "High" enumerator name.
    EXPECT_NE(text.find("ScreenSpaceReflectionsEffect.sampleQuality = 2"), std::string::npos);
    EXPECT_EQ(text.find("ScreenSpaceReflectionsEffect.sampleQuality = High"), std::string::npos);

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto* loaded = w2.GetComponent<SSSR>(FindByTag(w2, "ssr"));
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->Enabled);
    EXPECT_FLOAT_EQ(loaded->Intensity, 0.85f);
    EXPECT_FLOAT_EQ(loaded->MaxDistance, 250.0f);
    EXPECT_FLOAT_EQ(loaded->Thickness, 0.5f);
    EXPECT_FLOAT_EQ(loaded->EdgeFade, 0.25f);
    EXPECT_EQ(loaded->MaxSteps, 96);
    EXPECT_EQ(loaded->SampleQuality, Components::SssrSampleQuality::High);
    EXPECT_TRUE(loaded->MultiBounce);

    // A hand-edited scene past every bound loads clamped, not rejected.
    std::string hostile = text;
    const auto replaceLine = [&hostile](const std::string& from, const std::string& to)
    {
        const auto pos = hostile.find(from);
        ASSERT_NE(pos, std::string::npos) << from;
        hostile.replace(pos, from.size(), to);
    };
    replaceLine("ScreenSpaceReflectionsEffect.intensity = 0.85",
                "ScreenSpaceReflectionsEffect.intensity = 500");
    replaceLine("ScreenSpaceReflectionsEffect.edgeFade = 0.25",
                "ScreenSpaceReflectionsEffect.edgeFade = 0");
    replaceLine("ScreenSpaceReflectionsEffect.maxSteps = 96",
                "ScreenSpaceReflectionsEffect.maxSteps = 100000");
    replaceLine("ScreenSpaceReflectionsEffect.sampleQuality = 2",
                "ScreenSpaceReflectionsEffect.sampleQuality = 77");
    {
        std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
        out << hostile;
    }
    ECS::World w3;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w3, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto* clamped = w3.GetComponent<SSSR>(FindByTag(w3, "ssr"));
    ASSERT_NE(clamped, nullptr);
    EXPECT_FLOAT_EQ(clamped->Intensity, SSSR::kIntensityMax);
    EXPECT_FLOAT_EQ(clamped->EdgeFade, SSSR::kMinEdgeFade);
    EXPECT_EQ(clamped->MaxSteps, SSSR::kMaxSteps);
    EXPECT_EQ(clamped->SampleQuality, Components::SssrSampleQuality::High);
}

// FilmSimulation: HalationTint[3] keeps the hand schema's single-tuple form
// "(r, g, b)" via TupleVec3, and both enums (grainMode/gateMask) keep their
// integer on-disk form with the future-value clamp to the last known mode.
TEST(SceneIO, PostProcessSchemaCollapse_FilmSimulationEffectReflectionParity)
{
    Components::FilmSimulationEffect film{};
    film.Enabled = false;
    film.FilmFrameRate = 18.0f;
    film.HalationEnabled = false;
    film.HalationIntensity = 1.5f;
    film.HalationRadius = 6.5f;
    film.HalationTint[0] = 0.5f;
    film.HalationTint[1] = 0.25f;
    film.HalationTint[2] = 0.125f;
    film.GrainEnabled = false;
    film.GrainMode = Components::FilmGrainMode::FidelityFXFast;
    film.GrainIntensity = 0.5f;
    film.GrainSize = 2.5f;
    film.GrainSmooth = false;
    film.GrainDensity = 0.75f;
    film.GrainShadowResponse = 0.25f;
    film.GrainMidtoneResponse = 0.5f;
    film.GrainHighlightResponse = 0.75f;
    film.GrainColored = false;
    film.HairEnabled = false;
    film.HairAmount = 1.5f;
    film.HairIntensity = 2.5f;
    film.HairWidth = 0.5f;
    film.HairLength = 128.0f;
    film.HairRandomSize = 0.5f;
    film.HairCurl = 0.75f;
    film.HairCurlRandomness = 0.25f;
    film.ScratchesEnabled = false;
    film.ScratchAmount = 3.5f;
    film.ScratchIntensity = 1.25f;
    film.ScratchWidth = 0.25f;
    film.ScratchLength = 0.5f;
    film.DustEnabled = false;
    film.DustAmount = 1.25f;
    film.DustIntensity = 0.5f;
    film.DustSize = 2.5f;
    film.DustRandomSize = 0.75f;
    film.GateWeaveEnabled = false;
    film.GateWeaveHorizontal = 1.5f;
    film.GateWeaveVertical = 0.25f;
    film.GateWeaveRotation = 0.5f;
    film.GateMask = Components::FilmGateMask::Academy137;
    film.GateMaskFeather = 16.5f;
    film.GateMaskRoundness = 0.5f;
    ExpectReflectionSchemaParity(
        "FilmSimulationEffect", film,
        [](const Components::FilmSimulationEffect& a, const Components::FilmSimulationEffect& b)
        {
            EXPECT_EQ(a.Enabled, b.Enabled);
            EXPECT_FLOAT_EQ(a.FilmFrameRate, b.FilmFrameRate);
            EXPECT_EQ(a.HalationEnabled, b.HalationEnabled);
            EXPECT_FLOAT_EQ(a.HalationIntensity, b.HalationIntensity);
            EXPECT_FLOAT_EQ(a.HalationRadius, b.HalationRadius);
            for (int i = 0; i < 3; ++i)
                EXPECT_FLOAT_EQ(a.HalationTint[i], b.HalationTint[i]);
            EXPECT_EQ(a.GrainEnabled, b.GrainEnabled);
            EXPECT_EQ(a.GrainMode, b.GrainMode);
            EXPECT_FLOAT_EQ(a.GrainIntensity, b.GrainIntensity);
            EXPECT_FLOAT_EQ(a.GrainSize, b.GrainSize);
            EXPECT_EQ(a.GrainSmooth, b.GrainSmooth);
            EXPECT_FLOAT_EQ(a.GrainDensity, b.GrainDensity);
            EXPECT_FLOAT_EQ(a.GrainShadowResponse, b.GrainShadowResponse);
            EXPECT_FLOAT_EQ(a.GrainMidtoneResponse, b.GrainMidtoneResponse);
            EXPECT_FLOAT_EQ(a.GrainHighlightResponse, b.GrainHighlightResponse);
            EXPECT_EQ(a.GrainColored, b.GrainColored);
            EXPECT_EQ(a.HairEnabled, b.HairEnabled);
            EXPECT_FLOAT_EQ(a.HairAmount, b.HairAmount);
            EXPECT_FLOAT_EQ(a.HairIntensity, b.HairIntensity);
            EXPECT_FLOAT_EQ(a.HairWidth, b.HairWidth);
            EXPECT_FLOAT_EQ(a.HairLength, b.HairLength);
            EXPECT_FLOAT_EQ(a.HairRandomSize, b.HairRandomSize);
            EXPECT_FLOAT_EQ(a.HairCurl, b.HairCurl);
            EXPECT_FLOAT_EQ(a.HairCurlRandomness, b.HairCurlRandomness);
            EXPECT_EQ(a.ScratchesEnabled, b.ScratchesEnabled);
            EXPECT_FLOAT_EQ(a.ScratchAmount, b.ScratchAmount);
            EXPECT_FLOAT_EQ(a.ScratchIntensity, b.ScratchIntensity);
            EXPECT_FLOAT_EQ(a.ScratchWidth, b.ScratchWidth);
            EXPECT_FLOAT_EQ(a.ScratchLength, b.ScratchLength);
            EXPECT_EQ(a.DustEnabled, b.DustEnabled);
            EXPECT_FLOAT_EQ(a.DustAmount, b.DustAmount);
            EXPECT_FLOAT_EQ(a.DustIntensity, b.DustIntensity);
            EXPECT_FLOAT_EQ(a.DustSize, b.DustSize);
            EXPECT_FLOAT_EQ(a.DustRandomSize, b.DustRandomSize);
            EXPECT_EQ(a.GateWeaveEnabled, b.GateWeaveEnabled);
            EXPECT_FLOAT_EQ(a.GateWeaveHorizontal, b.GateWeaveHorizontal);
            EXPECT_FLOAT_EQ(a.GateWeaveVertical, b.GateWeaveVertical);
            EXPECT_FLOAT_EQ(a.GateWeaveRotation, b.GateWeaveRotation);
            EXPECT_EQ(a.GateMask, b.GateMask);
            EXPECT_FLOAT_EQ(a.GateMaskFeather, b.GateMaskFeather);
            EXPECT_FLOAT_EQ(a.GateMaskRoundness, b.GateMaskRoundness);
        });
}

// ShadowSettings: mode keeps its integer on-disk form (the shipped
// Load_ShadowSettingsEffectModeDefaultsAndFutureFallback test greps the
// literal "mode = 1" line), and the bias clamps ride the descriptor.
TEST(SceneIO, PostProcessSchemaCollapse_ShadowSettingsEffectReflectionParity)
{
    Components::ShadowSettingsEffect shadow{};
    shadow.Enabled = false;
    shadow.Mode = Components::DirectionalShadowMode::RayTraced;
    shadow.MaxShadowDistance = 150.5f;
    shadow.SplitLambda = 0.5f;
    shadow.DepthBias = 0.005f;
    shadow.NormalBias = 1.25f;
    ExpectReflectionSchemaParity(
        "ShadowSettingsEffect", shadow,
        [](const Components::ShadowSettingsEffect& a, const Components::ShadowSettingsEffect& b)
        {
            EXPECT_EQ(a.Enabled, b.Enabled);
            EXPECT_EQ(a.Mode, b.Mode);
            EXPECT_FLOAT_EQ(a.MaxShadowDistance, b.MaxShadowDistance);
            EXPECT_FLOAT_EQ(a.SplitLambda, b.SplitLambda);
            EXPECT_FLOAT_EQ(a.DepthBias, b.DepthBias);
            EXPECT_FLOAT_EQ(a.NormalBias, b.NormalBias);
        });
}

// ColorGrade's retired pre-unification keys (brightness/gamma/hue) load as
// silent no-ops through the descriptor's dropped-keys list — no error, no
// state change — matching the hand schema's consume-and-drop.
TEST(SceneIO, PostProcessSchemaCollapse_ColorGradeDropsRetiredKeysSilently)
{
    const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("ColorGradeEffect");
    ASSERT_NE(typeId, 0u);
    const Scene::ReflectionSceneSchema reflection(typeId, "ColorGradeEffect");
    ECS::World w;
    const ECS::EntityHandle e = w.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    const std::vector<std::pair<std::string_view, std::string_view>> props{
        {"brightness", "0.5"}, {"gamma", "1.2"}, {"hue", "0.1"}, {"contrast", "1.25"}};
    Scene::SceneLoadContext ctx{};
    std::string err;
    ASSERT_TRUE(reflection.ApplyProperties(w, e, ctx, props, &err, nullptr)) << err;
    const auto* grade = w.GetComponent<Components::ColorGradeEffect>(e);
    ASSERT_NE(grade, nullptr);
    EXPECT_FLOAT_EQ(grade->Contrast, 1.25f); // the live key still applies
}

// The reflection path applies the descriptor-registered load clamps (the hand
// schemas' out-of-range protection) — here min-only clamps on AmbientOcclusion
// and the two-sided CAS strength clamp, exercised straight through the
// synthesized schema the collapse ships.
TEST(SceneIO, PostProcessSchemaCollapse_ReflectionAppliesRegisteredLoadClamps)
{
    {
        const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("AmbientOcclusionEffect");
        ASSERT_NE(typeId, 0u);
        const Scene::ReflectionSceneSchema reflection(typeId, "AmbientOcclusionEffect");
        ECS::World w;
        const ECS::EntityHandle e = w.CreateEntity();
        ASSERT_TRUE(e.IsValid());
        const std::vector<std::pair<std::string_view, std::string_view>> props{
            {"intensity", "-3"}, {"radius", "-0.5"}, {"thickness", "2.5"}};
        Scene::SceneLoadContext ctx{};
        std::string err;
        ASSERT_TRUE(reflection.ApplyProperties(w, e, ctx, props, &err, nullptr)) << err;
        const auto* ao = w.GetComponent<Components::AmbientOcclusionEffect>(e);
        ASSERT_NE(ao, nullptr);
        EXPECT_FLOAT_EQ(ao->Intensity, 0.0f);
        EXPECT_FLOAT_EQ(ao->Radius, 0.0f);
        EXPECT_FLOAT_EQ(ao->Thickness, 2.5f); // in-range value passes through
    }
    {
        const ECS::ComponentTypeId typeId =
            ECS::ComponentFieldRegistry::FindByName("ContrastAdaptiveSharpenEffect");
        ASSERT_NE(typeId, 0u);
        const Scene::ReflectionSceneSchema reflection(typeId, "ContrastAdaptiveSharpenEffect");
        ECS::World w;
        const ECS::EntityHandle e = w.CreateEntity();
        ASSERT_TRUE(e.IsValid());
        const std::vector<std::pair<std::string_view, std::string_view>> props{{"strength", "7.5"}};
        Scene::SceneLoadContext ctx{};
        std::string err;
        ASSERT_TRUE(reflection.ApplyProperties(w, e, ctx, props, &err, nullptr)) << err;
        const auto* cas = w.GetComponent<Components::ContrastAdaptiveSharpenEffect>(e);
        ASSERT_NE(cas, nullptr);
        EXPECT_FLOAT_EQ(cas->Strength, 1.0f);
    }
    {
        // Enum-as-int range: an out-of-table blendMode clamps into the table
        // (the hand schema hard-failed the load; the descriptor path prefers
        // load resilience — the value lands on the nearest valid mode).
        const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("ColorFilterEffect");
        ASSERT_NE(typeId, 0u);
        const Scene::ReflectionSceneSchema reflection(typeId, "ColorFilterEffect");
        ECS::World w;
        const ECS::EntityHandle e = w.CreateEntity();
        ASSERT_TRUE(e.IsValid());
        const std::vector<std::pair<std::string_view, std::string_view>> props{{"blendmode", "99"}};
        Scene::SceneLoadContext ctx{};
        std::string err;
        ASSERT_TRUE(reflection.ApplyProperties(w, e, ctx, props, &err, nullptr)) << err;
        const auto* filter = w.GetComponent<Components::ColorFilterEffect>(e);
        ASSERT_NE(filter, nullptr);
        EXPECT_EQ(filter->BlendMode, Components::ColorFilterBlendMode::SoftLight);
    }
    {
        // DepthOfField legacy load-only keys: accepted (with clamps) from older
        // scenes even though the reflection path never serializes them.
        const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("DepthOfFieldEffect");
        ASSERT_NE(typeId, 0u);
        const Scene::ReflectionSceneSchema reflection(typeId, "DepthOfFieldEffect");
        ECS::World w;
        const ECS::EntityHandle e = w.CreateEntity();
        ASSERT_TRUE(e.IsValid());
        const std::vector<std::pair<std::string_view, std::string_view>> props{
            {"debugmode", "5"}, {"debugalpha", "1.5"}, {"maxradius", "4096"}};
        Scene::SceneLoadContext ctx{};
        std::string err;
        ASSERT_TRUE(reflection.ApplyProperties(w, e, ctx, props, &err, nullptr)) << err;
        const auto* dof = w.GetComponent<Components::DepthOfFieldEffect>(e);
        ASSERT_NE(dof, nullptr);
        EXPECT_EQ(dof->DebugMode, 1);
        EXPECT_FLOAT_EQ(dof->DebugAlpha, 1.0f);
        EXPECT_FLOAT_EQ(dof->MaxRadius, Components::DepthOfFieldEffect::kMaxRadiusMax);
    }
    {
        // ColorGrade white-balance/hue clamps: hand-edited out-of-range values
        // land on the descriptor bounds (URP ranges) instead of reaching the
        // extraction unclamped.
        const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("ColorGradeEffect");
        ASSERT_NE(typeId, 0u);
        const Scene::ReflectionSceneSchema reflection(typeId, "ColorGradeEffect");
        ECS::World w;
        const ECS::EntityHandle e = w.CreateEntity();
        ASSERT_TRUE(e.IsValid());
        const std::vector<std::pair<std::string_view, std::string_view>> props{
            {"temperature", "500"}, {"tint", "-500"}, {"hueshift", "900"}};
        Scene::SceneLoadContext ctx{};
        std::string err;
        ASSERT_TRUE(reflection.ApplyProperties(w, e, ctx, props, &err, nullptr)) << err;
        const auto* grade = w.GetComponent<Components::ColorGradeEffect>(e);
        ASSERT_NE(grade, nullptr);
        EXPECT_FLOAT_EQ(grade->Temperature, 100.0f);
        EXPECT_FLOAT_EQ(grade->Tint, -100.0f);
        EXPECT_FLOAT_EQ(grade->HueShift, 180.0f);
    }
}

// ColorGradeEffect round-trips every authored field. The .scene parser
// lowercases property keys before schema dispatch, so the schema must compare
// lowercased names — the original mixed-case comparisons (shadowsColorR, ...)
// never matched, and any scene carrying a saved or converted grade failed to
// load. Found by the Phase 0 schema audit.
TEST(SceneIO, SaveThenLoad_RoundTripColorGradeEffect)
{
    const auto scenePath = MakeTempPath("color_grade.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'c', 'g', 'r', '\0'}});
    w1.AddComponentImmediate(e, Components::PostProcessVolume{});
    Components::ColorGradeEffect grade{};
    grade.ShadowsColor[0] = 0.05f;
    grade.ShadowsColor[2] = -0.04f;
    grade.ShadowsLightness = -0.1f;
    grade.MidtonesColor[1] = 0.02f;
    grade.MidtonesLightness = 0.05f;
    grade.HighlightsColor[0] = -0.03f;
    grade.HighlightsColor[1] = 0.01f;
    grade.HighlightsLightness = 0.2f;
    grade.Contrast = 1.15f;
    grade.Saturation = 0.9f;
    grade.HueShift = 40.0f;
    grade.Temperature = -30.0f;
    grade.Tint = 12.5f;
    grade.GradeInLog = false;
    grade.ShadowsStart = 0.05f;
    grade.ShadowsEnd = 0.35f;
    grade.HighlightsStart = 0.55f;
    grade.HighlightsEnd = 0.85f;
    w1.AddComponentImmediate(e, grade);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto e2 = FindByTag(w2, "cgr");
    ASSERT_TRUE(e2.IsValid());
    const auto* g2 = w2.GetComponent<Components::ColorGradeEffect>(e2);
    ASSERT_NE(g2, nullptr);
    EXPECT_TRUE(g2->Enabled);
    EXPECT_FLOAT_EQ(g2->ShadowsColor[0], 0.05f);
    EXPECT_FLOAT_EQ(g2->ShadowsColor[1], 0.0f);
    EXPECT_FLOAT_EQ(g2->ShadowsColor[2], -0.04f);
    EXPECT_FLOAT_EQ(g2->ShadowsLightness, -0.1f);
    EXPECT_FLOAT_EQ(g2->MidtonesColor[1], 0.02f);
    EXPECT_FLOAT_EQ(g2->MidtonesLightness, 0.05f);
    EXPECT_FLOAT_EQ(g2->HighlightsColor[0], -0.03f);
    EXPECT_FLOAT_EQ(g2->HighlightsColor[1], 0.01f);
    EXPECT_FLOAT_EQ(g2->HighlightsLightness, 0.2f);
    EXPECT_FLOAT_EQ(g2->Contrast, 1.15f);
    EXPECT_FLOAT_EQ(g2->Saturation, 0.9f);
    EXPECT_FLOAT_EQ(g2->HueShift, 40.0f);
    EXPECT_FLOAT_EQ(g2->Temperature, -30.0f);
    EXPECT_FLOAT_EQ(g2->Tint, 12.5f);
    EXPECT_FALSE(g2->GradeInLog);
    EXPECT_FLOAT_EQ(g2->ShadowsStart, 0.05f);
    EXPECT_FLOAT_EQ(g2->ShadowsEnd, 0.35f);
    EXPECT_FLOAT_EQ(g2->HighlightsStart, 0.55f);
    EXPECT_FLOAT_EQ(g2->HighlightsEnd, 0.85f);
}

// Whole-file drift-guard for the collapse: a scene carrying collapsed
// (reflection-serialized) and kept (hand-serialized) effects together resaves
// byte-identically after a load — the serialize→load→serialize fixed point
// the §6 gate demands. Holds under both regimes, so it also proves the
// deletion commit changed no serialized state.
TEST(SceneIO, PostProcessSchemaCollapse_SceneResaveIsByteIdentical)
{
    // Both saves write a fresh file (same stem, so identical headers): saving
    // over an existing file runs the preserve-unknown-sections scan, whose
    // cosmetic whitespace would leak into the byte compare.
    const auto scenePath = MakeTempPath("pp_collapse_fixed_point.scene");
    const auto resaveDir = scenePath.parent_path() / "pp_resave";
    std::error_code ec;
    std::filesystem::create_directories(resaveDir, ec);
    const auto resavePath = resaveDir / scenePath.filename();
    std::filesystem::remove(scenePath, ec);
    std::filesystem::remove(resavePath, ec);

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'p', 'p', 'x', '\0'}});
    // Author a Name so the loader's fill-Name-from-entity-id fallback does not
    // add state on load (the fixed point would otherwise gain a Name line).
    Components::Name name{};
    std::memcpy(name.value, "ppx", 4);
    w1.AddComponentImmediate(e, name);
    Components::PostProcessVolume vol{};
    vol.Weight = 0.5f;
    vol.Priority = 2;
    vol.BlendDistance = 3.0f;
    w1.AddComponentImmediate(e, vol);
    Components::CrtEffect crt{};
    crt.Intensity = 0.8f;
    crt.Scanlines = 0.22f;
    w1.AddComponentImmediate(e, crt);
    Components::ExposureAdjustmentEffect adjust{};
    adjust.Compensation = 1.5f;
    adjust.ClampMin = true;
    adjust.MinEv = 5.0f;
    w1.AddComponentImmediate(e, adjust);
    Components::BloomEffect bloom{};
    bloom.Threshold = 1.3f;
    bloom.Radius = 3.5f;
    w1.AddComponentImmediate(e, bloom);
    Components::VignetteEffect vig{};
    vig.Intensity = 0.35f;
    vig.Color[0] = 0.1f;
    vig.Color[2] = 0.2f;
    w1.AddComponentImmediate(e, vig);
    Components::DebandEffect deband{};
    deband.ThresholdLsb = 4.0f;
    w1.AddComponentImmediate(e, deband);
    Components::AmbientOcclusionEffect ao{};
    ao.Intensity = 1.5f;
    ao.Radius = 2.25f;
    w1.AddComponentImmediate(e, ao);
    Components::ContrastAdaptiveSharpenEffect cas{};
    cas.Strength = 0.65f;
    cas.StackOrder = 2;
    w1.AddComponentImmediate(e, cas);
    Components::FastBlurEffect blur{};
    blur.Intensity = 0.35f;
    blur.FocusDistance = 18.5f;
    w1.AddComponentImmediate(e, blur);
    Components::HeatDistortionEffect heat{};
    heat.Strength = 5.5f;
    heat.Softness = 1.75f;
    w1.AddComponentImmediate(e, heat);
    Components::ColorFilterEffect filter{};
    filter.BlendMode = Components::ColorFilterBlendMode::Add;
    filter.Color[0] = 0.25f;
    filter.Intensity = 0.8f;
    w1.AddComponentImmediate(e, filter);
    Components::ColorGradeEffect grade{};
    grade.ShadowsColor[1] = 0.05f;
    grade.Contrast = 1.15f;
    w1.AddComponentImmediate(e, grade);
    Components::ChromaticAberrationEffect ca{};
    ca.Intensity = 2.5f;
    ca.LongitudinalIntensity = 3.25f;
    w1.AddComponentImmediate(e, ca);
    Components::DepthOfFieldEffect dof{};
    dof.MaxRadius = 24.5f;
    dof.SamplingQuality = Components::DofSamplingQuality::Quality;
    w1.AddComponentImmediate(e, dof);
    Components::FilmSimulationEffect film{};
    film.FilmFrameRate = 18.0f;
    film.HalationTint[0] = 0.5f;
    film.GateMask = Components::FilmGateMask::Academy137;
    w1.AddComponentImmediate(e, film);
    Components::ShadowSettingsEffect shadow{};
    shadow.Mode = Components::DirectionalShadowMode::RayTraced;
    shadow.MaxShadowDistance = 150.5f;
    w1.AddComponentImmediate(e, shadow);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string firstSave;
    {
        std::ifstream in(scenePath, std::ios::binary);
        firstSave.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto e2 = FindByTag(w2, "ppx");
    ASSERT_TRUE(e2.IsValid());
    const auto* crt2 = w2.GetComponent<Components::CrtEffect>(e2);
    ASSERT_NE(crt2, nullptr);
    EXPECT_FLOAT_EQ(crt2->Intensity, 0.8f);
    EXPECT_FLOAT_EQ(crt2->Scanlines, 0.22f);
    const auto* adj2 = w2.GetComponent<Components::ExposureAdjustmentEffect>(e2);
    ASSERT_NE(adj2, nullptr);
    EXPECT_FLOAT_EQ(adj2->Compensation, 1.5f);
    EXPECT_TRUE(adj2->ClampMin);
    EXPECT_FLOAT_EQ(adj2->MinEv, 5.0f);
    const auto* vig2 = w2.GetComponent<Components::VignetteEffect>(e2);
    ASSERT_NE(vig2, nullptr);
    EXPECT_FLOAT_EQ(vig2->Color[0], 0.1f);
    EXPECT_FLOAT_EQ(vig2->Color[2], 0.2f);

    ASSERT_TRUE(Scene::SaveSceneToFile(w2, resavePath, Scene::SaveOptions{}));
    std::string secondSave;
    {
        std::ifstream in(resavePath, std::ios::binary);
        secondSave.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(firstSave, secondSave);
}

// A scene that predates physical-unit serialization (no intensityUnit/colorTemperature lines) must
// load to the struct defaults: Unitless (anchor-invariant) and colour temperature off. Built by
// saving a real scene then stripping the new lines, so it exercises the actual on-disk format.
TEST(SceneIO, LoadLegacyLightWithoutUnitFields_DefaultsToUnitless)
{
    const auto scenePath = MakeTempPath("light_legacy.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'l', 'e', 'g', 'a', 'c', 'y', '\0'}});
    Components::Light l{};
    l.Intensity = 2.0f; // left Unitless / temperature off (the pre-physical-units state)
    w1.AddComponentImmediate(e, l);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    // Strip the new physical-authoring lines to emulate a scene saved before they existed.
    {
        std::ifstream in(scenePath, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::istringstream iss(body);
        std::ostringstream oss;
        for (std::string line; std::getline(iss, line);)
        {
            if (line.find("intensityUnit") != std::string::npos ||
                line.find("useColorTemperature") != std::string::npos ||
                line.find("colorTemperature") != std::string::npos)
                continue;
            oss << line << "\n";
        }
        std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
        out << oss.str();
    }

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "legacy");
    ASSERT_TRUE(e2.IsValid());
    auto* l2 = w2.GetComponent<Components::Light>(e2);
    ASSERT_NE(l2, nullptr);
    EXPECT_EQ(l2->IntensityUnit, Components::LightUnit::Unitless); // anchor-invariant -> no brightness change
    EXPECT_FALSE(l2->UseColorTemperature);
    EXPECT_FLOAT_EQ(l2->Intensity, 2.0f);
}

TEST(SceneIO, SaveThenLoad_RoundTripAllTerrainGrassSettings)
{
    const auto scenePath = MakeTempPath("terrain_grass_roundtrip.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();
    Scene::EnsureTerrainGrassSceneSchemasRegistered();

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'t', 'e', 'r', 'r', 'a', 'i', 'n', '\0'}});

    Components::Terrain terrain{};
    terrain.MaterialTiling = 7.5f;
    const GUID terrainAssetGuid(GUID::Data{{
        0x10, 0x32, 0x54, 0x76,
        0x98, 0xba,
        0xdc, 0xfe,
        0x01, 0x23,
        0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
    }});
    terrain.TerrainAssetGuid.Set(terrainAssetGuid);
    terrain.TerrainAssetType = 7;
    w1.AddComponentImmediate(e, terrain);

    Components::TerrainGrass grass{};
    // The non-default value, so a dropped key would load as Dither and fail below.
    grass.RenderMode = Components::TerrainGrassRenderMode::Blend;
    grass.BladesPerSquareMeter = 1.75f;
    grass.BladeHeight = 3.25f;
    grass.BladeWidth = 0.075f;
    grass.MaxWidthRatio = 0.125f;
    grass.ClumpSize = 2.25f;
    grass.ClumpHeightVariance = 0.55f;
    grass.ClumpAlignment = 0.8f;
    grass.ClumpGather = 0.65f;
    grass.HueVariation = 0.7f;
    grass.RootShade = 0.55f;
    grass.RootFadeStart = 0.12f;
    grass.RootFadeEnd = 0.83f;
    grass.BladeSegments = 9u;
    grass.RandomScale = 0.42f;
    grass.LayerIndex = 2;
    grass.MaskThreshold = 0.62f;
    grass.Range = 620.0f;
    grass.DensityFalloff = 3.5f;
    grass.PlacementSeed = 11.25f;
    grass.WindDirection = -1.25f;
    grass.WindGustSpeed = 1.4f;
    grass.WindGustScale = 0.08f;
    grass.WindStrength = 3.5f;
    grass.WindRestingLean = 0.2f;
    grass.WindFlutterAmount = 0.31f;
    grass.WindFlutterSpeed = 4.6f;
    grass.WindSeed = 9.5f;
    grass.Brightness = 1.35f;
    grass.RandomBrightness = 0.22f;
    grass.GroundingStrength = 0.25f;
    grass.BladeNormalForm = 0.42f;
    grass.BladeScatterGain = 0.37f;
    grass.Translucency = 0.65f;
    grass.TextureGrass = true;
    grass.TextureCardsPerSquareMeter = 0.73f;
    grass.TextureSize = 1.25f;
    grass.UseSplatRootColor = false;
    grass.RootColor = 0xFF112233u;
    grass.TipColor = 0xFF88AA22u;
    grass.BacklightColor = 0xFFE6F06Au;
    const GUID albedoGuid("00112233-4455-6677-8899-aabbccddeeff");
    const GUID alphaGuid("11112222-3333-4444-5555-666677778888");
    const GUID normalGuid("99998888-7777-6666-5555-444433332222");
    grass.AlbedoTextureAssetGuid.Set(albedoGuid);
    grass.AlphaTextureAssetGuid.Set(alphaGuid);
    grass.NormalTextureAssetGuid.Set(normalGuid);
    grass.AtlasColumns = 4u;
    grass.AtlasRows = 3u;
    grass.AtlasTileCount = 10u;
    grass.AlphaCutoff = 0.41f;
    grass.NormalStrength = 1.2f;
    w1.AddComponentImmediate(e, grass);
    ECS::Entity(&w1, e).SetEnabled<Components::TerrainGrass>(false);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("Terrain.terrainAsset = \"10325476-98ba-dcfe-0123-456789abcdef\""), std::string::npos);
    EXPECT_NE(saved.find("Terrain.terrainAssetType = 7"), std::string::npos);
    EXPECT_NE(saved.find("Terrain.materialTiling = 7.5"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.enabled = false"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.renderMode = Blend"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.bladesPerSquareMeter = 1.75"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.bladeHeight = 3.25"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.bladeWidth = 0.075"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.maxWidthRatio = 0.125"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.clumpSize = 2.25"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.clumpHeightVariance = 0.55"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.clumpAlignment = 0.8"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.clumpGather = 0.65"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.hueVariation = 0.7"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.rootShade = 0.55"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.bladeSegments = 9"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.randomScale = 0.42"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.layerIndex = 2"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.maskThreshold = 0.62"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.range = 620"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.densityFalloff = 3.5"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.placementSeed = 11.25"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.windDirection = -1.25"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.windGustSpeed = 1.4"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.windGustScale = 0.08"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.windStrength = 3.5"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.windRestingLean = 0.2"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.windFlutterAmount = 0.31"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.windFlutterSpeed = 4.6"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.windSeed = 9.5"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.brightness = 1.35"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.randomBrightness = 0.22"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.groundingStrength = 0.25"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.translucency = 0.65"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.textureGrass = true"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.textureCardsPerSquareMeter = 0.73"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.textureSize = 1.25"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.useSplatRootColor = false"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.rootColor = 4279312947"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.tipColor = 4287146530"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.backlightColor = 4293324906"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.albedoTexture = &{00112233-4455-6677-8899-aabbccddeeff}"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.alphaTexture = &{11112222-3333-4444-5555-666677778888}"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.normalTexture = &{99998888-7777-6666-5555-444433332222}"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.atlasColumns = 4"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.atlasRows = 3"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.atlasTileCount = 10"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.alphaCutoff = 0.41"), std::string::npos);
    EXPECT_NE(saved.find("TerrainGrass.normalStrength = 1.2"), std::string::npos);
    // The retired root-fade knobs are not emitted at all any more.
    EXPECT_EQ(saved.find("TerrainGrass.rootAlpha"), std::string::npos);

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto loadedEntity = FindByTag(w2, "terrain");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* loaded = w2.GetComponent<Components::Terrain>(loadedEntity);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->TerrainAssetGuid.ToGuid(), terrainAssetGuid);
    EXPECT_EQ(loaded->TerrainAssetType, 7u);
    EXPECT_FLOAT_EQ(loaded->MaterialTiling, 7.5f);
    const auto* loadedGrass = w2.GetComponent<Components::TerrainGrass>(loadedEntity);
    ASSERT_NE(loadedGrass, nullptr);
    EXPECT_FALSE(ECS::Entity(&w2, loadedEntity).IsEnabled<Components::TerrainGrass>());
    EXPECT_EQ(loadedGrass->RenderMode, Components::TerrainGrassRenderMode::Blend);
    EXPECT_FLOAT_EQ(loadedGrass->BladesPerSquareMeter, 1.75f);
    EXPECT_FLOAT_EQ(loadedGrass->BladeHeight, 3.25f);
    EXPECT_FLOAT_EQ(loadedGrass->BladeWidth, 0.075f);
    EXPECT_FLOAT_EQ(loadedGrass->MaxWidthRatio, 0.125f);
    EXPECT_FLOAT_EQ(loadedGrass->ClumpSize, 2.25f);
    EXPECT_FLOAT_EQ(loadedGrass->ClumpHeightVariance, 0.55f);
    EXPECT_FLOAT_EQ(loadedGrass->ClumpAlignment, 0.8f);
    EXPECT_FLOAT_EQ(loadedGrass->ClumpGather, 0.65f);
    EXPECT_FLOAT_EQ(loadedGrass->HueVariation, 0.7f);
    EXPECT_FLOAT_EQ(loadedGrass->RootShade, 0.55f);
    EXPECT_FLOAT_EQ(loadedGrass->RootFadeStart, 0.12f);
    EXPECT_FLOAT_EQ(loadedGrass->RootFadeEnd, 0.83f);
    EXPECT_EQ(loadedGrass->BladeSegments, 9u);
    EXPECT_FLOAT_EQ(loadedGrass->RandomScale, 0.42f);
    EXPECT_EQ(loadedGrass->LayerIndex, 2u);
    EXPECT_FLOAT_EQ(loadedGrass->MaskThreshold, 0.62f);
    EXPECT_FLOAT_EQ(loadedGrass->Range, 620.0f);
    EXPECT_FLOAT_EQ(loadedGrass->DensityFalloff, 3.5f);
    EXPECT_FLOAT_EQ(loadedGrass->PlacementSeed, 11.25f);
    EXPECT_FLOAT_EQ(loadedGrass->WindDirection, -1.25f);
    EXPECT_FLOAT_EQ(loadedGrass->WindGustSpeed, 1.4f);
    EXPECT_FLOAT_EQ(loadedGrass->WindGustScale, 0.08f);
    EXPECT_FLOAT_EQ(loadedGrass->WindStrength, 3.5f);
    EXPECT_FLOAT_EQ(loadedGrass->WindRestingLean, 0.2f);
    EXPECT_FLOAT_EQ(loadedGrass->WindFlutterAmount, 0.31f);
    EXPECT_FLOAT_EQ(loadedGrass->WindFlutterSpeed, 4.6f);
    EXPECT_FLOAT_EQ(loadedGrass->WindSeed, 9.5f);
    EXPECT_FLOAT_EQ(loadedGrass->Brightness, 1.35f);
    EXPECT_FLOAT_EQ(loadedGrass->RandomBrightness, 0.22f);
    EXPECT_FLOAT_EQ(loadedGrass->GroundingStrength, 0.25f);
    EXPECT_FLOAT_EQ(loadedGrass->BladeNormalForm, 0.42f);
    EXPECT_FLOAT_EQ(loadedGrass->BladeScatterGain, 0.37f);
    EXPECT_FLOAT_EQ(loadedGrass->Translucency, 0.65f);
    EXPECT_TRUE(loadedGrass->TextureGrass);
    EXPECT_FLOAT_EQ(loadedGrass->TextureCardsPerSquareMeter, 0.73f);
    EXPECT_FLOAT_EQ(loadedGrass->TextureSize, 1.25f);
    EXPECT_FALSE(loadedGrass->UseSplatRootColor);
    EXPECT_EQ(loadedGrass->RootColor, 0xFF112233u);
    EXPECT_EQ(loadedGrass->TipColor, 0xFF88AA22u);
    EXPECT_EQ(loadedGrass->BacklightColor, 0xFFE6F06Au);
    EXPECT_EQ(loadedGrass->AlbedoTextureAssetGuid.ToGuid(), albedoGuid);
    EXPECT_EQ(loadedGrass->AlphaTextureAssetGuid.ToGuid(), alphaGuid);
    EXPECT_EQ(loadedGrass->NormalTextureAssetGuid.ToGuid(), normalGuid);
    EXPECT_EQ(loadedGrass->AtlasColumns, 4u);
    EXPECT_EQ(loadedGrass->AtlasRows, 3u);
    EXPECT_EQ(loadedGrass->AtlasTileCount, 10u);
    EXPECT_FLOAT_EQ(loadedGrass->AlphaCutoff, 0.41f);
    EXPECT_FLOAT_EQ(loadedGrass->NormalStrength, 1.2f);
}

// DebugView is a viewing mode, not authored terrain: the schema applies it (which is
// how the debug server drives the CBT visualizations, ApplyComponentViaSchema) and
// never emits it, so a scene saved with facets on reopens shading normally.
TEST(SceneIO, TerrainDebugViewAppliesWithoutSerializing)
{
    Scene::EnsureTerrainSceneSchemasRegistered();
    const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find("Terrain");
    ASSERT_NE(schema, nullptr);

    ECS::World w;
    const ECS::EntityHandle e = w.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    Components::Terrain authored{};
    authored.SizeX = 512.0f;
    authored.TargetPixelError = 6.0f;
    w.AddComponentImmediate(e, authored);

    const Scene::SceneLoadContext loadCtx{};
    std::string err;
    ASSERT_TRUE(schema->ApplyProperty(w, e, loadCtx, "debugview", "1", &err)) << err;
    const auto* applied = w.GetComponent<Components::Terrain>(e);
    ASSERT_NE(applied, nullptr);
    EXPECT_EQ(applied->DebugView, Components::TerrainDebugView::Facets);
    // The rest of the terrain is untouched by the mode switch.
    EXPECT_FLOAT_EQ(applied->SizeX, 512.0f);
    EXPECT_FLOAT_EQ(applied->TargetPixelError, 6.0f);

    ASSERT_TRUE(schema->ApplyProperty(w, e, loadCtx, "debugview", "2", &err)) << err;
    EXPECT_EQ(w.GetComponent<Components::Terrain>(e)->DebugView,
              Components::TerrainDebugView::AtlasSlots);

    // Out of range fails loudly and leaves the mode where it was — the shader reads
    // this value as its debug branch index.
    EXPECT_FALSE(schema->ApplyProperty(w, e, loadCtx, "debugview", "3", &err));
    EXPECT_EQ(w.GetComponent<Components::Terrain>(e)->DebugView,
              Components::TerrainDebugView::AtlasSlots);

    const Scene::SceneSaveContext saveCtx{};
    std::vector<std::string> lines;
    schema->Serialize(w, e, saveCtx, lines);
    ASSERT_FALSE(lines.empty());
    for (std::string line : lines)
    {
        std::transform(line.begin(), line.end(), line.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        EXPECT_EQ(line.find("debugview"), std::string::npos) << line;
    }
}

// The authored water surface height round-trips through TerrainSchema, and an unset one leaves
// no line behind: the sentinel is the lowest float, which the formatter would not reproduce.
TEST(SceneIO, SaveThenLoad_RoundTripTerrainSeaLevel)
{
    Scene::EnsureTerrainSceneSchemasRegistered();

    auto roundTrip = [](const char* fileName, float seaLevel, std::string& outSaved) {
        const auto scenePath = MakeTempPath(fileName);
        ECS::World w1;
        const ECS::EntityHandle e = w1.CreateEntity();
        w1.AddComponentImmediate(e, Components::SceneEntityTag{{'i', 's', 'l', 'e', '\0'}});
        Components::Terrain terrain{};
        terrain.SeaLevel = seaLevel;
        w1.AddComponentImmediate(e, terrain);
        EXPECT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
        EXPECT_TRUE(ReadFile(scenePath, outSaved));

        ECS::World w2;
        EXPECT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
            << Scene::GetLastSceneIOError().message;
        const auto* loaded = w2.GetComponent<Components::Terrain>(FindByTag(w2, "isle"));
        return loaded ? loaded->SeaLevel : std::numeric_limits<float>::quiet_NaN();
    };

    std::string saved;
    EXPECT_FLOAT_EQ(roundTrip("terrain_sea_level_set.scene", 3.0f, saved), 3.0f);
    EXPECT_NE(saved.find("Terrain.seaLevel = 3"), std::string::npos);

    EXPECT_EQ(roundTrip("terrain_sea_level_unset.scene", Components::kNoTerrainSeaLevel, saved),
              Components::kNoTerrainSeaLevel);
    EXPECT_EQ(saved.find("Terrain.seaLevel"), std::string::npos);
}

// The planet/domain fields round-trip through TerrainSchema; the base relief round-trips
// through its own TerrainPlanetReliefSchema block (relief-unification slice).
TEST(SceneIO, SaveThenLoad_RoundTripPlanetTerrainFields)
{
    const auto scenePath = MakeTempPath("terrain_planet_roundtrip.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'p', 'l', 'a', 'n', 'e', 't', '\0'}});

    Components::Terrain terrain{};
    terrain.Domain = Components::TerrainDomain::Spherical;
    terrain.PlanetRadius = 5000.0f;
    terrain.TargetPixelError = 6.0f;
    w1.AddComponentImmediate(e, terrain);

    Components::TerrainPlanetRelief relief{};
    relief.Amplitude = 125.0f;
    relief.Frequency = 8.0f;
    relief.Octaves = 4u;
    w1.AddComponentImmediate(e, relief);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("Terrain.domain = 1"), std::string::npos);
    EXPECT_NE(saved.find("Terrain.planetRadius = 5000"), std::string::npos);
    EXPECT_NE(saved.find("Terrain.targetPixelError = 6"), std::string::npos);
    // Relief now serializes as its own component block, NOT as Terrain fields.
    EXPECT_EQ(saved.find("Terrain.planetRelief"), std::string::npos);
    EXPECT_NE(saved.find("TerrainPlanetRelief.amplitude = 125"), std::string::npos);
    EXPECT_NE(saved.find("TerrainPlanetRelief.frequency = 8"), std::string::npos);
    EXPECT_NE(saved.find("TerrainPlanetRelief.octaves = 4"), std::string::npos);
    // Retired fields must not be emitted anymore.
    EXPECT_EQ(saved.find("Terrain.patchGridSize"), std::string::npos);
    EXPECT_EQ(saved.find("Terrain.lodRangeScale"), std::string::npos);

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto loadedEntity = FindByTag(w2, "planet");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* loaded = w2.GetComponent<Components::Terrain>(loadedEntity);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->Domain, Components::TerrainDomain::Spherical);
    EXPECT_FLOAT_EQ(loaded->PlanetRadius, 5000.0f);
    EXPECT_FLOAT_EQ(loaded->TargetPixelError, 6.0f);

    const auto* loadedRelief = w2.GetComponent<Components::TerrainPlanetRelief>(loadedEntity);
    ASSERT_NE(loadedRelief, nullptr);
    EXPECT_FLOAT_EQ(loadedRelief->Amplitude, 125.0f);
    EXPECT_FLOAT_EQ(loadedRelief->Frequency, 8.0f);
    EXPECT_EQ(loadedRelief->Octaves, 4u);
}

// A planar terrain omits the planet block; the domain still round-trips as 0.
TEST(SceneIO, SaveThenLoad_PlanarTerrainOmitsPlanetBlock)
{
    const auto scenePath = MakeTempPath("terrain_planar_roundtrip.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'f', 'l', 'a', 't', '\0'}});
    Components::Terrain terrain{}; // default: planar
    terrain.SizeX = 512.0f;
    w1.AddComponentImmediate(e, terrain);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("Terrain.domain = 0"), std::string::npos);
    EXPECT_EQ(saved.find("Terrain.planetRadius"), std::string::npos);

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    const auto* loaded = w2.GetComponent<Components::Terrain>(FindByTag(w2, "flat"));
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->Domain, Components::TerrainDomain::Planar);
}

// A pre-usability scene carries retired Terrain.patchGridSize / Terrain.lodRangeScale
// lines. A hand-written schema hard-fails the whole load on an unknown key, so these
// must be parse-ignored (one release beat) — the scene loads and the terrain resolves.
// A scene authored before density became blades/m2 must still LOAD and SAVE cleanly. The retired
// knobs are recognized and dropped, not rejected: an unrecognized key hard-fails the hand-written
// schema, which marks the whole scene degraded and puts every other authored assignment at risk of
// being lost on the next save. Both spellings are covered — the TerrainGrass component's own
// property names and the legacy Terrain.grass* aliases.
TEST(SceneIO, Load_RetiredTerrainGrassFieldsAreIgnoredAndDoNotDegradeTheScene)
{
    const auto scenePath = MakeTempPath("terrain_grass_legacy_fields.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();
    Scene::EnsureTerrainGrassSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"LegacyGrass\" version=1]\n"
        "\n"
        "[entity id=\"legacygrass\"]\n"
        "Terrain.enabled = true\n"
        "Terrain.sizeX = 512\n"
        "Terrain.sizeZ = 512\n"
        "Terrain.heightScale = 100\n"
        "Terrain.grassDensity = 0.5\n"
        "Terrain.grassFadeStart = 350\n"
        "Terrain.grassFadeEnd = 500\n"
        "Terrain.grassScale = 2\n"
        "TerrainGrass.enabled = true\n"
        "TerrainGrass.density = 0.5\n"
        "TerrainGrass.fadeStart = 350\n"
        "TerrainGrass.fadeEnd = 500\n"
        "TerrainGrass.distanceFadeEnabled = true\n"
        "TerrainGrass.textureDensity = 0.18\n"
        "TerrainGrass.bladeHeight = 3.5\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto loadedEntity = FindByTag(w, "legacygrass");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* grass = w.GetComponent<Components::TerrainGrass>(loadedEntity);
    ASSERT_NE(grass, nullptr);
    // The assignments that still exist survived...
    EXPECT_TRUE(ECS::Entity(&w, loadedEntity).IsEnabled<Components::TerrainGrass>());
    EXPECT_FLOAT_EQ(grass->BladeHeight, 3.5f);
    // ...and the retired ones fell back to the current defaults rather than being reinterpreted.
    // No value is carried across: the old Density was a fraction of a terrain-sized lattice, so it
    // names no particular blades/m2.
    EXPECT_FLOAT_EQ(grass->BladesPerSquareMeter, 21.0f);
    EXPECT_FLOAT_EQ(grass->Range, 500.0f);
    EXPECT_FLOAT_EQ(grass->DensityFalloff, 2.0f);

    // And the round trip is clean: re-saving carries no retired key forward.
    const auto resavedPath = MakeTempPath("terrain_grass_legacy_resaved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w, resavedPath, Scene::SaveOptions{}));
    std::string resaved;
    ASSERT_TRUE(ReadFile(resavedPath, resaved));
    EXPECT_EQ(resaved.find("TerrainGrass.density ="), std::string::npos);
    EXPECT_EQ(resaved.find("TerrainGrass.fadeStart ="), std::string::npos);
    EXPECT_EQ(resaved.find("TerrainGrass.distanceFadeEnabled ="), std::string::npos);
    EXPECT_NE(resaved.find("TerrainGrass.bladesPerSquareMeter = 21"), std::string::npos);
}

using TestLog::CountLinesContaining;
using TestLog::FirstLineContaining;
using TestLog::ScopedEngineLogCapture;

// The root-fade knobs are retired: a blade body is opaque, and its base is grounded by the root
// embed and contact shading rather than by fading the geometry out. An unrecognized key would not
// fail the load — the tolerant loader would census it as degradation and preserve its text across
// saves forever — so retirement exists to warn once, drop the key, and let the next save shed it.
//
// This test owns rootAlpha/rootAlphaBlendRange within this binary. The warn-once set is a process
// static, so a second test loading these keys would silently turn the count assertions below into
// zero-vs-one noise; put new coverage for them here rather than in a sibling.
TEST(SceneIO, Load_RetiredTerrainGrassRootAlphaWarnsOnceAndResavesClean)
{
    const auto scenePath = MakeTempPath("terrain_grass_root_alpha.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();
    Scene::EnsureTerrainGrassSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"RootFade\" version=1]\n"
        "\n"
        "[entity id=\"rootfade\"]\n"
        "TerrainGrass.enabled = true\n"
        "TerrainGrass.rootAlpha = 0.9\n"
        "TerrainGrass.rootAlphaBlendRange = 0.15\n"
        "TerrainGrass.bladeHeight = 0.72\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    // Positive control on the instrument. A capture that sees nothing is indistinguishable from a
    // notice that was never emitted, and the second reading is the one that would quietly pass a
    // broken retirement. Prove the sink is live BEFORE reading a zero out of it.
    Logger::Log::Warning("SceneIO log-capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(CountLinesContaining(logLines, "SceneIO log-capture self test"), 1u)
        << "the log capture is not receiving engine warnings — every count below would be a "
           "false zero";
    logLines.clear();

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();

    // Exactly one notice per retired key, and each one is actionable: it names the key, says the
    // value is being dropped, and says what to do about it.
    std::string dump;
    for (const auto& l : logLines) dump += "\n  | " + l;
    EXPECT_EQ(CountLinesContaining(logLines, "TerrainGrass.rootAlpha is retired"), 1u)
        << "captured " << logLines.size() << " line(s):" << dump;
    EXPECT_EQ(CountLinesContaining(logLines, "TerrainGrass.rootAlphaBlendRange is retired"), 1u);
    const std::string notice = FirstLineContaining(logLines, "TerrainGrass.rootAlpha is retired");
    EXPECT_NE(notice.find("ignoring"), std::string::npos) << notice;
    EXPECT_NE(notice.find("Re-save"), std::string::npos) << notice;
    // The reason must describe THIS retirement, not the density/fade one that shares the helper.
    EXPECT_NE(notice.find("opaque"), std::string::npos) << notice;

    // The assignments that still exist survived the load.
    const auto loadedEntity = FindByTag(w, "rootfade");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* grass = w.GetComponent<Components::TerrainGrass>(loadedEntity);
    ASSERT_NE(grass, nullptr);
    EXPECT_TRUE(ECS::Entity(&w, loadedEntity).IsEnabled<Components::TerrainGrass>());
    EXPECT_FLOAT_EQ(grass->BladeHeight, 0.72f);

    // Warn ONCE per process: a second load of the same scene is silent.
    logLines.clear();
    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();
    EXPECT_EQ(CountLinesContaining(logLines, "is retired"), 0u);

    // And the round trip is clean: re-saving carries no retired key forward, under either spelling.
    const auto resavedPath = MakeTempPath("terrain_grass_root_alpha_resaved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w, resavedPath, Scene::SaveOptions{}));
    std::string resaved;
    ASSERT_TRUE(ReadFile(resavedPath, resaved));
    EXPECT_EQ(resaved.find("TerrainGrass.rootAlpha"), std::string::npos);
    EXPECT_NE(resaved.find("TerrainGrass.bladeHeight = 0.72"), std::string::npos);
}

// The contact pair is retired. rootContactAo dimmed the blade base's indirect diffuse and
// rootContactShade scaled the albedo it adopts from the ground; both dimmed only the BLADE, so
// their whole sub-1.0 ranges put a step on the contact line instead of a shadow. The contact is
// structural now, and no surviving dial can carry an authored value.
//
// This test owns both keys within this binary — the warn-once set is a process static, so a
// second test loading either would turn the count assertions below into zero-vs-one noise.
TEST(SceneIO, Load_RetiredTerrainGrassContactPairWarnsOnceAndResavesClean)
{
    const auto scenePath = MakeTempPath("terrain_grass_contact_pair.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();
    Scene::EnsureTerrainGrassSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"ContactAo\" version=1]\n"
        "\n"
        "[entity id=\"contactao\"]\n"
        "TerrainGrass.enabled = true\n"
        "TerrainGrass.rootContactAo = 0.78\n"
        "TerrainGrass.rootContactShade = 0.75\n"
        "TerrainGrass.rootShade = 0.82\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    // Positive control on the instrument: a capture that sees nothing is indistinguishable from a
    // notice that was never emitted.
    Logger::Log::Warning("SceneIO log-capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(CountLinesContaining(logLines, "SceneIO log-capture self test"), 1u)
        << "the log capture is not receiving engine warnings — every count below would be a "
           "false zero";
    logLines.clear();

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();

    std::string dump;
    for (const auto& l : logLines) dump += "\n  | " + l;
    EXPECT_EQ(CountLinesContaining(logLines, "TerrainGrass.rootContactAo is retired"), 1u)
        << "captured " << logLines.size() << " line(s):" << dump;
    EXPECT_EQ(CountLinesContaining(logLines, "TerrainGrass.rootContactShade is retired"), 1u)
        << "captured " << logLines.size() << " line(s):" << dump;
    const std::string notice =
        FirstLineContaining(logLines, "TerrainGrass.rootContactAo is retired");
    EXPECT_NE(notice.find("ignoring"), std::string::npos) << notice;
    EXPECT_NE(notice.find("Re-save"), std::string::npos) << notice;
    // Each reason must describe ITS retirement, not one of the others sharing the helper.
    EXPECT_NE(notice.find("ground beside it"), std::string::npos) << notice;
    const std::string shadeNotice =
        FirstLineContaining(logLines, "TerrainGrass.rootContactShade is retired");
    EXPECT_NE(shadeNotice.find("value step"), std::string::npos) << shadeNotice;

    // The load is not degraded: the assignments that still exist survived it.
    const auto loadedEntity = FindByTag(w, "contactao");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* grass = w.GetComponent<Components::TerrainGrass>(loadedEntity);
    ASSERT_NE(grass, nullptr);
    EXPECT_TRUE(ECS::Entity(&w, loadedEntity).IsEnabled<Components::TerrainGrass>());
    EXPECT_FLOAT_EQ(grass->RootShade, 0.82f);

    // Warn ONCE per process: a second load of the same scene is silent.
    logLines.clear();
    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();
    EXPECT_EQ(CountLinesContaining(logLines, "is retired"), 0u);

    // And the round trip is clean: re-saving sheds the key.
    const auto resavedPath = MakeTempPath("terrain_grass_contact_pair_resaved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w, resavedPath, Scene::SaveOptions{}));
    std::string resaved;
    ASSERT_TRUE(ReadFile(resavedPath, resaved));
    EXPECT_EQ(resaved.find("TerrainGrass.rootContact"), std::string::npos);
    EXPECT_NE(resaved.find("TerrainGrass.rootShade = 0.82"), std::string::npos);
}

// Colour variation has one owner. The hue PAIR became a single authored amount at the shipped
// ratio between its two granularities, and randomColor's per-blade tint had no amount dial at all —
// its strength was a shader literal — so neither retired key maps onto a value of the survivor.
//
// This test owns those three keys within this binary: the warn-once set is a process static, so a
// second test loading any of them would turn the count assertions below into zero-vs-one noise.
TEST(SceneIO, Load_RetiredTerrainGrassColourVariationKeysWarnOnceAndResaveClean)
{
    const auto scenePath = MakeTempPath("terrain_grass_colour_variation.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();
    Scene::EnsureTerrainGrassSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"ColourVariation\" version=1]\n"
        "\n"
        "[entity id=\"colourvar\"]\n"
        "TerrainGrass.enabled = true\n"
        "TerrainGrass.clumpHueVariance = 0.7\n"
        "TerrainGrass.bladeHueVariance = 0.45\n"
        "TerrainGrass.randomColor = 4282672844\n"
        "TerrainGrass.hueVariation = 0.25\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    // Positive control on the instrument: a capture that sees nothing is indistinguishable from a
    // notice that was never emitted.
    Logger::Log::Warning("SceneIO log-capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(CountLinesContaining(logLines, "SceneIO log-capture self test"), 1u)
        << "the log capture is not receiving engine warnings — every count below would be a "
           "false zero";
    logLines.clear();

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();

    std::string dump;
    for (const auto& l : logLines) dump += "\n  | " + l;
    for (const char* key : {"TerrainGrass.clumpHueVariance is retired",
                            "TerrainGrass.bladeHueVariance is retired",
                            "TerrainGrass.randomColor is retired"})
        EXPECT_EQ(CountLinesContaining(logLines, key), 1u)
            << key << "; captured " << logLines.size() << " line(s):" << dump;

    // No value is carried across: the survivor keeps what the scene authored for IT, and the
    // retired keys neither overwrite it nor are reinterpreted into it.
    const auto loadedEntity = FindByTag(w, "colourvar");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* grass = w.GetComponent<Components::TerrainGrass>(loadedEntity);
    ASSERT_NE(grass, nullptr);
    EXPECT_TRUE(ECS::Entity(&w, loadedEntity).IsEnabled<Components::TerrainGrass>());
    EXPECT_FLOAT_EQ(grass->HueVariation, 0.25f);

    // Warn ONCE per process: a second load of the same scene is silent.
    logLines.clear();
    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();
    EXPECT_EQ(CountLinesContaining(logLines, "is retired"), 0u);

    // And the round trip is clean: re-saving sheds all three keys and keeps the survivor.
    const auto resavedPath = MakeTempPath("terrain_grass_colour_variation_resaved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w, resavedPath, Scene::SaveOptions{}));
    std::string resaved;
    ASSERT_TRUE(ReadFile(resavedPath, resaved));
    EXPECT_EQ(resaved.find("TerrainGrass.clumpHueVariance"), std::string::npos);
    EXPECT_EQ(resaved.find("TerrainGrass.bladeHueVariance"), std::string::npos);
    EXPECT_EQ(resaved.find("TerrainGrass.randomColor"), std::string::npos);
    EXPECT_NE(resaved.find("TerrainGrass.hueVariation = 0.25"), std::string::npos);
}

// A RENAME retires the old key like any other: the loader drops it rather than carrying its value
// across, because a value that silently reappears under a different name is worse than a notice
// that says where it went. What makes that honest is the notice NAMING the successor, so this pins
// that and not just the drop. Every reachable scene authors the shipped default, so nothing that
// exists loses anything.
//
// This test owns shadowStrength within this binary: the warn-once set is a process static.
TEST(SceneIO, Load_RetiredTerrainGrassShadowStrengthNamesItsSuccessor)
{
    const auto scenePath = MakeTempPath("terrain_grass_shadow_strength.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();
    Scene::EnsureTerrainGrassSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"Grounding\" version=1]\n"
        "\n"
        "[entity id=\"grounding\"]\n"
        "TerrainGrass.enabled = true\n"
        "TerrainGrass.shadowStrength = 0.25\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);
    Logger::Log::Warning("SceneIO log-capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(CountLinesContaining(logLines, "SceneIO log-capture self test"), 1u)
        << "the log capture is not receiving engine warnings";
    logLines.clear();

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();

    EXPECT_EQ(CountLinesContaining(logLines, "TerrainGrass.shadowStrength is retired"), 1u);
    const std::string notice =
        FirstLineContaining(logLines, "TerrainGrass.shadowStrength is retired");
    EXPECT_NE(notice.find("groundingStrength"), std::string::npos)
        << "the notice does not name the key the value moved to: " << notice;

    // Dropped, not carried: the successor is at its own default.
    const auto loadedEntity = FindByTag(w, "grounding");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* grass = w.GetComponent<Components::TerrainGrass>(loadedEntity);
    ASSERT_NE(grass, nullptr);
    EXPECT_FLOAT_EQ(grass->GroundingStrength, 1.0f);

    const auto resavedPath = MakeTempPath("terrain_grass_shadow_strength_resaved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w, resavedPath, Scene::SaveOptions{}));
    std::string resaved;
    ASSERT_TRUE(ReadFile(resavedPath, resaved));
    EXPECT_EQ(resaved.find("TerrainGrass.shadowStrength"), std::string::npos);
    EXPECT_NE(resaved.find("TerrainGrass.groundingStrength = 1"), std::string::npos);
}
TEST(SceneIO, Load_RetiredTerrainFieldsAreIgnored)
{
    const auto scenePath = MakeTempPath("terrain_legacy_fields.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"Legacy\" version=1]\n"
        "\n"
        "[entity id=\"legacy\"]\n"
        "Terrain.enabled = true\n"
        "Terrain.sizeX = 512\n"
        "Terrain.sizeZ = 512\n"
        "Terrain.heightScale = 100\n"
        "Terrain.patchGridSize = 128\n"
        "Terrain.samplesPerMeter = 1\n"
        "Terrain.lodRangeScale = 1\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto loadedEntity = FindByTag(w, "legacy");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* loaded = w.GetComponent<Components::Terrain>(loadedEntity);
    ASSERT_NE(loaded, nullptr);
    EXPECT_FLOAT_EQ(loaded->SizeX, 512.0f);
    EXPECT_FLOAT_EQ(loaded->HeightScale, 100.0f);
}

// set_component renders a JSON string as RAW schema text (no quotes), so a per-layer albedo
// arrives as a bare GUID token. Both that and the quoted scene-file form must bind the layer;
// an explicit 0 clears it.
TEST(SceneIO, Load_TerrainLayerAlbedoAcceptsBareAndQuotedGuid)
{
    const auto scenePath = MakeTempPath("terrain_layer_albedo_forms.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    // Digit-leading on purpose: the number branch used to swallow "01234567-..." as Int.
    const std::string src =
        "[scene name=\"Layers\" version=1]\n"
        "\n"
        "[entity id=\"terrain\"]\n"
        "Terrain.enabled = true\n"
        "Terrain.layerAlbedo0 = 01234567-89ab-cdef-0123-456789abcdef\n"
        "Terrain.layerAlbedo1 = \"fedcba98-7654-3210-fedc-ba9876543210\"\n"
        "Terrain.layerAlbedo2 = 0\n"
        "Terrain.layerTiling1 = 3.5\n"
        "Terrain.layerHex1 = true\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto e = FindByTag(w, "terrain");
    ASSERT_TRUE(e.IsValid());
    const auto* loaded = w.GetComponent<Components::Terrain>(e);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->LayerAlbedoTexture[0].ToGuid(), GUID("01234567-89ab-cdef-0123-456789abcdef"));
    EXPECT_EQ(loaded->LayerAlbedoTexture[1].ToGuid(), GUID("fedcba98-7654-3210-fedc-ba9876543210"));
    EXPECT_TRUE(loaded->LayerAlbedoTexture[2].IsNull());
    EXPECT_FLOAT_EQ(loaded->LayerTiling[1], 3.5f);
    EXPECT_EQ(loaded->LayerHexTiling & 2u, 2u);
}

// The base heightmap reference takes the same two forms as a layer albedo: the quoted scene-file
// GUID and the bare token set_component hands the schema. The bare form used to be refused, so a
// terrain could not be bound to its heightmap over the debug port. An explicit 0 clears it.
TEST(SceneIO, Load_TerrainHeightmapAcceptsBareAndQuotedGuid)
{
    const auto scenePath = MakeTempPath("terrain_heightmap_forms.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"Heightmaps\" version=1]\n"
        "\n"
        "[entity id=\"bare\"]\n"
        "Terrain.baseSource = 1\n"
        "Terrain.terrainAsset = 01234567-89ab-cdef-0123-456789abcdef\n"
        "\n"
        "[entity id=\"quoted\"]\n"
        "Terrain.baseSource = 1\n"
        "Terrain.terrainAsset = \"fedcba98-7654-3210-fedc-ba9876543210\"\n"
        "\n"
        "[entity id=\"cleared\"]\n"
        "Terrain.terrainAsset = \"fedcba98-7654-3210-fedc-ba9876543210\"\n"
        "Terrain.terrainAsset = 0\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto* bare = w.GetComponent<Components::Terrain>(FindByTag(w, "bare"));
    const auto* quoted = w.GetComponent<Components::Terrain>(FindByTag(w, "quoted"));
    const auto* cleared = w.GetComponent<Components::Terrain>(FindByTag(w, "cleared"));
    ASSERT_NE(bare, nullptr);
    ASSERT_NE(quoted, nullptr);
    ASSERT_NE(cleared, nullptr);
    EXPECT_EQ(bare->TerrainAssetGuid.ToGuid(), GUID("01234567-89ab-cdef-0123-456789abcdef"));
    EXPECT_EQ(quoted->TerrainAssetGuid.ToGuid(), GUID("fedcba98-7654-3210-fedc-ba9876543210"));
    EXPECT_TRUE(cleared->TerrainAssetGuid.IsNull());
}

// GUID's string constructor yields a NULL guid for malformed text instead of throwing, so an
// unvalidated Set would silently UNBIND the layer. The schema must reject it loudly.
TEST(SceneIO, Load_TerrainLayerAlbedoRejectsMalformedGuid)
{
    const auto scenePath = MakeTempPath("terrain_layer_albedo_malformed.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"BadLayer\" version=1]\n"
        "\n"
        "[entity id=\"terrain\"]\n"
        "Terrain.enabled = true\n"
        "Terrain.layerAlbedo0 = \"not-a-guid\"\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    EXPECT_TRUE(Scene::LoadSceneFromFile(w, scenePath, opts));

    // The value is still rejected — it just costs the property, not the scene. Being named in the
    // census is what stops it clearing the layer silently.
    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].component, "Terrain");
    EXPECT_EQ(degradation.skips[0].field, "layerAlbedo0");
}

// A paint layer index is a splat CHANNEL the bake indexes, and the splat has exactly
// kMaxTerrainMaterialLayers of them. Scene text is hand-editable and script-writable, so an
// out-of-range value has to be clamped at the door the way the grass layer index already is —
// otherwise it reaches the bake, which used to drop the whole modifier and paint nothing at all.
// All four paint-carrying schemas parse the value independently, so all four are checked.
TEST(SceneIO, Load_PaintLayerIndexClampsToTheChannelCount)
{
    const auto scenePath = MakeTempPath("terrain_paint_layer_clamp.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    // TerrainPaintLayerModifier and TerrainSplineModifier are legacy shapes: the schema migrates
    // them into a TerrainPaintLayerEffect and removes itself during the load, so the clamp has to
    // hold on the MIGRATED value, which is what the bake actually reads. TerrainPaintZone and
    // TerrainPaintLayerEffect survive the load as themselves.
    const std::string src =
        "[scene name=\"PaintClamp\" version=1]\n"
        "\n"
        "[entity id=\"paintmod\"]\n"
        "TerrainPaintLayerModifier.layerIndex = 99\n"
        "\n"
        "[entity id=\"splinemod\"]\n"
        "TerrainSplineModifier.paintLayer = true\n"
        "TerrainSplineModifier.paintLayerIndex = 7\n"
        "\n"
        "[entity id=\"paintzone\"]\n"
        "TerrainPaintZone.layerIndex = 250\n"
        "\n"
        "[entity id=\"painteffect\"]\n"
        "TerrainPaintLayerEffect.layerIndex = 4\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const uint32 lastChannel = Terrain::kMaxTerrainMaterialLayers - 1u;

    const auto* pm =
        w.GetComponent<Components::TerrainPaintLayerEffect>(FindByTag(w, "paintmod"));
    ASSERT_NE(pm, nullptr) << "the legacy paint modifier must migrate into a paint effect";
    EXPECT_EQ(pm->LayerIndex, lastChannel);

    const auto* sm =
        w.GetComponent<Components::TerrainPaintLayerEffect>(FindByTag(w, "splinemod"));
    ASSERT_NE(sm, nullptr) << "a painting spline modifier must migrate into a paint effect";
    EXPECT_EQ(sm->LayerIndex, lastChannel);

    const auto* pz = w.GetComponent<Components::TerrainPaintZone>(FindByTag(w, "paintzone"));
    ASSERT_NE(pz, nullptr);
    EXPECT_EQ(pz->LayerIndex, lastChannel);

    const auto* pe =
        w.GetComponent<Components::TerrainPaintLayerEffect>(FindByTag(w, "painteffect"));
    ASSERT_NE(pe, nullptr);
    EXPECT_EQ(pe->LayerIndex, lastChannel);
}

// In-range values must survive untouched — a clamp that also rewrote legal input would silently
// collapse every painted layer onto one channel, which the test above could not tell apart.
TEST(SceneIO, Load_PaintLayerIndexLeavesInRangeValuesAlone)
{
    const auto scenePath = MakeTempPath("terrain_paint_layer_inrange.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"PaintInRange\" version=1]\n"
        "\n"
        "[entity id=\"paintmod\"]\n"
        "TerrainPaintLayerModifier.layerIndex = 0\n"
        "\n"
        "[entity id=\"paintzone\"]\n"
        "TerrainPaintZone.layerIndex = 2\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto* pm =
        w.GetComponent<Components::TerrainPaintLayerEffect>(FindByTag(w, "paintmod"));
    ASSERT_NE(pm, nullptr);
    EXPECT_EQ(pm->LayerIndex, 0u);

    const auto* pz = w.GetComponent<Components::TerrainPaintZone>(FindByTag(w, "paintzone"));
    ASSERT_NE(pz, nullptr);
    EXPECT_EQ(pz->LayerIndex, 2u);
}

// The tests above cover the PARSE side only. The layer authoring UI writes these fields, and the
// schema emits per-layer lines only when they differ from the default — so a serialize-side gap
// would drop a user's layer authoring at the next load with nothing failing. Author all twelve
// per-layer fields plus the global tiling, save, reload, compare.
TEST(SceneIO, SaveThenLoad_TerrainMaterialLayersRoundTrip)
{
    const auto scenePath = MakeTempPath("terrain_material_layers_roundtrip.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    static constexpr const char* kLayerGuids[4] = {
        "f78498e7-2643-4d6e-a345-d96c7bede21e",
        "f0d2ab62-d04a-4d09-be27-b0ea9573dc68",
        "98356aeb-6cbe-48f4-a62a-f8622ab5cb35",
        "7305db25-159f-44dc-bd58-2699954a00b4",
    };
    // Distinct per layer: a serializer that wrote one layer's value into every slot would pass an
    // all-equal fixture.
    constexpr float kLayerTilings[4] = {0.5f, 2.0f, 3.25f, 8.0f};
    constexpr uint32 kHexMask = (1u << 1) | (1u << 3); // per-layer bits, not a global flag

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'l', 'a', 'y', 'e', 'r', '\0'}});
    Components::Terrain terrain{};
    terrain.MaterialTiling = 3.0f;
    for (uint32 i = 0; i < 4; ++i)
    {
        terrain.LayerAlbedoTexture[i].Set(GUID(kLayerGuids[i]));
        terrain.LayerTiling[i] = kLayerTilings[i];
    }
    terrain.LayerHexTiling = kHexMask;
    w1.AddComponentImmediate(e, terrain);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    const auto* loaded = w2.GetComponent<Components::Terrain>(FindByTag(w2, "layer"));
    ASSERT_NE(loaded, nullptr);

    EXPECT_FLOAT_EQ(loaded->MaterialTiling, 3.0f);
    for (uint32 i = 0; i < 4; ++i)
    {
        EXPECT_EQ(loaded->LayerAlbedoTexture[i].ToGuid(), GUID(kLayerGuids[i])) << "layer " << i;
        EXPECT_FLOAT_EQ(loaded->LayerTiling[i], kLayerTilings[i]) << "layer " << i;
    }
    EXPECT_EQ(loaded->LayerHexTiling, kHexMask);
}

// The material library reference is what carries a terrain's authored materials across a
// save/load, so a serializer that dropped it would lose every material the user authored and
// silently fall the terrain back to the built-in palette.
TEST(SceneIO, SaveThenLoad_TerrainMaterialLibraryRoundTrips)
{
    const auto scenePath = MakeTempPath("terrain_material_library_roundtrip.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    static constexpr const char* kLibraryGuid = "3f0f7b4a-6a2c-4a3e-9d67-1c4a0b2e5f81";

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'m', 'a', 't', 'l', 'i', 'b', '\0'}});
    Components::Terrain terrain{};
    terrain.MaterialLibraryGuid.Set(GUID(kLibraryGuid));
    w1.AddComponentImmediate(e, terrain);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("Terrain.materialLibrary"), std::string::npos);

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    const auto* loaded = w2.GetComponent<Components::Terrain>(FindByTag(w2, "matlib"));
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->MaterialLibraryGuid.ToGuid(), GUID(kLibraryGuid));
}

// Which material each channel role shades with is authored per terrain, so a serializer that
// dropped it would silently re-point every role at the identity slot and repaint the terrain.
TEST(SceneIO, SaveThenLoad_TerrainLayerRoleSlotsRoundTrip)
{
    const auto scenePath = MakeTempPath("terrain_layer_roles_roundtrip.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    // Distinct per role and none of them the identity default: a serializer that wrote one role's
    // slot into every entry, or that skipped the field and left the defaults, would both fail.
    constexpr uint8 kRoleSlots[4] = {7u, 0u, 255u, 12u};

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'r', 'o', 'l', 'e', 's', '\0'}});
    Components::Terrain terrain{};
    for (uint32 i = 0; i < 4; ++i)
        terrain.LayerRoleSlot[i] = kRoleSlots[i];
    w1.AddComponentImmediate(e, terrain);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    const auto* loaded = w2.GetComponent<Components::Terrain>(FindByTag(w2, "roles"));
    ASSERT_NE(loaded, nullptr);
    for (uint32 i = 0; i < 4; ++i)
        EXPECT_EQ(loaded->LayerRoleSlot[i], kRoleSlots[i]) << "role " << i;
}

// The identity binding is what an unmigrated terrain carries, so it writes no line at all — the
// same "only non-default lines serialize" rule the per-layer fields follow.
TEST(SceneIO, SaveThenLoad_TerrainDefaultLayerRolesWriteNoLines)
{
    const auto scenePath = MakeTempPath("terrain_default_layer_roles.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'i', 'd', 'e', 'n', 't', '\0'}});
    w1.AddComponentImmediate(e, Components::Terrain{});

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_EQ(saved.find("Terrain.layerRole"), std::string::npos);
}

// SCENES ON DISK CARRY THE OLD KEY. `layerRole<i>` is what saves today; `classifierRole<i>`
// is the name it shipped under, and an unknown property does not degrade gracefully here --
// it fails the WHOLE scene load (SceneIO's ApplyProperties returns false), and a failed load
// is SILENT, leaving the previously-open world rendering with nothing on screen to say so.
//
// Both keys must therefore land the VALUE in the right slot. Accepting-and-ignoring the old
// one would be worse than the failure it replaces: the terrain would load with the identity
// binding and silently shade with different materials than the author chose.
//
// Modelled on the real carriers this was found on (ComposedIsland.scene classifierRole2 = 4,
// ComposedIsland-mintgate.scene classifierRole3 = 5).
TEST(SceneIO, Load_TerrainAcceptsTheLegacyClassifierRoleKeyAndMapsItsValue)
{
    const auto scenePath = MakeTempPath("terrain_legacy_classifier_role.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"LegacyRole\" version=1]\n"
        "\n"
        "[entity id=\"t\"]\n"
        "Terrain.classifierRole2 = 4\n"
        "Terrain.classifierRole3 = 5\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << "a scene carrying the shipped key name must still load: "
        << Scene::GetLastSceneIOError().message;

    const auto* loaded = w.GetComponent<Components::Terrain>(FindByTag(w, "t"));
    ASSERT_NE(loaded, nullptr);
    // The VALUES, not just a successful load: role 2 -> slot 4, role 3 -> slot 5, and the
    // untouched roles keep the identity default. Accept-and-ignore would leave all four at
    // identity and pass a load-only assertion.
    EXPECT_EQ(loaded->LayerRoleSlot[0], 0u);
    EXPECT_EQ(loaded->LayerRoleSlot[1], 1u);
    EXPECT_EQ(loaded->LayerRoleSlot[2], 4u) << "the legacy key loaded but its value was dropped";
    EXPECT_EQ(loaded->LayerRoleSlot[3], 5u) << "the legacy key loaded but its value was dropped";
}

// The alias is READ-ONLY: a save rewrites the file under the current name, so a scene migrates
// off the old key the first time it is saved rather than carrying it forever.
TEST(SceneIO, SaveThenLoad_TerrainRoleSlotsSaveUnderTheCurrentKeyName)
{
    const auto scenePath = MakeTempPath("terrain_role_key_migrates_on_save.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'m', 'i', 'g', '\0'}});
    Components::Terrain terrain{};
    terrain.LayerRoleSlot[2] = 4u;
    w1.AddComponentImmediate(e, terrain);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("Terrain.layerRole2"), std::string::npos);
    EXPECT_EQ(saved.find("Terrain.classifierRole"), std::string::npos)
        << "saving must migrate the key, not re-emit the legacy name";
}

// A slot ID is 8-bit by construction, so a wider value is malformed rather than clampable:
// clamping would bind the role to slot 255 and shade with a material the author never chose.
TEST(SceneIO, Load_TerrainLayerRoleRejectsOutOfRangeSlot)
{
    const auto scenePath = MakeTempPath("terrain_layer_role_out_of_range.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"BadRole\" version=1]\n"
        "\n"
        "[entity id=\"t\"]\n"
        "Terrain.layerRole1 = 256\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    EXPECT_TRUE(Scene::LoadSceneFromFile(w, scenePath, opts));

    // Still not clamped to 255 — the assignment is skipped and censused rather than coerced.
    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].component, "Terrain");
    EXPECT_EQ(degradation.skips[0].field, "layerRole1");
}

// A terrain with no library writes no library line — the same "only non-default lines serialize"
// rule the per-layer fields follow, so an unmigrated scene's block stays exactly as it was.
TEST(SceneIO, SaveThenLoad_TerrainWithoutLibraryWritesNoLibraryLine)
{
    const auto scenePath = MakeTempPath("terrain_no_material_library.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'n', 'o', 'l', 'i', 'b', '\0'}});
    w1.AddComponentImmediate(e, Components::Terrain{});

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_EQ(saved.find("Terrain.materialLibrary"), std::string::npos);
}

// Malformed library text must FAIL the load rather than Set: GUID's string constructor yields a
// NULL guid for it, so an unvalidated Set would silently drop the terrain's whole material list.
TEST(SceneIO, Load_TerrainMaterialLibraryRejectsMalformedGuid)
{
    const auto scenePath = MakeTempPath("terrain_material_library_bad_guid.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"BadLib\" version=1]\n"
        "\n"
        "[entity id=\"t\"]\n"
        "Terrain.materialLibrary = \"not-a-guid\"\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    EXPECT_TRUE(Scene::LoadSceneFromFile(w, scenePath, opts));

    // Still not Set from malformed text — skipped and censused, so it cannot silently drop the
    // terrain's whole material list.
    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].component, "Terrain");
    EXPECT_EQ(degradation.skips[0].field, "materialLibrary");
}

// An untextured terrain writes no per-layer lines at all — the property the schema's
// "only non-default lines serialize" rule exists for, and the state the inspector card presents
// as the built-in tint fallback.
TEST(SceneIO, SaveThenLoad_UntexturedTerrainWritesNoLayerLines)
{
    const auto scenePath = MakeTempPath("terrain_untextured_layers.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'p', 'l', 'a', 'i', 'n', '\0'}});
    w1.AddComponentImmediate(e, Components::Terrain{});

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_EQ(saved.find("Terrain.layerAlbedo"), std::string::npos);
    EXPECT_EQ(saved.find("Terrain.layerTiling"), std::string::npos);
    EXPECT_EQ(saved.find("Terrain.layerHex"), std::string::npos);
}

// A pre-merge scene carries a scanner-serialized TerrainCBT block (enum name form,
// how it was actually written). Post-merge the type is gone: without migration the
// block loads as an unknown component (values never applied) and the planet silently
// flattens to Planar. This must fold the fields into Terrain and CONSUME the block so
// a re-save drops it. maxDepth is intentionally dropped (auto-derived now).
TEST(SceneIO, Load_LegacyTerrainCBTBlockMigratesIntoTerrain)
{
    const auto scenePath = MakeTempPath("terrain_cbt_migration.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"LegacyPlanet\" version=1]\n"
        "\n"
        "[entity id=\"planet\"]\n"
        "Terrain.enabled = true\n"
        "Terrain.sizeX = 512\n"
        "Terrain.sizeZ = 512\n"
        "TerrainCBT.Domain = Spherical\n"
        "TerrainCBT.PlanetRadius = 5000\n"
        "TerrainCBT.PlanetReliefAmplitude = 125\n"
        "TerrainCBT.PlanetReliefFrequency = 8\n"
        "TerrainCBT.PlanetReliefOctaves = 4\n"
        "TerrainCBT.TargetPixelError = 6\n"
        "TerrainCBT.MaxDepth = 26\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto e = FindByTag(w, "planet");
    ASSERT_TRUE(e.IsValid());
    const auto* loaded = w.GetComponent<Components::Terrain>(e);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->Domain, Components::TerrainDomain::Spherical); // the planet stays a planet
    EXPECT_FLOAT_EQ(loaded->PlanetRadius, 5000.0f);
    EXPECT_FLOAT_EQ(loaded->TargetPixelError, 6.0f);
    EXPECT_FLOAT_EQ(loaded->SizeX, 512.0f);          // from the Terrain block
    EXPECT_EQ(loaded->MaxDepthOverride, 0u);         // MaxDepth dropped -> auto

    // The relief folds into the companion component (values exact).
    const auto* loadedRelief = w.GetComponent<Components::TerrainPlanetRelief>(e);
    ASSERT_NE(loadedRelief, nullptr);
    EXPECT_FLOAT_EQ(loadedRelief->Amplitude, 125.0f);
    EXPECT_FLOAT_EQ(loadedRelief->Frequency, 8.0f);
    EXPECT_EQ(loadedRelief->Octaves, 4u);

    // Consumed: a re-save must NOT re-emit the orphan block.
    const auto resavePath = MakeTempPath("terrain_cbt_migration_resave.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w, resavePath, Scene::SaveOptions{}));
    std::string resaved;
    ASSERT_TRUE(ReadFile(resavePath, resaved));
    EXPECT_EQ(resaved.find("TerrainCBT"), std::string::npos);
    EXPECT_NE(resaved.find("Terrain.domain = 1"), std::string::npos);
    EXPECT_NE(resaved.find("Terrain.planetRadius = 5000"), std::string::npos);
}

// The integer domain form (TerrainCBT.Domain = 1) also migrates — the reflected enum
// codec's dual-read means old/hand-edited scenes may carry either form.
TEST(SceneIO, Load_LegacyTerrainCBTIntegerDomainMigrates)
{
    const auto scenePath = MakeTempPath("terrain_cbt_int_domain.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"LegacyPlanetInt\" version=1]\n"
        "\n"
        "[entity id=\"planet\"]\n"
        "TerrainCBT.Domain = 1\n"
        "TerrainCBT.PlanetRadius = 5000\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    const auto* loaded = w.GetComponent<Components::Terrain>(FindByTag(w, "planet"));
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->Domain, Components::TerrainDomain::Spherical);
    EXPECT_FLOAT_EQ(loaded->PlanetRadius, 5000.0f);
}

// The discriminating relief-unification fixture: a scene from the immediately-prior era
// carries the relief as Terrain.planetRelief* fields (not a separate block). Post-slice
// the Terrain component has no such fields, so without in-place migration the planet
// would render with the default relief. This must fold each legacy field into the
// companion TerrainPlanetRelief component on the SAME entity, byte-exact, and a re-save
// must emit the component block (never the retired Terrain keys).
TEST(SceneIO, Load_LegacyTerrainReliefFieldsMigrateIntoComponent)
{
    const auto scenePath = MakeTempPath("terrain_relief_field_migration.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    const std::string src =
        "[scene name=\"LegacyReliefFields\" version=1]\n"
        "\n"
        "[entity id=\"planet\"]\n"
        "Terrain.enabled = true\n"
        "Terrain.domain = 1\n"
        "Terrain.planetRadius = 5000\n"
        "Terrain.planetReliefAmplitude = 125\n"
        "Terrain.planetReliefFrequency = 8\n"
        "Terrain.planetReliefOctaves = 4\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto e = FindByTag(w, "planet");
    ASSERT_TRUE(e.IsValid());
    const auto* loaded = w.GetComponent<Components::Terrain>(e);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->Domain, Components::TerrainDomain::Spherical);
    EXPECT_FLOAT_EQ(loaded->PlanetRadius, 5000.0f);

    const auto* relief = w.GetComponent<Components::TerrainPlanetRelief>(e);
    ASSERT_NE(relief, nullptr) << "legacy Terrain.planetRelief* must migrate into the component";
    EXPECT_FLOAT_EQ(relief->Amplitude, 125.0f);
    EXPECT_FLOAT_EQ(relief->Frequency, 8.0f);
    EXPECT_EQ(relief->Octaves, 4u);

    // Re-save: the retired Terrain keys are gone; the component block carries the relief.
    const auto resavePath = MakeTempPath("terrain_relief_field_migration_resave.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w, resavePath, Scene::SaveOptions{}));
    std::string resaved;
    ASSERT_TRUE(ReadFile(resavePath, resaved));
    EXPECT_EQ(resaved.find("Terrain.planetRelief"), std::string::npos);
    EXPECT_NE(resaved.find("TerrainPlanetRelief.amplitude = 125"), std::string::npos);
}

TEST(SceneIO, SaveThenLoad_RoundTripHeightFogEffectCamelCaseProperties)
{
    const auto scenePath = MakeTempPath("height_fog.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'p', 'o', 's', 't', '\0'}});

    Components::HeightFogEffect fog{};
    fog.Enabled = true;
    fog.Preset = Components::HeightFogPreset::MountainValley;
    fog.Intensity = 1.0f;
    fog.Density = 0.47f;
    fog.MaxOpacity = 0.68f;
    fog.DistanceFogEnabled = false;
    fog.MinDistance = 2.0f;
    fog.SmoothLength = 3.0f;
    fog.MaxDistance = 640.0f;
    fog.LayerMode = Components::HeightFogLayerMode::Additive;
    fog.HeightFogEnabled = true;
    fog.BaseHeight = 4.0f;
    fog.TransitionLength = 120.0f;
    fog.HorizonHeightOffset = 33.0f;
    fog.HorizonHeightBlendStart = 210.0f;
    fog.HorizonHeightBlendEnd = 620.0f;
    fog.AxisMode = Components::HeightFogAxisMode::Custom;
    fog.CustomAxis[0] = 0.1f;
    fog.CustomAxis[1] = 0.9f;
    fog.CustomAxis[2] = 0.2f;
    fog.Emissive[0] = 0.48f;
    fog.Emissive[1] = 0.54f;
    fog.Emissive[2] = 0.60f;
    fog.GradientMode = Components::HeightFogGradientMode::MainLight;
    fog.GradientStrength = 0.38f;
    fog.GradientLowColor[0] = 0.2f;
    fog.GradientLowColor[1] = 0.3f;
    fog.GradientLowColor[2] = 0.4f;
    fog.GradientHighColor[0] = 0.7f;
    fog.GradientHighColor[1] = 0.8f;
    fog.GradientHighColor[2] = 0.9f;
    fog.TrackDirectionalLight = false;
    fog.SunDirection[0] = 0.35f;
    fog.SunDirection[1] = -0.65f;
    fog.SunDirection[2] = 0.68f;
    fog.SunColor[0] = 1.0f;
    fog.SunColor[1] = 0.82f;
    fog.SunColor[2] = 0.58f;
    fog.SunIntensity = 1.0f;
    fog.Phase = -0.5f;
    fog.PhaseWeight0 = 1.0f;
    fog.PhaseWeight1 = 0.0f;
    fog.NoiseEnabled = true;
    fog.NoiseScale = 44.0f;
    fog.NoiseStrength = 0.27f;
    fog.NoiseVelocity[0] = 0.01f;
    fog.NoiseVelocity[1] = 0.02f;
    fog.NoiseVelocity[2] = 0.03f;
    fog.NoiseContrast = 1.7f;
    fog.NoiseMin = 0.12f;
    fog.NoiseMax = 0.84f;
    fog.NoiseFadeStart = 140.0f;
    fog.NoiseFadeEnd = 500.0f;
    fog.UseTimeOfDay = true;
    fog.SkyEnabled = true;
    fog.SkyPower = 1.0f;
    fog.SkyFillStart = 0.0f;
    fog.SkyFillEnd = 1.0f;
    fog.SkyHorizonOffset = 0.07f;
    fog.SkyBottomStrength = 0.18f;
    fog.FogGlowEnabled = true;
    fog.FogGlowQualityLevel = Components::FogGlowQuality::Cinematic;
    fog.FogGlowIntensity = 0.42f;
    fog.FogGlowRadius = 4.5f;
    fog.FogGlowOctaves = 7;
    fog.FogGlowScatter = 0.73f;
    fog.FogGlowThreshold = 1.2f;
    fog.FogGlowKnee = 0.35f;
    fog.FogGlowFadeStart = 0.08f;
    fog.FogGlowFadeEnd = 0.66f;
    fog.FogGlowTint[0] = 1.1f;
    fog.FogGlowTint[1] = 0.8f;
    fog.FogGlowTint[2] = 0.6f;
    fog.FogGlowAntiFlicker = false;
    w1.AddComponentImmediate(e, fog);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("HeightFogEffect.layerMode = 1"), std::string::npos);
    EXPECT_NE(saved.find("HeightFogEffect.horizonHeightOffset = 33"), std::string::npos);
    EXPECT_NE(saved.find("HeightFogEffect.noiseMin = 0.12"), std::string::npos);
    EXPECT_NE(saved.find("HeightFogEffect.skyBottomStrength = 0.18"), std::string::npos);
    EXPECT_NE(saved.find("HeightFogEffect.fogGlowOctaves = 7"), std::string::npos);
    EXPECT_NE(saved.find("HeightFogEffect.fogGlowQuality = 3"), std::string::npos);

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto h = FindByTag(w2, "post");
    ASSERT_TRUE(h.IsValid());
    const auto* loaded = w2.GetComponent<Components::HeightFogEffect>(h);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->Enabled);
    EXPECT_EQ(loaded->Preset, fog.Preset);
    EXPECT_FLOAT_EQ(loaded->Intensity, fog.Intensity);
    EXPECT_FLOAT_EQ(loaded->Density, fog.Density);
    EXPECT_FLOAT_EQ(loaded->MaxOpacity, fog.MaxOpacity);
    EXPECT_EQ(loaded->DistanceFogEnabled, fog.DistanceFogEnabled);
    EXPECT_FLOAT_EQ(loaded->MinDistance, fog.MinDistance);
    EXPECT_FLOAT_EQ(loaded->SmoothLength, fog.SmoothLength);
    EXPECT_FLOAT_EQ(loaded->MaxDistance, fog.MaxDistance);
    EXPECT_EQ(loaded->LayerMode, fog.LayerMode);
    EXPECT_EQ(loaded->HeightFogEnabled, fog.HeightFogEnabled);
    EXPECT_FLOAT_EQ(loaded->BaseHeight, fog.BaseHeight);
    EXPECT_FLOAT_EQ(loaded->TransitionLength, fog.TransitionLength);
    EXPECT_FLOAT_EQ(loaded->HorizonHeightOffset, fog.HorizonHeightOffset);
    EXPECT_FLOAT_EQ(loaded->HorizonHeightBlendStart, fog.HorizonHeightBlendStart);
    EXPECT_FLOAT_EQ(loaded->HorizonHeightBlendEnd, fog.HorizonHeightBlendEnd);
    EXPECT_EQ(loaded->AxisMode, fog.AxisMode);
    EXPECT_FLOAT_EQ(loaded->CustomAxis[0], fog.CustomAxis[0]);
    EXPECT_FLOAT_EQ(loaded->CustomAxis[1], fog.CustomAxis[1]);
    EXPECT_FLOAT_EQ(loaded->CustomAxis[2], fog.CustomAxis[2]);
    EXPECT_FLOAT_EQ(loaded->Emissive[0], fog.Emissive[0]);
    EXPECT_FLOAT_EQ(loaded->Emissive[1], fog.Emissive[1]);
    EXPECT_FLOAT_EQ(loaded->Emissive[2], fog.Emissive[2]);
    EXPECT_EQ(loaded->GradientMode, fog.GradientMode);
    EXPECT_FLOAT_EQ(loaded->GradientStrength, fog.GradientStrength);
    EXPECT_FLOAT_EQ(loaded->GradientLowColor[0], fog.GradientLowColor[0]);
    EXPECT_FLOAT_EQ(loaded->GradientLowColor[1], fog.GradientLowColor[1]);
    EXPECT_FLOAT_EQ(loaded->GradientLowColor[2], fog.GradientLowColor[2]);
    EXPECT_FLOAT_EQ(loaded->GradientHighColor[0], fog.GradientHighColor[0]);
    EXPECT_FLOAT_EQ(loaded->GradientHighColor[1], fog.GradientHighColor[1]);
    EXPECT_FLOAT_EQ(loaded->GradientHighColor[2], fog.GradientHighColor[2]);
    EXPECT_EQ(loaded->TrackDirectionalLight, fog.TrackDirectionalLight);
    EXPECT_FLOAT_EQ(loaded->SunDirection[0], fog.SunDirection[0]);
    EXPECT_FLOAT_EQ(loaded->SunDirection[1], fog.SunDirection[1]);
    EXPECT_FLOAT_EQ(loaded->SunDirection[2], fog.SunDirection[2]);
    EXPECT_FLOAT_EQ(loaded->SunColor[0], fog.SunColor[0]);
    EXPECT_FLOAT_EQ(loaded->SunColor[1], fog.SunColor[1]);
    EXPECT_FLOAT_EQ(loaded->SunColor[2], fog.SunColor[2]);
    EXPECT_FLOAT_EQ(loaded->SunIntensity, fog.SunIntensity);
    EXPECT_FLOAT_EQ(loaded->SunIntensityScale, fog.SunIntensityScale);
    EXPECT_FLOAT_EQ(loaded->Phase, fog.Phase);
    EXPECT_FLOAT_EQ(loaded->PhaseWeight0, fog.PhaseWeight0);
    EXPECT_FLOAT_EQ(loaded->PhaseWeight1, fog.PhaseWeight1);
    EXPECT_EQ(loaded->NoiseEnabled, fog.NoiseEnabled);
    EXPECT_FLOAT_EQ(loaded->NoiseScale, fog.NoiseScale);
    EXPECT_FLOAT_EQ(loaded->NoiseStrength, fog.NoiseStrength);
    EXPECT_FLOAT_EQ(loaded->NoiseVelocity[0], fog.NoiseVelocity[0]);
    EXPECT_FLOAT_EQ(loaded->NoiseVelocity[1], fog.NoiseVelocity[1]);
    EXPECT_FLOAT_EQ(loaded->NoiseVelocity[2], fog.NoiseVelocity[2]);
    EXPECT_FLOAT_EQ(loaded->NoiseContrast, fog.NoiseContrast);
    EXPECT_FLOAT_EQ(loaded->NoiseMin, fog.NoiseMin);
    EXPECT_FLOAT_EQ(loaded->NoiseMax, fog.NoiseMax);
    EXPECT_FLOAT_EQ(loaded->NoiseFadeStart, fog.NoiseFadeStart);
    EXPECT_FLOAT_EQ(loaded->NoiseFadeEnd, fog.NoiseFadeEnd);
    EXPECT_EQ(loaded->UseTimeOfDay, fog.UseTimeOfDay);
    EXPECT_EQ(loaded->SkyEnabled, fog.SkyEnabled);
    EXPECT_FLOAT_EQ(loaded->SkyPower, fog.SkyPower);
    EXPECT_FLOAT_EQ(loaded->SkyFillStart, fog.SkyFillStart);
    EXPECT_FLOAT_EQ(loaded->SkyFillEnd, fog.SkyFillEnd);
    EXPECT_FLOAT_EQ(loaded->SkyHorizonOffset, fog.SkyHorizonOffset);
    EXPECT_FLOAT_EQ(loaded->SkyBottomStrength, fog.SkyBottomStrength);
    EXPECT_EQ(loaded->FogGlowEnabled, fog.FogGlowEnabled);
    EXPECT_EQ(loaded->FogGlowQualityLevel, fog.FogGlowQualityLevel);
    EXPECT_FLOAT_EQ(loaded->FogGlowIntensity, fog.FogGlowIntensity);
    EXPECT_FLOAT_EQ(loaded->FogGlowRadius, fog.FogGlowRadius);
    EXPECT_EQ(loaded->FogGlowOctaves, fog.FogGlowOctaves);
    EXPECT_FLOAT_EQ(loaded->FogGlowScatter, fog.FogGlowScatter);
    EXPECT_FLOAT_EQ(loaded->FogGlowThreshold, fog.FogGlowThreshold);
    EXPECT_FLOAT_EQ(loaded->FogGlowKnee, fog.FogGlowKnee);
    EXPECT_FLOAT_EQ(loaded->FogGlowFadeStart, fog.FogGlowFadeStart);
    EXPECT_FLOAT_EQ(loaded->FogGlowFadeEnd, fog.FogGlowFadeEnd);
    EXPECT_FLOAT_EQ(loaded->FogGlowTint[0], fog.FogGlowTint[0]);
    EXPECT_FLOAT_EQ(loaded->FogGlowTint[1], fog.FogGlowTint[1]);
    EXPECT_FLOAT_EQ(loaded->FogGlowTint[2], fog.FogGlowTint[2]);
    EXPECT_EQ(loaded->FogGlowAntiFlicker, fog.FogGlowAntiFlicker);
}

TEST(SceneIO, SaveThenLoad_RoundTripVolumetricFogEffectCamelCaseProperties)
{
    const auto scenePath = MakeTempPath("volumetric_fog.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'v', 'f', 'o', 'g', '\0'}});

    Components::VolumetricFogEffect fog{};
    fog.Enabled = false;
    fog.Intensity = 0.7f;
    fog.MaxDistance = 512.0f;
    fog.XYCellSizePixels = 12;
    fog.ZSliceCount = 96;
    fog.DepthDistribution = 1.9f;
    fog.Density = 0.05f;
    fog.BaseHeight = 3.0f;
    fog.HeightFalloff = 40.0f;
    fog.SkyFade = 0.65f;
    fog.Albedo[0] = 0.6f;
    fog.Albedo[1] = 0.5f;
    fog.Albedo[2] = 0.4f;
    fog.Emission[0] = 0.1f;
    fog.Emission[1] = 0.2f;
    fog.Emission[2] = 0.3f;
    fog.Anisotropy = 0.4f;
    fog.TrackDirectionalLight = false;
    fog.SunIntensityScale = 1.5f;
    fog.SunScatteringTint[0] = 0.9f;
    fog.SunScatteringTint[1] = 0.6f;
    fog.SunScatteringTint[2] = 0.3f;
    fog.AmbientScatteringTint[0] = 0.2f;
    fog.AmbientScatteringTint[1] = 0.3f;
    fog.AmbientScatteringTint[2] = 0.5f;
    fog.NoiseEnabled = false;
    fog.NoiseScale = 55.0f;
    fog.NoiseStrength = 0.4f;
    fog.NoiseVelocity[0] = 0.01f;
    fog.NoiseVelocity[1] = 0.02f;
    fog.NoiseVelocity[2] = 0.03f;
    fog.NoiseContrast = 1.6f;
    fog.NoiseChannelWeights[0] = 0.5f;
    fog.NoiseChannelWeights[1] = 0.3f;
    fog.NoiseChannelWeights[2] = 0.15f;
    fog.NoiseChannelWeights[3] = 0.05f;
    fog.DensityThreshold = 0.1f;
    fog.DensityThresholdSoftness = 0.25f;
    fog.DensityMode = Components::VolumetricFogDensityMode::Subtractive;
    fog.GradientMode = Components::VolumetricFogGradientMode::ScreenY;
    fog.GradientStrength = 0.5f;
    fog.GradientLowTint[0] = 0.2f;
    fog.GradientLowTint[1] = 0.3f;
    fog.GradientLowTint[2] = 0.4f;
    fog.GradientHighTint[0] = 0.7f;
    fog.GradientHighTint[1] = 0.8f;
    fog.GradientHighTint[2] = 0.9f;
    fog.TemporalEnabled = false;
    fog.TemporalBlend = 0.85f;
    fog.JitterStrength = 0.6f;
    fog.JitterMotion = true;
    fog.CompositeDepthBias = 0.02f;
    fog.ShadowBias = 0.01f;
    fog.FogGlowEnabled = true;
    fog.FogGlowQualityLevel = Components::FogGlowQuality::Cinematic;
    fog.FogGlowIntensity = 0.42f;
    fog.FogGlowRadius = 4.5f;
    fog.FogGlowOctaves = 7;
    fog.FogGlowScatter = 0.55f;
    fog.FogGlowThreshold = 1.2f;
    fog.FogGlowKnee = 0.35f;
    fog.FogGlowFadeStart = 0.08f;
    fog.FogGlowFadeEnd = 0.66f;
    fog.FogGlowTint[0] = 1.1f;
    fog.FogGlowTint[1] = 0.8f;
    fog.FogGlowTint[2] = 0.6f;
    fog.FogGlowAntiFlicker = false;
    w1.AddComponentImmediate(e, fog);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("VolumetricFogEffect.densityMode = 1"), std::string::npos);
    EXPECT_NE(saved.find("VolumetricFogEffect.gradientMode = 4"), std::string::npos);
    EXPECT_NE(saved.find("VolumetricFogEffect.fogGlowEnabled = true"), std::string::npos);
    EXPECT_NE(saved.find("VolumetricFogEffect.fogGlowQuality = 3"), std::string::npos);
    EXPECT_NE(saved.find("VolumetricFogEffect.fogGlowOctaves = 7"), std::string::npos);
    EXPECT_NE(saved.find("VolumetricFogEffect.fogGlowRadius = 4.5"), std::string::npos);
    EXPECT_NE(saved.find("VolumetricFogEffect.fogGlowAntiFlicker = false"), std::string::npos);

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto h = FindByTag(w2, "vfog");
    ASSERT_TRUE(h.IsValid());
    const auto* loaded = w2.GetComponent<Components::VolumetricFogEffect>(h);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->Enabled, fog.Enabled);
    EXPECT_FLOAT_EQ(loaded->Intensity, fog.Intensity);
    EXPECT_FLOAT_EQ(loaded->MaxDistance, fog.MaxDistance);
    EXPECT_EQ(loaded->XYCellSizePixels, fog.XYCellSizePixels);
    EXPECT_EQ(loaded->ZSliceCount, fog.ZSliceCount);
    EXPECT_FLOAT_EQ(loaded->DepthDistribution, fog.DepthDistribution);
    EXPECT_FLOAT_EQ(loaded->Density, fog.Density);
    EXPECT_FLOAT_EQ(loaded->BaseHeight, fog.BaseHeight);
    EXPECT_FLOAT_EQ(loaded->HeightFalloff, fog.HeightFalloff);
    EXPECT_FLOAT_EQ(loaded->SkyFade, fog.SkyFade);
    EXPECT_FLOAT_EQ(loaded->Albedo[0], fog.Albedo[0]);
    EXPECT_FLOAT_EQ(loaded->Albedo[1], fog.Albedo[1]);
    EXPECT_FLOAT_EQ(loaded->Albedo[2], fog.Albedo[2]);
    EXPECT_FLOAT_EQ(loaded->Emission[0], fog.Emission[0]);
    EXPECT_FLOAT_EQ(loaded->Emission[1], fog.Emission[1]);
    EXPECT_FLOAT_EQ(loaded->Emission[2], fog.Emission[2]);
    EXPECT_FLOAT_EQ(loaded->Anisotropy, fog.Anisotropy);
    EXPECT_EQ(loaded->TrackDirectionalLight, fog.TrackDirectionalLight);
    EXPECT_FLOAT_EQ(loaded->SunIntensityScale, fog.SunIntensityScale);
    EXPECT_FLOAT_EQ(loaded->SunScatteringTint[0], fog.SunScatteringTint[0]);
    EXPECT_FLOAT_EQ(loaded->SunScatteringTint[1], fog.SunScatteringTint[1]);
    EXPECT_FLOAT_EQ(loaded->SunScatteringTint[2], fog.SunScatteringTint[2]);
    EXPECT_FLOAT_EQ(loaded->AmbientScatteringTint[0], fog.AmbientScatteringTint[0]);
    EXPECT_FLOAT_EQ(loaded->AmbientScatteringTint[1], fog.AmbientScatteringTint[1]);
    EXPECT_FLOAT_EQ(loaded->AmbientScatteringTint[2], fog.AmbientScatteringTint[2]);
    EXPECT_EQ(loaded->NoiseEnabled, fog.NoiseEnabled);
    EXPECT_FLOAT_EQ(loaded->NoiseScale, fog.NoiseScale);
    EXPECT_FLOAT_EQ(loaded->NoiseStrength, fog.NoiseStrength);
    EXPECT_FLOAT_EQ(loaded->NoiseVelocity[0], fog.NoiseVelocity[0]);
    EXPECT_FLOAT_EQ(loaded->NoiseVelocity[1], fog.NoiseVelocity[1]);
    EXPECT_FLOAT_EQ(loaded->NoiseVelocity[2], fog.NoiseVelocity[2]);
    EXPECT_FLOAT_EQ(loaded->NoiseContrast, fog.NoiseContrast);
    EXPECT_FLOAT_EQ(loaded->NoiseChannelWeights[0], fog.NoiseChannelWeights[0]);
    EXPECT_FLOAT_EQ(loaded->NoiseChannelWeights[1], fog.NoiseChannelWeights[1]);
    EXPECT_FLOAT_EQ(loaded->NoiseChannelWeights[2], fog.NoiseChannelWeights[2]);
    EXPECT_FLOAT_EQ(loaded->NoiseChannelWeights[3], fog.NoiseChannelWeights[3]);
    EXPECT_FLOAT_EQ(loaded->DensityThreshold, fog.DensityThreshold);
    EXPECT_FLOAT_EQ(loaded->DensityThresholdSoftness, fog.DensityThresholdSoftness);
    EXPECT_EQ(loaded->DensityMode, fog.DensityMode);
    EXPECT_EQ(loaded->GradientMode, fog.GradientMode);
    EXPECT_FLOAT_EQ(loaded->GradientStrength, fog.GradientStrength);
    EXPECT_FLOAT_EQ(loaded->GradientLowTint[0], fog.GradientLowTint[0]);
    EXPECT_FLOAT_EQ(loaded->GradientLowTint[1], fog.GradientLowTint[1]);
    EXPECT_FLOAT_EQ(loaded->GradientLowTint[2], fog.GradientLowTint[2]);
    EXPECT_FLOAT_EQ(loaded->GradientHighTint[0], fog.GradientHighTint[0]);
    EXPECT_FLOAT_EQ(loaded->GradientHighTint[1], fog.GradientHighTint[1]);
    EXPECT_FLOAT_EQ(loaded->GradientHighTint[2], fog.GradientHighTint[2]);
    EXPECT_EQ(loaded->TemporalEnabled, fog.TemporalEnabled);
    EXPECT_FLOAT_EQ(loaded->TemporalBlend, fog.TemporalBlend);
    EXPECT_FLOAT_EQ(loaded->JitterStrength, fog.JitterStrength);
    EXPECT_EQ(loaded->JitterMotion, fog.JitterMotion);
    EXPECT_FLOAT_EQ(loaded->CompositeDepthBias, fog.CompositeDepthBias);
    EXPECT_FLOAT_EQ(loaded->ShadowBias, fog.ShadowBias);
    // FogGlow* fields — the untested-before-this-change surface (gap 1).
    EXPECT_EQ(loaded->FogGlowEnabled, fog.FogGlowEnabled);
    EXPECT_EQ(loaded->FogGlowQualityLevel, fog.FogGlowQualityLevel);
    EXPECT_FLOAT_EQ(loaded->FogGlowIntensity, fog.FogGlowIntensity);
    EXPECT_FLOAT_EQ(loaded->FogGlowRadius, fog.FogGlowRadius);
    EXPECT_EQ(loaded->FogGlowOctaves, fog.FogGlowOctaves);
    EXPECT_FLOAT_EQ(loaded->FogGlowScatter, fog.FogGlowScatter);
    EXPECT_FLOAT_EQ(loaded->FogGlowThreshold, fog.FogGlowThreshold);
    EXPECT_FLOAT_EQ(loaded->FogGlowKnee, fog.FogGlowKnee);
    EXPECT_FLOAT_EQ(loaded->FogGlowFadeStart, fog.FogGlowFadeStart);
    EXPECT_FLOAT_EQ(loaded->FogGlowFadeEnd, fog.FogGlowFadeEnd);
    EXPECT_FLOAT_EQ(loaded->FogGlowTint[0], fog.FogGlowTint[0]);
    EXPECT_FLOAT_EQ(loaded->FogGlowTint[1], fog.FogGlowTint[1]);
    EXPECT_FLOAT_EQ(loaded->FogGlowTint[2], fog.FogGlowTint[2]);
    EXPECT_EQ(loaded->FogGlowAntiFlicker, fog.FogGlowAntiFlicker);
}

TEST(SceneIO, SaveThenLoad_RoundTripFilmSimulationEffectGrainAndGateModes)
{
    const auto scenePath = MakeTempPath("film_simulation.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'f', 'i', 'l', 'm', '\0'}});

    Components::FilmSimulationEffect film{};
    film.Enabled = false;
    film.FilmFrameRate = 30.0f;
    film.HalationEnabled = false;
    film.HalationIntensity = 1.5f;
    film.HalationRadius = 5.0f;
    film.HalationTint[0] = 0.9f;
    film.HalationTint[1] = 0.2f;
    film.HalationTint[2] = 0.05f;
    film.GrainEnabled = false;
    // Non-default grain mode (default is Filmic) is the primary coverage gap (gap 2):
    // FidelityFXFast round-trips only if serialize emits and deserialize applies it.
    film.GrainMode = Components::FilmGrainMode::FidelityFXFast;
    film.GrainIntensity = 0.6f;
    film.GrainSize = 4.5f;
    film.GrainSmooth = false;
    film.GrainDensity = 0.8f;
    film.GrainShadowResponse = 0.7f;
    film.GrainMidtoneResponse = 0.9f;
    film.GrainHighlightResponse = 0.5f;
    film.GrainColored = false;
    film.HairEnabled = false;
    film.HairAmount = 0.5f;
    film.HairIntensity = 1.0f;
    film.HairWidth = 2.0f;
    film.HairLength = 300.0f;
    film.HairRandomSize = 0.5f;
    film.HairCurl = 0.4f;
    film.HairCurlRandomness = 0.6f;
    film.ScratchesEnabled = false;
    film.ScratchAmount = 0.4f;
    film.ScratchIntensity = 0.5f;
    film.ScratchWidth = 1.0f;
    film.ScratchLength = 0.5f;
    film.DustEnabled = false;
    film.DustAmount = 3.0f;
    film.DustIntensity = 0.7f;
    film.DustSize = 5.0f;
    film.DustRandomSize = 0.5f;
    film.GateWeaveEnabled = false;
    film.GateWeaveHorizontal = 0.5f;
    film.GateWeaveVertical = 0.3f;
    film.GateWeaveRotation = 0.12f;
    // Non-default gate mask (default is RoundedGate) — the other enum with no
    // prior round-trip coverage.
    film.GateMask = Components::FilmGateMask::Anamorphic239;
    film.GateMaskFeather = 6.0f;
    film.GateMaskRoundness = 0.05f;
    w1.AddComponentImmediate(e, film);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("FilmSimulationEffect.grainMode = 0"), std::string::npos);
    EXPECT_NE(saved.find("FilmSimulationEffect.gateMask = 4"), std::string::npos);
    EXPECT_NE(saved.find("FilmSimulationEffect.enabled = false"), std::string::npos);
    EXPECT_NE(saved.find("FilmSimulationEffect.grainSize = 4.5"), std::string::npos);

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto h = FindByTag(w2, "film");
    ASSERT_TRUE(h.IsValid());
    const auto* loaded = w2.GetComponent<Components::FilmSimulationEffect>(h);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->Enabled, film.Enabled);
    EXPECT_FLOAT_EQ(loaded->FilmFrameRate, film.FilmFrameRate);
    EXPECT_EQ(loaded->HalationEnabled, film.HalationEnabled);
    EXPECT_FLOAT_EQ(loaded->HalationIntensity, film.HalationIntensity);
    EXPECT_FLOAT_EQ(loaded->HalationRadius, film.HalationRadius);
    EXPECT_FLOAT_EQ(loaded->HalationTint[0], film.HalationTint[0]);
    EXPECT_FLOAT_EQ(loaded->HalationTint[1], film.HalationTint[1]);
    EXPECT_FLOAT_EQ(loaded->HalationTint[2], film.HalationTint[2]);
    EXPECT_EQ(loaded->GrainEnabled, film.GrainEnabled);
    EXPECT_EQ(loaded->GrainMode, film.GrainMode);
    EXPECT_FLOAT_EQ(loaded->GrainIntensity, film.GrainIntensity);
    EXPECT_FLOAT_EQ(loaded->GrainSize, film.GrainSize);
    EXPECT_EQ(loaded->GrainSmooth, film.GrainSmooth);
    EXPECT_FLOAT_EQ(loaded->GrainDensity, film.GrainDensity);
    EXPECT_FLOAT_EQ(loaded->GrainShadowResponse, film.GrainShadowResponse);
    EXPECT_FLOAT_EQ(loaded->GrainMidtoneResponse, film.GrainMidtoneResponse);
    EXPECT_FLOAT_EQ(loaded->GrainHighlightResponse, film.GrainHighlightResponse);
    EXPECT_EQ(loaded->GrainColored, film.GrainColored);
    EXPECT_EQ(loaded->HairEnabled, film.HairEnabled);
    EXPECT_FLOAT_EQ(loaded->HairAmount, film.HairAmount);
    EXPECT_FLOAT_EQ(loaded->HairIntensity, film.HairIntensity);
    EXPECT_FLOAT_EQ(loaded->HairWidth, film.HairWidth);
    EXPECT_FLOAT_EQ(loaded->HairLength, film.HairLength);
    EXPECT_FLOAT_EQ(loaded->HairRandomSize, film.HairRandomSize);
    EXPECT_FLOAT_EQ(loaded->HairCurl, film.HairCurl);
    EXPECT_FLOAT_EQ(loaded->HairCurlRandomness, film.HairCurlRandomness);
    EXPECT_EQ(loaded->ScratchesEnabled, film.ScratchesEnabled);
    EXPECT_FLOAT_EQ(loaded->ScratchAmount, film.ScratchAmount);
    EXPECT_FLOAT_EQ(loaded->ScratchIntensity, film.ScratchIntensity);
    EXPECT_FLOAT_EQ(loaded->ScratchWidth, film.ScratchWidth);
    EXPECT_FLOAT_EQ(loaded->ScratchLength, film.ScratchLength);
    EXPECT_EQ(loaded->DustEnabled, film.DustEnabled);
    EXPECT_FLOAT_EQ(loaded->DustAmount, film.DustAmount);
    EXPECT_FLOAT_EQ(loaded->DustIntensity, film.DustIntensity);
    EXPECT_FLOAT_EQ(loaded->DustSize, film.DustSize);
    EXPECT_FLOAT_EQ(loaded->DustRandomSize, film.DustRandomSize);
    EXPECT_EQ(loaded->GateWeaveEnabled, film.GateWeaveEnabled);
    EXPECT_FLOAT_EQ(loaded->GateWeaveHorizontal, film.GateWeaveHorizontal);
    EXPECT_FLOAT_EQ(loaded->GateWeaveVertical, film.GateWeaveVertical);
    EXPECT_FLOAT_EQ(loaded->GateWeaveRotation, film.GateWeaveRotation);
    EXPECT_EQ(loaded->GateMask, film.GateMask);
    EXPECT_FLOAT_EQ(loaded->GateMaskFeather, film.GateMaskFeather);
    EXPECT_FLOAT_EQ(loaded->GateMaskRoundness, film.GateMaskRoundness);
}

TEST(HeightFogPresets, ApplyPresetDoesNotCarryPreviousPresetState)
{
    Components::HeightFogEffect fog{};
    Components::ApplyHeightFogPreset(fog, Components::HeightFogPreset::GroundMist);
    ASSERT_TRUE(fog.NoiseEnabled);
    ASSERT_NE(fog.NoiseVelocity[0], 0.0f);

    fog.AxisMode = Components::HeightFogAxisMode::Custom;
    fog.CustomAxis[0] = 1.0f;
    fog.CustomAxis[1] = 0.0f;
    fog.CustomAxis[2] = 0.0f;
    fog.PhaseWeight1 = 5.0f;

    Components::ApplyHeightFogPreset(fog, Components::HeightFogPreset::MountainValley);

    EXPECT_EQ(fog.Preset, Components::HeightFogPreset::MountainValley);
    EXPECT_EQ(fog.AxisMode, Components::HeightFogAxisMode::WorldY);
    EXPECT_FALSE(fog.NoiseEnabled);
    EXPECT_EQ(fog.LayerMode, Components::HeightFogLayerMode::Dominant);
    EXPECT_FLOAT_EQ(fog.NoiseFadeEnd, 0.0f);
    EXPECT_FLOAT_EQ(fog.NoiseVelocity[0], 0.0f);
    EXPECT_FLOAT_EQ(fog.NoiseVelocity[1], 0.0f);
    EXPECT_FLOAT_EQ(fog.NoiseVelocity[2], 0.0f);
    EXPECT_FLOAT_EQ(fog.PhaseWeight1, 0.0f);
    EXPECT_FLOAT_EQ(fog.SkyPower, 1.45f);
}


// An old scene's upgrade is loud once per component, not quiet or once per field: a component block
// whose type no module registers warns, and the keys a registered component no longer has warn in one
// line for the block.
TEST(SceneIO, Load_AnUnregisteredComponentAndDroppedFieldsWarnOncePerComponent)
{
    const auto scenePath = MakeTempPath("upgrade_path_warnings.scene");
    const std::string src =
        "[scene name=\"Upgrade\" version=1]\n"
        "\n"
        "[entity id=\"old_emitter\"]\n"
        "ParticleEmitter3D.Amount = 1000\n"
        "ParticleEmitter3D.fixedFps = 60\n"
        "ParticleEmitter3D.lifetime = 2\n"
        "ParticleEmitter3D.transformAlign = 1\n"
        "RetiredEffectSettings.amount = 1000\n"
        "RetiredEffectSettings.gravity = (0, -1, 0)\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines, Logger::LogLevel::Warning);
    Logger::Log::Warning("SceneIO warning capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(CountLinesContaining(logLines, "SceneIO warning capture self test"), 1u)
        << "the capture is not receiving engine warnings; every count below would be a false zero";
    logLines.clear();

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();

    std::string dump;
    for (const auto& line : logLines)
        dump += "\n  | " + line;
    EXPECT_EQ(CountLinesContaining(logLines, "no registered type is named 'RetiredEffectSettings'"), 1u)
        << "an unregistered component warns once" << dump;
    EXPECT_EQ(CountLinesContaining(logLines, "has no reflected field"), 1u) << "one line per component" << dump;
    const std::string dropped = FirstLineContaining(logLines, "has no reflected field");
    for (const char* key : {"'fixedfps'", "'lifetime'", "'transformalign'"})
        EXPECT_NE(dropped.find(key), std::string::npos) << key << " missing from: " << dropped;

    const auto entity = FindByTag(w, "old_emitter");
    ASSERT_TRUE(entity.IsValid());
    const auto* emitter = w.GetComponent<Components::ParticleEmitter3D>(entity);
    ASSERT_NE(emitter, nullptr);
    EXPECT_EQ(emitter->Amount, 1000u) << "the fields the component still has are applied";
}

TEST(SceneIO, SaveThenLoad_RoundTripParticleComponents)
{
    const auto scenePath = MakeTempPath("reference_particles_roundtrip.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'g', 'p', 'u', 'p', '\0'}});
    const ECS::EntityHandle child = w1.CreateEntity();
    w1.AddComponentImmediate(child, Components::SceneEntityTag{{'k', 'i', 'd', '\0'}});
    w1.AddComponentImmediate(child, Components::ParticleEmitter3D{});

    Components::ParticleEmitter3D emitter;
    emitter.Emitting = false;
    emitter.Amount = 64u;
    emitter.PrewarmSeconds = 0.25f;
    emitter.SimulationSpeed = 1.75f;
    emitter.Dimension = Components::ParticleEmitterDimension::World2D;
    emitter.LocalSpace = true;
    emitter.TicksPerSecond = 60u;
    emitter.Interpolate = false;
    emitter.Seed = 99u;
    const GUID stack = GUID::Derive(GUID::Null(), "particle/stack");
    emitter.Stack.Set(stack);
    emitter.SubEmitters[2].Emitter = child;
    w1.AddComponentImmediate(e, emitter);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("ParticleEmitter3D.Amount = 64"), std::string::npos) << saved;
    EXPECT_NE(saved.find("ParticleEmitter3D.Dimension = World2D"), std::string::npos) << saved;
    EXPECT_NE(saved.find("ParticleEmitter3D.Stack = "), std::string::npos) << saved;
    EXPECT_NE(saved.find("ParticleEmitter3D.SubEmitters2.Emitter = "), std::string::npos) << saved;

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;

    const auto loadedEntity = FindByTag(w2, "gpup");
    const auto loadedChild = FindByTag(w2, "kid");
    ASSERT_TRUE(loadedEntity.IsValid());
    ASSERT_TRUE(loadedChild.IsValid());
    const auto* loadedEmitter = w2.GetComponent<Components::ParticleEmitter3D>(loadedEntity);
    ASSERT_NE(loadedEmitter, nullptr);

    EXPECT_EQ(loadedEmitter->Stack.ToGuid(), stack);
    EXPECT_EQ(loadedEmitter->Emitting, emitter.Emitting);
    EXPECT_EQ(loadedEmitter->Amount, emitter.Amount);
    EXPECT_FLOAT_EQ(loadedEmitter->PrewarmSeconds, emitter.PrewarmSeconds);
    EXPECT_FLOAT_EQ(loadedEmitter->SimulationSpeed, emitter.SimulationSpeed);
    EXPECT_EQ(loadedEmitter->Dimension, emitter.Dimension);
    EXPECT_EQ(loadedEmitter->LocalSpace, emitter.LocalSpace);
    EXPECT_EQ(loadedEmitter->TicksPerSecond, emitter.TicksPerSecond);
    EXPECT_EQ(loadedEmitter->Interpolate, emitter.Interpolate);
    EXPECT_EQ(loadedEmitter->Seed, emitter.Seed);
    EXPECT_EQ(loadedEmitter->SubEmitters[2].Emitter, loadedChild);
    for (const uint32 slot : {0u, 1u, 3u})
        EXPECT_FALSE(loadedEmitter->SubEmitters[slot].Emitter.IsValid()) << slot;
}

TEST(SceneIO, Save_EntitySectionsFollowHierarchyDepthFirstNotLexicalTagOrder)
{
    const auto scenePath = MakeTempPath("hierarchy_order.scene");

    ECS::World w;
    const ECS::EntityHandle parent = w.CreateEntity();
    const ECS::EntityHandle child = w.CreateEntity();
    ASSERT_TRUE(parent.IsValid() && child.IsValid());

    w.AddComponentImmediate(parent, Components::SceneEntityTag{{'z', 'z', 'z', '\0'}});
    w.AddComponentImmediate(child, Components::SceneEntityTag{{'a', 'a', 'a', '\0'}});

    Components::HierarchyOrder ho{};
    ho.order = 0;
    w.AddComponentImmediate(parent, ho);

    Components::HierarchyOrder hc{};
    hc.order = 0;
    w.AddComponentImmediate(child, hc);

    Components::Parent p{};
    p.parent = parent;
    w.AddComponentImmediate(child, p);

    ASSERT_TRUE(Scene::SaveSceneToFile(w, scenePath, Scene::SaveOptions{}));

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    // Match prefix only — child entity blocks include `parent="..."` after the id attr.
    const size_t posParent = saved.find("[entity id=\"zzz\"");
    const size_t posChild = saved.find("[entity id=\"aaa\"");
    ASSERT_NE(posParent, std::string::npos);
    ASSERT_NE(posChild, std::string::npos);
    EXPECT_LT(posParent, posChild) << ".scene entity sections should depth-first traverse (parent before child), "
                                      "not sort lexically by SceneEntityTag (which would put aaa before zzz)";
}

TEST(SceneIO, Save_PreservesLeadingCommentBlock)
{
    const auto scenePath = MakeTempPath("preserve_comment.scene");
    const std::string initial =
        "; Top comment\n"
        "; Another comment\n"
        "\n"
        "[scene name=\"Old\" version=1]\n"
        "\n"
        "[entity id=\"root\"]\n"
        "Name.value = \"Root\"\n";
    ASSERT_TRUE(WriteFile(scenePath, initial));

    ECS::World w;
    const ECS::EntityHandle e = w.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w.AddComponentImmediate(e, Components::SceneEntityTag{{'r', 'o', 'o', 't', '\0'}});
    Components::Name nm{};
    std::memset(nm.value, 0, sizeof(nm.value));
    std::memcpy(nm.value, "Root", 4);
    w.AddComponentImmediate(e, nm);

    ASSERT_TRUE(Scene::SaveSceneToFile(w, scenePath, Scene::SaveOptions{}));

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_TRUE(saved.rfind("; Top comment", 0) == 0);
}

TEST(SceneIO, Validate_DuplicateEntityIdFailsLoad)
{
    const auto scenePath = MakeTempPath("dupe.scene");
    const std::string src =
        "[scene name=\"Dupe\" version=1]\n"
        "\n"
        "[entity id=\"a\"]\n"
        "[entity id=\"a\"]\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
}

TEST(SceneIO, Validate_CycleFailsLoad)
{
    const auto scenePath = MakeTempPath("cycle.scene");
    const std::string src =
        "[scene name=\"Cycle\" version=1]\n"
        "\n"
        "[entity id=\"a\" parent=\"b\"]\n"
        "[entity id=\"b\" parent=\"a\"]\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
}

TEST(SceneIO, BlueprintInstance_InstantiatesWithPrefixAndOverrides)
{
    const auto dir = std::filesystem::temp_directory_path() / "GameEngine_SceneIOTests";
    const auto bpPath = (dir / "bp.blueprint").lexically_normal();
    const auto scenePath = (dir / "bp_scene.scene").lexically_normal();

    const std::string bp =
        "[blueprint name=\"BP\" version=1]\n"
        "\n"
        "[entity id=\"root\"]\n"
        "Name.value = \"BP\"\n"
        "\n"
        "[entity id=\"child\" parent=\"root\"]\n"
        "Name.value = \"Child\"\n";
    ASSERT_TRUE(WriteFile(bpPath, bp));

    const std::string scene =
        "[scene name=\"S\" version=1]\n"
        "[resource id=\"bp\" path=\"bp.blueprint\"]\n"
        "\n"
        "[blueprint id=\"inst\" source=\"bp\"]\n"
        "Transform.position = (5, 0, 0)\n";
    ASSERT_TRUE(WriteFile(scenePath, scene));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    auto inst = FindByTag(world, "inst");
    auto instChild = FindByTag(world, "inst.child");
    ASSERT_TRUE(inst.IsValid());
    ASSERT_TRUE(instChild.IsValid());

    auto* p = world.GetComponent<Components::Parent>(instChild);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->parent.id, inst.id);
}

TEST(SceneIO, Save_PreservesBlueprintInstanceAndRemovalDirectives)
{
    const auto dir = std::filesystem::temp_directory_path() / "GameEngine_SceneIOTests";
    const auto bpPath = (dir / "bp_remove.blueprint").lexically_normal();
    const auto scenePath = (dir / "bp_remove_scene.scene").lexically_normal();
    const auto outPath = (dir / "bp_remove_scene_out.scene").lexically_normal();

    const std::string bp =
        "[blueprint name=\"BP\" version=1]\n"
        "\n"
        "[entity id=\"root\"]\n"
        "Name.value = \"BP\"\n"
        "DebugMarker.value = 1\n"
        "\n"
        "[entity id=\"child\" parent=\"root\"]\n"
        "Name.value = \"Child\"\n";
    ASSERT_TRUE(WriteFile(bpPath, bp));

    const std::string scene =
        "[scene name=\"S\" version=1]\n"
        "[resource id=\"bp\" path=\"bp_remove.blueprint\"]\n"
        "\n"
        "[blueprint id=\"inst\" source=\"bp\"]\n"
        "-DebugMarker\n";
    ASSERT_TRUE(WriteFile(scenePath, scene));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    // Save back out and ensure we didn't flatten the blueprint's child entities and we kept removals.
    Scene::SaveOptions so{};
    so.preserveBlueprintInstances = true;
    ASSERT_TRUE(Scene::SaveSceneToFile(world, outPath, so));

    std::string saved;
    ASSERT_TRUE(ReadFile(outPath, saved));
    EXPECT_NE(saved.find("[blueprint id=\"inst\""), std::string::npos);
    EXPECT_NE(saved.find("-DebugMarker"), std::string::npos);
    EXPECT_EQ(saved.find("[entity id=\"inst.child\""), std::string::npos);
}

TEST(SceneIO, Save_PreservesSubsceneInstanceAndDoesNotFlatten)
{
    const auto dir = std::filesystem::temp_directory_path() / "GameEngine_SceneIOTests";
    const auto chunkPath = (dir / "chunk_a.scene").lexically_normal();
    const auto scenePath = (dir / "subscene_main.scene").lexically_normal();
    const auto outPath = (dir / "subscene_main_out.scene").lexically_normal();

    const std::string chunk =
        "[scene name=\"Chunk\" version=1]\n"
        "\n"
        "[entity id=\"root\"]\n"
        "Name.value = \"ChunkRoot\"\n";
    ASSERT_TRUE(WriteFile(chunkPath, chunk));

    const std::string scene =
        "[scene name=\"Main\" version=1]\n"
        "[resource id=\"chunk\" path=\"chunk_a.scene\"]\n"
        "\n"
        "[subscene id=\"area\" source=\"chunk\"]\n"
        "offset = (100, 0, 0)\n"
        "enabled = true\n";
    ASSERT_TRUE(WriteFile(scenePath, scene));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    Scene::SaveOptions so{};
    so.preserveSubscenes = true;
    ASSERT_TRUE(Scene::SaveSceneToFile(world, outPath, so));

    std::string saved;
    ASSERT_TRUE(ReadFile(outPath, saved));
    EXPECT_NE(saved.find("[subscene id=\"area\""), std::string::npos);
    EXPECT_NE(saved.find("offset = (100, 0, 0)"), std::string::npos);
    EXPECT_EQ(saved.find("[entity id=\"area.root\""), std::string::npos);
}

TEST(SceneIO, Save_PreservesCommentBlocksBeforeSections)
{
    const auto dir = std::filesystem::temp_directory_path() / "GameEngine_SceneIOTests";
    const auto bpPath = (dir / "bp_comment.blueprint").lexically_normal();
    const auto chunkPath = (dir / "chunk_comment.scene").lexically_normal();
    const auto scenePath = (dir / "comment_main.scene").lexically_normal();
    const auto outPath = (dir / "comment_main.scene").lexically_normal(); // overwrite to preserve comments

    const std::string bp =
        "[blueprint name=\"BP\" version=1]\n"
        "\n"
        "[entity id=\"root\"]\n"
        "Name.value = \"BP\"\n";
    ASSERT_TRUE(WriteFile(bpPath, bp));

    const std::string chunk =
        "[scene name=\"Chunk\" version=1]\n"
        "\n"
        "[entity id=\"root\"]\n"
        "Name.value = \"Chunk\"\n";
    ASSERT_TRUE(WriteFile(chunkPath, chunk));

    const std::string scene =
        "; Header comment\n"
        "[scene name=\"Main\" version=1]\n"
        "[resource id=\"bp\" path=\"bp_comment.blueprint\"]\n"
        "[resource id=\"chunk\" path=\"chunk_comment.scene\"]\n"
        "\n"
        "; Comment before blueprint\n"
        "[blueprint id=\"inst\" source=\"bp\"]\n"
        "\n"
        "; Comment before subscene\n"
        "[subscene id=\"area\" source=\"chunk\"]\n"
        "enabled = true\n";
    ASSERT_TRUE(WriteFile(scenePath, scene));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    Scene::SaveOptions so{};
    so.preserveBlueprintInstances = true;
    so.preserveSubscenes = true;
    ASSERT_TRUE(Scene::SaveSceneToFile(world, outPath, so));

    std::string saved;
    ASSERT_TRUE(ReadFile(outPath, saved));

    const size_t c1 = saved.find("; Comment before blueprint");
    const size_t h1 = saved.find("[blueprint id=\"inst\"");
    ASSERT_NE(c1, std::string::npos);
    ASSERT_NE(h1, std::string::npos);
    EXPECT_LT(c1, h1);

    const size_t c2 = saved.find("; Comment before subscene");
    const size_t h2 = saved.find("[subscene id=\"area\"");
    ASSERT_NE(c2, std::string::npos);
    ASSERT_NE(h2, std::string::npos);
    EXPECT_LT(c2, h2);
}

TEST(SceneIO, Save_PreservesEmbedBlocksOnOverwrite)
{
    const auto dir = std::filesystem::temp_directory_path() / "GameEngine_SceneIOTests";
    const auto scenePath = (dir / "embed_preserve.scene").lexically_normal();

    const std::string scene =
        "[scene name=\"Main\" version=1]\n"
        "\n"
        "[embed id=\"mat\" type=\"Material\"]\n"
        "albedo = (1, 0, 0)\n"
        "\n"
        "[entity id=\"e\"]\n"
        "Name.value = \"E\"\n";
    ASSERT_TRUE(WriteFile(scenePath, scene));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    // Overwrite same file so comment/embed preservation can kick in.
    ASSERT_TRUE(Scene::SaveSceneToFile(world, scenePath, Scene::SaveOptions{}));

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("[embed id=\"mat\" type=\"Material\"]"), std::string::npos);
    EXPECT_NE(saved.find("albedo = (1, 0, 0)"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Embed identity persistence.
//
// An [embed] block's identity used to be derived from the containing scene's
// GUID at every load, so renaming a scene with N embeds changed N+1 identities.
// The identity is now written into the block; derivation survives only as the
// fallback for files authored before the attribute existed.
// ---------------------------------------------------------------------------

// The frozen legacy derivation, spelled out independently of SceneIO so the
// fallback tests would fail if SceneIO's formula ever drifted.
static GUID LegacyEmbedGuid(const GUID& sceneGuid, const std::string& embedId)
{
    return GUID::Derive(sceneGuid, "embed:" + embedId);
}

// A scene whose only entity references embed "mat". `embedHeaderExtra` is
// appended inside the [embed] header (e.g. R"( guid="...")").
static std::string MakeEmbedIdentityScene(const std::string& embedHeaderExtra)
{
    return "[scene name=\"EmbedIdentity\" version=1]\n"
           "\n"
           "[embed id=\"mat\" type=\"Material\"" +
           embedHeaderExtra +
           "]\n"
           "albedo = (1, 0, 0)\n"
           "\n"
           "[entity id=\"e\"]\n"
           "MatUser.material = #mat\n";
}

// Loads the scene and returns the GUID string the embed reference resolved to.
static std::string ResolvedEmbedGuidString(const std::filesystem::path& scenePath,
                                           Scene::ISceneAssetResolver* resolver)
{
    ECS::World world;
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    opts.assetResolver = resolver;
    if (!Scene::LoadSceneFromFile(world, scenePath, opts))
        return {};
    const auto e = FindByTag(world, "e");
    if (!e.IsValid())
        return {};
    const auto* m = world.GetComponent<MatUser>(e);
    return m ? std::string(m->guid) : std::string{};
}

TEST(SceneIO, EmbedIdentity_PersistedGuidWinsOverDerivation)
{
    const auto scenePath = MakeTempPath("embed_identity_persisted.scene");
    const GUID persisted("0123456f-89ab-cdef-0123-456789abcdef");
    ASSERT_FALSE(persisted.IsNull());
    ASSERT_TRUE(WriteFile(scenePath, MakeEmbedIdentityScene(" guid=\"" + persisted.ToString() + "\"")));

    FakeAssetResolver resolver;
    resolver.SetRoot(scenePath.parent_path());
    const GUID sceneGuid = resolver.GetOrCreateAssetGuid(scenePath);

    EXPECT_EQ(ResolvedEmbedGuidString(scenePath, &resolver), persisted.ToString());
    // Guard against a false pass: the persisted value must differ from what
    // derivation would have produced, or this test proves nothing.
    EXPECT_NE(persisted, LegacyEmbedGuid(sceneGuid, "mat"));
}

TEST(SceneIO, EmbedIdentity_WithoutGuidFallsBackToLegacyDerivation)
{
    const auto scenePath = MakeTempPath("embed_identity_legacy.scene");
    ASSERT_TRUE(WriteFile(scenePath, MakeEmbedIdentityScene("")));

    FakeAssetResolver resolver;
    resolver.SetRoot(scenePath.parent_path());
    const GUID sceneGuid = resolver.GetOrCreateAssetGuid(scenePath);

    EXPECT_EQ(ResolvedEmbedGuidString(scenePath, &resolver),
              LegacyEmbedGuid(sceneGuid, "mat").ToString());
}

TEST(SceneIO, EmbedIdentity_MalformedGuidFallsBackToLegacyDerivation)
{
    const auto scenePath = MakeTempPath("embed_identity_malformed.scene");
    ASSERT_TRUE(WriteFile(scenePath, MakeEmbedIdentityScene(" guid=\"not-a-guid\"")));

    FakeAssetResolver resolver;
    resolver.SetRoot(scenePath.parent_path());
    const GUID sceneGuid = resolver.GetOrCreateAssetGuid(scenePath);

    // Must land on the derivation, not on a null identity.
    EXPECT_EQ(ResolvedEmbedGuidString(scenePath, &resolver),
              LegacyEmbedGuid(sceneGuid, "mat").ToString());
}

TEST(SceneIO, Save_PersistsDerivedEmbedIdentityOnFirstWrite)
{
    const auto scenePath = MakeTempPath("embed_identity_migrate.scene");
    ASSERT_TRUE(WriteFile(scenePath, MakeEmbedIdentityScene("")));

    FakeAssetResolver resolver;
    resolver.SetRoot(scenePath.parent_path());
    const GUID sceneGuid = resolver.GetOrCreateAssetGuid(scenePath);
    const GUID expected = LegacyEmbedGuid(sceneGuid, "mat");

    ECS::World world;
    Scene::LoadOptions loadOpts{};
    loadOpts.mode = Scene::LoadMode::Replace;
    loadOpts.assetResolver = &resolver;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, loadOpts));

    Scene::SaveOptions saveOpts{};
    saveOpts.assetResolver = &resolver;
    ASSERT_TRUE(Scene::SaveSceneToFile(world, scenePath, saveOpts));

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    // Migration in place: the identity written is the one this file already had.
    EXPECT_NE(saved.find("[embed id=\"mat\" type=\"Material\" guid=\"" + expected.ToString() + "\"]"),
              std::string::npos);
    EXPECT_NE(saved.find("albedo = (1, 0, 0)"), std::string::npos);

    // Closing the round trip on the block that was migrated: the header this save
    // produced, read back, resolves to the identity the file started with.
    // (Reloading the saved file wholesale would not test this — save rewrites the
    // entity's `#mat` reference into a bare-GUID ref, so nothing resolves the embed
    // id afterwards. That linkage gap predates persisted identity.)
    const size_t headerBegin = saved.find("[embed ");
    ASSERT_NE(headerBegin, std::string::npos);
    const size_t headerEnd = saved.find('\n', headerBegin);
    ASSERT_NE(headerEnd, std::string::npos);
    const std::string savedHeader = saved.substr(headerBegin, headerEnd - headerBegin);

    const auto reloadPath = MakeTempPath("embed_identity_migrate_reload.scene");
    ASSERT_TRUE(WriteFile(reloadPath,
                          "[scene name=\"EmbedIdentity\" version=1]\n"
                          "\n" +
                              savedHeader +
                              "\n"
                              "albedo = (1, 0, 0)\n"
                              "\n"
                              "[entity id=\"e\"]\n"
                              "MatUser.material = #mat\n"));
    // Resolved from a different path than the one it was derived at, so a pass
    // can only come from the persisted attribute.
    EXPECT_EQ(ResolvedEmbedGuidString(reloadPath, &resolver), expected.ToString());
}

TEST(SceneIO, Save_DoesNotRewriteAnExistingEmbedGuid)
{
    const auto scenePath = MakeTempPath("embed_identity_idempotent.scene");
    const GUID persisted("0123456f-89ab-cdef-0123-456789abcdef");
    ASSERT_TRUE(WriteFile(scenePath, MakeEmbedIdentityScene(" guid=\"" + persisted.ToString() + "\"")));

    FakeAssetResolver resolver;
    resolver.SetRoot(scenePath.parent_path());

    ECS::World world;
    Scene::LoadOptions loadOpts{};
    loadOpts.mode = Scene::LoadMode::Replace;
    loadOpts.assetResolver = &resolver;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, loadOpts));

    // Each save re-reads the file on disk to collect the blocks it preserves, so
    // saving twice puts the second save on the first save's output — the path that
    // would double-stamp the attribute.
    Scene::SaveOptions saveOpts{};
    saveOpts.assetResolver = &resolver;
    ASSERT_TRUE(Scene::SaveSceneToFile(world, scenePath, saveOpts));
    ASSERT_TRUE(Scene::SaveSceneToFile(world, scenePath, saveOpts));

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    // The authored identity survives untouched, and exactly one guid= is present
    // in the [embed] header.
    const size_t headerBegin = saved.find("[embed ");
    ASSERT_NE(headerBegin, std::string::npos);
    const size_t headerEnd = saved.find('\n', headerBegin);
    ASSERT_NE(headerEnd, std::string::npos);
    const std::string header = saved.substr(headerBegin, headerEnd - headerBegin);

    EXPECT_NE(header.find("guid=\"" + persisted.ToString() + "\""), std::string::npos);
    const size_t firstGuid = header.find("guid=");
    ASSERT_NE(firstGuid, std::string::npos);
    EXPECT_EQ(header.find("guid=", firstGuid + 1), std::string::npos);
}

// Without a resolver, save cannot reproduce the registry-backed identity a real
// load resolves, so it must not guess one: the block stays as authored rather
// than being frozen to a path-derived GUID.
TEST(SceneIO, Save_WithoutResolverLeavesEmbedIdentityUnstamped)
{
    const auto scenePath = MakeTempPath("embed_identity_no_resolver.scene");
    ASSERT_TRUE(WriteFile(scenePath, MakeEmbedIdentityScene("")));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    ASSERT_TRUE(Scene::SaveSceneToFile(world, scenePath, Scene::SaveOptions{}));

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("[embed id=\"mat\" type=\"Material\"]"), std::string::npos);
    EXPECT_EQ(saved.find("guid="), std::string::npos);
}

// The defect this slice closes, and its fix, in one test: identical bytes loaded
// from two different paths. Derived identity moves with the path (a rename of a
// scene with N embeds changes N+1 identities); persisted identity does not, so a
// rename moves exactly one GUID — the scene's own.
TEST(SceneIO, EmbedIdentity_SurvivesSceneRename_UnlikeDerivedIdentity)
{
    const auto beforePath = MakeTempPath("embed_rename_before.scene");
    const auto afterPath = MakeTempPath("embed_rename_after.scene");

    FakeAssetResolver resolver;
    resolver.SetRoot(beforePath.parent_path());

    // Legacy: no persisted identity — the embed GUID follows the file name.
    ASSERT_TRUE(WriteFile(beforePath, MakeEmbedIdentityScene("")));
    ASSERT_TRUE(WriteFile(afterPath, MakeEmbedIdentityScene("")));
    const std::string legacyBefore = ResolvedEmbedGuidString(beforePath, &resolver);
    const std::string legacyAfter = ResolvedEmbedGuidString(afterPath, &resolver);
    ASSERT_FALSE(legacyBefore.empty());
    EXPECT_NE(legacyBefore, legacyAfter);

    // Persisted: the same bytes at either path resolve to one identity.
    const GUID persisted("0123456f-89ab-cdef-0123-456789abcdef");
    const std::string src = MakeEmbedIdentityScene(" guid=\"" + persisted.ToString() + "\"");
    ASSERT_TRUE(WriteFile(beforePath, src));
    ASSERT_TRUE(WriteFile(afterPath, src));
    EXPECT_EQ(ResolvedEmbedGuidString(beforePath, &resolver), persisted.ToString());
    EXPECT_EQ(ResolvedEmbedGuidString(afterPath, &resolver), persisted.ToString());
}

// A scene rename heals the container's own GUID through a redirect (S2). That
// redirect must not reach the embed: persisted identity is not derived from the
// container, so no subasset cascade rows are owed for it, and the embed keeps
// resolving to exactly what the file says.
TEST(SceneIO, EmbedIdentity_PersistedIdentityIsUnaffectedByContainerRedirect)
{
    const auto scenePath = MakeTempPath("embed_identity_container_redirect.scene");
    const GUID persisted("0123456f-89ab-cdef-0123-456789abcdef");
    ASSERT_TRUE(WriteFile(scenePath, MakeEmbedIdentityScene(" guid=\"" + persisted.ToString() + "\"")));

    FakeAssetResolver resolver;
    resolver.SetRoot(scenePath.parent_path());
    const GUID sceneGuidBefore = GUID::Derive(GUID::Null(), "stale-scene-identity");
    const GUID sceneGuidAfter = resolver.GetOrCreateAssetGuid(scenePath);
    ASSERT_NE(sceneGuidBefore, sceneGuidAfter);

    // The container's rename heal — the one GUID that moves.
    resolver.AddRedirect(sceneGuidBefore, sceneGuidAfter);

    EXPECT_EQ(ResolvedEmbedGuidString(scenePath, &resolver), persisted.ToString());
    // The cascade B2 emits for derive-based subassets would have rebound the embed
    // to Derive(after, "embed:mat"); scene embeds journal no derive keys, so it
    // must not exist. Asserting the absence keeps a future wiring honest.
    EXPECT_NE(persisted, LegacyEmbedGuid(sceneGuidAfter, "mat"));
    EXPECT_NE(ResolvedEmbedGuidString(scenePath, &resolver),
              LegacyEmbedGuid(sceneGuidAfter, "mat").ToString());
}

TEST(SceneIO, EmbedConsumer_ResolvesMaterialEmbedToAssetReference)
{
    const auto scenePath = MakeTempPath("embed_material_ref.scene");
    const std::string src =
        "[scene name=\"EmbedMat\" version=1]\n"
        "\n"
        "[embed id=\"mat\" type=\"Material\"]\n"
        "albedo = (1, 0, 0)\n"
        "\n"
        "[entity id=\"e\"]\n"
        "MatUser.material = #mat\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    struct TestEmbedMaterializer final : Scene::ISceneEmbedMaterializer
    {
        int calls = 0;
        std::unordered_map<GUID, int> callsByGuid;

        bool Materialize(const std::filesystem::path& /*owningSceneFile*/,
                         const GUID& embedGuid,
                         std::string_view /*embedId*/,
                         AssetType assetType,
                         std::string_view /*embedTypeName*/,
                         const std::unordered_map<std::string, std::string>& properties,
                         AssetReference& outRef,
                         std::string* /*outError*/) override
        {
            ++calls;
            ++callsByGuid[embedGuid];
            // The embed block should be visible here.
            EXPECT_NE(properties.find("albedo"), properties.end());
            outRef = AssetReference(embedGuid, assetType, "<test-embed>");
            return true;
        }
    } mat;

    ECS::World world;
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    opts.embedMaterializer = &mat;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));

    auto e = FindByTag(world, "e");
    ASSERT_TRUE(e.IsValid());
    auto* m = world.GetComponent<MatUser>(e);
    ASSERT_NE(m, nullptr);
    EXPECT_NE(m->guid[0], '\0');
    EXPECT_EQ(m->type, AssetType::Material);

    EXPECT_EQ(mat.calls, 1);
    EXPECT_EQ(mat.callsByGuid.size(), 1u);
}

TEST(SceneIO, EmbedMaterialization_CachesPerEmbedGuid)
{
    const auto scenePath = MakeTempPath("embed_material_cache.scene");
    const std::string src =
        "[scene name=\"EmbedCache\" version=1]\n"
        "\n"
        "[embed id=\"mat\" type=\"Material\"]\n"
        "albedo = (1, 0, 0)\n"
        "\n"
        "[entity id=\"a\"]\n"
        "MatUser.material = #mat\n"
        "\n"
        "[entity id=\"b\"]\n"
        "MatUser.material = #mat\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    struct TestEmbedMaterializer final : Scene::ISceneEmbedMaterializer
    {
        int calls = 0;
        bool Materialize(const std::filesystem::path& /*owningSceneFile*/,
                         const GUID& embedGuid,
                         std::string_view /*embedId*/,
                         AssetType assetType,
                         std::string_view /*embedTypeName*/,
                         const std::unordered_map<std::string, std::string>& /*properties*/,
                         AssetReference& outRef,
                         std::string* /*outError*/) override
        {
            ++calls;
            outRef = AssetReference(embedGuid, assetType, "<test-embed>");
            return true;
        }
    } mat;

    ECS::World world;
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    opts.embedMaterializer = &mat;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));

    auto a = FindByTag(world, "a");
    auto b = FindByTag(world, "b");
    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(b.IsValid());

    auto* ma = world.GetComponent<MatUser>(a);
    auto* mb = world.GetComponent<MatUser>(b);
    ASSERT_NE(ma, nullptr);
    ASSERT_NE(mb, nullptr);
    EXPECT_EQ(ma->type, AssetType::Material);
    EXPECT_EQ(mb->type, AssetType::Material);
    EXPECT_STREQ(ma->guid, mb->guid);

    EXPECT_EQ(mat.calls, 1);
}

TEST(SceneIO, EmbedConsumer_DegradesOnUnsupportedEmbedAssetType)
{
    const auto scenePath = MakeTempPath("embed_unknown_ref.scene");
    const std::string src =
        "[scene name=\"EmbedBad\" version=1]\n"
        "\n"
        "[embed id=\"x\" type=\"NotARealAssetType\"]\n"
        "foo = 1\n"
        "\n"
        "[entity id=\"e\"]\n"
        "MatUser.material = #x\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    EXPECT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));

    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].file.lexically_normal(), scenePath.lexically_normal());
    EXPECT_EQ(degradation.skips[0].line, 7);
    EXPECT_NE(degradation.skips[0].message.find("unsupported asset type"), std::string::npos)
        << degradation.skips[0].message;
}

TEST(SceneIO, ParseFails_OnGarbageFile)
{
    const auto scenePath = MakeTempPath("garbage.scene");
    ASSERT_TRUE(WriteFile(scenePath, "this is not a scene file\nno brackets here\n"));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
}

TEST(SceneIO, ParseFails_WhenHeaderMissingVersion)
{
    const auto scenePath = MakeTempPath("missing_version.scene");
    const std::string src =
        "[scene name=\"NoVersion\"]\n"
        "\n"
        "[entity id=\"a\"]\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
}

TEST(SceneIO, ParseFails_WhenContentBeforeHeader)
{
    const auto scenePath = MakeTempPath("content_before_header.scene");
    const std::string src =
        "Name.value = \"oops\"\n"
        "[scene name=\"S\" version=1]\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
}

TEST(SceneIO, ValidateFails_WhenParentMissing)
{
    const auto scenePath = MakeTempPath("missing_parent.scene");
    const std::string src =
        "[scene name=\"MissingParent\" version=1]\n"
        "\n"
        "[entity id=\"a\" parent=\"missing\"]\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
}

// Not a parse failure: the line parses, the VALUE does not. It costs the one property.
TEST(SceneIO, ApplyDegrades_OnInvalidTransformTuple)
{
    const auto scenePath = MakeTempPath("bad_tuple.scene");
    const std::string src =
        "[scene name=\"BadTuple\" version=1]\n"
        "\n"
        "[entity id=\"a\"]\n"
        "Transform.position = (1, 2)\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;
    EXPECT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));

    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].file.lexically_normal(), scenePath.lexically_normal());
    EXPECT_EQ(degradation.skips[0].component, "Transform");
    EXPECT_FALSE(degradation.skips[0].message.empty());

    // The entity still exists — losing a position must not lose the object.
    EXPECT_TRUE(FindByTag(world, "a").IsValid());
}

// Value SYNTAX the parser cannot read used to abort the whole load from the pre-apply validation
// pass, before the tolerant apply ever saw the line. That put a scene one unbalanced bracket away
// from being unopenable, which is the same cliff the tolerant apply exists to remove.
TEST(SceneIO, UnreadableValueSyntaxDegradesInsteadOfAbortingTheLoad)
{
    const auto scenePath = MakeTempPath("bad_value_syntax.scene");
    ASSERT_TRUE(WriteFile(scenePath,
                          "[scene name=\"BadSyntax\" version=1]\n"
                          "\n"
                          "[entity id=\"a\"]\n"
                          "Transform.position = (1, 2, 3\n"
                          "Light.intensity = 5\n"
                          "\n"
                          "[entity id=\"bystander\"]\n"
                          "Transform.position = (9, 9, 9)\n"));

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions opts{Scene::LoadMode::Replace};
    opts.outDegradation = &degradation;

    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts))
        << "one unreadable value must not cost the caller the whole scene";

    const ECS::EntityHandle a = FindByTag(world, "a");
    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(FindByTag(world, "bystander").IsValid()) << "entities after the bad one still load";

    // The good field on the same entity applied, so the skip really is confined to the one value.
    const auto* light = world.GetComponent<Components::Light>(a);
    ASSERT_NE(light, nullptr);
    EXPECT_FLOAT_EQ(light->Intensity, 5.0f);

    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].component, "Transform");
    EXPECT_EQ(degradation.skips[0].entityId, "a");
}

// The other side of the line. A dangling entity reference is a structural claim about the document
// rather than a value a component can decline, so it still stops the load.
TEST(SceneIO, DanglingEntityReferenceStillAbortsTheLoad)
{
    const auto scenePath = MakeTempPath("dangling_ref.scene");
    ASSERT_TRUE(WriteFile(scenePath,
                          "[scene name=\"Dangling\" version=1]\n"
                          "\n"
                          "[entity id=\"a\"]\n"
                          "Transform.position = (1, 2, 3)\n"
                          "SplineFence.SpanGrade = $no_such_entity\n"));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    EXPECT_NE(Scene::GetLastSceneIOError().message.find("Dangling entity reference"), std::string::npos)
        << Scene::GetLastSceneIOError().message;
}

TEST(SceneIO, ValidateFails_WhenBlueprintSourceMissing)
{
    const auto scenePath = MakeTempPath("bp_missing_resource.scene");
    const std::string src =
        "[scene name=\"BPBad\" version=1]\n"
        "\n"
        "[blueprint id=\"inst\" source=\"missing\"]\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
}

TEST(SceneIO, CustomComponentSchema_LoadAndSaveRoundTrip)
{
    const auto scenePath = MakeTempPath("custom_health.scene");
    const std::string src =
        "[scene name=\"Custom\" version=1]\n"
        "\n"
        "[entity id=\"e\"]\n"
        "Health.value = 12.5\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    auto e = FindByTag(w, "e");
    ASSERT_TRUE(e.IsValid());
    auto* h = w.GetComponent<Health>(e);
    ASSERT_NE(h, nullptr);
    EXPECT_FLOAT_EQ(h->value, 12.5f);

    const auto outPath = MakeTempPath("custom_health_out.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w, outPath, Scene::SaveOptions{}));

    std::string saved;
    ASSERT_TRUE(ReadFile(outPath, saved));
    EXPECT_NE(saved.find("Health.value"), std::string::npos);
}

TEST(SceneIO, Include_MergesResourcesAndEnablesBlueprintUse)
{
    const auto dir = std::filesystem::temp_directory_path() / "GameEngine_SceneIOTests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const auto incPath = (dir / "common.scene").lexically_normal();
    const auto bpPath = (dir / "bp_inc.blueprint").lexically_normal();
    const auto scenePath = (dir / "main_with_include.scene").lexically_normal();

    const std::string bp =
        "[blueprint name=\"BP\" version=1]\n"
        "\n"
        "[entity id=\"root\"]\n"
        "Name.value = \"BP\"\n";
    ASSERT_TRUE(WriteFile(bpPath, bp));

    const std::string inc =
        "[scene name=\"Common\" version=1]\n"
        "\n"
        "[resource id=\"bp\" path=\"bp_inc.blueprint\"]\n";
    ASSERT_TRUE(WriteFile(incPath, inc));

    const std::string scene =
        "[scene name=\"Main\" version=1]\n"
        "[include path=\"common.scene\"]\n"
        "\n"
        "[blueprint id=\"inst\" source=\"bp\"]\n"
        "Name.value = \"Over\"\n";
    ASSERT_TRUE(WriteFile(scenePath, scene));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));

    auto inst = FindByTag(world, "inst");
    ASSERT_TRUE(inst.IsValid());
}

TEST(SceneIO, AssetDependencyExtractor_FindsSceneAndBlueprintPathRefs)
{
    const auto dir = std::filesystem::temp_directory_path() / "GameEngine_SceneIOTests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const auto scenePath = (dir / "deps.scene").lexically_normal();
    const std::string src =
        "[scene name=\"Deps\" version=1]\n"
        "[include path=common.scene]\n"
        "[resource id=\"bp\" path=enemy.blueprint]\n"
        "\n"
        "[entity id=\"e\"]\n"
        "Foo.path = @\"meshes/cube.fbx\"\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    AssetMetadata md{};
    md.Guid = GUID::Derive(GUID::Null(), scenePath.string());
    md.Type = AssetType::Scene;
    md.Path = scenePath;
    md.Name = scenePath.stem().string();
    md.Extension = scenePath.extension().string();

    const auto info = AssetDependencyExtractor::ExtractDependencies(md);
    // We don't require the registry to be initialized here; we just want path refs recorded.
    EXPECT_NE(std::find(info.DependencyPaths.begin(), info.DependencyPaths.end(), "common.scene"), info.DependencyPaths.end());
    EXPECT_NE(std::find(info.DependencyPaths.begin(), info.DependencyPaths.end(), "enemy.blueprint"), info.DependencyPaths.end());
    EXPECT_NE(std::find(info.DependencyPaths.begin(), info.DependencyPaths.end(), "meshes/cube.fbx"), info.DependencyPaths.end());
}

TEST(SceneIO, ValidateFails_OnDanglingEntityReference)
{
    const auto scenePath = MakeTempPath("dangling_entity_ref.scene");
    const std::string src =
        "[scene name=\"DanglingEnt\" version=1]\n"
        "[entity id=\"a\"]\n"
        "Foo.target = $missing\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto& err = Scene::GetLastSceneIOError();
    EXPECT_EQ(err.file.lexically_normal(), scenePath.lexically_normal());
    EXPECT_EQ(err.line, 3);
    EXPECT_NE(err.message.find("Dangling entity reference"), std::string::npos);
}

TEST(SceneIO, ValidateFails_OnDanglingResourceReference)
{
    const auto scenePath = MakeTempPath("dangling_resource_ref.scene");
    const std::string src =
        "[scene name=\"DanglingRes\" version=1]\n"
        "[entity id=\"a\"]\n"
        "Foo.material = #missing\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto& err = Scene::GetLastSceneIOError();
    EXPECT_EQ(err.file.lexically_normal(), scenePath.lexically_normal());
    EXPECT_EQ(err.line, 3);
    EXPECT_NE(err.message.find("Dangling resource reference"), std::string::npos);
}

TEST(SceneIO, ValidateFails_WhenIncludeContainsEntities)
{
    const auto dir = std::filesystem::temp_directory_path() / "GameEngine_SceneIOTests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const auto incPath = (dir / "include_with_entity.scene").lexically_normal();
    const auto scenePath = (dir / "main_with_bad_include.scene").lexically_normal();

    const std::string inc =
        "[scene name=\"Inc\" version=1]\n"
        "[entity id=\"e\"]\n"
        "Name.value = \"Nope\"\n";
    ASSERT_TRUE(WriteFile(incPath, inc));

    const std::string scene =
        "[scene name=\"Main\" version=1]\n"
        "[include path=\"include_with_entity.scene\"]\n";
    ASSERT_TRUE(WriteFile(scenePath, scene));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto& err = Scene::GetLastSceneIOError();
    EXPECT_EQ(err.file.lexically_normal(), incPath.lexically_normal());
    EXPECT_NE(err.line, 0);
}

TEST(SceneIO, ValidateFails_WhenEmbedIdCollidesWithResourceId)
{
    const auto scenePath = MakeTempPath("embed_resource_collision.scene");
    const std::string src =
        "[scene name=\"Collide\" version=1]\n"
        "[resource id=\"x\" path=\"something.blueprint\"]\n"
        "[embed id=\"x\" type=\"Material\"]\n"
        "color = (1, 0, 0)\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto& err = Scene::GetLastSceneIOError();
    EXPECT_EQ(err.file.lexically_normal(), scenePath.lexically_normal());
    EXPECT_EQ(err.line, 3);
    EXPECT_NE(err.message.find("Duplicate id"), std::string::npos);
}

TEST(SceneIO, ParseFails_WhenSubsceneEnabledIsNotBool)
{
    const auto scenePath = MakeTempPath("bad_subscene_enabled.scene");
    const std::string src =
        "[scene name=\"BadSub\" version=1]\n"
        "[resource id=\"s\" path=\"chunk.scene\"]\n"
        "[subscene id=\"area\" source=\"s\"]\n"
        "enabled = \"yes\"\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto& err = Scene::GetLastSceneIOError();
    EXPECT_EQ(err.file.lexically_normal(), scenePath.lexically_normal());
    EXPECT_EQ(err.line, 4);
}

TEST(SceneIO, BlueprintFileMustContainEntities)
{
    const auto dir = std::filesystem::temp_directory_path() / "GameEngine_SceneIOTests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const auto bpPath = (dir / "empty.blueprint").lexically_normal();
    const auto scenePath = (dir / "scene_uses_empty_bp.scene").lexically_normal();

    const std::string bp =
        "[blueprint name=\"Empty\" version=1]\n";
    ASSERT_TRUE(WriteFile(bpPath, bp));

    const std::string scene =
        "[scene name=\"Main\" version=1]\n"
        "[resource id=\"bp\" path=\"empty.blueprint\"]\n"
        "[blueprint id=\"inst\" source=\"bp\"]\n";
    ASSERT_TRUE(WriteFile(scenePath, scene));

    ECS::World world;
    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto& err = Scene::GetLastSceneIOError();
    EXPECT_EQ(err.file.lexically_normal(), bpPath.lexically_normal());
    EXPECT_NE(err.message.find("must contain at least one"), std::string::npos);
}

TEST(SceneValue, ParseAssetRef_PathAndGuid)
{
    Scene::SceneValue v;
    std::string err;
    ASSERT_TRUE(Scene::ParseValue("[path=\"Models/cube.fbx\" guid=\"12345678-aaaa-bbbb-cccc-1234567890ab\"]", v, &err)) << err;
    ASSERT_EQ(v.Kind, Scene::SceneValueKind::AssetRef);
    EXPECT_EQ(Scene::AssetRefPath(v), "Models/cube.fbx");
    EXPECT_EQ(Scene::AssetRefGuid(v), "12345678-aaaa-bbbb-cccc-1234567890ab");
}

TEST(SceneValue, ParseAssetRef_GuidOnly)
{
    Scene::SceneValue v;
    std::string err;
    ASSERT_TRUE(Scene::ParseValue("[guid=\"abc-123\"]", v, &err)) << err;
    ASSERT_EQ(v.Kind, Scene::SceneValueKind::AssetRef);
    EXPECT_EQ(Scene::AssetRefPath(v), "");
    EXPECT_EQ(Scene::AssetRefGuid(v), "abc-123");
}

TEST(SceneValue, ParseAssetRef_PathOnly)
{
    Scene::SceneValue v;
    std::string err;
    ASSERT_TRUE(Scene::ParseValue("[path=\"foo/bar.fbx\"]", v, &err)) << err;
    ASSERT_EQ(v.Kind, Scene::SceneValueKind::AssetRef);
    EXPECT_EQ(Scene::AssetRefPath(v), "foo/bar.fbx");
    EXPECT_EQ(Scene::AssetRefGuid(v), "");
}

TEST(SceneValue, ParseAssetRef_OrderIndependent)
{
    Scene::SceneValue v;
    std::string err;
    ASSERT_TRUE(Scene::ParseValue("[guid=\"g1\" path=\"p1\"]", v, &err)) << err;
    ASSERT_EQ(v.Kind, Scene::SceneValueKind::AssetRef);
    EXPECT_EQ(Scene::AssetRefPath(v), "p1");
    EXPECT_EQ(Scene::AssetRefGuid(v), "g1");
}

TEST(SceneValue, ParseAssetRef_RejectsBothFieldsEmpty)
{
    // [path="" guid=""] reaches the AssetRef branch (sawAssign=true) but is
    // rejected because at least one field must be non-empty.
    Scene::SceneValue v;
    std::string err;
    EXPECT_FALSE(Scene::ParseValue("[path=\"\" guid=\"\"]", v, &err));
    EXPECT_NE(err.find("at least one"), std::string::npos) << err;
}

TEST(SceneValue, ParseAssetRef_RejectsUnknownKey)
{
    Scene::SceneValue v;
    std::string err;
    EXPECT_FALSE(Scene::ParseValue("[name=\"foo\"]", v, &err));
    EXPECT_NE(err.find("unknown key"), std::string::npos) << err;
}

TEST(SceneValue, ParseValue_ArrayStillWorks)
{
    // Ensure the disambiguation didn't break array parsing.
    Scene::SceneValue v;
    std::string err;
    ASSERT_TRUE(Scene::ParseValue("[1, 2, 3]", v, &err)) << err;
    ASSERT_EQ(v.Kind, Scene::SceneValueKind::Array);
    ASSERT_EQ(v.Items.size(), 3u);
}

// A bare (sigil-free) GUID is the token the debug server's set_component hands a schema —
// it renders JSON strings as raw text. Without a dedicated branch it falls through to the
// number/identifier fallbacks, so schemas see Int or Identifier and reject a valid reference.
TEST(SceneValue, ParseValue_BareGuidToken)
{
    Scene::SceneValue v;
    std::string err;
    // Letter-leading: used to land as Identifier.
    ASSERT_TRUE(Scene::ParseValue("fedcba98-7654-3210-fedc-ba9876543210", v, &err)) << err;
    EXPECT_EQ(v.Kind, Scene::SceneValueKind::GuidRef);
    EXPECT_EQ(v.StringValue, "fedcba98-7654-3210-fedc-ba9876543210");

    // Digit-leading: used to land as Int(1234567) — strtoll stops at the first non-digit and
    // ParseInt64 never checked for full consumption.
    ASSERT_TRUE(Scene::ParseValue("01234567-89ab-cdef-0123-456789abcdef", v, &err)) << err;
    EXPECT_EQ(v.Kind, Scene::SceneValueKind::GuidRef);
    EXPECT_EQ(v.StringValue, "01234567-89ab-cdef-0123-456789abcdef");

    // Uppercase hex is still canonical GUID text.
    ASSERT_TRUE(Scene::ParseValue("0123ABCD-89AB-CDEF-0123-456789ABCDEF", v, &err)) << err;
    EXPECT_EQ(v.Kind, Scene::SceneValueKind::GuidRef);
}

// Only the canonical 8-4-4-4-12 shape is a GUID; near-misses must NOT be promoted (a schema
// would then hand them to GUID(), which silently yields a null guid).
TEST(SceneValue, ParseValue_NonGuidTokensAreNotGuidRefs)
{
    Scene::SceneValue v;
    for (const char* s : {"not-a-guid",                            // too short, non-hex
                          "0123456789ab-cdef-0123-456789abcdef",   // dashes in the wrong places
                          "0123456g-89ab-cdef-0123-456789abcdef",  // 'g' is not hex
                          "0123456789abcdef0123456789abcdef"})     // compact (no dashes)
    {
        Scene::ParseValue(s, v);
        EXPECT_NE(v.Kind, Scene::SceneValueKind::GuidRef) << "wrongly promoted '" << s << "'";
    }

    // The existing forms still parse as themselves.
    std::string err;
    ASSERT_TRUE(Scene::ParseValue("&{fedcba98-7654-3210-fedc-ba9876543210}", v, &err)) << err;
    EXPECT_EQ(v.Kind, Scene::SceneValueKind::GuidRef);
    ASSERT_TRUE(Scene::ParseValue("42", v, &err)) << err;
    EXPECT_EQ(v.Kind, Scene::SceneValueKind::Int);
    ASSERT_TRUE(Scene::ParseValue("Spherical", v, &err)) << err;
    EXPECT_EQ(v.Kind, Scene::SceneValueKind::Identifier);
}

TEST(SceneValue, FormatAssetRef_RoundTrips)
{
    const std::string s = Scene::FormatAssetRef("Models/cube.fbx", "12345678-aaaa-bbbb-cccc-1234567890ab");
    EXPECT_EQ(s, "[path=\"Models/cube.fbx\" guid=\"12345678-aaaa-bbbb-cccc-1234567890ab\"]");

    Scene::SceneValue v;
    std::string err;
    ASSERT_TRUE(Scene::ParseValue(s, v, &err)) << err;
    EXPECT_EQ(v.Kind, Scene::SceneValueKind::AssetRef);
    EXPECT_EQ(Scene::AssetRefPath(v), "Models/cube.fbx");
    EXPECT_EQ(Scene::AssetRefGuid(v), "12345678-aaaa-bbbb-cccc-1234567890ab");
}

TEST(SceneValue, FormatAssetRef_GuidOnly)
{
    EXPECT_EQ(Scene::FormatAssetRef("", "abc"), "[guid=\"abc\"]");
}

TEST(SceneValue, FormatAssetRef_PathOnly)
{
    EXPECT_EQ(Scene::FormatAssetRef("p", ""), "[path=\"p\"]");
}

namespace
{
// Helper: build a temp asset root, populate the resolver, and return both.
struct HealHarness
{
    std::filesystem::path projectRoot;
    std::filesystem::path scenePath;
    FakeAssetResolver resolver;
    GUID matGuid;
    std::filesystem::path matRel = "Materials/Mat.material";

    HealHarness(const char* tag)
    {
        projectRoot = std::filesystem::temp_directory_path() / (std::string("ge_heal_") + tag);
        std::error_code ec;
        std::filesystem::remove_all(projectRoot, ec);
        std::filesystem::create_directories(projectRoot / "Materials", ec);
        scenePath = projectRoot / "main.scene";
        resolver.SetRoot(projectRoot);
        // Mint a stable guid for the material.
        matGuid = GUID::Derive(GUID::Null(), "test-material-fixture");
        resolver.Bind(matGuid, matRel, AssetType::Material);
        // Touch the file on disk so AbsolutizeAuthoredPath has something real.
        std::ofstream f(projectRoot / matRel, std::ios::binary);
        f << "{}";
    }
    ~HealHarness()
    {
        std::error_code ec;
        std::filesystem::remove_all(projectRoot, ec);
    }
};

static void SetMatUserV2(MatUserV2& mu, const GUID& g, std::string_view path)
{
    if (!g.IsNull())
    {
        const std::string gs = g.ToString();
        const size_t n = std::min(gs.size(), sizeof(mu.guid) - 1);
        std::memcpy(mu.guid, gs.data(), n);
        mu.guid[n] = '\0';
    }
    if (!path.empty())
    {
        const size_t n = std::min(path.size(), sizeof(mu.path) - 1);
        std::memcpy(mu.path, path.data(), n);
        mu.path[n] = '\0';
    }
}

// Build a scene with a single MatUserV2 entity carrying a given (guid, path) pair,
// save it through SaveSceneToFile (engaging the heal pass), and return the saved text.
static std::string SaveMatUserV2AndReadBack(HealHarness& h, const GUID& g, std::string_view path)
{
    ECS::World w;
    auto e = w.CreateEntity();
    Components::SceneEntityTag tag{};
    std::strcpy(tag.value, "ent");
    w.AddComponentImmediate(e, tag);
    MatUserV2 mu{};
    SetMatUserV2(mu, g, path);
    w.AddComponentImmediate(e, mu);

    Scene::SaveOptions opts;
    opts.assetResolver = &h.resolver;
    opts.preserveBlueprintInstances = false;
    opts.preserveSubscenes = false;
    if (!Scene::SaveSceneToFile(w, h.scenePath, opts))
        return {};

    std::ifstream f(h.scenePath, std::ios::binary);
    std::string out((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return out;
}
} // namespace

TEST(SceneIO_HealOnSave, GuidOnly_LooksUpPath)
{
    HealHarness h("guidonly");
    const std::string saved = SaveMatUserV2AndReadBack(h, h.matGuid, /*path*/ "");
    ASSERT_FALSE(saved.empty());
    EXPECT_NE(saved.find("MatUserV2.mat = [path=\"Materials/Mat.material\" guid=\""), std::string::npos)
        << saved;
    EXPECT_NE(saved.find(h.matGuid.ToString()), std::string::npos) << saved;
}

TEST(SceneIO_HealOnSave, PathOnly_LooksUpGuid)
{
    HealHarness h("pathonly");
    const std::string saved = SaveMatUserV2AndReadBack(h, GUID::Null(), "Materials/Mat.material");
    ASSERT_FALSE(saved.empty());
    EXPECT_NE(saved.find("MatUserV2.mat = [path=\"Materials/Mat.material\" guid=\""), std::string::npos)
        << saved;
    EXPECT_NE(saved.find(h.matGuid.ToString()), std::string::npos) << saved;
}

TEST(SceneIO_HealOnSave, StalePath_HealsToCanonicalPath)
{
    HealHarness h("stalepath");
    // Caller hands in a wrong path; resolver knows the guid maps to Mat.material.
    const std::string saved = SaveMatUserV2AndReadBack(h, h.matGuid, "Materials/OldNameThatMoved.material");
    ASSERT_FALSE(saved.empty());
    // Canonical path wins; stale path does NOT appear.
    EXPECT_NE(saved.find("path=\"Materials/Mat.material\""), std::string::npos) << saved;
    EXPECT_EQ(saved.find("OldNameThatMoved"), std::string::npos) << saved;
}

TEST(SceneIO_HealOnSave, BothPresent_GuidWinsWhenDisagreeing)
{
    HealHarness h("guidwins");
    // Bind a SECOND material with a different guid+path, then save with the OLD guid + NEW path.
    const GUID otherGuid = GUID::Derive(GUID::Null(), "other-material");
    h.resolver.Bind(otherGuid, "Materials/Other.material", AssetType::Material);
    std::ofstream f(h.projectRoot / "Materials/Other.material", std::ios::binary);
    f << "{}";
    f.close();

    // We pass matGuid (resolves to Mat.material) but authored path "Materials/Other.material".
    // Healing pass 1 trusts the guid → Mat.material wins.
    const std::string saved = SaveMatUserV2AndReadBack(h, h.matGuid, "Materials/Other.material");
    ASSERT_FALSE(saved.empty());
    EXPECT_NE(saved.find("path=\"Materials/Mat.material\""), std::string::npos) << saved;
    EXPECT_NE(saved.find(h.matGuid.ToString()), std::string::npos) << saved;
    EXPECT_EQ(saved.find(otherGuid.ToString()), std::string::npos) << saved;
}

TEST(SceneIO_HealOnSave, NoResolver_EmitsAuthoredFieldsAsIs)
{
    HealHarness h("noresolver");

    ECS::World w;
    auto e = w.CreateEntity();
    Components::SceneEntityTag tag{};
    std::strcpy(tag.value, "ent");
    w.AddComponentImmediate(e, tag);
    MatUserV2 mu{};
    SetMatUserV2(mu, h.matGuid, "Materials/Mat.material");
    w.AddComponentImmediate(e, mu);

    Scene::SaveOptions opts; // no resolver
    opts.preserveBlueprintInstances = false;
    opts.preserveSubscenes = false;
    ASSERT_TRUE(Scene::SaveSceneToFile(w, h.scenePath, opts));

    std::ifstream f(h.scenePath, std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    EXPECT_NE(saved.find("path=\"Materials/Mat.material\""), std::string::npos) << saved;
    EXPECT_NE(saved.find(h.matGuid.ToString()), std::string::npos) << saved;
}

TEST(SceneIO_LoadFallback, CrossProjectGuidMiss_FallsBackToPath)
{
    HealHarness h("xprojload");

    // Write a scene where the saved guid is UNKNOWN to the resolver, but the path matches.
    const GUID unknownGuid = GUID::Derive(GUID::Null(), "stale-guid-from-old-project");
    const std::string scene = std::string(
                                  "[scene name=\"x\" version=1]\n\n"
                                  "[entity id=\"ent\"]\n"
                                  "Name.value = \"E\"\n"
                                  "Transform.position = (0, 0, 0)\n"
                                  "Transform.rotation = (0, 0, 0, 1)\n"
                                  "Transform.scale = (1, 1, 1)\n"
                                  "MatUserV2.mat = [path=\"Materials/Mat.material\" guid=\"") +
                              unknownGuid.ToString() + "\"]\n";
    {
        std::ofstream f(h.scenePath, std::ios::binary);
        f.write(scene.data(), static_cast<std::streamsize>(scene.size()));
    }

    Scene::LoadOptions opts;
    opts.mode = Scene::LoadMode::Replace;
    opts.assetResolver = &h.resolver;
    opts.assetRootOverride = h.projectRoot;

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, h.scenePath, opts));
    auto handle = FindByTag(w, "ent");
    ASSERT_TRUE(handle.IsValid());
    auto* m = w.GetComponent<MatUserV2>(handle);
    ASSERT_NE(m, nullptr);

    // Cross-project recovery: registry didn't know unknownGuid, so the load helper
    // called GetOrCreateAssetGuid(path) and bound a fresh guid.
    ASSERT_NE(m->guid[0], '\0');
    const GUID storedGuid(std::string(m->guid));
    EXPECT_FALSE(storedGuid.IsNull());
    EXPECT_NE(storedGuid, unknownGuid);
    EXPECT_GE(h.resolver.MintCalls(), 1);
}

namespace
{
// One entity whose MeshRenderer.material names a GUID nothing knows, at a path with no file; returns
// the reference line as authored.
std::string WriteAbsentAssetScene(HealHarness& h, const GUID& absentGuid)
{
    h.resolver.SetMintRequiresFileOnDisk(true); // the editor's registry refuses absent files
    const std::string referenceLine = "MeshRenderer.material = [path=\"Materials/Gone.material\" guid=\"" +
                                      absentGuid.ToString() + "\"]";
    const std::string scene = "[scene name=\"x\" version=1]\n\n"
                              "[entity id=\"ent\"]\n" +
                              referenceLine + "\n";
    std::ofstream f(h.scenePath, std::ios::binary);
    f.write(scene.data(), static_cast<std::streamsize>(scene.size()));
    return referenceLine;
}

bool LoadHarnessScene(HealHarness& h, ECS::World& w)
{
    Scene::LoadOptions opts;
    opts.mode = Scene::LoadMode::Replace;
    opts.assetResolver = &h.resolver;
    opts.assetRootOverride = h.projectRoot;
    return Scene::LoadSceneFromFile(w, h.scenePath, opts);
}

std::string SaveHarnessSceneAndReadBack(HealHarness& h, ECS::World& w)
{
    Scene::SaveOptions opts;
    opts.assetResolver = &h.resolver;
    opts.preserveBlueprintInstances = false;
    opts.preserveSubscenes = false;
    if (!Scene::SaveSceneToFile(w, h.scenePath, opts))
        return {};
    std::ifstream f(h.scenePath, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
} // namespace

// A reference whose file is absent loads degraded, and the component keeps only its GUID. The save
// must still write the path the scene authored: it is what heals the reference once the file
// returns, and dropping it is silent data loss in every scene with a missing asset.
TEST(SceneIO_LoadFallback, AbsentAssetKeepsItsAuthoredPathThroughLoadAndSave)
{
    HealHarness h("absentkeepspath");
    const std::string referenceLine =
        WriteAbsentAssetScene(h, GUID::Derive(GUID::Null(), "material-whose-file-is-gone"));
    ECS::World w;
    ASSERT_TRUE(LoadHarnessScene(h, w));

    const std::string saved = SaveHarnessSceneAndReadBack(h, w);
    EXPECT_NE(saved.find(referenceLine + "\n"), std::string::npos) << saved;
}

// Leaving play mode and a snapshot undo or redo restore the world from a snapshot; the next save
// must still write the authored path.
TEST(SceneIO_LoadFallback, AbsentAssetKeepsItsAuthoredPathThroughASnapshotRestore)
{
    HealHarness h("absentsnapshot");
    const std::string referenceLine =
        WriteAbsentAssetScene(h, GUID::Derive(GUID::Null(), "material-whose-file-is-gone"));
    ECS::World w;
    ASSERT_TRUE(LoadHarnessScene(h, w));

    w.DeserializeWorld(w.SerializeWorld());

    const std::string saved = SaveHarnessSceneAndReadBack(h, w);
    EXPECT_NE(saved.find(referenceLine + "\n"), std::string::npos) << saved;
}

// The recorded path matters only while the GUID names no asset: once the asset is known, at
// whatever path, the save writes the asset's own path.
TEST(SceneIO_LoadFallback, ResolvedAssetWritesItsOwnPathOverTheRecordedOne)
{
    HealHarness h("absentresolved");
    const GUID absentGuid = GUID::Derive(GUID::Null(), "material-whose-file-is-gone");
    WriteAbsentAssetScene(h, absentGuid);
    ECS::World w;
    ASSERT_TRUE(LoadHarnessScene(h, w));

    h.resolver.Bind(absentGuid, "Materials/Moved.material", AssetType::Material);

    const std::string saved = SaveHarnessSceneAndReadBack(h, w);
    EXPECT_NE(saved.find("MeshRenderer.material = [path=\"Materials/Moved.material\" guid=\"" +
                         absentGuid.ToString() + "\"]\n"),
              std::string::npos)
        << saved;
    EXPECT_EQ(saved.find("Gone.material"), std::string::npos) << saved;
}

namespace
{
// The five retired pre-volume modifier block names, as they are spelled in a
// scene file. A scene carrying any of them is a scene the loader must migrate.
constexpr const char* kRetiredModifierBlockNames[] = {
    "TerrainFlattenModifier", "TerrainNoiseModifier", "TerrainStampModifier",
    "TerrainPaintLayerModifier", "TerrainSplineModifier"};

// The retired FILE FORMAT fixture. Deliberately the only scene in the tree that
// still carries these blocks: no shipped scene does, so this is what keeps the
// load path they exist for exercised against a real file rather than only
// against text a test synthesizes.
std::filesystem::path LegacyModifierFixturePath()
{
    return ResolveStagedFixturePath(
        "Engine/Tests/Fixtures/TerrainLegacyModifierScene/LegacyTerrainModifiers.scene");
}

// Copies a scene to a unique temp path. WarnMigratedPreVolumeModifier reports
// once per scene FILE for the life of the process, so a test that reads the
// warning must load through a path no other test uses — otherwise it measures
// which test ran first.
std::filesystem::path CopySceneToTemp(const std::filesystem::path& source, const char* name)
{
    const auto destination = MakeTempPath(name);
    std::error_code ec;
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing, ec);
    return ec ? std::filesystem::path{} : destination;
}
} // namespace

// Every retired pre-volume modifier block, loaded from a real scene file.
//
// The migration parity oracles in TerrainModifierVolumeTests drive these blocks
// through the schema as in-memory property lists; this is the half that proves a
// FILE spelling them still parses and still arrives at the volume model, which
// is the whole reason the block names are still registered.
//
// Values in the fixture are off their defaults, so a migration that dropped a
// field lands on the default and fails here rather than agreeing by coincidence
// — the pre-volume components and the effects share most of their default
// values, so equal-to-default assertions prove nothing.
TEST(SceneIO, LoadLegacyTerrainModifierScene_MigratesEveryRetiredBlock)
{
    const auto scenePath = LegacyModifierFixturePath();
    ASSERT_TRUE(std::filesystem::exists(scenePath)) << scenePath.string();

    ECS::World world;
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    opts.assetRootOverride = scenePath.parent_path();

    Scene::EnsureTerrainSceneSchemasRegistered();

    const bool ok = Scene::LoadSceneFromFile(world, scenePath, opts);
    const auto& err = Scene::GetLastSceneIOError();
    ASSERT_TRUE(ok) << err.file.string() << ":" << err.line << " " << err.message;

    const auto terrain = FindByTag(world, "terrain");
    ASSERT_TRUE(terrain.IsValid());
    EXPECT_NE(world.GetComponent<Components::Terrain>(terrain), nullptr);

    // The schemas are the only things that still understand these blocks, and
    // each reports IsPresent off its own Serialize — so a false everywhere is
    // both "no pre-volume component survived the load" and "a re-save of this
    // scene emits no legacy block".
    const std::vector<std::string> migratedEntities = {
        "flat_middle", "pad_absolute", "noise_dunes",
        "stamp_crater", "paint_scree", "road_cut", "road_berm"};
    for (const auto& tag : migratedEntities)
    {
        const auto entity = FindByTag(world, tag);
        ASSERT_TRUE(entity.IsValid()) << tag;
        for (const char* blockName : kRetiredModifierBlockNames)
        {
            const auto* schema = Scene::SceneSchemaRegistry::Find(blockName);
            ASSERT_NE(schema, nullptr) << blockName;
            EXPECT_FALSE(schema->IsPresent(world, entity)) << tag << " kept " << blockName;
        }
        EXPECT_NE(world.GetComponent<Components::TerrainModifierVolume>(entity), nullptr) << tag;
    }

    // Two blocks over ONE shared region on one entity: they migrate into a single
    // volume carrying two stacked effects, in file order.
    const auto flatMiddle = FindByTag(world, "flat_middle");
    const auto* sharedVolume = world.GetComponent<Components::TerrainModifierVolume>(flatMiddle);
    ASSERT_NE(sharedVolume, nullptr);
    EXPECT_EQ(sharedVolume->Shape, Components::TerrainVolumeShape::Circle);
    EXPECT_FLOAT_EQ(sharedVolume->Radius, 75.0f);
    EXPECT_FLOAT_EQ(sharedVolume->Falloff, 100.0f);

    const auto* sharedFlatten = world.GetComponent<Components::TerrainFlattenEffect>(flatMiddle);
    const auto* sharedNoise = world.GetComponent<Components::TerrainNoiseEffect>(flatMiddle);
    ASSERT_NE(sharedFlatten, nullptr);
    ASSERT_NE(sharedNoise, nullptr);
    EXPECT_TRUE(sharedFlatten->UseVolumeHeight);
    EXPECT_LT(sharedFlatten->StackOrder, sharedNoise->StackOrder);
    // A pre-erosion scene migrates with the block disarmed, which is what keeps
    // every authored scene baking exactly what it baked before.
    EXPECT_FLOAT_EQ(sharedNoise->ErosionStrength, 0.0f);

    // useEntityHeight false is the branch that carries targetHeight across; true
    // maps it to a zero offset from the volume's reference height, whatever the
    // stale authored value was.
    const auto padAbsolute = FindByTag(world, "pad_absolute");
    const auto* padVolume = world.GetComponent<Components::TerrainModifierVolume>(padAbsolute);
    const auto* padFlatten = world.GetComponent<Components::TerrainFlattenEffect>(padAbsolute);
    ASSERT_NE(padVolume, nullptr);
    ASSERT_NE(padFlatten, nullptr);
    EXPECT_EQ(padVolume->Shape, Components::TerrainVolumeShape::Rectangle);
    EXPECT_FLOAT_EQ(padVolume->RectHalfX, 34.0f);
    EXPECT_FLOAT_EQ(padVolume->RectHalfZ, 22.0f);
    EXPECT_FLOAT_EQ(padVolume->Falloff, 7.5f);
    EXPECT_FLOAT_EQ(padVolume->Priority, 6.0f);
    EXPECT_FALSE(padFlatten->UseVolumeHeight);
    EXPECT_FLOAT_EQ(padFlatten->TargetHeight, 42.5f);

    // Every noise field, each authored away from its default.
    const auto dunes = FindByTag(world, "noise_dunes");
    const auto* dunesNoise = world.GetComponent<Components::TerrainNoiseEffect>(dunes);
    ASSERT_NE(dunesNoise, nullptr);
    EXPECT_EQ(dunesNoise->Blend, Components::TerrainModifierBlend::Subtract);
    EXPECT_FLOAT_EQ(dunesNoise->Frequency, 6.5f);
    EXPECT_FLOAT_EQ(dunesNoise->Amplitude, 18.0f);
    EXPECT_EQ(dunesNoise->Octaves, 3u);
    EXPECT_EQ(dunesNoise->Seed, 91u);
    EXPECT_FLOAT_EQ(dunesNoise->Lacunarity, 2.1f);
    EXPECT_FLOAT_EQ(dunesNoise->Persistence, 0.45f);
    EXPECT_FLOAT_EQ(dunesNoise->BlendSmoothing, 1.75f);

    // The stamp is authored DISABLED: `enabled` is a shared base field and its
    // carry-over onto the effect is what a scene turning a modifier off relies on.
    const auto crater = FindByTag(world, "stamp_crater");
    const auto* craterStamp = world.GetComponent<Components::TerrainStampEffect>(crater);
    ASSERT_NE(craterStamp, nullptr);
    EXPECT_FALSE(craterStamp->Enabled);
    EXPECT_EQ(craterStamp->Blend, Components::TerrainModifierBlend::Max);
    EXPECT_FLOAT_EQ(craterStamp->HeightScale, 22.0f);
    EXPECT_FLOAT_EQ(craterStamp->Rotation, 37.0f);
    EXPECT_FLOAT_EQ(craterStamp->BlendSmoothing, 2.5f);
    EXPECT_EQ(craterStamp->StampAssetGuid.ToGuid(),
              GUID("3f7c1d20-95a4-4e6b-9a11-6c8d2b4f0e77"));

    const auto scree = FindByTag(world, "paint_scree");
    const auto* screePaint = world.GetComponent<Components::TerrainPaintLayerEffect>(scree);
    ASSERT_NE(screePaint, nullptr);
    EXPECT_EQ(screePaint->LayerIndex, 2u);
    EXPECT_FLOAT_EQ(screePaint->Strength, 0.65f);
    EXPECT_TRUE(screePaint->Replace);

    // A spline modifier's region is its spline, and Shape::Spline resolved
    // through a fill — so it migrates to SplineArea, never SplinePath. Its
    // flatten branch targets the height offset from the volume's own height.
    const auto roadCut = FindByTag(world, "road_cut");
    const auto* cutVolume = world.GetComponent<Components::TerrainModifierVolume>(roadCut);
    const auto* cutFlatten = world.GetComponent<Components::TerrainFlattenEffect>(roadCut);
    const auto* cutPaint = world.GetComponent<Components::TerrainPaintLayerEffect>(roadCut);
    ASSERT_NE(cutVolume, nullptr);
    ASSERT_NE(cutFlatten, nullptr);
    ASSERT_NE(cutPaint, nullptr);
    EXPECT_EQ(cutVolume->Shape, Components::TerrainVolumeShape::SplineArea);
    EXPECT_FLOAT_EQ(cutVolume->Falloff, 7.5f);
    EXPECT_FLOAT_EQ(cutVolume->Priority, 4.0f);
    EXPECT_TRUE(cutFlatten->UseVolumeHeight);
    EXPECT_FLOAT_EQ(cutFlatten->TargetHeight, 1.25f);
    EXPECT_EQ(cutPaint->LayerIndex, 1u);
    EXPECT_FLOAT_EQ(cutPaint->Strength, 0.45f);
    // Spline painting replaced the layer so grass masks could exclude the path.
    EXPECT_TRUE(cutPaint->Replace);

    // flatten = false is the other branch: a height offset, and offset mode
    // always accumulated whatever the block's blend said.
    const auto roadBerm = FindByTag(world, "road_berm");
    const auto* bermOffset = world.GetComponent<Components::TerrainHeightOffsetEffect>(roadBerm);
    ASSERT_NE(bermOffset, nullptr);
    EXPECT_FLOAT_EQ(bermOffset->Offset, -2.5f);
    EXPECT_EQ(bermOffset->Blend, Components::TerrainModifierBlend::Add);
    EXPECT_EQ(world.GetComponent<Components::TerrainFlattenEffect>(roadBerm), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainPaintLayerEffect>(roadBerm), nullptr);
}

// The conversion changes what a re-save writes, and a scene nobody re-saves
// converts again on every load — so it reports, once per scene file.
TEST(SceneIO, LoadLegacyTerrainModifierScene_ReportsTheMigration)
{
    // Its own copy: the warn-once set is keyed on the scene path and lives for
    // the process, so loading the staged fixture here would read whichever test
    // ran first rather than the warning.
    const auto scenePath = CopySceneToTemp(LegacyModifierFixturePath(), "legacy_modifiers_warn.scene");
    ASSERT_FALSE(scenePath.empty());
    Scene::EnsureTerrainSceneSchemasRegistered();

    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    // Positive control on the instrument before any zero is read out of it.
    Logger::Log::Warning("SceneIO legacy-modifier capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(CountLinesContaining(logLines, "SceneIO legacy-modifier capture self test"), 1u)
        << "the log capture is not receiving engine warnings";
    logLines.clear();

    ECS::World world;
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    opts.assetRootOverride = scenePath.parent_path();
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();

    EXPECT_EQ(CountLinesContaining(logLines, "retired pre-volume terrain modifier"), 1u)
        << "seven legacy blocks across seven entities must report once for the scene, not once "
           "each and not never";
    EXPECT_NE(FirstLineContaining(logLines, "retired pre-volume terrain modifier")
                  .find(scenePath.string()),
              std::string::npos)
        << "the report must name the scene, which is what points at the file to re-save";
}

// The real-content counterpart to the fixture above: an authored project scene,
// saved in the volume model, so it neither carries a retired block nor converts
// anything on load. A retirement whose warning fires on authored content is a
// warning people learn to ignore, and a block left in a saved scene is a
// conversion running on every load forever.
TEST(SceneIO, AuthoredProjectScenesCarryNoRetiredTerrainModifierBlock)
{
    const auto scenesDir = ResolveStagedFixturePath("Tests/Projects/Biggles/Assets/Scenes");
    ASSERT_TRUE(std::filesystem::is_directory(scenesDir)) << scenesDir.string();

    std::vector<std::filesystem::path> scenes;
    for (const auto& entry : std::filesystem::directory_iterator(scenesDir))
        if (entry.is_regular_file() && entry.path().extension() == ".scene")
            scenes.push_back(entry.path());
    ASSERT_FALSE(scenes.empty()) << "no staged project scene to check — " << scenesDir.string();

    for (const auto& scene : scenes)
    {
        std::string text;
        ASSERT_TRUE(ReadFile(scene, text)) << scene.string();
        for (const char* blockName : kRetiredModifierBlockNames)
            EXPECT_EQ(text.find(blockName), std::string::npos)
                << scene.filename().string() << " still carries " << blockName
                << " — load it in the editor and re-save it";
    }

    // The load half. A file grep says nothing about what the loader does with
    // the scene; this is the assertion that the retirement stays quiet on it.
    const auto landscape = CopySceneToTemp(scenesDir / "Lanscape.scene", "biggles_quiet.scene");
    ASSERT_FALSE(landscape.empty());
    Scene::EnsureTerrainSceneSchemasRegistered();

    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    Logger::Log::Warning("SceneIO project-scene capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(CountLinesContaining(logLines, "SceneIO project-scene capture self test"), 1u)
        << "the log capture is not receiving engine warnings — the zero below would be false";
    logLines.clear();

    ECS::World world;
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    opts.assetRootOverride = landscape.parent_path();
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, landscape, opts))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();

    EXPECT_EQ(CountLinesContaining(logLines, "retired pre-volume terrain modifier"), 0u)
        << FirstLineContaining(logLines, "retired pre-volume terrain modifier");

    // ... and the terrain the re-save was supposed to preserve is still authored:
    // a clean-up that dropped the modifier would also pass every check above.
    const auto flatMiddle = FindByTag(world, "flat_middle");
    ASSERT_TRUE(flatMiddle.IsValid());
    const auto* volume = world.GetComponent<Components::TerrainModifierVolume>(flatMiddle);
    ASSERT_NE(volume, nullptr);
    EXPECT_EQ(volume->Shape, Components::TerrainVolumeShape::Circle);
    EXPECT_FLOAT_EQ(volume->Radius, 75.0f);
    EXPECT_FLOAT_EQ(volume->Falloff, 100.0f);

    const auto* flatten = world.GetComponent<Components::TerrainFlattenEffect>(flatMiddle);
    const auto* noise = world.GetComponent<Components::TerrainNoiseEffect>(flatMiddle);
    ASSERT_NE(flatten, nullptr);
    ASSERT_NE(noise, nullptr);
    EXPECT_TRUE(flatten->UseVolumeHeight);
    EXPECT_FLOAT_EQ(noise->Amplitude, 5.0f);
    EXPECT_LT(flatten->StackOrder, noise->StackOrder);
    EXPECT_FLOAT_EQ(noise->ErosionStrength, 0.0f);
}

TEST(SceneIO, SaveThenLoad_RoundTripTerrainNoiseErosionFields)
{
    const auto scenePath = MakeTempPath("terrain_noise_erosion_roundtrip.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'e', 'r', 'o', 'd', 'e', '\0'}});

    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Circle;
    volume.Radius = 60.0f;
    w1.AddComponentImmediate(e, volume);

    Components::TerrainNoiseEffect noise{};
    noise.Frequency = 6.5f;
    noise.Amplitude = 14.0f;
    // Every erosion field set to something distinct from its default, so a field
    // dropped from either the writer or the reader shows up as a mismatch rather
    // than as a default that happens to agree.
    noise.ErosionStrength = 0.85f;
    noise.ErosionOctaves = 6u;
    noise.ErosionFrequency = 3.25f;
    noise.ErosionDetail = 0.4f;
    noise.ErosionGullyWeight = 0.7f;
    noise.ErosionEdgeRounding = 0.15f;
    noise.ErosionFade = 0.55f;
    w1.AddComponentImmediate(e, noise);

    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("TerrainNoiseEffect.erosionStrength = "), std::string::npos);
    EXPECT_NE(saved.find("TerrainNoiseEffect.erosionOctaves = 6"), std::string::npos);
    EXPECT_NE(saved.find("TerrainNoiseEffect.erosionEdgeRounding = "), std::string::npos);

    ECS::World w2;
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, opts));

    const auto loaded = FindByTag(w2, "erode");
    ASSERT_TRUE(loaded.IsValid());
    const auto* out = w2.GetComponent<Components::TerrainNoiseEffect>(loaded);
    ASSERT_NE(out, nullptr);
    EXPECT_FLOAT_EQ(out->Frequency, 6.5f);
    EXPECT_FLOAT_EQ(out->Amplitude, 14.0f);
    EXPECT_FLOAT_EQ(out->ErosionStrength, 0.85f);
    EXPECT_EQ(out->ErosionOctaves, 6u);
    EXPECT_FLOAT_EQ(out->ErosionFrequency, 3.25f);
    EXPECT_FLOAT_EQ(out->ErosionDetail, 0.4f);
    EXPECT_FLOAT_EQ(out->ErosionGullyWeight, 0.7f);
    EXPECT_FLOAT_EQ(out->ErosionEdgeRounding, 0.15f);
    EXPECT_FLOAT_EQ(out->ErosionFade, 0.55f);
}

// TerrainCorridorEffect is RETIRED — the corridor dissolved into a Flatten whose
// blend is Average plus the volume's own StationSpacing — but scenes written
// before that carry the old block, so the schema keeps the retired NAME and
// translates it on the way in.
//
// The failure this guards is SILENT LOSS ON SAVE, and it needs BOTH halves. A
// schema that simply stopped recognising the name would let those entities load
// without the effect and then be written back without it: a road network that
// quietly stops grading, with no error anywhere. So this asserts the fold on the
// way IN and then saves, and requires the new form on disk and no trace of the
// old one.
TEST(SceneIO, LoadRetiredTerrainCorridor_FoldsIntoAPooledFlattenAndSavesInTheNewForm)
{
    const auto scenePath = MakeTempPath("terrain_retired_corridor.scene");
    Scene::EnsureTerrainSceneSchemasRegistered();

    // Pre-dissolution scene text, with every corridor property set to something
    // distinct from BOTH components' defaults, so a property dropped by either
    // side of the fold shows up as a mismatch rather than as an agreeing default.
    // stationSpacing is the one that changes owner: it was the corridor's, and it
    // belongs to the volume now.
    const std::string src =
        "[scene name=\"RetiredCorridor\" version=1]\n"
        "\n"
        "[entity id=\"approach\"]\n"
        "Transform.position = (0, 0, 0)\n"
        "Transform.rotation = (0, 0, 0, 1)\n"
        "Transform.scale = (1, 1, 1)\n"
        "TerrainCorridorEffect.stackOrder = 3\n"
        "TerrainCorridorEffect.group = \"Approaches\"\n"
        "TerrainCorridorEffect.targetHeight = 2.5\n"
        "TerrainCorridorEffect.useRouteHeight = true\n"
        "TerrainCorridorEffect.respectClaims = false\n"
        "TerrainCorridorEffect.stationSpacing = 0.25\n"
        "TerrainModifierVolume.shape = 2\n"
        "TerrainModifierVolume.falloff = 4\n"
        "TerrainModifierVolume.priority = 7\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w1;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w1, scenePath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}));

    const auto loaded = FindByTag(w1, "approach");
    ASSERT_TRUE(loaded.IsValid());

    const auto* flatten = w1.GetComponent<Components::TerrainFlattenEffect>(loaded);
    ASSERT_NE(flatten, nullptr) << "the retired corridor did not fold into a flatten at all";
    EXPECT_EQ(flatten->Blend, Components::TerrainModifierBlend::Average)
        << "a corridor was always pooled, so the fold must pin Average — Set would make it "
           "overwrite its neighbours instead of averaging with them";
    EXPECT_EQ(Components::EffectPoolName(*flatten), std::string_view("Approaches"));
    EXPECT_TRUE(flatten->UseVolumeHeight) << "useRouteHeight maps onto useVolumeHeight";
    EXPECT_FLOAT_EQ(flatten->TargetHeight, 2.5f);
    EXPECT_FALSE(flatten->RespectClaims);
    EXPECT_EQ(flatten->StackOrder, 3);

    // Geometry moved to the volume, and the volume's own block must not have
    // clobbered it: saved scenes write component blocks in NAME order, so
    // TerrainCorridorEffect is applied before TerrainModifierVolume.
    const auto* volume = w1.GetComponent<Components::TerrainModifierVolume>(loaded);
    ASSERT_NE(volume, nullptr);
    EXPECT_FLOAT_EQ(volume->StationSpacing, 0.25f)
        << "the corridor's station spacing must land on the volume and survive the volume block "
           "that follows it";
    EXPECT_EQ(volume->Shape, Components::TerrainVolumeShape::SplinePath);
    EXPECT_FLOAT_EQ(volume->Falloff, 4.0f);
    EXPECT_FLOAT_EQ(volume->Priority, 7.0f);

    // SAVE. The retired schema serializes nothing; the flatten's own schema is
    // what writes the entity now.
    const auto savedPath = MakeTempPath("terrain_retired_corridor_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, savedPath, Scene::SaveOptions{}));

    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_EQ(saved.find("TerrainCorridorEffect"), std::string::npos)
        << "a re-saved scene must contain no trace of the retired component";
    EXPECT_NE(saved.find("TerrainFlattenEffect.poolGroup = \"Approaches\""), std::string::npos);
    EXPECT_NE(saved.find("TerrainFlattenEffect.blend = 8"), std::string::npos)
        << "Average is blend id 8; the route must be written back as a POOLED flatten";
    EXPECT_NE(saved.find("TerrainModifierVolume.stationSpacing = "), std::string::npos);

    // And the round trip closes: loading what was written reproduces the entity.
    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, savedPath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto reloaded = FindByTag(w2, "approach");
    ASSERT_TRUE(reloaded.IsValid());
    const auto* out = w2.GetComponent<Components::TerrainFlattenEffect>(reloaded);
    ASSERT_NE(out, nullptr) << "the route was lost on save — the failure this test exists for";
    EXPECT_EQ(out->Blend, Components::TerrainModifierBlend::Average);
    EXPECT_EQ(Components::EffectPoolName(*out), std::string_view("Approaches"));
    EXPECT_FLOAT_EQ(out->TargetHeight, 2.5f);
    EXPECT_TRUE(out->UseVolumeHeight);
    EXPECT_FALSE(out->RespectClaims);
    const auto* outVolume = w2.GetComponent<Components::TerrainModifierVolume>(reloaded);
    ASSERT_NE(outVolume, nullptr);
    EXPECT_FLOAT_EQ(outVolume->StationSpacing, 0.25f);
}

// An entity carrying BOTH a retired TerrainCorridorEffect and a
// TerrainFlattenEffect is a conflict one component of a type per entity cannot
// express, and the documented resolution is that the ROUTE wins, loudly. That
// outcome must not depend on which block the file lists first: a saved scene
// writes blocks in name order (corridor before flatten), a hand-edited one can
// order them either way, and a resolution that flips with the order would make
// the load warning a lie in one of the two.
namespace
{
void ExpectRouteWinsOverAuthoredFlatten(const char* sceneName, const std::string& blocks)
{
    const auto scenePath = MakeTempPath(sceneName);
    Scene::EnsureTerrainSceneSchemasRegistered();
    const std::string src =
        "[scene name=\"CorridorPlusFlatten\" version=1]\n"
        "\n"
        "[entity id=\"clash\"]\n"
        "Transform.position = (0, 0, 0)\n"
        "Transform.rotation = (0, 0, 0, 1)\n"
        "Transform.scale = (1, 1, 1)\n"
        + blocks +
        "TerrainModifierVolume.shape = 2\n"
        "TerrainModifierVolume.priority = 7\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto loaded = FindByTag(w, "clash");
    ASSERT_TRUE(loaded.IsValid());

    const auto* flatten = w.GetComponent<Components::TerrainFlattenEffect>(loaded);
    ASSERT_NE(flatten, nullptr);
    EXPECT_EQ(flatten->Blend, Components::TerrainModifierBlend::Average)
        << "the route's pooled fold must win the slot";
    EXPECT_EQ(Components::EffectPoolName(*flatten), std::string_view("Approaches"))
        << "the surviving component must be the ROUTE's, not the authored flatten";
    EXPECT_FLOAT_EQ(flatten->TargetHeight, 2.5f)
        << "the authored flatten's target height must be dropped, as the load warning says";
    EXPECT_FALSE(flatten->RespectClaims) << "the corridor's authored respectClaims must survive";
}
} // namespace

TEST(SceneIO, RetiredCorridorPlusFlatten_RouteWinsWhenTheCorridorBlockComesFirst)
{
    // Name order — what every saved scene contains.
    ExpectRouteWinsOverAuthoredFlatten(
        "terrain_corridor_flatten_nameorder.scene",
        "TerrainCorridorEffect.group = \"Approaches\"\n"
        "TerrainCorridorEffect.targetHeight = 2.5\n"
        "TerrainCorridorEffect.respectClaims = false\n"
        "TerrainCorridorEffect.stationSpacing = 0.25\n"
        "TerrainFlattenEffect.blend = 0\n"
        "TerrainFlattenEffect.targetHeight = 99\n"
        "TerrainFlattenEffect.poolGroup = \"HandAuthored\"\n"
        "TerrainFlattenEffect.respectClaims = true\n");
}

TEST(SceneIO, RetiredCorridorPlusFlatten_RouteWinsWhenTheFlattenBlockComesFirst)
{
    // Hand-edited order: the flatten block precedes the corridor's.
    ExpectRouteWinsOverAuthoredFlatten(
        "terrain_corridor_flatten_handorder.scene",
        "TerrainFlattenEffect.blend = 0\n"
        "TerrainFlattenEffect.targetHeight = 99\n"
        "TerrainFlattenEffect.poolGroup = \"HandAuthored\"\n"
        "TerrainFlattenEffect.respectClaims = true\n"
        "TerrainCorridorEffect.group = \"Approaches\"\n"
        "TerrainCorridorEffect.targetHeight = 2.5\n"
        "TerrainCorridorEffect.respectClaims = false\n"
        "TerrainCorridorEffect.stationSpacing = 0.25\n");
}

// respectClaims round-trips on a NON-pooled blend.
//
// The flag is read on every blend mode, so the schema must carry it on every
// blend mode too. It always parsed and always serialized — the schema never
// gated it on Average — and this pins that, because the combination is now
// load-bearing rather than inert and a "tidy-up" that dropped the line off the
// non-pooled path would silently disarm every claim-respecting region that does
// not pool.
TEST(SceneIO, TerrainFlattenEffect_RespectClaimsRoundTripsOnANonPooledBlend)
{
    const auto scenePath = MakeTempPath("terrain_flatten_claims_unpooled.scene");
    const std::string src =
        "[scene name=\"UnpooledClaimant\" version=1]\n"
        "\n"
        "[entity id=\"cutting\"]\n"
        "Transform.position = (0, 0, 0)\n"
        "Transform.rotation = (0, 0, 0, 1)\n"
        "Transform.scale = (1, 1, 1)\n"
        "TerrainFlattenEffect.blend = 4\n"          // Min, the CUT operator — not Average
        "TerrainFlattenEffect.targetHeight = 12.5\n"
        "TerrainFlattenEffect.useVolumeHeight = false\n"
        "TerrainFlattenEffect.respectClaims = true\n"
        "TerrainModifierVolume.shape = 0\n"
        "TerrainModifierVolume.priority = 3\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w1;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w1, scenePath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto loaded = FindByTag(w1, "cutting");
    ASSERT_TRUE(loaded.IsValid());
    const auto* flatten = w1.GetComponent<Components::TerrainFlattenEffect>(loaded);
    ASSERT_NE(flatten, nullptr);
    EXPECT_EQ(flatten->Blend, Components::TerrainModifierBlend::Min)
        << "the authored operator must survive: Average would make this a different feature";
    EXPECT_TRUE(flatten->RespectClaims)
        << "respectClaims must load on a non-pooled blend — the combination the bake now honours";

    const auto savedPath = MakeTempPath("terrain_flatten_claims_unpooled_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, savedPath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_NE(saved.find("TerrainFlattenEffect.respectClaims = true"), std::string::npos)
        << "the flag must be WRITTEN on a non-pooled blend, or a save silently disarms the region";
    EXPECT_NE(saved.find("TerrainFlattenEffect.blend = 4"), std::string::npos);

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, savedPath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto reloaded = FindByTag(w2, "cutting");
    ASSERT_TRUE(reloaded.IsValid());
    const auto* out = w2.GetComponent<Components::TerrainFlattenEffect>(reloaded);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->Blend, Components::TerrainModifierBlend::Min);
    EXPECT_TRUE(out->RespectClaims);
    EXPECT_FLOAT_EQ(out->TargetHeight, 12.5f);
}

// respectClaims round-trips on the two SPLAT effects.
//
// Same field, same semantics as the height effects carry, and it decides whether
// claimed ground keeps the material its claimant painted. Both are pinned in one
// test because they must not drift apart: a rules effect IS a paint write whose
// strength came from its conditions.
TEST(SceneIO, TerrainSplatEffects_RespectClaimsRoundTrips)
{
    const auto scenePath = MakeTempPath("terrain_splat_claims.scene");
    const std::string src =
        "[scene name=\"SplatClaims\" version=1]\n"
        "\n"
        "[entity id=\"surface\"]\n"
        "Transform.position = (0, 0, 0)\n"
        "Transform.rotation = (0, 0, 0, 1)\n"
        "Transform.scale = (1, 1, 1)\n"
        "TerrainPaintLayerEffect.layerIndex = 2\n"
        "TerrainPaintLayerEffect.strength = 0.75\n"
        "TerrainPaintLayerEffect.replace = true\n"
        "TerrainPaintLayerEffect.respectClaims = true\n"
        "TerrainSurfaceRulesEffect.ruleCount = 1\n"
        "TerrainSurfaceRulesEffect.respectClaims = true\n"
        "TerrainSurfaceRulesEffect.rule0.material = 3\n"
        "TerrainSurfaceRulesEffect.rule0.strength = 1\n"
        "TerrainSurfaceRulesEffect.rule0.replace = false\n"
        "TerrainSurfaceRulesEffect.rule0.conditionCount = 0\n"
        "TerrainModifierVolume.shape = 0\n"
        "TerrainModifierVolume.priority = 5\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World w1;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w1, scenePath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto loaded = FindByTag(w1, "surface");
    ASSERT_TRUE(loaded.IsValid());

    const auto* paint = w1.GetComponent<Components::TerrainPaintLayerEffect>(loaded);
    ASSERT_NE(paint, nullptr);
    EXPECT_TRUE(paint->RespectClaims);
    EXPECT_EQ(paint->LayerIndex, 2u);

    const auto* rules = w1.GetComponent<Components::TerrainSurfaceRulesEffect>(loaded);
    ASSERT_NE(rules, nullptr);
    EXPECT_TRUE(rules->RespectClaims);
    EXPECT_EQ(rules->RuleCount, 1u);

    const auto savedPath = MakeTempPath("terrain_splat_claims_saved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, savedPath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(savedPath, saved));
    EXPECT_NE(saved.find("TerrainPaintLayerEffect.respectClaims = true"), std::string::npos)
        << "a save that drops the flag silently disarms every claim-respecting paint stroke";
    EXPECT_NE(saved.find("TerrainSurfaceRulesEffect.respectClaims = true"), std::string::npos);

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, savedPath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto reloaded = FindByTag(w2, "surface");
    ASSERT_TRUE(reloaded.IsValid());
    const auto* outPaint = w2.GetComponent<Components::TerrainPaintLayerEffect>(reloaded);
    ASSERT_NE(outPaint, nullptr);
    EXPECT_TRUE(outPaint->RespectClaims);
    EXPECT_FLOAT_EQ(outPaint->Strength, 0.75f);
    const auto* outRules = w2.GetComponent<Components::TerrainSurfaceRulesEffect>(reloaded);
    ASSERT_NE(outRules, nullptr);
    EXPECT_TRUE(outRules->RespectClaims);
    EXPECT_EQ(outRules->RuleCount, 1u);
}

// Regression test for the case-mismatch silent-failure bug: schemas that
// override EnumerateAssetReferences MUST emit property names in the same
// case ApplyProperty accepts (lowercase, since SceneIO case-folds property
// dispatch). The Missing Assets panel's "Remove from scene" action feeds
// the visitor's prop name straight into ApplyProperty(prop, "0", ...) — if
// the case doesn't match, the slot silently isn't cleared.
TEST(SceneIO_SchemaContract, EnumerateVisitorPropertyNamesMatchApplyProperty)
{
    ECS::World w;
    auto e = w.CreateEntity();
    Components::SceneEntityTag tag{};
    std::strcpy(tag.value, "ent");
    w.AddComponentImmediate(e, tag);

    // Seed an entity with a real asset reference via the schema's own
    // ApplyProperty so we know the slot is initially populated.
    const auto* schema = Scene::SceneSchemaRegistry::Find("MatUserV2");
    ASSERT_NE(schema, nullptr);
    const GUID seededGuid = GUID::Derive(GUID::Null(), "round-trip-seed");
    const std::string seedValue = std::string("[guid=\"") + seededGuid.ToString() + "\"]";
    std::string err;
    Scene::SceneLoadContext ctx{};
    ASSERT_TRUE(schema->ApplyProperty(w, e, ctx, "mat", seedValue, &err)) << err;

    auto* mu = w.GetComponent<MatUserV2>(e);
    ASSERT_NE(mu, nullptr);
    ASSERT_NE(mu->guid[0], '\0') << "seed didn't populate the slot";

    // Walk the schema's references and feed each (prop, "0") back through
    // ApplyProperty. If the visitor emits a case the schema doesn't accept,
    // the call silently succeeds-without-clearing — caught by the post-check.
    bool visited = false;
    schema->EnumerateAssetReferences(w, e,
                                     [&](const GUID&, AssetType, std::string_view, std::string_view propertyName)
                                     {
                                         visited = true;
                                         std::string clearErr;
                                         EXPECT_TRUE(schema->ApplyProperty(w, e, ctx, propertyName, "0", &clearErr))
                                             << "ApplyProperty rejected '" << propertyName << "': " << clearErr;
                                     });
    EXPECT_TRUE(visited) << "EnumerateAssetReferences didn't visit the seeded slot";

    auto* after = w.GetComponent<MatUserV2>(e);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->guid[0], '\0')
        << "Slot still populated after ApplyProperty(prop, '0') — case-mismatch?";
}

// R1.2: pre-2026-07 scenes carry MeshRenderer.lodGroupId (dead data, removed)
// and LODGroup.lodCount/threshold* (thresholds moved to the MeshGPURegistry
// row). Loading such a scene must succeed, ignore the legacy keys, and still
// parse LODGroup.bias — the surviving per-entity knob.
TEST(SceneIO, LoadLegacyLODFields_AcceptedAndIgnored)
{
    const auto scenePath = MakeTempPath("lod_legacy.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'l', 'o', 'd', '\0'}});
    Components::MeshRenderer mr{};
    w1.AddComponentImmediate(e, mr);
    Components::LODGroup lg{};
    lg.Bias = 0.5f;
    w1.AddComponentImmediate(e, lg);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    // Inject the legacy lines a pre-R1.2 save would have written.
    {
        std::ifstream in(scenePath, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::istringstream iss(body);
        std::ostringstream oss;
        for (std::string line; std::getline(iss, line);)
        {
            oss << line << "\n";
            if (line.find("MeshRenderer.castShadows") != std::string::npos)
                oss << "MeshRenderer.lodGroupId = 7\n";
            if (line.find("LODGroup.bias") != std::string::npos)
            {
                oss << "LODGroup.lodCount = 3\n";
                oss << "LODGroup.threshold0 = 0.6\n";
                oss << "LODGroup.threshold1 = 0.3\n";
            }
        }
        std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
        out << oss.str();
    }

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "lod");
    ASSERT_TRUE(e2.IsValid());
    EXPECT_NE(w2.GetComponent<Components::MeshRenderer>(e2), nullptr);
    auto* lg2 = w2.GetComponent<Components::LODGroup>(e2);
    ASSERT_NE(lg2, nullptr);
    EXPECT_FLOAT_EQ(lg2->Bias, 0.5f);
}

// The submesh-by-name selector saves as the NAME, never as its hash: the name is
// what a future build can re-hash, so changing HashMeshName re-keys the binding
// instead of orphaning it.
TEST(SceneIO, MeshRenderer_MeshNameSavesAsStringAndRoundTrips)
{
    const auto scenePath = MakeTempPath("meshname_roundtrip.scene");

    const StringId nameId = Engine::Renderer::InternMeshName("Seat");
    ASSERT_NE(nameId, 0u);

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'m', 'n', '\0'}});
    Components::MeshRenderer mr{};
    mr.MeshNameId = nameId;
    w1.AddComponentImmediate(e, mr);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    std::string body;
    ASSERT_TRUE(ReadFile(scenePath, body));
    EXPECT_NE(body.find("MeshRenderer.meshName = \"Seat\""), std::string::npos);
    EXPECT_EQ(body.find("meshNameHash"), std::string::npos) << "the raw hash form must not be written";

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "mn");
    ASSERT_TRUE(e2.IsValid());
    auto* mr2 = w2.GetComponent<Components::MeshRenderer>(e2);
    ASSERT_NE(mr2, nullptr);
    EXPECT_EQ(mr2->MeshNameId, nameId);
}

// Scenes saved before the selector became a name carry `meshNameHash`. Loading
// one must still set the selector, so such a scene keeps binding for as long as
// HashMeshName still produces that number.
TEST(SceneIO, MeshRenderer_LegacyMeshNameHashStillLoads)
{
    const auto scenePath = MakeTempPath("meshname_legacy_hash.scene");

    // A pre-existing hash-form file, verbatim. The literal hash is what the
    // engine of the day wrote for "Seat"; the loader must take the number as
    // given and must not require it to match anything it can recompute.
    const StringId legacyId = Components::HashMeshName("Seat");
    ASSERT_NE(legacyId, 0u);
    const std::string body =
        "[scene name=\"legacy\" version=1]\n"
        "\n"
        "[entity id=\"mnlegacy\"]\n"
        "MeshRenderer.meshId = 0\n"
        "MeshRenderer.meshNameHash = " + std::to_string(legacyId) + "\n"
        "MeshRenderer.enabled = true\n";
    ASSERT_TRUE(WriteFile(scenePath, body));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e = FindByTag(w, "mnlegacy");
    ASSERT_TRUE(e.IsValid());
    auto* mr = w.GetComponent<Components::MeshRenderer>(e);
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->MeshNameId, legacyId);
}

// A hash-form selector that is valid but has not bound yet (a save before
// SceneBuildPump reaches the entity, or with the model missing) has no name to
// write. The save must keep the selector in the form the loader reads rather
// than drop it: dropping it binds submesh 0 forever with only a log line.
TEST(SceneIO, MeshRenderer_ValidHashSelectorSurvivesSaveBeforeBinding)
{
    const auto scenePath = MakeTempPath("meshname_hash_unbound.scene");

    // Never interned in this process: the id only ever arrives as a number.
    constexpr const char* kNeverBoundName = "SM_NeverBound_Unique_7731";
    const StringId id = Components::HashMeshName(kNeverBoundName);
    ASSERT_TRUE(Engine::Renderer::FindMeshName(id).empty()) << "precondition: no name known";
    const std::string body =
        "[scene name=\"unbound\" version=1]\n"
        "\n"
        "[entity id=\"mnunbound\"]\n"
        "MeshRenderer.meshId = 0\n"
        "MeshRenderer.meshNameHash = " + std::to_string(id) + "\n"
        "MeshRenderer.enabled = true\n";
    ASSERT_TRUE(WriteFile(scenePath, body));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e = FindByTag(w, "mnunbound");
    ASSERT_TRUE(e.IsValid());
    ASSERT_EQ(w.GetComponent<Components::MeshRenderer>(e)->MeshNameId, id);

    const auto resavedPath = MakeTempPath("meshname_hash_unbound_resaved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w, resavedPath, Scene::SaveOptions{}));
    std::string resaved;
    ASSERT_TRUE(ReadFile(resavedPath, resaved));
    EXPECT_NE(resaved.find("MeshRenderer.meshNameHash = " + std::to_string(id)), std::string::npos)
        << "an unbound selector is written back as the hash it arrived as:\n" << resaved;

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, resavedPath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "mnunbound");
    ASSERT_TRUE(e2.IsValid());
    EXPECT_EQ(w2.GetComponent<Components::MeshRenderer>(e2)->MeshNameId, id)
        << "the selector survives load/save/load with no bind in between";
}

// The #1671 case. A scene authored before HashMeshName folded an interior
// "_LOD<N>" token stored the name `..._LOD1_0`. Because the name — not a hash —
// is what the file carries, the load re-derives the id with today's function and
// it equals the id of every other level's spelling of the same part. A file that
// had stored the PRE-FOLD hash instead would carry a number today's function
// never produces, which is the orphaning this form removes.
TEST(SceneIO, MeshRenderer_PreFoldMeshNameRehashesToTodaysId)
{
    const auto scenePath = MakeTempPath("meshname_prefold.scene");

    constexpr const char* kAuthoredBeforeFold = "SM_Farm_GenTree_A_FullTree_01_LOD1_0";
    const std::string body =
        "[scene name=\"prefold\" version=1]\n"
        "\n"
        "[entity id=\"mnfold\"]\n"
        "MeshRenderer.meshId = 0\n"
        "MeshRenderer.meshName = \"" + std::string(kAuthoredBeforeFold) + "\"\n"
        "MeshRenderer.enabled = true\n";
    ASSERT_TRUE(WriteFile(scenePath, body));

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e = FindByTag(w, "mnfold");
    ASSERT_TRUE(e.IsValid());
    auto* mr = w.GetComponent<Components::MeshRenderer>(e);
    ASSERT_NE(mr, nullptr);

    // Derived at load with the current function...
    EXPECT_EQ(mr->MeshNameId, Components::HashMeshName(kAuthoredBeforeFold));
    // ...which, post-fold, is the same id the LOD0 spelling of that part yields.
    EXPECT_EQ(mr->MeshNameId, Components::HashMeshName("SM_Farm_GenTree_A_FullTree_01_LOD0_0"));

    // Saving writes the name back, so the next hash change re-keys it again.
    const auto resavedPath = MakeTempPath("meshname_prefold_resaved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(w, resavedPath, Scene::SaveOptions{}));
    std::string resaved;
    ASSERT_TRUE(ReadFile(resavedPath, resaved));
    EXPECT_NE(resaved.find("MeshRenderer.meshName = \""), std::string::npos);
    EXPECT_EQ(resaved.find("meshNameHash"), std::string::npos);
}

// `MeshRenderer.meshName = "..."` (the importer-authored form) hashes into
// MeshNameId with ASCII case folded, so importer/DCC casing never blocks a
// submesh match.
TEST(SceneIO, MeshRenderer_MeshNameAuthoredCaseFolded)
{
    const auto scenePath = MakeTempPath("meshname_authored.scene");

    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    ASSERT_TRUE(e.IsValid());
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'m', 'n', '2', '\0'}});
    Components::MeshRenderer mr{}; // MeshNameId left 0 so no selector line is written.
    w1.AddComponentImmediate(e, mr);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    // Inject the human-authored string form an importer emits.
    std::string body;
    ASSERT_TRUE(ReadFile(scenePath, body));
    std::istringstream iss(body);
    std::ostringstream oss;
    for (std::string line; std::getline(iss, line);)
    {
        oss << line << "\n";
        if (line.find("MeshRenderer.castShadows") != std::string::npos)
            oss << "MeshRenderer.meshName = \"SEAT\"\n";
    }
    ASSERT_TRUE(WriteFile(scenePath, oss.str()));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    auto e2 = FindByTag(w2, "mn2");
    ASSERT_TRUE(e2.IsValid());
    auto* mr2 = w2.GetComponent<Components::MeshRenderer>(e2);
    ASSERT_NE(mr2, nullptr);
    EXPECT_EQ(mr2->MeshNameId, Components::HashMeshName("seat"));
    EXPECT_EQ(Components::HashMeshName("SEAT"), Components::HashMeshName("seat"));
}

TEST(ProjectPackagesManifest, DisableCreatesMissingManifest)
{
    const std::filesystem::path projectRoot =
        MakeTempPath("missing_package_manifest_project");
    std::error_code ec;
    std::filesystem::remove_all(projectRoot, ec);
    const std::filesystem::path manifestFile =
        projectRoot / "Packages" / "manifest.json";

    std::string error;
    ASSERT_TRUE(SetPackageDisabledInProjectManifest(
        manifestFile, "film-simulation", true, error)) << error;
    EXPECT_TRUE(std::filesystem::is_regular_file(manifestFile));

    ProjectPackagesManifest manifest;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(manifestFile, manifest, error)) << error;
    EXPECT_EQ(manifest.Disabled.count("film-simulation"), 1u);

    ASSERT_TRUE(SetPackageDisabledInProjectManifest(
        manifestFile, "film-simulation", false, error)) << error;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(manifestFile, manifest, error)) << error;
    EXPECT_TRUE(manifest.Disabled.empty());
}

// Phase split of the main-thread load. The single "parse+instantiate" number a
// caller can measure from outside cannot say which phase dominates, and a
// delta-based reload can only ever skip the instantiate half — so the split is
// what makes that trade-off falsifiable rather than a matter of opinion.
TEST(SceneIO, LoadTimings_ReportEveryPhaseAndSumToTheWhole)
{
    const auto scenePath = MakeTempPath("timings.scene");
    std::string src = "[scene name=\"Timings\" version=1]\n";
    for (int i = 0; i < 200; ++i)
    {
        src += "\n[entity id=\"e_" + std::to_string(i) + "\"]\n";
        src += "Transform.position = (0, 0, 0)\n";
        src += "Transform.rotation = (0, 0, 0, 1)\n";
        src += "Transform.scale = (1, 1, 1)\n";
    }
    ASSERT_TRUE(WriteFile(scenePath, src));

    ECS::World world;
    Scene::SceneLoadTimings timings{};
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    opts.outTimings = &timings;

    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    const double wholeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // Every phase ran, so none may be left at its zero initializer, and none is
    // negative (which is what a mis-ordered mark would produce).
    EXPECT_GT(timings.ParseMs, 0.0);
    EXPECT_GT(timings.InstantiateMs, 0.0);
    EXPECT_GE(timings.ReadMs, 0.0);
    EXPECT_GE(timings.ValidateMs, 0.0);
    EXPECT_GE(timings.ClearMs, 0.0);

    // The phases partition the call: their sum cannot exceed what the caller
    // measured around it. This is the assertion that catches a phase double
    // counted or a mark left in the wrong place.
    const double sum = timings.ReadMs + timings.ParseMs + timings.ValidateMs +
                       timings.ClearMs + timings.InstantiateMs;
    EXPECT_LE(sum, wholeMs + 1.0) << "phases overlap or are double counted";
}

// An additive load never clears, so that phase must stay at zero rather than
// absorbing a neighbouring phase's time.
TEST(SceneIO, LoadTimings_AdditiveLoadReportsNoClearPhase)
{
    const auto scenePath = MakeTempPath("timings_additive.scene");
    ASSERT_TRUE(WriteFile(scenePath,
                          "[scene name=\"Add\" version=1]\n"
                          "\n[entity id=\"a\"]\n"
                          "Transform.position = (0, 0, 0)\n"));

    ECS::World world;
    Scene::SceneLoadTimings timings{};
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Additive;
    opts.outTimings = &timings;

    ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, opts));
    EXPECT_EQ(timings.ClearMs, 0.0);
    EXPECT_GT(timings.InstantiateMs, 0.0);
}

// A load that fails before the world is touched must not report phases it never
// reached — a stale nonzero there would be read as work that happened.
TEST(SceneIO, LoadTimings_FailedParseLeavesLaterPhasesZero)
{
    const auto scenePath = MakeTempPath("timings_bad.scene");
    ASSERT_TRUE(WriteFile(scenePath, "this is not a scene file\n[[[\n"));

    ECS::World world;
    Scene::SceneLoadTimings timings{};
    timings.InstantiateMs = 123.0; // must be reset, not left for the caller to read
    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    opts.outTimings = &timings;

    EXPECT_FALSE(Scene::LoadSceneFromFile(world, scenePath, opts));
    EXPECT_EQ(timings.InstantiateMs, 0.0);
    EXPECT_EQ(timings.ClearMs, 0.0);
}


// The sun-colour day keys are retired: the sun's colour is the atmosphere's transmittance now, and
// an authored ramp on top of it reddened the sky's own scattering a second time. The defect was
// WHERE the ramp landed, not that an artistic tint exists, so the values are not dropped — they
// convert onto SunTintKeys, which is applied once, at the source, ahead of the atmosphere.
//
// Only the DAY keys convert. Midnight/Dawn encoded absolute LEVELS (the old midnight default 0.0048
// WAS the night's brightness, which the moon's colour carries now), so reading them as multipliers
// would invert intent — and the tint no longer touches the moon at all. They are dropped, with the
// authored value named in the notice. For Midday/Sunset:
//   * a key holding the old default was never art direction, and becomes white (no tint);
//   * an authored key carries its value across.
// A scene written before the retirement must still load, with the keys named and shed on the next
// save, because the alternative is a tolerant loader carrying dead text forward forever.
//
// This test owns the SkyEnvironment.SunColor* keys within this binary. The warn-once set is a
// process static, so a second test loading these keys would silently turn the count assertions
// below into zero-vs-one noise; put new coverage for them here rather than in a sibling — which is
// why the authored-ramp half lives in this test body rather than in one of its own.
TEST(SceneIO, Load_RetiredSkySunColorKeysConvertToSunTintAndResaveClean)
{
    const auto scenePath = MakeTempPath("sky_sun_color_keys.scene");

    const std::string src =
        "[scene name=\"SunKeys\" version=1]\n"
        "\n"
        "[entity id=\"sky\"]\n"
        "SkyEnvironment.Enabled = true\n"
        "SkyEnvironment.TimeOfDayHours = 17\n"
        "SkyEnvironment.SunColorMidnight = (0.004776953, 0.005181517, 0.006048833)\n"
        "SkyEnvironment.SunColorDawn = (0.2746773, 0.14412847, 0.099898726)\n"
        "SkyEnvironment.SunColorMidday = (1, 1, 1)\n"
        "SkyEnvironment.SunColorSunset = (1, 0.1651322, 0.49102086)\n"
        "SkyEnvironment.IblIntensity = 0.75\n";
    ASSERT_TRUE(WriteFile(scenePath, src));

    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    // Positive control on the instrument. A capture that sees nothing is indistinguishable from a
    // notice that was never emitted, and the second reading is the one that would quietly pass a
    // broken retirement. Prove the sink is live BEFORE reading a zero out of it.
    Logger::Log::Warning("SceneIO sky log-capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(CountLinesContaining(logLines, "SceneIO sky log-capture self test"), 1u)
        << "the log capture is not receiving engine warnings — every count below would be a "
           "false zero";
    logLines.clear();

    ECS::World w;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();

    // One notice per retired key, each naming the key as the FILE spells it and saying what to do.
    std::string dump;
    for (const auto& l : logLines) dump += "\n  | " + l;
    EXPECT_EQ(CountLinesContaining(logLines, "SkyEnvironment.SunColorSunset is retired"), 1u)
        << "captured " << logLines.size() << " line(s):" << dump;
    EXPECT_EQ(CountLinesContaining(logLines, "SkyEnvironment.SunColorMidnight is retired"), 1u);
    EXPECT_EQ(CountLinesContaining(logLines, "SkyEnvironment.SunColorDawn is retired"), 1u);
    EXPECT_EQ(CountLinesContaining(logLines, "SkyEnvironment.SunColorMidday is retired"), 1u);
    const std::string notice = FirstLineContaining(logLines, "SkyEnvironment.SunColorSunset is retired");
    EXPECT_NE(notice.find("converts to white"), std::string::npos) << notice;
    EXPECT_NE(notice.find("never art direction"), std::string::npos) << notice;
    EXPECT_NE(notice.find("Re-save"), std::string::npos) << notice;
    EXPECT_NE(notice.find("transmittance"), std::string::npos) << notice;
    EXPECT_NE(notice.find("SunTint"), std::string::npos) << notice;

    // The properties that still exist survived the load.
    const auto loadedEntity = FindByTag(w, "sky");
    ASSERT_TRUE(loadedEntity.IsValid());
    const auto* sky = w.GetComponent<Components::SkyEnvironment>(loadedEntity);
    ASSERT_NE(sky, nullptr);
    EXPECT_TRUE(ECS::Entity(&w, loadedEntity).IsEnabled<Components::SkyEnvironment>());
    EXPECT_FLOAT_EQ(sky->TimeOfDayHours, 17.0f);
    EXPECT_FLOAT_EQ(sky->IblIntensity, 0.75f);

    // THE DEFAULT RAMP WAS NEVER ART DIRECTION. Every key held the retired default, so every key
    // converts to white and the scene renders the physical sky. This is the case the twelve scenes
    // migrated in this branch were all in.
    const float* defaultRampKeys[4] = {sky->SunTintKeys.Midnight, sky->SunTintKeys.Dawn,
                                       sky->SunTintKeys.Midday, sky->SunTintKeys.Sunset};
    for (const float* key : defaultRampKeys)
        for (int c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ(key[c], 1.0f) << "default ramp key must convert to white";

    // AN AUTHORED RAMP KEEPS ITS INTENT. Only Sunset was moved off the default here, so only Sunset
    // carries across; the three untouched keys become white rather than dragging the default's
    // near-black midnight onto a tint that would multiply an already-dark moon.
    //
    // No log assertions on this load: the warn-once set is a process static and the keys above
    // already consumed it. The values are the contract; the notice is covered above.
    const auto authoredPath = MakeTempPath("sky_sun_color_keys_authored.scene");
    const std::string authoredSrc =
        "[scene name=\"SunKeysAuthored\" version=1]\n"
        "\n"
        "[entity id=\"sky\"]\n"
        "SkyEnvironment.Enabled = true\n"
        "SkyEnvironment.SunColorMidnight = (0.004776953, 0.005181517, 0.006048833)\n"
        "SkyEnvironment.SunColorDawn = (0.2746773, 0.14412847, 0.099898726)\n"
        "SkyEnvironment.SunColorMidday = (1, 1, 1)\n"
        "SkyEnvironment.SunColorSunset = (1, 0.62, 0.35)\n";
    ASSERT_TRUE(WriteFile(authoredPath, authoredSrc));

    ECS::World authoredWorld;
    ASSERT_TRUE(Scene::LoadSceneFromFile(authoredWorld, authoredPath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    const auto authoredEntity = FindByTag(authoredWorld, "sky");
    ASSERT_TRUE(authoredEntity.IsValid());
    const auto* authoredSky =
        authoredWorld.GetComponent<Components::SkyEnvironment>(authoredEntity);
    ASSERT_NE(authoredSky, nullptr);
    EXPECT_FLOAT_EQ(authoredSky->SunTintKeys.Sunset[0], 1.0f);
    EXPECT_FLOAT_EQ(authoredSky->SunTintKeys.Sunset[1], 0.62f);
    EXPECT_FLOAT_EQ(authoredSky->SunTintKeys.Sunset[2], 0.35f);
    const float* untouched[3] = {authoredSky->SunTintKeys.Midnight, authoredSky->SunTintKeys.Dawn,
                                 authoredSky->SunTintKeys.Midday};
    for (const float* key : untouched)
        for (int c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ(key[c], 1.0f) << "an untouched key must not carry the old default";

    // A night key that WAS authored is still dropped, not converted: its value was a level, and the
    // tint is a multiplier that no longer reaches the moon. Loading it must leave the tint white and
    // must not turn 42x brighter than the old default into a 5x darker night.
    const auto nightPath = MakeTempPath("sky_sun_color_keys_night.scene");
    const std::string nightSrc =
        "[scene name=\"SunKeysNight\" version=1]\n"
        "\n"
        "[entity id=\"sky\"]\n"
        "SkyEnvironment.Enabled = true\n"
        "SkyEnvironment.SunColorMidnight = (0.2, 0.2, 0.4)\n";
    ASSERT_TRUE(WriteFile(nightPath, nightSrc));
    ECS::World nightWorld;
    ASSERT_TRUE(Scene::LoadSceneFromFile(nightWorld, nightPath,
                                         Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    const auto nightEntity = FindByTag(nightWorld, "sky");
    ASSERT_TRUE(nightEntity.IsValid());
    const auto* nightSky = nightWorld.GetComponent<Components::SkyEnvironment>(nightEntity);
    ASSERT_NE(nightSky, nullptr);
    for (int c = 0; c < 3; ++c)
        EXPECT_FLOAT_EQ(nightSky->SunTintKeys.Midnight[c], 1.0f)
            << "an authored night LEVEL must be dropped, never reinterpreted as a multiplier";

    // Warn ONCE per process: a second load of the same scene is silent.
    logLines.clear();
    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}))
        << Scene::GetLastSceneIOError().message;
    Logger::Log::Flush();
    EXPECT_EQ(CountLinesContaining(logLines, "is retired"), 0u);

    // And the round trip is clean: re-saving sheds the retired name and stores the converted value
    // under the new one, so the file stops carrying dead text.
    const auto resavedPath = MakeTempPath("sky_sun_color_keys_resaved.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(authoredWorld, resavedPath, Scene::SaveOptions{}));
    std::string resaved;
    ASSERT_TRUE(ReadFile(resavedPath, resaved));
    EXPECT_EQ(resaved.find("SkyEnvironment.SunColor"), std::string::npos);
    EXPECT_NE(resaved.find("SkyEnvironment.SunTintSunset = (1, 0.62, 0.35)"), std::string::npos)
        << resaved.substr(0, 2000);
    EXPECT_NE(resaved.find("SkyEnvironment.SunTintMidday = (1, 1, 1)"), std::string::npos);
}

TEST(SceneIO, ParticleRendererAndBudgetRoundTrip)
{
    const auto scenePath = MakeTempPath("particle_rendering.scene");
    ECS::World source;
    const auto entity = source.CreateEntity();
    source.AddComponentImmediate(entity, Components::SceneEntityTag{{'f','x','\0'}});
    source.AddComponentImmediate(entity, Components::ParticleEmitter3D{});
    Components::ParticleRenderer renderer;
    renderer.Lighting = Components::ParticleLightingMode::SixWay;
    renderer.Columns = 8;
    renderer.Rows = 4;
    renderer.FrameCount = 30;
    renderer.FrameRate = 12.5f;
    renderer.StartFrame = 3.25f;
    renderer.Loop = false;
    renderer.BlendFrames = false;
    renderer.VelocityStretch = 0.75f;
    renderer.TrailFadeOverLength = true;
    renderer.ThinningStart = 20;
    renderer.ThinningEnd = 40;
    renderer.RenderLayerMask = 4;
    renderer.SixWayContrast = 1.5f;
    renderer.SixWayLayout = Components::ParticleSixWayLayout::TopLeftRightBottomBackFront;
    renderer.Billboard = Components::ParticleBillboard::FaceCameraYAlongVelocity;
    renderer.DrawOrder = Components::ParticleDrawOrder::ViewDepth;
    renderer.EmissionIntensity = 12.0f;
    renderer.EmissionColor = {0.8f, 0.4f, 0.1f, 0.75f};
    const GUID emission = GUID::Derive(GUID::Null(), "emission-particle-test");
    const GUID positive = GUID::Derive(GUID::Null(), "positive-particle-test");
    const GUID negative = GUID::Derive(GUID::Null(), "negative-particle-test");
    const GUID mesh = GUID::Derive(GUID::Null(), "particle/mesh1");
    renderer.EmissionTexture.Set(emission);
    renderer.SixWayMapA.Set(positive);
    renderer.SixWayMapB.Set(negative);
    renderer.Meshes[1].Set(mesh);
    source.AddComponentImmediate(entity, renderer);
    source.AddComponentImmediate(entity, Components::ParticleWorldSettings{1200});
    source.AddComponentImmediate(entity, Components::ParticlePlayback{});
    ASSERT_TRUE(Scene::SaveSceneToFile(source, scenePath, Scene::SaveOptions{}));
    std::string saved;
    ASSERT_TRUE(ReadFile(scenePath, saved));
    EXPECT_NE(saved.find("ParticleRenderer.SixWayLayout = TopLeftRightBottomBackFront"), std::string::npos) << saved;
    EXPECT_NE(saved.find("ParticleRenderer.Meshes1 = "), std::string::npos) << saved;
    EXPECT_EQ(saved.find("ParticleRenderer.Meshes0 = "), std::string::npos) << saved;
    EXPECT_EQ(saved.find("ParticlePlayback"), std::string::npos) << saved;

    ECS::World result;
    ASSERT_TRUE(Scene::LoadSceneFromFile(result, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto restored = FindByTag(result, "fx");
    ASSERT_TRUE(restored.IsValid());
    const auto* loaded = result.GetComponent<Components::ParticleRenderer>(restored);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(*loaded == renderer);
    ASSERT_NE(result.GetComponent<Components::ParticleWorldSettings>(restored), nullptr);
    EXPECT_EQ(result.GetComponent<Components::ParticleWorldSettings>(restored)->MaxParticles, 1200u);
    EXPECT_FALSE(result.HasComponent<Components::ParticlePlayback>(restored));
}

// A reflected component's asset references are listed by the keys its scene lines use, which is
// what lets the editor find a missing one and clear it.
TEST(SceneIO, ReflectedComponentsListTheirAssetReferencesByTheirSceneKeys)
{
    Scene::EnsureBuiltInSchemasRegistered();
    ECS::World world;
    const auto entity = world.CreateEntity();
    Components::ParticleRenderer renderer;
    const GUID texture = GUID::Derive(GUID::Null(), "references/texture");
    const GUID mesh = GUID::Derive(GUID::Null(), "references/mesh");
    renderer.Texture.Set(texture);
    renderer.Meshes[2].Set(mesh);
    world.AddComponentImmediate(entity, renderer);
    const auto* schema = Scene::SceneSchemaRegistry::ReflectionSchemaForUnhandledType(
        ECS::GetComponentTypeId<Components::ParticleRenderer>());
    ASSERT_NE(schema, nullptr);

    struct Reference
    {
        GUID Guid;
        AssetType Type;
        std::string Key;
    };
    std::vector<Reference> references;
    schema->EnumerateAssetReferences(world, entity,
                                     [&references](const GUID& guid, AssetType type, std::string_view, std::string_view key)
                                     { references.push_back({guid, type, std::string(key)}); });
    ASSERT_EQ(references.size(), 2u);
    EXPECT_EQ(references[0].Guid, texture);
    EXPECT_EQ(references[0].Type, AssetType::Texture);
    EXPECT_EQ(references[0].Key, "Texture");
    EXPECT_EQ(references[1].Guid, mesh);
    EXPECT_EQ(references[1].Type, AssetType::Model);
    EXPECT_EQ(references[1].Key, "Meshes2");

    Scene::SceneLoadContext context{};
    std::string error;
    ASSERT_TRUE(schema->ApplyProperty(world, entity, context, references[1].Key, "0", &error)) << error;
    const auto* cleared = world.GetComponent<Components::ParticleRenderer>(entity);
    EXPECT_TRUE(cleared->Meshes[2].IsNull());
    EXPECT_EQ(cleared->Texture.ToGuid(), texture);
}

// The web seed project's smoke emitter references a stack asset of the project by path; a path
// that names no stack leaves the emitter on the default stack, which shows a different effect
// without an error.
TEST(SceneIO, TheWebSeedScenesReferenceTheirSmokeStack)
{
    const auto project = ResolveStagedFixturePath("Tools/Web/smoke-project/Assets");
    const std::string reference = "ParticleEmitter3D.Stack = [path=\"Particles/Smoke.particlestack\"]";
    std::string text;
    ASSERT_TRUE(ReadFile(project / "Particles" / "Smoke.particlestack", text));
    Particles::StackDocument document;
    std::vector<Particles::StackDiagnostic> diagnostics;
    ASSERT_TRUE(Particles::ParseParticleStack(text, document, diagnostics))
        << (diagnostics.empty() ? "" : diagnostics.front().Message);
    ASSERT_EQ(document.Phases.size(), 1u);
    EXPECT_EQ(document.Phases.front().Label, "Smoke");
    EXPECT_EQ(document.Phases.front().Processors.size(), 9u);
    for (const char* sceneName : {"WebSmoke.scene", "PostFxAll.scene"})
    {
        std::string scene;
        ASSERT_TRUE(ReadFile(project / "Scenes" / sceneName, scene)) << sceneName;
        EXPECT_NE(scene.find(reference), std::string::npos) << sceneName;
    }
}

// The sky's drive set and its curve survive save and load: the colour toggle, the illuminance source,
// and a curve whose keys are Smooth with tangents, every field of every key. A file without these
// lines loads as the shipped sky; hand-edited values are brought into range: an unknown source is the
// light, an empty curve the default, a non-finite or negative illuminance 0 and a non-finite time
// dropped, and the two-field "time,value" form reads as Linear keys.
TEST(SceneIO, SkySunDriveRoundTripsAndSanitizes)
{
    const auto scenePath = MakeTempPath("sky_sun_drive.scene");
    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'s', 'k', 'y', '\0'}});
    Components::SkyEnvironment s{};
    s.DriveSunColor = false;
    s.SunIlluminanceSource = Components::SkySunIlluminanceSource::Curve;
    s.SunIlluminanceCurve = Math::Curve{};
    Math::CurveKey dawn{};
    dawn.Time = 6.25f;
    dawn.Value = 1234.5f;
    dawn.InTangent = 0.5f;
    dawn.OutTangent = -2.25f;
    dawn.Interp = Math::CurveInterp::Smooth;
    dawn.TangentMode = Math::CurveTangentMode::Broken;
    s.SunIlluminanceCurve.TryInsert(dawn);
    s.SunIlluminanceCurve.TryInsert(Math::CurveKey{12.0f, 61000.0f});
    w1.AddComponentImmediate(e, s);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));

    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto* s2 = w2.GetComponent<Components::SkyEnvironment>(FindByTag(w2, "sky"));
    ASSERT_NE(s2, nullptr);
    EXPECT_FALSE(s2->DriveSunColor);
    EXPECT_EQ(s2->SunIlluminanceSource, Components::SkySunIlluminanceSource::Curve);
    EXPECT_EQ(std::memcmp(&s2->SunIlluminanceCurve, &s.SunIlluminanceCurve, sizeof(Math::Curve)), 0)
        << "the curve lost a field of a key on the way through the file";

    struct Case
    {
        const char* Lines;
        Components::SkySunIlluminanceSource Source;
        std::vector<Math::CurveKey> Keys; // empty: the component's default curve
    };
    const Case cases[] = {
        {"", Components::SkySunIlluminanceSource::Light, {}},
        {"SkyEnvironment.SunIlluminanceSource = Moon\n", Components::SkySunIlluminanceSource::Light, {}},
        {"SkyEnvironment.SunIlluminanceSource = \"curve\"\nSkyEnvironment.SunIlluminanceCurve = \n",
         Components::SkySunIlluminanceSource::Curve, {}},
        {"SkyEnvironment.SunIlluminanceCurve = 1,1e39;2,-5;1e39,3;30,7\n", Components::SkySunIlluminanceSource::Light,
         {Math::CurveKey{1.0f, 0.0f}, Math::CurveKey{2.0f, 0.0f}, Math::CurveKey{std::nextafter(24.0f, 0.0f), 7.0f}}},
        {"SkyEnvironment.SunIlluminanceCurve = 6,100;12,1000\n", Components::SkySunIlluminanceSource::Light,
         {Math::CurveKey{6.0f, 100.0f}, Math::CurveKey{12.0f, 1000.0f}}},
    };
    for (const Case& c : cases)
    {
        const auto path = MakeTempPath("sky_sun_drive_sanitised.scene");
        ASSERT_TRUE(WriteFile(path, std::string("[scene name=\"Drive\" version=1]\n\n[entity id=\"sky\"]\n"
                                                "SkyEnvironment.Enabled = true\n") +
                                        c.Lines));
        ECS::World w;
        ASSERT_TRUE(Scene::LoadSceneFromFile(w, path, Scene::LoadOptions{Scene::LoadMode::Replace})) << c.Lines;
        const auto* sky = w.GetComponent<Components::SkyEnvironment>(FindByTag(w, "sky"));
        ASSERT_NE(sky, nullptr) << c.Lines;
        EXPECT_EQ(sky->SunIlluminanceSource, c.Source) << c.Lines;
        EXPECT_TRUE(sky->DriveSunColor) << c.Lines;
        if (c.Keys.empty())
        {
            EXPECT_TRUE(Components::SkySunDrive::IsDefaultSunIlluminanceCurve(sky->SunIlluminanceCurve)) << c.Lines;
            continue;
        }
        ASSERT_EQ(sky->SunIlluminanceCurve.KeyCount, c.Keys.size()) << c.Lines;
        for (size_t k = 0; k < c.Keys.size(); ++k)
        {
            EXPECT_EQ(sky->SunIlluminanceCurve.Keys[k].Time, c.Keys[k].Time) << c.Lines << " key " << k;
            EXPECT_EQ(sky->SunIlluminanceCurve.Keys[k].Value, c.Keys[k].Value) << c.Lines << " key " << k;
            EXPECT_EQ(sky->SunIlluminanceCurve.Keys[k].Interp, Math::CurveInterp::Linear) << c.Lines << " key " << k;
        }
    }
}

// Moonlight survives save and load; a file without it has the default, and a hand-edited value is
// brought into range: not finite reads the default, negative 0, above the ceiling the ceiling.
TEST(SceneIO, SkyMoonlightRoundTripsAndSanitizes)
{
    const auto scenePath = MakeTempPath("sky_moonlight.scene");
    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'s', 'k', 'y', '\0'}});
    Components::SkyEnvironment s{};
    s.MoonlightIlluminance = 0.25f;
    w1.AddComponentImmediate(e, s);
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    EXPECT_EQ(w2.GetComponent<Components::SkyEnvironment>(FindByTag(w2, "sky"))->MoonlightIlluminance, 0.25f);

    struct Case
    {
        const char* Lines;
        float Moonlight;
    };
    for (const Case c : {Case{"", Components::kDefaultMoonlightIlluminanceLux},
                         Case{"SkyEnvironment.MoonlightIlluminance = 1e39\n", Components::kDefaultMoonlightIlluminanceLux},
                         Case{"SkyEnvironment.MoonlightIlluminance = -1e39\n", Components::kDefaultMoonlightIlluminanceLux},
                         Case{"SkyEnvironment.MoonlightIlluminance = -3\n", 0.0f},
                         Case{"SkyEnvironment.MoonlightIlluminance = 25000\n", Components::kMoonlightIlluminanceMaxLux}})
    {
        const auto path = MakeTempPath("sky_moonlight_sanitised.scene");
        ASSERT_TRUE(WriteFile(path, std::string("[scene name=\"Moon\" version=1]\n\n[entity id=\"sky\"]\n"
                                                "SkyEnvironment.Enabled = true\n") +
                                        c.Lines));
        ECS::World w;
        ASSERT_TRUE(Scene::LoadSceneFromFile(w, path, Scene::LoadOptions{Scene::LoadMode::Replace})) << c.Lines;
        const auto* sky = w.GetComponent<Components::SkyEnvironment>(FindByTag(w, "sky"));
        ASSERT_NE(sky, nullptr) << c.Lines;
        EXPECT_EQ(sky->MoonlightIlluminance, c.Moonlight) << c.Lines;
    }
}

// A sun size that is not finite in the file is the physical sun, as the sky system reads it, not the
// range's limit an overflow would clamp to.
TEST(SceneIO, SkySunSizeRejectsNonFinite)
{
    for (const char* line : {"SkyEnvironment.SunSize = 1e39\n", "SkyEnvironment.SunSize = -1e39\n"})
    {
        const auto path = MakeTempPath("sky_sun_size.scene");
        ASSERT_TRUE(WriteFile(path, std::string("[scene name=\"Sun\" version=1]\n\n[entity id=\"sky\"]\n"
                                                "SkyEnvironment.Enabled = true\n") +
                                        line));
        ECS::World w;
        ASSERT_TRUE(Scene::LoadSceneFromFile(w, path, Scene::LoadOptions{Scene::LoadMode::Replace})) << line;
        const auto* sky = w.GetComponent<Components::SkyEnvironment>(FindByTag(w, "sky"));
        ASSERT_NE(sky, nullptr) << line;
        EXPECT_EQ(sky->SunSize, Components::SkyEnvironment{}.SunSize) << line;
    }
}

// An untouched sky saved and loaded still holds the default curve bit for bit, Smooth keys and all, so
// the first switch to the curve after a reopen seeds it from the scene's own path.
TEST(SceneIO, TheDefaultSunIlluminanceCurveSurvivesSaveAndLoad)
{
    const auto scenePath = MakeTempPath("sky_default_curve.scene");
    ECS::World w1;
    const ECS::EntityHandle e = w1.CreateEntity();
    w1.AddComponentImmediate(e, Components::SceneEntityTag{{'s', 'k', 'y', '\0'}});
    w1.AddComponentImmediate(e, Components::SkyEnvironment{});
    ASSERT_TRUE(Scene::SaveSceneToFile(w1, scenePath, Scene::SaveOptions{}));
    ECS::World w2;
    ASSERT_TRUE(Scene::LoadSceneFromFile(w2, scenePath, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const auto* s2 = w2.GetComponent<Components::SkyEnvironment>(FindByTag(w2, "sky"));
    ASSERT_NE(s2, nullptr);
    EXPECT_TRUE(Components::SkySunDrive::IsDefaultSunIlluminanceCurve(s2->SunIlluminanceCurve));
}
