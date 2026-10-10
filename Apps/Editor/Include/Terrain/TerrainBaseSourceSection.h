#pragma once

namespace GameEngine
{

struct InspectorContext;

// The Terrain inspector's Base section: where a planar terrain's heights come from before its
// modifiers apply (procedural noise, an imported heightmap, or flat) and, for a heightmap, which
// asset and whether it decoded.
void AddTerrainBaseSourceSection(const InspectorContext& ctx);

} // namespace GameEngine
