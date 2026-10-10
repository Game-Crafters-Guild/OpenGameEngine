#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine::SplineECS
{

// ECS system that manages spline data lifecycle.
// Creates SplineData in SplineService when a SplineComponent is first seen,
// and rebuilds caches when spline data is dirty.
class SplineExtractionSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "SplineExtractionSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
};

} // namespace GameEngine::SplineECS
