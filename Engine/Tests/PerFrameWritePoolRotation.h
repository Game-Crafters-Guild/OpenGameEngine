#pragma once

#include "Engine/Rendering/PerFrameWritePool.h"
#include "Rendering/Core/FrameBufferAllocator.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace GameEngine::Testing
{
// Calls `stepFrame` until the usage class is back on the slot it held when this was
// called, and returns how many steps that took: the ring's depth, observed through
// the rotation rather than read from the pool. The pool must already have begun a
// frame, so that there is a current slot to come back to.
template <typename StepFrame>
uint32_t RotateBackToCurrentSlot(const Engine::Renderer::PerFrameWritePool& pool,
                                 Engine::Renderer::FrameWriteUsage usage, StepFrame&& stepFrame)
{
    // The slot INDEX, not its buffer handle: a slot that grows is destroyed and
    // recreated, so the handle after a rotation is a different one by design.
    const uint32_t start = pool.GetCurrentSlot(usage);
    for (uint32_t steps = 1; steps <= Rendering::FrameBufferAllocator::kMaxRingSlots; ++steps)
    {
        stepFrame();
        if (pool.GetCurrentSlot(usage) == start)
            return steps;
    }
    ADD_FAILURE() << "the ring never returned to its starting slot";
    return 0;
}
} // namespace GameEngine::Testing
