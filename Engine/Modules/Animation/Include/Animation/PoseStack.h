#pragma once

#include "Animation/AnimationPose.h"

#include <array>
#include <cassert>
#include <cstdint>

namespace GameEngine
{
namespace Animation
{

// Pre-allocated stack of AnimationPose scratch buffers used inside graph
// node Evaluate() calls. Each top-level Evaluate resets the stack pointer
// to 0 via PoseStack::Frame (RAII); nested allocations push and pop.
//
// Capacity is sized for the worst-case nested graph depth: 8 levels of
// LayeredBlend / Additive / Retarget / Montage chains, each potentially
// requesting a scratch pose. Per-thread instance lives via thread_local
// in the animation system that owns it (one per ECS worker).
//
// The PoseStack does NOT allocate per Evaluate call after warm-up: the
// underlying AnimationPose instances are resized once to the target
// skeleton's bone count and reused for the lifetime of the thread.
class PoseStack
{
public:
    static constexpr uint32_t kMaxDepth = 8;

    PoseStack() = default;

    // Resize all scratch slots to the given bone count. Call once per
    // session bake (when the target skeleton is known); subsequent
    // Evaluate calls reuse capacity without allocating.
    void Reserve(uint32_t boneCount)
    {
        for (auto& pose : m_Slots)
        {
            pose.Resize(boneCount);
        }
        m_Reserved = boneCount;
    }

    // Number of slots currently in use (debug only).
    uint32_t Depth() const { return m_Top; }

    // Reserved bone count from the last Reserve() call.
    uint32_t ReservedBoneCount() const { return m_Reserved; }

    // RAII handle that pushes a pose slot on construction and pops on
    // destruction. Use as the natural way to allocate a scratch pose
    // for the duration of an Evaluate call:
    //
    //   void Foo::Evaluate(EvaluationContext& ctx, AnimationPose& outPose) {
    //       PoseStack::Frame scratch(*ctx.ScratchStack);
    //       AnimationPose& tmp = scratch.Pose();
    //       ...
    //   }
    class Frame
    {
    public:
        explicit Frame(PoseStack& stack) : m_Stack(&stack), m_Index(stack.m_Top)
        {
            assert(stack.m_Top < kMaxDepth && "PoseStack overflow; raise kMaxDepth");
            ++stack.m_Top;
        }

        ~Frame()
        {
            assert(m_Stack->m_Top == m_Index + 1 && "PoseStack frames must pop in LIFO order");
            --m_Stack->m_Top;
        }

        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;

        AnimationPose& Pose() { return m_Stack->m_Slots[m_Index]; }

    private:
        PoseStack* m_Stack;
        uint32_t   m_Index;
    };

private:
    std::array<AnimationPose, kMaxDepth> m_Slots;
    uint32_t m_Top = 0;
    uint32_t m_Reserved = 0;
};

} // namespace Animation
} // namespace GameEngine
