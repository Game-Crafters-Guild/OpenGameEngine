#pragma once

namespace GameEngine
{

struct MaterialDocument;

namespace Editor::MaterialRows
{

/// What assigning an emissive map in the material inspector does to the rest of the
/// document. The surface multiplies the map by the emission color and luminance, and a
/// fresh material's luminance is 0, so on its own the map would show nothing: a zero
/// luminance becomes reference white (203 nits) and a black color becomes white, in the
/// same edit. A material that already emits keeps its color and luminance.
void TurnOnEmissionForAssignedMap(MaterialDocument& doc);

} // namespace Editor::MaterialRows

} // namespace GameEngine
