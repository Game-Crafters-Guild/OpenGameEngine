#pragma once

#include "SplineLayout/TileLayout.h"


// Source compatibility for editor clients; implementation belongs to Engine.
namespace GameEngine::Editor
{
using SplineLayout::CenterSample;
using SplineLayout::HoldSurfaceAcrossGaps;
using SplineLayout::TilePose;
using SplineLayout::NormalizedOrFallback;
using SplineLayout::ClampTiltFromWorldUp;
using SplineLayout::TiltClampReport;
using SplineLayout::StationSurfaceProbe;
using SplineLayout::TileLayoutParams;
using SplineLayout::BuildTilePoses;
} // namespace GameEngine::Editor
