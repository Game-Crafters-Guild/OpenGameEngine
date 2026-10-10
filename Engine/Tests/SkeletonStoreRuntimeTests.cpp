// Tests for SkeletonStore per-entity runtime allocation and lifecycle.
// Validates that each entity gets independent skinning state
// (CompactSkinMatrices, atlas offsets) and that ReleaseRuntime reclaims slots.

#include <gtest/gtest.h>

#include "ECSModules/Rendering/SkeletonStore.h"

using namespace GameEngine::Engine::Renderer;

class SkeletonStoreRuntimeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        auto& store = SkeletonStore::Instance();
        m_SkelId = store.CreateSkeleton(4);
        auto* skel = store.Get(m_SkelId);
        ASSERT_NE(skel, nullptr);
        skel->SkinJointCount = 4;
        skel->JointNodes = {0, 1, 2, 3};
    }

    uint32_t m_SkelId = 0;
};

TEST_F(SkeletonStoreRuntimeTest, GetRuntime_ReturnsNullForInvalidId)
{
    EXPECT_EQ(SkeletonStore::Instance().GetRuntime(0), nullptr);
    EXPECT_EQ(SkeletonStore::Instance().GetRuntime(999999), nullptr);
}

TEST_F(SkeletonStoreRuntimeTest, CreateRuntime_ReturnsNonZeroId)
{
    uint32_t rtId = SkeletonStore::Instance().CreateRuntime(m_SkelId);
    EXPECT_NE(rtId, 0u);
}

TEST_F(SkeletonStoreRuntimeTest, CreateRuntime_InitializesIdentityPalette)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtId = store.CreateRuntime(m_SkelId);
    auto* runtime = store.GetRuntime(rtId);
    ASSERT_NE(runtime, nullptr);

    // 4 joints * 16 floats = 64 floats.
    ASSERT_EQ(runtime->CompactSkinMatrices.size(), 4u * 16u);

    for (uint32_t j = 0; j < 4; ++j)
    {
        const size_t base = static_cast<size_t>(j) * 16;
        EXPECT_FLOAT_EQ(runtime->CompactSkinMatrices[base + 0], 1.0f) << "joint " << j;
        EXPECT_FLOAT_EQ(runtime->CompactSkinMatrices[base + 5], 1.0f) << "joint " << j;
        EXPECT_FLOAT_EQ(runtime->CompactSkinMatrices[base + 10], 1.0f) << "joint " << j;
        EXPECT_FLOAT_EQ(runtime->CompactSkinMatrices[base + 15], 1.0f) << "joint " << j;
        EXPECT_FLOAT_EQ(runtime->CompactSkinMatrices[base + 1], 0.0f) << "joint " << j;
        EXPECT_FLOAT_EQ(runtime->CompactSkinMatrices[base + 4], 0.0f) << "joint " << j;
    }

    EXPECT_EQ(runtime->AtlasPaletteOffsetBones, 0u);
}

TEST_F(SkeletonStoreRuntimeTest, MultipleRuntimes_AreIndependent)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtA = store.CreateRuntime(m_SkelId);
    uint32_t rtB = store.CreateRuntime(m_SkelId);

    EXPECT_NE(rtA, rtB);

    auto* runtimeA = store.GetRuntime(rtA);
    auto* runtimeB = store.GetRuntime(rtB);
    ASSERT_NE(runtimeA, nullptr);
    ASSERT_NE(runtimeB, nullptr);
    EXPECT_NE(runtimeA, runtimeB);

    // Write different data.
    runtimeA->AtlasPaletteOffsetBones = 100;
    runtimeB->AtlasPaletteOffsetBones = 200;
    runtimeA->CompactSkinMatrices[0] = 99.0f;

    // Verify isolation.
    EXPECT_EQ(store.GetRuntime(rtA)->AtlasPaletteOffsetBones, 100u);
    EXPECT_EQ(store.GetRuntime(rtB)->AtlasPaletteOffsetBones, 200u);
    EXPECT_FLOAT_EQ(store.GetRuntime(rtA)->CompactSkinMatrices[0], 99.0f);
    EXPECT_FLOAT_EQ(store.GetRuntime(rtB)->CompactSkinMatrices[0], 1.0f);
}

TEST_F(SkeletonStoreRuntimeTest, GetRuntimeSkeletonId_ReturnsCorrectSkeleton)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtId = store.CreateRuntime(m_SkelId);
    EXPECT_EQ(store.GetRuntimeSkeletonId(rtId), m_SkelId);
    EXPECT_EQ(store.GetRuntimeSkeletonId(0), 0u);
}

