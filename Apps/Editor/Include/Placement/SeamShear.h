#pragma once

#include "SplineLayout/SeamShear.h"
#include "Placement/PieceBasis.h"

// Source compatibility for editor clients; implementation belongs to Engine.
namespace GameEngine::Editor
{
using SplineLayout::ComputeSeamShearFactors;
using SplineLayout::ApplySeamShear;
} // namespace GameEngine::Editor
