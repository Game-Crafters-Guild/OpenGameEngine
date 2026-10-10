// Integration test for humanoid auto-bootstrap on real FBX fixtures.
//
// Validates the Phase 2a bootstrap wiring landed by closing the gap between
// AutoImportHumanoidRig (Phase 2a) and ModelAsset::PostLoad: importing a
// skinned model must produce its `<modelname>.humanoidrig.json` sidecar
// next to the source FBX, with `auto = true`. Subsequent imports against
// the same sidecar must NOT clobber a hand-edited (auto = false) file.
//
// Skip-gated: when the real Synty fixtures aren't present (e.g. CI without
// the Assets/Models/FBXTest/ payload), every test SUCCEED()s with a
// diagnostic. Locally on a developer machine the fixtures live where the
// repo's `Assets/Models/FBXTest/BusinessMale.fbx` lays them down.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"

#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidNameMatcher.h"
#include "Animation/HumanoidRig.h"
#include "Animation/SkeletonProfile.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Logger/Logger.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include "StagedTestPaths.h"
#include "Types/Types.h"

using namespace GameEngine;

namespace
{

constexpr const char* kBusinessMaleRel = "Assets/Models/FBXTest/BusinessMale.fbx";
constexpr const char* kWalkClipRel     = "Assets/Models/FBXTest/A_Walk_F_Masc.fbx";
constexpr const char* kKnightRel       = "Assets/Models/FBXTest/Knight_skinned.fbx";
constexpr const char* kBarrelRel       = "Assets/Models/FBXTest/Barrel_static.fbx";
constexpr const char* kSkeletonProfile = "Assets/SkeletonProfiles/HumanoidStandard.profile.json";

// Staged fixture mirror root (StageTestAssets); see StagedTestPaths.h.
std::filesystem::path StagedRoot()
{
    return TestPaths::StagedRoot();
}

std::filesystem::path SidecarFor(const std::filesystem::path& modelPath)
{
    auto p = modelPath;
    p.replace_extension();
    p += ".humanoidrig.json";
    return p;
}

// Set up an AssetManager that knows about the project + editor mounts so
// LoadRuntimeHumanoidProfile resolves the shared default during
// ModelAsset::PostLoad.
class HumanoidPostLoadFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_StagedRoot = StagedRoot();
        m_FixtureDir = m_StagedRoot / "Assets/Models/FBXTest";
        m_BusinessMale = m_StagedRoot / kBusinessMaleRel;
        m_WalkClip     = m_StagedRoot / kWalkClipRel;
        m_Knight       = m_StagedRoot / kKnightRel;
        m_Barrel       = m_StagedRoot / kBarrelRel;
        m_ProfilePath  = m_StagedRoot / kSkeletonProfile;

        std::error_code ec;
        m_HasFixtures = std::filesystem::exists(m_BusinessMale, ec) && !ec
                      && std::filesystem::exists(m_WalkClip,    ec) && !ec
                      && std::filesystem::exists(m_ProfilePath, ec) && !ec;
        if (!m_HasFixtures) return;

        // Wipe ANY prior sidecars (auto or hand) so each test starts from a
        // clean slate — tests that simulate user edits write auto=false and
        // should not bleed into siblings within the same exe run. Production
        // PostLoad guards against clobbering auto=false sidecars; this test
        // bypasses that with explicit removes.
        ForceDeleteSidecar(m_BusinessMale);
        ForceDeleteSidecar(m_WalkClip);
        ForceDeleteSidecar(m_Knight);
        ForceDeleteSidecar(m_Barrel);

        // Narrow project mount to just the fixture directory — scanning the
        // whole repo trips on non-ASCII filenames in vendored deps. The editor
        // mount targets the staged Assets root, where SkeletonProfiles/ holds
        // the shared default ModelAsset::PostLoad resolves.
        m_AssetManager = std::make_unique<AssetManager>();
        ASSERT_TRUE(m_AssetManager->Initialize());

        AssetSourceDesc projectSrc;
        projectSrc.Alias            = std::string(kAssetSourceAliasProject);
        projectSrc.Root             = m_FixtureDir;
        projectSrc.DerivedIdentity  = true;     // no DB needed for this test
        projectSrc.Priority         = 100;
        ASSERT_TRUE(m_AssetManager->RegisterSource(projectSrc));