TEST_F(SkeletonStoreRuntimeTest, ReleaseRuntime_MakesIdInvalid)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtId = store.CreateRuntime(m_SkelId);
    ASSERT_NE(store.GetRuntime(rtId), nullptr);

    store.ReleaseRuntime(rtId);

    EXPECT_EQ(store.GetRuntime(rtId), nullptr);
    EXPECT_EQ(store.GetRuntimeSkeletonId(rtId), 0u);
}

TEST_F(SkeletonStoreRuntimeTest, ReleaseRuntime_SlotIsReused)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtA = store.CreateRuntime(m_SkelId);
    store.ReleaseRuntime(rtA);

    uint32_t rtB = store.CreateRuntime(m_SkelId);
    EXPECT_EQ(rtB, rtA); // reused same slot

    auto* runtime = store.GetRuntime(rtB);
    ASSERT_NE(runtime, nullptr);
    // Re-initialized to identity.
    EXPECT_EQ(runtime->CompactSkinMatrices.size(), 4u * 16u);
    EXPECT_FLOAT_EQ(runtime->CompactSkinMatrices[0], 1.0f);
}

TEST_F(SkeletonStoreRuntimeTest, ReleaseRuntime_DoubleFreeSafe)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtId = store.CreateRuntime(m_SkelId);
    store.ReleaseRuntime(rtId);
    store.ReleaseRuntime(rtId); // should not crash or corrupt
    EXPECT_EQ(store.GetRuntime(rtId), nullptr);
}

// --- Reference counting (SkeletonStore.h ownership rule) ---

TEST_F(SkeletonStoreRuntimeTest, CreateRuntime_StartsWithOneReference)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtId = store.CreateRuntime(m_SkelId);
    EXPECT_EQ(store.GetRuntimeRefCount(rtId), 1u);
    store.ReleaseRuntime(rtId);
    EXPECT_EQ(store.GetRuntimeRefCount(rtId), 0u);
}

TEST_F(SkeletonStoreRuntimeTest, RetainRuntime_KeepsSlotAliveUntilLastRelease)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtId = store.CreateRuntime(m_SkelId);
    ASSERT_TRUE(store.RetainRuntime(rtId));
    ASSERT_TRUE(store.RetainRuntime(rtId));
    EXPECT_EQ(store.GetRuntimeRefCount(rtId), 3u);

    store.ReleaseRuntime(rtId);
    EXPECT_NE(store.GetRuntime(rtId), nullptr) << "freed while two references were still held";
    store.ReleaseRuntime(rtId);
    EXPECT_NE(store.GetRuntime(rtId), nullptr) << "freed while one reference was still held";

    store.ReleaseRuntime(rtId);
    EXPECT_EQ(store.GetRuntime(rtId), nullptr);
}

// A held runtime must never be handed to the next CreateRuntime — that is the
// aliasing half of the multi-entity spawn defect.
TEST_F(SkeletonStoreRuntimeTest, RetainedRuntime_IsNotReissuedToNextCreate)
{
    auto& store = SkeletonStore::Instance();
    uint32_t shared = store.CreateRuntime(m_SkelId);
    ASSERT_TRUE(store.RetainRuntime(shared)); // a second SkeletonRef takes it

    store.ReleaseRuntime(shared); // one of the two entities is deleted
    ASSERT_NE(store.GetRuntime(shared), nullptr);

    uint32_t next = store.CreateRuntime(m_SkelId);
    EXPECT_NE(next, shared) << "surviving sibling's runtime was reissued to a new spawn";

    store.ReleaseRuntime(next);
    store.ReleaseRuntime(shared);
}

TEST_F(SkeletonStoreRuntimeTest, RetainRuntime_RejectsFreedAndInvalidIds)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtId = store.CreateRuntime(m_SkelId);
    store.ReleaseRuntime(rtId);

    EXPECT_FALSE(store.RetainRuntime(rtId)) << "retained a freed slot";
    EXPECT_FALSE(store.RetainRuntime(0));
    EXPECT_FALSE(store.RetainRuntime(999999));
    EXPECT_EQ(store.GetRuntime(rtId), nullptr) << "a rejected retain must not resurrect the slot";
}

TEST_F(SkeletonStoreRuntimeTest, ReleaseRuntime_ExtraReleasesDoNotUnderflow)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtId = store.CreateRuntime(m_SkelId);
    store.ReleaseRuntime(rtId);
    store.ReleaseRuntime(rtId); // stray release on a freed slot

    uint32_t reused = store.CreateRuntime(m_SkelId);
    EXPECT_EQ(store.GetRuntimeRefCount(reused), 1u);
    store.ReleaseRuntime(reused);
    EXPECT_EQ(store.GetRuntime(reused), nullptr) << "stray release left a phantom reference behind";
}

