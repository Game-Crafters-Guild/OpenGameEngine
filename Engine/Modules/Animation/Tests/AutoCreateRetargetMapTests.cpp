#include <gtest/gtest.h>

#include "Animation/HumanBone.h"
#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/RetargetMap.h"

#include "AssetCore/GUID.h"

#include <filesystem>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

// Populate a HumanoidRig with chain entries of the given kinds. Used only
// to exercise AutoCreateRetargetMap; no skeleton parsing.
void PopulateRigWithChains(HumanoidRig& rig, const std::vector<ChainKind>& kinds)
{
    auto& chains = rig.ChainsMutable();
    for (ChainKind k : kinds)
    {
        HumanoidChain c;
        c.Kind = k;
        c.Start = HumanBone::Hips;
        c.End   = HumanBone::Head; // placeholder; AutoCreateRetargetMap doesn't read these
        chains.push_back(c);
    }
}

} // namespace

TEST(AutoCreateRetargetMapTest, PopulatesChainPairingsForCommonChains)
{
    const GUID srcGuid("01010101-0202-0303-0404-050505050505");
    const GUID tgtGuid("06060606-0707-0808-0909-0a0a0a0a0a0a");

    HumanoidRig src(srcGuid, std::filesystem::path("test://src.humanoidrig.json"));
    HumanoidRig tgt(tgtGuid, std::filesystem::path("test://tgt.humanoidrig.json"));
    PopulateRigWithChains(src, {ChainKind::Spine, ChainKind::LeftArm, ChainKind::RightArm,
                                ChainKind::LeftLeg, ChainKind::RightLeg, ChainKind::Head});
    PopulateRigWithChains(tgt, {ChainKind::Spine, ChainKind::LeftArm, ChainKind::RightArm,
                                ChainKind::LeftLeg, ChainKind::RightLeg, ChainKind::Head});

    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    AutoCreateRetargetMap(src, tgt, map);

    EXPECT_EQ(map.SourceRigRef().ToString(), srcGuid.ToString());
    EXPECT_EQ(map.TargetRigRef().ToString(), tgtGuid.ToString());
    EXPECT_EQ(map.ChainMap().size(), 6u);
    EXPECT_TRUE(map.OpStack().empty());

    auto findPairing = [&](ChainKind k) -> const ChainPairing*
    {
        for (const auto& p : map.ChainMap())
            if (p.Kind == k) return &p;
        return nullptr;
    };
    ASSERT_NE(findPairing(ChainKind::Spine), nullptr);
    EXPECT_EQ(findPairing(ChainKind::Spine)->FK.RotationMode, FKRotationMode::OneToOne);
    EXPECT_FLOAT_EQ(findPairing(ChainKind::Spine)->FK.RotationAlpha, 1.0f);
    EXPECT_EQ(findPairing(ChainKind::Spine)->FK.TranslationMode, FKTranslationMode::None);
    EXPECT_FALSE(findPairing(ChainKind::Spine)->IK.Enabled);
    EXPECT_FALSE(findPairing(ChainKind::Spine)->FootLock.Enabled);
}

TEST(AutoCreateRetargetMapTest, OnlyPairsChainsPresentOnBothRigs)
{
    const GUID srcGuid("11111111-2222-3333-4444-555555555555");
    const GUID tgtGuid("66666666-7777-8888-9999-aaaaaaaaaaaa");

    HumanoidRig src(srcGuid, std::filesystem::path("test://src.humanoidrig.json"));
    HumanoidRig tgt(tgtGuid, std::filesystem::path("test://tgt.humanoidrig.json"));
    PopulateRigWithChains(src, {ChainKind::Spine, ChainKind::LeftArm, ChainKind::RightArm,
                                ChainKind::LeftLeg, ChainKind::RightLeg, ChainKind::Head});
    // Target has no Right arm (e.g., one-armed character).
    PopulateRigWithChains(tgt, {ChainKind::Spine, ChainKind::LeftArm,
                                ChainKind::LeftLeg, ChainKind::RightLeg, ChainKind::Head});

    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    AutoCreateRetargetMap(src, tgt, map);

    EXPECT_EQ(map.ChainMap().size(), 5u);
    auto findPairing = [&](ChainKind k) -> const ChainPairing*
    {
        for (const auto& p : map.ChainMap())
            if (p.Kind == k) return &p;
        return nullptr;
    };
    EXPECT_EQ(findPairing(ChainKind::RightArm), nullptr) << "Right arm should not pair";
    EXPECT_NE(findPairing(ChainKind::LeftArm),  nullptr);
    EXPECT_NE(findPairing(ChainKind::Spine),    nullptr);
    EXPECT_NE(findPairing(ChainKind::Head),     nullptr);
}

TEST(AutoCreateRetargetMapTest, MapIsBrokenWhenRigsMissing)
{
    HumanoidRig src(GUID(), std::filesystem::path("test://src.humanoidrig.json"));
    HumanoidRig tgt(GUID(), std::filesystem::path("test://tgt.humanoidrig.json"));
    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    AutoCreateRetargetMap(src, tgt, map);
    // GUIDs are still null, IsBroken() should fire.
    EXPECT_TRUE(map.IsBroken());
}

TEST(AutoCreateRetargetMapTest, OpStackStartsEmpty)
{
    const GUID srcGuid("11111111-2222-3333-4444-555555555555");
    const GUID tgtGuid("66666666-7777-8888-9999-aaaaaaaaaaaa");

    HumanoidRig src(srcGuid, std::filesystem::path("test://src.humanoidrig.json"));
    HumanoidRig tgt(tgtGuid, std::filesystem::path("test://tgt.humanoidrig.json"));
    PopulateRigWithChains(src, {ChainKind::Spine});
    PopulateRigWithChains(tgt, {ChainKind::Spine});
    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    AutoCreateRetargetMap(src, tgt, map);
    EXPECT_TRUE(map.OpStack().empty());
}
