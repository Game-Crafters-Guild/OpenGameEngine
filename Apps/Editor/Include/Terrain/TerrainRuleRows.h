#pragma once

namespace GameEngine
{

struct InspectorContext;
class TerrainMaterialLibraryAsset;

namespace Components
{
struct Terrain;
struct TerrainSurfaceRulesEffect;
} // namespace Components

// The Surface Rules effect's body: the rule list, each row's result material and
// blend mode, and each condition's range-band widget.
//
// Its own TU rather than another lambda in TerrainModifierInspectors.cpp. That
// file owns REGISTRATION — one short lambda per effect kind, plus the chrome
// every effect shares — and a rules body large enough to need foldouts, a custom
// two-handle band row and four per-condition control shapes would have made it
// the file where terrain inspector code goes rather than the file that registers
// terrain inspectors.
//
// `terrain` and `library` are resolved by the caller and may be null: a rules
// effect can be authored on a volume in a scene with no terrain in it yet, and
// the row falls back to the semantic channel names when it is.
void AddTerrainSurfaceRuleRows(const InspectorContext& ctx,
                               const Components::TerrainSurfaceRulesEffect& effect,
                               const Components::Terrain* terrain,
                               TerrainMaterialLibraryAsset* library);

} // namespace GameEngine
