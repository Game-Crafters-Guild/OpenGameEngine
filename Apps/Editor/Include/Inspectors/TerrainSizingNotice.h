#pragma once

#include "Types/Types.h"

#include <string>

namespace GameEngine
{
class UIElement;
namespace TerrainECS
{
struct TerrainSizingPlan;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

// Size and Samples Per Meter are two raw float drags that between them decide the terrain's
// resolution, its tile grid, which height source it resolves through, how much VRAM it costs,
// and how much of it is held at full detail — none of which either row shows. An author could
// set a legal pair and get a terrain that rendered nothing, with the only signal a log warning.
//
// These sentences are the derived consequence, rendered AT the rows that cause it. Kept in their
// own TU, like TerrainRuleNotices and TerrainVolumeNotices, so the wording is reachable from a
// test rather than only from a running editor.

// The one-line summary of what the authored size + density actually buy: resolution, detail in
// metres per texel, and the tile grid. Always present — an author should never have to guess
// what a drag did.
std::string DescribeTerrainSizing(const TerrainECS::TerrainSizingPlan& plan);

// What the terrain costs and what it gives up, when either is worth saying: the VRAM figure, and
// for a terrain past the unified ceiling the resident window and the far-field texel density it
// falls back to. Empty for a terrain small enough that neither is a decision.
std::string DescribeTerrainResidency(const TerrainECS::TerrainSizingPlan& plan);

// Renders the two above under the size/density rows, warning-styled when the terrain is only
// partially resident (the far-field detail call).
void AddTerrainSizingNotice(UIElement* parent, const TerrainECS::TerrainSizingPlan& plan);

} // namespace GameEngine::Editor
