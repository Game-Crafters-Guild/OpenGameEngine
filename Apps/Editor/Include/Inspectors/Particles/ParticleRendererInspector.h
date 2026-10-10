#pragma once

namespace GameEngine::ParticleInspectors
{
/// Registers the Particle Renderer's inspector: its fields grouped by what they change (material
/// and texture sheet, lighting, orientation, meshes, trails, visibility), showing the six-way,
/// emitter-scale and trail rows only where they apply.
void RegisterParticleRendererInspector();
} // namespace GameEngine::ParticleInspectors
