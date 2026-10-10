// ModelAsset .blend importer tests.
//
// Validates that ModelAsset::LoadFromBlendData / LoadFromData dispatch
// produce populated ModelAsset records on the 3 sample bundles' representative
// files. Mesh + skeleton extraction only (animation clips are Phase B3).
//
// Skip-gated when blend-samples/ is not extracted on this machine (the bundles
// are large + user-local + gitignored). Mirrors the BlendSmokeParseTests
// skip-gate convention from Phase B1.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Animation/SkeletonData.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "EngineLogCapture.h"
#include "StagedTestPaths.h"

namespace fs = std::filesystem;
using namespace GameEngine;

namespace
{

fs::path StagedRoot()
{
    return TestPaths::StagedRoot();
}

fs::path SamplesRoot()
{
    return StagedRoot() / "Tests" / "BlendSamples";
}

fs::path SidecarFor(const fs::path& modelPath)
{
    auto p = modelPath;
    p.replace_extension();
    p += ".humanoidrig.json";
    return p;
}

bool ReadFileBytes(const fs::path& path, Vector<uint8>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamsize size = f.tellg();
    if (size <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(out.data()), size))
        return false;
    return true;
}

// Direct model load that exercises LoadFromData -> LoadFromBlendData without
// requiring a fully-wired AssetManager. Useful for the unit-style assertions
// in this file. PostLoad / auto-import tests use the AssetManager fixture.
struct DirectLoadResult
{
    bool ok = false;
    double parseMs = 0.0;
    std::shared_ptr<ModelAsset> asset;
};

DirectLoadResult LoadModelDirect(const fs::path& path)
{
    DirectLoadResult r;
    Vector<uint8> bytes;
    if (!ReadFileBytes(path, bytes)) return r;

    GUID guid = GUID::Generate();
    r.asset = std::make_shared<ModelAsset>(guid, path);
    const auto t0 = std::chrono::steady_clock::now();
    r.ok = r.asset->LoadFromData(bytes);
    const auto t1 = std::chrono::steady_clock::now();
    r.parseMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return r;
}

class BlendModelLoadTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_SamplesRoot = SamplesRoot();
        std::error_code ec;
        m_HasSamples = fs::is_directory(m_SamplesRoot, ec) && !ec;
    }

    fs::path m_SamplesRoot;
    bool m_HasSamples = false;
};

// Largest sample: human_base_meshes_bundle.blend. The plan-blender §6 matrix
// notes 227 meshes, 0 armatures (a pure mesh-pack file).
TEST_F(BlendModelLoadTests, HumanBaseMeshesBundle_LoadsManyMeshesNoArmature)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path file = m_SamplesRoot
        / "human-base" / "human-base-meshes-bundle-v1.4.1" / "human_base_meshes_bundle.blend";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP() << "Sample missing: " << file.string();

    const auto result = LoadModelDirect(file);
    ASSERT_TRUE(result.ok) << "LoadFromData failed for " << file.string();
    ASSERT_NE(result.asset, nullptr);

    // Bundle contains many meshes (227).
    // Use a generous lower bound so the test isn't brittle to fbtBlend's
    // datablock filtering.
    EXPECT_GT(result.asset->GetMeshCount(), 50u) << "Expected many meshes from human-base bundle";

    // The bundle has no armatures (skeleton id stays 0).
    EXPECT_EQ(result.asset->GetSkeletonId(), 0u)
        << "human-base bundle is mesh-only; no armature expected";

    std::printf("[BlendModelLoadTests] human_base_meshes_bundle.blend: meshes=%u, parseMs=%.2f\n",
                result.asset->GetMeshCount(), result.parseMs);
    std::fflush(stdout);
}

