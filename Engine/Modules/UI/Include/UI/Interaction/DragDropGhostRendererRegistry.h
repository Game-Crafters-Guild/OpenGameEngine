#pragma once

#include "UI/Interaction/Payload.h"
#include "UI/UIElement.h"

namespace GameEngine
{
class UIElement;
class Label;

namespace UI::Interaction
{
// Optional extension point:
// Allow host/app code to customize the drag ghost visuals per payload type,
// without adding payload-specific branching to UIManager/DragDropOverlay.
//
// Notes:
// - Registration is expected to occur on the UI thread during startup.
// - Renderer should be cheap; it may be called when dragging.
// - The overlay provides a stable ghost container with an icon + label; renderers can
//   add classes/children or set background overrides, but should avoid removing nodes.
using DragGhostRendererFn = void (*)(const DragPayload& payload,
                                     int mods,
                                     UIElement& ghostRoot,
                                     UIElement& icon,
                                     Label& label,
                                     Label* badge,
                                     void* userData);

struct DragGhostRendererEntry
{
    DragGhostRendererFn fn = nullptr;
    void* userData = nullptr;
};

void RegisterDragGhostRenderer(PayloadTypeId typeId, DragGhostRendererFn fn, void* userData = nullptr);
void UnregisterDragGhostRenderer(PayloadTypeId typeId);
const DragGhostRendererEntry* FindDragGhostRenderer(PayloadTypeId typeId);

} // namespace UI::Interaction
} // namespace GameEngine