TEST_F(SkeletonStoreRuntimeTest, CreateRuntime_FallsToBoneCountWhenSkinJointCountZero)
{
    auto& store = SkeletonStore::Instance();
    uint32_t skelId2 = store.CreateSkeleton(8);
    // Leave SkinJointCount at 0 — should fall back to BoneCount (8).

    uint32_t rtId = store.CreateRuntime(skelId2);
    auto* runtime = store.GetRuntime(rtId);
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->CompactSkinMatrices.size(), 8u * 16u);
}

TEST_F(SkeletonStoreRuntimeTest, PointerStability_AcrossCreateRuntime)
{
    auto& store = SkeletonStore::Instance();
    uint32_t rtA = store.CreateRuntime(m_SkelId);
    auto* ptrA = store.GetRuntime(rtA);
    ASSERT_NE(ptrA, nullptr);

    // Write a marker value.
    ptrA->AtlasPaletteOffsetBones = 42;

    // Create several more runtimes (may trigger deque growth).
    for (int i = 0; i < 100; ++i)
        store.CreateRuntime(m_SkelId);

    // Original pointer must still be valid (deque guarantee).
    EXPECT_EQ(ptrA->AtlasPaletteOffsetBones, 42u);
    EXPECT_EQ(store.GetRuntime(rtA), ptrA);
}

TEST_F(SkeletonStoreRuntimeTest, GetCount_ReflectsCreatedSkeletons)
{
    EXPECT_GE(SkeletonStore::Instance().GetCount(), 1u);
}

// --- Topological sort tests ---
// None of these skeletons has a joint table, so every bone is in the joint closure and the
// closure's levels are the hierarchy's.

TEST(SkeletonTopologicalSort, EmptySkeleton)
{
    SkeletonData skel{};
    skel.BoneCount = 0;
    skel.ComputeTopologicalSort();
    EXPECT_TRUE(skel.TopologicalOrder.empty());
    EXPECT_EQ(skel.JointClosureLevelOffsets.size(), 1u); // just the sentinel 0
    EXPECT_EQ(skel.JointClosureLevelOffsets[0], 0u);
}

TEST(SkeletonTopologicalSort, SingleBone)
{
    SkeletonData skel{};
    skel.BoneCount = 1;
    skel.Parent = {-1};
    skel.ComputeTopologicalSort();
    ASSERT_EQ(skel.TopologicalOrder.size(), 1u);
    EXPECT_EQ(skel.TopologicalOrder[0], 0u);
    ASSERT_EQ(skel.JointClosureLevelOffsets.size(), 2u);
    EXPECT_EQ(skel.JointClosureLevelOffsets[0], 0u);
    EXPECT_EQ(skel.JointClosureLevelOffsets[1], 1u);
}

TEST(SkeletonTopologicalSort, LinearChain)
{
    // 0 -> 1 -> 2 -> 3
    SkeletonData skel{};
    skel.BoneCount = 4;
    skel.Parent = {-1, 0, 1, 2};
    skel.ComputeTopologicalSort();
    ASSERT_EQ(skel.TopologicalOrder.size(), 4u);
    // Each bone is its own level.
    ASSERT_EQ(skel.JointClosureLevelOffsets.size(), 5u);
    // Parents must come before children in the order.
    for (uint32_t i = 0; i < 4; ++i)
        EXPECT_EQ(skel.TopologicalOrder[i], i);
}

TEST(SkeletonTopologicalSort, BranchingHierarchy)
{
    //     0
    //    / \
    //   1   2
    //  / \
    // 3   4
    SkeletonData skel{};
    skel.BoneCount = 5;
    skel.Parent = {-1, 0, 0, 1, 1};
    skel.ComputeTopologicalSort();
    ASSERT_EQ(skel.TopologicalOrder.size(), 5u);
    // 3 levels: {0}, {1,2}, {3,4}
    ASSERT_EQ(skel.JointClosureLevelOffsets.size(), 4u);
    EXPECT_EQ(skel.JointClosureLevelOffsets[0], 0u);
    EXPECT_EQ(skel.JointClosureLevelOffsets[1], 1u);
    EXPECT_EQ(skel.JointClosureLevelOffsets[2], 3u);
    EXPECT_EQ(skel.JointClosureLevelOffsets[3], 5u);
    // Root must be first.
    EXPECT_EQ(skel.TopologicalOrder[0], 0u);
    // Every bone's parent appears earlier in the order.
    std::vector<uint32_t> posInOrder(5);
    for (uint32_t i = 0; i < 5; ++i)
        posInOrder[skel.TopologicalOrder[i]] = i;
    for (uint32_t i = 0; i < 5; ++i) {
        if (skel.Parent[i] >= 0)
            EXPECT_LT(posInOrder[static_cast<uint32_t>(skel.Parent[i])], posInOrder[i]);
    }
}

