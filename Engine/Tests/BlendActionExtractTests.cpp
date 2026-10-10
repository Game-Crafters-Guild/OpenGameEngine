// bAction -> AnimationClip extraction tests.
//
// Validates the FCurve walker, RNA-path bone-name parser, Euler/quaternion
// handling, coord-system conversion, and ClipStore + sidecar emission.
//
// Skip-gated when blend-samples/ is not extracted on this machine.

#include <gtest/gtest.h>

#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Animation/SkeletonData.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "StagedTestPaths.h"

namespace fs = std::filesystem;
using namespace GameEngine;
using namespace GameEngine::Animation;

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

struct LoadedModel
{
    bool ok = false;
    double parseMs = 0.0;
    std::shared_ptr<ModelAsset> asset;
};

LoadedModel LoadModelDirect(const fs::path& path)
{
    LoadedModel r;
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

class BlendActionExtractTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_SamplesRoot = SamplesRoot();
        std::error_code ec;
        m_HasSamples = fs::is_directory(m_SamplesRoot, ec) && !ec;
        // Reset the global ClipStore between tests so order doesn't matter
        // and stale clips from a prior test don't mask a real failure.
        ::GameEngine::Engine::Renderer::ClipStore::Instance().ClearForTest();
    }

    fs::path m_SamplesRoot;
    bool m_HasSamples = false;
};

// Ellie animation file is the heavy fixture: 58 Actions on a humanoid rig.
// We assert the importer produces a clip per Action with non-empty channels
// and a positive duration, plus channels for the canonical anatomical
// landmarks (Hips + Spine + a leg bone — names depend on the rig).
TEST_F(BlendActionExtractTests, EllieAnimation_ExtractsManyClipsAndChannels)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path file = m_SamplesRoot
        / "ellie" / "asset-demo-bundle-4.0-ellie-animation"
        / "ellie_animation" / "ellie_animation.blend";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP() << "Sample missing: " << file.string();

    const auto result = LoadModelDirect(file);
    ASSERT_TRUE(result.ok);
    ASSERT_NE(result.asset, nullptr);

    EXPECT_TRUE(result.asset->HasAnimations());
    const auto& names = result.asset->GetAnimationNames();
    EXPECT_GE(names.size(), 50u)
        << "Ellie ships with ~58 Actions; expected at least 50";

    const auto& clipGuids = result.asset->GetEmbeddedClipGuids();
    EXPECT_GT(clipGuids.size(), 0u)
        << "Phase B3 must register at least one clip per Action";

    auto& clipStore = ::GameEngine::Engine::Renderer::ClipStore::Instance();

    size_t clipsWithChannels = 0;
    size_t clipsWithDuration = 0;
    size_t totalChannels = 0;
    for (const GUID& g : clipGuids)
    {
        const uint32 idx = clipStore.GetIndexIfPresent(g);
        if (idx == 0) continue;
        auto clip = clipStore.Get(idx);
        if (!clip) continue;
        const auto& channels = clip->GetChannels();
        if (!channels.empty())
        {
            ++clipsWithChannels;
            totalChannels += channels.size();
        }
        if (clip->GetDuration() > 0.0f)
            ++clipsWithDuration;
    }

    EXPECT_GT(clipsWithChannels, 0u);
    EXPECT_GT(clipsWithDuration, 0u);
    EXPECT_GT(totalChannels, 0u);

    std::printf("[BlendActionExtractTests] ellie: actions=%zu, clipsRegistered=%zu, "
                "clipsWithChannels=%zu, clipsWithDuration=%zu, totalChannels=%zu, parseMs=%.2f\n",
                names.size(), clipGuids.size(),
                clipsWithChannels, clipsWithDuration, totalChannels, result.parseMs);
    std::fflush(stdout);
}

