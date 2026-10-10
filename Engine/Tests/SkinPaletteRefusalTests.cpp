// What a palette producer must do when the atlas REFUSES an allocation.
//
// The bone-palette ring refuses rather than wrapping, and grows only on the
// next visit to the slot that overflowed. A producer that ignores the refusal
// leaves the runtime's CURRENT-frame offset naming last frame's slot — which
// the atlas has since handed to a different runtime. RenderExtractionSystem
// copies that offset straight into GPUInstance::skinPaletteOffset, so the
// entity is skinned with another entity's bones: a visibly wrong pose, not a
// dropped frame.
//
// Atlas slot 0 is the identity block SkinPaletteAtlas::BeginFrame reserves, so
// retiring the offset to it renders bind pose for the cycle the ring takes to
// grow. The first test pins that slot 0 really is that block; the second pins
// the producer behaviour that depends on it.

#include <gtest/gtest.h>

#include "Components/Animation/SkeletonRef.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/Systems/SkinningUploadSystem.h"
#include "Engine/Rendering/BonePaletteLayout.h"
#include "Engine/Rendering/PerFrameWritePool.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkinPaletteAtlas.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <memory>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

#include "TestDeviceHelper.h"

namespace
{
constexpr uint32_t kTestJointCount = 4;
constexpr float32 kDt = 1.0f / 60.0f;

// BeginFrame's reservation, and therefore the base every caller allocation of
// the frame sits above.
constexpr size_t kIdentityBlockBytes =
    static_cast<size_t>(SkinPaletteAtlas::kIdentityBoneCount)
    * BonePaletteLayout::kFloatsPerBone * sizeof(float);

constexpr size_t kOnePaletteBytes =
    static_cast<size_t>(kTestJointCount)
    * BonePaletteLayout::kFloatsPerBone * sizeof(float);
} // namespace

class SkinPaletteRefusalTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(m_RenderServices.Initialize(m_Device.get()));

        auto& store = SkeletonStore::Instance();
        m_SkeletonId = store.CreateSkeleton(kTestJointCount);
        ASSERT_NE(m_SkeletonId, 0u);
        SkeletonData* skeleton = store.Get(m_SkeletonId);
        ASSERT_NE(skeleton, nullptr);
        skeleton->SkinJointCount = kTestJointCount;
        skeleton->JointNodes = {0, 1, 2, 3};
    }

    void TearDown() override
    {
        // One reference per CreateRuntime. This world has no render hooks
        // registered, so entity destruction releases nothing.
        for (uint32_t runtimeId : m_Runtimes)
            SkeletonStore::Instance().ReleaseRuntime(runtimeId);
        m_RenderServices.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    // Re-provision the pool so the bone-palette ring holds the identity block
    // plus exactly `paletteSlots` caller palettes. Growth is off: a refusal
    // must stay one, so what is measured is the refusal path rather than the
    // ring's recovery from it.
    void SetBonePaletteCapacity(size_t paletteSlots)
    {
        PerFrameWritePoolConfig config{};
        auto& bonePalette = config.usages[static_cast<size_t>(FrameWriteUsage::BonePalette)];
        bonePalette.capacityBytes = kIdentityBlockBytes + paletteSlots * kOnePaletteBytes;
        bonePalette.maxCapacityBytes = 0;

        PerFrameWritePool& pool = m_RenderServices.GetPerFrameWritePool();
        pool.Shutdown();
        ASSERT_TRUE(pool.Initialize(m_Device.get(), config));
    }

    // The render loop's per-frame prologue for the palette path
    // (RenderingLoop.cpp): pool slot, identity block, then the rollover that
    // ages last frame's offsets — all before any producer runs.
    void BeginPaletteFrame(uint32_t frameIndex)
    {
        m_RenderServices.GetPerFrameWritePool().BeginFrame(frameIndex);
        m_RenderServices.GetSkinPaletteAtlas().BeginFrame(
            m_RenderServices.GetPerFrameWritePool());
        SkeletonStore::Instance().RollPaletteOffsets();
    }

    uint32_t MakeRuntime()
    {
        const uint32_t runtimeId = SkeletonStore::Instance().CreateRuntime(m_SkeletonId);
        if (runtimeId != 0)
            m_Runtimes.push_back(runtimeId);
        return runtimeId;
    }

    std::unique_ptr<IDevice> m_Device;
    RenderServices m_RenderServices;
    uint32_t m_SkeletonId = 0;
    std::vector<uint32_t> m_Runtimes;
};

