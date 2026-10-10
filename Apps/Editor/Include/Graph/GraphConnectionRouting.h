#pragma once

#include <algorithm>
#include <cmath>

namespace GameEngine::GraphConnectionRouting {

/** Cubic handle length so a forward link cannot loop when the ports sit close.
 *  Takes the target minus the source in screen pixels and does its own signs;
 *  `zoom` converts the two floors from graph units so the curve keeps one shape
 *  at every zoom instead of flattening as the endpoints spread. */
inline float BezierControlOffset(float dx, float dy, float zoom)
{
    constexpr float kMinControlOffsetGraph = 30.0f;
    constexpr float kMinForwardOffsetGraph = 8.0f;
    constexpr float kForwardHandleFraction = 0.45f;
    constexpr float kForwardRise = 0.15f;
    constexpr float kBackwardRise = 0.25f;
    const float minControlOffset = kMinControlOffsetGraph * zoom;
    const float absDx = std::abs(dx);
    const float absDy = std::abs(dy);
    const float rise = dx >= 0.0f ? kForwardRise : kBackwardRise;
    float offset = std::max(minControlOffset, 0.5f * absDx + rise * absDy);
    if (dx >= 0.0f)
    {
        const float minForwardOffset = kMinForwardOffsetGraph * zoom;
        offset = std::min(offset, std::max(minForwardOffset, dx * kForwardHandleFraction));
    }
    return offset;
}

/** Sideways offset for a wire's stub column so links leaving (or entering) the
 *  same node do not stack their vertical runs on one line. Centred on the
 *  column, so a single link keeps the column it always had. */
inline float PortLaneBias(int portSlot, int portCount, float offsetPx)
{
    if (portCount <= 1 || portSlot < 0)
        return 0.f;
    return (static_cast<float>(portSlot) - static_cast<float>(portCount - 1) * 0.5f) * offsetPx;
}

/** Shortest horizontal run a wire may have where it meets a port. Port centres
 *  sit on the node edge and the circle reaches a few units past it, so a stub
 *  column nearer than this draws the wire across the ports it runs by. */
inline constexpr float kMinPortStubGraph = 10.f;

struct OrthogonalStubs
{
    float ExitX = 0.f;
    float EnterX = 0.f;
};

/**
 * Orthogonal stubs for a straight-mode wire. Forward links whose nodes do not
 * overlap in X compress the stubs into the remaining gap so a short run stays
 * in-band instead of looping over both headers.
 */
inline OrthogonalStubs ForwardAwareStubs(float sx0, float sx1,
                                         float srcLeft, float srcRight,
                                         float tgtLeft, float tgtRight,
                                         float stubPx)
{
    OrthogonalStubs stubs;
    stubs.ExitX = std::max(sx0, srcRight) + stubPx;
    stubs.EnterX = std::min(sx1, tgtLeft) - stubPx;

    const bool bodiesOverlapX = tgtLeft < srcRight && srcLeft < tgtRight;
    const bool backward = sx1 + 1.f < sx0;
    if (backward || bodiesOverlapX || stubs.EnterX >= stubs.ExitX)
        return stubs;

    const float mid = (sx0 + sx1) * 0.5f;
    stubs.ExitX = std::max(sx0 + kMinPortStubGraph, std::min(stubs.ExitX, mid));
    stubs.EnterX = std::min(sx1 - kMinPortStubGraph, std::max(stubs.EnterX, mid));
    if (stubs.EnterX < stubs.ExitX)
        stubs.ExitX = stubs.EnterX = mid;
    return stubs;
}

} // namespace GameEngine::GraphConnectionRouting