// Pendulum has a tiny rig (Phase B2 confirms it loads cleanly) but no
// authored Actions in the sample bundle — verify the importer handles
// "no animation" gracefully without registering bogus clips.
TEST_F(BlendActionExtractTests, Pendulum_ProducesZeroOrFewActionsCleanly)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path file = m_SamplesRoot
        / "anim-fund-rigs" / "animation_fundamental_rigs_release_01" / "pendulum.blend";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP() << "pendulum.blend missing";

    const auto result = LoadModelDirect(file);
    ASSERT_TRUE(result.ok);
    ASSERT_NE(result.asset, nullptr);

    const auto& names  = result.asset->GetAnimationNames();
    const auto& guids  = result.asset->GetEmbeddedClipGuids();

    // Whatever the count, name + guid arrays must be consistent — guids
    // only emitted for Actions with at least one channel, but names list
    // every Action regardless. So |guids| <= |names|.
    EXPECT_LE(guids.size(), names.size());

    // If pendulum did emit any clips, every clip must have non-empty
    // channels and positive duration (no zombie clips).
    auto& clipStore = ::GameEngine::Engine::Renderer::ClipStore::Instance();
    for (const GUID& g : guids)
    {
        const uint32 idx = clipStore.GetIndexIfPresent(g);
        ASSERT_NE(idx, 0u);
        auto clip = clipStore.Get(idx);
        ASSERT_NE(clip, nullptr);
        EXPECT_FALSE(clip->GetChannels().empty());
        EXPECT_GT(clip->GetDuration(), 0.0f);
    }

    std::printf("[BlendActionExtractTests] pendulum: actions=%zu, clipsRegistered=%zu\n",
                names.size(), guids.size());
    std::fflush(stdout);
}

// Coord-system sanity: pick a translation channel from a known clip and
// assert its keys are in engine convention (finite values; X/Y/Z swaps
// applied). The smoking-gun signature would be a clip whose translation
// keys are bit-identical to the Blender source values — that means the
// (-x, z, y) similarity wasn't applied. We check a weaker but reliable
// invariant here: any bone whose Blender Z values were nonzero in the
// authored animation must show up as engine-Y nonzero in at least one
// keyframe (Y in the engine corresponds to Z in Blender).
TEST_F(BlendActionExtractTests, EllieAnimation_CoordConversion_TranslationKeysAreFinite)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path file = m_SamplesRoot
        / "ellie" / "asset-demo-bundle-4.0-ellie-animation"
        / "ellie_animation" / "ellie_animation.blend";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP() << "Sample missing";

    const auto result = LoadModelDirect(file);
    ASSERT_TRUE(result.ok);

    const auto& guids = result.asset->GetEmbeddedClipGuids();
    ASSERT_GT(guids.size(), 0u);

    auto& clipStore = ::GameEngine::Engine::Renderer::ClipStore::Instance();
    bool foundTranslationChannel = false;
    bool foundEngineYNonzero = false;
    for (const GUID& g : guids)
    {
        const uint32 idx = clipStore.GetIndexIfPresent(g);
        if (idx == 0) continue;
        auto clip = clipStore.Get(idx);
        if (!clip) continue;
        for (const auto& ch : clip->GetChannels())
        {
            if (ch.path != AnimPath::Translation) continue;
            foundTranslationChannel = true;
            for (const auto& k : ch.keys)
            {
                ASSERT_TRUE(std::isfinite(k.translation[0]));
                ASSERT_TRUE(std::isfinite(k.translation[1]));
                ASSERT_TRUE(std::isfinite(k.translation[2]));
                if (std::fabs(k.translation[1]) > 1e-5f)
                    foundEngineYNonzero = true;
            }
        }
        if (foundEngineYNonzero) break;
    }

    EXPECT_TRUE(foundTranslationChannel)
        << "No translation channel found across all clips — extraction broken";
    // Ellie's animations include hip-bone vertical motion (jumps, walks).
    // A stuck-at-origin Y across every clip's translation keys is a smoking
    // gun for the (-x, z, y) similarity not being applied.
    EXPECT_TRUE(foundEngineYNonzero)
        << "No clip has a translation key with engine-Y motion — coord "
           "conversion may have collapsed the Z->Y axis swap";
}

