#pragma once

namespace GameEngine::Scene
{

// Ensures TerrainECS scene component schemas are registered even when static
// schema auto-registration is stripped from a linked static library.
void EnsureTerrainSceneSchemasRegistered();

} // namespace GameEngine::Scene