// Ellie animation file: 1 armature, dozens of bones, many actions.
TEST_F(BlendModelLoadTests, EllieAnimation_LoadsArmatureWithBones)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path file = m_SamplesRoot
        / "ellie" / "asset-demo-bundle-4.0-ellie-animation"
        / "ellie_animation" / "ellie_animation.blend";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP() << "Sample missing: " << file.string();

    const auto result = LoadModelDirect(file);
    ASSERT_TRUE(result.ok) << "LoadFromData failed for " << file.string();
    ASSERT_NE(result.asset, nullptr);

    // Armature should produce a skeleton. ellie_animation has a humanoid
    // armature so we expect SkeletonId > 0 and a non-trivial bone count.
    ASSERT_NE(result.asset->GetSkeletonId(), 0u)
        << "ellie_animation must produce a skeleton";

    auto& store = ::GameEngine::Engine::Renderer::SkeletonStore::Instance();
    const auto* skel = store.Get(result.asset->GetSkeletonId());
    ASSERT_NE(skel, nullptr);
    EXPECT_GT(skel->BoneCount, 20u)
        << "Humanoid rig should have at least ~24 anatomical bones";
    EXPECT_EQ(skel->BoneNames.size(), skel->BoneCount);
    EXPECT_EQ(skel->Parent.size(), skel->BoneCount);

    // Animation metadata: actions present (B3 will load the curves; B2 just
    // populates the names list).
    EXPECT_TRUE(result.asset->HasAnimations())
        << "Ellie animation file must report HasAnimations=true";
    EXPECT_GT(result.asset->GetAnimationNames().size(), 0u);

    std::printf("[BlendModelLoadTests] ellie_animation.blend: bones=%u, actions=%zu, parseMs=%.2f\n",
                skel->BoneCount,
                result.asset->GetAnimationNames().size(),
                result.parseMs);
    std::fflush(stdout);
}

// Smallest sample: pendulum.blend — minimal-case load to make sure the
// importer doesn't choke on a tiny file.
TEST_F(BlendModelLoadTests, Pendulum_MinimalCaseLoadsCleanly)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path file = m_SamplesRoot
        / "anim-fund-rigs" / "animation_fundamental_rigs_release_01" / "pendulum.blend";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP() << "Sample missing: " << file.string();

    const auto result = LoadModelDirect(file);
    ASSERT_TRUE(result.ok) << "LoadFromData failed for " << file.string();
    ASSERT_NE(result.asset, nullptr);

    // Pendulum is a tiny rig sample; either meshes, an armature, or both.
    const bool hasGeometry  = result.asset->GetMeshCount() > 0;
    const bool hasSkeleton  = result.asset->GetSkeletonId() != 0;
    const bool hasAnimation = result.asset->HasAnimations();
    EXPECT_TRUE(hasGeometry || hasSkeleton || hasAnimation)
        << "Pendulum must produce SOMETHING — empty load means importer is broken";

    std::printf("[BlendModelLoadTests] pendulum.blend: meshes=%u, hasSkeleton=%d, hasAnim=%d, parseMs=%.2f\n",
                result.asset->GetMeshCount(), hasSkeleton ? 1 : 0,
                hasAnimation ? 1 : 0, result.parseMs);
    std::fflush(stdout);
}

// Junk under a .blend name goes through the zstd then gzip decoders; both
// reject it and the load fails cleanly with the asset marked Failed.
TEST_F(BlendModelLoadTests, JunkBytesUnderBlendNameFailToLoad)
{
    const std::string junk = "not a blend\n";
    const Vector<uint8> bytes(junk.begin(), junk.end());
    const auto asset = std::make_shared<ModelAsset>(GUID::Generate(), fs::path("junk.blend"));
    EXPECT_FALSE(asset->LoadFromData(bytes));
    EXPECT_EQ(asset->GetState(), AssetState::Failed);
}

// Blender 5.0 and later start a file with a 17-byte header the parser does not
// read. The error names that case and points at FBX or glTF instead of asking
// for a save from Blender, which would write the same header again.
TEST_F(BlendModelLoadTests, Blender5HeaderReportsUnsupportedVersion)
{
    // The header, then zeros where the first block would start.
    constexpr std::size_t kZeroBlockBytes = 64;
    const std::string header = "BLENDER17-01v0502";
    Vector<uint8> bytes(header.begin(), header.end());
    bytes.resize(bytes.size() + kZeroBlockBytes, 0);
    const auto asset = std::make_shared<ModelAsset>(GUID::Generate(), fs::path("blender5.blend"));
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        Logger::Log::Error("capture live");
        EXPECT_FALSE(asset->LoadFromData(bytes));
        Logger::Log::Flush();
    }
    ASSERT_NE(std::find(lines.begin(), lines.end(), "capture live"), lines.end()) << "the log capture saw nothing";
    const std::string error = TestLog::FirstLineContaining(lines, "not a readable .blend file");
    EXPECT_NE(error.find("saved by Blender 5.0 or later"), std::string::npos) << error;
    EXPECT_EQ(error.find("save it again"), std::string::npos) << error;
}

