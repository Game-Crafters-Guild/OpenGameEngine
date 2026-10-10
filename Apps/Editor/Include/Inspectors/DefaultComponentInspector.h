#pragma once

#include "ECS/ComponentFieldRegistry.h"  // ECS::ComponentTypeId

#include <span>
#include <string_view>

namespace GameEngine {

struct InspectorContext;

// Render a reflection-driven inspector for a component that has a registered
// field table (see ComponentFieldRegistry / GE_REFLECT) but no hand-written
// InspectorFn. This is the editor's default fallback: instead of showing
// "(No inspector registered)", it walks the component's fields and emits an
// editable widget per field (toggle / float drag / int drag), wired to the
// undo/redo service and change notifications.
//
// Returns true if it rendered at least one field row. Returns false when the
// component has no field table or its bytes can't be read, so the caller can
// fall back to the plain placeholder.
bool RenderDefaultComponentInspector(const InspectorContext& ctx, ECS::ComponentTypeId typeId);

// The same rows for just the reflected fields `fieldNames` names, in the component's declaration
// order, into ctx.Parent: for a registered inspector that groups a component's fields into
// sections (InspectorUI::AddComponentSection) and shows a group only when it applies. Edits, undo,
// multi-selection and live refresh behave as in the default inspector. Returns true if it rendered
// at least one row.
bool RenderReflectedFieldRows(const InspectorContext& ctx, ECS::ComponentTypeId typeId,
                              std::span<const std::string_view> fieldNames);

} // namespace GameEngine
