#pragma once

namespace GameEngine::Components
{
struct Terrain;
} // namespace GameEngine::Components

namespace GameEngine::TerrainECS
{

/// Zeroes every runtime handle pair on a Terrain component that the live
/// TerrainService no longer resolves.
///
/// Generational handles make "non-zero but unresolvable" unambiguous: the slot
/// was freed or has already been recycled, so the component's handle names
/// nothing. Provisioning must treat that as "not provisioned yet" and build a
/// fresh terrain — a component revived from bytes captured before a release
/// (undo of an entity delete or of a component remove) is otherwise stranded
/// with a handle that can never resolve again.
///
/// With no service at all, every handle is unresolvable and all are cleared.
void ClearUnresolvedTerrainHandles(Components::Terrain& terrain);

} // namespace GameEngine::TerrainECS