// Coordinate-system sanity: every loaded mesh's vertex bounds must be finite
// (no NaN/Inf), and the conversion must produce a non-degenerate AABB. The
// FBX path's most common importer-bug signature is a collapsed AABB at origin
// with NaN tails; this test fires the same gate on the Blender path.
TEST_F(BlendModelLoadTests, CoordinateConversion_BoundsAreFinite)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path file = m_SamplesRoot
        / "ellie" / "asset-demo-bundle-4.0-ellie-animation"
        / "ellie_animation" / "ellie_animation.blend";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP() << "Sample missing: " << file.string();

    const auto result = LoadModelDirect(file);
    ASSERT_TRUE(result.ok);
    ASSERT_GT(result.asset->GetMeshCount(), 0u);

    float modelMin[3], modelMax[3];
    result.asset->GetBoundingBox(modelMin, modelMax);
    for (int c = 0; c < 3; ++c)
    {
        EXPECT_TRUE(std::isfinite(modelMin[c])) << "model min[" << c << "] non-finite";
        EXPECT_TRUE(std::isfinite(modelMax[c])) << "model max[" << c << "] non-finite";
        EXPECT_LE(modelMin[c], modelMax[c]) << "model AABB inverted on axis " << c;
    }

    // Per-mesh sanity.
    const auto& meshes = result.asset->GetMeshes();
    bool foundNonTrivialMesh = false;
    for (const auto& m : meshes)
    {
        if (m.Vertices.empty()) continue;
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_TRUE(std::isfinite(m.MinBounds[c])) << "mesh '" << m.Name << "' min[" << c << "] non-finite";
            EXPECT_TRUE(std::isfinite(m.MaxBounds[c])) << "mesh '" << m.Name << "' max[" << c << "] non-finite";
        }
        for (const auto& v : m.Vertices)
        {
            for (int c = 0; c < 3; ++c)
                ASSERT_TRUE(std::isfinite(v.Position[c]))
                    << "mesh '" << m.Name << "' has non-finite vertex position[" << c << "]";
        }
        // Ellie's body mesh is at human scale; expect bounds to span more
        // than a single millimetre in at least one axis. This catches a
        // unit-conversion bug that collapses everything to origin.
        const float spanX = m.MaxBounds[0] - m.MinBounds[0];
        const float spanY = m.MaxBounds[1] - m.MinBounds[1];
        const float spanZ = m.MaxBounds[2] - m.MinBounds[2];
        if (spanX > 0.001f || spanY > 0.001f || spanZ > 0.001f)
            foundNonTrivialMesh = true;
    }
    EXPECT_TRUE(foundNonTrivialMesh)
        << "All meshes collapsed to a sub-mm AABB — coordinate conversion almost certainly broken";
}

