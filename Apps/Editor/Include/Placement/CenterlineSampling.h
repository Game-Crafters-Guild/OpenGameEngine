#pragma once

#include "SplineLayout/CenterlineSampling.h"


// Source compatibility for editor clients; implementation belongs to Engine.
namespace GameEngine::Editor
{
using SplineLayout::kCenterlineStepMetres;
using SplineLayout::kMaxCenterlineLengthMetres;
using SplineLayout::kMaxCenterlineSamples;
using SplineLayout::LargestAxisScale;
using SplineLayout::WorldCenterlineLength;
using SplineLayout::CenterlineSampleCount;
} // namespace GameEngine::Editor
