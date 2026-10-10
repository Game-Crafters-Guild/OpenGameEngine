#pragma once

#include "SplineLayout/FenceLayout.h"
#include "Placement/PieceAxis.h"
#include "Placement/TileLayout.h"

// Source compatibility for editor clients; implementation belongs to Engine.
namespace GameEngine::Editor
{
using SplineLayout::FencePieceBounds;
using SplineLayout::FenceStation;
using SplineLayout::FenceSpan;
using SplineLayout::FenceCrest;
using SplineLayout::FenceLayoutParams;
using SplineLayout::FenceLayoutResult;
using SplineLayout::BuildFenceLayout;
using SplineLayout::SanitizeFenceRecipe;
} // namespace GameEngine::Editor
