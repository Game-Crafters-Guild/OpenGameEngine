#pragma once

#include "SplineLayout/PieceBasis.h"
#include "Placement/PieceAxis.h"
#include "Placement/TileLayout.h"

// Source compatibility for editor clients; implementation belongs to Engine.
namespace GameEngine::Editor
{
using SplineLayout::PieceBasis;
using SplineLayout::MakePieceBasis;
using SplineLayout::PieceCenterOnPoseRight;
} // namespace GameEngine::Editor
