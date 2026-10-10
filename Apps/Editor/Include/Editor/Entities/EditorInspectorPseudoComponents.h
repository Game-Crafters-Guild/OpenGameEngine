#pragma once

namespace GameEngine::Editor
{

// Inspector-only pseudo component: not stored on entities. Used with
// ECS::GetComponentTypeId<> for a stable InspectorPanel section identity.
struct MeshRendererMaterialInspectorSection
{
};

} // namespace GameEngine::Editor