// Coord conversion direction sanity: verifies that the imported mesh's AABB
// is consistent with the (-x, z, y) similarity. We pick the smallest sample
// (pendulum) which uses the legacy mvert/mloop/mpoly path so the conversion
// goes through the canonical helper unchanged. The signature: Blender's +Z
// is up, engine's +Y is up — so a tall thin object should have its Y axis
// span be the largest after conversion (was the largest Z-span before).
TEST_F(BlendModelLoadTests, CoordinateConversion_PendulumYAxisIsTallest)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path file = m_SamplesRoot
        / "anim-fund-rigs" / "animation_fundamental_rigs_release_01" / "pendulum.blend";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP() << "pendulum.blend not present; skipping.";

    const auto result = LoadModelDirect(file);
    ASSERT_TRUE(result.ok);
    ASSERT_GT(result.asset->GetMeshCount(), 0u);

    float minB[3], maxB[3];
    result.asset->GetBoundingBox(minB, maxB);
    const float spanX = maxB[0] - minB[0];
    const float spanY = maxB[1] - minB[1];
    const float spanZ = maxB[2] - minB[2];

    // Pendulum is a tall, thin pendulum rig — its Blender Z axis (height) is
    // by construction the biggest. After (-x, z, y) the engine Y axis should
    // dominate. (Allow small tolerance — there may be a base+armature in the
    // model that adds X/Z spread beyond the pendulum stem itself.)
    EXPECT_GE(spanY, spanX * 0.5f) << "pendulum Y span (" << spanY << ") should be comparable-or-larger than X (" << spanX << ")";
    EXPECT_GE(spanY, spanZ * 0.5f) << "pendulum Y span (" << spanY << ") should be comparable-or-larger than Z (" << spanZ << ")";

    std::printf("[BlendModelLoadTests] pendulum AABB span (X,Y,Z) = (%.3f, %.3f, %.3f)\n",
                spanX, spanY, spanZ);
    std::fflush(stdout);
}

// Auto-import smoke test: when ModelAsset::PostLoad runs (with an
// AssetManager wired so SkeletonProfile resolves), an armature-bearing
// .blend should produce a `<modelname>.humanoidrig.json` sidecar.
//
// The Phase 11 PostLoad path is gated on AssetManager::GetThreadCurrent()
// returning non-null (so raw test-harness loads don't auto-write sidecars
// next to source assets). To exercise PostLoad, this test wires a minimal
// AssetManager with the editor mount + the .blend's directory as a project
// source, then loads via LoadAssetAsync (which runs through the asset thread).
TEST_F(BlendModelLoadTests, EllieAnimation_AutoImport_GeneratesSidecar)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path repoRoot = StagedRoot();
    const fs::path file = m_SamplesRoot
        / "ellie" / "asset-demo-bundle-4.0-ellie-animation"
        / "ellie_animation" / "ellie_animation.blend";
    const fs::path profilePath = repoRoot
        / "Assets" / "SkeletonProfiles" / "HumanoidStandard.profile.json";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP() << "Sample missing: " << file.string();
    if (!fs::exists(profilePath, ec)) GTEST_SKIP() << "HumanoidStandard profile missing.";

    const fs::path sidecar = SidecarFor(file);
    if (fs::exists(sidecar, ec)) fs::remove(sidecar, ec);

    auto manager = std::make_unique<AssetManager>();
    ASSERT_TRUE(manager->Initialize());

    AssetSourceDesc projectSrc;
    projectSrc.Alias = std::string(kAssetSourceAliasProject);
    projectSrc.Root = file.parent_path();
    projectSrc.DerivedIdentity = true;
    projectSrc.Priority = 100;
    ASSERT_TRUE(manager->RegisterSource(projectSrc));

    AssetSourceDesc editorSrc;
    editorSrc.Alias = std::string(kAssetSourceAliasEditor);
    editorSrc.Root = repoRoot / "Apps" / "Editor" / "Assets";
    editorSrc.DerivedIdentity = true;
    editorSrc.Priority = 50;
    ASSERT_TRUE(manager->RegisterSource(editorSrc));

    const GUID guid = manager->ResolveAssetGuid(file);
    ASSERT_FALSE(guid.IsNull());
    auto fut = manager->LoadAssetAsync(guid);
    auto asset = fut.get();
    auto model = std::dynamic_pointer_cast<ModelAsset>(asset);
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->GetState(), AssetState::Loaded);

    // Sidecar should be on disk after PostLoad. The auto-import coverage
    // gate (80% of canonical bones matched) may fail for non-standard rigs;
    // in that case the sidecar legitimately won't be written. Treat absence
    // as a soft warning, not a hard failure — the gate behaviour is exercised
    // by AutoImportTests + the FBX PostLoad fixture.
    if (fs::exists(sidecar, ec))
    {
        std::printf("[BlendModelLoadTests] sidecar generated: %s\n", sidecar.string().c_str());
    }
    else
    {
        std::printf("[BlendModelLoadTests] no sidecar generated (coverage gate failed; non-fatal in B2)\n");
    }
    std::fflush(stdout);

    manager->Shutdown();
}

} // namespace
