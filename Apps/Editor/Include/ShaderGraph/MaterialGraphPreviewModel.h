#pragma once

#include "Graph/GraphModel.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine {

struct MaterialDocument;

namespace Editor {

/**
 * A material graph rewritten for preview rendering: every numeric value typed
 * into a node becomes a read from the material param block's generic user lanes
 * rather than a literal spliced into the shader.
 *
 * That means any unwired pin default, not just the ones on Constant nodes — a
 * Fresnel's power and a Multiply's operand are typed in the same way and have to
 * go live the same way. Pins fed by a wire are skipped (their default is never
 * read), as are the parameters the bridge consumes as identity rather than value
 * (slot, component, swizzle, variableName, texture).
 *
 * That one substitution is what makes a value edit realtime. A literal is part
 * of the compiled source, so changing it means re-materializing and recompiling
 * the surface; a lane read is the same shader whatever the value, so a scrub is
 * a uniform push and every preview drawing that material updates on the next
 * frame together.
 *
 * The lanes are the engine's documented project-extensibility seam (user0..15 /
 * userVec0..3 in material_params.glsl), so this needs nothing from the engine —
 * the surface reads `Mat.uUser0.x`, the material sets `"user0"`.
 */
struct MaterialGraphPreviewModel
{
    /** The graph as previews should compile it. */
    Graph::Model Model;
    /** Material property keys and the values behind them, ready for a document. */
    std::vector<std::pair<std::string, float>> LaneValues;
    /** Values that found no free lane and kept their literal. Non-zero means
        those still cost a recompile when edited. */
    std::uint32_t UnboundScalars = 0;
};

/** Builds the preview projection of `model`. Leaves a graph with no constant
    nodes untouched apart from the copy. */
MaterialGraphPreviewModel MakeMaterialGraphPreviewModel(const Graph::Model& model);

/** Writes the lane values into `doc` so the shader's reads resolve. */
void ApplyPreviewLaneValues(const MaterialGraphPreviewModel& preview, MaterialDocument& doc);

} // namespace Editor
} // namespace GameEngine
