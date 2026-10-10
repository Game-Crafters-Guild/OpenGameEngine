#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Lightweight ECS component for a spline entity. Heavyweight data (control points,
// LUT, segment bounds) is owned by SplineService; this component stores only
// a handle and creation-time configuration. Follows the Terrain component pattern.
struct SplineComponent
{
    // Handle into SplineService (generational for safe access). Runtime-only: a load
    // re-creates the spline and binds a fresh handle, so a saved handle would alias a
    // live spline of the same session.
    uint32 SplineDataIndex = 0;      // [DoNotSerialize]
    uint32 SplineDataGeneration = 0; // [DoNotSerialize]

    // Default radius for newly added control points.
    float32 DefaultRadius = 5.0f;
};

static_assert(std::is_trivially_copyable_v<SplineComponent>,
              "SplineComponent must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<SplineComponent>,
              "SplineComponent must be standard layout for ECS storage");

} // namespace GameEngine::Components