TEST(SkeletonTopologicalSort, MultipleRoots)
{
    // Two independent chains: 0->1, 2->3
    SkeletonData skel{};
    skel.BoneCount = 4;
    skel.Parent = {-1, 0, -1, 2};
    skel.ComputeTopologicalSort();
    ASSERT_EQ(skel.TopologicalOrder.size(), 4u);
    // 2 levels: {0,2}, {1,3}
    ASSERT_EQ(skel.JointClosureLevelOffsets.size(), 3u);
    EXPECT_EQ(skel.JointClosureLevelOffsets[0], 0u);
    EXPECT_EQ(skel.JointClosureLevelOffsets[1], 2u);
    EXPECT_EQ(skel.JointClosureLevelOffsets[2], 4u);
}

// ---- Previous-frame palette offsets (TAA skinned motion vectors) ----
//
// The skinned MV pass needs both pose endpoints. RollPaletteOffsets ages each
// runtime's published offset into its previous-frame field once per frame; the
// validity flag is what stops a consumer from pairing a live offset with an
// atlas range that now belongs to somebody else.

TEST(SkeletonRuntimePaletteHistory, RollAfterPublish_MakesPreviousValid)
{
    auto& store = SkeletonStore::Instance();
    const uint32_t skelId = store.CreateSkeleton(4);
    const uint32_t rtId = store.CreateRuntime(skelId);
    ASSERT_NE(rtId, 0u);
    auto* rt = store.GetRuntime(rtId);
    ASSERT_NE(rt, nullptr);

    rt->SetAtlasPaletteOffset(64u);
    EXPECT_TRUE(rt->AtlasPaletteWrittenThisFrame);

    store.RollPaletteOffsets();
    EXPECT_TRUE(rt->PrevAtlasPaletteValid);
    EXPECT_EQ(rt->PrevAtlasPaletteOffsetBones, 64u);
    EXPECT_FALSE(rt->AtlasPaletteWrittenThisFrame)
        << "the written stamp is consumed by the roll";

    // Second frame at a different offset: previous must name frame one's slot.
    rt->SetAtlasPaletteOffset(128u);
    store.RollPaletteOffsets();
    EXPECT_TRUE(rt->PrevAtlasPaletteValid);
    EXPECT_EQ(rt->PrevAtlasPaletteOffsetBones, 128u);

    store.ReleaseRuntime(rtId);
}

TEST(SkeletonRuntimePaletteHistory, RollWithoutPublish_InvalidatesPrevious)
{
    auto& store = SkeletonStore::Instance();
    const uint32_t skelId = store.CreateSkeleton(4);
    const uint32_t rtId = store.CreateRuntime(skelId);
    auto* rt = store.GetRuntime(rtId);
    ASSERT_NE(rt, nullptr);

    rt->SetAtlasPaletteOffset(64u);
    store.RollPaletteOffsets();
    ASSERT_TRUE(rt->PrevAtlasPaletteValid);

    // A frame where no producer publishes (animation stopped, allocation
    // failed): the stale offset must not be presented as a previous pose.
    store.RollPaletteOffsets();
    EXPECT_FALSE(rt->PrevAtlasPaletteValid)
        << "no publish last frame means there is no previous pose to read";

    store.ReleaseRuntime(rtId);
}

TEST(SkeletonRuntimePaletteHistory, ReleaseRuntime_ClearsPaletteHistory)
{
    auto& store = SkeletonStore::Instance();
    const uint32_t skelId = store.CreateSkeleton(4);
    const uint32_t rtId = store.CreateRuntime(skelId);
    auto* rt = store.GetRuntime(rtId);
    ASSERT_NE(rt, nullptr);

    rt->SetAtlasPaletteOffset(64u);
    store.RollPaletteOffsets();
    ASSERT_TRUE(rt->PrevAtlasPaletteValid);

    store.ReleaseRuntime(rtId);

    // Recycled slots must not hand the next occupant a stale pose history.
    const uint32_t reusedId = store.CreateRuntime(skelId);
    auto* reused = store.GetRuntime(reusedId);
    ASSERT_NE(reused, nullptr);
    EXPECT_FALSE(reused->PrevAtlasPaletteValid);
    EXPECT_EQ(reused->PrevAtlasPaletteOffsetBones, 0u);
    EXPECT_FALSE(reused->AtlasPaletteWrittenThisFrame);

    store.ReleaseRuntime(reusedId);
}