        AssetSourceDesc editorSrc;
        editorSrc.Alias            = std::string(kAssetSourceAliasEditor);
        editorSrc.Root             = m_StagedRoot / "Assets";
        editorSrc.DerivedIdentity  = true;
        editorSrc.Priority         = 50;
        ASSERT_TRUE(m_AssetManager->RegisterSource(editorSrc));
    }

    void TearDown() override
    {
        if (m_AssetManager) m_AssetManager->Shutdown();
        // Leave generated sidecars in place — they are valid output and the
        // editor will pick them up on a future run. The next SetUp() wipes
        // them again.
    }

    // Trigger the asset-loading task path so ModelAsset::PostLoad runs in a
    // proper AssetManager-thread context (where GetThreadCurrent returns
    // m_AssetManager).
    SharedPtr<ModelAsset> LoadModelSync(const std::filesystem::path& abs)
    {
        const GUID guid = m_AssetManager->ResolveAssetGuid(abs);
        EXPECT_FALSE(guid.IsNull()) << "Failed to resolve GUID for " << abs.string();
        if (guid.IsNull()) return nullptr;
        auto fut = m_AssetManager->LoadAssetAsync(guid);
        auto asset = fut.get();
        return std::dynamic_pointer_cast<ModelAsset>(asset);
    }

    static void ForceDeleteSidecar(const std::filesystem::path& modelPath)
    {
        const auto sidecar = SidecarFor(modelPath);
        std::error_code ec;
        if (std::filesystem::exists(sidecar, ec) && !ec)
            std::filesystem::remove(sidecar, ec);
    }

    std::filesystem::path m_StagedRoot;
    std::filesystem::path m_FixtureDir;
    std::filesystem::path m_BusinessMale;
    std::filesystem::path m_WalkClip;
    std::filesystem::path m_Knight;
    std::filesystem::path m_Barrel;
    std::filesystem::path m_ProfilePath;
    bool m_HasFixtures = false;
    std::unique_ptr<AssetManager> m_AssetManager;
};

// Sanity: BusinessMale.fbx -> BusinessMale.humanoidrig.json with auto=true,
// and reloading the model leaves the sidecar's `auto = true` flag intact
// (overwrite is safe) but doesn't clobber a manual edit (auto = false).
TEST_F(HumanoidPostLoadFixture, BusinessMaleAutoGeneratesSidecar)
{
    if (!m_HasFixtures) {
        GTEST_SKIP() << "Synty FBX fixtures + HumanoidStandard profile not present; skipping.";
    }

    auto model = LoadModelSync(m_BusinessMale);
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->GetState(), AssetState::Loaded);

    const auto sidecar = SidecarFor(m_BusinessMale);
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::exists(sidecar, ec)) << "Sidecar not generated at " << sidecar.string();

    // Inspect the generated rig: auto flag set, BoneMap populated, profileRef present.
    Animation::HumanoidRig rig(GUID(), sidecar);
    ASSERT_TRUE(rig.Load());
    EXPECT_TRUE(rig.AutoGenerated()) << "Generated sidecar must carry auto=true";
    EXPECT_FALSE(rig.ProfileRef().IsNull()) << "ProfileRef must point at HumanoidStandard";
    EXPECT_GT(rig.BoneMap().size(), 0u);
    EXPECT_GT(rig.Chains().size(), 0u);
}

TEST_F(HumanoidPostLoadFixture, WalkClipAutoGeneratesSidecar)
{
    if (!m_HasFixtures) {
        GTEST_SKIP() << "Synty FBX fixtures + HumanoidStandard profile not present; skipping.";
    }

    auto model = LoadModelSync(m_WalkClip);
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->GetState(), AssetState::Loaded);

    const auto sidecar = SidecarFor(m_WalkClip);
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::exists(sidecar, ec)) << "Walk-clip sidecar missing at " << sidecar.string();

    Animation::HumanoidRig rig(GUID(), sidecar);
    ASSERT_TRUE(rig.Load());
    EXPECT_TRUE(rig.AutoGenerated());
    EXPECT_GT(rig.BoneMap().size(), 0u);
}

