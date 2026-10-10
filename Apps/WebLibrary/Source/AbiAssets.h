#pragma once

namespace GameEngine::WebLibrary
{

/// Moves the page's URL loads forward. Runs inside ge_tick and ge_update_assets only, so a
/// load's status never changes between them.
void AdvanceAssetLoads();

/// Forgets every URL load (at shutdown).
void DropAssetLoads();

} // namespace GameEngine::WebLibrary