// The premise the refusal fix rests on: offset 0 names the identity block and
// nothing else, and that block holds identity rows.
TEST_F(SkinPaletteRefusalTest, IdentityBlockOwnsAtlasSlotZero)
{
    SetBonePaletteCapacity(4);
    BeginPaletteFrame(0);

    SkinPaletteAtlas& atlas = m_RenderServices.GetSkinPaletteAtlas();

    // A caller palette that is emphatically NOT identity, so the readback below
    // cannot pass by reading this instead of the reserved block.
    std::vector<float> palette(kTestJointCount * 16u, 0.0f);
    for (uint32_t bone = 0; bone < kTestJointCount; ++bone)
    {
        const size_t base = static_cast<size_t>(bone) * 16u;
        palette[base + 0] = 2.0f;
        palette[base + 5] = 3.0f;
        palette[base + 10] = 4.0f;
        palette[base + 15] = 1.0f;
    }

    const PaletteAllocation alloc = atlas.Upload(palette.data(), kTestJointCount);
    ASSERT_TRUE(alloc.valid);
    EXPECT_EQ(alloc.OffsetInBones(), SkinPaletteAtlas::kIdentityBoneCount)
        << "the frame's first caller allocation must land ABOVE the reserved "
           "identity block, or offset 0 is not that block";

    const auto* rows = static_cast<const float*>(m_Device->MapBuffer(atlas.GetBuffer()));
    ASSERT_NE(rows, nullptr);

    // Identity packed to 3 rows (bone_palette.glsl): (1,0,0,0) (0,1,0,0) (0,0,1,0).
    // Blended with weights that sum to 1 this leaves position and normal
    // untouched — i.e. bind pose.
    static constexpr float kIdentityRows[BonePaletteLayout::kFloatsPerBone] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};

    uint32_t firstBadBone = SkinPaletteAtlas::kIdentityBoneCount;
    for (uint32_t bone = 0; bone < SkinPaletteAtlas::kIdentityBoneCount; ++bone)
    {
        const float* boneRows = rows + static_cast<size_t>(bone) * BonePaletteLayout::kFloatsPerBone;
        if (!std::equal(kIdentityRows, kIdentityRows + BonePaletteLayout::kFloatsPerBone, boneRows))
        {
            firstBadBone = bone;
            break;
        }
    }
    EXPECT_EQ(firstBadBone, SkinPaletteAtlas::kIdentityBoneCount)
        << "bone " << firstBadBone << " of the identity block is not identity; an "
           "instance pointed at slot 0 would not render bind pose";
}

// A producer whose upload is refused must retire the runtime's offset to the
// identity block. Leaving it stale points the entity at the slot the atlas has
// just handed to the newcomer that displaced it.
TEST_F(SkinPaletteRefusalTest, RefusedUploadRetiresOffsetToIdentityBlock)
{
    // Room for the identity block and exactly one caller palette.
    SetBonePaletteCapacity(1);

    const uint32_t runtimeA = MakeRuntime();
    const uint32_t runtimeB = MakeRuntime();
    ASSERT_NE(runtimeA, 0u);
    ASSERT_NE(runtimeB, 0u);
    ASSERT_NE(runtimeA, runtimeB);

    // SkinningUploadSystem walks runtime ids ascending, so the lower id wins the
    // single slot. Which id that is depends on the store's free list, not on the
    // order these were created in.
    const uint32_t winner = (std::min)(runtimeA, runtimeB);
    const uint32_t displaced = (std::max)(runtimeA, runtimeB);

    ECS::World world(nullptr);
    SkinningUploadSystem uploadSystem(&m_RenderServices);
    SkeletonStore& store = SkeletonStore::Instance();

    // Frame 1: only one character in the scene, so it settles on the slot.
    ECS::Entity displacedEntity = world.Create();
    displacedEntity.Set(Components::SkeletonRef{m_SkeletonId, displaced});
    world.ProcessCommands();

    BeginPaletteFrame(0);
    uploadSystem.Update(world, kDt);

    ASSERT_NE(store.GetRuntime(displaced), nullptr);
    const uint32_t settledOffset = store.GetRuntime(displaced)->AtlasPaletteOffsetBones;
    ASSERT_EQ(settledOffset, SkinPaletteAtlas::kIdentityBoneCount)
        << "premise: with room for one palette, the only character in the scene "
           "gets the slot immediately above the identity block";

    // Frame 2: a second character enters. It sorts first and takes the one slot,
    // so the settled character's upload is refused.
    ECS::Entity winnerEntity = world.Create();
    winnerEntity.Set(Components::SkeletonRef{m_SkeletonId, winner});
    world.ProcessCommands();

    BeginPaletteFrame(1);
    uploadSystem.Update(world, kDt);

    ASSERT_NE(store.GetRuntime(winner), nullptr);
    ASSERT_EQ(store.GetRuntime(winner)->AtlasPaletteOffsetBones, settledOffset)
        << "premise: the newcomer took the one slot";

    ASSERT_NE(store.GetRuntime(displaced), nullptr);
    EXPECT_NE(store.GetRuntime(displaced)->AtlasPaletteOffsetBones, settledOffset)
        << "the refused character kept the offset the atlas has since handed to "
           "another runtime — it would be skinned with that runtime's bones";
    EXPECT_EQ(store.GetRuntime(displaced)->AtlasPaletteOffsetBones,
              SkinPaletteAtlas::kIdentityPaletteOffsetBones)
        << "a refused allocation must retire the offset to the identity block so "
           "the frame renders bind pose";
}
