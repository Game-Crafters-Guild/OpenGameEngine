#pragma once

#include "ECS/Entity.h"

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// The two Inspector notices a TerrainModifierVolume with Shape::Global needs.
//
// Both exist because the global scope makes fields STOP meaning what the widget
// next to them implies, and a control that silently does nothing is the worst of
// the available behaviours. They live here rather than inside the volume
// inspector's registration lambda so their wording is reachable from a test —
// the same reason AddGeneratedEntityNotice does.

// Region-scope notice, appended for a Global volume and nothing else.
//
// States what the shape means and what it takes away: there is no edge (so the
// falloff rows are hidden), and the transform no longer scopes the region (so
// dragging the volume changes nothing visible, and the footprint gizmo is not
// drawn). Deliberately makes NO claim about strength — the volume's Weight still
// multiplies into every effect, and the Weight row sits directly beneath this.
//
// `anySphericalTerrain` adds the planet caveat; the sphere bake has no global
// path, so such a volume affects planar / tiled terrains only.
void AddGlobalVolumeScopeNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity,
                                bool anySphericalTerrain);

// Notice for a Stamp effect the bake will drop: appended only when `entity`
// carries BOTH a Global-shaped volume and an ENABLED TerrainStampEffect.
//
// The Enabled half matters. The gather takes a stamp only when it is enabled, so
// a disabled one is never refused and never warned about — flagging it would name
// a cause that is not the reason it is inactive, and prescribe a shape change
// that is not the fix.
//
// Called twice per volume by design, once at each place the author might be
// looking: under the volume's own shape rows, and on the Stamp section's header,
// which at a real inspector width is several hundred pixels further down.
void AddStampInGlobalVolumeNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity);

} // namespace GameEngine::Editor