TEST_F(HumanoidPostLoadFixture, UserEditedSidecarIsPreserved)
{
    if (!m_HasFixtures) {
        GTEST_SKIP() << "Synty FBX fixtures + HumanoidStandard profile not present; skipping.";
    }

    // Initial load to bring the sidecar into existence.
    auto model = LoadModelSync(m_BusinessMale);
    ASSERT_NE(model, nullptr);
    const auto sidecar = SidecarFor(m_BusinessMale);
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::exists(sidecar, ec));

    // Simulate a user opening the rig in the inspector and saving.
    Animation::HumanoidRig rig(GUID(), sidecar);
    ASSERT_TRUE(rig.Load());
    rig.SetAutoGenerated(false); // user-saved
    ASSERT_TRUE(rig.SaveToPath(sidecar));

    // Capture file size as a proxy for content; the in-place schema is
    // identical for both auto and hand cases except for the flag itself.
    const auto sizeBefore = std::filesystem::file_size(sidecar, ec);

    // Reload the model and re-trigger PostLoad.
    auto reloaded = LoadModelSync(m_BusinessMale);
    ASSERT_NE(reloaded, nullptr);

    // Sidecar must still report auto=false; ModelAsset::PostLoad must not
    // overwrite it.
    Animation::HumanoidRig rig2(GUID(), sidecar);
    ASSERT_TRUE(rig2.Load());
    EXPECT_FALSE(rig2.AutoGenerated()) << "User-edited sidecar got clobbered by PostLoad";
    const auto sizeAfter = std::filesystem::file_size(sidecar, ec);
    EXPECT_EQ(sizeBefore, sizeAfter);
}

TEST_F(HumanoidPostLoadFixture, StaticPropDoesNotGenerateSidecar)
{
    if (!m_HasFixtures) {
        GTEST_SKIP() << "Synty FBX fixtures not present; skipping.";
    }
    std::error_code ec;
    if (!std::filesystem::exists(m_Barrel, ec)) {
        GTEST_SKIP() << "Barrel_static.fbx not present; skipping.";
    }

    auto model = LoadModelSync(m_Barrel);
    ASSERT_NE(model, nullptr);

    // A static (non-skinned) model has no skeleton; the coverage gate inside
    // AutoImportHumanoidRig fails and no sidecar is written. PostLoad
    // detects the early-out via SkeletonId == 0.
    const auto sidecar = SidecarFor(m_Barrel);
    EXPECT_FALSE(std::filesystem::exists(sidecar, ec))
        << "Static prop produced an unexpected humanoid sidecar at " << sidecar.string();
}

// A four-legged creature from the optional Assets/Models/FBXTest/ payload; empty when absent.
std::filesystem::path CreatureFbx(const char* fileName)
{
    const auto p = StagedRoot() / "Assets/Models/FBXTest" / fileName;
    std::error_code ec;
    if (std::filesystem::exists(p, ec) && !ec)
        return p;
    return {};
}

bool ReadAllBytes(const std::filesystem::path& path, Vector<uint8>& out)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const auto size = in.tellg();
    if (size <= 0)
        return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

TEST(HumanoidAutoImport, FourLeggedCreatureFbxFailsStanceGate)
{
    const auto profilePath = TestPaths::StagedRoot()
        / "Assets/SkeletonProfiles/HumanoidStandard.profile.json";
    std::error_code ec;
    if (!std::filesystem::exists(profilePath, ec) || ec)
        GTEST_SKIP() << "HumanoidStandard.profile.json not staged";

    Animation::SkeletonProfile profile(GUID(), profilePath);
    ASSERT_TRUE(profile.Load()) << profilePath.string();
    ASSERT_EQ(profile.GetState(), AssetState::Loaded);

    const char* fileNames[] = {
        "Sheep.fbx",
        "Wolf.fbx",
    };
    int ran = 0;
    for (const char* fileName : fileNames)
    {
        const auto path = CreatureFbx(fileName);
        if (path.empty())
            continue;
        Vector<uint8> bytes;
        ASSERT_TRUE(ReadAllBytes(path, bytes)) << path.string();
        ModelAsset model(GUID::Generate(), path);
        ASSERT_TRUE(model.LoadFromData(bytes)) << path.string();
        ASSERT_NE(model.GetSkeletonId(), 0u) << path.string();
        const auto* skel =
            ::GameEngine::Engine::Renderer::SkeletonStore::Instance().Get(model.GetSkeletonId());
        ASSERT_NE(skel, nullptr) << path.string();
        Animation::HumanoidNameMatcher matcher;
        EXPECT_GE(matcher.MatchSkeleton(skel->BoneNames).Coverage,
                  Animation::kAutoImportCoverageThreshold)
            << path.string();
        Animation::HumanoidRig rig(GUID(), std::filesystem::path{});
        EXPECT_FALSE(Animation::AutoImportHumanoidRig(*skel, profile, rig))
            << path.string();
        ++ran;
    }
    if (ran == 0)
        GTEST_SKIP() << "four-legged creature FBX not present";
}

} // namespace
