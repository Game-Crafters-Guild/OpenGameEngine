#pragma once

#include "SplineLayout/PieceEntity.h"
#include "Placement/PieceAxis.h"

// Source compatibility for editor clients; implementation belongs to Engine.
namespace GameEngine::Editor
{
using SplineLayout::TilePose;
using SplineLayout::InvertPlacerWorld;
using SplineLayout::WriteParentLocalPose;
using SplineLayout::MakePieceLabel;
} // namespace GameEngine::Editor
