#pragma once
#include "ECS/Entity.h"
#include "SceneView/SceneViewGizmos.h"

namespace GameEngine::Components
{
struct ParticleEmitter3D;
struct WorldTransform;
} // namespace GameEngine::Components

namespace GameEngine::Particles
{
struct StackDocument;
}

namespace GameEngine::Editor::SceneTools
{
/// Scene-view overlay for a selected or hovered particle emitter: the shape every Shape processor of
/// its stack places particles on, the cone every Velocity Cone processor launches them in, and the
/// plane of every plane Collision processor. Particles do not show where they will spawn from; this
/// does. Reads ParticleEmitter3D, WorldTransform and the stack the emitter runs: its stack asset once
/// loaded, the default stack when it references none.
void DrawParticleEmitterGizmo(GizmoRenderContext& context, const ECS::World& world, ECS::EntityHandle entity);

/// The overlay of `document` for an emitter at `worldTransform`.
void DrawParticleStackGizmo(GizmoRenderContext& context, const Components::ParticleEmitter3D& emitter,
                            const Components::WorldTransform& worldTransform, const Particles::StackDocument& document);

/// Registers DrawParticleEmitterGizmo on ComponentGizmoRegistry keyed on ParticleEmitter3D.
void RegisterParticleEmitterGizmo();
} // namespace GameEngine::Editor::SceneTools