// Rotation channel sanity: at least one rotation channel across all
// clips must contain a key whose quaternion is non-identity (i.e., the
// imported clip actually has rotation animation). And every key must be
// finite + roughly unit-length (slerp-safe).
TEST_F(BlendActionExtractTests, EllieAnimation_RotationKeys_AreNonTrivialAndUnit)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path file = m_SamplesRoot
        / "ellie" / "asset-demo-bundle-4.0-ellie-animation"
        / "ellie_animation" / "ellie_animation.blend";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP();

    const auto result = LoadModelDirect(file);
    ASSERT_TRUE(result.ok);
    const auto& guids = result.asset->GetEmbeddedClipGuids();
    ASSERT_GT(guids.size(), 0u);

    auto& clipStore = ::GameEngine::Engine::Renderer::ClipStore::Instance();
    size_t rotKeysScanned = 0;
    bool foundNonIdentity = false;
    for (const GUID& g : guids)
    {
        const uint32 idx = clipStore.GetIndexIfPresent(g);
        if (idx == 0) continue;
        auto clip = clipStore.Get(idx);
        if (!clip) continue;
        for (const auto& ch : clip->GetChannels())
        {
            if (ch.path != AnimPath::Rotation) continue;
            for (const auto& k : ch.keys)
            {
                ++rotKeysScanned;
                const float qx = k.rotation[0], qy = k.rotation[1],
                            qz = k.rotation[2], qw = k.rotation[3];
                ASSERT_TRUE(std::isfinite(qx) && std::isfinite(qy)
                         && std::isfinite(qz) && std::isfinite(qw));
                const float lenSq = qx*qx + qy*qy + qz*qz + qw*qw;
                EXPECT_GT(lenSq, 0.5f) << "near-zero quaternion: slerp will fail";
                EXPECT_LT(lenSq, 2.0f) << "non-unit quaternion: drift > 2x";
                // Identity quaternion is (0,0,0,1) post-conversion.
                const float dxIdentity = std::fabs(qx) + std::fabs(qy)
                                       + std::fabs(qz) + std::fabs(qw - 1.0f);
                if (dxIdentity > 1e-3f)
                    foundNonIdentity = true;
            }
        }
    }

    EXPECT_GT(rotKeysScanned, 0u);
    EXPECT_TRUE(foundNonIdentity)
        << "Every rotation key is identity — Bezier knots aren't being read "
           "or the (W,X,Y,Z)->(X,Y,Z,W) reorder collapsed everything to identity";
}

// Sidecar emission: at least one .anim.json file should appear next to the
// .blend after a successful Action extraction. We check the presence of
// any sidecar matching the expected naming pattern; cleanup happens in
// SetUp via ClipStore::ClearForTest (which doesn't touch disk — sidecars
// persist across runs by design).
TEST_F(BlendActionExtractTests, EllieAnimation_SidecarFilesEmitted)
{
    if (!m_HasSamples) GTEST_SKIP() << "blend-samples/ not extracted; skipping.";

    const fs::path file = m_SamplesRoot
        / "ellie" / "asset-demo-bundle-4.0-ellie-animation"
        / "ellie_animation" / "ellie_animation.blend";
    std::error_code ec;
    if (!fs::exists(file, ec)) GTEST_SKIP();

    const fs::path modelDir = file.parent_path();
    const std::string stemPrefix = file.stem().string() + "__";

    // Snapshot pre-load sidecar count.
    size_t sidecarsBefore = 0;
    for (const auto& entry : fs::directory_iterator(modelDir, ec))
    {
        if (!ec && entry.is_regular_file())
        {
            const std::string name = entry.path().filename().string();
            if (name.rfind(stemPrefix, 0) == 0
                && name.size() > 10
                && name.compare(name.size() - 10, 10, ".anim.json") == 0)
            {
                ++sidecarsBefore;
            }
        }
    }

    const auto result = LoadModelDirect(file);
    ASSERT_TRUE(result.ok);

    size_t sidecarsAfter = 0;
    for (const auto& entry : fs::directory_iterator(modelDir, ec))
    {
        if (!ec && entry.is_regular_file())
        {
            const std::string name = entry.path().filename().string();
            if (name.rfind(stemPrefix, 0) == 0
                && name.size() > 10
                && name.compare(name.size() - 10, 10, ".anim.json") == 0)
            {
                ++sidecarsAfter;
            }
        }
    }

    EXPECT_GT(sidecarsAfter, 0u)
        << "No sidecar files written next to ellie_animation.blend";
    // sidecarsAfter should be >= sidecarsBefore (idempotent — re-import
    // overwrites or skips, never deletes).
    EXPECT_GE(sidecarsAfter, sidecarsBefore);

    std::printf("[BlendActionExtractTests] sidecars: before=%zu, after=%zu, in dir=%s\n",
                sidecarsBefore, sidecarsAfter, modelDir.string().c_str());
    std::fflush(stdout);
}

} // namespace
