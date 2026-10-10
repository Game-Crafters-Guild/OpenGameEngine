#pragma once

#include "SplineLayout/PieceAxis.h"


// Source compatibility for editor clients; implementation belongs to Engine.
namespace GameEngine::Editor
{
using SplineLayout::PieceAxis;
using SplineLayout::kPieceAxisDominance;
using SplineLayout::ChoosePieceAxis;
using SplineLayout::PieceLength;
using SplineLayout::PieceAxisScale;
using SplineLayout::MakePieceAxisScale;
} // namespace GameEngine::Editor
