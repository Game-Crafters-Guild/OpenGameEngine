#include "UI/Interaction/DragDropGhostRendererRegistry.h"

#include "Editor/DragDropPayloads.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

namespace GameEngine::Editor
{
static bool IsCopyMods(int mods)
{
    // Primary shortcut: Ctrl on Windows/Linux, Cmd on macOS.
    return Input::IsPrimaryShortcutModifier(mods);
}

static void RenderAssetPathsGhost(const UI::Interaction::DragPayload& payload,
                                 int mods,
                                 UIElement& /*ghostRoot*/,
                                 UIElement& /*icon*/,
                                 Label& /*label*/,
                                 Label* badge,
                                 void* /*userData*/)
{
    (void)payload;
    if (!badge)
        return;

    if (IsCopyMods(mods))
    {
        badge->SetText("COPY");
        badge->RemoveClass("hidden");
    }
    else
    {
        badge->SetText("");
        badge->AddClass("hidden");
    }
}

static void RenderHierarchyEntitiesGhost(const UI::Interaction::DragPayload& payload,
                                        int /*mods*/,
                                        UIElement& ghostRoot,
                                        UIElement& /*icon*/,
                                        Label& /*label*/,
                                        Label* badge,
                                        void* /*userData*/)
{
    const auto* p = payload.TryGet<Editor::HierarchyEntityDragPayload>();
    if (!p)
        return;

    const std::size_t n = p->treeIds.size();
    if (n > 1)
        ghostRoot.AddClass("dnd-ghost-multi");
    else
        ghostRoot.RemoveClass("dnd-ghost-multi");

    if (!badge)
        return;

    if (n > 1)
    {
        badge->SetText("+" + std::to_string(n - 1));
        badge->RemoveClass("hidden");
    }
    else
    {
        badge->SetText("");
        badge->AddClass("hidden");
    }
}

void RegisterEditorDragDropGhostRenderers()
{
    UI::Interaction::RegisterDragGhostRenderer(
        UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>(),
        &RenderAssetPathsGhost,
        nullptr);

    UI::Interaction::RegisterDragGhostRenderer(
        UI::Interaction::GetPayloadTypeId<Editor::HierarchyEntityDragPayload>(),
        &RenderHierarchyEntitiesGhost,
        nullptr);
}
} // namespace GameEngine::Editor

