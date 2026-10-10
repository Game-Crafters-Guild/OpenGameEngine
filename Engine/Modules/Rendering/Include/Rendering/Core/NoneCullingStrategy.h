#pragma once

#include "Rendering/Core/FrustumCullingStrategy.h"

namespace GameEngine
{
namespace Rendering
{

// Skip spatial culling for previews whose instances are already in view.
// Layer membership still applies: a shared mesh/material batch is not proof
// that every instance belongs to this view. Permissive planes reuse the
// ordinary eligibility pass without introducing spatial or HZB history.
class NoneCullingStrategy final : public ICullingStrategy
{
public:
    void ScheduleCulling(const ViewCullingContext& ctx) override
    {
        ViewCullingContext membership = ctx;
        for (auto& plane : membership.FrustumPlanes)
            plane = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        FrustumCullingStrategy{}.ScheduleCulling(membership);
    }
};

} // namespace Rendering
} // namespace GameEngine
